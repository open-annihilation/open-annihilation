// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The gamepad's pointer: the right trackpad, the stick cursor and the gyro
// sent as the pad's own mouse, and the camera the sticks and the left
// trackpad move (docs/controllers.md). The pointer's events go through
// dispatch_event as a mouse's do, so hover, clicks, boxes, placement and
// the screen's edges behave as with a mouse.
#include "oa/app/runtime.hpp"
#include "pad_state.hpp"
#include "touch_state.hpp"
#include "oa/app/frame_pacing.hpp"
#include "oa/app/platform_hooks.hpp"
#include "oa/sim/gameplay_input/input.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/ui/hud/order_panel.hpp"
#include "oa/ui/pad_controls.hpp"
#include "oa/ui/touch_hud.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <optional>

namespace oa::app {
namespace {

namespace pc = oa::ui::pad_controls;
namespace hud = oa::ui::touch_hud;
namespace input = oa::sim::gameplay_input;

/// Nanoseconds in a millisecond.
constexpr uint64_t kNanosecondsPerMillisecond = 1'000'000;
/// Nanoseconds in a second.
constexpr double kNanosecondsPerSecond = 1e9;
/// How far the engine's pointer may lie from the pad's before the pad takes the engine's
/// place (another device moved it), canvas pixels.
constexpr float kPointerAdoptPixels = 0.5F;
/// The zoom's fastest change with the right stick held or the D-pad's zoom held: doublings a
/// second.
constexpr float kZoomDoublingsPerSecond = 1.0F;
/// The stick deflection past which a direction counts in menus and over the side panel.
constexpr float kStepDeflection = 0.5F;
/// How long after the pad last moved the pointer a change of what lies under it gives a detent,
/// nanoseconds.
constexpr uint64_t kDetentWindowNs = 150 * kNanosecondsPerMillisecond;
/// The step the stick cursor's walk over the side panel probes by, canvas pixels.
constexpr float kPanelProbePixels = 4.0F;
/// How far the walk over the side panel looks for the next gadget, canvas pixels.
constexpr float kPanelReachPixels = 240.0F;
/// How far the walk moves the pointer when no gadget lies that way, canvas pixels: it leaves
/// the panel.
constexpr float kPanelLeavePixels = 48.0F;
/// The least distance a unit jump moves the pointer, canvas pixels: a unit under the pointer
/// is not the next one.
constexpr float kJumpLeastPixels = 4.0F;

/// Returns whether an armed command takes a dragged box as an area order.
///
/// @param command the armed command
/// @return whether a box gives an area order
bool area_command(MatchCommand command) noexcept {
    return command == MatchCommand::attack || command == MatchCommand::dgun ||
           command == MatchCommand::reclaim || command == MatchCommand::repair;
}

/// Returns whether two settings configure the pointers alike.
///
/// @param a one
/// @param b the other
/// @return whether every field the pointers read is equal
bool same_pointer_settings(const pc::PadSettings& a, const pc::PadSettings& b) noexcept {
    return a.right_trackpad == b.right_trackpad && a.pointer_speed == b.pointer_speed &&
           a.acceleration == b.acceleration && a.glide == b.glide && a.magnetism == b.magnetism &&
           a.gyro == b.gyro && a.gyro_speed == b.gyro_speed && a.scheme == b.scheme;
}

/// Returns a stick's direction for steps: the axis it leans along most, past the step
/// deflection.
///
/// @param stick the stick, -1..1 each way, y down
/// @param[out] way_x -1, 0 or 1
/// @param[out] way_y -1, 0 or 1
void step_way(pc::Vec2 stick, int8_t& way_x, int8_t& way_y) noexcept {
    way_x = 0;
    way_y = 0;
    if (std::hypot(stick.x, stick.y) < kStepDeflection)
        return;
    if (std::fabs(stick.x) >= std::fabs(stick.y))
        way_x = stick.x < 0.0F ? -1 : 1;
    else
        way_y = stick.y < 0.0F ? -1 : 1;
}

/// Returns the menu action of a stick's step direction.
///
/// @param way_x -1, 0 or 1
/// @param way_y -1, 0 or 1
/// @return the focus action
pc::Action focus_action(int8_t way_x, int8_t way_y) noexcept {
    if (way_x < 0)
        return pc::Action::focus_left;
    if (way_x > 0)
        return pc::Action::focus_right;
    return way_y < 0 ? pc::Action::focus_up : pc::Action::focus_down;
}

/// Runs a held direction's repeated steps: the first at once, then by the menu repeat.
///
/// @param[in,out] steps the held direction
/// @param way_x the direction now, -1, 0 or 1
/// @param way_y the direction now, -1, 0 or 1
/// @param now_ms the time, milliseconds
/// @return the steps due now
uint32_t repeat_steps(PadRepeat& steps, int8_t way_x, int8_t way_y, uint64_t now_ms) noexcept {
    if (way_x == 0 && way_y == 0) {
        steps = {};
        return 0;
    }
    if (way_x != steps.way_x || way_y != steps.way_y) {
        steps = {};
        steps.way_x = way_x;
        steps.way_y = way_y;
        return steps.repeat.press(now_ms);
    }
    return steps.repeat.advance(now_ms);
}

} // namespace

pc::Vec2 PadAccess::canvas(const Runtime& runtime) {
    if (runtime.screen_ == Screen::match && runtime.match_layout_.width > 0 &&
        runtime.match_layout_.height > 0)
        return {
            static_cast<float>(runtime.match_layout_.width),
            static_cast<float>(runtime.match_layout_.height)
        };
    // As apply_output_mode lays a frontend screen out: the frame a panel is
    // drawn over, the end screen's battlefield, else the 640x480 canvas.
    if (const auto* parent = runtime.panel_parent(); parent != nullptr)
        return {static_cast<float>(parent->width), static_cast<float>(parent->height)};
    if (const auto* end = runtime.end_screen_battlefield_size(); end != nullptr)
        return {static_cast<float>(end->width), static_cast<float>(end->height)};
    return {static_cast<float>(kCanvasWidth), static_cast<float>(kCanvasHeight)};
}

void PadAccess::sync_pointer(Runtime& runtime) {
    auto& state = runtime.pad_state();
    const auto size = canvas(runtime);
    // Another device (a mouse, Steam's mouse, a finger, mouse look) moved the
    // engine's pointer: the pad goes on from there.
    if (!state.pointer_known) {
        if (runtime.pointer_x_ == 0.0F && runtime.pointer_y_ == 0.0F) {
            state.pointer_x = size.x / 2.0F;
            state.pointer_y = size.y / 2.0F;
        } else {
            state.pointer_x = runtime.pointer_x_;
            state.pointer_y = runtime.pointer_y_;
        }
        state.pointer_known = true;
    } else if (
        std::fabs(runtime.pointer_x_ - state.pointer_x) > kPointerAdoptPixels ||
        std::fabs(runtime.pointer_y_ - state.pointer_y) > kPointerAdoptPixels
    ) {
        state.pointer_x = runtime.pointer_x_;
        state.pointer_y = runtime.pointer_y_;
    }
    state.pointer_x = std::clamp(state.pointer_x, 0.0F, std::max(0.0F, size.x - 1.0F));
    state.pointer_y = std::clamp(state.pointer_y, 0.0F, std::max(0.0F, size.y - 1.0F));
}

void PadAccess::configure_pointers(Runtime& runtime, const pc::PadSettings& chosen) {
    auto& state = runtime.pad_state();
    const auto size = canvas(runtime);
    pc::Area area{0.0F, 0.0F, size.x, size.y};
    // The absolute right trackpad covers the battlefield on the match.
    if (runtime.screen_ == Screen::match && runtime.match_) {
        const auto& layout = runtime.match_layout_;
        area = {
            static_cast<float>(layout.left),
            static_cast<float>(layout.top),
            static_cast<float>(std::max(1, layout.battlefield_width())),
            static_cast<float>(std::max(1, layout.battlefield_height()))
        };
    }
    const float px_per_point = std::max(runtime.touch_px_per_point(), 0.01F);
    if (state.configured_once && same_pointer_settings(state.configured, chosen) &&
        state.configured_canvas.x == size.x && state.configured_canvas.y == size.y &&
        state.configured_area.x == area.x && state.configured_area.y == area.y &&
        state.configured_area.width == area.width && state.configured_area.height == area.height &&
        state.configured_px_per_point == px_per_point)
        return;
    state.pointer.configure(chosen, size, area);
    // The camera pad drags the map under the thumb, with glide after a flick.
    pc::PadSettings drag = chosen;
    drag.right_trackpad = pc::RightTrackpad::relative;
    drag.acceleration = pc::Acceleration::off;
    drag.pointer_speed = pc::default_pointer_speed;
    drag.glide = true;
    state.drag.configure(drag, size, area);
    state.stick_cursor.configure(chosen, px_per_point);
    state.gyro_pointer.configure(chosen, size);
    state.configured = chosen;
    state.configured_canvas = size;
    state.configured_area = area;
    state.configured_px_per_point = px_per_point;
    state.configured_once = true;
}

void PadAccess::dispatch(Runtime& runtime, SDL_Event& event) {
    // A synthetic event that ends the run reaches the run loop through the
    // event's flag, or from a frame through the exit request.
    bool own_running = true;
    bool* const running = runtime.pad_state().running;
    runtime.dispatch_event(event, running != nullptr ? *running : own_running);
    if (!own_running)
        runtime.exit_requested_ = true;
}

void PadAccess::send_mouse(Runtime& runtime, uint32_t type, uint8_t button, uint8_t clicks) {
    const auto& state = runtime.pad_state();
    SDL_Event event{};
    event.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        event.motion.timestamp = SDL_GetTicksNS();
        event.motion.windowID = 0;
        event.motion.which = pad_mouse_id;
        event.motion.state = (state.left_holders > 0 ? SDL_BUTTON_LMASK : 0U) |
                             (state.right_holders > 0 ? SDL_BUTTON_RMASK : 0U);
        event.motion.x = state.pointer_x;
        event.motion.y = state.pointer_y;
    } else {
        event.button.timestamp = SDL_GetTicksNS();
        event.button.windowID = 0;
        event.button.which = pad_mouse_id;
        event.button.button = button;
        event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.clicks = clicks;
        event.button.x = state.pointer_x;
        event.button.y = state.pointer_y;
    }
    // FORCE is the Ctrl a click is made with: the engine reads a click's Ctrl
    // as the keyboard's (force fire), so it holds as a pulse for the click's
    // events alone and never reaches the keys the pad presses.
    if (type == SDL_EVENT_MOUSE_MOTION || !runtime.pad_force_held()) {
        dispatch(runtime, event);
        return;
    }
    auto& touch = runtime.touch_state().dispatch;
    const SDL_Keymod before = touch.pulse;
    touch.pulse = static_cast<SDL_Keymod>(before | SDL_KMOD_LCTRL);
    try {
        dispatch(runtime, event);
    } catch (...) {
        runtime.touch_state().dispatch.pulse = before;
        throw;
    }
    runtime.touch_state().dispatch.pulse = before;
}

void PadAccess::place_pointer(Runtime& runtime, float x, float y) {
    auto& state = runtime.pad_state();
    const auto size = canvas(runtime);
    x = std::clamp(x, 0.0F, std::max(0.0F, size.x - 1.0F));
    y = std::clamp(y, 0.0F, std::max(0.0F, size.y - 1.0F));
    const bool moved = !state.pointer_known || x != state.pointer_x || y != state.pointer_y ||
                       runtime.pointer_x_ != x || runtime.pointer_y_ != y;
    state.pointer_x = x;
    state.pointer_y = y;
    state.pointer_known = true;
    if (!moved)
        return;
    state.moved_ns = now_ns(runtime);
    // The pad pointer takes the hover back from a finger resting on the
    // battlefield.
    if (runtime.touch_)
        runtime.touch_->dispatch.finger_point.reset();
    send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, 0, 0);
}

void PadAccess::apply_step(Runtime& runtime, const pc::PointerStep& step, bool ticks) {
    if (step.place) {
        sync_pointer(runtime);
        place_pointer(runtime, step.place->x, step.place->y);
    } else if (step.move.x != 0.0F || step.move.y != 0.0F) {
        sync_pointer(runtime);
        const auto& state = runtime.pad_state();
        place_pointer(runtime, state.pointer_x + step.move.x, state.pointer_y + step.move.y);
    }
    if (ticks && step.ticks > 0)
        runtime.play_pad_feel(pc::Feel::pointer_tick);
}

void PadAccess::pointer_button(Runtime& runtime, bool left, bool down, uint64_t now) {
    auto& state = runtime.pad_state();
    uint8_t& holders = left ? state.left_holders : state.right_holders;
    if (down) {
        // R2, A and the pointer pad's click share one left button.
        if (holders++ > 0)
            return;
        sync_pointer(runtime);
        // The engine's pointer goes where the pad's is before the press.
        send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, 0, 0);
        float x = state.pointer_x;
        float y = state.pointer_y;
        const bool battlefield = in_match(runtime) && !runtime.hovered_ &&
                                 runtime.battlefield_contains(x, y) &&
                                 !runtime.placed_hud_covers(x, y);
        const bool on_build = pointer_on_build_button(runtime);
        const bool right_click_interface =
            runtime.match_ && runtime.match_->state().game.interface_type != 0;
        // A click on the battlefield goes to the unit under the pointer, or
        // the nearest within reach; a building being placed goes where the
        // ghost is, and a refused site says so on the pad too.
        if (battlefield && placing(runtime)) {
            if (left)
                if (const auto site = runtime.build_site_under(x, y); !site || !site->legal)
                    runtime.play_haptic(Haptic::site_refused);
        } else if (battlefield && (left || right_click_interface)) {
            const auto point = TouchDispatchAccess::fat_finger_point(runtime, x, y);
            if (point[0] != x || point[1] != y) {
                place_pointer(runtime, point[0], point[1]);
                x = point[0];
                y = point[1];
            }
        }
        if (left) {
            state.left_on_build = on_build;
            state.left_on_battlefield = battlefield;
            state.box_felt = false;
            state.left_class = hud::ActionClass::selection;
            if (battlefield) {
                runtime.refresh_pointer_modifiers();
                const auto cursor = static_cast<input::OrderCursor>(runtime.pick_match_cursor());
                state.left_class = cursor == input::OrderCursor::select &&
                                           runtime.match_command_ == MatchCommand::none
                                       ? hud::ActionClass::selection
                                       : hud::ActionClass::order;
            }
            state.left_clicks = state.double_click.press(
                now / kNanosecondsPerMillisecond,
                pc::Vec2{x, y},
                std::max(runtime.touch_px_per_point(), 0.01F)
            );
            send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, state.left_clicks);
        } else {
            state.right_on_build = on_build;
            send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT, 1);
        }
        return;
    }
    // The button comes up with the last pad button holding it; a screen
    // change in between let go of it already.
    if (holders == 0 || --holders > 0)
        return;
    if (left) {
        const bool box = runtime.match_drag_ && !runtime.match_drag_is_click();
        const bool area = box && area_command(runtime.match_command_);
        const bool on_build = state.left_on_build;
        const bool battlefield = state.left_on_battlefield;
        const auto action_class = state.left_class;
        send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT, state.left_clicks);
        runtime.pad_state().box_felt = false;
        // The click, box or build button used its latch, as a tap does.
        if (on_build)
            use_latch(runtime, hud::ActionClass::build_button);
        else if (box)
            use_latch(runtime, area ? hud::ActionClass::order : hud::ActionClass::selection);
        else if (battlefield)
            use_latch(runtime, action_class);
        return;
    }
    const bool on_build = state.right_on_build;
    send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_RIGHT, 1);
    // A build button's right click took one (or five) off its factory's
    // queue.
    if (on_build && runtime.screen_ == Screen::match && !runtime.match_paused_) {
        runtime.play_haptic(Haptic::queue_reduced);
        use_latch(runtime, hud::ActionClass::build_button);
    }
}

bool PadAccess::pointer_on_build_button(const Runtime& runtime) {
    if (runtime.screen_ != Screen::match || runtime.match_paused_ || !runtime.match_hud_ ||
        !runtime.hovered_)
        return false;
    const auto& gadgets = runtime.match_hud_->layout.gadgets;
    if (*runtime.hovered_ >= gadgets.size())
        return false;
    constexpr uint8_t build_buttons =
        oa::ui::hud::kCommonUnitButton | oa::ui::hud::kCommonWeaponButton;
    return (static_cast<uint8_t>(gadgets[*runtime.hovered_].common.common_attributes) &
            build_buttons) != 0;
}

void PadAccess::take_touchpad(Runtime& runtime, const SDL_GamepadTouchpadEvent& event) {
    // A pad with one touchpad (or none) is played with its sticks.
    const auto context = map_context(runtime);
    if (!context.trackpads || event.touchpad < 0 ||
        event.touchpad >= static_cast<int32_t>(pad_trackpad_count))
        return;
    auto& state = runtime.pad_state();
    const auto side = event.touchpad == 0 ? pc::Side::left : pc::Side::right;
    const bool pointer_pad = side == physical_side(runtime, pc::Side::right);
    const pc::Vec2 at{std::clamp(event.x, 0.0F, 1.0F), std::clamp(event.y, 0.0F, 1.0F)};
    const uint64_t now = now_ns(runtime);
    configure_pointers(runtime, runtime.pad_settings());
    if (pointer_pad) {
        switch (event.type) {
        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
            state.pointer.touch_down(at, now);
            if (runtime.touch_)
                runtime.touch_->dispatch.finger_point.reset();
            return;
        case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION: {
            const auto step = state.pointer.touch_move(at, now);
            // While a ring is open the thumb aims and the pointer stays where
            // the ring opened.
            if (state.ring.kind != PadRingKind::none)
                aim_ring(runtime, now);
            else
                apply_step(runtime, step, true);
            return;
        }
        default:
            state.pointer.touch_up(now);
            return;
        }
    }
    const bool on_match = in_match(runtime);
    const bool groups = on_match && held(runtime).groups;
    const auto aim_group = [&]() {
        auto& pad = runtime.pad_state();
        const auto before = pad.group_aimed;
        const auto slot =
            pad.group_aim.aim({(at.x - 0.5F) * 2.0F, (at.y - 0.5F) * 2.0F}, pc::pad_ring_dead_zone);
        if (slot)
            pad.group_aimed = static_cast<uint8_t>(*slot + 1);
        if (pad.group_aimed && pad.group_aimed != before)
            runtime.play_pad_feel(pc::Feel::wedge_change);
    };
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
        state.drag.touch_down(at, now);
        if (state.minimap)
            minimap(runtime, true);
        else if (groups)
            aim_group();
        return;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION: {
        const auto step = state.drag.touch_move(at, now);
        if (state.minimap) {
            minimap(runtime, true);
        } else if (groups) {
            aim_group();
        } else if (on_match && (step.move.x != 0.0F || step.move.y != 0.0F)) {
            // The ground follows the thumb.
            runtime.pan_match_camera_by(-step.move.x, -step.move.y);
            if (runtime.match_drag_ && runtime.pad_state().left_holders > 0) {
                runtime.match_pointer_x_ = runtime.pad_state().pointer_x;
                runtime.match_pointer_y_ = runtime.pad_state().pointer_y;
                runtime.track_match_drag();
            }
        }
        return;
    }
    default:
        state.drag.touch_up(now);
        // The minimap and the group ring leave no glide behind.
        if (state.minimap || groups)
            state.drag.stop();
        return;
    }
}

void PadAccess::take_sensor(Runtime& runtime, const SDL_GamepadSensorEvent& event) {
    if (event.sensor != SDL_SENSOR_GYRO)
        return;
    const auto chosen = runtime.pad_settings();
    auto& state = runtime.pad_state();
    if (chosen.gyro == pc::Gyro::off)
        return;
    configure_pointers(runtime, chosen);
    // Gated by a touch, as Steam Input's gyro on touch is, so it does not
    // drift.
    bool gate = false;
    switch (chosen.gyro) {
    case pc::Gyro::off:
        return;
    case pc::Gyro::right_pad_touched:
        gate = state.pointer.touched();
        break;
    case pc::Gyro::right_stick_touched: {
        const auto touch =
            static_cast<std::size_t>(physical(runtime, pc::PadButton::right_stick_touch));
        gate = touch < state.down.size() && state.down[touch];
        break;
    }
    case pc::Gyro::always:
        gate = true;
        break;
    }
    // The pointer stays where a ring opened.
    if (state.ring.kind != PadRingKind::none)
        gate = false;
    const auto step =
        state.gyro_pointer.advance(event.data[0], event.data[1], gate, now_ns(runtime));
    apply_step(runtime, step, true);
}

void PadAccess::tick_pointer(
    Runtime& runtime, const pc::PadSettings& chosen, uint64_t now, uint64_t elapsed_ns
) {
    auto& state = runtime.pad_state();
    const bool on_match = in_match(runtime);
    const bool ring_open = state.ring.kind != PadRingKind::none;
    const uint64_t now_ms = now / kNanosecondsPerMillisecond;
    const double seconds = static_cast<double>(elapsed_ns) / kNanosecondsPerSecond;
    // Glides: the pointer coasts after a flick, as does the dragged map.
    const auto glide = state.pointer.advance(now);
    if (!ring_open)
        apply_step(runtime, glide, true);
    const auto drift = runtime.pad_state().drag.advance(now);
    if (on_match && !runtime.pad_state().minimap && (drift.move.x != 0.0F || drift.move.y != 0.0F))
        runtime.pan_match_camera_by(-drift.move.x, -drift.move.y);
    // The group ring forgets its aim once the groups layer is let go.
    if (!on_match || !held(runtime).groups) {
        runtime.pad_state().group_aim.reset();
        runtime.pad_state().group_aimed.reset();
    }
    const pc::Vec2 pan_stick = stick(runtime, pc::Side::left);
    const pc::Vec2 point_stick = stick(runtime, pc::Side::right);
    const bool pointer_stick = stick_is_pointer(runtime, chosen);
    const auto speed = runtime.match_ != nullptr ? runtime.match_->state().game.scroll_speed
                                                 : runtime.preferences_.scroll_speed;
    const auto zoom_by = [&](float doublings) {
        if (doublings == 0.0F || !runtime.engine_settings().wheel_zoom)
            return;
        sync_pointer(runtime);
        runtime.zoom_match_about(
            std::exp2(doublings), runtime.pad_state().pointer_x, runtime.pad_state().pointer_y
        );
    };
    const auto stretch_box = [&runtime]() {
        // A box held by R2 stretches with the camera: its far corner is the
        // ground now under the pointer.
        auto& pad = runtime.pad_state();
        if (runtime.match_drag_ && pad.left_holders > 0) {
            runtime.match_pointer_x_ = pad.pointer_x;
            runtime.match_pointer_y_ = pad.pointer_y;
            runtime.track_match_drag();
        }
    };
    if (on_match) {
        // The left stick pans: the keyboard's scroll rate at full deflection,
        // growing with the square of the deflection; the pan divides by the
        // zoom, as the keys' scroll does.
        if (const float deflection = pc::stick_deflection(pan_stick);
            deflection > 0.0F && elapsed_ns > 0) {
            const float length = std::hypot(pan_stick.x, pan_stick.y);
            const auto step = static_cast<float>(frame_pacing::scroll_distance(speed, elapsed_ns)) *
                              deflection * deflection;
            runtime.pan_match_camera_by(pan_stick.x / length * step, pan_stick.y / length * step);
            stretch_box();
        }
        if (ring_open) {
            // The right stick aims the ring (aim_ring).
        } else if (pointer_stick) {
            // R3 held, whose FOLLOW waits for its release, makes the stick
            // jump instead of glide.
            bool jumping = false;
            for (std::size_t index = 0; index < state.down.size(); ++index)
                jumping = jumping || (state.down[index] && state.deferred[index] &&
                                      state.pressed[index].action == pc::Action::follow);
            sync_pointer(runtime);
            const bool over_panel =
                runtime.pad_state().pointer_x < static_cast<float>(runtime.match_layout_.left) &&
                !runtime.radar_contains(
                    runtime.pad_state().pointer_x, runtime.pad_state().pointer_y
                );
            if (jumping) {
                // R3 held: a flick jumps the pointer to the nearest visible
                // unit that way.
                runtime.pad_state().stick_cursor.reset();
                const auto flick = runtime.pad_state().flick.update(point_stick);
                if (flick != pc::Flick::none) {
                    const pc::Vec2 way{
                        flick == pc::Flick::left    ? -1.0F
                        : flick == pc::Flick::right ? 1.0F
                                                    : 0.0F,
                        flick == pc::Flick::up     ? -1.0F
                        : flick == pc::Flick::down ? 1.0F
                                                   : 0.0F
                    };
                    const auto& pad = runtime.pad_state();
                    std::optional<pc::Vec2> best;
                    float best_distance = 0.0F;
                    for (const auto& target : pad.targets) {
                        const float dx = target.x - pad.pointer_x;
                        const float dy = target.y - pad.pointer_y;
                        const float along = dx * way.x + dy * way.y;
                        const float across = std::fabs(dx * way.y - dy * way.x);
                        if (along < kJumpLeastPixels || across > along)
                            continue;
                        const float distance = std::hypot(dx, dy);
                        if (!best || distance < best_distance) {
                            best = target;
                            best_distance = distance;
                        }
                    }
                    if (best) {
                        place_pointer(runtime, best->x, best->y);
                        runtime.play_pad_feel(pc::Feel::target_detent);
                        runtime.pad_state().jumped = true;
                    }
                }
            } else if (over_panel) {
                // Over the side panel the stick steps from gadget to gadget.
                runtime.pad_state().stick_cursor.reset();
                int8_t way_x = 0;
                int8_t way_y = 0;
                step_way(point_stick, way_x, way_y);
                for (uint32_t steps = repeat_steps(runtime.pad_state().panel, way_x, way_y, now_ms);
                     steps > 0;
                     --steps) {
                    auto& pad = runtime.pad_state();
                    const float x = pad.pointer_x;
                    const float y = pad.pointer_y;
                    const auto current = TouchDispatchAccess::hud_gadget_at(runtime, x, y);
                    std::optional<float> enter;
                    float exit = 0.0F;
                    std::optional<std::size_t> found;
                    for (float distance = kPanelProbePixels; distance <= kPanelReachPixels;
                         distance += kPanelProbePixels) {
                        const auto at = TouchDispatchAccess::hud_gadget_at(
                            runtime, x + way_x * distance, y + way_y * distance
                        );
                        if (!enter) {
                            if (at && at != current) {
                                enter = distance;
                                exit = distance;
                                found = at;
                            }
                        } else if (at == found) {
                            exit = distance;
                        } else {
                            break;
                        }
                    }
                    if (enter) {
                        const float middle = (*enter + exit) / 2.0F;
                        place_pointer(runtime, x + way_x * middle, y + way_y * middle);
                        runtime.play_pad_feel(pc::Feel::target_detent);
                    } else {
                        place_pointer(
                            runtime, x + way_x * kPanelLeavePixels, y + way_y * kPanelLeavePixels
                        );
                    }
                }
            } else {
                runtime.pad_state().panel = {};
                // The stick cursor: friction near units and magnetism onto a
                // lone one, except with R2 held, while placing or over the HUD.
                auto& pad = runtime.pad_state();
                const bool blocked = pad.left_holders > 0 || placing(runtime) || runtime.hovered_ ||
                                     !runtime.battlefield_contains(pad.pointer_x, pad.pointer_y);
                const auto step = pad.stick_cursor.advance(
                    point_stick, {pad.pointer_x, pad.pointer_y}, pad.targets, blocked, now
                );
                apply_step(runtime, step, false);
            }
        } else if (chosen.right_stick == pc::RightStick::zoom_and_pages) {
            // Up and down zoom about the pointer; a flick across turns the
            // builder's page.
            if (std::fabs(point_stick.y) >= std::fabs(point_stick.x) && elapsed_ns > 0) {
                const float deflection = pc::stick_deflection({0.0F, point_stick.y});
                if (deflection > 0.0F)
                    zoom_by(
                        (point_stick.y < 0.0F ? 1.0F : -1.0F) * deflection *
                        static_cast<float>(seconds) * kZoomDoublingsPerSecond
                    );
            }
            const auto flick = runtime.pad_state().flick.update(point_stick);
            const auto* definition = runtime.selected_match_unit_ != 0
                                         ? runtime.definition_for(runtime.selected_match_unit_)
                                         : nullptr;
            if ((flick == pc::Flick::left || flick == pc::Flick::right) && definition != nullptr &&
                definition->builder)
                runtime.press_match_action(
                    flick == pc::Flick::left ? "build.page-previous" : "build.page-next"
                );
        }
        // The D-pad's held zoom (sticks scheme).
        if (elapsed_ns > 0) {
            const float zoom_way = (action_held(runtime, pc::Action::zoom_in) ? 1.0F : 0.0F) -
                                   (action_held(runtime, pc::Action::zoom_out) ? 1.0F : 0.0F);
            zoom_by(zoom_way * static_cast<float>(seconds) * kZoomDoublingsPerSecond);
        }
        auto& pad = runtime.pad_state();
        // Starting a box gives a short feel.
        if (pad.left_holders > 0 && !pad.box_felt && runtime.match_drag_ &&
            !runtime.match_drag_is_click() &&
            (runtime.match_drag_->start[0] != runtime.match_drag_->end[0] ||
             runtime.match_drag_->start[2] != runtime.match_drag_->end[2])) {
            pad.box_felt = true;
            runtime.play_haptic(Haptic::box_started);
        }
    } else {
        // Menus: the left stick steps the focus as the D-pad does, the right
        // stick is the wheel, or the pointer in the sticks scheme.
        int8_t way_x = 0;
        int8_t way_y = 0;
        step_way(pan_stick, way_x, way_y);
        for (uint32_t steps = repeat_steps(state.stick_steps, way_x, way_y, now_ms); steps > 0;
             --steps)
            menu_step(runtime, focus_action(way_x, way_y));
        if (pointer_stick && !ring_open) {
            sync_pointer(runtime);
            auto& pad = runtime.pad_state();
            const auto step = pad.stick_cursor.advance(
                point_stick, {pad.pointer_x, pad.pointer_y}, {}, true, now
            );
            apply_step(runtime, step, false);
        } else if (!pointer_stick && chosen.right_stick != pc::RightStick::nothing) {
            const int8_t wheel_way =
                point_stick.y < -kStepDeflection ? -1 : (point_stick.y > kStepDeflection ? 1 : 0);
            for (uint32_t steps = repeat_steps(runtime.pad_state().wheel, 0, wheel_way, now_ms);
                 steps > 0;
                 --steps)
                menu_step(runtime, wheel_way < 0 ? pc::Action::wheel_up : pc::Action::wheel_down);
        }
    }
    // A detent when what lies under the pointer changes while the pad moves
    // it: ground to a unit, or the cursor between select and an order.
    auto& pad = runtime.pad_state();
    const uint8_t cursor = runtime.cursor_index_;
    const uint16_t unit = runtime.hovered_match_unit_;
    if (on_match && pad.seen_once && pad.moved_ns != 0 && now >= pad.moved_ns &&
        now - pad.moved_ns <= kDetentWindowNs &&
        (cursor != pad.cursor_seen || unit != pad.unit_seen))
        runtime.play_pad_feel(pc::Feel::target_detent);
    pad.cursor_seen = cursor;
    pad.unit_seen = unit;
    pad.seen_once = true;
}

void PadAccess::minimap(Runtime& runtime, bool on) {
    auto& state = runtime.pad_state();
    state.minimap = on;
    if (!on || !state.drag.touched() || !in_match(runtime))
        return;
    // The pad is the whole map: the camera goes to the point under the
    // thumb and follows it while the pad is held.
    const auto& radar = runtime.radar_picture_;
    if (radar.width <= 0 || radar.height <= 0)
        return;
    const auto point = pc::pad_to_area(
        state.drag.touch(),
        pc::Area{
            static_cast<float>(radar.x),
            static_cast<float>(radar.y),
            static_cast<float>(radar.width),
            static_cast<float>(radar.height)
        }
    );
    runtime.center_camera_on_radar_point(point.x, point.y);
}

void PadAccess::refresh_targets(Runtime& runtime) {
    auto& state = runtime.pad_state();
    state.targets.clear();
    if (!in_match(runtime) || !stick_is_pointer(runtime, runtime.pad_settings()))
        return;
    const auto& world = runtime.match_->state();
    const auto listed = static_cast<std::size_t>(std::max<int32_t>(world.game.hot_unit_count, 0));
    const std::size_t count = std::min(listed, runtime.on_screen_units_.size());
    const auto viewport = runtime.live_viewport(runtime.match_camera_x_, runtime.match_camera_z_);
    const auto& slots = runtime.match_->world().slots;
    for (std::size_t index = 0; index < count; ++index) {
        const uint16_t id = runtime.on_screen_units_[index];
        if (id == 0 || id >= slots.size() || slots[id].unit == nullptr)
            continue;
        const auto point = runtime.project_match_point(viewport, slots[id].unit->position);
        const auto x = static_cast<float>(point.x);
        const auto y = static_cast<float>(point.y);
        if (runtime.battlefield_contains(x, y))
            state.targets.push_back({x, y});
    }
}

bool PadAccess::stick_is_pointer(const Runtime& runtime, const pc::PadSettings& chosen) {
    return map_context(runtime).scheme == pc::Scheme::sticks ||
           chosen.right_stick == pc::RightStick::pointer;
}

pc::Vec2 PadAccess::stick(const Runtime& runtime, pc::Side role) {
    const Runtime::PadState* state = runtime.pad_state_if_made();
    if (state == nullptr)
        return {};
    return state->sticks[static_cast<std::size_t>(physical_side(runtime, role))];
}

hud::Viewport PadAccess::ring_viewport(Runtime& runtime) {
    auto& touch = runtime.touch_state();
    if (touch.viewport.width > 0 && touch.viewport.height > 0)
        return touch.viewport;
    // Before the touch layer's first frame: its viewport as it lays it out.
    const auto& layout = runtime.match_layout_;
    hud::Viewport viewport{};
    viewport.width = layout.width;
    viewport.height = layout.height;
    viewport.px_per_point =
        (layout.px_per_point > 0.0 ? static_cast<float>(layout.px_per_point) : 1.0F) *
        runtime.touch_control_scale();
    viewport.safe = layout.safe;
    viewport.device =
        runtime.touch_phone_class() ? hud::DeviceClass::phone : hud::DeviceClass::tablet;
    viewport.left_handed = runtime.engine_settings().touch_left_handed;
    viewport.chrome = layout;
    touch.viewport = viewport;
    return viewport;
}

} // namespace oa::app
