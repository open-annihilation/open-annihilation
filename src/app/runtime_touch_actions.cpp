// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The touch controls' actions: what each control, radial pick, placement
// step, group chip, SELECT ▾ item, rail slot, FORCE chip and ring wedge
// does, each through the engine's own functions, so replays, saves and
// shared games see the orders a mouse would give (docs/touch-controls.md).
#include "oa/app/runtime.hpp"
#include "touch_state.hpp"
#include "oa/app/extension.hpp"
#include "oa/app/platform_hooks.hpp"
#include "oa/data/languages/unit_texts.hpp"
#include "oa/sim/gameplay_input/input.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/ui/touch_gestures.hpp"
#include "oa/ui/touch_hud.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace oa::app {
namespace {

namespace gestures = oa::ui::touch_gestures;
namespace hud = oa::ui::touch_hud;
namespace input = oa::sim::gameplay_input;

/// Nanoseconds in a millisecond.
constexpr uint64_t kNanosecondsPerMillisecond = 1'000'000;
/// How long a tip bubble shows, milliseconds.
constexpr uint64_t kTipMilliseconds = 2500;
/// The rings of the battlefield tap's nearest-unit probe, as fractions of unit_pick_points
/// (4, 8 and 12 points).
constexpr float kUnitProbeRings[] = {1.0F / 3.0F, 2.0F / 3.0F, 1.0F};
/// The directions each ring of the nearest-unit probe looks in.
constexpr int kUnitProbeDirections = 8;
/// What a tap on a self-destruct gadget shows.
constexpr std::string_view kSelfDestructHint = "Hold to self-destruct";

/// Returns whether an armed command takes a dragged box as an area order.
///
/// @param command the armed command
/// @return whether a box gives an area order
bool area_command(MatchCommand command) noexcept {
    return command == MatchCommand::attack || command == MatchCommand::dgun ||
           command == MatchCommand::reclaim || command == MatchCommand::repair;
}

/// Returns the radial item a plain tap at the point would give.
///
/// @param command the armed command
/// @param cursor the order cursor at the point
/// @return the item, or none
std::optional<hud::RadialItem> radial_default(MatchCommand command, input::OrderCursor cursor) {
    switch (command) {
    case MatchCommand::move:
        return hud::RadialItem::move;
    case MatchCommand::attack:
        return hud::RadialItem::attack;
    case MatchCommand::dgun:
        return hud::RadialItem::blast;
    case MatchCommand::patrol:
        return hud::RadialItem::patrol;
    case MatchCommand::repair:
        return hud::RadialItem::repair;
    case MatchCommand::reclaim:
        return hud::RadialItem::reclaim;
    case MatchCommand::capture:
        return hud::RadialItem::capture;
    case MatchCommand::load:
        return hud::RadialItem::load;
    case MatchCommand::unload:
        return hud::RadialItem::unload;
    case MatchCommand::guard:
        return hud::RadialItem::guard;
    case MatchCommand::none:
    case MatchCommand::build:
        break;
    }
    switch (cursor) {
    case input::OrderCursor::move:
    case input::OrderCursor::teleport:
        return hud::RadialItem::move;
    case input::OrderCursor::attack:
    case input::OrderCursor::attack_dropped:
    case input::OrderCursor::attack_out_of_range:
        return hud::RadialItem::attack;
    case input::OrderCursor::patrol:
        return hud::RadialItem::patrol;
    case input::OrderCursor::guard:
        return hud::RadialItem::guard;
    case input::OrderCursor::repair:
    case input::OrderCursor::resurrect:
        return hud::RadialItem::repair;
    case input::OrderCursor::reclaim:
        return hud::RadialItem::reclaim;
    case input::OrderCursor::capture:
        return hud::RadialItem::capture;
    case input::OrderCursor::load:
    case input::OrderCursor::load_by_air:
        return hud::RadialItem::load;
    case input::OrderCursor::unload:
        return hud::RadialItem::unload;
    case input::OrderCursor::select:
    case input::OrderCursor::build:
    case input::OrderCursor::enemy:
    case input::OrderCursor::friendly:
    case input::OrderCursor::normal:
        break;
    }
    return std::nullopt;
}

/// Returns a rectangle grown by a margin on every side.
///
/// @param rect the rectangle, canvas pixels
/// @param margin canvas pixels
/// @return the grown rectangle
hud::Rect grown(const hud::Rect& rect, int margin) noexcept {
    return {rect.x - margin, rect.y - margin, rect.width + margin * 2, rect.height + margin * 2};
}

/// How near the ghost, in points, a finger's hold or double tap places it
/// where it is rather than moving it to the finger first.
constexpr float ghost_reach_points = 44.0F;

/// Returns whether a canvas point lies inside a rectangle.
///
/// @param rect the rectangle, canvas pixels
/// @param x canvas pixels
/// @param y canvas pixels
/// @return whether it lies inside
bool rect_holds(const hud::Rect& rect, float x, float y) noexcept {
    return x >= static_cast<float>(rect.x) && y >= static_cast<float>(rect.y) &&
           x < static_cast<float>(rect.x + rect.width) &&
           y < static_cast<float>(rect.y + rect.height);
}

} // namespace

void TouchDispatchAccess::battlefield_gestures(
    Runtime& runtime, const gestures::GestureBatch& batch
) {
    auto& dispatch = runtime.touch_state().dispatch;
    // A pan and a pinch of the same move: the pan first, then the zoom
    // about the fingers' new centre, so the ground under them stays there.
    std::optional<gestures::Gesture> pinch;
    float pinch_scale = 1.0F;
    for (std::size_t index = 0; index < batch.count && index < batch.items.size(); ++index) {
        const auto& gesture = batch.items[index];
        switch (gesture.kind) {
        case gestures::GestureKind::press:
            dispatch.hold_placed = false;
            battlefield_hover(runtime, gesture.x, gesture.y);
            break;
        case gestures::GestureKind::rest_moved:
            battlefield_hover(runtime, gesture.x, gesture.y);
            break;
        case gestures::GestureKind::tap:
            battlefield_tap(runtime, gesture.x, gesture.y, gesture.taps);
            break;
        case gestures::GestureKind::hold_started:
            // While placing, a hold places the building: at the ghost
            // when the finger is on it, else where the finger is; on the
            // open megamap where the finger is, as a tap there does.
            if (runtime.megamap_shown() && runtime.match_command_ == MatchCommand::build) {
                dispatch.hold_placed = true;
                send_click(runtime, gesture.x, gesture.y, 1);
            } else if (dispatch.placement_touch) {
                dispatch.hold_placed = true;
                place_at(runtime, gesture.x, gesture.y);
            } else {
                runtime.play_haptic(Haptic::hold_started);
            }
            break;
        case gestures::GestureKind::hold_released:
            // A hold that placed a building opens no radial on its lift,
            // though the placement has ended since.
            if (!dispatch.hold_placed && !dispatch.placement_touch)
                open_radial(runtime, gesture.x, gesture.y);
            dispatch.hold_placed = false;
            break;
        case gestures::GestureKind::drag_began:
            drag_began(runtime, gesture);
            break;
        case gestures::GestureKind::drag_moved:
            drag_moved(runtime, gesture);
            break;
        case gestures::GestureKind::drag_ended:
            drag_ended(runtime, gesture);
            break;
        case gestures::GestureKind::pan_began:
        case gestures::GestureKind::pan_moved:
            dispatch.inertia_x = 0.0F;
            dispatch.inertia_y = 0.0F;
            dispatch.finger_point.reset();
            runtime.pan_match_camera_by(-gesture.dx, -gesture.dy);
            break;
        case gestures::GestureKind::pan_ended:
            dispatch.inertia_x = gesture.velocity_x;
            dispatch.inertia_y = gesture.velocity_y;
            break;
        case gestures::GestureKind::pinch_began:
            dispatch.finger_point.reset();
            break;
        case gestures::GestureKind::pinch_moved:
            pinch_scale *= gesture.scale;
            pinch = gesture;
            break;
        case gestures::GestureKind::pinch_ended:
            break;
        case gestures::GestureKind::two_finger_tap:
            if (!close_overlay(runtime))
                runtime.clear_or_cancel_match_command();
            break;
        case gestures::GestureKind::cancelled:
            cancel_drags(runtime);
            break;
        }
    }
    if (pinch && pinch_scale > 0.0F)
        runtime.zoom_match_about(pinch_scale, pinch->x, pinch->y);
}

void TouchDispatchAccess::battlefield_hover(Runtime& runtime, float x, float y) {
    auto& dispatch = runtime.touch_state().dispatch;
    dispatch.finger_point = std::array<float, 2>{x, y};
    // While placing, the pointer stays on the ghost.
    if (!dispatch.placement_touch && !dispatch.box_active && !dispatch.scroll_active &&
        runtime.screen_ == Screen::match)
        runtime.update_pointer(x, y);
}

std::array<float, 2> TouchDispatchAccess::fat_finger_point(Runtime& runtime, float x, float y) {
    runtime.update_pointer(x, y);
    if (!runtime.match_ || runtime.hovered_match_unit_ != 0 || runtime.hovered_ ||
        runtime.match_command_ == MatchCommand::build)
        return {x, y};
    // No unit is under the finger: the nearest one a probe finds within
    // reach, tested as the pointer's pick tests it.
    auto& world = runtime.match_->state();
    const float reach = points_to_pixels(runtime, hud::unit_pick_points);
    for (const float ring : kUnitProbeRings) {
        for (int direction = 0; direction < kUnitProbeDirections; ++direction) {
            const double angle = 2.0 * std::numbers::pi * direction / kUnitProbeDirections;
            const float px = x + static_cast<float>(std::cos(angle)) * reach * ring;
            const float py = y + static_cast<float>(std::sin(angle)) * reach * ring;
            if (!runtime.battlefield_contains(px, py) || runtime.placed_hud_covers(px, py))
                continue;
            const auto point = runtime.game_screen_point(px, py);
            world.game.pointer_state[0] = static_cast<uint32_t>(point.x);
            world.game.pointer_state[1] = static_cast<uint32_t>(point.y);
            runtime.refresh_pointer_area();
            const auto unit = oa::sim::selection::unit_under_pointer(
                world, runtime.on_screen_lists(), runtime.selection_hooks()
            );
            if (unit == 0)
                continue;
            runtime.update_pointer(px, py);
            if (runtime.hovered_match_unit_ != 0)
                return {px, py};
        }
    }
    // None: the pointer and the Game block's pointer go back to the finger.
    runtime.update_pointer(x, y);
    return {x, y};
}

void TouchDispatchAccess::battlefield_tap(Runtime& runtime, float x, float y, uint8_t taps) {
    auto& state = runtime.touch_state();
    if (runtime.screen_ != Screen::match || !runtime.match_)
        return;
    // On the open megamap a tap is the mouse's click there, at the finger,
    // which the megamap takes as it takes the mouse's (megamap_pointer), and
    // nothing reaches the battlefield under it. A building armed is placed
    // where it is tapped, as a click there places it; the second tap of that
    // double tap gives nothing more.
    const bool megamap = runtime.megamap_shown();
    if (megamap) {
        const bool placed = std::exchange(state.dispatch.megamap_placed, false);
        if (taps >= 2 && placed)
            return;
        state.dispatch.megamap_placed = runtime.match_command_ == MatchCommand::build;
    } else if (state.dispatch.placement_touch) {
        // While placing, a tap moves the ghost and gives no order; a double
        // tap places the building where it was tapped.
        if (taps >= 2)
            place_at(runtime, x, y);
        else
            move_ghost(runtime, x, y);
        return;
    }
    const auto point = megamap ? std::array<float, 2>{x, y} : fat_finger_point(runtime, x, y);
    runtime.refresh_pointer_modifiers();
    // What the tap does decides which latch it used: a select cursor is a
    // selection, anything else an order.
    const auto cursor = static_cast<input::OrderCursor>(
        megamap ? runtime.megamap_cursor_at(x, y) : runtime.pick_match_cursor()
    );
    const auto action_class =
        cursor == input::OrderCursor::select && runtime.match_command_ == MatchCommand::none
            ? hud::ActionClass::selection
            : hud::ActionClass::order;
    // The right-click interface gives a default order with the right
    // press; its left click only selects or gives an armed order.
    const bool right_click_interface = runtime.match_->state().game.interface_type != 0;
    if (right_click_interface && runtime.match_command_ == MatchCommand::none &&
        cursor != input::OrderCursor::select) {
        if (megamap) {
            send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, x, y, 0);
            send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, 1, SDL_BUTTON_RIGHT);
            send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_UP, x, y, 1, SDL_BUTTON_RIGHT);
        } else {
            runtime.handle_match_right_press(point[0], point[1]);
        }
    } else {
        send_click(runtime, point[0], point[1], taps == 0 ? 1 : taps);
    }
    auto& look = runtime.touch_state().hud;
    look.latches.used(action_class, look.latch_mode);
    runtime.refresh_pointer_modifiers();
}

void TouchDispatchAccess::drag_began(Runtime& runtime, const gestures::Gesture& gesture) {
    auto& dispatch = runtime.touch_state().dispatch;
    if (runtime.screen_ != Screen::match || !runtime.match_)
        return;
    // On the open megamap a drag is its box, as the mouse's left drag there:
    // the battlefield under it neither scrolls nor takes a box.
    if (runtime.megamap_shown()) {
        dispatch.megamap_drag = true;
        send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, gesture.start_x, gesture.start_y, 0);
        send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_DOWN, gesture.start_x, gesture.start_y, 1);
        send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, gesture.x, gesture.y, 0);
        return;
    }
    if (dispatch.placement_touch) {
        dispatch.ghost_drag = true;
        move_ghost(runtime, gesture.x, gesture.y);
        return;
    }
    if (gesture.role == gestures::DragRole::scroll && !gesture.from_hold) {
        dispatch.scroll_active = true;
        dispatch.finger_point.reset();
        dispatch.drag_last_x = gesture.start_x;
        dispatch.drag_last_y = gesture.start_y;
        scroll_to(runtime, gesture.x, gesture.y);
        return;
    }
    // A box from where the finger landed, through the engine's own press,
    // motion and release.
    runtime.play_haptic(Haptic::box_started);
    dispatch.box_active = true;
    dispatch.auto_scroll_x = gesture.x;
    dispatch.auto_scroll_y = gesture.y;
    send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, gesture.start_x, gesture.start_y, 0);
    send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_DOWN, gesture.start_x, gesture.start_y, 1);
    if (!runtime.touch_state().dispatch.box_active)
        return;
    // The recogniser already saw a drag, so the release reads as a box
    // however short or quick; an order that takes an area (the engine
    // starts no box with one armed) gets its box here, unless the press was
    // the unit info panel's.
    const uint32_t tick = runtime.frontend_tick();
    const uint32_t window = runtime.drag_click_ticks();
    const uint32_t pressed = tick >= window ? tick - window : 0;
    if (runtime.match_drag_)
        runtime.match_drag_->pressed_tick = pressed;
    else if (area_command(runtime.match_command_) && runtime.unit_info_pressed_button_ == 0)
        if (const auto ground = runtime.match_pointer_ground(gesture.start_x, gesture.start_y))
            runtime.match_drag_ = Runtime::MatchDragBox{*ground, *ground, pressed};
    send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, gesture.x, gesture.y, 0);
}

void TouchDispatchAccess::drag_moved(Runtime& runtime, const gestures::Gesture& gesture) {
    auto& dispatch = runtime.touch_state().dispatch;
    if (dispatch.megamap_drag) {
        send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, gesture.x, gesture.y, 0);
    } else if (dispatch.ghost_drag) {
        move_ghost(runtime, gesture.x, gesture.y);
    } else if (dispatch.scroll_active) {
        scroll_to(runtime, gesture.x, gesture.y);
    } else if (dispatch.box_active) {
        dispatch.auto_scroll_x = gesture.x;
        dispatch.auto_scroll_y = gesture.y;
        send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, gesture.x, gesture.y, 0);
    }
}

void TouchDispatchAccess::drag_ended(Runtime& runtime, const gestures::Gesture& gesture) {
    auto& dispatch = runtime.touch_state().dispatch;
    if (dispatch.megamap_drag) {
        dispatch.megamap_drag = false;
        send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, gesture.x, gesture.y, 0);
        send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_UP, gesture.x, gesture.y, 1);
        auto& look = runtime.touch_state().hud;
        look.latches.used(hud::ActionClass::selection, look.latch_mode);
        runtime.refresh_pointer_modifiers();
    } else if (dispatch.ghost_drag) {
        move_ghost(runtime, gesture.x, gesture.y);
        dispatch.ghost_drag = false;
    } else if (dispatch.scroll_active) {
        scroll_to(runtime, gesture.x, gesture.y);
        dispatch.scroll_active = false;
    } else if (dispatch.box_active) {
        const bool area = runtime.match_drag_ && area_command(runtime.match_command_);
        dispatch.box_active = false;
        dispatch.auto_scrolling = false;
        send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, gesture.x, gesture.y, 0);
        send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_UP, gesture.x, gesture.y, 1);
        auto& look = runtime.touch_state().hud;
        look.latches.used(
            area ? hud::ActionClass::order : hud::ActionClass::selection, look.latch_mode
        );
        runtime.refresh_pointer_modifiers();
    }
}

void TouchDispatchAccess::cancel_drags(Runtime& runtime) {
    auto& dispatch = runtime.touch_state().dispatch;
    if (dispatch.box_active || dispatch.megamap_drag) {
        // The box is dropped with no release: nothing is selected or
        // ordered, and the left button is up again.
        runtime.match_drag_.reset();
        runtime.megamap_.pressed = false;
        if (runtime.match_)
            runtime.match_->state().game.pointer_state[2] &= ~input::pointer_key_left;
        runtime.selected_ = -1;
    }
    dispatch.box_active = false;
    dispatch.megamap_drag = false;
    dispatch.scroll_active = false;
    dispatch.ghost_drag = false;
    dispatch.auto_scrolling = false;
}

void TouchDispatchAccess::scroll_to(Runtime& runtime, float x, float y) {
    auto& dispatch = runtime.touch_state().dispatch;
    // The map follows the finger.
    runtime.pan_match_camera_by(-(x - dispatch.drag_last_x), -(y - dispatch.drag_last_y));
    dispatch.drag_last_x = x;
    dispatch.drag_last_y = y;
}

void TouchDispatchAccess::move_ghost(Runtime& runtime, float x, float y) {
    auto& state = runtime.touch_state();
    const auto& layout = runtime.match_layout_;
    const float left = static_cast<float>(layout.left);
    const float top = static_cast<float>(layout.top);
    const float right =
        static_cast<float>(layout.left + std::max(1, layout.battlefield_width()) - 1);
    const float bottom =
        static_cast<float>(layout.top + std::max(1, layout.battlefield_height()) - 1);
    const float anchor_x = std::clamp(x, left, right);
    const float anchor_y = std::clamp(y, top, bottom);
    state.hud.placement.anchor = {
        static_cast<int>(std::lround(anchor_x)), static_cast<int>(std::lround(anchor_y))
    };
    state.dispatch.auto_scroll_x = x;
    state.dispatch.auto_scroll_y = y;
    runtime.update_pointer(anchor_x, anchor_y);
}

void TouchDispatchAccess::place_at(Runtime& runtime, float x, float y) {
    const auto& placement = runtime.touch_state().hud.placement;
    if (!placement.active)
        return;
    // A finger on the ghost places it where it is; elsewhere the ghost
    // goes to the finger first.
    const float reach = points_to_pixels(runtime, ghost_reach_points);
    const float dx = x - static_cast<float>(placement.anchor.x);
    const float dy = y - static_cast<float>(placement.anchor.y);
    if (dx * dx + dy * dy > reach * reach)
        move_ghost(runtime, x, y);
    confirm_placement(runtime);
}

void TouchDispatchAccess::confirm_placement(Runtime& runtime) {
    auto& state = runtime.touch_state();
    if (!state.hud.placement.active)
        return;
    const auto anchor_x = static_cast<float>(state.hud.placement.anchor.x);
    const auto anchor_y = static_cast<float>(state.hud.placement.anchor.y);
    // A refused site plays the engine's refusal and stays armed; the
    // haptic says so too.
    if (const auto site = runtime.build_site_under(anchor_x, anchor_y); !site || !site->legal)
        runtime.play_haptic(Haptic::site_refused);
    runtime.refresh_pointer_modifiers();
    runtime.place_pending_build(anchor_x, anchor_y);
    auto& look = runtime.touch_state().hud;
    look.latches.used(hud::ActionClass::order, look.latch_mode);
    runtime.refresh_pointer_modifiers();
}

void TouchDispatchAccess::refresh_placement(Runtime& runtime) {
    auto& state = runtime.touch_state();
    auto& dispatch = state.dispatch;
    auto& placement = state.hud.placement;
    const bool pending = runtime.match_ && runtime.match_command_ == MatchCommand::build &&
                         runtime.pending_build_type_ != 0;
    // A placement ended, or a mouse took it over.
    if (!pending || (dispatch.placement_touch && dispatch.pointer_seen)) {
        if (!pending)
            dispatch.placement_by_pad = false;
        if (dispatch.placement_touch || placement.active) {
            placement = {};
            dispatch.placement_touch = false;
            dispatch.ghost_drag = false;
        }
        return;
    }
    if (!dispatch.placement_touch) {
        // Armed by a mouse, a mouse places it; started by the pad, the ghost follows the pad's
        // pointer until a finger lands on the battlefield (hand_placement_to_touch).
        if (dispatch.pointer_seen || dispatch.placement_by_pad)
            return;
        dispatch.placement_touch = true;
        placement.active = true;
        const auto& layout = runtime.match_layout_;
        placement.anchor = {
            layout.left + layout.battlefield_width() / 2,
            layout.top + layout.battlefield_height() / 2
        };
    }
    // Other code moves the pointer; the ghost stays at its anchor.
    const auto anchor_x = static_cast<float>(placement.anchor.x);
    const auto anchor_y = static_cast<float>(placement.anchor.y);
    runtime.update_pointer(anchor_x, anchor_y);
    const auto site = runtime.build_site_under(anchor_x, anchor_y);
    placement.legal = site && site->legal;
    placement.name.clear();
    const auto type = static_cast<std::size_t>(runtime.pending_build_type_);
    if (type != 0 && type <= runtime.unit_definitions_.size()) {
        const auto& definition = runtime.unit_definitions_[type - 1];
        // The name in the language shown, as the bottom bar shows it.
        if (definition.display_name.empty())
            placement.name = definition.unit_name;
        else
            placement.name = oa::data::languages::unit_display_name(
                definition.unit_name, definition.display_name
            );
    }
}

void TouchDispatchAccess::hand_placement_to_touch(Runtime& runtime) {
    auto& state = runtime.touch_state();
    auto& dispatch = state.dispatch;
    dispatch.placement_by_pad = false;
    const bool pending = runtime.match_ && runtime.match_command_ == MatchCommand::build &&
                         runtime.pending_build_type_ != 0;
    if (!pending || dispatch.placement_touch)
        return;
    // The ghost stays where the pad's pointer left it, when that is on the battlefield.
    const auto& layout = runtime.match_layout_;
    float x = static_cast<float>(layout.left + layout.battlefield_width() / 2);
    float y = static_cast<float>(layout.top + layout.battlefield_height() / 2);
    if (runtime.battlefield_contains(runtime.match_pointer_x_, runtime.match_pointer_y_) &&
        !runtime.placed_hud_covers(runtime.match_pointer_x_, runtime.match_pointer_y_)) {
        x = runtime.match_pointer_x_;
        y = runtime.match_pointer_y_;
    }
    dispatch.placement_touch = true;
    state.hud.placement.active = true;
    state.hud.placement.anchor = {
        static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))
    };
}

void TouchDispatchAccess::hold_force(Runtime& runtime, bool held) {
    auto& look = runtime.touch_state().hud;
    if (look.force_touch == held)
        return;
    look.force_touch = held;
    runtime.refresh_pointer_modifiers();
}

void TouchDispatchAccess::open_radial(Runtime& runtime, float x, float y) {
    if (runtime.screen_ != Screen::match || !runtime.match_)
        return;
    // On the open megamap the wheel is for the icon and the ground under the
    // finger. While a panel holds the match the megamap takes no left press,
    // and no wheel opens over it.
    const bool megamap = runtime.megamap_shown();
    if (megamap && runtime.match_paused_)
        return;
    auto& state = runtime.touch_state();
    runtime.update_pointer(x, y);
    hud::RadialAvailability availability{};
    const bool selected = runtime.has_local_selection();
    for (std::size_t index = 0; index < hud::order_count; ++index)
        availability.orders[index] =
            selected &&
            runtime.order_command_available(hud::order_name(static_cast<hud::Order>(index)));
    const uint16_t unit = megamap ? runtime.megamap_unit_at(x, y) : runtime.hovered_match_unit_;
    const auto& slots = runtime.match_->world().slots;
    const bool own = unit != 0 && unit < slots.size() && slots[unit].unit != nullptr &&
                     slots[unit].owner_index == runtime.match_local_player_;
    availability.info = unit != 0 || runtime.selected_match_unit_ != 0;
    availability.type = own;
    availability.blast_slot_shows_blast = selected && runtime.order_command_available("BLAST");
    availability.default_item = radial_default(
        runtime.match_command_,
        static_cast<input::OrderCursor>(
            megamap ? runtime.megamap_cursor_at(x, y) : runtime.pick_match_cursor()
        )
    );
    auto radial = hud::make_radial(
        {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))},
        availability,
        state.viewport
    );
    radial.target_unit = unit;
    if (state.hud.sheet != hud::Sheet::none)
        close_sheet(runtime, true);
    runtime.touch_state().hud.radial = radial;
}

void TouchDispatchAccess::radial_pick(Runtime& runtime, hud::RadialItem item) {
    auto& state = runtime.touch_state();
    if (!state.hud.radial || !runtime.match_)
        return;
    const hud::Radial radial = *state.hud.radial;
    const auto wedge = std::find_if(
        radial.wedges.begin(), radial.wedges.end(), [item](const hud::RadialWedge& candidate) {
            return candidate.item == item;
        }
    );
    // A greyed wedge leaves the menu open.
    if (wedge == radial.wedges.end() || !wedge->available)
        return;
    state.hud.radial.reset();
    const auto x = static_cast<float>(radial.anchor.x);
    const auto y = static_cast<float>(radial.anchor.y);
    if (const auto order = hud::radial_order(item)) {
        const auto name = hud::order_name(*order);
        // STOP is given at once.
        if (*order == hud::Order::stop) {
            std::ignore = runtime.arm_match_command(name, false);
            return;
        }
        if (!runtime.arm_match_command(name, false))
            return;
        // The order is given at the held point; the hub queues it.
        auto& dispatch = runtime.touch_state().dispatch;
        const SDL_Keymod before = dispatch.pulse;
        if (radial.queue_hub)
            dispatch.pulse = static_cast<SDL_Keymod>(dispatch.pulse | SDL_KMOD_LSHIFT);
        runtime.refresh_pointer_modifiers();
        try {
            // On the open megamap the order is the megamap's click there.
            if (runtime.megamap_shown())
                runtime.megamap_click(runtime.megamap_unit_at(x, y), runtime.megamap_ground(x, y));
            else
                runtime.handle_match_left_click(x, y, 1);
        } catch (...) {
            runtime.touch_state().dispatch.pulse = before;
            throw;
        }
        runtime.touch_state().dispatch.pulse = before;
        auto& look = runtime.touch_state().hud;
        look.latches.used(hud::ActionClass::order, look.latch_mode);
        runtime.refresh_pointer_modifiers();
        return;
    }
    if (item == hud::RadialItem::info) {
        // On the open megamap the unit is the one whose icon was under the
        // finger, named to the panel as a unit off the screen is.
        if (runtime.megamap_shown()) {
            if (radial.target_unit == 0) {
                show_unit_info(runtime);
                return;
            }
            runtime.hovered_.reset();
            runtime.hovered_match_unit_ = radial.target_unit;
            runtime.match_->state().game.cursor_unit_id = radial.target_unit;
            std::ignore = runtime.open_unit_info();
            return;
        }
        runtime.update_pointer(x, y);
        if (runtime.hovered_match_unit_ == 0)
            show_unit_info(runtime);
        else
            std::ignore = runtime.open_unit_info();
        return;
    }
    if (item == hud::RadialItem::type) {
        // Every unit of that type, with ADD's Shift off.
        const auto latches = runtime.touch_state().hud.latches;
        runtime.touch_state().hud.latches.clear();
        runtime.refresh_pointer_modifiers();
        // On the open megamap, every unit of the type of the one whose icon
        // was under the finger, as the megamap's double click selects them.
        if (runtime.megamap_shown()) {
            const auto& slots = runtime.match_->world().slots;
            if (radial.target_unit < slots.size() && slots[radial.target_unit].unit != nullptr) {
                const auto type = slots[radial.target_unit].unit->type_index;
                runtime.select_units_matching([type](const oa::sim::unit_spawn::Slot& slot) {
                    return slot.unit && slot.unit->type_index == type;
                });
            }
        } else {
            runtime.select_match_unit(x, y, 2);
        }
        runtime.touch_state().hud.latches = latches;
        runtime.refresh_pointer_modifiers();
    }
}

void TouchDispatchAccess::finger_landed(Runtime& runtime, std::size_t slot, uint64_t now) {
    auto& state = runtime.touch_state();
    auto& finger = state.dispatch.fingers[slot];
    const auto id = finger.id;
    switch (finger.target) {
    case TouchTarget::control: {
        // Outside an open sheet or radial: it closes, and the finger is
        // used up. On a sheet's panel away from its controls: taken.
        if (finger.control.control == hud::Control::sheet_outside) {
            finger.slid_off = true;
            std::ignore = close_overlay(runtime);
            return;
        }
        if (finger.control.control == hud::Control::none) {
            finger.slid_off = true;
            return;
        }
        press_look(runtime, finger.control);
        if (const auto latch = control_latch(finger.control.control)) {
            state.hud.latches.press(*latch, now / kNanosecondsPerMillisecond);
            runtime.refresh_pointer_modifiers();
        } else if (finger.control.control == hud::Control::force) {
            // FORCE holds while the finger rests.
            hold_force(runtime, true);
        }
        return;
    }
    case TouchTarget::hud_gadget:
    case TouchTarget::frontend: {
        // A self-destruct gadget waits for its hold.
        if (finger.target == TouchTarget::hud_gadget && finger.self_destruct)
            return;
        const float x = finger.press_x;
        const float y = finger.press_y;
        send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, x, y, 0);
        if (finger_at(runtime, slot, id) == nullptr)
            return;
        send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, 1);
        if (auto* still = finger_at(runtime, slot, id); still != nullptr)
            still->pressed = true;
        return;
    }
    case TouchTarget::minimap:
    case TouchTarget::battlefield:
    case TouchTarget::none:
        return;
    }
}

void TouchDispatchAccess::finger_moved(Runtime& runtime, std::size_t slot, uint64_t) {
    auto& state = runtime.touch_state();
    auto& finger = state.dispatch.fingers[slot];
    switch (finger.target) {
    case TouchTarget::control: {
        if (finger.slid_off)
            return;
        // Off the control and its reach, the press is taken back.
        const int margin =
            static_cast<int>(std::lround(points_to_pixels(runtime, hud::gadget_pick_points)));
        if (rect_holds(grown(finger.control.rect, margin), finger.x, finger.y))
            return;
        finger.slid_off = true;
        release_look(runtime, finger.control);
        if (const auto latch = control_latch(finger.control.control)) {
            state.hud.latches.cancel(*latch);
            runtime.refresh_pointer_modifiers();
        }
        if (finger.control.control == hud::Control::force)
            hold_force(runtime, false);
        if (finger.control.control == hud::Control::more_item &&
            finger.control.index == static_cast<uint8_t>(hud::MoreItem::self_destruct))
            state.hud.self_destruct_progress = 0.0F;
        return;
    }
    case TouchTarget::hud_gadget:
    case TouchTarget::frontend: {
        if (finger.target == TouchTarget::hud_gadget && finger.self_destruct) {
            if (finger.moved)
                finger.slid_off = true;
            return;
        }
        if (!finger.pressed || !finger.moved)
            return;
        // On its gadget the pointer stays on it; off it, the pointer
        // follows the finger (a scroll bar's knob follows it too).
        const auto on_gadget = on_own_gadget(runtime, finger);
        const std::array<float, 2> point =
            on_gadget ? *on_gadget : std::array<float, 2>{finger.x, finger.y};
        if (point[0] == finger.sent_x && point[1] == finger.sent_y)
            return;
        finger.sent_x = point[0];
        finger.sent_y = point[1];
        send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, point[0], point[1], 0);
        return;
    }
    case TouchTarget::minimap:
    case TouchTarget::battlefield:
    case TouchTarget::none:
        return;
    }
}

void TouchDispatchAccess::finger_lifted(
    Runtime& runtime, std::size_t slot, bool cancelled, uint64_t now
) {
    auto& state = runtime.touch_state();
    // The finger as it lifted: what it does may drop every finger.
    const ClaimedFinger finger = state.dispatch.fingers[slot];
    switch (finger.target) {
    case TouchTarget::control: {
        release_look(runtime, finger.control);
        if (finger.control.control == hud::Control::force) {
            hold_force(runtime, false);
            return;
        }
        if (finger.control.control == hud::Control::more_item &&
            finger.control.index == static_cast<uint8_t>(hud::MoreItem::self_destruct))
            state.hud.self_destruct_progress = 0.0F;
        if (const auto latch = control_latch(finger.control.control)) {
            if (!finger.slid_off && !cancelled)
                state.hud.latches.release(
                    *latch, now / kNanosecondsPerMillisecond, state.dispatch.settings.hold_ms
                );
            else
                state.hud.latches.cancel(*latch);
            runtime.refresh_pointer_modifiers();
            return;
        }
        if (!cancelled && !finger.slid_off && !finger.held)
            control_tap(runtime, finger.control, now);
        return;
    }
    case TouchTarget::hud_gadget:
        hud_release(runtime, finger, cancelled, now);
        return;
    case TouchTarget::frontend:
        frontend_release(runtime, finger, cancelled, now);
        return;
    case TouchTarget::minimap:
    case TouchTarget::battlefield:
    case TouchTarget::none:
        return;
    }
}

void TouchDispatchAccess::finger_gestures(
    Runtime& runtime, std::size_t slot, const gestures::GestureBatch& batch, uint64_t now
) {
    const auto id = runtime.touch_state().dispatch.fingers[slot].id;
    for (std::size_t index = 0; index < batch.count && index < batch.items.size(); ++index) {
        auto* finger = finger_at(runtime, slot, id);
        if (finger == nullptr)
            return;
        const auto& gesture = batch.items[index];
        switch (finger->target) {
        case TouchTarget::control:
            if (gesture.kind == gestures::GestureKind::hold_started && !finger->slid_off) {
                const auto control = finger->control;
                const bool acted = control_hold(runtime, control, now);
                if (auto* still = finger_at(runtime, slot, id); still != nullptr && acted)
                    still->held = true;
            }
            break;
        case TouchTarget::hud_gadget:
            if (gesture.kind == gestures::GestureKind::hold_started && finger->pressed &&
                !finger->moved && !finger->held)
                hud_hold(runtime, slot, now);
            break;
        case TouchTarget::minimap:
            // A tap or a drag on the minimap moves the camera. A tap with a
            // command armed, and a hold, are the radar's left press
            // (issue_radar_orders), which selects, gives an order or does
            // nothing by the cursor there and never moves the camera; but a
            // hold with no command armed in the right-click interface gives
            // the default order there, as that interface's right press on
            // the battlefield does.
            switch (gesture.kind) {
            case gestures::GestureKind::tap:
                if (finger->held)
                    break;
                if (runtime.match_command_ != MatchCommand::none) {
                    runtime.refresh_pointer_modifiers();
                    if (runtime.issue_radar_orders(gesture.x, gesture.y)) {
                        auto& look = runtime.touch_state().hud;
                        look.latches.used(hud::ActionClass::order, look.latch_mode);
                        runtime.refresh_pointer_modifiers();
                    }
                } else {
                    runtime.center_camera_on_radar_point(gesture.x, gesture.y);
                }
                break;
            case gestures::GestureKind::drag_began:
            case gestures::GestureKind::drag_moved:
            case gestures::GestureKind::drag_ended:
                if (!finger->held)
                    runtime.center_camera_on_radar_point(gesture.x, gesture.y);
                break;
            case gestures::GestureKind::hold_started: {
                finger->held = true;
                runtime.play_haptic(Haptic::hold_started);
                runtime.refresh_pointer_modifiers();
                // In the right-click interface the default order needs no
                // cursor, as that interface's right press gives it.
                const bool default_order =
                    runtime.match_ && runtime.match_command_ == MatchCommand::none &&
                    runtime.match_->state().game.interface_type == input::interface_right_click;
                if (default_order ? runtime.issue_radar_default_order(gesture.x, gesture.y)
                                  : runtime.issue_radar_orders(gesture.x, gesture.y)) {
                    auto& look = runtime.touch_state().hud;
                    look.latches.used(hud::ActionClass::order, look.latch_mode);
                    runtime.refresh_pointer_modifiers();
                }
                break;
            }
            default:
                break;
            }
            break;
        case TouchTarget::frontend:
        case TouchTarget::battlefield:
        case TouchTarget::none:
            break;
        }
    }
}

std::optional<hud::Latch> TouchDispatchAccess::control_latch(hud::Control control) noexcept {
    switch (control) {
    case hud::Control::queue:
        return hud::Latch::queue;
    case hud::Control::add:
        return hud::Latch::add;
    case hud::Control::times_five:
        return hud::Latch::times_five;
    default:
        return std::nullopt;
    }
}

void TouchDispatchAccess::press_look(Runtime& runtime, const hud::ControlRect& control) {
    auto& pressed = runtime.touch_state().hud.pressed;
    for (auto& entry : pressed)
        if (entry.control == control.control && entry.index == control.index)
            return;
    for (auto& entry : pressed)
        if (entry.control == hud::Control::none) {
            entry.control = control.control;
            entry.index = control.index;
            return;
        }
}

void TouchDispatchAccess::release_look(Runtime& runtime, const hud::ControlRect& control) {
    for (auto& entry : runtime.touch_state().hud.pressed)
        if (entry.control == control.control && entry.index == control.index)
            entry = {};
}

void TouchDispatchAccess::close_sheet(Runtime& runtime, bool explicit_close) {
    auto& look = runtime.touch_state().hud;
    const auto sheet = look.sheet;
    if (sheet == hud::Sheet::none)
        return;
    look.sheet = hud::Sheet::none;
    // The drawer and MORE loaded a page of their own; closing them shows
    // the selection's page again, unless a building waits to be placed
    // from the page they loaded.
    if (explicit_close && (sheet == hud::Sheet::drawer || sheet == hud::Sheet::more) &&
        runtime.pending_build_type_ == 0)
        runtime.apply_match_hud_for_selection();
}

bool TouchDispatchAccess::close_overlay(Runtime& runtime) {
    auto& look = runtime.touch_state().hud;
    if (look.radial) {
        look.radial.reset();
        return true;
    }
    if (look.sheet != hud::Sheet::none) {
        close_sheet(runtime, true);
        return true;
    }
    return false;
}

void TouchDispatchAccess::open_sheet(Runtime& runtime, hud::Sheet sheet) {
    auto& look = runtime.touch_state().hud;
    look.radial.reset();
    if (look.sheet != hud::Sheet::none && look.sheet != sheet)
        close_sheet(runtime, true);
    runtime.touch_state().hud.sheet = sheet;
}

void TouchDispatchAccess::open_drawer(Runtime& runtime) {
    open_sheet(runtime, hud::Sheet::drawer);
    auto& state = runtime.touch_state();
    if (runtime.selected_match_unit_ != state.dispatch.drawer_unit) {
        state.hud.drawer_page = 0;
        state.dispatch.drawer_unit = runtime.selected_match_unit_;
    }
    const auto* definition = runtime.selected_match_unit_ != 0
                                 ? runtime.definition_for(runtime.selected_match_unit_)
                                 : nullptr;
    if (definition != nullptr && definition->builder) {
        show_drawer_page(runtime, state.hud.drawer_page + 1);
        return;
    }
    state.hud.drawer_tab = hud::DrawerTab::orders;
    runtime.show_match_orders_page();
}

void TouchDispatchAccess::show_drawer_page(Runtime& runtime, int page) {
    runtime.show_match_build_page(std::max(1, page));
    auto& look = runtime.touch_state().hud;
    // A builder still being built shows its orders page instead.
    if (runtime.match_build_page_ > 0) {
        look.drawer_tab = hud::DrawerTab::build;
        look.drawer_page = static_cast<uint8_t>(std::clamp(runtime.match_build_page_ - 1, 0, 255));
    } else {
        look.drawer_tab = hud::DrawerTab::orders;
    }
}

void TouchDispatchAccess::select_item(Runtime& runtime, uint8_t index) {
    close_sheet(runtime, false);
    const auto control = SDL_KMOD_LCTRL;
    const bool add = runtime.touch_state().hud.latches.active(hud::Latch::add);
    switch (static_cast<hud::SelectItem>(index)) {
    case hud::SelectItem::all:
        runtime.press_match_key(SDLK_A, control);
        break;
    case hud::SelectItem::builders:
        runtime.press_match_key(SDLK_B, control);
        break;
    case hud::SelectItem::factories:
        runtime.press_match_key(SDLK_F, control);
        break;
    case hud::SelectItem::aircraft:
        runtime.press_match_key(SDLK_V, control);
        break;
    case hud::SelectItem::on_screen:
        runtime.press_match_key(SDLK_S, control);
        break;
    case hud::SelectItem::commander: {
        // With ADD the commander joins the selection, as Ctrl+Shift+C does.
        runtime.press_match_key(
            SDLK_C, add ? static_cast<SDL_Keymod>(control | SDL_KMOD_LSHIFT) : control
        );
        if (add) {
            auto& look = runtime.touch_state().hud;
            look.latches.used(hud::ActionClass::selection, look.latch_mode);
            runtime.refresh_pointer_modifiers();
        }
        break;
    }
    case hud::SelectItem::same_type:
        runtime.press_match_key(SDLK_Z, control);
        break;
    case hud::SelectItem::centre:
        runtime.press_match_key(SDLK_HOME, SDL_KMOD_NONE);
        break;
    case hud::SelectItem::follow:
        runtime.press_match_key(SDLK_T, SDL_KMOD_NONE);
        break;
    case hud::SelectItem::next_unit:
        runtime.press_match_key(SDLK_N, SDL_KMOD_NONE);
        break;
    case hud::SelectItem::next_report:
        runtime.press_match_key(SDLK_F3, SDL_KMOD_NONE);
        break;
    }
}

void TouchDispatchAccess::show_unit_info(Runtime& runtime) {
    if (!runtime.match_)
        return;
    const uint16_t unit = runtime.selected_match_unit_;
    const auto& slots = runtime.match_->world().slots;
    if (unit != 0 && unit < slots.size() && slots[unit].unit != nullptr) {
        // The pointer onto the unit, as F1 is pressed over it; a unit off
        // the screen is named to the panel directly.
        const auto viewport =
            runtime.live_viewport(runtime.match_camera_x_, runtime.match_camera_z_);
        const auto point = runtime.project_match_point(viewport, slots[unit].unit->position);
        runtime.update_pointer(static_cast<float>(point.x), static_cast<float>(point.y));
        if (runtime.hovered_match_unit_ != unit) {
            runtime.hovered_.reset();
            runtime.hovered_match_unit_ = unit;
            runtime.match_->state().game.cursor_unit_id = unit;
        }
    }
    std::ignore = runtime.open_unit_info();
}

void TouchDispatchAccess::control_tap(
    Runtime& runtime, const hud::ControlRect& control, uint64_t now
) {
    if (runtime.screen_ != Screen::match || !runtime.match_)
        return;
    auto& state = runtime.touch_state();
    const bool phone = state.viewport.device == hud::DeviceClass::phone;
    const auto toggle = [&runtime](hud::Sheet sheet) {
        if (runtime.touch_state().hud.sheet == sheet)
            close_sheet(runtime, true);
        else
            open_sheet(runtime, sheet);
    };
    const bool watcher = (runtime.current_extension_state() & extension_state::local_watcher) != 0;
    const auto& layout = runtime.match_layout_;
    const auto centre_x = static_cast<float>(layout.left + layout.battlefield_width() / 2);
    const auto centre_y = static_cast<float>(layout.top + layout.battlefield_height() / 2);
    switch (control.control) {
    case hud::Control::none:
    case hud::Control::queue:
    case hud::Control::add:
    case hud::Control::times_five:
    case hud::Control::sheet_outside:
    case hud::Control::force:
        return;
    case hud::Control::build_wedge:
        build_wedge_tap(runtime, control, now);
        return;
    case hud::Control::group_wedge:
        // A wedge of the group ring the pad shows: as the group's chip.
        if (control.index >= 1 && control.index <= hud::group_ring_slot_count) {
            runtime.select_squad(control.index, state.hud.latches.active(hud::Latch::add));
            auto& look = runtime.touch_state().hud;
            look.latches.used(hud::ActionClass::selection, look.latch_mode);
            runtime.refresh_pointer_modifiers();
        }
        return;
    case hud::Control::clear:
    case hud::Control::place_cancel:
    case hud::Control::banner_cancel:
        runtime.clear_or_cancel_match_command();
        return;
    case hud::Control::select_menu:
        toggle(hud::Sheet::select_menu);
        return;
    case hud::Control::group_store: {
        if (!runtime.has_local_selection())
            return;
        const auto& counts = state.hud.group_counts;
        for (std::size_t group = 1; group < counts.size(); ++group)
            if (counts[group] == 0) {
                runtime.assign_squad(static_cast<int>(group));
                return;
            }
        return;
    }
    case hud::Control::group_chip: {
        runtime.select_squad(control.index, state.hud.latches.active(hud::Latch::add));
        auto& look = runtime.touch_state().hud;
        look.latches.used(hud::ActionClass::selection, look.latch_mode);
        runtime.refresh_pointer_modifiers();
        return;
    }
    case hud::Control::pause:
        runtime.press_match_key(SDLK_PAUSE, SDL_KMOD_NONE);
        return;
    case hud::Control::speed:
        toggle(hud::Sheet::speed);
        return;
    case hud::Control::chat:
        runtime.open_chat_line();
        return;
    case hud::Control::centre:
        runtime.press_match_key(SDLK_HOME, SDL_KMOD_NONE);
        return;
    case hud::Control::follow:
        runtime.press_match_key(SDLK_T, SDL_KMOD_NONE);
        return;
    case hud::Control::next_unit:
        runtime.press_match_key(SDLK_N, SDL_KMOD_NONE);
        return;
    case hud::Control::info:
        show_unit_info(runtime);
        return;
    case hud::Control::menu:
        if (phone)
            toggle(hud::Sheet::phone_menu);
        else
            runtime.press_match_key(SDLK_F2, SDL_KMOD_NONE);
        return;
    case hud::Control::build_drawer:
        if (state.hud.sheet == hud::Sheet::drawer)
            close_sheet(runtime, true);
        else
            open_drawer(runtime);
        return;
    case hud::Control::zoom_out:
        runtime.handle_match_zoom(-1.0F, centre_x, centre_y, false);
        return;
    case hud::Control::zoom_in:
        runtime.handle_match_zoom(1.0F, centre_x, centre_y, false);
        return;
    case hud::Control::order_slot: {
        if (control.index >= state.hud.rail_count || control.index >= state.hud.rail.size())
            return;
        const auto slot = state.hud.rail[control.index];
        if (slot.more) {
            if (state.hud.sheet == hud::Sheet::more) {
                close_sheet(runtime, true);
            } else {
                open_sheet(runtime, hud::Sheet::more);
                runtime.show_match_orders_page();
            }
            return;
        }
        std::ignore = runtime.arm_match_command(hud::order_name(slot.order), true);
        return;
    }
    case hud::Control::more:
        if (state.hud.sheet == hud::Sheet::more) {
            close_sheet(runtime, true);
        } else {
            open_sheet(runtime, hud::Sheet::more);
            runtime.show_match_orders_page();
        }
        return;
    case hud::Control::drawer_build_tab:
        show_drawer_page(runtime, state.hud.drawer_page + 1);
        return;
    case hud::Control::drawer_orders_tab:
        state.hud.drawer_tab = hud::DrawerTab::orders;
        runtime.show_match_orders_page();
        return;
    case hud::Control::drawer_close:
        close_sheet(runtime, true);
        return;
    case hud::Control::drawer_prev:
    case hud::Control::drawer_next: {
        if (state.hud.drawer_tab != hud::DrawerTab::build || runtime.match_build_page_ <= 0)
            return;
        // As the panel's PREV and NEXT step: through a tall page's parts,
        // then the pages.
        const int step = control.control == hud::Control::drawer_next ? 1 : -1;
        show_drawer_page(runtime, runtime.match_build_page_ + step);
        return;
    }
    case hud::Control::menu_item: {
        switch (state.hud.sheet) {
        case hud::Sheet::select_menu:
            select_item(runtime, control.index);
            return;
        case hud::Sheet::speed:
            if (!watcher)
                runtime.adjust_game_speed(
                    control.index == static_cast<uint8_t>(hud::SpeedItem::faster) ? 1 : -1
                );
            return;
        case hud::Sheet::phone_menu:
            switch (static_cast<hud::PhoneMenuItem>(control.index)) {
            case hud::PhoneMenuItem::game_menu:
                close_sheet(runtime, false);
                runtime.press_match_key(SDLK_F2, SDL_KMOD_NONE);
                return;
            case hud::PhoneMenuItem::slower:
                if (!watcher)
                    runtime.adjust_game_speed(-1);
                return;
            case hud::PhoneMenuItem::faster:
                if (!watcher)
                    runtime.adjust_game_speed(1);
                return;
            case hud::PhoneMenuItem::chat:
                close_sheet(runtime, false);
                runtime.open_chat_line();
                return;
            }
            return;
        case hud::Sheet::none:
        case hud::Sheet::more:
        case hud::Sheet::drawer:
            return;
        }
        return;
    }
    case hud::Control::more_item:
        if (control.index == static_cast<uint8_t>(hud::MoreItem::info)) {
            close_sheet(runtime, true);
            show_unit_info(runtime);
        } else {
            show_tip(
                runtime,
                std::string(kSelfDestructHint),
                static_cast<float>(control.rect.x + control.rect.width / 2),
                static_cast<float>(control.rect.y),
                now
            );
        }
        return;
    case hud::Control::radial_item:
        radial_pick(runtime, static_cast<hud::RadialItem>(control.index));
        return;
    case hud::Control::radial_hub:
        if (state.hud.radial)
            state.hud.radial->queue_hub = !state.hud.radial->queue_hub;
        return;
    }
}

bool TouchDispatchAccess::control_hold(
    Runtime& runtime, const hud::ControlRect& control, uint64_t now
) {
    switch (control.control) {
    case hud::Control::queue:
    case hud::Control::add:
    case hud::Control::times_five:
        // A held latch is active while the finger rests; its lift decides.
        runtime.play_haptic(Haptic::hold_started);
        return false;
    case hud::Control::force:
        // FORCE is held while the finger rests; its lift lets go.
        return false;
    case hud::Control::group_chip:
    case hud::Control::group_wedge:
        runtime.play_haptic(Haptic::hold_started);
        runtime.assign_squad(control.index);
        return true;
    case hud::Control::build_wedge:
        return build_wedge_hold(runtime, control);
    case hud::Control::more_item:
        // SELF-DESTRUCT · HOLD runs its own timer.
        if (control.index == static_cast<uint8_t>(hud::MoreItem::self_destruct))
            return false;
        break;
    case hud::Control::clear:
    case hud::Control::select_menu:
    case hud::Control::group_store:
    case hud::Control::pause:
    case hud::Control::speed:
    case hud::Control::chat:
    case hud::Control::centre:
    case hud::Control::follow:
    case hud::Control::next_unit:
    case hud::Control::info:
    case hud::Control::menu:
    case hud::Control::build_drawer:
    case hud::Control::order_slot:
    case hud::Control::more:
        break;
    default:
        // Menu items, wedges, the drawer's header, ✕ and the zoom
        // buttons have no hold of their own: the lift still taps.
        return false;
    }
    runtime.play_haptic(Haptic::hold_started);
    // Once a pad was used, the help names its input too.
    const auto& pad = runtime.touch_state().hud.pad;
    show_tip(
        runtime,
        pad.badges ? hud::control_help_with_pad(control.control, control.index, pad.map, pad.glyphs)
                   : std::string(hud::control_help(control.control, control.index)),
        static_cast<float>(control.rect.x + control.rect.width / 2),
        static_cast<float>(control.rect.y),
        now
    );
    return true;
}

void TouchDispatchAccess::build_wedge_tap(
    Runtime& runtime, const hud::ControlRect& control, uint64_t now
) {
    const auto& ring = runtime.touch_state().hud.build_ring;
    if (!ring || control.index >= hud::build_ring_slot_count)
        return;
    const hud::BuildWedge wedge = ring->wedges[control.index];
    if (!wedge.available)
        return;
    switch (wedge.kind) {
    case hud::BuildWedgeKind::empty:
        return;
    case hud::BuildWedgeKind::info:
        show_unit_info(runtime);
        return;
    case hud::BuildWedgeKind::self_destruct:
        // The pad's hold gives it; a finger's tap says how.
        show_tip(
            runtime,
            std::string(kSelfDestructHint),
            static_cast<float>(control.rect.x + control.rect.width / 2),
            static_cast<float>(control.rect.y),
            now
        );
        return;
    case hud::BuildWedgeKind::build:
    case hud::BuildWedgeKind::prev:
    case hud::BuildWedgeKind::next:
    case hud::BuildWedgeKind::fire_orders:
    case hud::BuildWedgeKind::move_orders:
    case hud::BuildWedgeKind::on_off:
    case hud::BuildWedgeKind::cloak:
        break;
    }
    if (wedge.gadget < 0 || !runtime.match_hud_ ||
        static_cast<std::size_t>(wedge.gadget) >= runtime.match_hud_->layout.gadgets.size())
        return;
    // A press on the wedge is a press on its gadget, as the side panel's.
    runtime.activate_match_hud(static_cast<std::size_t>(wedge.gadget), true);
    if (wedge.kind == hud::BuildWedgeKind::build) {
        auto& look = runtime.touch_state().hud;
        look.latches.used(hud::ActionClass::build_button, look.latch_mode);
        runtime.refresh_pointer_modifiers();
    }
}

bool TouchDispatchAccess::build_wedge_hold(Runtime& runtime, const hud::ControlRect& control) {
    const auto& ring = runtime.touch_state().hud.build_ring;
    if (!ring || control.index >= hud::build_ring_slot_count)
        return false;
    const hud::BuildWedge wedge = ring->wedges[control.index];
    if (!wedge.available || wedge.kind != hud::BuildWedgeKind::build || wedge.gadget < 0 ||
        !runtime.match_hud_ ||
        static_cast<std::size_t>(wedge.gadget) >= runtime.match_hud_->layout.gadgets.size())
        return false;
    // A build picture's hold is its right button, as on the side panel: one off the queue
    // (five with x5).
    runtime.activate_match_hud(static_cast<std::size_t>(wedge.gadget), false);
    runtime.play_haptic(Haptic::queue_reduced);
    auto& look = runtime.touch_state().hud;
    look.latches.used(hud::ActionClass::build_button, look.latch_mode);
    runtime.refresh_pointer_modifiers();
    return true;
}

void TouchDispatchAccess::show_tip(
    Runtime& runtime, std::string text, float x, float y, uint64_t now
) {
    auto& tip = runtime.touch_state().hud.tip;
    if (text.empty())
        return;
    tip.text = std::move(text);
    tip.anchor = {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))};
    tip.until_ms = now / kNanosecondsPerMillisecond + kTipMilliseconds;
}

void TouchDispatchAccess::hud_hold(Runtime& runtime, std::size_t slot, uint64_t now) {
    auto& finger = runtime.touch_state().dispatch.fingers[slot];
    finger.held = true;
    const int16_t gadget = finger.gadget;
    const bool build_button = finger.build_button;
    const float x = finger.press_x;
    const float y = finger.press_y;
    // The coming release acts on nothing.
    runtime.match_hud_held_.reset();
    if (gadget < 0 || !runtime.match_hud_ ||
        static_cast<std::size_t>(gadget) >= runtime.match_hud_->layout.gadgets.size()) {
        runtime.play_haptic(Haptic::hold_started);
        return;
    }
    if (build_button) {
        // A build button's hold is its right button: one off the queue
        // (five with x5), or a mobile builder's placement.
        runtime.activate_match_hud(static_cast<std::size_t>(gadget), false);
        runtime.play_haptic(Haptic::queue_reduced);
        auto& look = runtime.touch_state().hud;
        look.latches.used(hud::ActionClass::build_button, look.latch_mode);
        runtime.refresh_pointer_modifiers();
        return;
    }
    runtime.play_haptic(Haptic::hold_started);
    // The gadget's own help, else the touch controls' line for an order panel gadget (the
    // game's GUI files give the in-game panels no help text).
    const auto& common =
        runtime.match_hud_->layout.gadgets[static_cast<std::size_t>(gadget)].common;
    std::string help = common.runtime_help;
    if (help.empty())
        help = std::string(hud::gadget_help(common.name));
    show_tip(runtime, std::move(help), x, y, now);
}

std::optional<std::array<float, 2>>
TouchDispatchAccess::on_own_gadget(Runtime& runtime, const ClaimedFinger& finger) {
    if (finger.gadget < 0)
        return std::nullopt;
    std::size_t found = 0;
    const auto point =
        nearest_gadget(runtime, finger.x, finger.y, finger.target == TouchTarget::frontend, &found);
    if (!point || found != static_cast<std::size_t>(finger.gadget))
        return std::nullopt;
    return point;
}

uint8_t
TouchDispatchAccess::tap_clicks(Runtime& runtime, const ClaimedFinger& finger, uint64_t now) {
    auto& dispatch = runtime.touch_state().dispatch;
    if (finger.moved) {
        dispatch.last_tap_ns = 0;
        return 1;
    }
    const gestures::Thresholds rules{};
    const float near = points_to_pixels(runtime, rules.double_tap_points);
    const uint64_t window = static_cast<uint64_t>(rules.double_tap_ms) * kNanosecondsPerMillisecond;
    uint8_t clicks = 1;
    // The second landing within the window of the first lift, near it.
    if (dispatch.last_tap_ns != 0 && dispatch.last_tap_clicks == 1 &&
        finger.down_ns >= dispatch.last_tap_ns && finger.down_ns - dispatch.last_tap_ns <= window &&
        std::hypot(finger.press_x - dispatch.last_tap_x, finger.press_y - dispatch.last_tap_y) <=
            near)
        clicks = 2;
    dispatch.last_tap_ns = now;
    dispatch.last_tap_x = finger.press_x;
    dispatch.last_tap_y = finger.press_y;
    dispatch.last_tap_clicks = clicks;
    return clicks;
}

void TouchDispatchAccess::hud_release(
    Runtime& runtime, const ClaimedFinger& finger, bool cancelled, uint64_t now
) {
    if (finger.self_destruct) {
        // A tap says how; the click came after a whole hold.
        if (!cancelled && !finger.fired && !finger.moved)
            show_tip(runtime, std::string(kSelfDestructHint), finger.press_x, finger.press_y, now);
        return;
    }
    if (!finger.pressed)
        return;
    if (cancelled || finger.held) {
        cancel_hud_press(runtime, finger);
        return;
    }
    // Where it pressed, or where it lies on its gadget; slid off, nothing.
    std::optional<std::array<float, 2>> point;
    if (!finger.moved)
        point = std::array<float, 2>{finger.press_x, finger.press_y};
    else
        point = on_own_gadget(runtime, finger);
    if (!point) {
        cancel_hud_press(runtime, finger);
        return;
    }
    const uint8_t clicks = tap_clicks(runtime, finger, now);
    auto& state = runtime.touch_state();
    const auto sheet = state.hud.sheet;
    const auto command_before = runtime.match_command_;
    if ((*point)[0] != finger.sent_x || (*point)[1] != finger.sent_y)
        send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, (*point)[0], (*point)[1], 0);
    send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_UP, (*point)[0], (*point)[1], clicks);
    auto& look = runtime.touch_state().hud;
    if (finger.build_button) {
        look.latches.used(hud::ActionClass::build_button, look.latch_mode);
        runtime.refresh_pointer_modifiers();
    }
    // A building tile chosen in the drawer starts placement from the page
    // it loaded: the drawer only closes. An order armed from MORE closes
    // it the same way, so the next tap gives the order.
    if (sheet == hud::Sheet::drawer && look.sheet == hud::Sheet::drawer &&
        runtime.pending_build_type_ != 0)
        look.sheet = hud::Sheet::none;
    if (sheet == hud::Sheet::more && look.sheet == hud::Sheet::more &&
        runtime.match_command_ != MatchCommand::none && runtime.match_command_ != command_before)
        look.sheet = hud::Sheet::none;
}

void TouchDispatchAccess::cancel_hud_press(Runtime& runtime, const ClaimedFinger& finger) {
    // The held button is let go first, so the release acts on nothing.
    runtime.match_hud_held_.reset();
    bool on_gadget = false;
    std::ignore = hud_gadget_at(runtime, finger.press_x, finger.press_y, &on_gadget);
    if (on_gadget || runtime.match_paused_ ||
        (!runtime.battlefield_contains(finger.press_x, finger.press_y) &&
         !runtime.radar_contains(finger.press_x, finger.press_y))) {
        send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_UP, finger.press_x, finger.press_y, 1);
        return;
    }
    // A release there would be a battlefield click: the button is let go
    // without one.
    if (runtime.match_)
        runtime.match_->state().game.pointer_state[2] &= ~input::pointer_key_left;
    runtime.selected_ = -1;
}

void TouchDispatchAccess::frontend_release(
    Runtime& runtime, const ClaimedFinger& finger, bool cancelled, uint64_t now
) {
    if (!finger.pressed)
        return;
    std::array<float, 2> point{finger.press_x, finger.press_y};
    if (finger.moved)
        point = on_own_gadget(runtime, finger).value_or(std::array<float, 2>{finger.x, finger.y});
    if (cancelled) {
        // Nothing is chosen: the press is forgotten before the release.
        runtime.selected_ = -1;
        send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_UP, point[0], point[1], 1);
        return;
    }
    const uint8_t clicks = tap_clicks(runtime, finger, now);
    if (point[0] != finger.sent_x || point[1] != finger.sent_y)
        send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, point[0], point[1], 0);
    send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_UP, point[0], point[1], clicks);
}

void TouchDispatchAccess::hold_timers(Runtime& runtime, uint64_t now) {
    constexpr uint64_t hold_ns =
        static_cast<uint64_t>(hud::self_destruct_hold_ms) * kNanosecondsPerMillisecond;
    auto& state = runtime.touch_state();
    for (std::size_t slot = 0; slot < state.dispatch.fingers.size(); ++slot) {
        auto& finger = runtime.touch_state().dispatch.fingers[slot];
        if (finger.target == TouchTarget::none || finger.slid_off || finger.fired ||
            now < finger.down_ns)
            continue;
        const uint64_t held = now - finger.down_ns;
        if (finger.target == TouchTarget::hud_gadget && finger.self_destruct) {
            if (finger.moved || held < hold_ns)
                continue;
            // A whole hold on a self-destruct gadget clicks it.
            finger.fired = true;
            runtime.play_haptic(Haptic::hold_started);
            send_click(runtime, finger.press_x, finger.press_y, 1);
            continue;
        }
        if (finger.target == TouchTarget::control &&
            finger.control.control == hud::Control::more_item &&
            finger.control.index == static_cast<uint8_t>(hud::MoreItem::self_destruct)) {
            const float progress =
                std::clamp(static_cast<float>(held) / static_cast<float>(hold_ns), 0.0F, 1.0F);
            state.hud.self_destruct_progress = progress;
            if (progress < 1.0F)
                continue;
            finger.fired = true;
            runtime.play_haptic(Haptic::hold_started);
            runtime.press_match_key(SDLK_D, SDL_KMOD_LCTRL);
        }
    }
}

hud::TapAction TouchDispatchAccess::tap_action_of(uint8_t cursor) noexcept {
    switch (static_cast<input::OrderCursor>(cursor)) {
    case input::OrderCursor::attack:
    case input::OrderCursor::attack_dropped:
    case input::OrderCursor::attack_out_of_range:
        return hud::TapAction::attack;
    case input::OrderCursor::capture:
        return hud::TapAction::capture;
    case input::OrderCursor::guard:
        return hud::TapAction::guard;
    case input::OrderCursor::repair:
    case input::OrderCursor::resurrect:
        return hud::TapAction::repair;
    case input::OrderCursor::patrol:
        return hud::TapAction::patrol;
    case input::OrderCursor::load_by_air:
    case input::OrderCursor::load:
        return hud::TapAction::load;
    case input::OrderCursor::teleport:
    case input::OrderCursor::move:
        return hud::TapAction::move;
    case input::OrderCursor::reclaim:
        return hud::TapAction::reclaim;
    case input::OrderCursor::unload:
        return hud::TapAction::unload;
    case input::OrderCursor::select:
        return hud::TapAction::select;
    case input::OrderCursor::build:
        return hud::TapAction::build;
    case input::OrderCursor::enemy:
    case input::OrderCursor::friendly:
    case input::OrderCursor::normal:
        break;
    }
    return hud::TapAction::none;
}

} // namespace oa::app
