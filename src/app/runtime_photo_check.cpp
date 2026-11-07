// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-photo mode: photo mode over the skirmish's fight, in a window.
// One pass enters, moves the camera within play's limits and leaves;
// another never enters. At the last tick the two worlds read back alike,
// and the view photo mode replaced is the view it puts back. Pointer,
// key and resize steps go through the window a player would use.
// --check-photo time: one pass freezes, steps, draws each eighth and runs
// slowly; the other never enters. A shared match and a watcher hold the picture.
#include "oa/app/runtime.hpp"

#include "language_state.hpp"
#include "oa_layer.hpp"
#include "photo_state.hpp"

#include "oa/app/frame_coordinates.hpp"
#include "oa/app/frame_pacing.hpp"
#include "oa/core/unit.h"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/input_actions/actions.hpp"
#include "oa/sim/messages.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/hud/kill_board.hpp"
#include "oa/ui/hud/resource_panel.hpp"
#include "oa/ui/key_editor/overlay.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace actions = oa::data::input_actions;
namespace console = oa::ui::console;
namespace layout = oa::ui::display_layout;

/// Ticks each pass of the fight plays, one drawn frame a tick.
constexpr int kPassTicks = 120;
/// The tick photo mode opens on, before that tick's frame.
constexpr int kEnterTick = 30;
/// The tick photo mode closes on, after that tick's frame.
constexpr int kLeaveTick = 90;
/// The fight's armies, each side, as the director view check plants them.
constexpr std::size_t kArmy = 8;
/// The zoom both passes hold outside photo mode.
constexpr float kPlayZoom = 1.0F;
/// The photo camera's zoom, within play's limits and no wider on the map
/// than the play view at kPlayZoom on the check's window.
constexpr float kPhotoZoomNear = 1.25F;
constexpr float kPhotoZoomFar = 1.35F;
/// Map pixels one photo pan step covers, before the zoom divides it. The
/// next tick steps the other way, so the view comes back.
constexpr double kPanStep = 4.0;

/// Primary on this platform: Command on a Mac, Ctrl elsewhere.
#ifdef SDL_PLATFORM_MACOS
constexpr SDL_Keymod kPrimaryModifier = SDL_KMOD_LGUI;
#else
constexpr SDL_Keymod kPrimaryModifier = SDL_KMOD_LCTRL;
#endif

/// The extension state the shared and watcher passes report.
uint32_t photo_extension_bits = 0;

/// The extension state hook for the check. It cannot capture.
///
/// @return photo_extension_bits
uint32_t photo_extension_state(void*, const Runtime&) {
    return photo_extension_bits;
}

/// The bit of a context, or 0 when the table has no such name.
///
/// @param name the context id
/// @return the bit
uint32_t context_bit(std::string_view name) {
    const std::span<const actions::Context> table = actions::contexts();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index) {
        if (table[index].name == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// The player's view, the fields leaving puts back.
struct KeptView {
    layout::MatchLayout placed{};
    int32_t camera_x{};
    int32_t camera_z{};
    float zoom{};
    float zoom_target{};
    bool tracking{};
    uint16_t tracked_unit{};
    bool exact_held{};
    double exact_x{};
    double exact_z{};
    int32_t exact_camera_x{};
    int32_t exact_camera_z{};
    double exact_drawn_x{};
    double exact_drawn_z{};
    MatchCommand command{};
    uint16_t build{};
    bool hold_held{};
    double hold_x{};
    double hold_z{};
};

/// One unit's selection and orders.
struct UnitOrders {
    uint16_t id{};
    uint16_t type{};
    uint32_t flags{};
    uint32_t primary{};
    uint32_t secondary{};

    /// @return true when the selection and the orders are the same
    [[nodiscard]] bool operator==(const UnitOrders&) const = default;
};

/// The Game fields photo mode must not write.
struct MatchFields {
    uint16_t console{};
    uint8_t visibility{};
    uint16_t sim_run{};
    std::vector<char> messages{};
};

/// Reads a preferences file's bytes. An absent file is empty.
///
/// @param path the preferences file the check was given
/// @return the bytes
/// @throws std::runtime_error when no preferences file was given
std::vector<char> file_bytes(const std::optional<std::filesystem::path>& path) {
    if (!path.has_value())
        throw std::runtime_error("photo mode check: no preferences file");
    std::ifstream in(*path, std::ios::binary);
    if (!in)
        return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

void Runtime::check_photo() {
    switch (options_.photo_group) {
    case PhotoGroup::mode:
        check_photo_mode();
        return;
    case PhotoGroup::time:
        check_photo_time();
        return;
    }
    throw std::runtime_error("photo mode check: unknown group");
}

void Runtime::check_photo_mode() {
    /// Puts the preferences file back to the bytes the check started with.
    /// The fights write their own rows; those are put back too, and the run
    /// is stopped from writing the file again on the way out.
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& path = host.options_.preferences_file;
            if (path.has_value())
                existed = std::filesystem::exists(*path);
            bytes = file_bytes(path);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& path = runtime.options_.preferences_file;
            if (path.has_value()) {
                if (!existed) {
                    std::error_code error;
                    std::filesystem::remove(*path, error);
                } else {
                    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    /// Puts the extension state back, and clears the check's bits.
    struct ExtensionHold {
        Runtime& runtime;
        uint32_t (*state)(void*, const Runtime&);
        void* context;

        explicit ExtensionHold(Runtime& host)
            : runtime(host), state(host.extension_.state), context(host.extension_.context) {}

        ExtensionHold(const ExtensionHold&) = delete;
        ExtensionHold& operator=(const ExtensionHold&) = delete;

        ~ExtensionHold() {
            runtime.extension_.state = state;
            runtime.extension_.context = context;
            photo_extension_bits = 0;
        }
    };

    const PreferencesHold preferences_hold(*this);
    const ExtensionHold extension_hold(*this);
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo mode check: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    const auto step_tick = [this] {
        try {
            step_match_simulation();
        } catch (const std::exception& error) {
            report_match_tick_error(error.what());
        }
    };
    const auto same_layout = [](const layout::MatchLayout& left, const layout::MatchLayout& right) {
        return left.width == right.width && left.height == right.height &&
               left.left == right.left && left.top == right.top && left.bottom == right.bottom &&
               left.hud_width == right.hud_width && left.hud_height == right.hud_height &&
               left.scale == right.scale && left.column_scale == right.column_scale &&
               left.phone == right.phone && left.placed_count == right.placed_count;
    };
    const auto read_fields = [this] {
        MatchFields fields;
        const oa::Game& game = match_->state().game;
        fields.console = console::console_flags(game);
        fields.visibility = game.visibility_flags;
        fields.sim_run = game.sim_run_flags;
        fields.messages.resize(static_cast<std::size_t>(OA_CHAT_LINE_COUNT) * OA_CHAT_LINE_BYTES);
        std::memcpy(fields.messages.data(), game.chat_lines, fields.messages.size());
        return fields;
    };
    const auto same_fields = [](const MatchFields& left, const MatchFields& right) {
        return left.console == right.console && left.visibility == right.visibility &&
               left.sim_run == right.sim_run && left.messages == right.messages;
    };
    const auto read_orders = [this] {
        std::vector<UnitOrders> orders;
        const oa::World& world = match_->state();
        for (uint32_t slot = 0; slot < world.unit_slot_count; ++slot) {
            const oa::Unit* unit = oa::world_unit_at(&world, slot);
            if (unit == nullptr || unit->type_index == 0)
                continue;
            orders.push_back(
                {unit->id,
                 unit->type_index,
                 unit->flags,
                 static_cast<uint32_t>(unit->primary_order),
                 static_cast<uint32_t>(unit->secondary_order)}
            );
        }
        return orders;
    };
    const auto keep_view = [this] {
        KeptView kept;
        kept.placed = match_layout_;
        kept.camera_x = match_camera_x_;
        kept.camera_z = match_camera_z_;
        kept.zoom = match_zoom_;
        kept.zoom_target = match_zoom_target_;
        kept.tracking = match_tracking_;
        kept.tracked_unit = tracked_match_unit_;
        kept.exact_held = exact_view_.held;
        kept.exact_x = exact_view_.x;
        kept.exact_z = exact_view_.z;
        kept.exact_camera_x = exact_view_.camera_x;
        kept.exact_camera_z = exact_view_.camera_z;
        kept.exact_drawn_x = exact_view_.drawn_x;
        kept.exact_drawn_z = exact_view_.drawn_z;
        kept.command = match_command_;
        kept.build = pending_build_type_;
        kept.hold_held = view_hold_.held;
        kept.hold_x = view_hold_.centre_x;
        kept.hold_z = view_hold_.centre_z;
        return kept;
    };
    const auto view_restored = [&](const KeptView& kept) {
        return same_layout(match_layout_, kept.placed) && match_camera_x_ == kept.camera_x &&
               match_camera_z_ == kept.camera_z && match_zoom_ == kept.zoom &&
               match_zoom_target_ == kept.zoom_target && match_tracking_ == kept.tracking &&
               tracked_match_unit_ == kept.tracked_unit && exact_view_.held == kept.exact_held &&
               exact_view_.x == kept.exact_x && exact_view_.z == kept.exact_z &&
               exact_view_.camera_x == kept.exact_camera_x &&
               exact_view_.camera_z == kept.exact_camera_z &&
               exact_view_.drawn_x == kept.exact_drawn_x &&
               exact_view_.drawn_z == kept.exact_drawn_z && view_hold_.held == kept.hold_held &&
               view_hold_.centre_x == kept.hold_x && view_hold_.centre_z == kept.hold_z &&
               match_command_ == kept.command && pending_build_type_ == kept.build;
    };
    const auto hold_play_zoom = [&] {
        const float least = least_match_zoom();
        const float most = most_match_zoom();
        require(
            least <= kPlayZoom && most >= kPlayZoom, "zoom 1 is outside the match's play limits"
        );
        match_zoom_ = kPlayZoom;
        match_zoom_target_ = kPlayZoom;
    };
    const auto arm_and_select = [&] {
        oa::Game& game = match_->state().game;
        console::set_console_flags(
            game,
            static_cast<uint16_t>(
                console::console_flags(game) | console::console_flag::selection_boxes
            )
        );
        oa::Unit* commander = nullptr;
        for (const auto& slot : match_->world().slots) {
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                slot.record.owner_index == match_local_player_) {
                commander = oa::world_unit_at(&match_->state(), slot.unit_index);
                break;
            }
        }
        require(commander != nullptr, "the fight has no local commander");
        commander->flags |= OA_UNIT_FLAG_SELECTED;
        match_command_ = MatchCommand::attack;
        pending_build_type_ = 0;
    };
    const auto start_fight = [&] {
        start_benchmark_skirmish();
        match_layout_ = lay_out_match(options_.match_width, options_.match_height);
        hold_play_zoom();
        spawn_combat_armies(kArmy);
        arm_and_select();
    };
    const auto end_fight = [this] {
        if (photo_mode())
            leave_photo_mode();
        if (match_ != nullptr) {
            leave_match();
            load(Screen::main_menu);
        }
    };
    const auto move_photo_camera = [&](int tick) {
        const float least = least_match_zoom();
        const float most = most_match_zoom();
        const float want = (tick % 2) == 0 ? kPhotoZoomNear : kPhotoZoomFar;
        const float zoom = std::max(want, least);
        require(zoom <= most, "the photo zoom leaves play's limits");
        require(zoom >= kPhotoZoomNear, "the photo zoom would draw a wider map than play");
        match_zoom_ = zoom;
        match_zoom_target_ = zoom;
        scroll_match_view((tick % 2) == 0 ? 1 : -1, 0, kPanStep);
        set_photo_show_selection((tick % 4) < 2);
        set_photo_show_health_bars((tick % 4) >= 2);
    };
    const auto play_pass = [&](bool enter) {
        start_fight();
        for (int tick = 1; tick <= kPassTicks; ++tick) {
            step_tick();
            if (enter && tick == kEnterTick)
                require(enter_photo_mode(), "photo mode did not open");
            if (enter && photo_mode())
                move_photo_camera(tick);
            render_match_surface();
            if (enter && tick == kLeaveTick)
                leave_photo_mode();
        }
        require(!photo_mode(), "photo mode was still open at the last tick");
        DrawReadBack read = match_draw_read_back();
        end_fight();
        return read;
    };
    const auto play_pair = [&](uint32_t bits) {
        photo_extension_bits = bits;
        extension_.state = photo_extension_state;
        const DrawReadBack entered = play_pass(true);
        const DrawReadBack stayed = play_pass(false);
        const char* difference = draw_read_back_difference(entered, stayed);
        require(difference == nullptr, difference == nullptr ? "" : std::string(difference));
    };

    leave_photo_mode();
    require(!photo_mode(), "photo mode was open before the check");

    start_fight();
    match_finished_ = true;
    require(!enter_photo_mode() && !photo_mode(), "a finished match entered photo mode");
    match_finished_ = false;
    chat_composing_ = true;
    require(!enter_photo_mode() && !photo_mode(), "an open chat line entered photo mode");
    chat_composing_ = false;
    require(enter_photo_mode(), "photo mode did not open");
    require(enter_photo_mode() && photo_mode(), "entering again changed photo mode");
    const layout::MatchLayout battlefield = layout::make_battlefield_layout(
        match_layout_.width > 0 ? match_layout_.width : options_.match_width,
        match_layout_.height > 0 ? match_layout_.height : options_.match_height
    );
    require(
        same_layout(match_layout_, battlefield) && match_layout_.hud_width == 0 &&
            !match_layout_.phone,
        "photo mode is not the battlefield alone"
    );
    leave_photo_mode();
    require(!photo_mode(), "leaving did not end photo mode");
    end_fight();
    require(!enter_photo_mode() && !photo_mode(), "photo mode opened with no match");
    // The first fight writes the engine's own rows into an empty file.
    // Photo mode must add nothing after that. The hold puts the caller's
    // bytes back on the way out, empty file included.
    const std::vector<char> preferences_settled = file_bytes(options_.preferences_file);

    const auto check_interface = [&](uint32_t bits) {
        photo_extension_bits = bits;
        extension_.state = photo_extension_state;
        start_fight();
        oa::Unit* commander = nullptr;
        uint16_t commander_slot = 0;
        for (const auto& slot : match_->world().slots) {
            if (slot.unit_index == 0 || slot.unit == nullptr ||
                slot.owner_index != match_local_player_ || slot.unit->type_index == 0)
                continue;
            commander = oa::world_unit_at(&match_->state(), slot.unit_index);
            commander_slot = slot.unit_index;
            break;
        }
        require(commander != nullptr && commander_slot != 0, "the fight has no local commander");
        bool running = true;
        const auto dispatch = [&](SDL_Event& event) {
            dispatch_event(event, running);
            require(running, "a key ended the run");
        };
        const auto tap = [&](SDL_Scancode scancode, SDL_Keymod mod = SDL_KMOD_NONE) {
            require(sdl_.window != nullptr, "the check has no window");
            SDL_Event event{};
            event.key.windowID = SDL_GetWindowID(sdl_.window);
            event.key.scancode = scancode;
            event.key.key = SDL_GetKeyFromScancode(scancode, mod, true);
            event.key.mod = mod;
            for (const bool down : {true, false}) {
                event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                event.key.down = down;
                event.key.repeat = false;
                dispatch(event);
            }
        };
        const auto window_point = [&](float frame_x, float frame_y) {
            float window_x = 0;
            float window_y = 0;
            require(
                frame_to_window(sdl_.renderer, frame_x, frame_y, &window_x, &window_y),
                "the point is not on the window"
            );
            return std::pair<float, float>{window_x, window_y};
        };
        const auto motion = [&](float window_x, float window_y) {
            SDL_Event event{};
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.windowID = SDL_GetWindowID(sdl_.window);
            event.motion.x = window_x;
            event.motion.y = window_y;
            dispatch(event);
        };
        const auto button = [&](uint8_t which, bool down, float window_x, float window_y) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = which;
            event.button.down = down;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
            dispatch(event);
        };
        const auto click_at = [&](uint8_t which, float frame_x, float frame_y) {
            const auto [window_x, window_y] = window_point(frame_x, frame_y);
            motion(window_x, window_y);
            button(which, true, window_x, window_y);
            button(which, false, window_x, window_y);
        };
        const auto same_orders = [&](const std::vector<UnitOrders>& had, const char* what) {
            require(read_orders() == had, std::string(what) + " changed a unit");
        };
        const KeptView kept = keep_view();
        const MatchFields before = read_fields();
        require(enter_photo_mode(), "photo mode did not open");
        render_match_surface();
        require(match_hud_cpu_.rgb.empty(), "the HUD layer was drawn");
        require(
            same_layout(
                match_layout_,
                layout::make_battlefield_layout(kept.placed.width, kept.placed.height)
            ),
            "the layout is not the battlefield at the window's size"
        );
        require(same_fields(before, read_fields()), "photo mode wrote the match");
        set_photo_show_selection(false);
        set_photo_show_health_bars(false);
        render_match_surface();
        const std::vector<uint8_t> plain = match_world_cpu_.rgb;
        set_photo_show_selection(true);
        render_match_surface();
        require(match_world_cpu_.rgb != plain, "Show selection drew the same frame");
        require(same_fields(before, read_fields()), "Show selection wrote the match");
        // Posted after the field compare: a line changes the message log.
        // The language stays the one the report was posted in, so the
        // under-attack ending photo mode reads is the one just written.
        if ((bits & extension_state::shared_match) != 0) {
            post_unit_report(commander_slot, "Under Attack");
            post_match_message("photo check", oa::sim::messages::kind_player_chat);
            const std::string under_attack = photo_latest_under_attack();
            require(!under_attack.empty(), "no under-attack line was posted");
            char clock[64]{};
            oa::ui::hud::format_game_time(clock, sizeof clock, match_->state().game.tick);
            const std::string status = photo_status_line();
            require(
                status.find(under_attack) != std::string::npos, "the under-attack line is missing"
            );
            require(
                std::string_view(clock).size() > 0 && status.find(clock) != std::string::npos,
                "the clock is missing"
            );
            require(photo_chat_lines_since_enter() == 1, "the chat count is not 1");
            require(status.find(" 1 ") != std::string::npos, "the chat count is missing");
        }
        const std::string kept_language = language_state().choice;
        language_state().choice = "de";
        apply_language_now();
        const std::string translated = photo_status_line();
        require(
            translated.find("Fotomodus") != std::string::npos ||
                translated.find("Esc beendet") != std::string::npos,
            "the photo line was not translated"
        );
        language_state().choice = kept_language;
        apply_language_now();
        move_photo_camera(0);
        leave_photo_mode();
        require(view_restored(kept), "leaving did not put the view back");

        // Follow is what T starts, and the frames after leaving are the
        // camera step. A pan while following, and T with follow off, both
        // leave follow as it was on entering.
        const auto follow_ref = [this] { return match_->state().game.follow_unit; };
        const auto settle_follow = [this] {
            for (int frame = 0; frame < 3; ++frame)
                move_match_camera();
        };
        tap(SDL_SCANCODE_T);
        require(
            match_tracking_ && tracked_match_unit_ == commander_slot && follow_ref() != 0,
            "T did not start follow"
        );
        const bool followed = match_tracking_;
        const uint16_t followed_unit = tracked_match_unit_;
        const uint32_t followed_ref = follow_ref();
        require(enter_photo_mode(), "photo mode did not open with follow on");
        settle_follow();
        require(!match_tracking_ && follow_ref() == 0, "follow carried into photo mode");
        tap(SDL_SCANCODE_T);
        require(match_tracking_ && follow_ref() != 0, "T did not follow in photo mode");
        scroll_match_view(1, 0, kPanStep);
        require(!match_tracking_ && follow_ref() == 0, "a pan left follow on in photo mode");
        leave_photo_mode();
        settle_follow();
        require(
            match_tracking_ == followed && tracked_match_unit_ == followed_unit &&
                follow_ref() == followed_ref,
            "leaving did not put follow back"
        );
        scroll_match_view(1, 0, kPanStep);
        require(!match_tracking_ && follow_ref() == 0, "a pan did not end follow");
        const uint32_t off_ref = follow_ref();
        require(enter_photo_mode(), "photo mode did not open with follow off");
        tap(SDL_SCANCODE_T);
        require(match_tracking_ && follow_ref() != 0, "T did not follow in photo mode");
        leave_photo_mode();
        settle_follow();
        require(
            !match_tracking_ && tracked_match_unit_ == 0 && follow_ref() == off_ref,
            "T in photo mode left follow on"
        );

        require(enter_photo_mode(), "photo mode did not open again");
        render_match_surface();
        const oa::ui::key_bindings::Contexts contexts = input_contexts();
        const uint32_t play_bit = context_bit("play");
        const uint32_t watch = context_bit("watch");
        const uint32_t placing = context_bit("placing");
        const uint32_t box = context_bit("box");
        const uint32_t panel = context_bit("panel");
        const uint32_t unit_info = context_bit("unit-info");
        require((contexts.active & context_bit("global")) != 0, "global is inactive");
        require((contexts.active & context_bit("match")) != 0, "match is inactive");
        require((contexts.active & context_bit("oa-screens")) != 0, "oa-screens is inactive");
        require((contexts.active & play_bit) == 0, "play is active");
        require((contexts.active & watch) == 0, "watch is active");
        require((contexts.active & placing) == 0, "placing is active");
        require((contexts.active & box) == 0, "box is active");
        require((contexts.active & panel) == 0, "panel is active");
        require((contexts.active & unit_info) == 0, "unit-info is active");

        const std::vector<UnitOrders> orders = read_orders();
        const uint16_t selected_before = selected_match_unit_;
        tap(SDL_SCANCODE_S);
        same_orders(orders, "Stop");
        require(match_command_ == MatchCommand::attack, "Stop disarmed the order");
        require(pending_build_type_ == 0, "Stop armed a build");
        require(commander != nullptr, "the commander is gone");
        const auto point = project_match_point(
            live_viewport(match_camera_x_, match_camera_z_),
            {static_cast<uint32_t>(commander->position.x),
             static_cast<uint32_t>(commander->position.y),
             static_cast<uint32_t>(commander->position.z)}
        );
        const auto gap = [](float left, float right) {
            return left > right ? left - right : right - left;
        };
        const float corner_x = 24.0F;
        const float corner_y = 24.0F;
        const float far_x = static_cast<float>(std::max(match_layout_.width - 24, 24));
        const float far_y = static_cast<float>(std::max(match_layout_.height - 24, 24));
        const float commander_x = static_cast<float>(point.x);
        const float commander_y = static_cast<float>(point.y);
        const float near_gap = gap(corner_x, commander_x) + gap(corner_y, commander_y);
        const float far_gap = gap(far_x, commander_x) + gap(far_y, commander_y);
        const float ground_x = far_gap > near_gap ? far_x : corner_x;
        const float ground_y = far_gap > near_gap ? far_y : corner_y;
        require(
            gap(ground_x, commander_x) + gap(ground_y, commander_y) > 80.0F,
            "open ground is on the commander"
        );
        click_at(SDL_BUTTON_LEFT, ground_x, ground_y);
        same_orders(orders, "a left click with Attack armed");
        require(match_command_ == MatchCommand::attack, "a left click used up the armed order");
        require(pending_build_type_ == 0, "a left click armed a build");
        // The other commander starts far from the camera. The click lands on
        // an enemy of the fight that the battlefield shows.
        oa::Unit* enemy = nullptr;
        float enemy_x = 0;
        float enemy_y = 0;
        const auto on_battlefield = [&](float x, float y) {
            return x >= 8.0F && y >= 8.0F && x < static_cast<float>(match_layout_.width - 8) &&
                   y < static_cast<float>(match_layout_.height - 8);
        };
        for (const auto& slot : match_->world().slots) {
            if (slot.unit_index == 0 || slot.unit == nullptr || slot.unit->type_index == 0 ||
                slot.owner_index == match_local_player_)
                continue;
            oa::Unit* candidate = oa::world_unit_at(&match_->state(), slot.unit_index);
            if (candidate == nullptr)
                continue;
            const auto enemy_point = project_match_point(
                live_viewport(match_camera_x_, match_camera_z_),
                {static_cast<uint32_t>(candidate->position.x),
                 static_cast<uint32_t>(candidate->position.y),
                 static_cast<uint32_t>(candidate->position.z)}
            );
            const float projected_x = static_cast<float>(enemy_point.x);
            const float projected_y = static_cast<float>(enemy_point.y);
            if (!on_battlefield(projected_x, projected_y) ||
                gap(projected_x, commander_x) + gap(projected_y, commander_y) <= 20.0F)
                continue;
            enemy = candidate;
            enemy_x = projected_x;
            enemy_y = projected_y;
            break;
        }
        require(enemy != nullptr, "the fight has no enemy on the battlefield");
        click_at(SDL_BUTTON_LEFT, enemy_x, enemy_y);
        same_orders(orders, "a click on an enemy");
        require(
            match_command_ == MatchCommand::attack, "a click on an enemy used up the armed order"
        );
        require(pending_build_type_ == 0, "a click on an enemy armed a build");
        require(
            selected_match_unit_ == selected_before, "a click on an enemy changed the selection"
        );
        match_command_ = MatchCommand::none;
        click_at(SDL_BUTTON_LEFT, ground_x, ground_y);
        same_orders(orders, "a left click with nothing armed");
        require(
            match_command_ == MatchCommand::none, "a left click with nothing armed armed an order"
        );
        require(pending_build_type_ == 0, "a left click with nothing armed armed a build");
        match_command_ = MatchCommand::attack;
        click_at(SDL_BUTTON_RIGHT, ground_x, ground_y);
        same_orders(orders, "a right click");
        require(match_command_ == MatchCommand::attack, "a right click cancelled the armed order");
        require(pending_build_type_ == 0, "a right click armed a build");
        const float drag_x = ground_x < static_cast<float>(match_layout_.width) / 2.0F
                                 ? ground_x + 36.0F
                                 : ground_x - 36.0F;
        const float drag_y = ground_y < static_cast<float>(match_layout_.height) / 2.0F
                                 ? ground_y + 28.0F
                                 : ground_y - 28.0F;
        const auto [drag_from_x, drag_from_y] = window_point(ground_x, ground_y);
        const auto [drag_to_x, drag_to_y] = window_point(drag_x, drag_y);
        motion(drag_from_x, drag_from_y);
        button(SDL_BUTTON_LEFT, true, drag_from_x, drag_from_y);
        motion(drag_to_x, drag_to_y);
        button(SDL_BUTTON_LEFT, false, drag_to_x, drag_to_y);
        same_orders(orders, "a box drag");
        require(match_command_ == MatchCommand::attack, "a box drag used up the armed order");
        require(pending_build_type_ == 0, "a box drag armed a build");

        commander->flags &= ~OA_UNIT_FLAG_SELECTED;
        selected_match_unit_ = 0;
        const std::vector<UnitOrders> cleared = read_orders();
        // Nothing is armed, so a click that reached the match would select
        // the commander.
        match_command_ = MatchCommand::none;
        click_at(SDL_BUTTON_LEFT, commander_x, commander_y);
        same_orders(cleared, "a click on the commander");
        require(selected_match_unit_ == 0, "a click on the commander selected a unit");
        require(
            (commander->flags & OA_UNIT_FLAG_SELECTED) == 0, "a click on the commander selected it"
        );
        require(match_command_ == MatchCommand::none, "a click on the commander armed an order");
        match_command_ = MatchCommand::attack;
        tap(SDL_SCANCODE_HOME);
        require(selected_match_unit_ == 0, "Home selected a unit");
        require((commander->flags & OA_UNIT_FLAG_SELECTED) == 0, "Home selected the commander");
        same_orders(cleared, "Home");
        require(photo_mode(), "Home left photo mode");
        tap(SDL_SCANCODE_RETURN);
        require(!chat_composing_, "Enter opened the chat line");
        require(photo_mode(), "Enter left photo mode");
        const uint16_t board = match_->state().game.graphics_flags;
        tap(SDL_SCANCODE_F4);
        require(
            match_->state().game.graphics_flags == board &&
                (match_->state().game.graphics_flags & oa::ui::hud::kGraphicsBoardPinned) ==
                    (board & oa::ui::hud::kGraphicsBoardPinned),
            "F4 pinned the kills board"
        );
        require(photo_mode(), "F4 left photo mode");
        tap(SDL_SCANCODE_SLASH, kPrimaryModifier);
        require(
            oa_layer().find(oa::ui::key_editor::name_keys_overlay) == nullptr,
            "the keys overlay opened"
        );
        require(photo_mode(), "the keys overlay key left photo mode");

        // The resize event is the one a window system posts. The layout it
        // lays out is read from the window, so the window changes first.
        const SDL_Point canvas_before = match_canvas_point();
        const int next_width = canvas_before.x == 1024 && canvas_before.y == 768 ? 800 : 1024;
        const int next_height = canvas_before.x == 1024 && canvas_before.y == 768 ? 600 : 768;
        require(
            sdl_.window != nullptr && SDL_SetWindowSize(sdl_.window, next_width, next_height) &&
                SDL_SyncWindow(sdl_.window),
            std::string("the window did not resize: ") + SDL_GetError()
        );
        SDL_Event resized{};
        resized.type = SDL_EVENT_WINDOW_RESIZED;
        resized.window.windowID = SDL_GetWindowID(sdl_.window);
        resized.window.data1 = next_width;
        resized.window.data2 = next_height;
        dispatch(resized);
        const SDL_Point canvas_after = match_canvas_point();
        require(
            canvas_after.x != canvas_before.x || canvas_after.y != canvas_before.y,
            "the window's size did not change"
        );
        // The same width, height and play layout apply_output_mode just stored.
        const SDL_Point canvas = match_canvas_point();
        int canvas_width = canvas.x;
        int canvas_height = canvas.y;
        int window_width = 0;
        int window_height = 0;
        if (sdl_.window != nullptr)
            SDL_GetWindowSize(sdl_.window, &window_width, &window_height);
        const SDL_Point scaled = scaled_frame_size();
        oa::ui::display_layout::Insets safe{};
        if (scaled.x > 0 && scaled.y > 0) {
            canvas_width = window_width = scaled.x;
            canvas_height = window_height = scaled.y;
        } else {
            safe = window_safe_insets();
        }
        auto saved_play = make_window_match_layout(
            canvas_width, canvas_height, window_width, window_height, safe
        );
        saved_play = layout::fit_side_column(saved_play, side_column_page_rows());
        saved_play = make_room_for_clock_line(saved_play);
        require(photo_ != nullptr, "a resize left photo mode");
        require(same_layout(photo_->layout, saved_play), "a resize did not keep the play layout");
        require(saved_play.hud_width != 0, "the play layout of the new size has no side column");
        require(
            same_layout(
                match_layout_, layout::make_battlefield_layout(canvas_width, canvas_height)
            ) && match_layout_.hud_width == 0 &&
                !match_layout_.phone,
            "a resize did not lay out the battlefield alone"
        );
        tap(SDL_SCANCODE_ESCAPE);
        require(!photo_mode(), "Escape left photo mode open");
        require(match_command_ == MatchCommand::attack, "Escape dropped the armed order");
        require(pending_build_type_ == 0, "Escape armed a build");
        require(same_layout(match_layout_, saved_play), "leaving put back the old window's layout");

        require(enter_photo_mode(), "photo mode did not open for the menu");
        tap(SDL_SCANCODE_F2);
        require(!photo_mode(), "F2 left photo mode open");
        require(pause_menu_shown() && match_paused_, "F2 did not open the game menu");
        resume_match_pause();
        require(!pause_menu_shown() && !match_paused_, "the game menu stayed open");
        require(enter_photo_mode(), "photo mode did not open for Settings");
        tap(SDL_SCANCODE_COMMA, kPrimaryModifier);
        require(!photo_mode(), "Settings left photo mode open");
        require(pause_menu_shown() && match_paused_, "Settings did not open the game menu");
        require(oa_layer().find("settings") != nullptr, "Settings did not open");
        // Settings is a modal screen. The next fight enters photo mode only
        // once it and the game menu are gone.
        if (LayerScreen* settings_screen = oa_layer().find("settings"))
            oa_layer().close(settings_screen);
        resume_match_pause();
        // The macOS application menu's Settings… item posts an event of
        // its own, registered on every platform.
        require(engine_settings_menu_event_ != 0, "the Settings item has no event");
        require(enter_photo_mode(), "photo mode did not open for the Settings item");
        SDL_Event chosen{};
        chosen.type = engine_settings_menu_event_;
        dispatch(chosen);
        require(!photo_mode(), "the Settings item left photo mode open");
        require(
            pause_menu_shown() && match_paused_, "the Settings item did not open the game menu"
        );
        require(oa_layer().find("settings") != nullptr, "the Settings item did not open Settings");
        if (LayerScreen* item_screen = oa_layer().find("settings"))
            oa_layer().close(item_screen);
        resume_match_pause();
        end_fight();
    };

    play_pair(0);
    check_interface(0);
    play_pair(extension_state::multiplayer | extension_state::shared_match);
    check_interface(extension_state::multiplayer | extension_state::shared_match);
    play_pair(
        extension_state::multiplayer | extension_state::shared_match |
        extension_state::local_watcher
    );
    check_interface(
        extension_state::multiplayer | extension_state::shared_match |
        extension_state::local_watcher
    );

    require(
        file_bytes(options_.preferences_file) == preferences_settled, "the preferences file changed"
    );
    std::printf("photo mode: kept the battlefield\n");
    std::fflush(stdout);
}

void Runtime::check_photo_time() {
    /// Puts the preferences file back to the bytes the check started with.
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& path = host.options_.preferences_file;
            if (path.has_value())
                existed = std::filesystem::exists(*path);
            bytes = file_bytes(path);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& path = runtime.options_.preferences_file;
            if (path.has_value()) {
                if (!existed) {
                    std::error_code error;
                    std::filesystem::remove(*path, error);
                } else {
                    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    /// Puts the extension state back, and clears the check's bits.
    struct ExtensionHold {
        Runtime& runtime;
        uint32_t (*state)(void*, const Runtime&);
        void* context;

        explicit ExtensionHold(Runtime& host)
            : runtime(host), state(host.extension_.state), context(host.extension_.context) {}

        ExtensionHold(const ExtensionHold&) = delete;
        ExtensionHold& operator=(const ExtensionHold&) = delete;

        ~ExtensionHold() {
            runtime.extension_.state = state;
            runtime.extension_.context = context;
            photo_extension_bits = 0;
        }
    };

    struct GamePace {
        uint16_t flags{};
        uint16_t requested{};
        uint16_t current{};
    };

    // The caller's bytes, put back when the hold ends, empty file included.
    // A fight writes the engine's own rows; those are compared separately,
    // after the fight that never enters photo mode.
    const std::vector<char> preferences_at_start = file_bytes(options_.preferences_file);
    {
        const PreferencesHold preferences_hold(*this);
        const ExtensionHold extension_hold(*this);
        const auto fail = [](const std::string& what) {
            throw std::runtime_error("photo time check: " + what);
        };
        const auto require = [&](bool ok, const std::string& what) {
            if (!ok)
                fail(what);
        };
        const auto step_tick = [this] {
            try {
                step_match_simulation();
            } catch (const std::exception& error) {
                report_match_tick_error(error.what());
            }
        };
        const auto pace_of = [this] {
            const oa::Game& game = match_->state().game;
            return GamePace{game.sim_run_flags, game.requested_speed, game.current_speed};
        };
        const auto pace_same = [](const GamePace& left, const GamePace& right) {
            return left.flags == right.flags && left.requested == right.requested &&
                   left.current == right.current;
        };
        const auto tick_now = [this] { return match_->state().game.tick; };
        const auto play_until = [&](uint32_t tick) {
            while (tick_now() < tick) {
                step_tick();
                render_match_surface();
            }
        };
        const auto start_fight = [&] {
            start_benchmark_skirmish();
            match_layout_ = lay_out_match(options_.match_width, options_.match_height);
            const float least = least_match_zoom();
            const float most = most_match_zoom();
            require(least <= kPlayZoom && most >= kPlayZoom, "zoom 1 is outside play's limits");
            match_zoom_ = kPlayZoom;
            match_zoom_target_ = kPlayZoom;
            spawn_combat_armies(kArmy);
            // A unit walking through the eighths, the same order in both
            // passes, so a frame between ticks is not the whole tick's.
            uint16_t walker = 0;
            for (const auto& slot : match_->world().slots) {
                if (slot.unit == nullptr || slot.record.type_index == 0 ||
                    slot.record.owner_index != match_local_player_ ||
                    !match_->takes_move_order(slot.unit_index))
                    continue;
                walker = slot.unit_index;
                break;
            }
            require(walker != 0, "the fight has no unit that can walk");
            const auto x =
                static_cast<int32_t>(match_->world().slots[walker].unit->position[0] >> 16);
            const auto z0 =
                static_cast<int32_t>(match_->world().slots[walker].unit->position[2] >> 16);
            const auto map_h = static_cast<int32_t>(selected_tnt_->tile_height * 32U);
            const auto z = std::min(z0 + 256, map_h - 64);
            const oa::sim::ground_orders::Point point{
                static_cast<int32_t>(static_cast<uint32_t>(x) << 16),
                static_cast<int32_t>(
                    static_cast<uint32_t>(match_->map_height(
                        static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16
                    ))
                    << 16
                ),
                static_cast<int32_t>(static_cast<uint32_t>(z) << 16)
            };
            match_->issue_ground_move(walker, point, false);
        };
        const auto end_fight = [this] {
            if (photo_mode())
                leave_photo_mode();
            if (match_ != nullptr) {
                leave_match();
                load(Screen::main_menu);
            }
        };
        const auto clock_text = [this] {
            char clock[64]{};
            oa::ui::hud::format_game_time(clock, sizeof clock, match_->state().game.tick);
            return std::string(clock);
        };

        const auto play_entered = [&] {
            start_fight();
            const GamePace pace = pace_of();
            play_until(30);
            require(tick_now() == 30, "the fight did not reach tick 30");
            require(enter_photo_mode(), "photo mode did not open");
            require(photo_frozen(), "entering did not freeze the match");
            require(!match_clock_steps(), "the frozen match's clock still steps");
            require(!photo_freeze(), "freezing again acted");
            require(!photo_hold_picture(), "hold picture acted on a local match");
            require(pace_same(pace, pace_of()), "freeze wrote the pause or the speed");
            const std::string frozen_word{oa::data::languages::interface_text("Frozen")};
            const std::string frozen_clock = clock_text();
            std::vector<uint8_t> frozen_frame;
            for (int frame = 0; frame < 40; ++frame) {
                render_match_surface();
                require(tick_now() == 30, "a frozen draw advanced the tick");
                if (frame == 0)
                    frozen_frame = match_world_cpu_.rgb;
                require(
                    !frozen_frame.empty() && match_world_cpu_.rgb == frozen_frame,
                    "two frozen draws of the same frame differ"
                );
            }
            const std::string frozen_line = photo_status_line();
            require(
                frozen_line.find(frozen_word) != std::string::npos, "the line does not say Frozen"
            );
            require(
                frozen_line.find(frozen_clock) != std::string::npos,
                "the frozen line has no game clock"
            );
            for (int step = 0; step < 5; ++step) {
                require(photo_step(), "a single step did not act");
                render_match_surface();
            }
            require(tick_now() == 35, "five steps did not reach tick 35");
            require(photo_frozen(), "a step did not leave the match frozen");
            require(!match_clock_steps(), "the clock steps after a single step");
            require(pace_same(pace, pace_of()), "step wrote the pause or the speed");
            // The frame of the whole tick, before the eighths replace it.
            const std::vector<uint8_t> whole_tick_frame = match_world_cpu_.rgb;
            std::vector<uint8_t> eighth_zero;
            std::vector<uint8_t> eighth_eight;
            for (int eighth = 0; eighth <= 8; ++eighth) {
                require(photo_between(eighth), "an eighth between ticks did not act");
                render_match_surface();
                if (eighth == 0)
                    eighth_zero = match_world_cpu_.rgb;
                if (eighth == 8)
                    eighth_eight = match_world_cpu_.rgb;
            }
            require(
                !eighth_zero.empty() && eighth_zero != eighth_eight,
                "the frame at eighth 0 matches the frame at eighth 8"
            );
            require(
                eighth_eight == whole_tick_frame,
                "eighth 8 is not the frame of the whole tick before the eighths"
            );
            require(!photo_between(9), "an eighth past the tick acted");
            require(tick_now() == 35, "drawing between ticks advanced the tick");
            require(pace_same(pace, pace_of()), "between ticks wrote the pause or the speed");
            require(!photo_slow(0) && !photo_slow(10), "a speed outside 1 to 9 acted");
            require(photo_slow(1), "slow did not act");
            require(photo_slow_speed() == 1, "slow is not speed 1");
            require(!photo_between(4), "between ticks acted while playing slowly");
            require(!match_clock_steps(), "the slow match's clock still steps");
            require(pace_same(pace, pace_of()), "slow wrote the pause or the speed");
            const std::string slow_line = photo_status_line();
            require(
                slow_line.find(oa::data::languages::interface_text("Slow")) != std::string::npos &&
                    slow_line.find(" 3") != std::string::npos,
                "the line does not say Slow at 3 ticks a second"
            );
            require(smooth_motion(), "smooth motion is off");
            const uint64_t interval = frame_pacing::kNanosecondsPerSecond / 3U;
            for (int grant = 0; grant < 15; ++grant) {
                frame_time_ns_ += interval;
                grant_photo_slow_ticks();
                render_match_surface();
                // Half an interval later the picture has glided on. The clock
                // is put back, so the grants that follow stay on the interval.
                if (grant == 0) {
                    const std::vector<uint8_t> at_tick = match_world_cpu_.rgb;
                    const uint32_t tick = tick_now();
                    frame_time_ns_ += interval / 2U;
                    render_match_surface();
                    require(
                        match_world_cpu_.rgb != at_tick,
                        "a frame half an interval after a slow tick matches the frame at it"
                    );
                    require(tick_now() == tick, "the half interval played a tick");
                    frame_time_ns_ -= interval / 2U;
                }
            }
            require(tick_now() == 50, "slow did not play 15 ticks");
            require(pace_same(pace, pace_of()), "the slow ticks wrote the pause or the speed");
            require(photo_run(), "run did not act");
            require(!photo_frozen() && photo_slow_speed() == 0, "run left the match held");
            require(match_clock_steps(), "the clock does not step after run");
            require(pace_same(pace, pace_of()), "run wrote the pause or the speed");
            play_until(70);
            require(tick_now() == 70, "the fight did not reach tick 70");
            leave_photo_mode();
            require(!photo_mode(), "leaving did not end photo mode");
            require(match_clock_steps(), "the clock does not step after leaving");
            require(pace_same(pace, pace_of()), "leaving wrote the pause or the speed");
            play_until(120);
            require(tick_now() == 120, "the entered pass did not reach tick 120");
            require(pace_same(pace, pace_of()), "the fight wrote the pause or the speed");
            DrawReadBack read = match_draw_read_back();
            end_fight();
            return read;
        };
        const auto play_stayed = [&] {
            start_fight();
            play_until(120);
            require(tick_now() == 120, "the other pass did not reach tick 120");
            require(!photo_mode(), "the other pass entered photo mode");
            DrawReadBack read = match_draw_read_back();
            end_fight();
            return read;
        };

        photo_extension_bits = 0;
        extension_.state = photo_extension_state;
        // The first fight never enters photo mode, and writes the engine's
        // own rows. Photo mode must add nothing after that.
        const DrawReadBack stayed = play_stayed();
        const std::vector<char> preferences_settled = file_bytes(options_.preferences_file);
        const DrawReadBack entered = play_entered();
        const char* difference = draw_read_back_difference(entered, stayed);
        require(difference == nullptr, difference == nullptr ? "" : std::string(difference));

        const auto hold_shared = [&](uint32_t bits, const char* who) {
            photo_extension_bits = bits;
            extension_.state = photo_extension_state;
            start_fight();
            const GamePace pace = pace_of();
            require(enter_photo_mode(), std::string(who) + " did not enter photo mode");
            require(!photo_frozen(), std::string(who) + " froze");
            require(match_clock_steps(), std::string(who) + " held the clock");
            require(
                !photo_freeze() && !photo_slow(1) && !photo_step() && !photo_between(4),
                std::string(who) + " accepted a time command"
            );
            require(!photo_frozen() && photo_slow_speed() == 0, std::string(who) + " changed time");
            require(pace_same(pace, pace_of()), std::string(who) + " wrote the pause or the speed");
            const std::string before_clock = clock_text();
            const uint32_t from = tick_now();
            play_until(from + 30);
            const std::string after_clock = clock_text();
            require(after_clock != before_clock, std::string(who) + " clock did not advance");
            const std::string running = photo_status_line();
            require(
                running.find(after_clock) != std::string::npos,
                std::string(who) + " line lost the clock"
            );
            require(
                running.find(oa::data::languages::interface_text("Frozen")) == std::string::npos,
                std::string(who) + " line says Frozen"
            );
            require(match_clock_steps(), std::string(who) + " clock stopped");
            render_match_surface();
            const std::vector<uint8_t> copied = match_world_cpu_.rgb;
            require(!copied.empty(), std::string(who) + " drew an empty battlefield");
            const int32_t camera_x = match_camera_x_;
            const int32_t camera_z = match_camera_z_;
            const float zoom = match_zoom_;
            const float zoom_target = match_zoom_target_;
            require(photo_hold_picture(), std::string(who) + " did not hold the picture");
            require(
                photo_picture_held() && photo_camera_locked(), std::string(who) + " is not held"
            );
            scroll_match_view(1, 0, kPanStep);
            handle_match_zoom(
                1.0F,
                static_cast<float>(match_layout_.left + 8),
                static_cast<float>(match_layout_.top + 8),
                false
            );
            bool keep_running = true;
            const auto dispatch = [&](SDL_Event& event) {
                dispatch_event(event, keep_running);
                require(keep_running, std::string(who) + " key ended the run");
            };
            const auto tap = [&](SDL_Scancode scancode) {
                require(sdl_.window != nullptr, std::string(who) + " has no window");
                SDL_Event event{};
                event.key.windowID = SDL_GetWindowID(sdl_.window);
                event.key.scancode = scancode;
                event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
                event.key.mod = SDL_KMOD_NONE;
                for (const bool down : {true, false}) {
                    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                    event.key.down = down;
                    event.key.repeat = false;
                    dispatch(event);
                }
            };
            // Home centres and T follows. Neither may move the held picture.
            tap(SDL_SCANCODE_HOME);
            tap(SDL_SCANCODE_T);
            require(
                match_camera_x_ == camera_x && match_camera_z_ == camera_z && match_zoom_ == zoom,
                std::string(who) + " reframed the held picture"
            );
            require(
                match_zoom_target_ == zoom_target, std::string(who) + " zoomed the held picture"
            );
            const std::string held_line = photo_status_line();
            require(
                held_line.find(oa::data::languages::interface_text("Held")) != std::string::npos,
                std::string(who) + " line does not say Held"
            );
            const uint32_t held_from = tick_now();
            while (tick_now() < held_from + 30) {
                step_tick();
                render_match_surface();
                require(
                    match_world_cpu_.rgb == copied,
                    std::string(who) + " held picture changed while the match ran"
                );
            }
            const std::string held_clock = clock_text();
            require(held_clock != after_clock, std::string(who) + " clock stopped while held");
            require(
                photo_status_line().find(held_clock) != std::string::npos,
                std::string(who) + " line clock stopped while held"
            );
            require(
                pace_same(pace, pace_of()), std::string(who) + " hold wrote the pause or the speed"
            );
            // Letting go of the picture puts the camera back, still in photo mode.
            require(photo_hold_picture(), std::string(who) + " did not let go of the picture");
            require(!photo_picture_held(), std::string(who) + " picture stayed held");
            require(
                match_camera_x_ == camera_x && match_camera_z_ == camera_z && match_zoom_ == zoom,
                std::string(who) + " camera was not put back"
            );
            leave_photo_mode();
            require(!photo_mode() && !photo_picture_held(), std::string(who) + " did not let go");
            // The picture is live again: a pan the hold refused now moves the
            // camera, and the battlefield drawn from there is not the copy.
            const int32_t released_x = match_camera_x_;
            scroll_match_view(1, 0, kPanStep);
            require(match_camera_x_ != released_x, std::string(who) + " camera stayed locked");
            render_match_surface();
            require(
                match_world_cpu_.rgb != copied, std::string(who) + " still shows the held picture"
            );
            require(
                pace_same(pace, pace_of()),
                std::string(who) + " leaving wrote the pause or the speed"
            );
            end_fight();
        };
        hold_shared(extension_state::multiplayer | extension_state::shared_match, "a shared match");
        hold_shared(
            extension_state::multiplayer | extension_state::shared_match |
                extension_state::local_watcher,
            "a watcher"
        );
        require(
            file_bytes(options_.preferences_file) == preferences_settled,
            "the preferences file changed"
        );
    }
    if (file_bytes(options_.preferences_file) != preferences_at_start)
        throw std::runtime_error("photo time check: the preferences file was not put back");
    std::printf("photo time: kept the battlefield\n");
    std::fflush(stdout);
}

void Runtime::exercise_shared_photo(uint32_t tick) {
    if (match_ == nullptr)
        throw std::runtime_error("net loopback check: photo mode has no match");
    const auto require = [](bool ok, const char* what) {
        if (!ok)
            throw std::runtime_error(std::string("net loopback check: ") + what);
    };
    auto& game = match_->state().game;
    if (tick == 100) {
        const uint16_t flags = game.sim_run_flags;
        const uint16_t requested = game.requested_speed;
        const uint16_t current = game.current_speed;
        require(enter_photo_mode(), "photo mode did not open on the host");
        scroll_match_view(1, 0, 4.0);
        const float least = least_match_zoom();
        const float most = most_match_zoom();
        const float zoom = std::min(most, std::max(least, 1.25F));
        match_zoom_ = zoom;
        match_zoom_target_ = zoom;
        require(!photo_freeze(), "freeze acted in the shared match");
        require(!photo_slow(1), "slow acted in the shared match");
        require(!photo_step(), "step acted in the shared match");
        require(!photo_between(4), "between ticks acted in the shared match");
        require(game.sim_run_flags == flags, "photo mode wrote the pause bit");
        require(
            game.requested_speed == requested && game.current_speed == current,
            "photo mode wrote the speed"
        );
        require(photo_mode(), "photo mode closed");
        return;
    }
    if (tick == 170) {
        require(photo_mode(), "photo mode was not open to hold the picture");
        render_match_surface();
        require(photo_hold_picture(), "hold picture did not act");
        require(photo_picture_held() && photo_camera_locked(), "the picture is not held");
        const int32_t camera_x = match_camera_x_;
        scroll_match_view(1, 0, 4.0);
        require(match_camera_x_ == camera_x, "the held picture was reframed");
        return;
    }
    if (tick == 200) {
        leave_photo_mode();
        require(!photo_mode() && !photo_picture_held(), "photo mode stayed open");
    }
}

} // namespace oa::app
