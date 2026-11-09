// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Photo mode: the running match with its interface put away. The battlefield
// fills the window from the camera the player had. A match on this machine
// freezes; a shared match, a watcher and a replay keep the clock they had.
// The match keeps its own presenter and sound, and leaving puts the view back.
// --check-photo mode drives entering and leaving.
#include "oa/app/runtime.hpp"

#include "oa/app/frame_pacing.hpp"
#include "oa/app/hook_call.hpp"

#include "director_state.hpp"
#include "oa_layer.hpp"
#include "photo_camera.hpp"
#include "photo_state.hpp"

#include "presentation_interpolation.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/sim/messages.hpp"
#include "oa/sim/selection.hpp"
#include "oa/sim/spatial_state/spatial.hpp"
#include "oa/sim/speed.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/console/hotkeys.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/hud/resource_panel.hpp"
#include "oa/ui/key_editor/overlay.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

static_assert(oa::app::kPhotoProcessorFloor == oa::app::kMinBattlefieldZoom);
static_assert(oa::app::kPhotoFullFloor == oa::app::kMinFullBattlefieldZoom);

namespace oa::app {
namespace {

namespace editor = oa::ui::key_editor;
namespace hud = oa::ui::hud;
namespace messages = oa::sim::messages;

/// The photo screen's name on the Open Annihilation layer.
constexpr std::string_view kPhotoScreen = "photo-mode";

/// Restores fields a visible-unit collect writes, if the collect throws.
struct KeptViewRestore {
    oa::Game& game;
    uint32_t camera_x;
    uint32_t camera_y;
    oa::Rect32 rect;
    int32_t hot_count;

    ~KeptViewRestore() {
        game.camera_x = camera_x;
        game.camera_y = camera_y;
        game.battlefield_rect = rect;
        game.hot_unit_count = hot_count;
    }
};

} // namespace

namespace {

/// One line of the message log.
///
/// @param game the match's Game block
/// @param index the ring slot, already wrapped
/// @return the line
[[nodiscard]] messages::MessageLine chat_line(const oa::Game& game, uint32_t index) {
    messages::MessageLine line{};
    std::memcpy(&line, game.chat_lines[index], sizeof line);
    return line;
}

/// Walks the message-log lines posted since photo mode was entered.
///
/// @param game the match's Game block
/// @param saved_head the next write slot remembered on entering
/// @param saved_line the line that stood in that slot on entering
/// @param visit called with each new line, oldest first
template <typename Visit>
void for_lines_since_entry(
    const oa::Game& game,
    uint16_t saved_head,
    const messages::MessageLine& saved_line,
    Visit&& visit
) {
    const uint32_t saved = static_cast<uint32_t>(saved_head) % OA_CHAT_LINE_COUNT;
    const uint32_t head = static_cast<uint32_t>(game.chat_head) % OA_CHAT_LINE_COUNT;
    if (saved == head) {
        const messages::MessageLine now = chat_line(game, saved);
        if (std::memcmp(&now, &saved_line, sizeof now) == 0)
            return;
        for (uint32_t index = 0; index < OA_CHAT_LINE_COUNT; ++index)
            visit(chat_line(game, index));
        return;
    }
    for (uint32_t index = saved; index != head; index = (index + 1U) % OA_CHAT_LINE_COUNT)
        visit(chat_line(game, index));
}

} // namespace

void Runtime::destroy_photo_state(PhotoState* state) noexcept {
    delete state;
}

bool Runtime::photo_mode() const noexcept {
    return photo_ != nullptr;
}

bool Runtime::match_interface_hidden() const noexcept {
    return (director_ != nullptr && !director_->presentation.show_interface) || photo_mode();
}

bool Runtime::photo_show_selection() const noexcept {
    return photo_ != nullptr && photo_->show_selection;
}

bool Runtime::photo_show_health_bars() const noexcept {
    return photo_ != nullptr && photo_->show_health_bars;
}

void Runtime::set_photo_show_selection(bool show) noexcept {
    if (photo_ != nullptr)
        photo_->show_selection = show;
}

void Runtime::set_photo_show_health_bars(bool show) noexcept {
    if (photo_ != nullptr)
        photo_->show_health_bars = show;
}

void Runtime::refresh_photo_kept_view() {
    if (photo_ == nullptr || match_ == nullptr)
        return;
    auto& world = match_->state();
    auto& game = world.game;
    if (photo_->kept_units.size() != world.unit_slot_count)
        photo_->kept_units.assign(world.unit_slot_count, 0);
    const KeptViewRestore restore{
        game, game.camera_x, game.camera_y, game.battlefield_rect, game.hot_unit_count
    };
    game.camera_x = photo_->kept_camera_x;
    game.camera_y = photo_->kept_camera_y;
    game.battlefield_rect = photo_->kept_rect;
    oa::sim::selection::VisibleLists lists{};
    lists.units = photo_->kept_units.data();
    lists.unit_capacity = static_cast<uint32_t>(photo_->kept_units.size());
    oa::sim::selection::collect_visible_units(world, lists, selection_hooks());
    const int32_t counted = game.hot_unit_count;
    photo_->kept_count =
        counted > 0 ? std::min(static_cast<uint32_t>(counted), lists.unit_capacity) : 0U;
    offline_services_.set_on_screen_test(&PhotoState::unit_kept, photo_.get());
}

std::string Runtime::photo_status_line() {
    // A photo that finished since the last line is reported here, on the
    // frame's thread.
    publish_photo_status();
    // The engine's own words, from the interface catalogue. 3.1c's tables
    // have none of these, so translate_ui would leave them in English.
    std::string line{oa::data::languages::interface_text("Photo mode")};
    line += "  ";
    line += oa::data::languages::interface_text("Esc leaves");
    if (photo_ != nullptr && match_ != nullptr) {
        const uint32_t bits = current_extension_state();
        const bool shared =
            (bits & (extension_state::shared_match | extension_state::local_watcher)) != 0;
        // A shared match never shows Frozen: its clock is the battle's, and it
        // keeps running. Freeze is this machine's hold, beside the pause key.
        // A replay says Frozen only while its own pause is on.
        const bool replay = !shared && photo_time_is_replay();
        const bool replay_paused =
            replay && (match_->state().game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0;
        const bool frozen =
            !shared && (replay ? replay_paused : photo_->time == PhotoState::Time::frozen);
        const bool slow = !shared && !frozen && photo_->time == PhotoState::Time::slow;
        if (frozen) {
            line += "  ";
            line += oa::data::languages::interface_text("Frozen");
        } else if (slow) {
            line += "  ";
            line += oa::data::languages::interface_text("Slow");
            line += ' ';
            line += std::to_string(photo_->slow_speed * 3);
        }
        if (photo_->picture_held) {
            line += "  ";
            line += oa::data::languages::interface_text("Held");
        }
        if (shared || frozen || slow || photo_->picture_held) {
            char clock[64]{};
            hud::format_game_time(clock, sizeof clock, match_->state().game.tick);
            line += "  ";
            line += clock;
        }
        if (shared) {
            const std::string under_attack = photo_latest_under_attack();
            if (!under_attack.empty()) {
                line += "  ";
                line += under_attack;
            }
            line += "  ";
            line += std::to_string(photo_chat_lines_since_enter());
            line += ' ';
            line += oa::data::languages::interface_text("chat lines");
        }
    }
    if (!photo_notice_.empty()) {
        line += "  ";
        line += photo_notice_;
    }
    return line;
}

std::string Runtime::photo_latest_under_attack() {
    if (photo_ == nullptr || match_ == nullptr)
        return {};
    const std::string ending = ": " + translate_ui("Under Attack");
    std::string latest;
    for_lines_since_entry(
        match_->state().game,
        photo_->chat_head,
        photo_->chat_at_head,
        [&](const messages::MessageLine& row) {
            if ((row.kind & messages::kind_mask) != messages::kind_unit_report)
                return;
            const std::string_view text(row.text);
            if (text.size() >= ending.size() && text.ends_with(ending))
                latest = std::string(text);
        }
    );
    return latest;
}

namespace {

/// The offer for the picture that is drawing now.
///
/// @param turned the turned view is drawing
/// @param card Full's classic view is drawing
/// @param map_width map pixels across the shown map
/// @param map_height map pixels down it
/// @param battlefield_width screen pixels across the battlefield
/// @param battlefield_height screen pixels down it
/// @return the offer
PhotoZoomRange photo_offer_now(
    bool turned,
    bool card,
    int32_t map_width,
    int32_t map_height,
    int32_t battlefield_width,
    int32_t battlefield_height
) noexcept {
    const PhotoTier tier = turned ? PhotoTier::turned
                           : card ? PhotoTier::full_classic
                                  : PhotoTier::processor;
    return photo_zoom_range(tier, map_width, map_height, battlefield_width, battlefield_height);
}

} // namespace

std::array<float, 2> Runtime::photo_zoom_limits() const noexcept {
    const auto [map_width, map_height] = shown_map_size();
    const PhotoZoomRange offer = photo_offer_now(
        live_renderer_active(),
        full_presentation(),
        map_width,
        map_height,
        match_layout_.battlefield_width(),
        match_layout_.battlefield_height()
    );
    return {offer.least, offer.most};
}

std::string Runtime::photo_zoom_reason() const {
    const auto [map_width, map_height] = shown_map_size();
    const PhotoZoomRange offer = photo_offer_now(
        live_renderer_active(),
        full_presentation(),
        map_width,
        map_height,
        match_layout_.battlefield_width(),
        match_layout_.battlefield_height()
    );
    if (offer.cut != PhotoZoomCut::card)
        return {};
    return std::string(
        oa::data::languages::interface_text("The graphics card draws no farther out than this.")
    );
}

void Runtime::snap_photo_zoom_to_far_end() {
    // A held picture keeps the zoom and the target it was copied with, even
    // when a failed turned frame leaves the view classic under the hold.
    if (!photo_mode() || !selected_tnt_ || photo_camera_locked())
        return;
    const std::array<float, 2> offered = photo_zoom_limits();
    const float least = offered[0];
    // Already within the offer: a wheel zoom still eases. Only a zoom the
    // tier in use no longer draws is taken there before the frame.
    if (!(match_zoom_ < least))
        return;
    match_zoom_target_ = least;
    const auto wide = static_cast<float>(match_layout_.battlefield_width());
    const auto high = static_cast<float>(match_layout_.battlefield_height());
    zoom_focus_ = {
        false,
        static_cast<float>(match_layout_.left) + wide / 2.0F,
        static_cast<float>(match_layout_.top) + high / 2.0F
    };
    zoom_view_about(least, static_cast<double>(wide) / 2.0, static_cast<double>(high) / 2.0);
}

void Runtime::clear_match_hover() noexcept {
    hovered_.reset();
    hovered_match_unit_ = 0;
    unit_panel_hover_name_.clear();
    unit_panel_hover_description_.clear();
    if (match_ == nullptr)
        return;
    auto& game = match_->state().game;
    game.cursor_unit_id = 0;
    game.cursor_feature = oa::sim::spatial_state::no_feature;
}

uint32_t Runtime::photo_chat_lines_since_enter() {
    if (photo_ == nullptr || match_ == nullptr)
        return 0;
    uint32_t count = 0;
    for_lines_since_entry(
        match_->state().game,
        photo_->chat_head,
        photo_->chat_at_head,
        [&](const messages::MessageLine& row) {
            if ((row.kind & messages::kind_mask) == messages::kind_player_chat)
                ++count;
        }
    );
    return count;
}

bool Runtime::enter_photo_mode() {
    if (photo_ != nullptr)
        return true;
    if (match_ == nullptr || screen_ != Screen::match)
        return false;
    if (match_finished_ || director_mode())
        return false;
    if (pause_menu_shown() || team_panel_open())
        return false;
    if (oa_layer().modal_shown(true))
        return false;
    if (chat_composing_)
        return false;

    std::unique_ptr<PhotoState, void (*)(PhotoState*) noexcept> state{
        new PhotoState{}, destroy_photo_state
    };
    state->layout = match_layout_;
    state->camera_x = match_camera_x_;
    state->camera_z = match_camera_z_;
    state->zoom = match_zoom_;
    state->zoom_target = match_zoom_target_;
    state->exact = exact_view_;
    state->hold = view_hold_;
    state->tracking = match_tracking_;
    state->tracked_unit = tracked_match_unit_;
    state->follow_unit = match_->state().game.follow_unit;
    if (live_renderer_ != nullptr) {
        state->live_kept = true;
        const std::array<float, 2> angles = live_camera_angles();
        state->live_yaw = angles[0];
        state->live_pitch = angles[1];
        state->live_unlocked = live_renderer_unlocked();
    }
    state->keys_overlay = oa_layer().find(editor::name_keys_overlay) != nullptr;
    state->cursor_was_hidden = frame_without_cursor_;
    auto& game = match_->state().game;
    state->kept_camera_x = game.camera_x;
    state->kept_camera_y = game.camera_y;
    state->kept_rect = game.battlefield_rect;
    state->chat_head = game.chat_head;
    const uint32_t slot = static_cast<uint32_t>(game.chat_head) % OA_CHAT_LINE_COUNT;
    std::memcpy(&state->chat_at_head, game.chat_lines[slot], sizeof state->chat_at_head);
    photo_ = std::move(state);

    const int width = match_layout_.width > 0 ? match_layout_.width : options_.match_width;
    const int height = match_layout_.height > 0 ? match_layout_.height : options_.match_height;
    match_layout_ = oa::ui::display_layout::make_battlefield_layout(width, height);
    // Follow does not carry in. The camera stays where the player was
    // looking. Clearing the field as well as the flags keeps the next frame
    // from reading it and centring the photo camera. T and a pan change
    // follow for this visit only; leaving writes the saved value back.
    match_tracking_ = false;
    tracked_match_unit_ = 0;
    match_->state().game.follow_unit = 0;
    // The pointer pass keeps running and would leave its line in the bar.
    clear_match_hover();
    terrain_cache_cam_x_ = kUncachedTerrainCamera;
    frame_without_cursor_ = true;
    if (photo_->keys_overlay) {
        if (LayerScreen* open = oa_layer().find(editor::name_keys_overlay))
            oa_layer().close(open);
    }
    try {
        open_photo_panel();
        refresh_photo_kept_view();
        // Freeze after the screen is up, so a throw still leaves cleanly.
        // A replay keeps its own pause. A shared match is not held.
        if (photo_time_is_local()) {
            photo_->time = PhotoState::Time::frozen;
            snap_held_clock();
        } else if (photo_time_is_replay())
            photo_->entry_speed = match_->state().game.requested_speed;
    } catch (...) {
        leave_photo_mode();
        return false;
    }
    // A photo that finished while photo mode was off still reaches the status
    // line. The line itself starts clear, including a visit after one that
    // saved a photo.
    publish_photo_status();
    photo_notice_.clear();
    return true;
}

void Runtime::leave_photo_mode(bool match_ending, bool restore_keys_overlay) noexcept {
    if (photo_ == nullptr)
        return;
    // A photo asked for before a frame, then left in that same visit, is
    // dropped. Say so, where the line used to stay quiet.
    if (photo_->capture_waiting) {
        photo_->capture_waiting = false;
        try {
            const std::string missed(oa::data::languages::interface_text("Photo not taken"));
            status_ = missed;
            photo_notice_ = missed;
            // The panel is gone once this returns. The match's message log
            // is what the player still sees.
            if (match_ != nullptr) {
                post_match_message(
                    missed, oa::sim::messages::kind_status, 0, oa::sim::messages::sender_none
                );
            }
        } catch (...) {
        }
    }
    // A feature the photo view put under the pointer stays in the bar.
    clear_match_hover();
    // The time field alone: this function must not ask the extension, whose
    // state hook can throw. A replay's pause bit and speed are left as they are.
    const bool resume =
        photo_->time == PhotoState::Time::frozen || photo_->time == PhotoState::Time::slow;
    const auto state = std::move(photo_);
    match_layout_ = state->layout;
    match_camera_x_ = state->camera_x;
    match_camera_z_ = state->camera_z;
    match_zoom_ = state->zoom;
    match_zoom_target_ = state->zoom_target;
    exact_view_ = state->exact;
    view_hold_ = state->hold;
    match_tracking_ = state->tracking;
    tracked_match_unit_ = state->tracked_unit;
    if (match_ != nullptr)
        match_->state().game.follow_unit = state->follow_unit;
    terrain_cache_cam_x_ = kUncachedTerrainCamera;
    frame_without_cursor_ = state->cursor_was_hidden;
    if (state->live_kept)
        restore_live_camera(state->live_yaw, state->live_pitch, state->live_unlocked);
    if (LayerScreen* screen = oa_layer().find(kPhotoScreen))
        oa_layer().close(screen);
    // Put the overlay back as it was on entering. One opened during the
    // visit closes. One that was open, and is not open now, opens. The
    // match ending leaves it as it is: victory sets the finished flag
    // after this returns, and does not reopen one that was closed on
    // entering. A press on the overlay itself asks not to restore, so
    // this does not close that screen under the press.
    if (restore_keys_overlay && !match_ending && !match_finished_) {
        const bool open_now = oa_layer().find(editor::name_keys_overlay) != nullptr;
        if (state->keys_overlay != open_now)
            toggle_keys_overlay_screen();
    }
    if (match_ == nullptr) {
        offline_services_.set_on_screen_test(nullptr, nullptr);
        if (resume)
            snap_held_clock();
        return;
    }
    try {
        refresh_on_screen_view();
    } catch (...) {
        offline_services_.set_on_screen_test(nullptr, nullptr);
    }
    // The fight continues from this tick. The time the picture was held is
    // not played.
    if (resume)
        snap_held_clock();
}

bool Runtime::photo_time_is_local() const {
    const uint32_t bits = current_extension_state();
    return (bits & (extension_state::shared_match | extension_state::local_watcher |
                    extension_state::replay)) == 0;
}

bool Runtime::photo_time_is_replay() const {
    const uint32_t bits = current_extension_state();
    if ((bits & (extension_state::shared_match | extension_state::local_watcher)) != 0)
        return false;
    return (bits & extension_state::replay) != 0;
}

bool Runtime::photo_clock_held() const {
    return photo_ != nullptr && photo_time_is_local() &&
           (photo_->time == PhotoState::Time::frozen || photo_->time == PhotoState::Time::slow);
}

bool Runtime::photo_frozen() const noexcept {
    return photo_ != nullptr && photo_->time == PhotoState::Time::frozen;
}

int32_t Runtime::photo_slow_speed() const noexcept {
    if (photo_ == nullptr || photo_->time != PhotoState::Time::slow)
        return 0;
    return photo_->slow_speed;
}

bool Runtime::photo_picture_held() const noexcept {
    return photo_ != nullptr && photo_->picture_held;
}

bool Runtime::photo_camera_locked() const noexcept {
    return photo_picture_held();
}

bool Runtime::photo_freeze() {
    if (photo_ == nullptr || match_ == nullptr)
        return false;
    if (!photo_time_is_local()) {
        if (!photo_time_is_replay())
            return false;
        auto& game = match_->state().game;
        if ((game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0)
            return false;
        run_console_hotkey(oa::ui::console::hotkey::pause);
        if ((game.sim_run_flags & oa::ui::console::kSimRunPaused) == 0)
            return false;
        photo_->time = PhotoState::Time::frozen;
        photo_->shutter = false;
        return true;
    }
    if (photo_->time == PhotoState::Time::frozen)
        return false;
    photo_->time = PhotoState::Time::frozen;
    photo_->slow_speed = 0;
    photo_->shutter = false;
    snap_held_clock();
    return true;
}

bool Runtime::photo_run() {
    if (photo_ == nullptr || match_ == nullptr)
        return false;
    if (!photo_time_is_local()) {
        if (!photo_time_is_replay())
            return false;
        bool acted = false;
        auto& game = match_->state().game;
        if ((game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0) {
            run_console_hotkey(oa::ui::console::hotkey::pause);
            acted = (game.sim_run_flags & oa::ui::console::kSimRunPaused) == 0;
        }
        if (photo_->slow_speed != 0) {
            auto& world = match_->state();
            const uint16_t before = world.game.requested_speed;
            const int32_t restore = static_cast<int32_t>(photo_->entry_speed);
            std::ignore =
                oa::sim::speed::set_speed(world, restore, message_hooks(), game_speed_range());
            match_timing_.requested_rate = world.game.requested_speed;
            match_timing_.actual_rate = world.game.current_speed;
            preferences_.current_game_speed = world.game.current_speed;
            if (world.game.requested_speed != before)
                call_hook_or_report<&Extension::speed_changed>(
                    extension_, hook_error_report(), *this, world.game.requested_speed
                );
            photo_->slow_speed = 0;
            acted = true;
        }
        if (!acted)
            return false;
        photo_->time = PhotoState::Time::running;
        photo_->shutter = false;
        return true;
    }
    if (photo_->time != PhotoState::Time::frozen && photo_->time != PhotoState::Time::slow)
        return false;
    photo_->time = PhotoState::Time::running;
    photo_->slow_speed = 0;
    photo_->shutter = false;
    snap_held_clock();
    return true;
}

bool Runtime::photo_slow(int32_t speed) {
    if (photo_ == nullptr || match_ == nullptr)
        return false;
    const bool local = photo_time_is_local();
    if (!local && !photo_time_is_replay())
        return false;
    if (speed < 1 || speed > 9)
        return false;
    const auto range = game_speed_range();
    const int32_t low = std::max(range.slowest, int32_t{1});
    const int32_t high = std::min(range.fastest, int32_t{9});
    if (low > high)
        return false;
    const int32_t paced = std::clamp(speed, low, high);
    if (photo_->time == PhotoState::Time::slow && photo_->slow_speed == paced)
        return false;
    if (!local) {
        auto& world = match_->state();
        auto& game = world.game;
        // Freeze leaves the replay's pause on. Slow lifts it, as Run does,
        // so the replay plays at the speed chosen.
        if ((game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0) {
            run_console_hotkey(oa::ui::console::hotkey::pause);
            if ((game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0)
                return false;
        }
        const uint16_t before = world.game.requested_speed;
        std::ignore = oa::sim::speed::set_speed(world, paced, message_hooks(), range);
        match_timing_.requested_rate = world.game.requested_speed;
        match_timing_.actual_rate = world.game.current_speed;
        preferences_.current_game_speed = world.game.current_speed;
        if (world.game.requested_speed != before)
            call_hook_or_report<&Extension::speed_changed>(
                extension_, hook_error_report(), *this, world.game.requested_speed
            );
        photo_->time = PhotoState::Time::slow;
        photo_->slow_speed = paced;
        photo_->shutter = false;
        return true;
    }
    photo_->time = PhotoState::Time::slow;
    photo_->slow_speed = paced;
    photo_->slow_interval_ns =
        frame_pacing::kNanosecondsPerSecond / (static_cast<uint64_t>(paced) * 3U);
    photo_->slow_origin_ns = frame_time_ns_;
    photo_->slow_granted = 0;
    photo_->shutter = false;
    snap_held_clock();
    return true;
}

bool Runtime::photo_step() {
    if (photo_ == nullptr || match_ == nullptr || !photo_time_is_local())
        return false;
    const GrantedTicks grant = run_granted_ticks(1, true);
    if (grant.ran == 0)
        return false;
    photo_->shutter = false;
    photo_->time = PhotoState::Time::frozen;
    photo_->slow_speed = 0;
    return true;
}

bool Runtime::photo_between(int32_t eighths) {
    if (photo_ == nullptr || !photo_time_is_local())
        return false;
    if (photo_->time != PhotoState::Time::frozen)
        return false;
    if (eighths < 0 || eighths > 8)
        return false;
    photo_->shutter = true;
    photo_->shutter_eighth = eighths;
    return true;
}

int32_t Runtime::photo_shutter_eighth() const noexcept {
    if (photo_ == nullptr || !photo_->shutter)
        return 8;
    return photo_->shutter_eighth;
}

bool Runtime::photo_hold_picture() {
    if (photo_ == nullptr || match_ == nullptr)
        return false;
    const uint32_t bits = current_extension_state();
    if ((bits & (extension_state::shared_match | extension_state::local_watcher)) == 0)
        return false;
    if (photo_->picture_held) {
        if (photo_->camera_held) {
            match_camera_x_ = photo_->held_camera_x;
            match_camera_z_ = photo_->held_camera_z;
            match_zoom_ = photo_->held_zoom;
            match_zoom_target_ = photo_->held_zoom_target;
            exact_view_ = photo_->held_exact;
            view_hold_ = photo_->held_view;
            match_tracking_ = photo_->held_tracking;
            tracked_match_unit_ = photo_->held_tracked_unit;
            match_->state().game.follow_unit = photo_->held_follow_unit;
            if (photo_->held_live)
                restore_live_camera(
                    photo_->held_live_yaw, photo_->held_live_pitch, photo_->held_live_unlocked
                );
            terrain_cache_cam_x_ = kUncachedTerrainCamera;
            photo_->camera_held = false;
        }
        photo_->picture_held = false;
        photo_->held_rgb.clear();
        photo_->held_width = 0;
        photo_->held_height = 0;
        return true;
    }
    if (!capture_photo_picture())
        return false;
    photo_->held_camera_x = match_camera_x_;
    photo_->held_tick = match_->state().game.tick;
    photo_->held_speed = match_->state().game.current_speed;
    photo_->held_camera_z = match_camera_z_;
    photo_->held_zoom = match_zoom_;
    photo_->held_zoom_target = match_zoom_target_;
    photo_->held_exact = exact_view_;
    photo_->held_view = view_hold_;
    photo_->held_tracking = match_tracking_;
    photo_->held_tracked_unit = tracked_match_unit_;
    photo_->held_follow_unit = match_->state().game.follow_unit;
    photo_->held_live = false;
    if (live_renderer_ != nullptr) {
        photo_->held_live = true;
        const std::array<float, 2> angles = live_camera_angles();
        photo_->held_live_yaw = angles[0];
        photo_->held_live_pitch = angles[1];
        photo_->held_live_unlocked = live_renderer_unlocked();
    }
    photo_->camera_held = true;
    middle_pan_ = {};
    middle_pan_swallow_button_ = 0;
    photo_->picture_held = true;
    return true;
}

void Runtime::grant_photo_slow_ticks() {
    if (photo_ == nullptr || photo_->time != PhotoState::Time::slow || !photo_time_is_local())
        return;
    const uint64_t interval = photo_->slow_interval_ns;
    if (interval == 0 || frame_time_ns_ < photo_->slow_origin_ns)
        return;
    const uint64_t due = (frame_time_ns_ - photo_->slow_origin_ns) / interval;
    if (due <= photo_->slow_granted)
        return;
    const uint64_t behind = due - photo_->slow_granted;
    const uint32_t count =
        static_cast<uint32_t>(behind > most_batch_ticks ? most_batch_ticks : behind);
    const GrantedTicks grant = run_granted_ticks(count, true);
    photo_->slow_granted += grant.ran;
}

void Runtime::apply_photo_fraction(uint32_t& fraction) const {
    if (photo_ == nullptr || !photo_time_is_local())
        return;
    if (photo_->time == PhotoState::Time::frozen && photo_->shutter) {
        const int32_t eighth = std::clamp(photo_->shutter_eighth, int32_t{0}, int32_t{8});
        fraction = static_cast<uint32_t>(eighth) * (whole_tick / 8U);
        return;
    }
    if (photo_->time != PhotoState::Time::slow || !smooth_motion() || photo_->slow_interval_ns == 0)
        return;
    const uint64_t last = photo_->slow_origin_ns + photo_->slow_granted * photo_->slow_interval_ns;
    if (frame_time_ns_ < last)
        return;
    const double since = static_cast<double>(frame_time_ns_ - last);
    float alpha = static_cast<float>(since / static_cast<double>(photo_->slow_interval_ns));
    if (alpha > 1.0F)
        alpha = 1.0F;
    fraction = tick_fraction(alpha);
}

void Runtime::show_held_photo_picture() {
    if (photo_ == nullptr || !photo_->picture_held || photo_->held_rgb.empty())
        return;
    if (photo_->held_width != match_world_cpu_.width ||
        photo_->held_height != match_world_cpu_.height)
        return;
    if (photo_->held_rgb.size() != match_world_cpu_.rgb.size())
        return;
    std::memcpy(match_world_cpu_.rgb.data(), photo_->held_rgb.data(), photo_->held_rgb.size());
}

// Network play's --net-loopback-check runs this in every build, the release
// packages' too, so it is here and not with the self-checks in
// runtime_photo_check.cpp.
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
