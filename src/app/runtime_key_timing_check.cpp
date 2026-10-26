// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Slow keys, bounce keys, the double-press time, the player's repeat and the
// drag threshold, on one skirmish. The drag's pixel limit is the setting and
// its tick limit widens in proportion, so one number widens both.
#include "oa/app/runtime.hpp"

#include "engine_settings_state.hpp"
#include "touch_state.hpp"

#include "oa/ui/key_bindings/dispatcher.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace oa::app {
namespace {

namespace settings = oa::ui::engine_settings;
namespace keys = oa::ui::key_bindings;

[[noreturn]] void fail(std::string_view what) {
    throw std::runtime_error("key timing: " + std::string(what));
}

void require(bool ok, std::string_view what) {
    if (!ok)
        fail(what);
}

void report_step(std::string_view name) {
    std::cout << "key timing: " << name << '\n';
}

/// A canvas point.
struct Point {
    float x{};
    float y{};
};

/// One drag whose map travel is between 40 and 63 pixels on one axis.
struct Drag {
    Point from{};
    Point to{};
    int32_t moved_x{};
    int32_t moved_z{};
    bool release_hits{};
    uint16_t unit{};
};

} // namespace

void Runtime::check_key_timing() {
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        fail("needs the SDL renderer");
    options_.trace_input = true;
    const auto saved_preferences = preference_values_;
    const settings::EngineSettings saved_settings = engine_settings_state().current;

    struct Restore {
        Runtime& self;
        settings::EngineSettings settings;
        decltype(preference_values_) preferences;

        ~Restore() {
            if (self.touch_state_if_made() != nullptr) {
                self.touch_state().dispatch.forced = false;
                self.touch_state().dispatch.accept_unregistered_touch = false;
            }
            self.apply_engine_settings(settings);
            self.preference_values_ = std::move(preferences);
            self.preferences_dirty_ = true;
            self.flush_preferences();
        }
    } restore{*this, saved_settings, saved_preferences};

    start_benchmark_skirmish();
    apply_output_mode();
    bool running = true;
    for (int frame = 0; frame < 4; ++frame)
        run_frame(running);

    uint16_t commander = 0;
    for (const auto& slot : match_->world().slots) {
        if (slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_) {
            commander = slot.unit_index;
            break;
        }
    }
    require(commander != 0, "found no local commander");

    const auto window_id = SDL_GetWindowID(sdl_.window);
    const auto move_canvas = [&](Point point) {
        float window_x = 0;
        float window_y = 0;
        if (!frame_to_window(sdl_.renderer, point.x, point.y, &window_x, &window_y))
            fail(SDL_GetError());
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.windowID = window_id;
        event.motion.x = window_x;
        event.motion.y = window_y;
        dispatch_event(event, running);
    };
    const auto button = [&](Point point, bool down) {
        float window_x = 0;
        float window_y = 0;
        if (!frame_to_window(sdl_.renderer, point.x, point.y, &window_x, &window_y))
            fail(SDL_GetError());
        SDL_Event event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.windowID = window_id;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = down;
        event.button.clicks = 1;
        event.button.x = window_x;
        event.button.y = window_y;
        dispatch_event(event, running);
    };
    const auto key_event = [&](SDL_Scancode scancode, SDL_Keycode code, bool down) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.windowID = window_id;
        event.key.scancode = scancode;
        event.key.key = code;
        event.key.down = down;
        event.key.repeat = false;
        event.key.mod = SDL_KMOD_NONE;
        dispatch_event(event, running);
    };
    const auto count_phase = [&](std::string_view action, keys::Phase phase) {
        return recent_action_phase_count(action, phase);
    };
    const auto unit_selected = [&](uint16_t id) {
        if (id == 0 || id >= match_->world().slots.size())
            return false;
        const auto* unit = match_->world().slots[id].unit;
        return unit != nullptr && (unit->flags & OA_UNIT_FLAG_SELECTED) != 0;
    };
    const auto spawn = [&](std::string_view name, int32_t dx, int32_t dz) -> uint16_t {
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (type == 0)
            return 0;
        const auto* home = match_->world().slots[commander].unit;
        if (home == nullptr)
            return 0;
        const auto x = static_cast<int32_t>(home->position[0] >> 16) + dx;
        const auto z = static_cast<int32_t>(home->position[2] >> 16) + dz;
        if (x < 32 || z < 32)
            return 0;
        oa::sim::unit_spawn::Request request;
        request.player = match_local_player_;
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
        auto* slot = match_->create(request);
        if (slot == nullptr || slot->unit == nullptr)
            return 0;
        slot->unit->object_present = true;
        return slot->unit_index;
    };

    const auto apply_access = [&](const settings::EngineSettings& chosen) {
        apply_engine_settings(chosen);
    };

    // A building large enough that a 40-pixel drag can end still on it.
    uint16_t building = 0;
    for (const std::string_view name :
         {"ARMFUS", "CORFUS", "ARMGANT", "CORGANT", "ARMCKFUS", "ARMAP", "ARMLAB", "ARMVP"}) {
        building = spawn(name, 192, 64);
        if (building != 0)
            break;
    }
    require(building != 0, "could not spawn a building");
    center_camera_on_unit(building);
    for (int frame = 0; frame < 3; ++frame)
        run_frame(running);
    clear_local_selection();

    const auto* built = match_->world().slots[building].unit;
    require(built != nullptr, "the building has no unit");
    const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
    const auto projected = project_match_point(
        viewport,
        {static_cast<uint32_t>(built->position[0]),
         static_cast<uint32_t>(built->position[1]),
         static_cast<uint32_t>(built->position[2])}
    );
    const Point origin{static_cast<float>(projected.x), static_cast<float>(projected.y)};
    const auto ground_of = [&](Point point) { return match_pointer_ground(point.x, point.y); };
    const auto origin_ground = ground_of(origin);
    require(origin_ground.has_value(), "the building's point is not on the ground");

    Drag drag;
    drag.unit = building;
    bool found = false;
    const Point steps[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const Point step_by : steps) {
        for (int distance = 4; distance <= 96; distance += 2) {
            const Point end{
                origin.x + step_by.x * static_cast<float>(distance),
                origin.y + step_by.y * static_cast<float>(distance)
            };
            const auto ground = ground_of(end);
            if (!ground)
                continue;
            const int32_t moved_x = std::abs((*ground)[0] - (*origin_ground)[0]);
            const int32_t moved_z = std::abs((*ground)[2] - (*origin_ground)[2]);
            const int32_t further = moved_x > moved_z ? moved_x : moved_z;
            if (further < 40 || further >= 64)
                continue;
            move_canvas(end);
            const bool hits = hovered_match_unit_ == building;
            drag.from = origin;
            drag.to = end;
            drag.moved_x = moved_x;
            drag.moved_z = moved_z;
            drag.release_hits = hits;
            found = true;
            if (hits)
                break;
        }
        if (found && drag.release_hits)
            break;
    }
    require(found, "found no drag of 40 to 63 map pixels");

    const auto play_drag = [&](uint32_t threshold) {
        settings::EngineSettings chosen = engine_settings_state().current;
        chosen.drag_threshold = threshold;
        chosen.slow_keys_ms = 0;
        chosen.bounce_keys_ms = 0;
        apply_access(chosen);
        clear_local_selection();
        status_ = "ready";
        match_command_ = MatchCommand::none;
        move_canvas(drag.from);
        button(drag.from, true);
        move_canvas(drag.to);
        require(match_drag_.has_value(), "the press did not start a drag");
    };

    report_step("threshold 64, a left drag of 40 map pixels");
    play_drag(64);
    require(match_drag_is_click(), "a drag under 64 pixels was not a click");
    button(drag.to, false);
    if (drag.release_hits) {
        require(unit_selected(building), "the click did not select the unit under the press");
        require(
            status_.find(" selected") == std::string::npos,
            "the click reported a box (" + status_ + ")"
        );
    } else {
        require(
            status_.find(" selected") == std::string::npos && status_ != "No units",
            "the click was reported as a box (" + status_ + ")"
        );
    }

    report_step("threshold 32, the same drag selects a box");
    play_drag(32);
    require(!match_drag_is_click(), "a drag past 32 pixels was still a click");
    button(drag.to, false);
    require(
        status_ == "No units" || status_.ends_with(" selected"),
        "the box was not reported (" + status_ + ")"
    );
    require(unit_selected(building), "the box did not select the unit under the press");

    report_step("threshold 96, a touch drag of 40 points is still a box");
    {
        settings::EngineSettings chosen = engine_settings_state().current;
        chosen.drag_threshold = 96;
        chosen.touch_drag = settings::TouchDrag::box;
        apply_access(chosen);
        auto& touch = touch_state().dispatch;
        touch.forced = true;
        touch.accept_unregistered_touch = true;
        clear_local_selection();
        status_ = "ready";
        // The widened tick limit is 75. A drag is a box only when the press is
        // backdated by that many ticks, which needs the clock to have reached
        // them. The fake clock puts the release on the far side of that window.
        fake_frontend_tick_ = 200;
        const float points = 40.0F * std::max(touch_px_per_point(), 0.01F);
        const Point from = drag.from;
        const Point to{from.x + points, from.y};
        int width = 0;
        int height = 0;
        require(
            SDL_GetWindowSize(sdl_.window, &width, &height) && width > 0 && height > 0,
            "the window has no size"
        );
        const auto finger = [&](uint32_t type, Point point, uint64_t stamp) {
            float window_x = 0;
            float window_y = 0;
            if (!frame_to_window(sdl_.renderer, point.x, point.y, &window_x, &window_y))
                fail(SDL_GetError());
            SDL_Event event{};
            event.type = type;
            event.tfinger.timestamp = stamp;
            event.tfinger.touchID = 1;
            event.tfinger.fingerID = 1;
            event.tfinger.x = window_x / static_cast<float>(width);
            event.tfinger.y = window_y / static_cast<float>(height);
            event.tfinger.pressure = type == SDL_EVENT_FINGER_UP ? 0.0F : 1.0F;
            event.tfinger.windowID = window_id;
            dispatch_event(event, running);
        };
        const uint64_t stamp = SDL_GetTicksNS();
        finger(SDL_EVENT_FINGER_DOWN, from, stamp);
        finger(SDL_EVENT_FINGER_MOTION, to, stamp + 1000000);
        finger(SDL_EVENT_FINGER_UP, to, stamp + 2000000);
        require(
            status_ == "No units" || status_.ends_with(" selected"),
            "the touch drag was not a box (" + status_ + ")"
        );
        fake_frontend_tick_.reset();
    }

    const auto press_for = [&](SDL_Scancode scancode, SDL_Keycode code, uint32_t wait_ms, bool up) {
        const uint64_t began = SDL_GetTicks();
        key_event(scancode, code, true);
        while (SDL_GetTicks() - began < wait_ms)
            SDL_Delay(1);
        const uint64_t elapsed = SDL_GetTicks() - began;
        if (up)
            key_event(scancode, code, false);
        return elapsed;
    };

    report_step("slow keys 300");
    {
        settings::EngineSettings chosen = engine_settings_state().current;
        chosen.slow_keys_ms = 300;
        chosen.bounce_keys_ms = 0;
        chosen.repeat_delay_ms = 0;
        chosen.repeat_rate = 0;
        chosen.touch_drag = settings::TouchDrag::automatic;
        apply_access(chosen);
        select_local_commander();
        apply_match_hud_for_selection();
        for (int frame = 0; frame < 2; ++frame)
            run_frame(running);
        match_command_ = MatchCommand::none;
        const uint64_t brief = press_for(SDL_SCANCODE_A, SDLK_A, 100, true);
        require(brief < 300, "the 100 ms press was not measured before 300 ms");
        advance_input_actions(running);
        require(match_command_ != MatchCommand::attack, "a 100 ms press armed Attack");
        match_command_ = MatchCommand::none;
        const uint64_t held = press_for(SDL_SCANCODE_A, SDLK_A, 400, false);
        require(held >= 300 && held < 1100, "the 400 ms press was not held into its window");
        advance_input_actions(running);
        require(match_command_ == MatchCommand::attack, "a 400 ms press did not arm Attack");
        key_event(SDL_SCANCODE_A, SDLK_A, false);
        match_command_ = MatchCommand::none;
    }

    report_step("bounce keys 300");
    {
        settings::EngineSettings chosen = engine_settings_state().current;
        chosen.slow_keys_ms = 0;
        chosen.bounce_keys_ms = 300;
        apply_access(chosen);
        const uint64_t first = SDL_GetTicks();
        key_event(SDL_SCANCODE_S, SDLK_S, true);
        require(
            count_phase("order.stop", keys::Phase::pressed) == 1,
            "the first Stop was not an action press"
        );
        key_event(SDL_SCANCODE_S, SDLK_S, false);
        while (SDL_GetTicks() - first < 150)
            SDL_Delay(1);
        const uint64_t gap = SDL_GetTicks() - first;
        require(gap < 300, "the second Stop was not inside 300 ms");
        key_event(SDL_SCANCODE_S, SDLK_S, true);
        require(
            count_phase("order.stop", keys::Phase::pressed) == 0,
            "a second Stop inside the bounce window was kept"
        );
        key_event(SDL_SCANCODE_S, SDLK_S, false);
    }

    report_step("repeat delay 200 and rate 10");
    {
        settings::EngineSettings chosen = engine_settings_state().current;
        chosen.bounce_keys_ms = 0;
        chosen.slow_keys_ms = 0;
        chosen.repeat_delay_ms = 200;
        chosen.repeat_rate = 10;
        apply_access(chosen);
        const uint64_t began = SDL_GetTicks();
        key_event(SDL_SCANCODE_RIGHT, SDLK_RIGHT, true);
        while (SDL_GetTicks() - began < 1000)
            SDL_Delay(1);
        const uint64_t elapsed = SDL_GetTicks() - began;
        require(elapsed < 1100, "the clock passed 1100 ms before the repeat advance");
        advance_input_actions(running);
        const int repeats = count_phase("camera.pan-right", keys::Phase::repeated);
        require(repeats == 9, "Right held 1 s fired " + std::to_string(repeats) + " repeats");
        key_event(SDL_SCANCODE_RIGHT, SDLK_RIGHT, false);
    }

    report_step("double press 600");
    {
        settings::EngineSettings chosen = engine_settings_state().current;
        chosen.repeat_delay_ms = 0;
        chosen.repeat_rate = 0;
        chosen.double_press_ms = 600;
        apply_access(chosen);
        const char* hint = SDL_GetHint(SDL_HINT_MOUSE_DOUBLE_CLICK_TIME);
        require(
            hint != nullptr && std::string_view{hint} == "600", "the double-click hint was not 600"
        );
        chosen.double_press_ms = 0;
        apply_access(chosen);
        require(
            SDL_GetHint(SDL_HINT_MOUSE_DOUBLE_CLICK_TIME) == nullptr,
            "the double-click hint stayed set at 0"
        );
    }

    std::cout << "key timing: passed\n";
}

} // namespace oa::app
