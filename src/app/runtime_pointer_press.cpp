// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The pointer event word, the right button as the interface type decides it,
// radar scrolls, mouse look, and the orders the pointer gives the selection
// with the queued-order cancel.
#include "oa/app/runtime.hpp"
#include "oa/data/mission_types.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include <SDL3/SDL.h>
#include <bit>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace oa::app {

namespace input = oa::sim::gameplay_input;

namespace {

oa::FixedVec3 fixed_point(const oa::sim::ground_orders::Point& point) {
    return {point[0], point[1], point[2]};
}

/// Returns a 16.16 point as the order issuers take it.
oa::sim::ground_orders::Point order_point(const oa::FixedVec3& point) {
    return {point.x, point.y, point.z};
}

} // namespace

void Runtime::record_pointer_event(const SDL_Event& event) {
    if (!match_)
        return;
    auto& game = match_->state().game;
    uint32_t keys = game.pointer_state[2] & (input::pointer_key_left | input::pointer_key_right);
    // The select button is the pointer word's left bit, and the command
    // button its right bit, so a stock layout still sets the same bits.
    const auto button_key = [this](uint8_t button) -> uint32_t {
        if (button == match_select_button())
            return input::pointer_key_left;
        if (button == match_command_button())
            return input::pointer_key_right;
        return 0;
    };
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        keys |= button_key(event.button.button);
    else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
        keys &= ~button_key(event.button.button);
    // The pointer key word's Shift is QUEUE's while touch controls are on.
    const auto mods = input_modifiers(ModifierUse::order);
    if ((mods & SDL_KMOD_SHIFT) != 0)
        keys |= input::pointer_key_shift;
    if ((mods & SDL_KMOD_CTRL) != 0)
        keys |= input::pointer_key_control;
    const auto source = game_screen_point(pointer_x_, pointer_y_);
    game.pointer_state[0] = static_cast<uint32_t>(source.x);
    game.pointer_state[1] = static_cast<uint32_t>(source.y);
    game.pointer_state[2] = keys;
}

bool Runtime::battlefield_contains(float x, float y) const {
    return x >= static_cast<float>(match_layout_.left) &&
           y >= static_cast<float>(match_layout_.top) &&
           x < static_cast<float>(match_layout_.left + match_layout_.battlefield_width()) &&
           y < static_cast<float>(match_layout_.top + match_layout_.battlefield_height());
}

void Runtime::refresh_pointer_area() {
    if (!match_)
        return;
    input::set_pointer_area(
        match_->state().game,
        radar_contains(pointer_x_, pointer_y_),
        battlefield_contains(pointer_x_, pointer_y_)
    );
}

void Runtime::center_camera_on_radar_point(float x, float y) {
    if (radar_map_w_ <= 0 || radar_map_h_ <= 0 || radar_picture_.width <= 0 ||
        radar_picture_.height <= 0)
        return;
    const auto rx = static_cast<int>(x) - radar_picture_.x;
    const auto ry = static_cast<int>(y) - radar_picture_.y;
    set_camera_position(
        rx * radar_map_w_ / radar_picture_.width - visible_map_width() / 2,
        ry * radar_map_h_ / radar_picture_.height - visible_map_height() / 2,
        match_camera_flags_
    );
    if (match_)
        oa::present::world_renderer::camera_stop_follow(match_->state().game);
    stop_match_tracking();
}

void Runtime::drive_mouse_look(bool begin) {
    namespace wr = oa::present::world_renderer;
    if (!match_)
        return;
    bind_match_view();
    auto& game = match_->state().game;
    wr::CursorSink sink{};
    sink.user = this;
    sink.screen_width = [](void* user) {
        return static_cast<int32_t>(
            static_cast<Runtime*>(user)->match_->state().game.offscreen_width
        );
    };
    sink.screen_height = [](void* user) {
        return static_cast<int32_t>(
            static_cast<Runtime*>(user)->match_->state().game.offscreen_height
        );
    };
    sink.set_position = [](void* user, int32_t x, int32_t y) {
        auto* self = static_cast<Runtime*>(user);
        const auto canvas = self->game_screen_canvas(x, y);
        self->pointer_x_ = self->match_pointer_x_ = static_cast<float>(canvas.x);
        self->pointer_y_ = self->match_pointer_y_ = static_cast<float>(canvas.y);
        auto& state = self->match_->state().game;
        state.pointer_state[0] = static_cast<uint32_t>(x);
        state.pointer_state[1] = static_cast<uint32_t>(y);
        float window_x = 0.0F;
        float window_y = 0.0F;
        if (self->sdl_.renderer != nullptr && self->sdl_.window != nullptr &&
            frame_to_window(
                self->sdl_.renderer,
                static_cast<float>(canvas.x),
                static_cast<float>(canvas.y),
                &window_x,
                &window_y
            ))
            SDL_WarpMouseInWindow(self->sdl_.window, window_x, window_y);
    };
    // The map cell a view's edge lies in, rounded toward negative infinity,
    // so that a view past the map's left or top edge counts its cells on
    // from the map's.
    const auto cell_of = [](int32_t pixel) {
        return pixel >= 0 ? pixel / OA_MAP_CELL_PIXELS
                          : -((OA_MAP_CELL_PIXELS - 1 - pixel) / OA_MAP_CELL_PIXELS);
    };
    if (begin) {
        stop_match_tracking();
        wr::mouse_look_begin(game, sink);
        game.mouse_look_cell_x = cell_of(match_camera_x_);
        game.mouse_look_cell_y = cell_of(match_camera_z_);
        return;
    }
    // The cells the pointer has travelled from the anchor, on from those
    // the look stands at, as the game counts them; the game's look then
    // warps the pointer back and ends on the button's release.
    const int32_t cell_x =
        (static_cast<int32_t>(game.pointer_state[0]) - game.mouse_look_anchor_x) /
            wr::mouse_look_pixels_per_cell +
        game.mouse_look_cell_x;
    const int32_t cell_y =
        (static_cast<int32_t>(game.pointer_state[1]) - game.mouse_look_anchor_y) /
            wr::mouse_look_pixels_per_cell +
        game.mouse_look_cell_y;
    wr::mouse_look_update(game, sink);
    // The view goes where the travel takes it, held within the view's
    // limits rather than on the map, and the look goes on from there.
    match_camera_x_ = cell_x * OA_MAP_CELL_PIXELS;
    match_camera_z_ = cell_y * OA_MAP_CELL_PIXELS;
    const auto held = view_camera();
    match_camera_x_ = held[0];
    match_camera_z_ = held[1];
    game.mouse_look_cell_x = cell_of(held[0]);
    game.mouse_look_cell_y = cell_of(held[1]);
    bind_match_view();
}

bool Runtime::follow_pointer_modes(const SDL_Event& event) {
    if (!match_)
        return false;
    auto& game = match_->state().game;
    if (game.mouse_look_active != 0) {
        drive_mouse_look(false);
        return true;
    }
    if ((input::pointer_flags(game) & input::pointer_radar_scroll) == 0)
        return false;
    // The scroll ends on the button that started it. The flag is the one this
    // interface accepts: the left-click interface ends on its command button.
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
        event.button.button == pointer_radar_scroll_button() &&
        input::end_radar_scroll(game, pointer_interface_is_left_click()))
        return true;
    center_camera_on_radar_point(pointer_x_, pointer_y_);
    return true;
}

void Runtime::handle_match_right_press(float x, float y) {
    if (!match_)
        return;
    auto& world = match_->state();
    // The frame's pointer pass: the unit under the pointer, the armed
    // command, the pointer area and the ground under it.
    update_pointer(x, y);
    // A press on the unit info panel is the panel's, as a left click is: OK
    // closes it, and its face takes the press with nothing done. A mouse's
    // press there goes to the panel before it gets here (unit_info_pointer);
    // a touch tap in the right-click interface comes here directly.
    if (click_unit_info(x, y))
        return;
    // The pick is wanted for what it writes into the Game block, which the
    // press reads; the cursor it returns is not needed.
    std::ignore = pick_match_cursor();
    switch (input::right_press(world.game)) {
    case input::RightPress::none:
        return;
    case input::RightPress::cancel_command:
        reset_match_command();
        pending_build_type_ = 0;
        input::set_pointer_command(world.game, input::OrderCommand::default_order);
        return;
    case input::RightPress::mouse_look:
        drive_mouse_look(true);
        return;
    case input::RightPress::clear_selection:
        // The selection marks drop, then the order panel is rebuilt. Every
        // panel over it closes first: the unit info panel closes, and an
        // open in-game menu closes and the match it held runs again, as F2
        // resumes it, as in 3.1c.
        clear_local_selection();
        close_unit_info();
        if (match_paused_)
            resume_match_pause();
        else
            apply_match_hud_for_selection();
        return;
    case input::RightPress::radar_scroll:
        // The view goes to the point at once, then follows the pointer until
        // the button comes up.
        select_game_cursor(static_cast<uint8_t>(input::OrderCursor::normal));
        center_camera_on_radar_point(x, y);
        return;
    case input::RightPress::radar_jump:
        center_camera_on_radar_point(x, y);
        return;
    case input::RightPress::default_order:
        break;
    }
    // The selection's orders resolve from the default order, the unit under
    // the pointer and the ground under it.
    const auto target = hovered_match_unit_;
    const auto ground = match_world_point(x, y);
    if (target == 0 && !ground)
        return;
    const auto issued =
        issue_selection_orders(input::OrderCommand::default_order, target, ground, queueing());
    if (!issued.empty())
        status_ = std::string(issued);
}

void Runtime::issue_pointer_ground_orders(float x, float y, bool queue) {
    if (!match_)
        return;
    const auto ground = match_world_point(x, y);
    if (!ground)
        return;
    const auto cursor = static_cast<input::OrderCursor>(pick_match_cursor());
    auto& world = match_->state();
    const auto command = input::pointer_command(world.game);
    switch (input::click_action(world, command, cursor)) {
    case input::ClickAction::none:
    case input::ClickAction::select_unit:
        return;
    case input::ClickAction::clear_selection:
        // As a right press that drops the selection, it closes the unit
        // info panel over the order panel.
        clear_local_selection();
        close_unit_info();
        apply_match_hud_for_selection();
        return;
    case input::ClickAction::issue_command:
        break;
    }
    const auto issued = issue_selection_orders(command, 0, ground, queue);
    if (!issued.empty())
        status_ = std::string(issued);
    finish_issued_command();
}

std::string_view Runtime::issue_selection_orders(
    input::OrderCommand command,
    uint16_t target,
    const std::optional<oa::sim::ground_orders::Point>& ground,
    bool queue
) {
    if (!match_)
        return {};
    auto& world = match_->state();
    world.game.local_player_index = match_local_player_;
    // The order table reads the unit under the pointer from the Game block;
    // the pointer's own pick is put back once the orders are given.
    const auto picked = world.game.cursor_unit_id;
    world.game.cursor_unit_id = target;

    struct RestorePick {
        oa::Game& game;
        uint16_t unit{};

        ~RestorePick() { game.cursor_unit_id = unit; }
    } restore_pick{world.game, picked};

    if (ground)
        input::set_pointer_position(world.game, fixed_point(*ground));
    const uint16_t bound =
        input::command_binds_cursor_unit(static_cast<uint8_t>(command)) ? target : uint16_t{0};
    std::vector<input::SelectionOrder> orders(world.unit_slot_count);
    const auto count = input::selection_orders(
        world, command, order_cursor_hooks(), orders.data(), static_cast<uint32_t>(orders.size())
    );
    std::optional<oa::sim::ground_orders::Point> target_point;
    if (bound != 0) {
        const auto& slot = match_->world().slots[bound];
        if (slot.unit != nullptr) {
            const std::array<uint32_t, 3> position = slot.unit->position;
            target_point = oa::sim::ground_orders::Point{
                std::bit_cast<int32_t>(position[0]),
                std::bit_cast<int32_t>(position[1]),
                std::bit_cast<int32_t>(position[2])
            };
        }
    }
    std::string_view issued;
    for (uint32_t index = 0; index < count; ++index) {
        const auto source = orders[index].actor->id;
        const auto order = orders[index].order;
        // The unit's own point: a move or patrol keeps its place in the
        // group around the ground under the pointer, and a queued order is
        // taken back at that point.
        std::optional<oa::sim::ground_orders::Point> destination;
        if (ground)
            destination = order_point(orders[index].position);
        if (cancels_queued_order(source, order, bound, destination, queue))
            continue;
        try {
            switch (order) {
            case input::UnitOrder::help_build:
            case input::UnitOrder::vtol_help_build:
                if (bound == 0)
                    break;
                match_->issue_help_build(source, bound, queue);
                issued = "Assist";
                break;
            case input::UnitOrder::repair_unit:
            case input::UnitOrder::vtol_repair_unit:
                if (bound == 0)
                    break;
                match_->issue_repair(source, bound, queue);
                issued = "Repair";
                break;
            case input::UnitOrder::follow_ground:
            case input::UnitOrder::vtol_follow:
                if (bound == 0)
                    break;
                match_->issue_guard(source, bound, queue);
                issued = "Guard";
                break;
            case input::UnitOrder::reclaim_unit:
            case input::UnitOrder::vtol_reclaim_unit:
                if (bound == 0)
                    break;
                match_->issue_reclaim(source, bound, queue);
                issued = "Reclaim";
                break;
            case input::UnitOrder::capture:
                if (bound == 0)
                    break;
                match_->issue_capture(source, bound, queue);
                issued = "Capture";
                break;
            case input::UnitOrder::ground_pickup:
            case input::UnitOrder::vtol_pickup:
                if (bound == 0)
                    break;
                match_->issue_load(source, bound, queue);
                issued = "Load";
                break;
            case input::UnitOrder::attack_chase:
            case input::UnitOrder::attack_nomove:
            case input::UnitOrder::attack_kamikaze:
            case input::UnitOrder::air_strike:
            case input::UnitOrder::air_to_air:
            case input::UnitOrder::air_to_ground:
            case input::UnitOrder::air_to_ground_hover:
                // A unit no attack resolves for is given no order.
                if (bound != 0) {
                    std::ignore = match_->issue_attack_command(
                        source, bound, queue, ground ? &*ground : nullptr
                    );
                    issued = "Attack";
                } else if (ground) {
                    std::ignore = match_->issue_attack_ground(source, *ground, queue);
                    issued = "Attack ground";
                }
                break;
            case input::UnitOrder::suppress:
                // The ground under the pointer, as for every other order;
                // the unit there is not followed.
                if (const auto& at = ground ? ground : target_point) {
                    std::ignore = match_->issue_attack_ground(source, *at, queue);
                    issued = "Attack ground";
                }
                break;
            case input::UnitOrder::attack_special:
                if (const auto& at = ground ? ground : target_point) {
                    match_->issue_attack_special(source, *at, queue, bound);
                    issued = "D-Gun";
                }
                break;
            case input::UnitOrder::reclaim:
            case input::UnitOrder::vtol_reclaim:
                if (const auto at =
                        ground ? feature_reclaim_point(feature_point_under_pointer(*ground))
                               : std::nullopt) {
                    match_->issue_feature_reclaim(source, *at, queue);
                    issued = "Reclaim";
                }
                break;
            case input::UnitOrder::resurrect:
                if (const auto at =
                        ground ? feature_reclaim_point(feature_point_under_pointer(*ground))
                               : std::nullopt) {
                    match_->issue_order(
                        source,
                        oa::data::mission_types::index_for_name(input::unit_order_name(order)),
                        queue,
                        0,
                        &*at,
                        0,
                        0
                    );
                    issued = "Resurrect";
                }
                break;
            case input::UnitOrder::patrol:
            case input::UnitOrder::vtol_patrol:
            case input::UnitOrder::repair_patrol:
            case input::UnitOrder::vtol_repair_patrol:
            case input::UnitOrder::qpatrol:
                if (destination) {
                    match_->issue_patrol(source, *destination, queue);
                    if (issued.empty())
                        issued = "Patrol";
                }
                break;
            case input::UnitOrder::ground_unload:
            case input::UnitOrder::vtol_unload:
                if (ground) {
                    match_->issue_unload(source, *ground, queue);
                    issued = "Unload";
                }
                break;
            case input::UnitOrder::vtol_landing:
                // An aircraft sent onto an allied air pad lands on it; a loaded
                // transport hands its cargo to the pad.
                if (bound == 0)
                    break;
                match_->issue_order(
                    source, oa::sim::ground_orders::vtol_landing_kind, queue, bound, nullptr, 0, 0
                );
                issued = "Land";
                break;
            case input::UnitOrder::move_ground:
            case input::UnitOrder::vtol_move:
            case input::UnitOrder::qmove:
                if (destination && match_->takes_move_order(source)) {
                    match_->issue_ground_move(source, *destination, queue);
                    if (issued.empty())
                        issued = "Move";
                }
                break;
            default:
                break;
            }
        } catch (const std::exception& error) {
            status_ = std::string("pointer order: ") + error.what();
            std::cerr << "unsupported operation: " << status_ << '\n';
            return {};
        }
    }
    return issued;
}

bool Runtime::cancels_queued_order(
    uint16_t source,
    input::UnitOrder order,
    uint16_t target,
    const std::optional<oa::sim::ground_orders::Point>& ground,
    bool queue
) {
    if (!queue || !match_)
        return false;
    const auto kind = oa::data::mission_types::index_for_name(input::unit_order_name(order));
    if (kind == oa::data::mission_types::unknown_mission)
        return false;
    return match_->cancel_queued_order(
        source, kind, target, ground ? &*ground : nullptr, reclaim_click_snapped_
    );
}

bool Runtime::cancels_queued_command(
    uint16_t source,
    input::OrderCommand command,
    uint16_t target,
    const std::optional<oa::sim::ground_orders::Point>& ground,
    bool queue
) {
    if (!queue || !match_)
        return false;
    const auto& world = match_->state();
    const auto* actor = oa::world_unit_at(&world, source);
    if (actor == nullptr)
        return false;
    const auto* aimed = target != 0 ? oa::world_unit_at(&world, target) : nullptr;
    const auto position = ground ? fixed_point(*ground) : oa::FixedVec3{};
    const auto order = input::unit_order(
        world, command, *actor, aimed, ground ? &position : nullptr, order_cursor_hooks()
    );
    return cancels_queued_order(source, order, target, ground, queue);
}

uint16_t Runtime::group_order_bound_unit(input::OrderCommand command, uint16_t target) {
    if (!match_ || target == 0 || !input::command_binds_cursor_unit(static_cast<uint8_t>(command)))
        return 0;
    const auto& slots = match_->world().slots;
    return target < slots.size() && slots[target].unit != nullptr ? target : uint16_t{0};
}

input::GroupCentre Runtime::local_selection_centre(uint16_t bound) {
    if (!match_)
        return {};
    auto& world = match_->state();
    world.game.local_player_index = match_local_player_;
    const auto* unit = bound != 0 ? oa::world_unit_at(&world, bound) : nullptr;
    return input::selection_centre(world, unit);
}

oa::sim::ground_orders::Point Runtime::group_order_destination(
    const input::GroupCentre& centre,
    input::OrderCommand command,
    uint16_t source,
    uint16_t target,
    const oa::sim::ground_orders::Point& point
) {
    if (!match_)
        return point;
    const auto& world = match_->state();
    const auto* actor = oa::world_unit_at(&world, source);
    if (actor == nullptr)
        return point;
    const auto* aimed = target != 0 ? oa::world_unit_at(&world, target) : nullptr;
    const auto position = fixed_point(point);
    const auto order =
        input::unit_order(world, command, *actor, aimed, &position, order_cursor_hooks());
    if (!input::order_keeps_group_shape(order))
        return point;
    return order_point(input::group_order_point(centre, *actor, position));
}

} // namespace oa::app
