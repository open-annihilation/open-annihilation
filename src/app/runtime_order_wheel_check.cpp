// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The order wheel a held mouse button opens, on a skirmish. Touch controls
// and the pad HUD stay off. A short click and a drag are not this wheel.
#include "oa/app/runtime.hpp"

#include "match_fault.hpp"
#include "oa/ui/engine_settings.hpp"
#include "touch_state.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace settings = oa::ui::engine_settings;
namespace hud = oa::ui::touch_hud;

/// The path `--snapshot` writes for one window size.
///
/// @param snapshot the --snapshot path
/// @param size the window, such as 640x480
/// @return the snapshot's path
fs::path size_snapshot(const fs::path& snapshot, std::string_view size) {
    return snapshot.parent_path() / (snapshot.stem().string() + "-" + std::string(size) + ".ppm");
}

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("order wheel: " + what);
}

} // namespace

void Runtime::check_order_wheel() {
    const auto step = [](std::string_view name) {
        std::cout << "order wheel: " << name << '\n' << std::flush;
    };
    if (sdl_.window == nullptr || sdl_.renderer == nullptr)
        fail("needs the window");

    // Set every preference this check relies on. A second run must not depend
    // on what the first left in the file.
    const auto apply_wheel = [&](settings::OrderWheel wheel) {
        auto chosen = engine_settings();
        chosen.order_wheel = wheel;
        chosen.long_press_ms = 500;
        chosen.drag_threshold = 32;
        chosen.middle_click_pan = true;
        apply_engine_settings(chosen);
    };
    apply_wheel(settings::OrderWheel::off);
    // A skirmish starts on the left-click interface, where a right press
    // clears the selection. These orders are the right-click interface's.
    preferences_.interface_type = oa::sim::gameplay_input::interface_right_click;
    if (touch_controls_active())
        fail("touch controls are on");
    if (const auto* made = touch_state_if_made(); made != nullptr && made->hud.pad.hud)
        fail("the pad HUD is on");

    start_benchmark_skirmish();
    if (match_ == nullptr || !selected_tnt_)
        fail("the match did not start");
    match_->state().game.interface_type = oa::sim::gameplay_input::interface_right_click;
    apply_wheel(settings::OrderWheel::off);
    if (touch_controls_active())
        fail("touch controls are on");
    if (const auto* made = touch_state_if_made(); made != nullptr && made->hud.pad.hud)
        fail("the pad HUD is on");
    if (match_layout_.width <= 0 || match_layout_.height <= 0)
        fail("the match has no layout");

    uint16_t commander = 0;
    for (const auto& slot : match_->world().slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr || slot.record.type_index == 0 ||
            slot.record.owner_index != match_local_player_)
            continue;
        if (const auto* def = definition_for(slot.unit_index); def != nullptr && def->can_dgun) {
            commander = slot.unit_index;
            break;
        }
    }
    if (commander == 0)
        fail("found no local commander");
    clear_local_selection();
    adopt_selection(commander);
    selected_match_unit_ = commander;
    apply_match_hud_for_selection();

    const auto enemy_player = static_cast<uint8_t>(match_local_player_ == 0 ? 1 : 0);
    const auto enemy_type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "CORAK");
    if (enemy_type == 0)
        fail("the game lacks CORAK");
    const auto home_x =
        static_cast<int32_t>(match_->world().slots[commander].unit->position[0] >> 16);
    const auto home_z =
        static_cast<int32_t>(match_->world().slots[commander].unit->position[2] >> 16);
    const int32_t toward_x =
        home_x < static_cast<int32_t>(selected_tnt_->tile_width * 16U) ? 1 : -1;
    const auto ground_at = [&](int32_t dx, int32_t dz) {
        const auto x = static_cast<uint32_t>(home_x + toward_x * dx) << 16;
        const auto z = static_cast<uint32_t>(home_z + dz) << 16;
        return std::array<uint32_t, 3>{x, static_cast<uint32_t>(match_->map_height(x, z)) << 16, z};
    };
    oa::sim::unit_spawn::Request request;
    request.player = enemy_player;
    request.type = enemy_type;
    request.finished = true;
    request.state = kGroundOccupancyState;
    request.position = ground_at(120, 0);
    const auto* spawned = match_->create(request);
    if (spawned == nullptr || spawned->unit == nullptr)
        fail("could not spawn an enemy");
    const uint16_t enemy = spawned->unit_index;
    ++match_timing_.tick;
    match_->simulation().tick = match_timing_.tick;
    tick_or_raise(*match_);
    match_->stop_orders(enemy);
    match_->world().slots[enemy].record.flags &=
        ~(OA_UNIT_FLAG_MOVE_ORDER_MASK | OA_UNIT_FLAG_FIRE_ORDER_MASK);

    bool running = true;
    const auto wait_ms = [](uint32_t milliseconds) {
        const uint64_t began = SDL_GetTicks();
        while (SDL_GetTicks() - began < milliseconds)
            SDL_Delay(1);
    };
    const auto send_pointer = [&](SDL_EventType type, uint8_t button, float at_x, float at_y) {
        float window_x = at_x;
        float window_y = at_y;
        if (!frame_to_window(sdl_.renderer, at_x, at_y, &window_x, &window_y))
            fail(std::string(SDL_GetError()));
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion.windowID = SDL_GetWindowID(sdl_.window);
            event.motion.x = window_x;
            event.motion.y = window_y;
        } else {
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = button;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
        }
        dispatch_event(event, running);
        if (!running)
            fail("a pointer event ended the run");
    };
    const auto key_escape = [&](bool down) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.key = SDLK_ESCAPE;
        event.key.scancode = SDL_SCANCODE_ESCAPE;
        event.key.down = down;
        dispatch_event(event, running);
        if (!running)
            fail("Escape ended the run");
    };
    const auto unit_point = [&](uint16_t id) {
        const auto screen = project_match_point(
            live_viewport(match_camera_x_, match_camera_z_),
            match_->world().slots[id].unit->position
        );
        const float at_x = static_cast<float>(screen.x);
        const float at_y = static_cast<float>(screen.y);
        update_pointer(at_x, at_y);
        if (hovered_.has_value())
            fail("the pointer over unit " + std::to_string(id) + " is on a control");
        if (hovered_match_unit_ != id)
            fail(
                "the pointer over unit " + std::to_string(id) + " found unit " +
                std::to_string(hovered_match_unit_)
            );
        return std::pair{at_x, at_y};
    };
    const float shift_x[] = {120.F, -120.F, 160.F, -160.F, 80.F, -80.F, 200.F, 40.F};
    const float shift_y[] = {40.F, 40.F, -30.F, 70.F, 100.F, -50.F, 20.F, 140.F};
    const auto empty_ground = [&](float origin_x, float origin_y) {
        for (int index = 0; index < 8; ++index) {
            const float at_x = origin_x + shift_x[index];
            const float at_y = origin_y + shift_y[index];
            if (!battlefield_contains(at_x, at_y) || radar_contains(at_x, at_y) ||
                placed_hud_covers(at_x, at_y))
                continue;
            update_pointer(at_x, at_y);
            if (hovered_.has_value() || hovered_match_unit_ != 0)
                continue;
            if (!match_world_point(at_x, at_y))
                continue;
            return std::pair{at_x, at_y};
        }
        fail("found no empty ground");
    };

    struct Orders {
        std::array<oa::sim::match_runtime::Match::OrderRecordView, 8> records{};
        std::size_t count{};
    };

    const auto orders_of = [&] {
        Orders found;
        found.count =
            match_->queue_records(commander, false, found.records.data(), found.records.size());
        return found;
    };
    const auto settle = [&] {
        match_->stop_orders(commander);
        status_.clear();
        match_command_ = MatchCommand::none;
    };
    const auto radial_open = [&] {
        const auto* made = touch_state_if_made();
        return made != nullptr && made->hud.radial.has_value();
    };
    const auto require_drawn = [&] {
        if (!radial_open())
            fail("the wheel is not open");
        if (!TouchDrawAccess::refresh_layer(*this))
            fail("the layer was not drawn");
        if (touch_->layer.bounds.empty())
            fail("the layer was not drawn");
    };
    const auto wedge_at = [&](std::size_t slot, hud::RadialItem item) {
        const auto* made = touch_state_if_made();
        if (made == nullptr || !made->hud.radial)
            fail("the wheel is not open");
        const auto wedge = made->hud.radial->wedges[slot];
        if (wedge.item != item)
            fail("the wedge is not that order");
        return std::pair{static_cast<float>(wedge.label.x), static_cast<float>(wedge.label.y)};
    };
    const auto hold_still = [&](uint8_t button, float at_x, float at_y) {
        send_pointer(SDL_EVENT_MOUSE_MOTION, 0, at_x, at_y);
        send_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, button, at_x, at_y);
        wait_ms(600);
        advance_input_actions(running);
        if (!running)
            fail("the hold ended the run");
    };
    const auto let_go = [&](uint8_t button, float at_x, float at_y) {
        send_pointer(SDL_EVENT_MOUSE_BUTTON_UP, button, at_x, at_y);
    };
    // Escape first, so the button coming up does not pick the wedge under it.
    const auto shut = [&](uint8_t button, float at_x, float at_y) {
        key_escape(true);
        key_escape(false);
        let_go(button, at_x, at_y);
    };

    center_camera_on_unit(enemy);
    render_match_surface();
    settle();
    step("setting off, a right press on the enemy gives Attack as it goes down");
    {
        const auto [at_x, at_y] = unit_point(enemy);
        send_pointer(SDL_EVENT_MOUSE_MOTION, 0, at_x, at_y);
        send_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT, at_x, at_y);
        if (status_ != "Attack")
            fail("the press gave '" + status_ + "'");
        let_go(SDL_BUTTON_RIGHT, at_x, at_y);
    }

    apply_wheel(settings::OrderWheel::hold_right);
    center_camera_on_unit(commander);
    render_match_surface();
    settle();
    step("hold-right, a right click of 100 ms on the ground gives a move");
    {
        const auto commander_at = unit_point(commander);
        const auto [at_x, at_y] = empty_ground(commander_at.first, commander_at.second);
        send_pointer(SDL_EVENT_MOUSE_MOTION, 0, at_x, at_y);
        send_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT, at_x, at_y);
        wait_ms(100);
        let_go(SDL_BUTTON_RIGHT, at_x, at_y);
        if (status_ != "Move")
            fail("the click gave '" + status_ + "'");
        if (radial_open())
            fail("a click opened the wheel");
    }

    settle();
    step("a right hold of 600 ms opens the wheel");
    const auto commander_at = unit_point(commander);
    const auto [ground_x, ground_y] = empty_ground(commander_at.first, commander_at.second);
    hold_still(SDL_BUTTON_RIGHT, ground_x, ground_y);
    require_drawn();
    if (!status_.empty() || orders_of().count != 0)
        fail("the hold gave an order");
    const auto* opened = touch_state_if_made();
    if (opened == nullptr || !opened->hud.radial)
        fail("the wheel is not open");
    const auto held_ground = match_world_point(
        static_cast<float>(opened->hud.radial->anchor.x),
        static_cast<float>(opened->hud.radial->anchor.y)
    );
    if (!held_ground)
        fail("the held point has no world point");

    step("moving onto Patrol and letting go gives a patrol to the held point");
    {
        const auto [at_x, at_y] = wedge_at(1, hud::RadialItem::patrol);
        send_pointer(SDL_EVENT_MOUSE_MOTION, 0, at_x, at_y);
        if (order_wheel_mouse_aim() != 1)
            fail("Patrol was not aimed");
        let_go(SDL_BUTTON_RIGHT, at_x, at_y);
        if (status_ != "Patrol")
            fail("letting go gave '" + status_ + "'");
        const auto found = orders_of();
        bool patrol = false;
        for (std::size_t index = 0; index < found.count; ++index) {
            const auto kind = found.records[index].kind;
            const bool is_patrol = kind == oa::sim::match_runtime::patrol_kind ||
                                   kind == oa::sim::match_runtime::repair_patrol_kind ||
                                   kind == oa::sim::match_runtime::qpatrol_kind;
            if (is_patrol && found.records[index].point == *held_ground)
                patrol = true;
        }
        if (!patrol)
            fail("the patrol is not at the held point");
        if (radial_open())
            fail("the wheel stayed open");
    }

    center_camera_on_unit(enemy);
    render_match_surface();
    settle();
    step("a hold on the enemy then a release on Attack attacks it");
    {
        const auto [at_x, at_y] = unit_point(enemy);
        hold_still(SDL_BUTTON_RIGHT, at_x, at_y);
        require_drawn();
        if (status_ == "Attack" || orders_of().count != 0)
            fail("the hold attacked before the release");
        const auto [aim_x, aim_y] = wedge_at(2, hud::RadialItem::attack);
        send_pointer(SDL_EVENT_MOUSE_MOTION, 0, aim_x, aim_y);
        let_go(SDL_BUTTON_RIGHT, aim_x, aim_y);
        if (status_ != "Attack")
            fail("the release gave '" + status_ + "'");
        const auto found = orders_of();
        bool attacked = false;
        for (std::size_t index = 0; index < found.count; ++index) {
            const auto kind = found.records[index].kind;
            const bool attack = kind == oa::sim::match_runtime::attack_chase_kind ||
                                kind == oa::sim::match_runtime::attack_no_move_kind;
            if (attack && found.records[index].target == enemy)
                attacked = true;
        }
        if (!attacked)
            fail("the attack is not on the enemy");
        if (radial_open())
            fail("the wheel stayed open");
    }

    center_camera_on_unit(commander);
    render_match_surface();
    settle();
    step("a release on the hub gives nothing");
    {
        const auto here = unit_point(commander);
        const auto [at_x, at_y] = empty_ground(here.first, here.second);
        const auto before = orders_of().count;
        const auto before_status = status_;
        hold_still(SDL_BUTTON_RIGHT, at_x, at_y);
        require_drawn();
        const auto* made = touch_state_if_made();
        if (made == nullptr || !made->hud.radial)
            fail("the wheel is not open");
        const auto centre_x = static_cast<float>(made->hud.radial->centre.x);
        const auto centre_y = static_cast<float>(made->hud.radial->centre.y);
        let_go(SDL_BUTTON_RIGHT, centre_x, centre_y);
        if (orders_of().count != before || status_ != before_status)
            fail("the hub gave an order");
        if (radial_open())
            fail("the hub left the wheel open");
    }

    settle();
    step("Escape closes it");
    {
        const auto here = unit_point(commander);
        const auto [at_x, at_y] = empty_ground(here.first, here.second);
        const auto before = orders_of().count;
        const auto before_status = status_;
        hold_still(SDL_BUTTON_RIGHT, at_x, at_y);
        require_drawn();
        key_escape(true);
        key_escape(false);
        if (radial_open() || order_wheel_mouse_open())
            fail("Escape left the wheel open");
        if (orders_of().count != before || status_ != before_status)
            fail("Escape gave an order");
        if (screen_ != Screen::match || match_paused_ || match_ == nullptr)
            fail("Escape left the match");
        let_go(SDL_BUTTON_RIGHT, at_x, at_y);
    }

    apply_wheel(settings::OrderWheel::hold_middle);
    center_camera_on_unit(commander);
    render_match_surface();
    settle();
    step("hold-middle, a middle click pans and a middle hold opens the wheel");
    {
        const auto here = unit_point(commander);
        const auto [at_x, at_y] = empty_ground(here.first, here.second);
        send_pointer(SDL_EVENT_MOUSE_MOTION, 0, at_x, at_y);
        send_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_MIDDLE, at_x, at_y);
        wait_ms(100);
        let_go(SDL_BUTTON_MIDDLE, at_x, at_y);
        if (middle_pan_.phase != middle_pan::Phase::clicked || middle_pan_.held)
            fail("the middle click did not pan");
        if (radial_open())
            fail("the middle click opened the wheel");
        hold_still(SDL_BUTTON_MIDDLE, at_x, at_y);
        require_drawn();
        if (middle_pan_.phase != middle_pan::Phase::idle || middle_pan_.held)
            fail("the middle hold left the pan running");
        shut(SDL_BUTTON_MIDDLE, at_x, at_y);
        if (radial_open())
            fail("the wheel stayed open");
    }

    apply_wheel(settings::OrderWheel::hold_right);
    const auto shoot = [&](int width, int height, std::string_view size) {
        step(std::string("snapshot ") + std::string(size));
        if (!SDL_SetWindowSize(sdl_.window, width, height) || !SDL_SyncWindow(sdl_.window))
            fail(std::string("SDL_SetWindowSize: ") + SDL_GetError());
        apply_output_mode();
        center_camera_on_unit(commander);
        render_match_surface();
        const auto here = unit_point(commander);
        const auto [at_x, at_y] = empty_ground(here.first, here.second);
        hold_still(SDL_BUTTON_RIGHT, at_x, at_y);
        require_drawn();
        const auto [aim_x, aim_y] = wedge_at(1, hud::RadialItem::patrol);
        send_pointer(SDL_EVENT_MOUSE_MOTION, 0, aim_x, aim_y);
        if (order_wheel_mouse_aim() != 1)
            fail(std::string(size) + " did not aim a wedge");
        require_drawn();
        if (!options_.snapshot.empty()) {
            renderer::Surface frame;
            compose_match_frame(frame);
            write_ppm(size_snapshot(options_.snapshot, size), frame);
        }
        shut(SDL_BUTTON_RIGHT, at_x, at_y);
        if (radial_open())
            fail(std::string(size) + " left the wheel open");
    };
    shoot(640, 480, "640x480");
    shoot(1280, 720, "1280x720");
    shoot(1920, 1080, "1920x1080");

    apply_wheel(settings::OrderWheel::off);
    if (order_wheel_mouse_open() || radial_open())
        fail("turning the wheel off left it open");
}

} // namespace oa::app
