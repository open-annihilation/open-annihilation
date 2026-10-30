// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-selection-keys: a skirmish in which the local player has radar,
// sonar and jammer structures, a radar kbot, a fighter, a bomber, a scout
// plane, armed kbots and a construction kbot, and the other player a radar
// tower. Every key goes in as the system sends it: the modifier's own key
// down first, the key down and up with the modifier held, then the
// modifier's key up, each through the event queue a frame at a time, with
// the pointer moving a pixel over the battlefield after each one. Each of
// 3.1c's Ctrl+letter keys is pressed in turn: Ctrl+A every unit that can be
// selected and no empty place in the player's unit list, Ctrl+C the
// commander, Ctrl+D self-destruct on and off again, Ctrl+S the units on
// screen, Ctrl+Z every unit of a selected unit's type, and every other
// letter the local units the loaded data tags CTRL_<letter>, the whole map
// over, and none of the other player's; a letter no unit is tagged with
// leaves the selection as it was. Ctrl+R draws the radar and sonar rings on
// the minimap, Ctrl+Shift+R and Ctrl+Shift+P add to the selection, Ctrl+1
// stores a group and its select key recalls it, and Primary+/ (Command+/ on
// a Mac) opens the keys overlay and closes it again. Group keys centre the
// view is played each way. The view is put at the clamped place furthest
// from the group, and following a unit is stopped so a frame does not pull
// it back: On a double press, one press selects and leaves the view, two
// quick presses centre the group and two slow ones do not; Never leaves it
// after two quick presses; On every press leaves the view on the first
// press and centres it on a later press, past the double-press time. Each
// scenario waits out the double-press time first, so the press before it
// is not its first. The check reads
// its expectations from the loaded data, so it runs under a mod's profile
// too, and it puts the setting back as it found it.

#include "oa/app/runtime.hpp"
#include "oa/app/frame_coordinates.hpp"

#include "device_state.hpp"
#include "engine_settings_state.hpp"
#include "oa_layer.hpp"

#include "oa/core/unit.h"
#include "oa/data/defs/categories.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/present/world_renderer/world_radar.hpp"
#include "oa/sim/simulation_state.hpp"
#include "oa/sim/unit_spawn/spawn.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/key_editor/overlay.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace editor = oa::ui::key_editor;
namespace wr = oa::present::world_renderer;

/// A modifier key as the system sends it: its own key, and its bit in the
/// modifier word of every key event while it is held.
struct ModifierKey {
    SDL_Scancode scancode{}; ///< the key's place
    SDL_Keycode key{};       ///< the key code its own events carry
    SDL_Keymod bit{};        ///< its bit in the modifier word
};

/// The left Ctrl key.
constexpr ModifierKey kControl{SDL_SCANCODE_LCTRL, SDLK_LCTRL, SDL_KMOD_LCTRL};
/// The left Shift key.
constexpr ModifierKey kShift{SDL_SCANCODE_LSHIFT, SDLK_LSHIFT, SDL_KMOD_LSHIFT};
/// The left Alt key (Option on a Mac).
constexpr ModifierKey kAlt{SDL_SCANCODE_LALT, SDLK_LALT, SDL_KMOD_LALT};

/// Primary on this system: Command on a Mac, Ctrl elsewhere.
#ifdef SDL_PLATFORM_MACOS
constexpr ModifierKey kPrimary{SDL_SCANCODE_LGUI, SDLK_LGUI, SDL_KMOD_LGUI};
#else
constexpr ModifierKey kPrimary = kControl;
#endif

/// The stock units planted for the local player, each beside the commander:
/// a radar tower, a sonar station and a jammer (tagged CTRL_R), a radar kbot
/// (CTRL_R), a fighter and a bomber (CTRL_P, CTRL_V, CTRL_W), a scout plane
/// (CTRL_V only), two armed kbots (CTRL_W) and a construction kbot (CTRL_B).
/// A type the loaded data lacks is left out.
constexpr const char* kPlanted[] = {
    "ARMRAD",
    "ARMSONAR",
    "ARMJAM",
    "ARMMARK",
    "ARMFIG",
    "ARMTHUND",
    "ARMPEEP",
    "ARMPW",
    "ARMPW",
    "ARMCK",
};

/// The other player's radar tower, which no key of the local player selects.
constexpr const char* kEnemyRadar = "CORRAD";

/// Map pixels between two planted units in a row.
constexpr int32_t kPlantSpacing = 48;

/// Map pixels from the commander to the first planted unit.
constexpr int32_t kPlantOffset = 96;

/// Planted units in a row before the next row starts.
constexpr int32_t kPlantRow = 5;

/// Map pixels from the commander to the other player's tower.
constexpr int32_t kEnemyOffset = 160;

/// Frames run with the clock going after the units are planted, so that a
/// tower's script turns its radar on, as it does once it is built.
constexpr int32_t kWarmFrames = 60;

/// The frame clock's start, in nanoseconds.
constexpr uint64_t kClockStartNs = 1'000'000'000ULL;

/// One frame's step of the frame clock, in nanoseconds.
constexpr uint64_t kFrameNs = 33'333'333ULL;

/// The double-press time the key bindings take while Settings leaves it to
/// the system, in milliseconds.
constexpr uint32_t kGroupDoublePressMs = 500;

/// How far past the double-press time the second of two slow presses comes,
/// in milliseconds.
constexpr uint32_t kGroupSlowMarginMs = 250;

/// Pixels of the sensor colour the rings must add to the minimap.
constexpr std::size_t kRingPixels = 16;

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("selection keys: " + std::string(what));
}

/// Names one step on standard output.
///
/// @param what the step
void say_step(std::string_view what) {
    std::cout << "selection keys: " << what << std::endl;
}

/// The ids as text, for a failure.
///
/// @param ids unit ids
/// @return the ids joined with commas
std::string id_text(const std::vector<uint16_t>& ids) {
    std::string text;
    for (const uint16_t id : ids) {
        if (!text.empty())
            text += ',';
        text += std::to_string(id);
    }
    return text.empty() ? std::string("none") : text;
}

} // namespace

void Runtime::check_selection_keys() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    start_benchmark_skirmish();
    require(screen_ == Screen::match && match_ != nullptr, "the skirmish did not start");
    apply_output_mode();

    bool running = true;
    const SDL_WindowID window_id = SDL_GetWindowID(sdl_.window);
    frame_run_clock_ns_ = kClockStartNs;
    frame_time_ns_ = kClockStartNs;
    scroll_clock_ = kClockStartNs;
    frame_run_scroll_ = 0;
    const auto next_frame = [&] {
        frame_run_clock_ns_ = *frame_run_clock_ns_ + kFrameNs;
        run_frame(running);
        require(running, "a frame ended the run");
        scroll_clock_ = frame_time_ns_;
    };

    auto& world = match_->state();
    const uint8_t seat = match_local_player_;
    const auto other = static_cast<uint8_t>(seat == 0 ? 1 : 0);
    uint16_t commander = 0;
    for (const auto& slot : match_->world().slots) {
        if (slot.unit == nullptr || slot.unit_index == 0 || slot.record.owner_index != seat)
            continue;
        if (const auto* def = definition_for(slot.unit_index); def != nullptr && def->can_dgun) {
            commander = slot.unit_index;
            break;
        }
    }
    require(commander != 0, "found no local commander");

    // A finished unit on the ground at a map pixel beside the commander.
    const auto plant = [&](std::string_view name, uint8_t player, int32_t dx, int32_t dz) {
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (type == 0)
            return uint16_t{0};
        const auto& home = *match_->world().slots[commander].unit;
        const auto x = static_cast<int32_t>(home.position[0] >> 16) + dx;
        const auto z = static_cast<int32_t>(home.position[2] >> 16) + dz;
        oa::sim::unit_spawn::Request request;
        request.player = player;
        request.type = type;
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>(x) << 16,
            static_cast<uint32_t>(
                match_->map_height(static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16)
            ) << 16,
            static_cast<uint32_t>(z) << 16
        };
        auto* placed = match_->create(request);
        require(
            placed != nullptr && placed->unit != nullptr, "could not plant " + std::string(name)
        );
        placed->unit->object_present = true;
        placed->unit->flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
        return placed->unit_index;
    };
    std::vector<uint16_t> planted;
    int32_t planted_count = 0;
    for (const char* name : kPlanted) {
        const int32_t dx = kPlantOffset + (planted_count % kPlantRow) * kPlantSpacing;
        const int32_t dz = (planted_count / kPlantRow) * kPlantSpacing - kPlantSpacing;
        if (const uint16_t id = plant(name, seat, dx, dz); id != 0)
            planted.push_back(id);
        ++planted_count;
    }
    const uint16_t enemy_radar = plant(kEnemyRadar, other, -kEnemyOffset, 0);
    // A planted unit stays where it was put, and a radar comes on as its
    // script runs.
    for (int32_t step = 0; step < kWarmFrames; ++step)
        next_frame();
    match_tick_blocked_ = true;
    center_camera_on_unit(commander);
    next_frame();
    say_step(
        "planted " + std::to_string(planted.size()) + " units for the local player" +
        (enemy_radar != 0 ? " and a radar tower for the other" : "")
    );

    // The pointer moved a pixel over the battlefield, as a hand on the mouse
    // moves it between a key's events. The move answers no key again.
    int32_t nudge = 0;
    const auto queue_motion = [&] {
        ++nudge;
        const int32_t side = nudge % 2;
        float window_x = 0;
        float window_y = 0;
        require(
            frame_to_window(
                sdl_.renderer,
                static_cast<float>(
                    match_layout_.battlefield_x() + match_layout_.battlefield_width() / 2 + side
                ),
                static_cast<float>(
                    match_layout_.battlefield_y() + match_layout_.battlefield_height() / 2
                ),
                &window_x,
                &window_y
            ),
            "the battlefield did not map into the window"
        );
        SDL_Event motion{};
        motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.windowID = window_id;
        motion.motion.x = window_x;
        motion.motion.y = window_y;
        motion.motion.xrel = side == 0 ? -1.0F : 1.0F;
        require(
            SDL_PushEvent(&motion), std::string("the motion was not queued: ") + SDL_GetError()
        );
    };
    // The keys as the system sends them, each event through the queue and a
    // frame, as the player's own keys arrive.
    SDL_Keymod held = SDL_KMOD_NONE;
    const auto send = [&](bool down, SDL_Scancode scancode, SDL_Keycode key) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.windowID = window_id;
        event.key.scancode = scancode;
        event.key.key = key;
        event.key.mod = held;
        event.key.down = down;
        require(SDL_PushEvent(&event), std::string("the key was not queued: ") + SDL_GetError());
        queue_motion();
        next_frame();
    };
    const auto press = [&](std::initializer_list<ModifierKey> modifiers, SDL_Scancode scancode) {
        for (const ModifierKey& modifier : modifiers) {
            held = static_cast<SDL_Keymod>(held | modifier.bit);
            SDL_SetModState(held);
            device_state::hold_key(modifier.scancode, true);
            send(true, modifier.scancode, modifier.key);
        }
        // The key code a key event carries: the key's own, with no modifier
        // applied, whatever is held.
        const SDL_Keycode key = SDL_GetKeyFromScancode(scancode, held, true);
        device_state::hold_key(scancode, true);
        send(true, scancode, key);
        device_state::hold_key(scancode, false);
        send(false, scancode, key);
        for (auto modifier = std::rbegin(modifiers); modifier != std::rend(modifiers); ++modifier) {
            held = static_cast<SDL_Keymod>(held & ~modifier->bit);
            SDL_SetModState(held);
            device_state::hold_key(modifier->scancode, false);
            send(false, modifier->scancode, modifier->key);
        }
    };
    const auto letter_place = [](char letter) {
        return static_cast<SDL_Scancode>(static_cast<int>(SDL_SCANCODE_A) + (letter - 'a'));
    };

    // The local units that can be selected, the ones Ctrl+A takes, and which
    // are selected now.
    const auto local_units = [&] {
        std::vector<uint16_t> ids;
        for (const auto& slot : match_->world().slots)
            if (slot.unit != nullptr && slot.unit_index != 0 && slot.record.owner_index == seat &&
                oa::sim::simulation_state::unit_selectable(world, slot.record))
                ids.push_back(slot.unit_index);
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    const auto selected = [&] {
        std::vector<uint16_t> ids;
        for (const auto& slot : match_->world().slots)
            if (slot.unit != nullptr && slot.unit_index != 0 &&
                (slot.record.flags & OA_UNIT_FLAG_SELECTED) != 0)
                ids.push_back(slot.unit_index);
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    // The local units a Category word names, as 3.1c reads the tag.
    const auto tagged = [&](std::string_view word) {
        std::vector<uint16_t> ids;
        const std::string name{word};
        const oa::data::defs::CategoryMask* mask =
            oa::data::defs::category_registry_find(&unit_table_.tables.categories, name.c_str());
        if (mask == nullptr)
            return ids;
        for (const auto& slot : match_->world().slots) {
            if (slot.unit == nullptr || slot.unit_index == 0 || slot.record.owner_index != seat)
                continue;
            if (!oa::sim::simulation_state::unit_selectable(world, slot.record))
                continue;
            if (oa::data::defs::category_mask_contains(mask, slot.record.type_index))
                ids.push_back(slot.unit_index);
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    const auto select_only = [&](std::vector<uint16_t> ids) {
        clear_local_selection();
        for (const uint16_t id : ids)
            adopt_selection(id);
        selected_match_unit_ = ids.empty() ? uint16_t{0} : ids.front();
        apply_match_hud_for_selection();
        next_frame();
    };
    const auto first_of = [&](std::string_view name) {
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        for (const uint16_t id : planted)
            if (match_->world().slots[id].record.type_index == type)
                return id;
        return uint16_t{0};
    };
    const auto unite = [](std::vector<uint16_t> left, const std::vector<uint16_t>& right) {
        left.insert(left.end(), right.begin(), right.end());
        std::sort(left.begin(), left.end());
        left.erase(std::unique(left.begin(), left.end()), left.end());
        return left;
    };
    const auto expect = [&](std::string_view what, const std::vector<uint16_t>& wanted) {
        const std::vector<uint16_t> got = selected();
        require(
            got == wanted,
            std::string(what) + " selected " + id_text(got) + ", wanted " + id_text(wanted)
        );
        if (enemy_radar != 0)
            require(
                std::find(got.begin(), got.end(), enemy_radar) == got.end(),
                std::string(what) + " selected the other player's radar tower"
            );
    };

    const uint16_t kbot = first_of("ARMPW");
    const std::vector<uint16_t> all_local = local_units();
    for (char letter = 'a'; letter <= 'z'; ++letter) {
        const std::string chord = std::string("Ctrl+") + static_cast<char>(letter - 'a' + 'A');
        if (letter == 'a') {
            select_only({});
            press({kControl}, letter_place(letter));
            expect(chord, all_local);
            say_step(chord + " selects every unit (" + std::to_string(all_local.size()) + ")");
            continue;
        }
        if (letter == 'c') {
            select_only({});
            press({kControl}, letter_place(letter));
            expect(chord, {commander});
            say_step(chord + " selects the commander");
            continue;
        }
        if (letter == 'd') {
            if (kbot == 0)
                continue;
            select_only({kbot});
            press({kControl}, letter_place(letter));
            require(
                match_->self_destruct_remaining(kbot) > 0, chord + " did not start self-destruct"
            );
            press({kControl}, letter_place(letter));
            require(match_->self_destruct_remaining(kbot) == 0, chord + " again did not stop it");
            say_step(chord + " starts self-destruct and stops it again");
            continue;
        }
        if (letter == 's') {
            select_only({});
            press({kControl}, letter_place(letter));
            const std::vector<uint16_t> got = selected();
            require(
                std::find(got.begin(), got.end(), commander) != got.end(),
                chord + " did not select the commander in view"
            );
            for (const uint16_t id : got)
                require(
                    std::find(all_local.begin(), all_local.end(), id) != all_local.end(),
                    chord + " selected a unit that is not the player's"
                );
            say_step(chord + " selects the units on screen (" + std::to_string(got.size()) + ")");
            continue;
        }
        if (letter == 'z') {
            if (kbot == 0)
                continue;
            select_only({kbot});
            press({kControl}, letter_place(letter));
            const auto type = match_->world().slots[kbot].record.type_index;
            std::vector<uint16_t> same;
            for (const uint16_t id : all_local)
                if (match_->world().slots[id].record.type_index == type)
                    same.push_back(id);
            expect(chord, same);
            say_step(chord + " selects every unit of the selected unit's type");
            continue;
        }
        const std::string word = std::string("CTRL_") + static_cast<char>(letter - 'a' + 'A');
        const std::vector<uint16_t> wanted = tagged(word);
        if (wanted.empty()) {
            // Nothing is tagged: the selection stays as it was.
            select_only({commander});
            press({kControl}, letter_place(letter));
            expect(chord + " with nothing tagged " + word, {commander});
            continue;
        }
        select_only({});
        press({kControl}, letter_place(letter));
        expect(chord, wanted);
        say_step(chord + " selects the " + std::to_string(wanted.size()) + " units tagged " + word);
    }

    say_step("Ctrl+Shift+R and Ctrl+Shift+P add to the selection");
    for (const char letter : {'r', 'p'}) {
        const std::string word = std::string("CTRL_") + static_cast<char>(letter - 'a' + 'A');
        const std::vector<uint16_t> wanted = tagged(word);
        if (wanted.empty() || kbot == 0)
            continue;
        select_only({kbot});
        press({kControl, kShift}, letter_place(letter));
        expect(
            std::string("Ctrl+Shift+") + static_cast<char>(letter - 'a' + 'A'),
            unite(wanted, {kbot})
        );
    }

    // Ctrl+R shows the radar and sonar rings of what it selected on the
    // minimap, as every selected sensor does.
    if (const std::vector<uint16_t> sensors = tagged("CTRL_R"); !sensors.empty()) {
        const auto ring_pixels = [&] {
            compose_radar_final();
            const ::oa::Surface* final_image = radar_state_.surfaces.final_image;
            require(final_image != nullptr, "the match has no minimap image");
            const uint8_t ink = wr::game_ui_color(world.game, wr::ui_color_sensor_range);
            std::size_t count = 0;
            for (int32_t y = 0; y < final_image->height; ++y) {
                const uint8_t* line =
                    final_image->pixels + static_cast<std::ptrdiff_t>(y) * final_image->pitch;
                count += static_cast<std::size_t>(std::count(line, line + final_image->width, ink));
            }
            return count;
        };
        select_only({});
        const std::size_t before = ring_pixels();
        press({kControl}, letter_place('r'));
        expect("Ctrl+R", sensors);
        const std::size_t after = ring_pixels();
        require(
            after >= before + kRingPixels,
            "Ctrl+R drew no rings on the minimap (" + std::to_string(before) +
                " sensor pixels, then " + std::to_string(after) + ")"
        );
        say_step(
            "Ctrl+R draws the rings on the minimap: " + std::to_string(after - before) +
            " pixels of the sensor colour"
        );
    }

    say_step("Ctrl+1 stores a group, and its select key brings it back");
    if (const uint16_t fighter = first_of("ARMFIG"); fighter != 0) {
        // The group's key: 1 alone with Select groups without Alt, else Alt+1.
        const auto group_key = [&] {
            if (EngineSettingsState::live_switch_alt(*this))
                press({}, SDL_SCANCODE_1);
            else
                press({kAlt}, SDL_SCANCODE_1);
        };
        select_only({fighter});
        press({kControl}, SDL_SCANCODE_1);
        select_only({});
        group_key();
        expect("the group 1 key", {fighter});

        // Group keys centre the view, each way. The view starts as far from
        // the group as the camera's limits allow, and a press either leaves
        // it there or moves it to the group.
        namespace settings = oa::ui::engine_settings;
        const settings::GroupKeysCentre kept = engine_settings().group_keys_centre;
        const auto choose = [&](settings::GroupKeysCentre way) {
            settings::EngineSettings chosen = engine_settings();
            chosen.group_keys_centre = way;
            apply_engine_settings(chosen);
        };
        const auto& flier = match_->world().slots[fighter].record;
        const int32_t group_x = static_cast<int16_t>(static_cast<uint32_t>(flier.position.x) >> 16);
        const int32_t group_z = static_cast<int16_t>(static_cast<uint32_t>(flier.position.z) >> 16);
        // A frame follows the commander while the game says so, which pulls a
        // camera put at the map's edge back beside the group. Following is
        // stopped so a parked view stays where it was put.
        stop_match_tracking();
        set_camera_position(
            group_x - visible_map_width() / 2, group_z - visible_map_height() / 2, 0
        );
        const std::pair<int32_t, int32_t> centred{match_camera_x_, match_camera_z_};
        // The map's pixel size is not the camera's range: set_camera_position
        // clamps, and the corner on the group's side of the map lands beside
        // the group. Keep the request whose clamped place is furthest away.
        const auto shown = shown_map_size();
        const std::array<std::pair<int32_t, int32_t>, 8> requests{{
            {-100000, -100000},
            {-100000, 100000},
            {100000, -100000},
            {100000, 100000},
            {0, 0},
            {shown[0], shown[1]},
            {0, shown[1]},
            {shown[0], 0},
        }};
        std::pair<int32_t, int32_t> far_request = requests[0];
        int64_t furthest = -1;
        for (const auto& request : requests) {
            set_camera_position(request.first, request.second, 0);
            const int64_t across = static_cast<int64_t>(match_camera_x_) - centred.first;
            const int64_t down = static_cast<int64_t>(match_camera_z_) - centred.second;
            const int64_t span = across * across + down * down;
            if (span > furthest) {
                furthest = span;
                far_request = request;
            }
        }
        const auto where = [](std::pair<int32_t, int32_t> place) {
            return std::to_string(place.first) + "," + std::to_string(place.second);
        };
        // Requires the view at a place, saying where it is otherwise.
        const auto require_view = [&](std::pair<int32_t, int32_t> wanted, std::string_view what) {
            const std::pair<int32_t, int32_t> now{match_camera_x_, match_camera_z_};
            require(
                now == wanted,
                std::string(what) + ": the view is at " + where(now) + ", wanted " + where(wanted) +
                    " (the group's view " + where(centred) + ")"
            );
        };
        // The same window the key bindings keep: 500 ms while Double press is
        // left to the system, otherwise the milliseconds the setting stores.
        const uint32_t stored_ms = engine_settings().double_press_ms;
        const uint32_t double_press_ms = stored_ms == 0 ? kGroupDoublePressMs : stored_ms;
        // Puts the view at the far place, clears the selection, and waits out
        // the double-press time so the group's key pressed just before is not
        // this scenario's first press. Returns where the view then is.
        const auto park = [&] {
            stop_match_tracking();
            set_camera_position(far_request.first, far_request.second, 0);
            select_only({});
            SDL_Delay(double_press_ms + kGroupSlowMarginMs);
            const std::pair<int32_t, int32_t> parked{match_camera_x_, match_camera_z_};
            require(
                parked != centred,
                "the view far from the group is where the group is (" + where(parked) + ")"
            );
            return parked;
        };

        choose(settings::GroupKeysCentre::double_press);
        const auto first_park = park();
        group_key();
        expect("one press of the group key", {fighter});
        require_view(first_park, "one press of the group key");
        say_step("On a double press: one press selects the group and leaves the view");
        group_key();
        require_view(centred, "two quick presses of the group key");
        say_step("On a double press: two quick presses centre the view on the group");
        const auto slow_park = park();
        group_key();
        SDL_Delay(double_press_ms + kGroupSlowMarginMs);
        group_key();
        expect("two slow presses of the group key", {fighter});
        require_view(slow_park, "two slow presses of the group key");
        say_step("On a double press: two slow presses leave the view");

        choose(settings::GroupKeysCentre::never);
        const auto never_park = park();
        group_key();
        group_key();
        expect("the group key under Never", {fighter});
        require_view(never_park, "Never, two quick presses");
        say_step("Never: two quick presses leave the view");

        choose(settings::GroupKeysCentre::every_press);
        const auto every_park = park();
        group_key();
        expect("the group key under On every press", {fighter});
        require_view(every_park, "On every press, the first press");
        say_step("On every press: one press selects the group and leaves the view");
        // Past the double-press time, so this is not On a double press's
        // second press: the group is already the selection, and the view moves.
        SDL_Delay(double_press_ms + kGroupSlowMarginMs);
        group_key();
        require_view(centred, "On every press, a later press");
        say_step("On every press: a later press centres the view, past the double-press time");
        choose(kept);
    }

    say_step("Primary+/ opens the keys overlay, and again closes it");
    select_only({commander});
    const auto overlay_open = [this] {
        return oa_layer().find(editor::name_keys_overlay) != nullptr;
    };
    require(!overlay_open(), "the keys overlay was open before Primary+/");
    press({kPrimary}, SDL_SCANCODE_SLASH);
    require(overlay_open(), "Primary+/ did not open the keys overlay");
    press({kPrimary}, SDL_SCANCODE_SLASH);
    require(!overlay_open(), "Primary+/ again did not close the keys overlay");
    say_step("done");
}

} // namespace oa::app
