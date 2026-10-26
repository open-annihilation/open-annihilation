// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The touch controls' dispatcher: finger events claimed by what they land on
// and turned into gestures and the engine's own actions, the touch state,
// whether touch controls are on, the cursor they draw, the window's density
// and class in points, the Control size, and the frame the slim pad HUD
// shows once a gamepad sent input (docs/touch-controls.md).
#include "oa/app/runtime.hpp"
#include "engine_settings_state.hpp"
#include "oa_layer.hpp"
#include "touch_state.hpp"
#include "oa/app/extension.hpp"
#include "oa/app/input_hints.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/gui_input.hpp"
#include "oa/ui/hud/order_panel.hpp"
#include "oa/ui/touch_gestures.hpp"
#include "oa/ui/touch_hud.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
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

/// Nanoseconds in a millisecond.
constexpr uint64_t kNanosecondsPerMillisecond = 1'000'000;
/// The longest step a frame's timers take, so a stall does not fling the camera.
constexpr uint64_t kLongestFrameStepNs = 100 * kNanosecondsPerMillisecond;
/// The rings the nearest-gadget search probes within gadget_pick_points.
constexpr int kGadgetProbeRings = 6;
/// The directions each ring of the nearest-gadget search probes.
constexpr int kGadgetProbeDirections = 16;
/// The rings past the first hit the nearest-gadget search steps on to put the press inside.
constexpr int kGadgetProbeDeepening = 2;
/// The zoom a phone-class match starts at when the command line gives none.
constexpr float kPhoneStartZoom = 1.25F;
/// The orders the phone's rail offers, in the order they are given slots.
constexpr hud::Order kRailOrders[] = {
    hud::Order::move,
    hud::Order::attack,
    hud::Order::patrol,
    hud::Order::guard,
    hud::Order::stop,
    hud::Order::repair,
    hud::Order::reclaim,
    hud::Order::capture,
    hud::Order::load,
    hud::Order::unload,
    hud::Order::blast,
};

/// Returns the size of the canvas a frontend screen is drawn on, as apply_output_mode lays it
/// out: the parent frame of a panel opened over one, the end screen's battlefield, else the
/// 640x480 canvas.
///
/// @param parent the frame a panel is drawn over, or null
/// @param end_screen the end screen's battlefield frame, or null
/// @return canvas pixels, width then height
std::pair<int, int>
frontend_canvas_size(const renderer::Surface* parent, const oa::Surface* end_screen) noexcept {
    if (parent != nullptr)
        return {static_cast<int>(parent->width), static_cast<int>(parent->height)};
    if (end_screen != nullptr)
        return {end_screen->width, end_screen->height};
    return {kCanvasWidth, kCanvasHeight};
}

/// Returns whether a touch device's fingers are the touch controls': a direct touch screen,
/// or the check's unregistered device when it is allowed. Trackpads and the touches SDL makes
/// from a mouse or a pen are not.
///
/// @param device the touch device
/// @param accept_unregistered whether a device SDL does not know is accepted
/// @return whether its fingers are taken
bool accepted_touch_device(SDL_TouchID device, bool accept_unregistered) noexcept {
    if (device == SDL_MOUSE_TOUCHID || device == SDL_PEN_TOUCHID)
        return false;
    const auto type = SDL_GetTouchDeviceType(device);
    return type == SDL_TOUCH_DEVICE_DIRECT ||
           (accept_unregistered && type == SDL_TOUCH_DEVICE_INVALID);
}

/// Returns whether a canvas point lies inside a rectangle.
///
/// @param rect the rectangle, canvas pixels
/// @param x canvas pixels
/// @param y canvas pixels
/// @return whether it lies inside
bool rect_holds(const hud::Rect& rect, int x, int y) noexcept {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

/// Returns the rail order an armed command is, if any.
///
/// @param command the armed command
/// @return the order, or none (no command, a building)
std::optional<hud::Order> order_of(MatchCommand command) noexcept {
    switch (command) {
    case MatchCommand::move:
        return hud::Order::move;
    case MatchCommand::attack:
        return hud::Order::attack;
    case MatchCommand::dgun:
        return hud::Order::blast;
    case MatchCommand::patrol:
        return hud::Order::patrol;
    case MatchCommand::repair:
        return hud::Order::repair;
    case MatchCommand::reclaim:
        return hud::Order::reclaim;
    case MatchCommand::capture:
        return hud::Order::capture;
    case MatchCommand::load:
        return hud::Order::load;
    case MatchCommand::unload:
        return hud::Order::unload;
    case MatchCommand::guard:
        return hud::Order::guard;
    case MatchCommand::none:
    case MatchCommand::build:
        break;
    }
    return std::nullopt;
}

/// Returns what a tap gives with a command armed.
///
/// @param command the armed command
/// @return the tap's action
hud::TapAction armed_action(MatchCommand command) noexcept {
    switch (command) {
    case MatchCommand::move:
        return hud::TapAction::move;
    case MatchCommand::attack:
        return hud::TapAction::attack;
    case MatchCommand::dgun:
        return hud::TapAction::blast;
    case MatchCommand::build:
        return hud::TapAction::build;
    case MatchCommand::patrol:
        return hud::TapAction::patrol;
    case MatchCommand::repair:
        return hud::TapAction::repair;
    case MatchCommand::reclaim:
        return hud::TapAction::reclaim;
    case MatchCommand::capture:
        return hud::TapAction::capture;
    case MatchCommand::load:
        return hud::TapAction::load;
    case MatchCommand::unload:
        return hud::TapAction::unload;
    case MatchCommand::guard:
        return hud::TapAction::guard;
    case MatchCommand::none:
        break;
    }
    return hud::TapAction::none;
}

/// Folds what the controls show into a signature, so a change bumps the HUD state's revision.
class LookSignature {
  public:

    /// Folds in a value.
    ///
    /// @param value the value
    void add(uint64_t value) noexcept {
        for (int shift = 0; shift < 64; shift += 8) {
            hash_ ^= (value >> shift) & 0xFFU;
            hash_ *= kPrime;
        }
    }

    /// Folds in a string.
    ///
    /// @param text the string
    void add(std::string_view text) noexcept {
        add(static_cast<uint64_t>(text.size()));
        for (const char ch : text) {
            hash_ ^= static_cast<uint8_t>(ch);
            hash_ *= kPrime;
        }
    }

    /// Folds in a rectangle.
    ///
    /// @param rect the rectangle
    void add(const hud::Rect& rect) noexcept {
        add(static_cast<uint64_t>(static_cast<uint32_t>(rect.x)));
        add(static_cast<uint64_t>(static_cast<uint32_t>(rect.y)));
        add(static_cast<uint64_t>(static_cast<uint32_t>(rect.width)));
        add(static_cast<uint64_t>(static_cast<uint32_t>(rect.height)));
    }

    /// Returns the signature.
    ///
    /// @return the signature
    [[nodiscard]] uint64_t value() const noexcept { return hash_; }

  private:

    static constexpr uint64_t kOffset = 14695981039346656037ULL;
    static constexpr uint64_t kPrime = 1099511628211ULL;
    uint64_t hash_{kOffset};
};

/// Folds a point into a signature.
///
/// @param[in,out] look the signature
/// @param point canvas pixels
void add_point(LookSignature& look, hud::Point point) noexcept {
    look.add(hud::Rect{point.x, point.y, 0, 0});
}

/// Folds the gamepad's looks into a signature: the pad HUD, badges, glyphs and map, the chips'
/// lit looks, the rings' aims, the hold, the open build ring and the SELECT ▾ focus.
///
/// @param[in,out] look the signature
/// @param state the HUD state
void add_pad_looks(LookSignature& look, const hud::HudState& state) noexcept {
    const auto& pad = state.pad;
    const uint64_t flags = (static_cast<uint64_t>(pad.hud) << 0U) |
                           (static_cast<uint64_t>(pad.badges) << 1U) |
                           (static_cast<uint64_t>(pad.map.fallback) << 2U) |
                           (static_cast<uint64_t>(pad.map.trackpads) << 3U) |
                           (static_cast<uint64_t>(pad.map.left_handed) << 4U) |
                           (static_cast<uint64_t>(pad.force_shown) << 5U) |
                           (static_cast<uint64_t>(pad.force_active) << 6U) |
                           (static_cast<uint64_t>(pad.groups_layer) << 7U) |
                           (static_cast<uint64_t>(pad.over_build_button) << 8U) |
                           (static_cast<uint64_t>(pad.ring_by_pad) << 9U) |
                           (static_cast<uint64_t>(state.force_touch) << 10U) |
                           (static_cast<uint64_t>(state.armed_or_placing) << 11U) |
                           (static_cast<uint64_t>(state.right_click_interface) << 12U);
    look.add(flags);
    look.add((static_cast<uint64_t>(pad.glyphs) << 8U) | static_cast<uint64_t>(pad.map.scheme));
    look.add(pad.radial_aim ? uint64_t{*pad.radial_aim} + 1U : 0U);
    look.add(pad.build_aim ? uint64_t{*pad.build_aim} + 1U : 0U);
    add_point(look, pad.aim_dot);
    look.add(static_cast<uint64_t>(pad.group_ring.has_value()));
    if (pad.group_ring) {
        const auto& ring = *pad.group_ring;
        look.add(hud::Rect{ring.centre.x, ring.centre.y, ring.inner_radius, ring.outer_radius});
        look.add(ring.aim ? uint64_t{*ring.aim} + 1U : 0U);
    }
    look.add(static_cast<uint64_t>(std::lround(pad.hold_progress * 1000.0F)));
    add_point(look, pad.hold_point);
    look.add(static_cast<uint64_t>(static_cast<uint8_t>(state.sheet_focus)));
    look.add(static_cast<uint64_t>(state.build_ring.has_value()));
    if (state.build_ring) {
        const auto& ring = *state.build_ring;
        look.add(hud::Rect{ring.anchor.x, ring.anchor.y, ring.centre.x, ring.centre.y});
        look.add(hud::Rect{ring.inner_radius, ring.outer_radius, 0, 0});
        look.add(static_cast<uint64_t>(ring.standing_orders));
        for (const auto& wedge : ring.wedges) {
            look.add(
                (static_cast<uint64_t>(wedge.kind) << 40U) |
                (static_cast<uint64_t>(static_cast<uint16_t>(wedge.gadget)) << 24U) |
                (static_cast<uint64_t>(wedge.queued) << 8U) | static_cast<uint64_t>(wedge.available)
            );
            look.add(wedge.picture);
        }
    }
}

/// Returns the signature of everything the controls draw from the HUD state and the viewport.
///
/// @param state the HUD state
/// @param viewport the viewport
/// @param pictures the signature of the 3.1c pictures the build ring copies (0 without one)
/// @return the signature
uint64_t
look_signature(const hud::HudState& state, const hud::Viewport& viewport, uint64_t pictures) {
    LookSignature look;
    look.add(static_cast<uint64_t>(viewport.width));
    look.add(static_cast<uint64_t>(viewport.height));
    look.add(static_cast<uint64_t>(std::lround(viewport.px_per_point * 1000.0F)));
    look.add(
        hud::Rect{viewport.safe.left, viewport.safe.top, viewport.safe.right, viewport.safe.bottom}
    );
    look.add(static_cast<uint64_t>(viewport.device));
    look.add(static_cast<uint64_t>(viewport.left_handed));
    const uint64_t flags = (static_cast<uint64_t>(state.in_match) << 0U) |
                           (static_cast<uint64_t>(state.paused) << 1U) |
                           (static_cast<uint64_t>(state.menu_open) << 2U) |
                           (static_cast<uint64_t>(state.watching) << 3U) |
                           (static_cast<uint64_t>(state.shared_game) << 4U) |
                           (static_cast<uint64_t>(state.following) << 5U) |
                           (static_cast<uint64_t>(state.chat_open) << 6U) |
                           (static_cast<uint64_t>(state.has_selection) << 7U) |
                           (static_cast<uint64_t>(state.builder_selected) << 8U) |
                           (static_cast<uint64_t>(state.build_page_loaded) << 9U);
    look.add(flags);
    look.add(static_cast<uint64_t>(state.latch_mode));
    for (const auto latch : {hud::Latch::queue, hud::Latch::add, hud::Latch::times_five})
        look.add(
            (static_cast<uint64_t>(state.latches.active(latch)) << 0U) |
            (static_cast<uint64_t>(state.latches.latched(latch)) << 1U) |
            (static_cast<uint64_t>(state.latches.held(latch)) << 2U)
        );
    for (const auto& pressed : state.pressed)
        look.add((static_cast<uint64_t>(pressed.control) << 8U) | pressed.index);
    look.add(static_cast<uint64_t>(state.sheet));
    look.add(static_cast<uint64_t>(state.drawer_tab));
    look.add(static_cast<uint64_t>(state.drawer_page));
    look.add(static_cast<uint64_t>(state.drawer_pages));
    look.add(static_cast<uint64_t>(state.drawer_cells_wanted));
    look.add(state.drawer_title);
    look.add(static_cast<uint64_t>(state.more_toggle_count));
    look.add(static_cast<uint64_t>(state.more_button_count));
    for (const auto count : state.group_counts)
        look.add(static_cast<uint64_t>(count));
    look.add(static_cast<uint64_t>(state.selected_group));
    look.add(static_cast<uint64_t>(state.rail_count));
    for (std::size_t slot = 0; slot < state.rail_count && slot < state.rail.size(); ++slot) {
        const auto& rail = state.rail[slot];
        look.add(
            (static_cast<uint64_t>(rail.order) << 8U) | (static_cast<uint64_t>(rail.more) << 0U) |
            (static_cast<uint64_t>(rail.available) << 1U) | (static_cast<uint64_t>(rail.lit) << 2U)
        );
    }
    look.add(static_cast<uint64_t>(state.radial.has_value()));
    if (state.radial) {
        const auto& radial = *state.radial;
        look.add(hud::Rect{radial.anchor.x, radial.anchor.y, radial.centre.x, radial.centre.y});
        look.add(static_cast<uint64_t>(radial.queue_hub));
        for (const auto& wedge : radial.wedges)
            look.add(
                (static_cast<uint64_t>(wedge.item) << 8U) |
                (static_cast<uint64_t>(wedge.available) << 0U) |
                (static_cast<uint64_t>(wedge.default_item) << 1U)
            );
    }
    look.add(static_cast<uint64_t>(state.banner.shown));
    look.add(state.banner.order ? static_cast<uint64_t>(*state.banner.order) + 1U : uint64_t{0});
    look.add(state.banner.hint);
    look.add(static_cast<uint64_t>(state.placement.active));
    look.add(static_cast<uint64_t>(state.placement.legal));
    look.add(hud::Rect{state.placement.anchor.x, state.placement.anchor.y, 0, 0});
    look.add(state.placement.name);
    look.add(state.selection_text);
    look.add(static_cast<uint64_t>(state.tap_action));
    look.add(static_cast<uint64_t>(state.enemy_action));
    look.add(state.tip.text);
    look.add(hud::Rect{state.tip.anchor.x, state.tip.anchor.y, 0, 0});
    look.add(static_cast<uint64_t>(state.tip.until_ms != 0));
    look.add(static_cast<uint64_t>(std::lround(state.self_destruct_progress * 1000.0F)));
    add_pad_looks(look, state);
    look.add(pictures);
    return look.value();
}

} // namespace

void Runtime::destroy_touch_state(TouchState* state) noexcept {
    delete state;
}

Runtime::TouchState& Runtime::touch_state() {
    if (!touch_)
        touch_.reset(new TouchState{});
    return *touch_;
}

const Runtime::TouchState* Runtime::touch_state_if_made() const {
    return touch_.get();
}

bool Runtime::take_touch_event(SDL_Event& event, bool& running) {
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        return TouchDispatchAccess::take_finger(*this, event, running);
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return TouchDispatchAccess::take_pointer(*this, event);
#if SDL_VERSION_ATLEAST(3, 4, 0)
    case SDL_EVENT_PINCH_BEGIN:
    case SDL_EVENT_PINCH_UPDATE:
    case SDL_EVENT_PINCH_END: {
        // The pinch the two battlefield fingers make is the recogniser's;
        // a trackpad's pinch is left as it always was.
        const auto* state = touch_state_if_made();
        return state != nullptr && state->dispatch.battlefield.fingers_down() >= 2;
    }
#endif
    default:
        return false;
    }
}

bool TouchDispatchAccess::take_finger(Runtime& runtime, const SDL_Event& event, bool& running) {
    const auto& finger = event.tfinger;
    const auto* made = runtime.touch_state_if_made();
    if (!accepted_touch_device(
            finger.touchID, made != nullptr && made->dispatch.accept_unregistered_touch
        ))
        return false;
    const bool was_active = runtime.touch_controls_active();
    auto& state = runtime.touch_state();
    auto& dispatch = state.dispatch;
    // The first finger switches the touch controls on for the run: SDL
    // stops making mouse events from fingers, and the match is laid out
    // for touch.
    dispatch.seen_direct_finger = true;
    if (!dispatch.hints_set) {
        dispatch.hints_set = true;
        try {
            oa::app::set_input_hints();
        } catch (const std::exception& error) {
            std::clog << "open-annihilation: touch input hints: " << error.what() << '\n';
        }
    }
    if (!was_active)
        runtime.apply_output_mode();
    read_settings(runtime);
    // The canvas point: the renderer's coordinates, or with no renderer
    // the window-relative position over the canvas the screen lays out.
    float x = finger.x;
    float y = finger.y;
    bool converted = false;
    if (runtime.sdl_.renderer != nullptr && runtime.sdl_.window != nullptr) {
        SDL_Event copy = event;
        if (convert_event_to_frame(runtime.sdl_.renderer, copy)) {
            x = copy.tfinger.x;
            y = copy.tfinger.y;
            converted = true;
        }
    }
    if (!converted) {
        int width = runtime.match_layout_.width;
        int height = runtime.match_layout_.height;
        if (runtime.screen_ != Screen::match)
            std::tie(width, height) =
                frontend_canvas_size(runtime.panel_parent(), runtime.end_screen_battlefield_size());
        x = finger.x * static_cast<float>(width);
        y = finger.y * static_cast<float>(height);
    }
    const uint64_t now = dispatch.check_clock_ns ? *dispatch.check_clock_ns
                         : finger.timestamp != 0 ? finger.timestamp
                                                 : SDL_GetTicksNS();
    const gestures::FingerId id{finger.touchID, finger.fingerID};
    const gestures::FingerSample sample{id, x, y, now};
    bool* const outer_running = dispatch.running;
    dispatch.running = &running;
    const auto restore = [&runtime, outer_running] {
        runtime.touch_state().dispatch.running = outer_running;
    };
    try {
        std::optional<std::size_t> slot;
        for (std::size_t index = 0; index < dispatch.fingers.size(); ++index)
            if (dispatch.fingers[index].target != TouchTarget::none &&
                dispatch.fingers[index].id == id)
                slot = index;
        switch (event.type) {
        case SDL_EVENT_FINGER_DOWN: {
            if (slot)
                break;
            for (std::size_t index = 0; index < dispatch.fingers.size() && !slot; ++index)
                if (dispatch.fingers[index].target == TouchTarget::none)
                    slot = index;
            // Past the fingers the dispatcher keeps, a finger is ignored
            // until it lifts.
            if (!slot)
                break;
            // A finger now drives the pointer: the cursor hides, and the
            // screen's edges scroll only for a pointer again.
            dispatch.pointer_seen = false;
            runtime.match_pointer_known_ = false;
            auto& claimed = dispatch.fingers[*slot];
            claimed = ClaimedFinger{};
            claimed.id = id;
            claimed.x = claimed.start_x = x;
            claimed.y = claimed.start_y = y;
            claimed.down_ns = now;
            claim(runtime, claimed);
            recount(runtime);
            if (claimed.target == TouchTarget::battlefield) {
                // A building the pad started placing is the touch model's from now on.
                if (dispatch.placement_by_pad)
                    hand_placement_to_touch(runtime);
                dispatch.inertia_x = 0.0F;
                dispatch.inertia_y = 0.0F;
                if (dispatch.battlefield.fingers_down() == 0)
                    dispatch.battlefield.set_thresholds(thresholds(runtime, true));
                battlefield_gestures(
                    runtime, dispatch.battlefield.finger(gestures::FingerPhase::down, sample)
                );
            } else {
                claimed.recogniser.set_thresholds(thresholds(runtime, false));
                std::ignore = claimed.recogniser.finger(gestures::FingerPhase::down, sample);
                finger_landed(runtime, *slot, now);
            }
            break;
        }
        case SDL_EVENT_FINGER_MOTION: {
            if (!slot)
                break;
            auto& claimed = dispatch.fingers[*slot];
            claimed.x = x;
            claimed.y = y;
            const float slop = points_to_pixels(runtime, gestures::Thresholds{}.slop_points);
            if (std::hypot(x - claimed.start_x, y - claimed.start_y) > slop)
                claimed.moved = true;
            if (claimed.target == TouchTarget::battlefield) {
                battlefield_gestures(
                    runtime, dispatch.battlefield.finger(gestures::FingerPhase::move, sample)
                );
                break;
            }
            const auto batch = claimed.recogniser.finger(gestures::FingerPhase::move, sample);
            finger_moved(runtime, *slot, now);
            if (finger_at(runtime, *slot, id) != nullptr)
                finger_gestures(runtime, *slot, batch, now);
            break;
        }
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
            if (!slot)
                break;
            const bool cancelled = event.type == SDL_EVENT_FINGER_CANCELED;
            const auto phase =
                cancelled ? gestures::FingerPhase::cancel : gestures::FingerPhase::up;
            auto& claimed = dispatch.fingers[*slot];
            claimed.x = x;
            claimed.y = y;
            if (claimed.target == TouchTarget::battlefield) {
                battlefield_gestures(runtime, dispatch.battlefield.finger(phase, sample));
            } else {
                const auto batch = claimed.recogniser.finger(phase, sample);
                if (!cancelled)
                    finger_gestures(runtime, *slot, batch, now);
                if (finger_at(runtime, *slot, id) != nullptr)
                    finger_lifted(runtime, *slot, cancelled, now);
            }
            if (auto* lifted = finger_at(runtime, *slot, id); lifted != nullptr)
                *lifted = ClaimedFinger{};
            recount(runtime);
            const bool battlefield_finger = std::any_of(
                dispatch.fingers.begin(), dispatch.fingers.end(), [](const ClaimedFinger& held) {
                    return held.target == TouchTarget::battlefield;
                }
            );
            if (!battlefield_finger)
                dispatch.finger_point.reset();
            break;
        }
        default:
            break;
        }
    } catch (...) {
        restore();
        throw;
    }
    restore();
    return true;
}

bool TouchDispatchAccess::take_pointer(Runtime& runtime, const SDL_Event& event) {
    // A desktop without touch controls keeps every mouse event as it is; the
    // slim pad HUD's chips take a pointer's clicks as the touch controls do.
    const auto* shown = runtime.touch_state_if_made();
    if (!runtime.touch_controls_active() && (shown == nullptr || !shown->hud.pad.hud))
        return false;
    const bool motion = event.type == SDL_EVENT_MOUSE_MOTION;
    const SDL_MouseID which = motion ? event.motion.which : event.button.which;
    const SDL_WindowID window = motion ? event.motion.windowID : event.button.windowID;
    // SDL's own mouse events from a finger are dropped; the dispatcher's
    // (no window) pass on.
    if (which == SDL_TOUCH_MOUSEID)
        return window != 0;
    auto& state = runtime.touch_state();
    auto& dispatch = state.dispatch;
    dispatch.pointer_seen = true;
    if (motion || event.button.button != SDL_BUTTON_LEFT)
        return false;
    const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (!down && !dispatch.pointer_control)
        return false;
    if (runtime.screen_ != Screen::match || !state.frame_ready) {
        if (!down && dispatch.pointer_control) {
            release_look(runtime, *dispatch.pointer_control);
            dispatch.pointer_control.reset();
        }
        return false;
    }
    SDL_Event converted = event;
    if (runtime.sdl_.renderer != nullptr &&
        !convert_event_to_frame(runtime.sdl_.renderer, converted))
        return false;
    const hud::Point point{
        static_cast<int>(std::floor(converted.button.x)),
        static_cast<int>(std::floor(converted.button.y))
    };
    const uint64_t now = now_ns(runtime);
    const uint64_t now_ms = now / kNanosecondsPerMillisecond;
    if (down) {
        const auto control = hud::hit(state.frame, point, 0);
        if (!control || control->control == hud::Control::none)
            return false;
        dispatch.pointer_control = *control;
        if (control->control == hud::Control::sheet_outside) {
            std::ignore = close_overlay(runtime);
            return true;
        }
        press_look(runtime, *control);
        if (const auto latch = control_latch(control->control)) {
            state.hud.latches.press(*latch, now_ms);
            runtime.refresh_pointer_modifiers();
        } else if (control->control == hud::Control::force) {
            hold_force(runtime, true);
        }
        return true;
    }
    const auto pressed = *dispatch.pointer_control;
    dispatch.pointer_control.reset();
    release_look(runtime, pressed);
    if (pressed.control == hud::Control::sheet_outside)
        return true;
    if (pressed.control == hud::Control::force) {
        hold_force(runtime, false);
        return true;
    }
    const auto released = hud::hit(state.frame, point, 0);
    const bool same =
        released && released->control == pressed.control && released->index == pressed.index;
    if (const auto latch = control_latch(pressed.control)) {
        if (same)
            state.hud.latches.release(*latch, now_ms, dispatch.settings.hold_ms);
        else
            state.hud.latches.cancel(*latch);
        runtime.refresh_pointer_modifiers();
    } else if (same) {
        control_tap(runtime, pressed, now);
    }
    return true;
}

uint64_t TouchDispatchAccess::now_ns(const Runtime& runtime) {
    const auto* state = runtime.touch_state_if_made();
    if (state != nullptr && state->dispatch.check_clock_ns)
        return *state->dispatch.check_clock_ns;
    return SDL_GetTicksNS();
}

void TouchDispatchAccess::read_settings(Runtime& runtime) {
    namespace settings = oa::ui::engine_settings;
    const auto& chosen = runtime.engine_settings();
    auto& seen = runtime.touch_state().dispatch.settings;
    seen.drag = chosen.touch_drag;
    seen.hold_ms = std::clamp(
        chosen.touch_hold_ms, settings::lowest_touch_hold_ms, settings::highest_touch_hold_ms
    );
    seen.one_action = chosen.touch_latches == settings::TouchLatches::one_action;
    seen.haptics = chosen.touch_haptics;
    seen.left_handed = chosen.touch_left_handed;
}

gestures::Thresholds TouchDispatchAccess::thresholds(const Runtime& runtime, bool battlefield) {
    namespace settings = oa::ui::engine_settings;
    gestures::Thresholds result{};
    result.px_per_point = std::max(runtime.touch_px_per_point(), 0.01F);
    const auto* state = runtime.touch_state_if_made();
    const TouchSettingsSeen seen =
        state != nullptr ? state->dispatch.settings : TouchSettingsSeen{};
    result.hold_ms = seen.hold_ms;
    if (battlefield) {
        // Automatic: a box on a tablet, the map dragged on a phone.
        const bool scroll =
            seen.drag == settings::TouchDrag::scroll ||
            (seen.drag == settings::TouchDrag::automatic && runtime.touch_phone_class());
        result.one_finger_drag =
            scroll ? gestures::OneFingerDrag::scroll : gestures::OneFingerDrag::box;
    }
    return result;
}

float TouchDispatchAccess::points_to_pixels(const Runtime& runtime, float points) {
    return points * std::max(runtime.touch_px_per_point(), 0.01F);
}

void TouchDispatchAccess::send_mouse(
    Runtime& runtime, uint32_t type, float x, float y, uint8_t clicks, uint8_t button
) {
    SDL_Event event{};
    event.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        event.motion.timestamp = SDL_GetTicksNS();
        event.motion.windowID = 0;
        event.motion.which = SDL_TOUCH_MOUSEID;
        event.motion.x = x;
        event.motion.y = y;
    } else {
        event.button.timestamp = SDL_GetTicksNS();
        event.button.windowID = 0;
        event.button.which = SDL_TOUCH_MOUSEID;
        event.button.button = button;
        event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.clicks = clicks;
        event.button.x = x;
        event.button.y = y;
    }
    // A click that ends the run reaches the run loop through the event's
    // flag, or from a frame's timer through the exit request.
    bool own_running = true;
    bool* const running = runtime.touch_state().dispatch.running;
    runtime.dispatch_event(event, running != nullptr ? *running : own_running);
    if (!own_running)
        runtime.exit_requested_ = true;
}

void TouchDispatchAccess::send_click(Runtime& runtime, float x, float y, uint8_t clicks) {
    send_mouse(runtime, SDL_EVENT_MOUSE_MOTION, x, y, 0);
    send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, clicks);
    send_mouse(runtime, SDL_EVENT_MOUSE_BUTTON_UP, x, y, clicks);
}

std::optional<std::size_t>
TouchDispatchAccess::hud_gadget_at(const Runtime& runtime, float x, float y, bool* on_gadget) {
    if (on_gadget != nullptr)
        *on_gadget = false;
    if (runtime.screen_ != Screen::match || runtime.match_finished_ || !runtime.match_hud_)
        return std::nullopt;
    // A phone's HUD shows only in its placed regions.
    const auto& layout = runtime.match_layout_;
    if (layout.phone && !oa::ui::display_layout::hud_covers(
                            layout, static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y))
                        ))
        return std::nullopt;
    const auto point = runtime.hud_source_point(x, y);
    const auto& gadgets = runtime.match_hud_->layout.gadgets;
    const oa::ui::gui_input::MenuObject menu{gadgets, -1};
    const auto hit = oa::ui::gui_input::hit_test(menu, point.x, point.y);
    if (!hit || *hit >= gadgets.size())
        return std::nullopt;
    if (on_gadget != nullptr)
        *on_gadget = true;
    // The gadgets the pointer skips, as update_pointer skips them.
    const auto& gadget = gadgets[*hit];
    if (runtime.match_gadget_state(gadget) == nullptr &&
        ((!runtime.pause_menu_shown() && runtime.is_build_page_nav(gadget.common.name) &&
          runtime.builder_gui_page_count() <= 1) ||
         !runtime.gadget_command_available(gadget)))
        return std::nullopt;
    return *hit;
}

std::array<float, 2>
TouchDispatchAccess::frontend_panel_point(const Runtime& runtime, float x, float y) {
    const auto& gadgets = runtime.resources_.layout.gadgets;
    if (runtime.screen_ == Screen::map_selection && !gadgets.empty()) {
        const auto& modal_root = gadgets.front().common;
        x -= static_cast<float>((kCanvasWidth - static_cast<int>(modal_root.width)) / 2);
        y -= static_cast<float>((kCanvasHeight - static_cast<int>(modal_root.height)) / 2);
    }
    const auto origin = runtime.panel_origin();
    return {x - static_cast<float>(origin.x), y - static_cast<float>(origin.y)};
}

std::optional<std::size_t>
TouchDispatchAccess::frontend_gadget_at(const Runtime& runtime, float x, float y, bool* on_gadget) {
    if (on_gadget != nullptr)
        *on_gadget = false;
    const auto& gadgets = runtime.resources_.layout.gadgets;
    if (runtime.screen_ == Screen::match || gadgets.empty())
        return std::nullopt;
    // The point the pointer's hover tests, as update_pointer finds it.
    const auto [panel_x, panel_y] = frontend_panel_point(runtime, x, y);
    const oa::ui::gui_input::MenuObject menu{gadgets, -1};
    const auto hit = oa::ui::gui_input::hit_test(
        menu, static_cast<int32_t>(panel_x), static_cast<int32_t>(panel_y)
    );
    if (!hit || *hit >= gadgets.size())
        return std::nullopt;
    if (on_gadget != nullptr)
        *on_gadget = true;
    if (!runtime.frontend_gadget_pressable(*hit))
        return std::nullopt;
    return *hit;
}

float TouchDispatchAccess::frontend_gadget_distance(
    const Runtime& runtime, float x, float y, std::size_t gadget
) {
    const auto rect = oa::ui::gui_input::gadget_geometry(runtime.resources_.layout.gadgets, gadget);
    if (!rect)
        return std::numeric_limits<float>::infinity();
    // A press finds the gadget from its left column up to the column after
    // its right one, and the same down its rows.
    const auto [panel_x, panel_y] = frontend_panel_point(runtime, x, y);
    const auto outside = [](float at, int32_t first, int32_t last) {
        return std::max({static_cast<float>(first) - at, 0.0F, at - static_cast<float>(last + 1)});
    };
    return std::hypot(
        outside(panel_x, rect->left, rect->right), outside(panel_y, rect->top, rect->bottom)
    );
}

std::optional<std::array<float, 2>> TouchDispatchAccess::nearest_gadget(
    const Runtime& runtime, float x, float y, bool frontend, std::size_t* gadget
) {
    const auto test = [&runtime, frontend](float px, float py) {
        return frontend ? frontend_gadget_at(runtime, px, py) : hud_gadget_at(runtime, px, py);
    };
    if (const auto at = test(x, y)) {
        if (gadget != nullptr)
            *gadget = *at;
        return std::array<float, 2>{x, y};
    }
    const float radius = points_to_pixels(runtime, hud::gadget_pick_points);
    const float step = radius / static_cast<float>(kGadgetProbeRings);
    for (int ring = 1; ring <= kGadgetProbeRings; ++ring) {
        for (int direction = 0; direction < kGadgetProbeDirections; ++direction) {
            const double angle = 2.0 * std::numbers::pi * direction / kGadgetProbeDirections;
            const auto dx = static_cast<float>(std::cos(angle));
            const auto dy = static_cast<float>(std::sin(angle));
            const float reach = step * static_cast<float>(ring);
            const auto at = test(x + dx * reach, y + dy * reach);
            if (!at)
                continue;
            // The press goes a little further in, off the gadget's edge.
            std::array<float, 2> inside{x + dx * reach, y + dy * reach};
            for (int deeper = 1; deeper <= kGadgetProbeDeepening; ++deeper) {
                const float further = reach + step * static_cast<float>(deeper);
                const auto still = test(x + dx * further, y + dy * further);
                if (!still || *still != *at)
                    break;
                inside = {x + dx * further, y + dy * further};
            }
            if (gadget != nullptr)
                *gadget = *at;
            return inside;
        }
    }
    return std::nullopt;
}

void TouchDispatchAccess::claim(Runtime& runtime, ClaimedFinger& finger) {
    auto& state = runtime.touch_state();
    const float x = finger.x;
    const float y = finger.y;
    const hud::Point point{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y))};
    const int radius =
        static_cast<int>(std::lround(points_to_pixels(runtime, hud::gadget_pick_points)));
    const bool on_match = runtime.screen_ == Screen::match && runtime.match_;
    finger.press_x = finger.sent_x = x;
    finger.press_y = finger.sent_y = y;
    const auto as_control = [&finger](const hud::ControlRect& control) {
        finger.target = TouchTarget::control;
        finger.control = control;
    };
    // A HUD finger presses its gadget where it lies, else the nearest one
    // within reach; self-destruct gadgets and build buttons are marked.
    // With nothing to press it is taken and does nothing, as a click there
    // would reach the battlefield behind the HUD; over a held match (the
    // in-game menu) and on a panel's sheet it still presses, for the
    // panel's scroll bars and buttons of its own.
    const auto as_hud = [&](bool near, bool bare_presses) {
        finger.target = TouchTarget::hud_gadget;
        bool on = false;
        if (const auto exact = hud_gadget_at(runtime, x, y, &on))
            finger.gadget = static_cast<int16_t>(*exact);
        else if (!on && near) {
            std::size_t found = 0;
            if (const auto snapped = nearest_gadget(runtime, x, y, false, &found)) {
                finger.gadget = static_cast<int16_t>(found);
                finger.press_x = finger.sent_x = (*snapped)[0];
                finger.press_y = finger.sent_y = (*snapped)[1];
            }
        }
        if (finger.gadget < 0 && !bare_presses && !runtime.match_paused_) {
            finger.target = TouchTarget::control;
            finger.control = {};
            return;
        }
        if (finger.gadget < 0 || !runtime.match_hud_ ||
            static_cast<std::size_t>(finger.gadget) >= runtime.match_hud_->layout.gadgets.size())
            return;
        const auto& gadget =
            runtime.match_hud_->layout.gadgets[static_cast<std::size_t>(finger.gadget)];
        const std::string_view name = gadget.common.name;
        finger.self_destruct =
            !runtime.match_paused_ && (name.find("SELFD") != std::string_view::npos ||
                                       name.find("DESTRUCT") != std::string_view::npos);
        constexpr auto build_buttons =
            oa::ui::hud::kCommonUnitButton | oa::ui::hud::kCommonWeaponButton;
        finger.build_button =
            !runtime.match_paused_ &&
            (static_cast<uint8_t>(gadget.common.common_attributes) & build_buttons) != 0;
    };
    // 1. The open radial takes every finger: a wedge, its hub, or outside
    //    (which closes it).
    if (on_match && state.hud.radial) {
        hud::ControlRect control{};
        control.control = hud::Control::sheet_outside;
        if (state.frame_ready)
            if (const auto hit = hud::hit(state.frame, point, radius);
                hit && (hit->control == hud::Control::radial_item ||
                        hit->control == hud::Control::radial_hub))
                control = *hit;
        if (control.control == hud::Control::sheet_outside) {
            const auto& radial = *state.hud.radial;
            if (const auto item = hud::radial_hit(radial, point, state.viewport)) {
                control.control = hud::Control::radial_item;
                control.index = static_cast<uint8_t>(*item);
                for (const auto& wedge : radial.wedges)
                    if (wedge.item == *item)
                        control.rect = wedge.hit;
            } else if (hud::radial_hub_hit(radial, point)) {
                control.control = hud::Control::radial_hub;
                control.rect = {
                    radial.centre.x - radial.inner_radius,
                    radial.centre.y - radial.inner_radius,
                    radial.inner_radius * 2,
                    radial.inner_radius * 2
                };
            }
        }
        as_control(control);
        return;
    }
    // 2. A dialog, the settings or a notice of the player's folder or the
    //    mod, over any screen, take the finger as a pointer; they find the
    //    nearest control themselves.
    if (oa::ui::frontend_dialogs::dialog_count() != 0 ||
        runtime.engine_settings_dialog() != nullptr || runtime.saves_notice_shown()) {
        finger.target = TouchTarget::frontend;
        return;
    }
    // 3. Off the match a finger is a pointer pressing the nearest gadget,
    //    unless an OA screen that shows has a pressable control under the
    //    point, or no farther than that gadget. That finger keeps its own
    //    point, and no gadget is named for it.
    if (!on_match) {
        finger.target = TouchTarget::frontend;
        bool on = false;
        if (const auto exact = frontend_gadget_at(runtime, x, y, &on))
            finger.gadget = static_cast<int16_t>(*exact);
        else if (!on) {
            const float pick = points_to_pixels(runtime, hud::gadget_pick_points);
            const std::optional<float> control = runtime.oa_layer().nearest_control(x, y, pick);
            std::size_t found = 0;
            if (const auto snapped = nearest_gadget(runtime, x, y, true, &found)) {
                // The control and the gadget, each to its rectangle's edge:
                // the nearer takes the finger, as the OA buttons judge it,
                // and a tie is the control's.
                const bool prefer_control =
                    control.has_value() &&
                    *control <= frontend_gadget_distance(runtime, x, y, found);
                if (!prefer_control) {
                    finger.gadget = static_cast<int16_t>(found);
                    finger.press_x = finger.sent_x = (*snapped)[0];
                    finger.press_y = finger.sent_y = (*snapped)[1];
                }
            }
        }
        return;
    }
    // 4. A phone's placed regions are drawn over the touch controls.
    const auto& layout = runtime.match_layout_;
    if (layout.placed_count > 0)
        if (const auto* region = oa::ui::display_layout::region_at(layout, point.x, point.y)) {
            if (region->role == oa::ui::display_layout::RegionRole::minimap)
                finger.target = TouchTarget::minimap;
            else
                as_hud(true, region->role == oa::ui::display_layout::RegionRole::sheet);
            return;
        }
    // 5. An open sheet: its controls, its panel (which takes the finger),
    //    or outside it (which closes it).
    if (state.hud.sheet != hud::Sheet::none && state.frame_ready) {
        if (const auto hit = hud::hit(state.frame, point, radius);
            hit && hit->control != hud::Control::none &&
            hit->control != hud::Control::sheet_outside) {
            as_control(*hit);
            return;
        }
        hud::ControlRect control{};
        control.control = rect_holds(state.frame.sheet, point.x, point.y)
                              ? hud::Control::none
                              : hud::Control::sheet_outside;
        control.rect = state.frame.sheet;
        as_control(control);
        return;
    }
    // 6. A touch control, the nearest within reach.
    if (state.frame_ready)
        if (const auto hit = hud::hit(state.frame, point, radius);
            hit && hit->control != hud::Control::none &&
            hit->control != hud::Control::sheet_outside) {
            as_control(*hit);
            return;
        }
    // 7. The rest of what the controls cover (the status pill, the banner,
    //    the placement bar) takes the finger and does nothing with it.
    if (state.frame_ready && hud::covers(state.frame, point)) {
        hud::ControlRect control{};
        as_control(control);
        return;
    }
    // 8. A HUD gadget under the finger.
    bool on_gadget = false;
    std::ignore = hud_gadget_at(runtime, x, y, &on_gadget);
    if (on_gadget) {
        as_hud(false, false);
        return;
    }
    // 9. The minimap.
    if (runtime.radar_contains(x, y)) {
        finger.target = TouchTarget::minimap;
        return;
    }
    // 10. The nearest HUD gadget, off the battlefield or over a held match
    //     (the in-game menu over it).
    const bool battlefield = runtime.battlefield_contains(x, y) && !runtime.placed_hud_covers(x, y);
    if (!battlefield || runtime.match_paused_) {
        std::size_t found = 0;
        if (nearest_gadget(runtime, x, y, false, &found)) {
            as_hud(true, false);
            return;
        }
    }
    // 11. The battlefield; else the bare HUD.
    if (battlefield) {
        finger.target = TouchTarget::battlefield;
        return;
    }
    as_hud(false, false);
}

ClaimedFinger* TouchDispatchAccess::finger_at(
    Runtime& runtime, std::size_t slot, const oa::ui::touch_gestures::FingerId& id
) {
    auto& fingers = runtime.touch_state().dispatch.fingers;
    if (slot >= fingers.size() || fingers[slot].target == TouchTarget::none ||
        !(fingers[slot].id == id))
        return nullptr;
    return &fingers[slot];
}

void TouchDispatchAccess::recount(Runtime& runtime) {
    auto& dispatch = runtime.touch_state().dispatch;
    dispatch.finger_count = static_cast<uint8_t>(std::count_if(
        dispatch.fingers.begin(), dispatch.fingers.end(), [](const ClaimedFinger& finger) {
            return finger.target != TouchTarget::none;
        }
    ));
}

void TouchDispatchAccess::reset_fingers(Runtime& runtime) {
    auto& state = runtime.touch_state();
    auto& dispatch = state.dispatch;
    dispatch.battlefield.reset();
    for (auto& finger : dispatch.fingers)
        finger = ClaimedFinger{};
    dispatch.finger_count = 0;
    dispatch.finger_point.reset();
    dispatch.box_active = false;
    dispatch.megamap_drag = false;
    dispatch.scroll_active = false;
    dispatch.ghost_drag = false;
    dispatch.auto_scrolling = false;
    dispatch.inertia_x = 0.0F;
    dispatch.inertia_y = 0.0F;
    dispatch.pointer_control.reset();
    dispatch.last_tap_ns = 0;
    // A latch a dropped finger held is let go; a latched one stays.
    for (const auto latch : {hud::Latch::queue, hud::Latch::add, hud::Latch::times_five})
        if (state.hud.latches.held(latch))
            state.hud.latches.cancel(latch);
    state.hud.pressed = {};
    state.hud.sheet = hud::Sheet::none;
    state.hud.radial.reset();
    state.hud.self_destruct_progress = 0.0F;
    // A finger that held FORCE is gone.
    if (state.hud.force_touch)
        hold_force(runtime, false);
}

void Runtime::tick_touch() {
    // The touch controls' frame, or with a gamepad used and no touch the pad HUD's alone;
    // without either nothing runs.
    const bool touch = touch_controls_active();
    if (!touch && !pad_used())
        return;
    auto& state = touch_state();
    auto& dispatch = state.dispatch;
    TouchDispatchAccess::read_settings(*this);
    const uint64_t now = TouchDispatchAccess::now_ns(*this);
    const uint64_t elapsed = dispatch.tick_ns != 0 && now > dispatch.tick_ns
                                 ? std::min(now - dispatch.tick_ns, kLongestFrameStepNs)
                                 : 0;
    dispatch.tick_ns = now;
    if (!touch) {
        // No finger has landed: no recogniser, touch placement or finger hover runs, so a
        // building the mouse or the pad armed stays the pointer's to place.
        if (screen_ != Screen::match || !match_) {
            state.hud.in_match = false;
            state.frame_ready = false;
            return;
        }
        TouchDispatchAccess::refresh_hud(*this, now);
        return;
    }
    // Holds come due with no finger event.
    if (dispatch.battlefield.fingers_down() == 0)
        dispatch.battlefield.set_thresholds(TouchDispatchAccess::thresholds(*this, true));
    if (const auto batch = dispatch.battlefield.advance(now); batch.count != 0)
        TouchDispatchAccess::battlefield_gestures(*this, batch);
    for (std::size_t slot = 0; slot < dispatch.fingers.size(); ++slot) {
        auto& finger = dispatch.fingers[slot];
        if (finger.target == TouchTarget::none || finger.target == TouchTarget::battlefield)
            continue;
        if (const auto batch = finger.recogniser.advance(now); batch.count != 0)
            TouchDispatchAccess::finger_gestures(*this, slot, batch, now);
    }
    TouchDispatchAccess::hold_timers(*this, now);
    if (screen_ != Screen::match || !match_) {
        state.hud.in_match = false;
        state.frame_ready = false;
        return;
    }
    TouchDispatchAccess::step_inertia(*this, elapsed);
    TouchDispatchAccess::step_auto_scroll(*this, elapsed);
    TouchDispatchAccess::refresh_placement(*this);
    // A finger resting on the battlefield keeps the pointer under it, so
    // the unit there stays the cursor's unit and the bottom bar's.
    if (dispatch.finger_point && !dispatch.placement_touch && !dispatch.box_active &&
        !dispatch.scroll_active && dispatch.battlefield.fingers_down() == 1)
        update_pointer((*dispatch.finger_point)[0], (*dispatch.finger_point)[1]);
    TouchDispatchAccess::refresh_hud(*this, now);
    // The phone's placed regions follow the frame just laid out (the open
    // sheet's cells), so a tap before the next frame is drawn hits them.
    refresh_placed_hud_regions();
}

void TouchDispatchAccess::refresh_hud(Runtime& runtime, uint64_t now) {
    namespace console = oa::ui::console;
    auto& state = runtime.touch_state();
    auto& dispatch = state.dispatch;
    auto& look = state.hud;
    const auto& layout = runtime.match_layout_;
    const uint64_t now_ms = now / kNanosecondsPerMillisecond;
    // The viewport the controls are laid out on: the window's points times the Control size.
    // Without touch controls the match keeps the 3.1c layout, which the pad HUD goes over,
    // mirrored for a left-handed pad.
    const bool touch = runtime.touch_controls_active();
    hud::Viewport viewport{};
    viewport.width = layout.width;
    viewport.height = layout.height;
    viewport.px_per_point =
        (layout.px_per_point > 0.0 ? static_cast<float>(layout.px_per_point) : 1.0F) *
        runtime.touch_control_scale();
    viewport.safe = layout.safe;
    viewport.device =
        touch && runtime.touch_phone_class() ? hud::DeviceClass::phone : hud::DeviceClass::tablet;
    viewport.left_handed = touch ? dispatch.settings.left_handed : look.pad.map.left_handed;
    viewport.chrome = layout;
    state.viewport = viewport;
    look.in_match = true;
    look.latch_mode =
        dispatch.settings.one_action ? hud::LatchMode::one_action : hud::LatchMode::stay_on;
    // The selection and the stored groups.
    std::array<uint16_t, 10> group_counts{};
    std::array<uint16_t, 10> selected_in_group{};
    uint32_t selected = 0;
    for (const auto& slot : runtime.match_->world().slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr ||
            slot.owner_index != runtime.match_local_player_)
            continue;
        const bool chosen = (slot.unit->flags & OA_UNIT_FLAG_SELECTED) != 0;
        const auto squad = static_cast<std::size_t>(slot.unit->squad);
        if (squad >= 1 && squad < group_counts.size()) {
            ++group_counts[squad];
            if (chosen)
                ++selected_in_group[squad];
        }
        if (chosen)
            ++selected;
    }
    look.has_selection = selected != 0;
    look.group_counts = group_counts;
    look.selected_group = 0;
    for (std::size_t group = 1; group < group_counts.size(); ++group)
        if (selected != 0 && group_counts[group] == selected &&
            selected_in_group[group] == selected)
            look.selected_group = static_cast<uint8_t>(group);
    look.selection_text.clear();
    if (selected != 0 && runtime.selected_match_unit_ != 0) {
        look.selection_text = runtime.unit_info_name(runtime.selected_match_unit_);
        if (selected > 1)
            look.selection_text += " + " + std::to_string(selected - 1);
    }
    // What a tap gives: at a finger resting on the battlefield the engine's
    // order cursor says; otherwise the armed order, or a move with a
    // selection that can move.
    const bool placing =
        runtime.match_command_ == MatchCommand::build && runtime.pending_build_type_ != 0;
    const auto armed = armed_action(runtime.match_command_);
    const bool finger_resting = dispatch.finger_point && dispatch.battlefield.fingers_down() == 1 &&
                                !dispatch.box_active && !dispatch.scroll_active;
    // With a gamepad's pointer on the match and no finger resting, a click at the pointer: the
    // cursor's on the battlefield, nothing of the battlefield's over the 3.1c panel.
    const bool pad_pointer = !finger_resting && runtime.pad_used() && runtime.match_pointer_known_;
    const bool pad_on_field =
        pad_pointer &&
        runtime.battlefield_contains(runtime.match_pointer_x_, runtime.match_pointer_y_) &&
        !runtime.placed_hud_covers(runtime.match_pointer_x_, runtime.match_pointer_y_);
    const auto pointer_action =
        pad_on_field ? tap_action_of(runtime.pick_match_cursor()) : hud::TapAction::none;
    if (placing)
        look.tap_action = look.placement.active ? hud::TapAction::place : hud::TapAction::build;
    else if (finger_resting)
        look.tap_action = tap_action_of(runtime.pick_match_cursor());
    else if (pointer_action != hud::TapAction::none)
        look.tap_action = pointer_action;
    else if (pad_pointer && !pad_on_field)
        look.tap_action = hud::TapAction::none;
    else if (runtime.match_command_ != MatchCommand::none)
        look.tap_action = armed;
    else if (look.has_selection && runtime.order_command_available("MOVE"))
        look.tap_action = hud::TapAction::move;
    else
        look.tap_action = hud::TapAction::select;
    if (runtime.match_command_ != MatchCommand::none && !placing)
        look.enemy_action = armed;
    else if (look.has_selection && runtime.order_command_available("ATTACK"))
        look.enemy_action = hud::TapAction::attack;
    else
        look.enemy_action = hud::TapAction::none;
    look.armed_or_placing = runtime.match_command_ != MatchCommand::none;
    look.right_click_interface = runtime.match_->state().game.interface_type != 0;
    // The banner: an armed order, or the building being placed.
    const bool queue_on = look.latches.active(hud::Latch::queue);
    look.banner = {};
    if (look.placement.active) {
        look.banner.shown = true;
        look.banner.hint =
            queue_on ? "DOUBLE TAP OR HOLD TO BUILD · QUEUE ON" : "DOUBLE TAP OR HOLD TO BUILD";
    } else if (const auto order = order_of(runtime.match_command_)) {
        look.banner.shown = true;
        look.banner.order = order;
        look.banner.hint =
            queue_on ? "TAP POINTS · QUEUE KEEPS ADDING" : "TAP A TARGET · ✕ TAKES IT BACK";
    }
    // The lit looks.
    auto& game = runtime.match_->state().game;
    look.paused = (game.sim_run_flags & console::kSimRunPaused) != 0;
    look.menu_open = runtime.match_paused_;
    look.following = runtime.match_tracking_;
    look.chat_open = runtime.chat_composing_;
    look.watching = (runtime.current_extension_state() & extension_state::local_watcher) != 0;
    look.shared_game = runtime.keeps_running_inactive();
    // The builder and its pages.
    const auto* definition = runtime.selected_match_unit_ != 0
                                 ? runtime.definition_for(runtime.selected_match_unit_)
                                 : nullptr;
    look.builder_selected = definition != nullptr && definition->builder;
    look.build_page_loaded =
        runtime.match_hud_.has_value() && runtime.match_build_page_ > 0 && !runtime.match_paused_;
    look.drawer_title = look.builder_selected ? runtime.unit_info_name(runtime.selected_match_unit_)
                                              : std::string("BUILD");
    look.drawer_pages =
        look.builder_selected
            ? static_cast<uint8_t>(std::clamp(runtime.builder_gui_page_count(), 0, 255))
            : 0;
    if (look.sheet == hud::Sheet::drawer && look.drawer_tab == hud::DrawerTab::build &&
        runtime.match_build_page_ > 0)
        look.drawer_page = static_cast<uint8_t>(std::clamp(runtime.match_build_page_ - 1, 0, 255));
    // The phone's rail: the orders the selection can take, in a fixed
    // order, as many as fit before MORE.
    const uint8_t capacity = hud::rail_capacity(viewport);
    const auto armed_order = order_of(runtime.match_command_);
    look.rail = {};
    uint8_t rail_count = 0;
    if (capacity > 0 && look.has_selection) {
        for (const auto order : kRailOrders) {
            if (rail_count + 1 >= capacity || std::size_t{rail_count} + 1 >= look.rail.size())
                break;
            if (!runtime.order_command_available(hud::order_name(order)))
                continue;
            auto& slot = look.rail[rail_count++];
            slot.order = order;
            slot.available = true;
            slot.lit = armed_order && *armed_order == order;
        }
        auto& more = look.rail[rail_count++];
        more.more = true;
        more.available = true;
        more.lit = look.sheet == hud::Sheet::more;
    }
    look.rail_count = rail_count;
    // The sheets' cells, counted with the gadgets the placed regions show.
    look.drawer_cells_wanted = 0;
    if (look.sheet == hud::Sheet::drawer)
        look.drawer_cells_wanted = static_cast<uint8_t>(std::min<std::size_t>(
            runtime.drawer_sheet_gadgets().button_count, hud::max_drawer_cells
        ));
    look.more_toggle_count = 0;
    look.more_button_count = 0;
    if (look.sheet == hud::Sheet::more) {
        const auto more = runtime.more_sheet_gadgets();
        look.more_toggle_count = more.toggle_count;
        look.more_button_count = more.button_count;
    }
    if (look.tip.until_ms != 0 && now_ms >= look.tip.until_ms)
        look.tip = {};
    state.frame = hud::lay_out(viewport, look);
    state.frame_ready = true;
    const uint64_t pictures =
        look.build_ring ? PhoneHudAccess::build_ring_picture_signature(runtime, *look.build_ring)
                        : 0U;
    if (const auto signature = look_signature(look, viewport, pictures);
        signature != dispatch.look_signature) {
        dispatch.look_signature = signature;
        ++look.revision;
    }
}

void Runtime::touch_screen_changed() {
    if (!touch_)
        return;
    TouchDispatchAccess::reset_fingers(*this);
    auto& state = touch_state();
    state.frame_ready = false;
    state.hud.placement = {};
    state.dispatch.placement_touch = false;
    state.dispatch.placement_by_pad = false;
}

void Runtime::touch_match_started() {
    // The latches start off in every match, the gamepad's grips sharing them.
    const bool touch = touch_controls_active();
    if (!touch && !pad_used())
        return;
    touch_screen_changed();
    auto& state = touch_state();
    state.hud.latches.clear();
    state.hud.tip = {};
    state.hud.drawer_page = 0;
    state.hud.drawer_tab = oa::ui::touch_hud::DrawerTab::build;
    state.dispatch.drawer_unit = 0;
    // A phone with touch controls starts nearer the ground unless the
    // command line chose a zoom; the player zooms freely from there.
    if (!touch || !touch_phone_class() || options_.match_zoom != kDefaultBattlefieldZoom)
        return;
    const float start = std::clamp(kPhoneStartZoom, least_match_zoom(), most_match_zoom());
    const float current = match_zoom_ > 0.0F ? match_zoom_ : kDefaultBattlefieldZoom;
    zoom_match_about(
        start / current,
        static_cast<float>(match_layout_.left + match_layout_.battlefield_width() / 2),
        static_cast<float>(match_layout_.top + match_layout_.battlefield_height() / 2)
    );
    match_zoom_ = start;
    match_zoom_target_ = start;
}

bool Runtime::touch_controls_active() const {
    if (OA_TOUCH_FIRST || options_.touch_controls)
        return true;
    const auto* state = touch_state_if_made();
    return state != nullptr && (state->dispatch.forced || state->dispatch.seen_direct_finger);
}

uint8_t Runtime::touch_finger_count() const {
    const auto* state = touch_state_if_made();
    return state != nullptr ? state->dispatch.finger_count : 0;
}

float Runtime::touch_px_per_point() const {
    // On the match the touch layer's points are the window's times the Control size.
    if (screen_ == Screen::match && match_layout_.px_per_point > 0.0)
        return static_cast<float>(match_layout_.px_per_point) * touch_control_scale();
    if (sdl_.window == nullptr)
        return 1.0f;
    int window_width = 0;
    int window_height = 0;
    if (!SDL_GetWindowSize(sdl_.window, &window_width, &window_height) || window_width <= 0)
        return 1.0f;
    const auto [canvas_width, canvas_height] =
        frontend_canvas_size(panel_parent(), end_screen_battlefield_size());
    if (sdl_.renderer == nullptr)
        return static_cast<float>(canvas_width) / static_cast<float>(window_width);
    // The logical presentation rectangle is in output pixels; the window's
    // points over its output pixels bring it to points.
    SDL_FRect shown{};
    int output_width = 0;
    int output_height = 0;
    if (!SDL_GetRenderLogicalPresentationRect(sdl_.renderer, &shown) ||
        !SDL_GetRenderOutputSize(sdl_.renderer, &output_width, &output_height) ||
        output_width <= 0 || shown.w <= 0.0f)
        return static_cast<float>(canvas_width) / static_cast<float>(window_width);
    const float shown_points =
        shown.w * static_cast<float>(window_width) / static_cast<float>(output_width);
    return static_cast<float>(canvas_width) / shown_points;
}

float Runtime::touch_control_scale() const {
    namespace settings = oa::ui::engine_settings;
    // The stored choice, else the defaults for this machine (Larger on a Steam Deck).
    const settings::ControlSize size =
        engine_settings_ != nullptr
            ? engine_settings_->current.touch_control_size
            : settings::default_settings(EngineSettingsState::inputs(*this)).touch_control_size;
    return settings::control_size_scale(size);
}

bool Runtime::touch_phone_class() const {
    int width_points = 0;
    int height_points = 0;
    if (sdl_.window == nullptr || !SDL_GetWindowSize(sdl_.window, &width_points, &height_points) ||
        width_points <= 0 || height_points <= 0) {
        int canvas_width = 0;
        int canvas_height = 0;
        if (screen_ == Screen::match) {
            canvas_width = match_layout_.width;
            canvas_height = match_layout_.height;
        } else {
            std::tie(canvas_width, canvas_height) =
                frontend_canvas_size(panel_parent(), end_screen_battlefield_size());
        }
        const float px_per_point = std::max(touch_px_per_point(), 0.001f);
        width_points = static_cast<int>(static_cast<float>(canvas_width) / px_per_point);
        height_points = static_cast<int>(static_cast<float>(canvas_height) / px_per_point);
        return oa::ui::touch_hud::classify_device(width_points, height_points) ==
               oa::ui::touch_hud::DeviceClass::phone;
    }
    // The window's points in the touch layer's points: a larger Control size makes the window
    // smaller in them (1280x800 at Larger is 853x533, still a tablet).
    const float scale = std::max(touch_control_scale(), 0.001F);
    return oa::ui::touch_hud::classify_device(
               static_cast<int>(static_cast<float>(width_points) / scale),
               static_cast<int>(static_cast<float>(height_points) / scale)
           ) == oa::ui::touch_hud::DeviceClass::phone;
}

Runtime::TouchCursor Runtime::touch_cursor() const {
    if (!touch_controls_active())
        return {};
    const auto* state = touch_state_if_made();
    // A mouse, trackpad or pen used since the last finger shows the
    // cursor at the pointer as ever.
    if (state != nullptr && state->dispatch.pointer_seen)
        return {};
    TouchCursor cursor{};
    cursor.replaces_pointer = true;
    if (state == nullptr || screen_ != Screen::match || !state->dispatch.finger_point)
        return cursor;
    const auto& dispatch = state->dispatch;
    // The ghost shows where a building goes, and a box or a dragged map
    // shows itself.
    if (dispatch.placement_touch || dispatch.box_active || dispatch.scroll_active ||
        dispatch.ghost_drag)
        return cursor;
    cursor.visible = true;
    cursor.x = (*dispatch.finger_point)[0];
    cursor.y = (*dispatch.finger_point)[1];
    return cursor;
}

} // namespace oa::app
