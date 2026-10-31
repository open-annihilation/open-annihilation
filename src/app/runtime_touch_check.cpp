// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-touch-controls: the touch controls, driven by finger events
// through the dispatcher, on a skirmish: taps, boxes, latches, the radial,
// placement, the minimap, pinch and pan, the lifecycle pause, the banner's
// title in English, German and Simplified Chinese, and the tablet or phone
// layout. It writes touch-<case>.ppm snapshots of the composed frame
// into its working directory for people to look at (docs/touch-controls.md).
//
// Each case starts from a known state and fails on its own, naming the piece
// that is missing and the part of the touch controls that owns it:
// dispatch (the finger dispatcher and its actions), touch-ui (the gesture
// recogniser and the controls' layout), draw (the touch layer's drawing),
// phone (the phone layout and its placed regions), fullbleed (the overlays
// kept clear of the controls), settings (the Touch section), platform (the
// lifecycle and the platform's services), touch-check (this check's own
// setup).
#include "oa/app/runtime.hpp"
#include "engine_settings_state.hpp"
#include "language_state.hpp"
#include "touch_state.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/present/world_renderer.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/match_runtime/construction_orders.hpp"
#include "oa/sim/messages.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/hud/kill_board.hpp"
#include "oa/ui/touch_gestures.hpp"
#include "oa/ui/touch_hud.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace hud = oa::ui::touch_hud;
namespace layout = oa::ui::display_layout;
namespace orders = oa::sim::match_runtime;
namespace input = oa::sim::gameplay_input;
namespace console = oa::ui::console;
namespace settings = oa::ui::engine_settings;

/// The touch device the check's fingers come from: a fixed id that no device registers, which
/// the dispatcher takes because the check allows unregistered touch.
constexpr SDL_TouchID kCheckTouchDevice = 0x70c4;
constexpr uint64_t kNanosecondsPerMillisecond = 1000000;
/// One frame of the check's clock, in milliseconds.
constexpr uint64_t kFrameMs = 16;
/// How long a tapping finger rests before it lifts.
constexpr uint64_t kTapRestMs = 60;
/// The pause after a tap, longer than a double tap's window, so the next tap counts once.
constexpr uint64_t kAfterTapMs = 400;
/// How long a held finger rests: past the 350 ms default hold delay.
constexpr uint64_t kHoldMs = 400;
/// The pause between the frames of a hold.
constexpr uint64_t kHoldStepMs = 100;
/// Fingers apart for two-finger gestures, in points.
constexpr float kTwoFingerSpreadPoints = 40.0F;
/// The first step of a drag, in points: past the 10 pt slop at once.
constexpr float kDragFirstStepPoints = 16.0F;
/// Moves a drag takes after its first step.
constexpr int kDragSteps = 8;
/// Points off a unit's edge a tap still takes it (inside the 12 pt reach), and points that do
/// not (beyond it).
constexpr float kNearMissPoints = 8.0F;
constexpr float kFarMissPoints = 20.0F;
/// Points from a unit to the ground points the order cases tap.
constexpr float kGroundOffsetPoints = 96.0F;
/// Points around two units that a selection box takes in.
constexpr float kBoxMarginPoints = 36.0F;
/// Points a scrolling drag travels.
constexpr float kScrollTravelPoints = 150.0F;
/// Points from the ghost that a hold still places at the ghost (inside its 44 pt reach).
constexpr float kNearGhostPoints = 24.0F;
/// The ghost's reach in points: a hold or a double tap within it places at the ghost.
constexpr float kGhostReachPoints = 44.0F;
/// Map pixels the units the check places keep from the commander and from each other.
constexpr int32_t kUnitSpacing = 48;
constexpr int32_t kUnitReach = 160;
constexpr int32_t kEdgeMargin = 96;
/// The unit pick's reach, in points, that a tap with no unit under it searches.
constexpr float kPickReachPoints = 12.0F;
/// The zoom the phone layout starts a match at.
constexpr float kPhoneStartZoom = 1.25F;
/// Frames the game's clock runs in the lifecycle case's one second, and their length.
constexpr uint32_t kClockFrameMs = 25;
constexpr uint32_t kClockFramesPerSecond = 1000 / kClockFrameMs;

/// The parts of the touch controls a failure points at.
enum class Lane : uint8_t {
    dispatch,  ///< the finger dispatcher, its gestures' actions, the camera and the modifiers
    touch_ui,  ///< the gesture recogniser and the controls' layout
    draw,      ///< the touch layer's drawing
    phone,     ///< the phone layout and its placed regions
    fullbleed, ///< the battlefield overlays kept clear of the controls
    settings,  ///< the Touch section of the settings
    platform,  ///< the app lifecycle and the platform's services
    check,     ///< this check's own setup
};

/// Returns the name a failure message gives a part.
///
/// @param lane the part
/// @return its name
const char* lane_name(Lane lane) noexcept {
    switch (lane) {
    case Lane::dispatch:
        return "dispatch";
    case Lane::touch_ui:
        return "touch-ui";
    case Lane::draw:
        return "draw";
    case Lane::phone:
        return "phone";
    case Lane::fullbleed:
        return "fullbleed";
    case Lane::settings:
        return "settings";
    case Lane::platform:
        return "platform";
    case Lane::check:
        return "touch-check";
    }
    return "touch-check";
}

/// A case's failure: what is missing and the part that owns it.
class CaseFailure : public std::runtime_error {
  public:

    /// Makes the failure.
    ///
    /// @param lane the part that owns the missing piece
    /// @param what what went wrong
    CaseFailure(Lane lane, const std::string& what)
        : std::runtime_error(what + " [" + lane_name(lane) + "]"), lane_(lane) {}

    /// Returns the part that owns the missing piece.
    ///
    /// @return the part
    [[nodiscard]] Lane lane() const noexcept { return lane_; }

  private:

    Lane lane_;
};

/// Fails the running case.
///
/// @param lane the part that owns the missing piece
/// @param what what went wrong
[[noreturn]] void fail(Lane lane, std::string_view what) {
    throw CaseFailure(lane, std::string(what));
}

/// Fails the running case unless a condition holds.
///
/// @param ok the condition
/// @param lane the part that owns the missing piece
/// @param what what went wrong when it does not hold
void require(bool ok, Lane lane, std::string_view what) {
    if (!ok)
        fail(lane, what);
}

/// A point on the canvas, in pixels.
struct CanvasPoint {
    float x{};
    float y{};
};

/// Returns the centre of a rectangle.
///
/// @param rect the rectangle
/// @return its centre
CanvasPoint centre_of(const layout::Rect& rect) noexcept {
    return {
        static_cast<float>(rect.x) + static_cast<float>(rect.width) / 2.0F,
        static_cast<float>(rect.y) + static_cast<float>(rect.height) / 2.0F
    };
}

/// Returns whether a rectangle has an area.
///
/// @param rect the rectangle
/// @return whether both sides are positive
bool has_area(const layout::Rect& rect) noexcept {
    return rect.width > 0 && rect.height > 0;
}

/// Returns whether a changed box is large enough to hold a line of text: a few letters wide
/// and a letter high.
///
/// @param box the box of what changed
/// @return whether it is
bool holds_text(const layout::Rect& box) noexcept {
    return box.width >= 24 && box.height >= 6;
}

/// Returns whether one rectangle lies inside another, within a slack.
///
/// @param inner the rectangle that should lie inside
/// @param outer the rectangle it should lie in
/// @param slack pixels inner may reach past outer on each side
/// @return whether it does
bool rect_inside(const layout::Rect& inner, const layout::Rect& outer, int slack = 0) noexcept {
    return inner.x >= outer.x - slack && inner.y >= outer.y - slack &&
           inner.x + inner.width <= outer.x + outer.width + slack &&
           inner.y + inner.height <= outer.y + outer.height + slack;
}

/// Returns whether two rectangles share pixels.
///
/// @param a one rectangle
/// @param b the other
/// @return whether they overlap
bool rects_overlap(const layout::Rect& a, const layout::Rect& b) noexcept {
    return has_area(a) && has_area(b) && a.x < b.x + b.width && b.x < a.x + a.width &&
           a.y < b.y + b.height && b.y < a.y + a.height;
}

/// Formats a rectangle as "x,y wxh".
///
/// @param rect the rectangle
/// @return the text
std::string rect_text(const layout::Rect& rect) {
    std::ostringstream text;
    text << rect.x << ',' << rect.y << ' ' << rect.width << 'x' << rect.height;
    return text.str();
}

/// The bounding box of the pixels that differ between two frames of one size.
///
/// @param before one frame
/// @param after the other
/// @param ignore pixels that differ by themselves between two frames of nothing changed
/// @return the box; empty when no pixel differs
layout::Rect changed_box(
    const renderer::Surface& before, const renderer::Surface& after, const std::vector<bool>& ignore
) {
    layout::Rect box{};
    if (before.width != after.width || before.height != after.height)
        return box;
    int left = static_cast<int>(before.width);
    int top = static_cast<int>(before.height);
    int right = -1;
    int bottom = -1;
    for (uint32_t y = 0; y < before.height; ++y)
        for (uint32_t x = 0; x < before.width; ++x) {
            const auto at = static_cast<std::size_t>(y) * before.width + x;
            if (at < ignore.size() && ignore[at])
                continue;
            const auto* a = before.rgb.data() + at * 3U;
            const auto* b = after.rgb.data() + at * 3U;
            if (a[0] == b[0] && a[1] == b[1] && a[2] == b[2])
                continue;
            left = std::min(left, static_cast<int>(x));
            top = std::min(top, static_cast<int>(y));
            right = std::max(right, static_cast<int>(x));
            bottom = std::max(bottom, static_cast<int>(y));
        }
    if (right < 0)
        return box;
    box.x = left;
    box.y = top;
    box.width = right - left + 1;
    box.height = bottom - top + 1;
    return box;
}

/// Marks the pixels that differ between two frames of nothing changed.
///
/// @param a one frame
/// @param b the other
/// @return one flag a pixel
std::vector<bool> restless_pixels(const renderer::Surface& a, const renderer::Surface& b) {
    std::vector<bool> marks(static_cast<std::size_t>(a.width) * a.height, false);
    if (a.width != b.width || a.height != b.height)
        return marks;
    for (std::size_t at = 0; at < marks.size(); ++at) {
        const auto* p = a.rgb.data() + at * 3U;
        const auto* q = b.rgb.data() + at * 3U;
        marks[at] = p[0] != q[0] || p[1] != q[1] || p[2] != q[2];
    }
    return marks;
}

/// A finger the check holds down.
struct HeldFinger {
    uint64_t id{};                         ///< the finger's id
    SDL_TouchID device{kCheckTouchDevice}; ///< the touch device it came from
    float x{};                             ///< last canvas point
    float y{};                             ///< last canvas point
};

/// One case's outcome.
struct CaseResult {
    std::string name;    ///< the case's number and name
    bool passed{};       ///< whether every assertion held
    std::string message; ///< the failure, empty when it passed
};

/// The check's own state through the run.
struct TouchRun {
    bool running{true};                ///< cleared when an event ends the run
    uint64_t clock_ns{};               ///< the check's clock, which the fingers' times come from
    uint64_t next_finger{1};           ///< the id the next finger gets
    SDL_WindowID window{};             ///< the window the fingers touch
    bool phone{};                      ///< the window is phone class
    float px_per_point{1.0F};          ///< canvas pixels per window point
    float start_zoom_target{1.0F};     ///< the zoom target the match started with
    uint16_t commander{};              ///< the local commander
    std::array<uint16_t, 3> peewees{}; ///< three Peewees beside the commander
    uint16_t lab{};                    ///< a finished Kbot Lab beside the commander
    std::vector<HeldFinger> held;      ///< fingers down now
    std::vector<CaseResult> results;   ///< every case's outcome, in order
    std::vector<std::string> notes;    ///< what the snapshots show, and cases left with a note
};

} // namespace

/// The touch check's helpers that reach the runtime's private members: static functions that
/// take Runtime&.
struct TouchCheckAccess {
    using CaseBody = void (*)(Runtime&, TouchRun&);

    // ---- The clock, the frame and the fingers ------------------------------------------

    /// Advances the check's clock and runs one frame as run_frame does: the queued events
    /// through dispatch_event, then idle_tick.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param ms milliseconds the frame advances the clock
    static void step(Runtime& runtime, TouchRun& run, uint64_t ms) {
        run.clock_ns += ms * kNanosecondsPerMillisecond;
        runtime.touch_state().dispatch.check_clock_ns = run.clock_ns;
        SDL_Event event{};
        while (SDL_PollEvent(&event))
            runtime.dispatch_event(event, run.running);
        runtime.idle_tick();
    }

    /// Runs frames for a time.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param total_ms milliseconds of the check's clock to run
    /// @param each_ms milliseconds each frame advances
    static void steps(Runtime& runtime, TouchRun& run, uint64_t total_ms, uint64_t each_ms) {
        for (uint64_t elapsed = 0; elapsed < total_ms; elapsed += each_ms)
            step(runtime, run, std::min(each_ms, total_ms - elapsed));
    }

    /// Sends one finger event through dispatch_event, its point in normalised window
    /// coordinates as SDL reports a finger.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param type SDL_EVENT_FINGER_DOWN, _MOTION, _UP or _CANCELED
    /// @param finger the finger
    static void
    send_finger(Runtime& runtime, TouchRun& run, SDL_EventType type, HeldFinger finger) {
        float window_x = finger.x;
        float window_y = finger.y;
        if (!frame_to_window(runtime.sdl_.renderer, finger.x, finger.y, &window_x, &window_y))
            fail(
                Lane::check, std::string("cannot place a finger on the window: ") + SDL_GetError()
            );
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSize(runtime.sdl_.window, &width, &height) || width <= 0 || height <= 0)
            fail(Lane::check, std::string("the window has no size: ") + SDL_GetError());
        SDL_Event event{};
        event.type = type;
        event.tfinger.timestamp = run.clock_ns;
        event.tfinger.touchID = finger.device;
        event.tfinger.fingerID = finger.id;
        event.tfinger.x = window_x / static_cast<float>(width);
        event.tfinger.y = window_y / static_cast<float>(height);
        event.tfinger.pressure = type == SDL_EVENT_FINGER_UP ? 0.0F : 1.0F;
        event.tfinger.windowID = run.window;
        for (const auto& held : run.held)
            if (held.id == finger.id) {
                float held_x = held.x;
                float held_y = held.y;
                (void)frame_to_window(runtime.sdl_.renderer, held.x, held.y, &held_x, &held_y);
                event.tfinger.dx = (window_x - held_x) / static_cast<float>(width);
                event.tfinger.dy = (window_y - held_y) / static_cast<float>(height);
            }
        runtime.dispatch_event(event, run.running);
    }

    /// Lands a finger.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param point canvas point
    /// @param device the touch device it comes from
    /// @return the finger's id
    static uint64_t press(
        Runtime& runtime, TouchRun& run, CanvasPoint point, SDL_TouchID device = kCheckTouchDevice
    ) {
        HeldFinger finger{run.next_finger++, device, point.x, point.y};
        send_finger(runtime, run, SDL_EVENT_FINGER_DOWN, finger);
        run.held.push_back(finger);
        return finger.id;
    }

    /// Returns a finger the check holds.
    ///
    /// @param run the check's state
    /// @param id the finger's id
    /// @return the finger
    static HeldFinger& held(TouchRun& run, uint64_t id) {
        for (auto& finger : run.held)
            if (finger.id == id)
                return finger;
        fail(Lane::check, "moved a finger that is not down");
    }

    /// Moves a finger that is down.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param id the finger
    /// @param point the canvas point it moves to
    static void move(Runtime& runtime, TouchRun& run, uint64_t id, CanvasPoint point) {
        auto finger = held(run, id);
        finger.x = point.x;
        finger.y = point.y;
        send_finger(runtime, run, SDL_EVENT_FINGER_MOTION, finger);
        auto& kept = held(run, id);
        kept.x = point.x;
        kept.y = point.y;
    }

    /// Lifts a finger where it is.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param id the finger
    static void lift(Runtime& runtime, TouchRun& run, uint64_t id) {
        const auto finger = held(run, id);
        std::erase_if(run.held, [id](const HeldFinger& other) { return other.id == id; });
        send_finger(runtime, run, SDL_EVENT_FINGER_UP, finger);
    }

    /// Cancels every finger still down, as the system does when it takes the touches.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    static void cancel_all(Runtime& runtime, TouchRun& run) {
        const auto fingers = run.held;
        run.held.clear();
        for (const auto& finger : fingers)
            send_finger(runtime, run, SDL_EVENT_FINGER_CANCELED, finger);
    }

    /// Taps: lands a finger, rests it, lifts it, then waits out the double tap.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param point canvas point
    static void tap(Runtime& runtime, TouchRun& run, CanvasPoint point) {
        const auto finger = press(runtime, run, point);
        step(runtime, run, kTapRestMs);
        lift(runtime, run, finger);
        step(runtime, run, kAfterTapMs);
    }

    /// Holds a finger still past the hold delay, then lifts it.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param point canvas point
    static void hold_release(Runtime& runtime, TouchRun& run, CanvasPoint point) {
        const auto finger = press(runtime, run, point);
        steps(runtime, run, kHoldMs, kHoldStepMs);
        lift(runtime, run, finger);
        step(runtime, run, kFrameMs);
    }

    /// Taps twice at one point within the double tap's window, then waits it out.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param point canvas point
    static void double_tap(Runtime& runtime, TouchRun& run, CanvasPoint point) {
        const auto first = press(runtime, run, point);
        step(runtime, run, kTapRestMs);
        lift(runtime, run, first);
        step(runtime, run, kTapRestMs);
        const auto second = press(runtime, run, point);
        step(runtime, run, kTapRestMs);
        lift(runtime, run, second);
        step(runtime, run, kAfterTapMs);
    }

    /// Returns a point on the way from one point to another.
    ///
    /// @param from the start
    /// @param to the end
    /// @param part how far, 0 to 1
    /// @return the point
    static CanvasPoint between(CanvasPoint from, CanvasPoint to, float part) noexcept {
        return {from.x + (to.x - from.x) * part, from.y + (to.y - from.y) * part};
    }

    /// Drags one finger from one point to another, first past the slop, then in even steps;
    /// optionally holding it first.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param from the start
    /// @param to the end
    /// @param hold_first whether the finger rests past the hold delay before it moves
    /// @param lift_at_end whether the finger lifts at the end
    /// @return the finger's id (down still when lift_at_end is false)
    static uint64_t drag(
        Runtime& runtime,
        TouchRun& run,
        CanvasPoint from,
        CanvasPoint to,
        bool hold_first,
        bool lift_at_end = true
    ) {
        const auto finger = press(runtime, run, from);
        if (hold_first)
            steps(runtime, run, kHoldMs, kHoldStepMs);
        else
            step(runtime, run, kFrameMs);
        const float distance = std::hypot(to.x - from.x, to.y - from.y);
        const float first = std::min(1.0F, kDragFirstStepPoints * run.px_per_point / distance);
        move(runtime, run, finger, between(from, to, first));
        step(runtime, run, kFrameMs);
        for (int index = 1; index <= kDragSteps; ++index) {
            move(
                runtime,
                run,
                finger,
                between(
                    from,
                    to,
                    first +
                        (1.0F - first) * static_cast<float>(index) / static_cast<float>(kDragSteps)
                )
            );
            step(runtime, run, kFrameMs);
        }
        if (lift_at_end) {
            lift(runtime, run, finger);
            step(runtime, run, kFrameMs);
        }
        return finger;
    }

    /// Moves two fingers in one step, the first one first on even steps and the other first on
    /// odd ones, as a device reports two moving fingers in either order.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param index the step's number
    /// @param first one finger
    /// @param first_to where it moves
    /// @param second the other finger
    /// @param second_to where it moves
    static void move_two(
        Runtime& runtime,
        TouchRun& run,
        int index,
        uint64_t first,
        CanvasPoint first_to,
        uint64_t second,
        CanvasPoint second_to
    ) {
        if (index % 2 == 0) {
            move(runtime, run, first, first_to);
            move(runtime, run, second, second_to);
        } else {
            move(runtime, run, second, second_to);
            move(runtime, run, first, first_to);
        }
    }

    /// Taps with two fingers together.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param point the point between the fingers
    static void two_finger_tap(Runtime& runtime, TouchRun& run, CanvasPoint point) {
        const float apart = kTwoFingerSpreadPoints * run.px_per_point;
        const auto first = press(runtime, run, {point.x - apart / 2.0F, point.y});
        const auto second = press(runtime, run, {point.x + apart / 2.0F, point.y});
        step(runtime, run, kTapRestMs);
        lift(runtime, run, first);
        lift(runtime, run, second);
        step(runtime, run, kAfterTapMs);
    }

    /// Sends a key through dispatch_event, down then up, with modifiers held.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param key the key
    /// @param scancode its scancode
    /// @param mods modifiers held, reported by SDL_GetModState too
    static void
    key(Runtime& runtime, TouchRun& run, SDL_Keycode key, SDL_Scancode scancode, SDL_Keymod mods) {
        for (const bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = run.window;
            event.key.key = key;
            event.key.scancode = scancode;
            event.key.mod = mods;
            event.key.down = down;
            SDL_SetModState(mods);
            runtime.dispatch_event(event, run.running);
            SDL_SetModState(SDL_KMOD_NONE);
        }
    }

    /// Clicks a mouse button at a canvas point as a real mouse does, with modifiers held.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param point canvas point
    /// @param mods modifiers held
    /// @param which the mouse the events come from (SDL_TOUCH_MOUSEID for SDL's own finger
    ///        mouse)
    static void mouse_click(
        Runtime& runtime, TouchRun& run, CanvasPoint point, SDL_Keymod mods, SDL_MouseID which = 0
    ) {
        float window_x = 0.0F;
        float window_y = 0.0F;
        if (!frame_to_window(runtime.sdl_.renderer, point.x, point.y, &window_x, &window_y))
            fail(Lane::check, SDL_GetError());
        for (const auto type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event{};
            event.type = type;
            if (type == SDL_EVENT_MOUSE_MOTION) {
                event.motion.windowID = run.window;
                event.motion.which = which;
                event.motion.x = window_x;
                event.motion.y = window_y;
            } else {
                event.button.windowID = run.window;
                event.button.which = which;
                event.button.button = SDL_BUTTON_LEFT;
                event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                event.button.clicks = 1;
                event.button.x = window_x;
                event.button.y = window_y;
            }
            SDL_SetModState(mods);
            runtime.dispatch_event(event, run.running);
            SDL_SetModState(SDL_KMOD_NONE);
        }
    }

    // ---- The controls ---------------------------------------------------------------------

    /// Returns the touch state.
    ///
    /// @param runtime the runtime
    /// @return the state
    static Runtime::TouchState& touch(Runtime& runtime) { return runtime.touch_state(); }

    /// Requires the controls' frame to be laid out.
    ///
    /// @param runtime the runtime
    static void require_frame(Runtime& runtime) {
        require(
            touch(runtime).frame_ready,
            Lane::dispatch,
            "the touch controls' frame was never laid out (tick_touch lays it out each frame)"
        );
    }

    /// Returns a laid-out control, if the frame has it.
    ///
    /// @param runtime the runtime
    /// @param control the control
    /// @param index its index, or -1 for any
    /// @return the control, or null
    static const hud::ControlRect* find_control(Runtime& runtime, hud::Control control, int index) {
        const auto& frame = touch(runtime).frame;
        for (std::size_t at = 0; at < frame.control_count && at < frame.controls.size(); ++at)
            if (frame.controls[at].control == control &&
                (index < 0 || frame.controls[at].index == index))
                return &frame.controls[at];
        return nullptr;
    }

    /// Returns the centre of a laid-out control, failing the case without it.
    ///
    /// @param runtime the runtime
    /// @param control the control
    /// @param index its index, or -1 for any
    /// @param name what the control is called, for the message
    /// @param lane the part that lays the control out when it shows
    /// @return its centre
    static CanvasPoint control_point(
        Runtime& runtime,
        hud::Control control,
        int index,
        std::string_view name,
        Lane lane = Lane::touch_ui
    ) {
        require_frame(runtime);
        const auto* found = find_control(runtime, control, index);
        require(found != nullptr, lane, "the controls' frame has no " + std::string(name));
        return centre_of(found->rect);
    }

    /// Taps a laid-out control.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param control the control
    /// @param index its index, or -1 for any
    /// @param name what the control is called, for the message
    /// @param lane the part that lays the control out when it shows
    static void tap_control(
        Runtime& runtime,
        TouchRun& run,
        hud::Control control,
        int index,
        std::string_view name,
        Lane lane = Lane::touch_ui
    ) {
        tap(runtime, run, control_point(runtime, control, index, name, lane));
    }

    /// Returns the fat finger's reach for gadgets and controls, in canvas pixels.
    ///
    /// @param run the check's state
    /// @return pixels
    static int gadget_reach(const TouchRun& run) noexcept {
        return static_cast<int>(std::ceil(hud::gadget_pick_points * run.px_per_point));
    }

    // ---- Units, the camera and the ground -------------------------------------------------

    /// Returns where a unit is on the canvas.
    ///
    /// @param runtime the runtime
    /// @param id the unit
    /// @return canvas point
    static CanvasPoint canvas_of(Runtime& runtime, uint16_t id) {
        const auto& slots = runtime.match_->world().slots;
        const auto viewport =
            runtime.live_viewport(runtime.match_camera_x_, runtime.match_camera_z_);
        const auto point = runtime.project_match_point(viewport, slots[id].unit->position);
        return {static_cast<float>(point.x), static_cast<float>(point.y)};
    }

    /// Returns whether a unit is selected.
    ///
    /// @param runtime the runtime
    /// @param id the unit
    /// @return whether it is
    static bool selected(Runtime& runtime, uint16_t id) {
        return (runtime.match_->world().slots[id].unit->flags & OA_UNIT_FLAG_SELECTED) != 0;
    }

    /// Returns a unit's primary order queue.
    ///
    /// @param runtime the runtime
    /// @param id the unit
    /// @return the orders, head first
    static std::vector<orders::Match::QueuedCommandView> queue_of(Runtime& runtime, uint16_t id) {
        std::vector<orders::Match::QueuedCommandView> queue;
        runtime.match_->visit_primary_queue(id, [&](const auto& view) { queue.push_back(view); });
        return queue;
    }

    /// Returns whether a queue is exactly moves to these points.
    ///
    /// @param queue the queue
    /// @param points the points, in order
    /// @return whether it is
    static bool moves_to(
        const std::vector<orders::Match::QueuedCommandView>& queue,
        const std::vector<oa::sim::ground_orders::Point>& points
    ) {
        if (queue.size() != points.size())
            return false;
        for (std::size_t at = 0; at < queue.size(); ++at)
            if (queue[at].kind != oa::sim::ground_orders::move_ground_kind ||
                queue[at].destination != points[at])
                return false;
        return true;
    }

    /// Selects one unit alone through the engine, as the cases' known state.
    ///
    /// @param runtime the runtime
    /// @param id the unit
    static void select_only(Runtime& runtime, uint16_t id) {
        runtime.clear_local_selection();
        runtime.reset_match_command();
        runtime.pending_build_type_ = 0;
        runtime.adopt_selection(id);
        runtime.selected_match_unit_ = id;
        runtime.apply_match_hud_for_selection();
    }

    /// Returns a unit type by name, failing the case without it.
    ///
    /// @param runtime the runtime
    /// @param name the type's name
    /// @return its index
    static uint16_t type_of(Runtime& runtime, std::string_view name) {
        const auto type = oa::sim::unit_spawn::find_type_index(runtime.spawn_type_names_, name);
        require(
            type != 0 && type < runtime.spawn_types_.size(),
            Lane::check,
            "the game data lacks " + std::string(name)
        );
        return type;
    }

    /// Places a finished unit of the local player's, holding fire.
    ///
    /// @param runtime the runtime
    /// @param name the type's name
    /// @param x map pixel column
    /// @param z map pixel row
    /// @return the unit
    static uint16_t spawn(Runtime& runtime, std::string_view name, int32_t x, int32_t z) {
        const auto map_width = static_cast<int32_t>(runtime.selected_tnt_->tile_width * 32U);
        const auto map_height = static_cast<int32_t>(runtime.selected_tnt_->tile_height * 32U);
        x = std::clamp(x, kEdgeMargin, map_width - kEdgeMargin);
        z = std::clamp(z, kEdgeMargin, map_height - kEdgeMargin);
        oa::sim::unit_spawn::Request request;
        request.player = runtime.match_local_player_;
        request.type = type_of(runtime, name);
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>(x) << 16,
            static_cast<uint32_t>(runtime.match_->map_height(
                static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16
            )) << 16,
            static_cast<uint32_t>(z) << 16
        };
        auto* slot = runtime.match_->create(request);
        require(
            slot != nullptr && slot->unit != nullptr,
            Lane::check,
            "could not place " + std::string(name)
        );
        slot->unit->object_present = true;
        slot->record.flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
        return slot->unit_index;
    }

    /// Returns a unit's map pixel column and row.
    ///
    /// @param runtime the runtime
    /// @param id the unit
    /// @return column, row
    static std::pair<int32_t, int32_t> map_of(Runtime& runtime, uint16_t id) {
        const auto& unit = *runtime.match_->world().slots[id].unit;
        return {
            static_cast<int32_t>(unit.position[0] >> 16),
            static_cast<int32_t>(unit.position[2] >> 16)
        };
    }

    /// Centres the view on a map point and draws a frame, which holds the view on the map.
    ///
    /// @param runtime the runtime
    /// @param x map pixel column
    /// @param z map pixel row
    static void look_at(Runtime& runtime, int32_t x, int32_t z) {
        runtime.set_camera_position(
            x - runtime.visible_map_width() / 2, z - runtime.visible_map_height() / 2, 0
        );
        runtime.render_match_surface();
    }

    /// Centres the view between units.
    ///
    /// @param runtime the runtime
    /// @param ids the units
    static void look_at_units(Runtime& runtime, std::initializer_list<uint16_t> ids) {
        int64_t x = 0;
        int64_t z = 0;
        for (const auto id : ids) {
            const auto [ux, uz] = map_of(runtime, id);
            x += ux;
            z += uz;
        }
        const auto count = static_cast<int64_t>(std::max<std::size_t>(1, ids.size()));
        look_at(runtime, static_cast<int32_t>(x / count), static_cast<int32_t>(z / count));
    }

    /// Returns whether a canvas point is battlefield a finger would land on: on the
    /// battlefield, clear of every placed region and of the controls' reach, open ground under
    /// it and, when asked, no unit within the unit pick's reach.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param point canvas point
    /// @param unit_free whether no unit may lie within the pick's reach
    /// @return whether it is
    static bool
    clear_ground(Runtime& runtime, const TouchRun& run, CanvasPoint point, bool unit_free) {
        if (!runtime.battlefield_contains(point.x, point.y) ||
            runtime.placed_hud_covers(point.x, point.y))
            return false;
        const auto& state = touch(runtime);
        const layout::Point at{static_cast<int>(point.x), static_cast<int>(point.y)};
        if (state.frame_ready &&
            (hud::covers(state.frame, at) || hud::hit(state.frame, at, gadget_reach(run))))
            return false;
        runtime.update_pointer(point.x, point.y);
        if (runtime.hovered_match_unit_ != 0 || !runtime.match_world_point(point.x, point.y))
            return false;
        if (!unit_free)
            return true;
        for (const float reach : {4.0F, 8.0F, kPickReachPoints + 2.0F})
            for (int direction = 0; direction < 8; ++direction) {
                const float angle = static_cast<float>(direction) * 0.785398F;
                const float px = point.x + std::cos(angle) * reach * run.px_per_point;
                const float py = point.y + std::sin(angle) * reach * run.px_per_point;
                runtime.update_pointer(px, py);
                if (runtime.hovered_match_unit_ != 0)
                    return false;
            }
        return true;
    }

    /// Finds clear ground near a canvas point, searching rings around it.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param want the point wanted
    /// @return the nearest clear point found
    static CanvasPoint open_ground_near(Runtime& runtime, const TouchRun& run, CanvasPoint want) {
        const float step_px = 8.0F * run.px_per_point;
        for (int ring = 0; ring < 24; ++ring) {
            const int around = ring == 0 ? 1 : 8 + ring * 4;
            for (int at = 0; at < around; ++at) {
                const float angle =
                    6.2831853F * static_cast<float>(at) / static_cast<float>(around);
                const CanvasPoint point{
                    want.x + std::cos(angle) * step_px * static_cast<float>(ring),
                    want.y + std::sin(angle) * step_px * static_cast<float>(ring)
                };
                if (clear_ground(runtime, run, point, true))
                    return point;
            }
        }
        fail(Lane::check, "found no open ground clear of the controls near a test point");
    }

    /// Returns the map point under a canvas point exactly, in map pixels, as the drawn view
    /// shows it.
    ///
    /// @param runtime the runtime
    /// @param point canvas point
    /// @return map column and row
    static std::array<double, 2> map_point(Runtime& runtime, CanvasPoint point) {
        const auto camera = runtime.view_camera();
        const auto offset = runtime.view_offset();
        const double zoom = runtime.match_zoom() > 0.0F ? runtime.match_zoom() : 1.0F;
        return {
            static_cast<double>(camera[0]) + offset.x +
                (static_cast<double>(point.x) - runtime.match_layout_.left) / zoom,
            static_cast<double>(camera[1]) + offset.y +
                (static_cast<double>(point.y) - runtime.match_layout_.top) / zoom
        };
    }

    /// Returns the centre of the battlefield's clear part: where the overlays go.
    ///
    /// @param runtime the runtime
    /// @return canvas point
    static CanvasPoint clear_centre(Runtime& runtime) { return centre_of(runtime.overlay_area()); }

    /// Returns the index of the loaded HUD page's gadget with a name.
    ///
    /// @param runtime the runtime
    /// @param name the gadget's name
    /// @return the index, or none
    static std::optional<std::size_t> gadget_named(Runtime& runtime, std::string_view name) {
        if (!runtime.match_hud_)
            return std::nullopt;
        const auto& gadgets = runtime.match_hud_->layout.gadgets;
        for (std::size_t at = 0; at < gadgets.size(); ++at)
            if (gadgets[at].common.name == name)
                return at;
        return std::nullopt;
    }

    /// Returns the index of the loaded HUD page's gadget whose name holds a word.
    ///
    /// @param runtime the runtime
    /// @param word the word
    /// @return the index, or none
    static std::optional<std::size_t> gadget_holding(Runtime& runtime, std::string_view word) {
        if (!runtime.match_hud_)
            return std::nullopt;
        const auto& gadgets = runtime.match_hud_->layout.gadgets;
        for (std::size_t at = 1; at < gadgets.size(); ++at)
            if (gadgets[at].common.name.find(word) != std::string::npos)
                return at;
        return std::nullopt;
    }

    /// Returns the canvas centre of a gadget of the 3.1c panel.
    ///
    /// @param runtime the runtime
    /// @param index the gadget
    /// @return canvas point
    static CanvasPoint gadget_centre(Runtime& runtime, std::size_t index) {
        const auto& gadget = runtime.match_hud_->layout.gadgets[index];
        const auto point = layout::source_to_canvas(
            runtime.match_layout_,
            gadget.common.x + gadget.common.width / 2,
            gadget.common.y + gadget.common.height / 2
        );
        return {static_cast<float>(point.x), static_cast<float>(point.y)};
    }

    /// Returns the drawer cell that shows a gadget of the loaded page, failing without it.
    ///
    /// @param runtime the runtime
    /// @param name the gadget's name
    /// @return the cell's centre
    static CanvasPoint drawer_cell(Runtime& runtime, std::string_view name) {
        const auto gadgets = runtime.drawer_sheet_gadgets();
        const auto wanted = gadget_named(runtime, name);
        require(
            wanted.has_value(), Lane::dispatch, "the drawer's page has no " + std::string(name)
        );
        std::optional<std::size_t> cell;
        for (std::size_t at = 0; at < gadgets.button_count && at < gadgets.buttons.size(); ++at)
            if (gadgets.buttons[at] == static_cast<int16_t>(*wanted))
                cell = at;
        require(
            cell.has_value(),
            Lane::phone,
            "drawer_sheet_gadgets lists no " + std::string(name) + " of the loaded build page"
        );
        const auto& frame = touch(runtime).frame;
        require(
            *cell < frame.drawer_cell_count,
            Lane::touch_ui,
            "the drawer lays out " + std::to_string(frame.drawer_cell_count) +
                " cells, too few for the page's gadgets (drawer_cells_wanted)"
        );
        bool placed = false;
        for (std::size_t at = 0; at < runtime.match_layout_.placed_count; ++at) {
            const auto& region = runtime.match_layout_.placed[at];
            placed |= region.gadget == static_cast<int16_t>(*wanted) &&
                      rects_overlap(region.canvas, frame.drawer_cells[*cell]);
        }
        require(
            placed,
            Lane::phone,
            "no placed region draws " + std::string(name) + " in its drawer cell"
        );
        return centre_of(frame.drawer_cells[*cell]);
    }

    /// Finds a legal site for a building near the commander whose ghost anchor lies on clear
    /// battlefield, with a finger's room under it.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param type the building
    /// @param skip a site to pass over, or none
    /// @return the site's canvas point
    static CanvasPoint legal_site_point(
        Runtime& runtime,
        const TouchRun& run,
        uint16_t type,
        std::optional<CanvasPoint> skip = std::nullopt
    ) {
        const auto saved_type = runtime.pending_build_type_;
        const auto [unit_x, unit_z] = map_of(runtime, run.commander);
        const int32_t cell_x = unit_x / 16;
        const int32_t cell_z = unit_z / 16;
        const auto& building = runtime.spawn_types_[type];
        const auto viewport =
            runtime.live_viewport(runtime.match_camera_x_, runtime.match_camera_z_);
        std::optional<CanvasPoint> found;
        for (int32_t ring = 4; ring < 24 && !found; ++ring)
            for (int32_t dz = -ring; dz <= ring && !found; dz += 2)
                for (int32_t dx = -ring; dx <= ring && !found; dx += 2) {
                    if (dx != -ring && dx != ring && dz != -ring && dz != ring)
                        continue;
                    const oa::sim::ground_orders::Point centre{
                        ((cell_x + dx) * 16 + building.footprint_x * 8) << 16,
                        0,
                        ((cell_z + dz) * 16 + building.footprint_z * 8) << 16
                    };
                    runtime.pending_build_type_ = type;
                    const auto site = runtime.pending_build_site(centre);
                    if (!site || !site->legal)
                        continue;
                    const auto screen = runtime.project_match_point(
                        viewport,
                        {static_cast<uint32_t>(site->world[0]),
                         static_cast<uint32_t>(site->world[1]),
                         static_cast<uint32_t>(site->world[2])}
                    );
                    const CanvasPoint point{
                        static_cast<float>(screen.x), static_cast<float>(screen.y)
                    };
                    if (skip && std::abs(point.x - skip->x) < 48.0F * run.px_per_point &&
                        std::abs(point.y - skip->y) < 48.0F * run.px_per_point)
                        continue;
                    runtime.pending_build_type_ = saved_type;
                    // The finger that moves the ghost there rests on it.
                    if (!clear_ground(runtime, run, point, false))
                        continue;
                    // The site under the anchor is the one found.
                    runtime.pending_build_type_ = type;
                    const auto under = runtime.build_site_under(point.x, point.y);
                    if (under && under->legal && under->world == site->world)
                        found = point;
                }
        runtime.pending_build_type_ = saved_type;
        require(found.has_value(), Lane::check, "found no legal building site in view");
        return *found;
    }

    /// Returns the build site a ghost anchored at a canvas point builds on, as the engine's
    /// placement reads a point.
    ///
    /// @param runtime the runtime
    /// @param point canvas point
    /// @return the site, or none
    static std::optional<Runtime::PendingBuildSite> site_at(Runtime& runtime, CanvasPoint point) {
        if (const auto snapped = runtime.snapped_build_site(point.x, point.y))
            return snapped;
        return runtime.build_site_under(point.x, point.y);
    }

    /// Returns whether a finger at a canvas point lands on the battlefield: on it, clear of
    /// every placed region and of the controls' reach.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param point canvas point
    /// @return whether it does
    static bool on_battlefield(Runtime& runtime, const TouchRun& run, CanvasPoint point) {
        if (!runtime.battlefield_contains(point.x, point.y) ||
            runtime.placed_hud_covers(point.x, point.y))
            return false;
        const auto& state = touch(runtime);
        const layout::Point at{static_cast<int>(point.x), static_cast<int>(point.y)};
        return !state.frame_ready ||
               (!hud::covers(state.frame, at) && !hud::hit(state.frame, at, gadget_reach(run)));
    }

    /// Finds a point on the battlefield where the building being placed is refused: on the Kbot
    /// Lab beside the commander.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @return the point's canvas point
    static CanvasPoint refused_site_point(Runtime& runtime, const TouchRun& run) {
        const auto lab = canvas_of(runtime, run.lab);
        const float step_px = 4.0F * run.px_per_point;
        for (int ring = 0; ring < 12; ++ring) {
            const int around = ring == 0 ? 1 : 8 * ring;
            for (int at = 0; at < around; ++at) {
                const float angle =
                    6.2831853F * static_cast<float>(at) / static_cast<float>(around);
                const CanvasPoint point{
                    lab.x + std::cos(angle) * step_px * static_cast<float>(ring),
                    lab.y + std::sin(angle) * step_px * static_cast<float>(ring)
                };
                if (!on_battlefield(runtime, run, point))
                    continue;
                if (const auto site = site_at(runtime, point); site && !site->legal)
                    return point;
            }
        }
        fail(Lane::check, "found no refused building site on the Kbot Lab");
    }

    /// Finds a point on clear ground a set distance from the ghost, inside its reach, where a
    /// building would stand on another site than the ghost's; the ghost's own point when none.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param ghost the ghost's anchor
    /// @return the point
    static CanvasPoint near_ghost_point(Runtime& runtime, const TouchRun& run, CanvasPoint ghost) {
        const auto ghost_site = site_at(runtime, ghost);
        const float off = kNearGhostPoints * run.px_per_point;
        for (int direction = 0; direction < 8; ++direction) {
            const float angle = static_cast<float>(direction) * 0.785398F;
            const CanvasPoint point{
                ghost.x + std::cos(angle) * off, ghost.y + std::sin(angle) * off
            };
            if (!clear_ground(runtime, run, point, false))
                continue;
            const auto site = site_at(runtime, point);
            if (site && ghost_site && site->world != ghost_site->world)
                return point;
        }
        return ghost;
    }

    /// Counts a unit's queued MobileBuild orders for a building.
    ///
    /// @param runtime the runtime
    /// @param id the builder
    /// @param type the building
    /// @return the count
    static std::size_t builds_of(Runtime& runtime, uint16_t id, uint16_t type) {
        std::size_t count = 0;
        for (const auto& view : queue_of(runtime, id))
            count += view.kind == orders::mobile_build_kind && view.build_type == type ? 1 : 0;
        return count;
    }

    /// Takes every queued unit of a type off a factory's queue through its button's right
    /// press.
    ///
    /// @param runtime the runtime
    /// @param factory the factory
    /// @param type the unit type
    static void empty_factory(Runtime& runtime, uint16_t factory, uint16_t type) {
        const auto button = gadget_named(runtime, runtime.spawn_type_names_.at(type));
        for (int guard = 0;
             button && guard < 64 && runtime.match_->queued_build_count(factory, type) > 0;
             ++guard)
            runtime.activate_match_hud(*button, false);
    }

    // ---- Settings, snapshots and the known state ------------------------------------------

    /// Puts the Touch settings in effect.
    ///
    /// @param runtime the runtime
    /// @param drag what a one-finger drag does
    /// @param latches whether QUEUE, ADD and x5 stay on
    /// @param left_handed whether the layout is mirrored
    static void set_touch_settings(
        Runtime& runtime, settings::TouchDrag drag, settings::TouchLatches latches, bool left_handed
    ) {
        auto chosen = runtime.engine_settings();
        if (chosen.touch_drag == drag && chosen.touch_latches == latches &&
            chosen.touch_left_handed == left_handed)
            return;
        chosen.touch_drag = drag;
        chosen.touch_latches = latches;
        chosen.touch_left_handed = left_handed;
        runtime.apply_engine_settings(chosen);
    }

    /// Writes the composed match frame to a file in the working directory.
    ///
    /// @param runtime the runtime
    /// @param name the file's name
    static void snapshot(Runtime& runtime, const char* name) {
        renderer::Surface composed;
        runtime.render_match_surface();
        runtime.compose_match_frame(composed);
        write_ppm(name, composed);
    }

    /// Composes the match frame without stepping anything.
    ///
    /// @param runtime the runtime
    /// @param frame the frame filled
    static void compose(Runtime& runtime, renderer::Surface& frame) {
        runtime.render_match_surface();
        runtime.compose_match_frame(frame);
    }

    /// Puts the check back in its known state: no finger down, no sheet, radial or latch, the
    /// Touch settings' defaults, no menu, panel or chat line, no armed order or building, no
    /// selection, the start zoom, the view on the commander.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    static void reset(Runtime& runtime, TouchRun& run) {
        cancel_all(runtime, run);
        runtime.touch_screen_changed();
        auto& state = touch(runtime);
        state.hud.radial.reset();
        state.hud.sheet = hud::Sheet::none;
        state.hud.latches.clear();
        state.hud.tip = {};
        state.hud.self_destruct_progress = 0.0F;
        ++state.hud.revision;
        if (state.safe_override) {
            state.safe_override.reset();
            runtime.apply_output_mode();
        }
        set_touch_settings(
            runtime, settings::TouchDrag::automatic, settings::TouchLatches::stay_on, false
        );
        if (runtime.chat_composing_)
            runtime.close_chat_line();
        if (runtime.unit_info_panel_)
            runtime.close_unit_info();
        if (runtime.match_paused_)
            runtime.resume_match_pause();
        auto& game = runtime.match_->state().game;
        game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags & ~console::kSimRunPaused);
        game.graphics_flags =
            static_cast<uint16_t>(game.graphics_flags & ~oa::ui::hud::kGraphicsBoardPinned);
        game.interface_type = input::interface_left_click;
        runtime.match_->stop_orders(run.commander);
        for (const auto id : run.peewees)
            runtime.match_->stop_orders(id);
        runtime.clear_local_selection();
        runtime.reset_match_command();
        runtime.pending_build_type_ = 0;
        runtime.apply_match_hud_for_selection();
        runtime.match_zoom_ = run.start_zoom_target;
        runtime.match_zoom_target_ = run.start_zoom_target;
        runtime.zoom_focus_ = {};
        runtime.match_pointer_known_ = false;
        SDL_SetModState(SDL_KMOD_NONE);
        look_at_units(runtime, {run.commander});
        steps(runtime, run, 3 * kFrameMs, kFrameMs);
    }

    /// Runs one case from the known state and records its outcome.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param name the case's number and name
    /// @param body the case
    static void run_case(Runtime& runtime, TouchRun& run, std::string name, CaseBody body) {
        CaseResult result{std::move(name), false, {}};
        try {
            reset(runtime, run);
            body(runtime, run);
            result.passed = true;
        } catch (const std::exception& error) {
            result.message = error.what();
        }
        std::cout << "touch case " << result.name << ": "
                  << (result.passed ? std::string("passed") : "FAILED: " + result.message) << '\n';
        run.results.push_back(std::move(result));
        try {
            reset(runtime, run);
        } catch (const std::exception&) {
            // The next case reports a known state it could not reach.
        }
    }

    // ---- The run ---------------------------------------------------------------------------

    /// Prepares the run after the skirmish started: the window, the class, the units the cases
    /// use and the zoom the match started with.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    static void prepare(Runtime& runtime, TouchRun& run) {
        run.window = SDL_GetWindowID(runtime.sdl_.window);
        step(runtime, run, kFrameMs);
        step(runtime, run, kFrameMs);
        run.phone = runtime.touch_phone_class();
        run.px_per_point = std::max(0.25F, static_cast<float>(runtime.match_layout_.px_per_point));
        run.start_zoom_target = runtime.match_zoom_target_;
        for (const auto& slot : runtime.match_->world().slots)
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                slot.record.owner_index == runtime.match_local_player_) {
                run.commander = slot.unit_index;
                break;
            }
        if (run.commander == 0)
            throw std::runtime_error("touch controls check: found no local commander");
        const auto map_width = static_cast<int32_t>(runtime.selected_tnt_->tile_width * 32U);
        const auto map_height = static_cast<int32_t>(runtime.selected_tnt_->tile_height * 32U);
        const auto [x, z] = map_of(runtime, run.commander);
        // The Peewees go toward the middle of the map, side by side, a third below them.
        const int32_t way_x = x < map_width / 2 ? 1 : -1;
        const int32_t way_z = z < map_height / 2 ? 1 : -1;
        run.peewees[0] = spawn(runtime, "ARMPW", x + way_x * kUnitReach, z);
        run.peewees[1] = spawn(runtime, "ARMPW", x + way_x * (kUnitReach + kUnitSpacing), z);
        run.peewees[2] =
            spawn(runtime, "ARMPW", x + way_x * kUnitReach, z + way_z * 2 * kUnitSpacing);
        // The lab goes on the commander's other side, clear of the boxes drawn around the
        // Peewees; anywhere near it when that side has no room.
        const auto lab_type = type_of(runtime, "ARMLAB");
        const auto& lab_def = runtime.spawn_types_[lab_type];
        const int32_t cell_x = x / 16;
        const int32_t cell_z = z / 16;
        std::optional<std::pair<int32_t, int32_t>> lab_site;
        for (int32_t reach = 6; reach < 40 && !lab_site; ++reach)
            for (int32_t dz = -reach; dz <= reach && !lab_site; dz += 2)
                if (runtime.match_->building_site(lab_type, cell_x - way_x * reach, cell_z + dz, 0))
                    lab_site = std::pair{cell_x - way_x * reach, cell_z + dz};
        if (lab_site) {
            run.lab = spawn(
                runtime,
                "ARMLAB",
                (lab_site->first * 2 + lab_def.footprint_x) * 8,
                (lab_site->second * 2 + lab_def.footprint_z) * 8
            );
        } else {
            const auto* lab = runtime.place_finished_structure(lab_type, run.commander);
            if (lab == nullptr)
                throw std::runtime_error("touch controls check: found no site for ARMLAB");
            run.lab = lab->unit_index;
        }
        auto& player = runtime.match_->world().players[runtime.match_local_player_];
        player.metal = player.metal_cap;
        player.energy = player.energy_cap;
    }

    /// Writes the snapshots for people before any case can fail: the layout, and the radial,
    /// placement, drawer and MORE as fingers open them. A piece a finger did not open is noted.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    static void write_snapshots(Runtime& runtime, TouchRun& run) {
        const auto attempt = [&](const char* file, auto&& body) {
            try {
                reset(runtime, run);
                body();
            } catch (const std::exception& error) {
                run.notes.push_back(std::string(file) + ": " + error.what());
                try {
                    snapshot(runtime, file);
                } catch (const std::exception&) {
                    // Nothing to compose; the note says why.
                }
            }
        };
        attempt(run.phone ? "touch-phone.ppm" : "touch-tablet.ppm", [&] {
            select_only(runtime, run.commander);
            steps(runtime, run, 2 * kFrameMs, kFrameMs);
            snapshot(runtime, run.phone ? "touch-phone.ppm" : "touch-tablet.ppm");
        });
        if (run.phone)
            attempt("touch-phone-safe.ppm", [&] {
                touch(runtime).safe_override = layout::Insets{59, 0, 59, 21};
                runtime.apply_output_mode();
                select_only(runtime, run.commander);
                steps(runtime, run, 2 * kFrameMs, kFrameMs);
                snapshot(runtime, "touch-phone-safe.ppm");
            });
        attempt("touch-radial.ppm", [&] {
            select_only(runtime, run.peewees[0]);
            look_at_units(runtime, {run.peewees[0]});
            steps(runtime, run, kFrameMs, kFrameMs);
            const auto unit = canvas_of(runtime, run.peewees[0]);
            const auto point = open_ground_near(
                runtime, run, {unit.x + kGroundOffsetPoints * run.px_per_point, unit.y}
            );
            hold_release(runtime, run, point);
            if (!touch(runtime).hud.radial)
                run.notes.push_back("touch-radial.ppm: a hold released did not open the radial");
            snapshot(runtime, "touch-radial.ppm");
        });
        attempt("touch-placement.ppm", [&] {
            select_only(runtime, run.commander);
            steps(runtime, run, kFrameMs, kFrameMs);
            const auto solar = type_of(runtime, "ARMSOLAR");
            if (run.phone) {
                tap_control(runtime, run, hud::Control::build_drawer, -1, "BUILD");
                tap(runtime, run, drawer_cell(runtime, "ARMSOLAR"));
            } else {
                const auto button = gadget_named(runtime, "ARMSOLAR");
                require(
                    button.has_value(), Lane::check, "the commander's build page has no ARMSOLAR"
                );
                tap(runtime, run, gadget_centre(runtime, *button));
            }
            if (runtime.pending_build_type_ != solar)
                run.notes.push_back("touch-placement.ppm: the tile's tap did not arm the building");
            steps(runtime, run, 2 * kFrameMs, kFrameMs);
            snapshot(runtime, "touch-placement.ppm");
        });
        attempt("touch-overlays.ppm", [&] {
            select_only(runtime, run.commander);
            key(runtime, run, SDLK_F4, SDL_SCANCODE_F4, SDL_KMOD_NONE);
            steps(runtime, run, 30 * kFrameMs, kFrameMs);
            runtime.post_match_message(
                "Touch controls check: the message log", oa::sim::messages::kind_player_chat
            );
            runtime.open_chat_line();
            runtime.chat_buffer_ = "TOUCH CONTROLS CHECK: THE CHAT LINE";
            snapshot(runtime, "touch-overlays.ppm");
            runtime.close_chat_line();
        });
        if (run.phone) {
            attempt("touch-drawer.ppm", [&] {
                select_only(runtime, run.commander);
                steps(runtime, run, kFrameMs, kFrameMs);
                tap_control(runtime, run, hud::Control::build_drawer, -1, "BUILD");
                if (touch(runtime).hud.sheet != hud::Sheet::drawer)
                    run.notes.push_back("touch-drawer.ppm: BUILD did not open the drawer");
                steps(runtime, run, 2 * kFrameMs, kFrameMs);
                snapshot(runtime, "touch-drawer.ppm");
            });
            attempt("touch-more.ppm", [&] {
                select_only(runtime, run.peewees[0]);
                steps(runtime, run, kFrameMs, kFrameMs);
                tap_control(runtime, run, hud::Control::more, -1, "MORE");
                if (touch(runtime).hud.sheet != hud::Sheet::more)
                    run.notes.push_back("touch-more.ppm: MORE did not open its sheet");
                steps(runtime, run, 2 * kFrameMs, kFrameMs);
                snapshot(runtime, "touch-more.ppm");
            });
        }
    }

    // ---- Tablet cases ---------------------------------------------------------------------

    /// 0. The tablet layout: the 3.1c layout kept, the default zoom, the thumb column, the
    /// group bar, the right rail and MENU laid out.
    static void case_tablet_layout(Runtime& runtime, TouchRun& run) {
        require(
            !runtime.match_layout_.phone && runtime.match_layout_.placed_count == 0,
            Lane::phone,
            "a tablet window was laid out in the phone's placed mode"
        );
        require(
            std::abs(run.start_zoom_target - kDefaultBattlefieldZoom) < 0.001F,
            Lane::dispatch,
            "a tablet match did not start at the default zoom"
        );
        require_frame(runtime);
        const std::array<std::pair<hud::Control, const char*>, 9> wanted{{
            {hud::Control::queue, "QUEUE"},
            {hud::Control::add, "ADD"},
            {hud::Control::clear, "CLEAR"},
            {hud::Control::group_store, "STORE"},
            {hud::Control::select_menu, "SELECT"},
            {hud::Control::pause, "PAUSE"},
            {hud::Control::centre, "CENTRE"},
            {hud::Control::info, "INFO"},
            {hud::Control::menu, "MENU"},
        }};
        const layout::Rect canvas{0, 0, runtime.match_layout_.width, runtime.match_layout_.height};
        for (const auto& [control, name] : wanted) {
            const auto* found = find_control(runtime, control, -1);
            require(
                found != nullptr, Lane::touch_ui, std::string("the tablet layout has no ") + name
            );
            require(
                rect_inside(found->rect, canvas),
                Lane::touch_ui,
                std::string(name) + " lies off the canvas at " + rect_text(found->rect)
            );
        }
    }

    /// 1. A tap on the commander selects it; a tap 8 pt off it still does, 20 pt off does not.
    static void case_tap_selects(Runtime& runtime, TouchRun& run) {
        const auto id = run.commander;
        const auto centre = canvas_of(runtime, id);
        tap(runtime, run, centre);
        require(
            selected(runtime, id) && runtime.selected_match_unit_ == id,
            Lane::dispatch,
            "a tap on the commander did not select it (battlefield tap: synthetic left click)"
        );
        // The commander's edge along a way clear of other units: the farthest point its pick
        // reaches on the rows within the pick's reach of its centre.
        const float ppp = run.px_per_point;
        const float reach = kPickReachPoints * ppp;
        const auto picks = [&](float x, float y) {
            runtime.update_pointer(x, y);
            return runtime.hovered_match_unit_ == id;
        };
        bool tried = false;
        for (const auto& way : std::array<std::pair<float, float>, 4>{
                 {{1.0F, 0.0F}, {-1.0F, 0.0F}, {0.0F, 1.0F}, {0.0F, -1.0F}}
             }) {
            float centre_edge = 0.0F;
            while (centre_edge < 200.0F * ppp &&
                   picks(centre.x + way.first * centre_edge, centre.y + way.second * centre_edge))
                centre_edge += 1.0F;
            float farthest = centre_edge;
            for (float side = -reach; side <= reach; side += 1.0F) {
                float edge = 0.0F;
                const float sx = centre.x + way.second * side;
                const float sy = centre.y + way.first * side;
                while (edge < 200.0F * ppp && picks(sx + way.first * edge, sy + way.second * edge))
                    edge += 1.0F;
                farthest = std::max(farthest, edge);
            }
            const CanvasPoint near{
                centre.x + way.first * (centre_edge + kNearMissPoints * ppp),
                centre.y + way.second * (centre_edge + kNearMissPoints * ppp)
            };
            const CanvasPoint far{
                centre.x + way.first * (farthest + kFarMissPoints * ppp),
                centre.y + way.second * (farthest + kFarMissPoints * ppp)
            };
            if (!clear_ground(runtime, run, near, false) || !clear_ground(runtime, run, far, false))
                continue;
            // Nothing else may lie within the pick's reach of either point.
            bool others = false;
            for (const auto& point : {near, far})
                for (int direction = 0; direction < 8 && !others; ++direction) {
                    const float angle = static_cast<float>(direction) * 0.785398F;
                    runtime.update_pointer(
                        point.x + std::cos(angle) * (reach + 2.0F),
                        point.y + std::sin(angle) * (reach + 2.0F)
                    );
                    others = runtime.hovered_match_unit_ != 0 && runtime.hovered_match_unit_ != id;
                }
            if (others)
                continue;
            tried = true;
            runtime.clear_local_selection();
            runtime.apply_match_hud_for_selection();
            tap(runtime, run, near);
            require(
                selected(runtime, id),
                Lane::dispatch,
                "a tap 8 pt off the commander did not take it (the 12 pt ring a tap searches)"
            );
            runtime.clear_local_selection();
            runtime.apply_match_hud_for_selection();
            tap(runtime, run, far);
            require(
                !runtime.has_local_selection(),
                Lane::dispatch,
                "a tap 20 pt off the commander took it (the ring reaches past 12 pt)"
            );
            break;
        }
        require(tried, Lane::check, "found no side of the commander clear of other units");
    }

    /// 1b. A double tap on a Peewee selects every Peewee, as a double click does.
    static void case_double_tap(Runtime& runtime, TouchRun& run) {
        const auto [a, b, c] = run.peewees;
        look_at_units(runtime, {a, b, c});
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto point = canvas_of(runtime, a);
        const auto first = press(runtime, run, point);
        step(runtime, run, kTapRestMs);
        lift(runtime, run, first);
        step(runtime, run, kTapRestMs);
        const auto second = press(runtime, run, point);
        step(runtime, run, kTapRestMs);
        lift(runtime, run, second);
        step(runtime, run, kAfterTapMs);
        require(
            selected(runtime, a) && selected(runtime, b) && selected(runtime, c) &&
                !selected(runtime, run.commander),
            Lane::dispatch,
            "a double tap on a Peewee did not select every Peewee (a click of 2)"
        );
    }

    /// 19. A long press on a touch control, or on a 3.1c button other than a build button,
    /// shows its help and does nothing else.
    static void case_help_tips(Runtime& runtime, TouchRun& run) {
        select_only(runtime, run.peewees[0]);
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto shows_tip = [&](CanvasPoint point, std::string_view what) {
            touch(runtime).hud.tip = {};
            const auto finger = press(runtime, run, point);
            steps(runtime, run, kHoldMs, kHoldStepMs);
            const auto& tip = touch(runtime).hud.tip;
            const bool shown = !tip.text.empty() && tip.until_ms != 0;
            lift(runtime, run, finger);
            step(runtime, run, kAfterTapMs);
            require(
                shown, Lane::dispatch, "a long press on " + std::string(what) + " showed no help"
            );
        };
        shows_tip(control_point(runtime, hud::Control::clear, -1, "CLEAR"), "CLEAR");
        require(
            selected(runtime, run.peewees[0]),
            Lane::dispatch,
            "a long press on CLEAR cleared the selection rather than showing its help"
        );
        if (!run.phone) {
            // A hold on a 3.1c button other than a build button releases its press, so the
            // lift acts on nothing, and shows the gadget's help: its own, else the touch
            // controls' line for an order panel gadget (the game data's GUI files give the
            // in-game panels no help text).
            const auto move_button = gadget_holding(runtime, "MOVE");
            require(move_button.has_value(), Lane::check, "the Peewee's orders page has no MOVE");
            shows_tip(gadget_centre(runtime, *move_button), "the 3.1c MOVE button");
            require(
                runtime.match_command_ == MatchCommand::none,
                Lane::dispatch,
                "a long press on the 3.1c MOVE button armed MOVE as a click would"
            );
        }
    }

    /// 20. STORE (or + on a phone) stores the selection in the lowest free group, whose chip
    /// then selects it; a hold on the chip stores another selection in it.
    static void case_groups(Runtime& runtime, TouchRun& run) {
        const auto [a, b, c] = run.peewees;
        select_only(runtime, a);
        steps(runtime, run, 2 * kFrameMs, kFrameMs);
        const auto before = touch(runtime).hud.group_counts;
        tap_control(runtime, run, hud::Control::group_store, -1, run.phone ? "+" : "STORE");
        step(runtime, run, kFrameMs);
        const auto& after = touch(runtime).hud.group_counts;
        std::optional<uint8_t> group;
        for (uint8_t number = 1; number < after.size() && !group; ++number)
            if (before[number] == 0 && after[number] == 1)
                group = number;
        require(group.has_value(), Lane::dispatch, "STORE stored the selection in no free group");
        runtime.clear_local_selection();
        runtime.apply_match_hud_for_selection();
        step(runtime, run, kFrameMs);
        tap_control(runtime, run, hud::Control::group_chip, *group, "the stored group's chip");
        require(
            selected(runtime, a) && !selected(runtime, b) && !selected(runtime, c),
            Lane::dispatch,
            "a tap on the stored group's chip did not select its unit"
        );
        select_only(runtime, b);
        step(runtime, run, kFrameMs);
        const auto finger =
            press(runtime, run, control_point(runtime, hud::Control::group_chip, *group, "chip"));
        steps(runtime, run, kHoldMs, kHoldStepMs);
        lift(runtime, run, finger);
        step(runtime, run, kAfterTapMs);
        runtime.clear_local_selection();
        runtime.apply_match_hud_for_selection();
        step(runtime, run, kFrameMs);
        tap_control(runtime, run, hud::Control::group_chip, *group, "the stored group's chip");
        require(
            selected(runtime, b) && !selected(runtime, a),
            Lane::dispatch,
            "a hold on a group's chip did not store the selection in it"
        );
    }

    /// 21. SELECT's menu: All selects every unit of the player's and closes the menu.
    static void case_select_menu(Runtime& runtime, TouchRun& run) {
        tap_control(runtime, run, hud::Control::select_menu, -1, "SELECT");
        require(
            touch(runtime).hud.sheet == hud::Sheet::select_menu,
            Lane::dispatch,
            "SELECT did not open its menu"
        );
        step(runtime, run, kFrameMs);
        tap_control(
            runtime,
            run,
            hud::Control::menu_item,
            static_cast<int>(hud::SelectItem::all),
            "SELECT's All"
        );
        require(
            selected(runtime, run.commander) && selected(runtime, run.peewees[0]) &&
                selected(runtime, run.peewees[2]),
            Lane::dispatch,
            "SELECT's All did not select every unit (Ctrl+A)"
        );
        require(
            touch(runtime).hud.sheet == hud::Sheet::none,
            Lane::dispatch,
            "SELECT's menu stayed open after a pick"
        );
    }

    /// Arms a building of the commander's first build page with a finger: its tile in the
    /// phone's drawer, or its button on the tablet's build page.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param name the building's unit name
    static void arm_building(Runtime& runtime, TouchRun& run, std::string_view name) {
        select_only(runtime, run.commander);
        steps(runtime, run, kFrameMs, kFrameMs);
        if (run.phone) {
            tap_control(runtime, run, hud::Control::build_drawer, -1, "BUILD");
            step(runtime, run, kFrameMs);
            tap(runtime, run, drawer_cell(runtime, name));
        } else {
            const auto button = gadget_named(runtime, name);
            require(
                button.has_value(),
                Lane::check,
                "the commander's build page has no " + std::string(name)
            );
            tap(runtime, run, gadget_centre(runtime, *button));
        }
        require(
            runtime.pending_build_type_ == type_of(runtime, name),
            Lane::dispatch,
            "a tap on the " + std::string(name) + " tile did not arm the building"
        );
        step(runtime, run, kFrameMs);
        require(
            touch(runtime).hud.placement.active,
            Lane::dispatch,
            "the armed building shows no touch placement (HudState::placement)"
        );
    }

    /// 22. The banner's title in the language shown, in English, German and Simplified
    /// Chinese: a Metal Extractor being placed, named as the bottom bar names it, and PATROL
    /// armed, each in its phrase's translation, or in English where the language has none. The
    /// language chosen as the case starts is chosen again as it ends.
    static void case_banner_languages(Runtime& runtime, TouchRun& run) {
        /// One language's titles.
        struct Titles {
            std::string_view tag;       ///< the language chosen
            std::string_view placement; ///< while a Metal Extractor is placed
            std::string_view armed;     ///< with PATROL armed
        };

        constexpr std::array<Titles, 3> languages{{
            {"en", "Place Metal Extractor", "PATROL armed"},
            {"de", "Metallextraktor platzieren", "PATROUILLIEREN bereit"},
            {"zh-Hans", "放置金属采集器", "巡逻已启用"},
        }};

        /// Chooses the language the case started with again as it ends.
        struct ChoiceKept {
            Runtime& runtime;   ///< the runtime
            std::string choice; ///< the choice the case started with

            ~ChoiceKept() {
                try {
                    runtime.set_language_choice(choice);
                } catch (const std::exception& error) {
                    std::cerr << "touch controls check: the language was not chosen again: "
                              << error.what() << '\n';
                }
            }
        } kept{runtime, runtime.language_state().choice};

        // Every title is read before the case fails, so that its message names each one wrong.
        std::string wrong;
        const auto expect = [&](const Titles& language,
                                std::string_view banner,
                                const std::string& shown,
                                std::string_view wanted) {
            if (shown == wanted)
                return;
            wrong += std::string(wrong.empty() ? "" : "; ") + "in " + std::string(language.tag) +
                     " the " + std::string(banner) + " banner says \"" + shown + "\", not \"" +
                     std::string(wanted) + "\"";
        };
        for (const auto& language : languages) {
            runtime.set_language_choice(language.tag);
            require(
                runtime.shown_language().tag == language.tag,
                Lane::check,
                "the language " + std::string(language.tag) + " could not be chosen"
            );
            reset(runtime, run);
            arm_building(runtime, run, "ARMMEX");
            expect(
                language, "placement", TouchDrawAccess::banner_title(runtime), language.placement
            );
            reset(runtime, run);
            select_only(runtime, run.peewees[0]);
            steps(runtime, run, kFrameMs, kFrameMs);
            require(
                runtime.arm_match_command("PATROL", false), Lane::check, "PATROL could not be armed"
            );
            step(runtime, run, kFrameMs);
            expect(language, "armed", TouchDrawAccess::banner_title(runtime), language.armed);
        }
        require(wrong.empty(), Lane::draw, wrong);
    }

    /// 2. A tap on open ground with the commander selected queues MoveGround there.
    static void case_tap_moves(Runtime& runtime, TouchRun& run) {
        select_only(runtime, run.commander);
        const auto unit = canvas_of(runtime, run.commander);
        const auto point = open_ground_near(
            runtime, run, {unit.x, unit.y - kGroundOffsetPoints * run.px_per_point}
        );
        const auto ground = runtime.match_world_point(point.x, point.y);
        require(ground.has_value(), Lane::check, "the test point has no ground");
        tap(runtime, run, point);
        require(
            moves_to(queue_of(runtime, run.commander), {*ground}) &&
                selected(runtime, run.commander),
            Lane::dispatch,
            "a tap on open ground did not move the selected commander there"
        );
    }

    /// Returns the corners of a box around units, on clear ground.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param ids the units
    /// @return the start and end corners
    static std::pair<CanvasPoint, CanvasPoint>
    box_around(Runtime& runtime, const TouchRun& run, std::initializer_list<uint16_t> ids) {
        float left = 1e9F;
        float top = 1e9F;
        float right = -1e9F;
        float bottom = -1e9F;
        for (const auto id : ids) {
            const auto point = canvas_of(runtime, id);
            left = std::min(left, point.x);
            top = std::min(top, point.y);
            right = std::max(right, point.x);
            bottom = std::max(bottom, point.y);
        }
        const float margin = kBoxMarginPoints * run.px_per_point;
        const CanvasPoint start{left - margin, top - margin};
        const CanvasPoint end{right + margin, bottom + margin};
        require(
            clear_ground(runtime, run, start, false) && clear_ground(runtime, run, end, false),
            Lane::check,
            "a box's corners around the units are not clear battlefield"
        );
        // No other unit the check placed lies in or near the box.
        const float reach = kBoxMarginPoints * 1.5F * run.px_per_point;
        for (const auto other :
             {run.commander, run.lab, run.peewees[0], run.peewees[1], run.peewees[2]}) {
            if (std::find(ids.begin(), ids.end(), other) != ids.end())
                continue;
            const auto point = canvas_of(runtime, other);
            require(
                point.x < start.x - reach || point.x > end.x + reach || point.y < start.y - reach ||
                    point.y > end.y + reach,
                Lane::check,
                "a box around the units would take in another unit"
            );
        }
        return {start, end};
    }

    /// 3. A one-finger drag around two units selects both; ADD and a drag add a box.
    static void case_box(Runtime& runtime, TouchRun& run) {
        const auto [a, b, c] = run.peewees;
        look_at_units(runtime, {a, b, c});
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto [start, end] = box_around(runtime, run, {a, b});
        drag(runtime, run, start, end, false);
        require(
            selected(runtime, a) && selected(runtime, b) && !selected(runtime, c) &&
                !selected(runtime, run.commander),
            Lane::dispatch,
            "a one-finger drag around two Peewees did not select both alone (box)"
        );
        tap_control(runtime, run, hud::Control::add, -1, "ADD");
        require(
            touch(runtime).hud.latches.latched(hud::Latch::add),
            Lane::dispatch,
            "a tap on ADD did not latch it"
        );
        const auto [third_start, third_end] = box_around(runtime, run, {c});
        drag(runtime, run, third_start, third_end, false);
        require(
            selected(runtime, a) && selected(runtime, b) && selected(runtime, c),
            Lane::dispatch,
            "a drag with ADD latched did not add the third Peewee to the selection"
        );
    }

    /// 4. A hold then a drag boxes; with the Touch setting Scroll a plain drag moves the camera
    /// and boxes nothing.
    static void case_hold_box_and_scroll(Runtime& runtime, TouchRun& run) {
        const auto [a, b, c] = run.peewees;
        look_at_units(runtime, {a, b, c});
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto [start, end] = box_around(runtime, run, {a, b});
        drag(runtime, run, start, end, true);
        require(
            selected(runtime, a) && selected(runtime, b) && !selected(runtime, c),
            Lane::dispatch,
            "a hold then a drag did not box the two Peewees"
        );
        runtime.clear_local_selection();
        runtime.apply_match_hud_for_selection();
        set_touch_settings(
            runtime, settings::TouchDrag::scroll, settings::TouchLatches::stay_on, false
        );
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto from = open_ground_near(runtime, run, clear_centre(runtime));
        const CanvasPoint to{from.x - kScrollTravelPoints * run.px_per_point, from.y};
        const auto camera_before = runtime.match_camera_x_;
        drag(runtime, run, from, to, false);
        require(
            runtime.match_camera_x_ > camera_before,
            Lane::dispatch,
            "with the Touch setting Scroll a plain drag did not move the camera"
        );
        require(
            !runtime.has_local_selection() && !runtime.match_drag_,
            Lane::dispatch,
            "with the Touch setting Scroll a plain drag drew a box"
        );
    }

    /// 5. QUEUE latched: three ground taps queue three moves and the queued orders show; a tap
    /// on another own unit switches the selection; One action turns QUEUE off after an order.
    static void case_queue(Runtime& runtime, TouchRun& run) {
        const auto [a, b, c] = run.peewees;
        hold_units_still(runtime);
        select_only(runtime, a);
        look_at_units(runtime, {a, c});
        steps(runtime, run, kFrameMs, kFrameMs);
        tap_control(runtime, run, hud::Control::queue, -1, "QUEUE");
        require(
            touch(runtime).hud.latches.latched(hud::Latch::queue),
            Lane::dispatch,
            "a tap on QUEUE did not latch it"
        );
        const auto unit = canvas_of(runtime, a);
        const float offset = kGroundOffsetPoints * run.px_per_point;
        std::vector<oa::sim::ground_orders::Point> grounds;
        for (const auto& want :
             {CanvasPoint{unit.x, unit.y - offset},
              CanvasPoint{unit.x - offset, unit.y - offset},
              CanvasPoint{unit.x - offset, unit.y}}) {
            const auto point = open_ground_near(runtime, run, want);
            const auto ground = runtime.match_world_point(point.x, point.y);
            require(ground.has_value(), Lane::check, "a test point has no ground");
            grounds.push_back(*ground);
            tap(runtime, run, point);
        }
        require(
            moves_to(queue_of(runtime, a), grounds),
            Lane::dispatch,
            "with QUEUE latched three ground taps did not queue three moves"
        );
        require(
            runtime.virtual_shift(Runtime::ModifierUse::order),
            Lane::dispatch,
            "with QUEUE latched the queued orders' overlays get no Shift (virtual_shift(order))"
        );
        tap(runtime, run, canvas_of(runtime, c));
        require(
            selected(runtime, c) && !selected(runtime, a),
            Lane::dispatch,
            "with QUEUE latched a tap on another own unit added it rather than switching to it"
        );
        // One action.
        runtime.clear_local_selection();
        touch(runtime).hud.latches.clear();
        set_touch_settings(
            runtime, settings::TouchDrag::automatic, settings::TouchLatches::one_action, false
        );
        select_only(runtime, a);
        runtime.match_->stop_orders(a);
        steps(runtime, run, kFrameMs, kFrameMs);
        tap_control(runtime, run, hud::Control::queue, -1, "QUEUE");
        require(
            touch(runtime).hud.latches.latched(hud::Latch::queue),
            Lane::dispatch,
            "in One action mode a tap on QUEUE did not latch it"
        );
        const auto point =
            open_ground_near(runtime, run, {canvas_of(runtime, a).x, unit.y - offset});
        tap(runtime, run, point);
        require(
            queue_of(runtime, a).size() == 1,
            Lane::dispatch,
            "in One action mode a ground tap with QUEUE latched did not queue one move"
        );
        require(
            !touch(runtime).hud.latches.latched(hud::Latch::queue),
            Lane::dispatch,
            "in One action mode QUEUE stayed latched after an order (Latches::used)"
        );
    }

    /// 6. Factory: a tap on a build button adds 1, a hold takes 1 off, x5 adds 5; a resting
    /// finger holds the button pressed; a finger that slides off adds nothing.
    static void case_factory(Runtime& runtime, TouchRun& run) {
        const auto kbot = type_of(runtime, "ARMPW");
        select_only(runtime, run.lab);
        look_at_units(runtime, {run.lab});
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto button = gadget_named(runtime, "ARMPW");
        require(button.has_value(), Lane::check, "the lab's build page has no ARMPW");
        const auto point = gadget_centre(runtime, *button);
        const auto count = [&] { return runtime.match_->queued_build_count(run.lab, kbot); };
        empty_factory(runtime, run.lab, kbot);
        tap(runtime, run, point);
        require(count() == 1, Lane::dispatch, "a tap on ARMPW did not queue one");
        hold_release(runtime, run, point);
        require(
            count() == 0, Lane::dispatch, "a hold on ARMPW did not take one off (the right button)"
        );
        const auto resting = press(runtime, run, point);
        step(runtime, run, kFrameMs);
        require(
            runtime.match_hud_held_ && *runtime.match_hud_held_ == *button,
            Lane::dispatch,
            "a finger resting on ARMPW does not hold it pressed (left down at landing)"
        );
        lift(runtime, run, resting);
        step(runtime, run, kAfterTapMs);
        require(count() == 1, Lane::dispatch, "lifting the resting finger did not queue one");
        const auto sliding = press(runtime, run, point);
        step(runtime, run, kFrameMs);
        const auto away = clear_centre(runtime);
        for (int index = 1; index <= kDragSteps; ++index) {
            move(
                runtime,
                run,
                sliding,
                between(point, away, static_cast<float>(index) / static_cast<float>(kDragSteps))
            );
            step(runtime, run, kFrameMs);
        }
        lift(runtime, run, sliding);
        step(runtime, run, kAfterTapMs);
        require(count() == 1, Lane::dispatch, "a finger that slid off ARMPW before lifting queued");
        tap_control(runtime, run, hud::Control::times_five, -1, "x5 (shown with a build page)");
        require(
            touch(runtime).hud.latches.latched(hud::Latch::times_five),
            Lane::dispatch,
            "a tap on x5 did not latch it"
        );
        tap(runtime, run, point);
        require(count() == 6, Lane::dispatch, "with x5 latched a tap on ARMPW did not queue five");
        touch(runtime).hud.latches.clear();
        empty_factory(runtime, run.lab, kbot);
    }

    /// 7. A two-finger tap takes back an armed ATTACK; a second clears the selection.
    static void case_two_finger_clear(Runtime& runtime, TouchRun& run) {
        select_only(runtime, run.peewees[0]);
        look_at_units(runtime, {run.peewees[0]});
        steps(runtime, run, kFrameMs, kFrameMs);
        require(
            runtime.arm_match_command("ATTACK", false) &&
                runtime.match_command_ == MatchCommand::attack,
            Lane::check,
            "ATTACK could not be armed for the Peewee"
        );
        const auto point = open_ground_near(runtime, run, clear_centre(runtime));
        two_finger_tap(runtime, run, point);
        require(
            runtime.match_command_ == MatchCommand::none && selected(runtime, run.peewees[0]),
            Lane::dispatch,
            "a two-finger tap did not take back the armed ATTACK alone"
        );
        two_finger_tap(runtime, run, point);
        require(
            !runtime.has_local_selection(),
            Lane::dispatch,
            "a second two-finger tap did not clear the selection"
        );
    }

    /// 8. Pinch out zooms at once about the fingers; a two-finger drag pans; a pinch and a pan
    /// together keep the map under the fingers.
    static void case_pinch_and_pan(Runtime& runtime, TouchRun& run) {
        const auto map_width = static_cast<int32_t>(runtime.selected_tnt_->tile_width * 32U);
        const auto map_height = static_cast<int32_t>(runtime.selected_tnt_->tile_height * 32U);
        runtime.match_zoom_ = kDefaultBattlefieldZoom;
        runtime.match_zoom_target_ = kDefaultBattlefieldZoom;
        look_at(runtime, map_width / 2, map_height / 2);
        steps(runtime, run, kFrameMs, kFrameMs);
        const float apart = kTwoFingerSpreadPoints * run.px_per_point;
        const auto centre = open_ground_near(runtime, run, clear_centre(runtime));
        // Pinch out about the centre.
        const auto under = map_point(runtime, centre);
        const auto zoom_before = runtime.match_zoom_;
        auto first = press(runtime, run, {centre.x - apart, centre.y});
        auto second = press(runtime, run, {centre.x + apart, centre.y});
        step(runtime, run, kFrameMs);
        bool together = true;
        for (int index = 1; index <= kDragSteps; ++index) {
            const float spread = apart * (1.0F + static_cast<float>(index) / kDragSteps);
            move_two(
                runtime,
                run,
                index,
                first,
                {centre.x - spread, centre.y},
                second,
                {centre.x + spread, centre.y}
            );
            together = together && runtime.match_zoom_ == runtime.match_zoom_target_;
            step(runtime, run, kFrameMs);
        }
        require(
            runtime.match_zoom_ > zoom_before * 1.2F, Lane::dispatch, "pinching out did not zoom in"
        );
        require(
            together,
            Lane::dispatch,
            "a pinch eased the zoom: the zoom and its target moved apart in a frame"
        );
        const auto after = map_point(runtime, centre);
        lift(runtime, run, first);
        lift(runtime, run, second);
        step(runtime, run, kFrameMs);
        require(
            std::abs(after[0] - under[0]) <= 1.0 && std::abs(after[1] - under[1]) <= 1.0,
            Lane::dispatch,
            "the map pixel under the pinch's centre moved more than one map pixel"
        );
        // A two-finger drag pans: the map follows the fingers.
        look_at(runtime, map_width / 2, map_height / 2);
        steps(runtime, run, kFrameMs, kFrameMs);
        const float travel = 120.0F * run.px_per_point;
        const auto camera_x = runtime.match_camera_x_;
        const auto camera_z = runtime.match_camera_z_;
        first = press(runtime, run, {centre.x - apart, centre.y});
        second = press(runtime, run, {centre.x + apart, centre.y});
        step(runtime, run, kFrameMs);
        for (int index = 1; index <= kDragSteps; ++index) {
            const float part = travel * static_cast<float>(index) / kDragSteps;
            move(runtime, run, first, {centre.x - apart - part, centre.y - part / 2.0F});
            move(runtime, run, second, {centre.x + apart - part, centre.y - part / 2.0F});
            step(runtime, run, kFrameMs);
        }
        const auto moved_x = runtime.match_camera_x_ - camera_x;
        const auto moved_z = runtime.match_camera_z_ - camera_z;
        lift(runtime, run, first);
        lift(runtime, run, second);
        step(runtime, run, kFrameMs);
        const double expected = travel / runtime.match_zoom();
        require(
            moved_x > expected * 0.6 && moved_z > expected * 0.3,
            Lane::dispatch,
            "a two-finger drag did not pan the camera with the fingers"
        );
        // A pinch and a pan together.
        runtime.match_zoom_ = kDefaultBattlefieldZoom;
        runtime.match_zoom_target_ = kDefaultBattlefieldZoom;
        look_at(runtime, map_width / 2, map_height / 2);
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto start_under = map_point(runtime, centre);
        first = press(runtime, run, {centre.x - apart, centre.y});
        second = press(runtime, run, {centre.x + apart, centre.y});
        step(runtime, run, kFrameMs);
        CanvasPoint last = centre;
        for (int index = 1; index <= kDragSteps; ++index) {
            const float part = static_cast<float>(index) / kDragSteps;
            last = {
                centre.x + 60.0F * run.px_per_point * part,
                centre.y + 40.0F * run.px_per_point * part
            };
            const float spread = apart * (1.0F + 0.5F * part);
            move_two(
                runtime,
                run,
                index,
                first,
                {last.x - spread, last.y},
                second,
                {last.x + spread, last.y}
            );
            step(runtime, run, kFrameMs);
        }
        const auto end_under = map_point(runtime, last);
        lift(runtime, run, first);
        lift(runtime, run, second);
        step(runtime, run, kFrameMs);
        require(
            std::abs(end_under[0] - start_under[0]) <= 2.0 &&
                std::abs(end_under[1] - start_under[1]) <= 2.0,
            Lane::dispatch,
            "a pinch with a pan did not keep the map pixel under the first centroid under the last"
        );
    }

    /// 9. A hold released opens the radial; Patrol patrols to its point; the hub queues the pick
    /// and PATROL ends with it; with QUEUE latched the pick keeps PATROL armed, its 3.1c button
    /// lit and no other, until QUEUE goes off; Info opens the unit info panel; a greyed wedge
    /// does nothing.
    static void case_radial(Runtime& runtime, TouchRun& run) {
        const auto unit = run.peewees[0];
        select_only(runtime, unit);
        look_at_units(runtime, {unit});
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto patrol_button = gadget_holding(runtime, "PATROL");
        require(patrol_button.has_value(), Lane::check, "the Peewee's orders page has no PATROL");
        const auto here = canvas_of(runtime, unit);
        const float offset = kGroundOffsetPoints * run.px_per_point;
        const auto open_radial = [&](CanvasPoint want) {
            const auto point = open_ground_near(runtime, run, want);
            hold_release(runtime, run, point);
            const auto& radial = touch(runtime).hud.radial;
            require(
                radial.has_value(),
                Lane::dispatch,
                "a hold released on open ground opened no radial"
            );
            require(
                std::abs(static_cast<float>(radial->anchor.x) - point.x) <= 2.0F &&
                    std::abs(static_cast<float>(radial->anchor.y) - point.y) <= 2.0F,
                Lane::dispatch,
                "the radial opened away from the held point"
            );
            return point;
        };
        const auto wedge = [&](hud::RadialItem item) -> const hud::RadialWedge& {
            const auto& radial = *touch(runtime).hud.radial;
            for (const auto& candidate : radial.wedges)
                if (candidate.item == item)
                    return candidate;
            fail(Lane::touch_ui, "the radial has no wedge for an item it must show");
        };
        const auto patrols = [&] {
            std::vector<oa::sim::ground_orders::Point> points;
            for (const auto& view : queue_of(runtime, unit))
                if (view.kind == orders::patrol_kind || view.kind == orders::repair_patrol_kind)
                    points.push_back(view.destination);
            return points;
        };
        // Patrol.
        const auto point = open_radial({here.x + offset, here.y});
        const auto ground = runtime.match_world_point(point.x, point.y);
        require(ground.has_value(), Lane::check, "the radial's point has no ground");
        std::size_t ringed = 0;
        bool move_ringed = false;
        for (const auto& candidate : touch(runtime).hud.radial->wedges)
            if (candidate.default_item) {
                ++ringed;
                move_ringed = candidate.item == hud::RadialItem::move;
            }
        require(
            ringed == 1 && move_ringed,
            Lane::dispatch,
            "the radial over open ground does not ring Move alone, the order a tap would give"
        );
        const auto& patrol = wedge(hud::RadialItem::patrol);
        require(patrol.available, Lane::dispatch, "the radial greyed Patrol for a Peewee");
        tap(runtime, run, centre_of(patrol.hit));
        const auto given = patrols();
        require(
            given.size() == 1 && given.front()[0] == (*ground)[0] &&
                given.front()[2] == (*ground)[2],
            Lane::dispatch,
            "tapping Patrol in the radial did not patrol the Peewee to the held point"
        );
        require(!touch(runtime).hud.radial, Lane::dispatch, "the radial stayed open after a pick");
        // The hub queues the pick; its QUEUE goes with the pick, and PATROL
        // with it, as letting Shift go ends an order given with Shift held.
        open_radial({here.x + offset, here.y + offset});
        tap(runtime,
            run,
            {static_cast<float>(touch(runtime).hud.radial->centre.x),
             static_cast<float>(touch(runtime).hud.radial->centre.y)});
        require(
            touch(runtime).hud.radial && touch(runtime).hud.radial->queue_hub,
            Lane::dispatch,
            "a tap on the radial's hub did not turn on its QUEUE"
        );
        tap(runtime, run, centre_of(wedge(hud::RadialItem::patrol).hit));
        require(
            patrols().size() == 2,
            Lane::dispatch,
            "Patrol picked with the hub's QUEUE did not queue a second patrol"
        );
        require(
            runtime.match_command_ == MatchCommand::none &&
                !runtime.match_command_lit(*patrol_button),
            Lane::dispatch,
            "PATROL stayed armed after the pick the hub's QUEUE queued"
        );
        // With QUEUE latched the pick is queued and PATROL stays armed until
        // QUEUE goes off.
        tap_control(runtime, run, hud::Control::queue, -1, "QUEUE");
        open_radial({here.x, here.y + offset});
        tap(runtime, run, centre_of(wedge(hud::RadialItem::patrol).hit));
        require(
            patrols().size() == 3,
            Lane::dispatch,
            "Patrol picked with QUEUE latched did not queue a third patrol"
        );
        require(
            runtime.match_command_lit(*patrol_button),
            Lane::dispatch,
            "the 3.1c PATROL button is not lit after the radial armed PATROL"
        );
        for (std::size_t at = 1; at < runtime.match_hud_->layout.gadgets.size(); ++at)
            require(
                at == *patrol_button || !runtime.match_command_lit(at) ||
                    runtime.match_hud_->layout.gadgets[at].common.name.find("ORD") !=
                        std::string::npos ||
                    runtime.match_hud_->layout.gadgets[at].common.name.find("ONOFF") !=
                        std::string::npos ||
                    runtime.match_hud_->layout.gadgets[at].common.name.find("CLOAK") !=
                        std::string::npos,
                Lane::dispatch,
                "another order button is lit with PATROL: " +
                    runtime.match_hud_->layout.gadgets[at].common.name
            );
        tap_control(runtime, run, hud::Control::queue, -1, "QUEUE");
        require(
            runtime.match_command_ == MatchCommand::none &&
                !runtime.match_command_lit(*patrol_button),
            Lane::dispatch,
            "PATROL stayed armed after QUEUE went off"
        );
        // Info.
        open_radial({here.x - offset, here.y});
        const auto& info = wedge(hud::RadialItem::info);
        require(info.available, Lane::dispatch, "the radial greyed Info with a unit selected");
        tap(runtime, run, centre_of(info.hit));
        require(
            runtime.unit_info_panel_.has_value(),
            Lane::dispatch,
            "Info in the radial did not open the unit info panel"
        );
        runtime.close_unit_info();
        // A greyed wedge.
        open_radial({here.x - offset, here.y - offset});
        const hud::RadialWedge* greyed = nullptr;
        for (const auto& candidate : touch(runtime).hud.radial->wedges)
            if (!candidate.available && greyed == nullptr)
                greyed = &candidate;
        require(greyed != nullptr, Lane::dispatch, "the radial greys nothing for a Peewee");
        const auto queue_before = queue_of(runtime, unit).size();
        tap(runtime, run, centre_of(greyed->hit));
        require(
            touch(runtime).hud.radial.has_value() && runtime.match_command_ == MatchCommand::none &&
                queue_of(runtime, unit).size() == queue_before,
            Lane::dispatch,
            "a tap on a greyed wedge did something, or closed the radial"
        );
    }

    /// Checks touch placement of a Solar Collector by the commander, each placement started by
    /// `start`, which arms the building: the ghost starts at the battlefield's centre; a drag
    /// moves it under the finger; a hold near the ghost places it at the ghost, a double tap
    /// where it taps and a hold away from the ghost where the finger is, each ending the
    /// placement and opening no order wheel; a refused site stays armed and shows refused;
    /// CLEAR and a two-finger tap end the placement; with QUEUE each placement leaves it armed
    /// for the next site; CANCEL ends it.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param start arms the building and checks it shows a touch placement
    template <typename Start>
    static void check_placement(Runtime& runtime, TouchRun& run, const Start& start) {
        const auto solar = type_of(runtime, "ARMSOLAR");
        const auto& placement = touch(runtime).hud.placement;
        const auto anchor_at = [&](CanvasPoint point) {
            return std::abs(static_cast<float>(placement.anchor.x) - point.x) <= 1.5F &&
                   std::abs(static_cast<float>(placement.anchor.y) - point.y) <= 1.5F;
        };
        const auto ended = [&] {
            return runtime.pending_build_type_ == 0 &&
                   runtime.match_command_ == MatchCommand::none && !placement.active;
        };
        const auto armed = [&] {
            return runtime.pending_build_type_ == solar &&
                   runtime.match_command_ == MatchCommand::build && placement.active;
        };
        // The site a ghost at a point builds on, read while the building is armed.
        const auto site_of = [&](CanvasPoint point) {
            const auto site = site_at(runtime, point);
            require(site.has_value(), Lane::check, "the placement's point has no site");
            return *site;
        };
        // The one MobileBuild queued, at a site.
        const auto built_once_at = [&](const Runtime::PendingBuildSite& site) {
            std::size_t at_site = 0;
            for (const auto& order : queue_of(runtime, run.commander))
                at_site += order.kind == orders::mobile_build_kind && order.build_type == solar &&
                                   order.destination == site.world
                               ? 1
                               : 0;
            return at_site == 1 && builds_of(runtime, run.commander, solar) == 1;
        };
        const auto placed_and_ended = [&](const Runtime::PendingBuildSite& site,
                                          std::string_view how) {
            require(
                built_once_at(site),
                Lane::dispatch,
                std::string(how) + " did not queue one MobileBuild at the site it places at"
            );
            require(ended(), Lane::dispatch, std::string(how) + " left the placement armed");
            require(
                !touch(runtime).hud.radial.has_value(),
                Lane::dispatch,
                std::string(how) + " opened the order wheel"
            );
            runtime.match_->stop_orders(run.commander);
        };

        start();
        const auto& view = runtime.match_layout_;
        const CanvasPoint middle{
            static_cast<float>(view.battlefield_x()) +
                static_cast<float>(view.battlefield_width()) / 2.0F,
            static_cast<float>(view.battlefield_y()) +
                static_cast<float>(view.battlefield_height()) / 2.0F
        };
        require(
            std::abs(static_cast<float>(placement.anchor.x) - middle.x) <=
                    2.0F * run.px_per_point &&
                std::abs(static_cast<float>(placement.anchor.y) - middle.y) <=
                    2.0F * run.px_per_point,
            Lane::dispatch,
            "the ghost did not start at the battlefield's centre"
        );
        // A drag moves the ghost under the finger; a hold near it places it at the ghost.
        const auto ghost_site = legal_site_point(runtime, run, solar);
        const auto from = open_ground_near(runtime, run, middle);
        drag(runtime, run, from, ghost_site, false);
        step(runtime, run, kFrameMs);
        require(
            anchor_at(ghost_site), Lane::dispatch, "a drag did not move the ghost under the finger"
        );
        require(placement.legal, Lane::dispatch, "the ghost over a legal site shows refused");
        const auto near = near_ghost_point(runtime, run, ghost_site);
        if (near.x == ghost_site.x && near.y == ghost_site.y)
            run.notes.push_back(
                "placement: no clear ground beside the ghost, so the hold near it "
                "landed on the ghost itself"
            );
        const auto at_ghost = site_of(ghost_site);
        hold_release(runtime, run, near);
        placed_and_ended(at_ghost, "a hold near the ghost");

        // A double tap places the building where it taps.
        start();
        const auto tapped_site = legal_site_point(runtime, run, solar, ghost_site);
        const auto at_tap = site_of(tapped_site);
        double_tap(runtime, run, tapped_site);
        placed_and_ended(at_tap, "a double tap");

        // A hold away from the ghost places the building where the finger is.
        start();
        const CanvasPoint ghost{
            static_cast<float>(placement.anchor.x), static_cast<float>(placement.anchor.y)
        };
        const auto held_site = legal_site_point(runtime, run, solar, ghost);
        require(
            std::hypot(held_site.x - ghost.x, held_site.y - ghost.y) >
                kGhostReachPoints * run.px_per_point,
            Lane::check,
            "the site found for a hold away from the ghost lies within the ghost's reach"
        );
        const auto at_finger = site_of(held_site);
        hold_release(runtime, run, held_site);
        placed_and_ended(at_finger, "a hold away from the ghost");

        // A refused site stays armed with the ghost on it, refused, for a double tap and a
        // hold; CLEAR then ends the placement.
        start();
        const auto refused = refused_site_point(runtime, run);
        double_tap(runtime, run, refused);
        step(runtime, run, kFrameMs);
        require(
            armed() && builds_of(runtime, run.commander, solar) == 0,
            Lane::dispatch,
            "a double tap on a refused site gave an order or ended the placement"
        );
        require(
            anchor_at(refused) && !placement.legal,
            Lane::dispatch,
            "the ghost on a refused site does not stand there showing refused"
        );
        hold_release(runtime, run, refused);
        step(runtime, run, kFrameMs);
        require(
            armed() && builds_of(runtime, run.commander, solar) == 0 && !placement.legal &&
                !touch(runtime).hud.radial.has_value(),
            Lane::dispatch,
            "a hold on a refused site gave an order, ended the placement or opened the wheel"
        );
        tap_control(runtime, run, hud::Control::clear, -1, "CLEAR");
        require(ended(), Lane::dispatch, "CLEAR did not end the placement");

        // A two-finger tap ends the placement.
        start();
        two_finger_tap(runtime, run, from);
        require(ended(), Lane::dispatch, "a two-finger tap did not end the placement");

        // With QUEUE each placement leaves the building armed for the next site.
        start();
        tap_control(runtime, run, hud::Control::queue, -1, "QUEUE");
        const auto first_queued = legal_site_point(runtime, run, solar);
        tap(runtime, run, first_queued);
        require(
            anchor_at(first_queued) && armed(),
            Lane::dispatch,
            "a tap during placement did not move the ghost there"
        );
        double_tap(runtime, run, first_queued);
        require(
            builds_of(runtime, run.commander, solar) == 1 && armed(),
            Lane::dispatch,
            "a double tap with QUEUE latched did not queue a MobileBuild and stay armed"
        );
        const auto second_queued = legal_site_point(runtime, run, solar, first_queued);
        hold_release(runtime, run, second_queued);
        require(
            builds_of(runtime, run.commander, solar) == 2 && armed() &&
                !touch(runtime).hud.radial.has_value(),
            Lane::dispatch,
            "a hold with QUEUE latched did not queue a second MobileBuild and stay armed"
        );
        tap_control(
            runtime, run, hud::Control::place_cancel, -1, "the cross (CANCEL)", Lane::touch_ui
        );
        require(ended(), Lane::dispatch, "the cross did not end the placement");
        runtime.match_->stop_orders(run.commander);
    }

    /// 9b. The unit info panel takes a finger as it takes the mouse: a tap on the side column's
    /// ATTACK under it does nothing, a drag from the panel draws no box, even with ATTACK armed,
    /// and a box on open ground that leaves nothing selected drops the selection and closes the
    /// panel; a tap on OK closes it.
    static void case_unit_info(Runtime& runtime, TouchRun& run) {
        const auto unit = run.peewees[0];
        const auto open_info = [&] {
            select_only(runtime, unit);
            look_at_units(runtime, {unit});
            steps(runtime, run, kFrameMs, kFrameMs);
            tap_control(runtime, run, hud::Control::info, -1, "INFO");
            const auto area = runtime.unit_info_area();
            require(
                runtime.unit_info_panel_.has_value() && area.has_value(),
                Lane::dispatch,
                "a tap on INFO did not show the unit info panel"
            );
            return *area;
        };
        const auto area = open_info();
        const auto attack = gadget_holding(runtime, "ATTACK");
        require(attack.has_value(), Lane::check, "the Peewee's orders page has no ATTACK");
        tap(runtime, run, gadget_centre(runtime, *attack));
        require(
            runtime.unit_info_panel_ && runtime.match_command_ == MatchCommand::none,
            Lane::dispatch,
            "a tap on the side column's ATTACK armed it with the unit info panel open"
        );
        require(
            runtime.arm_match_command("ATTACK", false) &&
                runtime.match_command_ == MatchCommand::attack,
            Lane::check,
            "ATTACK could not be armed for the Peewee"
        );
        const CanvasPoint face{
            static_cast<float>(area.x + area.width / 4), static_cast<float>(area.y + 8)
        };
        const auto beside = open_ground_near(
            runtime,
            run,
            {static_cast<float>(runtime.match_layout_.left + area.x) / 2.0F,
             static_cast<float>(area.y + area.height / 2)}
        );
        const auto finger = drag(runtime, run, face, beside, false, false);
        require(
            !runtime.match_drag_,
            Lane::dispatch,
            "a finger dragged from the unit info panel with ATTACK armed drew a box"
        );
        lift(runtime, run, finger);
        step(runtime, run, kAfterTapMs);
        require(
            runtime.unit_info_panel_ && queue_of(runtime, unit).empty(),
            Lane::dispatch,
            "a drag from the unit info panel closed it or gave an order"
        );
        runtime.reset_match_command();
        const CanvasPoint below{beside.x, beside.y + kScrollTravelPoints * run.px_per_point};
        drag(runtime, run, beside, below, false);
        require(
            !runtime.unit_info_panel_ && !runtime.has_local_selection(),
            Lane::dispatch,
            "a box on open ground did not drop the selection and close the unit info panel"
        );
        std::ignore = open_info();
        const auto& panel = *runtime.unit_info_panel_;
        for (const auto& gadget : panel.screen->layout.gadgets) {
            if (gadget.common.name != "DONE")
                continue;
            const auto& root = panel.root;
            tap(runtime,
                run,
                {static_cast<float>(
                     area.x +
                     (gadget.common.x + gadget.common.width / 2 - root.x) * area.width / root.width
                 ),
                 static_cast<float>(
                     area.y + (gadget.common.y + gadget.common.height / 2 - root.y) * area.height /
                                  root.height
                 )});
            break;
        }
        require(!runtime.unit_info_panel_, Lane::dispatch, "a tap on OK did not close the panel");
    }

    /// 10. Placement from the commander's build page: check_placement's gestures.
    static void case_placement(Runtime& runtime, TouchRun& run) {
        const auto solar = type_of(runtime, "ARMSOLAR");
        select_only(runtime, run.commander);
        look_at_units(runtime, {run.commander, run.lab});
        steps(runtime, run, kFrameMs, kFrameMs);
        check_placement(runtime, run, [&] {
            const auto button = gadget_named(runtime, "ARMSOLAR");
            require(button.has_value(), Lane::check, "the commander's build page has no ARMSOLAR");
            tap(runtime, run, gadget_centre(runtime, *button));
            require(
                runtime.pending_build_type_ == solar &&
                    runtime.match_command_ == MatchCommand::build,
                Lane::dispatch,
                "a tap on the ARMSOLAR tile did not arm the building"
            );
            step(runtime, run, kFrameMs);
            require(
                touch(runtime).hud.placement.active,
                Lane::dispatch,
                "the armed building shows no touch placement (HudState::placement)"
            );
        });
    }

    /// 11. A tap on the minimap moves the camera and gives no order; with MOVE armed it
    /// orders.
    static void case_minimap(Runtime& runtime, TouchRun& run) {
        select_only(runtime, run.commander);
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto& radar = runtime.radar_picture_;
        require(radar.width > 8 && radar.height > 8, Lane::phone, "the minimap is not drawn");
        const CanvasPoint first{
            static_cast<float>(radar.x + radar.width / 4),
            static_cast<float>(radar.y + radar.height / 4)
        };
        const CanvasPoint second{
            static_cast<float>(radar.x + radar.width * 3 / 4),
            static_cast<float>(radar.y + radar.height * 3 / 4)
        };
        const auto camera_x = runtime.match_camera_x_;
        const auto camera_z = runtime.match_camera_z_;
        tap(runtime, run, first);
        require(
            runtime.match_camera_x_ != camera_x || runtime.match_camera_z_ != camera_z,
            Lane::dispatch,
            "a tap on the minimap did not move the camera"
        );
        require(
            queue_of(runtime, run.commander).empty() && selected(runtime, run.commander),
            Lane::dispatch,
            "a tap on the minimap with no order armed gave an order"
        );
        require(
            runtime.arm_match_command("MOVE", false) &&
                runtime.match_command_ == MatchCommand::move,
            Lane::check,
            "MOVE could not be armed for the commander"
        );
        const auto target = runtime.radar_world_point(second.x, second.y);
        require(
            target.has_value(), Lane::check, "the minimap has no map point under the test point"
        );
        tap(runtime, run, second);
        const auto queue = queue_of(runtime, run.commander);
        require(
            !queue.empty() && queue.front().kind == oa::sim::ground_orders::move_ground_kind &&
                queue.front().destination[0] == (*target)[0] &&
                queue.front().destination[2] == (*target)[2],
            Lane::dispatch,
            "a tap on the minimap with MOVE armed did not move the commander there"
        );
    }

    /// 12. A finger resting at the window's edge does not edge-scroll; a box dragged into the
    /// edge band scrolls the camera; a resting finger counts and keeps the frame rate up.
    static void case_edges(Runtime& runtime, TouchRun& run) {
        const auto map_width = static_cast<int32_t>(runtime.selected_tnt_->tile_width * 32U);
        const auto map_height = static_cast<int32_t>(runtime.selected_tnt_->tile_height * 32U);
        look_at(runtime, map_width / 2, map_height / 2);
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto& view = runtime.match_layout_;
        // A point on the window's right edge that no control claims.
        std::optional<CanvasPoint> edge;
        for (int y = view.battlefield_y() + 4; y < view.bottom_bar_y() - 4 && !edge; y += 4) {
            const CanvasPoint candidate{static_cast<float>(view.width - 1), static_cast<float>(y)};
            if (clear_ground(runtime, run, candidate, false))
                edge = candidate;
        }
        require(
            edge.has_value(), Lane::check, "found no point on the right edge clear of the controls"
        );
        const auto scrolled = [&] {
            const auto x = runtime.match_camera_x_;
            const auto z = runtime.match_camera_z_;
            runtime.scroll_clock_ = frame_pacing::kNanosecondsPerSecond;
            runtime.frame_time_ns_ = 2 * frame_pacing::kNanosecondsPerSecond;
            runtime.pan_match_camera();
            const bool moved = runtime.match_camera_x_ != x || runtime.match_camera_z_ != z;
            runtime.match_camera_x_ = x;
            runtime.match_camera_z_ = z;
            return moved;
        };
        const auto saved_frame_time = runtime.frame_time_ns_;
        const auto saved_scroll_clock = runtime.scroll_clock_;
        const auto resting = press(runtime, run, *edge);
        step(runtime, run, kFrameMs);
        require(
            runtime.touch_finger_count() == 1,
            Lane::dispatch,
            "a resting finger is not counted (touch_finger_count)"
        );
        const bool edge_scrolled = scrolled();
        runtime.frame_time_ns_ = saved_frame_time;
        runtime.scroll_clock_ = saved_scroll_clock;
        require(
            !edge_scrolled, Lane::dispatch, "a finger resting at the window's edge edge-scrolled"
        );
        // The frame pacing: a resting finger is recent input.
        if (!runtime.options_.unattended) {
            auto& game = runtime.match_->state().game;
            game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags | console::kSimRunPaused);
            runtime.last_input_ns_ = 0;
            runtime.camera_moved_ = false;
            runtime.pace_next_frame(run.running);
            const auto with_finger = runtime.frame_wait_;
            lift(runtime, run, resting);
            step(runtime, run, kFrameMs);
            runtime.last_input_ns_ = 0;
            runtime.camera_moved_ = false;
            runtime.pace_next_frame(run.running);
            const auto without = runtime.frame_wait_;
            game.sim_run_flags =
                static_cast<uint16_t>(game.sim_run_flags & ~console::kSimRunPaused);
            if (without == frame_pacing::FrameWait::idle)
                require(
                    with_finger == frame_pacing::FrameWait::precise,
                    Lane::dispatch,
                    "the frame pacing let a frame idle while a finger rested"
                );
            else
                run.notes.push_back(
                    "case 12: the frame pacing never idles here; the resting "
                    "finger's rate was not compared"
                );
        } else {
            lift(runtime, run, resting);
            step(runtime, run, kFrameMs);
        }
        require(
            runtime.touch_finger_count() == 0, Lane::dispatch, "a lifted finger is still counted"
        );
        // A box dragged into the band along the battlefield's bottom edge scrolls down.
        const float band = hud::edge_scroll_points * run.px_per_point;
        const auto start = open_ground_near(runtime, run, clear_centre(runtime));
        std::optional<CanvasPoint> inside_band;
        for (int x = static_cast<int>(start.x); x < view.width - 8 && !inside_band; x += 8) {
            const CanvasPoint candidate{
                static_cast<float>(x), static_cast<float>(view.bottom_bar_y()) - band / 2.0F
            };
            const layout::Point at{x, static_cast<int>(candidate.y)};
            const auto& state = touch(runtime);
            if (!state.frame_ready || !hud::covers(state.frame, at))
                inside_band = candidate;
        }
        require(inside_band.has_value(), Lane::check, "found no point in the bottom edge band");
        const auto camera_z = runtime.match_camera_z_;
        const auto finger = drag(runtime, run, start, *inside_band, false, false);
        for (int frame = 0; frame < 10; ++frame) {
            SDL_Delay(5);
            step(runtime, run, 50);
        }
        const auto scrolled_z = runtime.match_camera_z_;
        lift(runtime, run, finger);
        step(runtime, run, kFrameMs);
        require(
            scrolled_z > camera_z,
            Lane::dispatch,
            "a box dragged into the bottom edge band did not scroll the camera down"
        );
    }

    /// 13. Fingers from the pen's and the mouse's touch devices, and the mouse SDL makes from a
    /// finger, change nothing.
    static void case_ignored_devices(Runtime& runtime, TouchRun& run) {
        const auto point = canvas_of(runtime, run.commander);
        for (const auto device : {SDL_PEN_TOUCHID, SDL_MOUSE_TOUCHID}) {
            const auto finger = press(runtime, run, point, device);
            step(runtime, run, kTapRestMs);
            require(
                runtime.touch_finger_count() == 0,
                Lane::dispatch,
                "a finger from the pen's or the mouse's touch device was claimed"
            );
            lift(runtime, run, finger);
            step(runtime, run, kAfterTapMs);
            require(
                !runtime.has_local_selection(),
                Lane::dispatch,
                "a finger from the pen's or the mouse's touch device selected a unit"
            );
        }
        mouse_click(runtime, run, point, SDL_KMOD_NONE, SDL_TOUCH_MOUSEID);
        step(runtime, run, kFrameMs);
        require(
            !runtime.has_local_selection(),
            Lane::dispatch,
            "a mouse click SDL made from a finger selected a unit while touch controls are on"
        );
    }

    /// 14. Cmd+. takes back an armed order as Escape does; Cmd+P flips the pause bit.
    static void case_command_keys(Runtime& runtime, TouchRun& run) {
        select_only(runtime, run.peewees[0]);
        require(
            runtime.arm_match_command("ATTACK", false),
            Lane::check,
            "ATTACK could not be armed for the Peewee"
        );
        key(runtime, run, SDLK_PERIOD, SDL_SCANCODE_PERIOD, SDL_KMOD_LGUI);
        require(
            runtime.match_command_ == MatchCommand::none && selected(runtime, run.peewees[0]),
            Lane::dispatch,
            "Cmd+. did not take back the armed order as Escape does"
        );
        auto& game = runtime.match_->state().game;
        key(runtime, run, SDLK_P, SDL_SCANCODE_P, SDL_KMOD_LGUI);
        require(
            (game.sim_run_flags & console::kSimRunPaused) != 0,
            Lane::dispatch,
            "Cmd+P did not set the pause bit as Pause does"
        );
        key(runtime, run, SDLK_P, SDL_SCANCODE_P, SDL_KMOD_LGUI);
        require(
            (game.sim_run_flags & console::kSimRunPaused) == 0,
            Lane::dispatch,
            "a second Cmd+P did not clear the pause bit"
        );
    }

    /// 15. With no latch every use's modifiers are the keyboard's.
    static void case_modifiers(Runtime& runtime, TouchRun&) {
        const std::array<SDL_Keymod, 7> states{
            SDL_KMOD_NONE,
            SDL_KMOD_LSHIFT,
            SDL_KMOD_RSHIFT,
            SDL_KMOD_LCTRL,
            SDL_KMOD_LALT,
            static_cast<SDL_Keymod>(SDL_KMOD_LSHIFT | SDL_KMOD_LCTRL),
            static_cast<SDL_Keymod>(SDL_KMOD_LGUI | SDL_KMOD_CAPS)
        };
        for (const auto mods : states) {
            SDL_SetModState(mods);
            for (const auto use :
                 {Runtime::ModifierUse::keyboard,
                  Runtime::ModifierUse::selection,
                  Runtime::ModifierUse::order,
                  Runtime::ModifierUse::build_button}) {
                const bool same = runtime.input_modifiers(use) == SDL_GetModState();
                if (!same) {
                    SDL_SetModState(SDL_KMOD_NONE);
                    fail(
                        Lane::dispatch, "with no latch input_modifiers differs from SDL_GetModState"
                    );
                }
            }
        }
        SDL_SetModState(SDL_KMOD_NONE);
    }

    /// 16. PAUSE sets and clears the pause bit alone; with When the window loses focus at Keep
    /// playing, going to the background opens the in-game menu over the match; coming back
    /// changes nothing; Resume runs the match again. Pause single player's pause there is
    /// --check-away-pause's.
    static void case_pause_and_lifecycle(Runtime& runtime, TouchRun& run) {
        auto& game = runtime.match_->state().game;
        const auto paused = [&] { return (game.sim_run_flags & console::kSimRunPaused) != 0; };
        tap_control(runtime, run, hud::Control::pause, -1, "PAUSE");
        step(runtime, run, kFrameMs);
        require(paused(), Lane::dispatch, "PAUSE did not set the pause bit");
        require(
            touch(runtime).hud.paused,
            Lane::dispatch,
            "HudState::paused does not follow the pause bit"
        );
        require(!runtime.match_paused_, Lane::dispatch, "PAUSE opened the in-game menu");
        tap_control(runtime, run, hud::Control::pause, -1, "PAUSE");
        step(runtime, run, kFrameMs);
        require(
            !paused() && !touch(runtime).hud.paused,
            Lane::dispatch,
            "PAUSE again did not clear the pause bit and HudState::paused"
        );
        // To the background with Keep playing: the watch runs as the event is pushed.
        const auto focus_lost = [&](settings::FocusLost lost) {
            auto chosen = runtime.engine_settings();
            chosen.focus_lost = lost;
            runtime.apply_engine_settings(chosen);
        };
        focus_lost(settings::FocusLost::keep_playing);
        SDL_Event background{};
        background.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
        background.common.timestamp = SDL_GetTicksNS();
        require(SDL_PushEvent(&background), Lane::check, SDL_GetError());
        step(runtime, run, kFrameMs);
        const auto& panel = runtime.match_hud_panel_;
        require(
            runtime.match_paused_ && (panel == oa::data::defs::gui_path("ARMOPT.GUI") ||
                                      panel == oa::data::defs::gui_path("COROPT.GUI")),
            Lane::platform,
            "going to the background did not open the in-game menu over the match"
        );
        require(!paused(), Lane::platform, "going to the background set the pause bit");
        require(
            touch(runtime).hud.menu_open,
            Lane::dispatch,
            "HudState::menu_open does not follow the menu"
        );
        SDL_Event foreground{};
        foreground.type = SDL_EVENT_DID_ENTER_FOREGROUND;
        foreground.common.timestamp = SDL_GetTicksNS();
        runtime.dispatch_event(foreground, run.running);
        step(runtime, run, kFrameMs);
        require(
            runtime.match_paused_ && !paused() && run.running,
            Lane::platform,
            "coming back to the foreground changed the held match"
        );
        runtime.resume_match_pause();
        step(runtime, run, kFrameMs);
        focus_lost(settings::FocusLost::pause_single_player);
        require(
            !runtime.match_paused_ && !paused() && runtime.match_clock_steps(),
            Lane::platform,
            "Resume left the match held"
        );
        // The match runs: the game's clock steps over a second of frames.
        uint32_t clock_ms = 1000;
        runtime.match_timing_.previous_clock =
            oa::base::game_loop::scaled_clock(clock_ms, runtime.match_clock_scale());
        runtime.match_timing_.remainder = 0.0F;
        const auto ticks_before = runtime.match_timing_.tick;
        for (uint32_t frame = 0; frame < kClockFramesPerSecond; ++frame) {
            clock_ms += kClockFrameMs;
            if (runtime.match_clock_steps())
                runtime.advance_match_clock(clock_ms);
        }
        require(
            runtime.match_timing_.tick > ticks_before,
            Lane::platform,
            "the match did not run again after Resume"
        );
        require(
            !touch(runtime).hud.menu_open, Lane::dispatch, "HudState::menu_open stayed after Resume"
        );
    }

    /// 17. The moves a tap and a QUEUE sequence give equal those the same mouse clicks give.
    static void case_same_as_mouse(Runtime& runtime, TouchRun& run) {
        const auto unit = run.peewees[0];
        hold_units_still(runtime);
        select_only(runtime, unit);
        look_at_units(runtime, {unit});
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto here = canvas_of(runtime, unit);
        const float offset = kGroundOffsetPoints * run.px_per_point;
        std::vector<CanvasPoint> points;
        for (const auto& want :
             {CanvasPoint{here.x, here.y - offset},
              CanvasPoint{here.x - offset, here.y - offset},
              CanvasPoint{here.x - offset, here.y + offset}})
            points.push_back(open_ground_near(runtime, run, want));
        tap(runtime, run, points[0]);
        tap_control(runtime, run, hud::Control::queue, -1, "QUEUE");
        tap(runtime, run, points[1]);
        tap(runtime, run, points[2]);
        tap_control(runtime, run, hud::Control::queue, -1, "QUEUE");
        const auto by_finger = queue_of(runtime, unit);
        runtime.match_->stop_orders(unit);
        mouse_click(runtime, run, points[0], SDL_KMOD_NONE);
        mouse_click(runtime, run, points[1], SDL_KMOD_LSHIFT);
        mouse_click(runtime, run, points[2], SDL_KMOD_LSHIFT);
        const auto by_mouse = queue_of(runtime, unit);
        bool same = by_finger.size() == by_mouse.size() && by_mouse.size() == 3;
        for (std::size_t at = 0; same && at < by_mouse.size(); ++at)
            same = by_finger[at].kind == by_mouse[at].kind &&
                   by_finger[at].destination == by_mouse[at].destination &&
                   by_finger[at].build_type == by_mouse[at].build_type;
        require(
            same,
            Lane::dispatch,
            "the orders a tap and a QUEUE sequence gave differ from the same mouse clicks'"
        );
    }

    /// Returns the box the chat line takes in the composed frame: the frame with it open and a
    /// line typed against the frame before, nothing else changed between them (the status
    /// line "Message" opening it writes is put back).
    ///
    /// @param runtime the runtime
    /// @return the box of what changed
    static layout::Rect chat_line_box(Runtime& runtime) {
        if (runtime.chat_composing_)
            runtime.close_chat_line();
        const auto saved_tick = runtime.fake_frontend_tick_;
        runtime.fake_frontend_tick_ = runtime.frontend_tick();
        renderer::Surface first;
        renderer::Surface second;
        renderer::Surface opened;
        compose(runtime, first);
        compose(runtime, second);
        const auto status = runtime.status_;
        runtime.open_chat_line();
        runtime.status_ = status;
        runtime.chat_buffer_ = "TOUCH CONTROLS CHECK";
        compose(runtime, opened);
        runtime.close_chat_line();
        runtime.status_ = status;
        runtime.fake_frontend_tick_ = saved_tick;
        return changed_box(second, opened, restless_pixels(first, second));
    }

    /// Holds the units still with the Pause key's bit, so that a queue the case reads is the
    /// one its taps gave and no move finishes on the way; orders are given while paused as
    /// ever. reset() clears the bit.
    ///
    /// @param runtime the runtime
    static void hold_units_still(Runtime& runtime) {
        auto& game = runtime.match_->state().game;
        game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags | console::kSimRunPaused);
    }

    /// Returns the box a newly posted message log line takes in the composed frame.
    ///
    /// @param runtime the runtime
    /// @return the box of what changed
    static layout::Rect message_box(Runtime& runtime) {
        const auto saved_tick = runtime.fake_frontend_tick_;
        runtime.fake_frontend_tick_ = runtime.frontend_tick();
        oa::sim::messages::clear_messages(runtime.match_->state().game);
        renderer::Surface first;
        renderer::Surface second;
        renderer::Surface posted;
        compose(runtime, first);
        compose(runtime, second);
        runtime.post_match_message("Touch controls check", oa::sim::messages::kind_player_chat);
        compose(runtime, posted);
        runtime.fake_frontend_tick_ = saved_tick;
        return changed_box(second, posted, restless_pixels(first, second));
    }

    /// 18. With touch on, the overlays' area lies on the battlefield, clear of every control.
    static void case_overlay_area(Runtime& runtime, TouchRun& run) {
        steps(runtime, run, kFrameMs, kFrameMs);
        require_frame(runtime);
        const auto area = runtime.overlay_area();
        const auto& view = runtime.match_layout_;
        const layout::Rect battlefield{
            view.battlefield_x(),
            view.battlefield_y(),
            view.battlefield_width(),
            view.battlefield_height()
        };
        require(
            has_area(area) && rect_inside(area, battlefield),
            Lane::touch_ui,
            "overlay_area " + rect_text(area) + " does not lie inside the battlefield " +
                rect_text(battlefield)
        );
        const auto& frame = touch(runtime).frame;
        for (std::size_t at = 0; at < frame.control_count; ++at)
            require(
                !rects_overlap(area, frame.controls[at].rect),
                Lane::touch_ui,
                "overlay_area " + rect_text(area) + " overlaps a control at " +
                    rect_text(frame.controls[at].rect)
            );
    }

    /// 18b. The chat line is drawn inside the overlays' area, above the group bar. CHAT shows
    /// only in shared games, so the line opens with open_chat_line, CHAT's action.
    static void case_chat_line(Runtime& runtime, TouchRun& run) {
        steps(runtime, run, kFrameMs, kFrameMs);
        require_frame(runtime);
        const auto area = runtime.overlay_area();
        const auto& frame = touch(runtime).frame;
        const auto chat = chat_line_box(runtime);
        require(
            holds_text(chat),
            Lane::fullbleed,
            "the chat line drew no line of text on the canvas (changed " + rect_text(chat) + ")"
        );
        int group_top = area.y + area.height;
        for (std::size_t at = 0; at < frame.control_count; ++at)
            if (frame.controls[at].control == hud::Control::group_store ||
                frame.controls[at].control == hud::Control::select_menu)
                group_top = std::min(group_top, frame.controls[at].rect.y);
        require(
            rect_inside(chat, area, 1) && chat.y + chat.height <= group_top,
            Lane::fullbleed,
            "the chat line " + rect_text(chat) + " is not drawn inside overlay_area " +
                rect_text(area) + " above the group bar"
        );
    }

    /// 18c. A posted message's log line starts inside the overlays' area.
    static void case_message_log(Runtime& runtime, TouchRun& run) {
        steps(runtime, run, kFrameMs, kFrameMs);
        require_frame(runtime);
        const auto area = runtime.overlay_area();
        const auto message = message_box(runtime);
        require(
            holds_text(message),
            Lane::fullbleed,
            "a posted message drew no line of text on the canvas (changed " + rect_text(message) +
                ")"
        );
        require(
            message.x >= area.x - 1 && message.y >= area.y - 1 && message.x < area.x + area.width &&
                message.y < area.y + area.height,
            Lane::fullbleed,
            "the message log's line " + rect_text(message) +
                " does not start inside overlay_area " + rect_text(area)
        );
    }

    /// Returns a left button event at a canvas point, as the pointer gives it.
    ///
    /// @param type SDL_EVENT_MOUSE_BUTTON_DOWN or _UP
    /// @param point canvas point
    /// @return the event
    static SDL_Event left_button(SDL_EventType type, CanvasPoint point) {
        SDL_Event event{};
        event.type = type;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.clicks = 1;
        event.button.x = point.x;
        event.button.y = point.y;
        return event;
    }

    /// 18d. Commander placement and the megamap with touch on: the prompt and Done are drawn
    /// inside the overlays' area; a press on a touch control is not the placement's, a tap in the
    /// area moves the commander and a tap on Done ends the placing. With ui.megamap on, the open
    /// megamap's map lies inside the area and a press on a touch control is not its.
    static void case_placement_and_megamap(Runtime& runtime, TouchRun& run) {
        using oa::sim::match_runtime::CommanderPlacement;
        steps(runtime, run, kFrameMs, kFrameMs);
        require_frame(runtime);
        const auto area = runtime.overlay_area();
        const auto& view = runtime.match_layout_;
        const auto& frame = touch(runtime).frame;
        require(frame.control_count > 0, Lane::touch_ui, "the touch frame has no controls");
        const auto control = centre_of(frame.controls[0].rect);
        // The prompt and Done over the battlefield, in the overlays' area.
        const auto saved_tick = runtime.fake_frontend_tick_;
        runtime.fake_frontend_tick_ = runtime.frontend_tick();
        hold_units_still(runtime);
        renderer::Surface first;
        renderer::Surface second;
        renderer::Surface placing;
        compose(runtime, first);
        compose(runtime, second);
        runtime.match_->begin_commander_placement();
        compose(runtime, placing);
        runtime.fake_frontend_tick_ = saved_tick;
        require(
            runtime.match_->commander_placement() == CommanderPlacement::placing,
            Lane::check,
            "the commander placement did not open"
        );
        const auto drawn = changed_box(second, placing, restless_pixels(first, second));
        require(
            has_area(drawn) && rect_inside(drawn, area, 1),
            Lane::fullbleed,
            "the commander placement's prompt and Done " + rect_text(drawn) +
                " are not drawn inside overlay_area " + rect_text(area)
        );
        // A press on a touch control is not the placement's.
        require(
            !runtime.commander_placement_pointer(
                left_button(SDL_EVENT_MOUSE_BUTTON_DOWN, control), control.x, control.y
            ),
            Lane::fullbleed,
            "the commander placement took a press on a touch control"
        );
        // A tap in the area moves the commander there.
        const auto& unit = runtime.match_->state().units[run.commander];
        const auto before_x = static_cast<int32_t>(unit.position.x >> 16);
        const auto before_z = static_cast<int32_t>(unit.position.z >> 16);
        const CanvasPoint inside{
            static_cast<float>(area.x + area.width / 4),
            static_cast<float>(area.y + area.height * 3 / 4)
        };
        const auto wanted = runtime.battlefield_map_point(inside.x, inside.y);
        tap(runtime, run, inside);
        require(
            static_cast<int32_t>(unit.position.x >> 16) == wanted[0] &&
                static_cast<int32_t>(unit.position.z >> 16) == wanted[1],
            Lane::fullbleed,
            "a tap in the overlays' area did not move the commander to the point under it"
        );
        (void)runtime.match_->place_commander(before_x, before_z);
        // Done: 96x20 at (340, 130) of the 640x480 battlefield, mapped as the battlefield maps
        // it, moved to the overlays' area.
        const auto source = layout::placed_mode(view)
                                ? layout::source_battlefield_to_canvas(view, 340 + 48, 130 + 10)
                                : layout::source_to_canvas(view, 340 + 48, 130 + 10);
        const CanvasPoint done{
            static_cast<float>(source.x + area.x - view.battlefield_x()),
            static_cast<float>(source.y + area.y - view.battlefield_y())
        };
        tap(runtime, run, done);
        require(
            runtime.match_->commander_placement() == CommanderPlacement::waiting,
            Lane::fullbleed,
            "a tap on Done did not end the placing"
        );
        auto& game = runtime.match_->state().game;
        game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags & ~console::kSimRunPaused);
        ++runtime.match_timing_.tick;
        runtime.match_->simulation().tick = runtime.match_timing_.tick;
        runtime.match_->tick();
        require(
            runtime.match_->commander_placement() == CommanderPlacement::none,
            Lane::check,
            "the running game did not close the placing"
        );
        if (!runtime.ui_rules().megamap.enabled) {
            run.notes.push_back("18d: ui.megamap is off, so its placement was not checked");
            return;
        }
        // The megamap acts only with the Mouse wheel zoom setting off.
        auto& wheel_zoom = runtime.engine_settings_state().current.wheel_zoom;
        const bool saved_wheel_zoom = std::exchange(wheel_zoom, false);
        runtime.set_megamap_open(true);
        renderer::Surface megamap;
        compose(runtime, megamap);
        const auto& map = runtime.megamap_.layout;
        const layout::Rect drawn_map{
            map.left + view.battlefield_x(), map.top + view.battlefield_y(), map.width, map.height
        };
        const bool map_inside = has_area(drawn_map) && rect_inside(drawn_map, area);
        const bool control_taken = runtime.megamap_pointer(
            left_button(SDL_EVENT_MOUSE_BUTTON_DOWN, control), control.x, control.y
        );
        const auto middle = centre_of(area);
        const bool middle_taken = runtime.megamap_pointer(
            left_button(SDL_EVENT_MOUSE_BUTTON_DOWN, middle), middle.x, middle.y
        );
        std::ignore = runtime.megamap_pointer(
            left_button(SDL_EVENT_MOUSE_BUTTON_UP, middle), middle.x, middle.y
        );
        runtime.set_megamap_open(false);
        wheel_zoom = saved_wheel_zoom;
        require(
            map_inside,
            Lane::fullbleed,
            "the megamap's map " + rect_text(drawn_map) + " is not inside overlay_area " +
                rect_text(area)
        );
        require(!control_taken, Lane::fullbleed, "the megamap took a press on a touch control");
        require(middle_taken, Lane::fullbleed, "the megamap did not take a press in its area");
        run.notes.push_back(
            "18d: the megamap's map " + rect_text(drawn_map) + " lies in overlay_area " +
            rect_text(area)
        );
    }

    // ---- Phone cases ----------------------------------------------------------------------

    /// P0. The phone match starts at zoom 1.25.
    static void case_phone_start_zoom(Runtime&, TouchRun& run) {
        require(
            std::abs(run.start_zoom_target - kPhoneStartZoom) < 0.001F,
            Lane::dispatch,
            "the phone match did not start at zoom 1.25 (touch_match_started)"
        );
    }

    /// P1. The phone layout: full-bleed battlefield, placed regions, the minimap's picture
    /// inside its region.
    static void case_phone_layout(Runtime& runtime, TouchRun& run) {
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto& view = runtime.match_layout_;
        require(
            view.phone, Lane::phone, "the phone window is not laid out full-bleed (placed mode)"
        );
        require(
            view.left == 0 && view.top == 0 && view.bottom == 0,
            Lane::phone,
            "the phone's battlefield is not the whole canvas"
        );
        require(view.placed_count > 0, Lane::phone, "the phone layout has no placed regions");
        const layout::PlacedRegion* minimap = nullptr;
        for (std::size_t at = 0; at < view.placed_count; ++at)
            if (view.placed[at].role == layout::RegionRole::minimap)
                minimap = &view.placed[at];
        require(minimap != nullptr, Lane::phone, "no placed region shows the minimap");
        const layout::Rect radar{
            runtime.radar_picture_.x,
            runtime.radar_picture_.y,
            runtime.radar_picture_.width,
            runtime.radar_picture_.height
        };
        require(
            has_area(radar) && rect_inside(radar, minimap->canvas, 1),
            Lane::phone,
            "the minimap's picture " + rect_text(radar) + " lies outside its region " +
                rect_text(minimap->canvas)
        );
        require_frame(runtime);
        require(
            rects_overlap(touch(runtime).frame.minimap, minimap->canvas),
            Lane::phone,
            "the minimap region is not where the controls' frame puts the minimap"
        );
    }

    /// P2. A one-finger drag scrolls on a phone and draws no box.
    static void case_phone_scroll(Runtime& runtime, TouchRun& run) {
        const auto from = open_ground_near(runtime, run, clear_centre(runtime));
        const CanvasPoint to{from.x - 100.0F * run.px_per_point, from.y};
        const auto camera_x = runtime.match_camera_x_;
        const auto finger = drag(runtime, run, from, to, false, false);
        require(!runtime.match_drag_, Lane::dispatch, "a one-finger drag on a phone drew a box");
        lift(runtime, run, finger);
        step(runtime, run, kFrameMs);
        require(
            runtime.match_camera_x_ > camera_x,
            Lane::dispatch,
            "a one-finger drag on a phone did not scroll the camera"
        );
        require(
            !runtime.has_local_selection(),
            Lane::dispatch,
            "a one-finger drag on a phone selected units"
        );
    }

    /// P3. A hold then a drag boxes on a phone.
    static void case_phone_hold_box(Runtime& runtime, TouchRun& run) {
        const auto [a, b, c] = run.peewees;
        look_at_units(runtime, {a, b});
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto [start, end] = box_around(runtime, run, {a, b});
        drag(runtime, run, start, end, true);
        require(
            selected(runtime, a) && selected(runtime, b) && !selected(runtime, c),
            Lane::dispatch,
            "a hold then a drag on a phone did not box the two Peewees"
        );
    }

    /// P4. A rail slot arms ATTACK and lights; a two-finger tap takes it back.
    static void case_phone_rail(Runtime& runtime, TouchRun& run) {
        select_only(runtime, run.commander);
        steps(runtime, run, 2 * kFrameMs, kFrameMs);
        const auto& state = touch(runtime);
        std::optional<uint8_t> slot;
        for (uint8_t at = 0; at < state.hud.rail_count && at < state.hud.rail.size(); ++at)
            if (!state.hud.rail[at].more && state.hud.rail[at].order == hud::Order::attack)
                slot = at;
        require(slot.has_value(), Lane::dispatch, "the rail lists no ATTACK for the commander");
        tap_control(runtime, run, hud::Control::order_slot, *slot, "ATTACK's rail slot");
        require(
            runtime.match_command_ == MatchCommand::attack,
            Lane::dispatch,
            "the rail's ATTACK did not arm ATTACK"
        );
        step(runtime, run, kFrameMs);
        require(
            touch(runtime).hud.rail[*slot].lit, Lane::dispatch, "the armed ATTACK slot is not lit"
        );
        two_finger_tap(runtime, run, open_ground_near(runtime, run, clear_centre(runtime)));
        require(
            runtime.match_command_ == MatchCommand::none,
            Lane::dispatch,
            "a two-finger tap did not take back the rail's ATTACK"
        );
    }

    /// P5. BUILD opens the drawer and a factory's cell adds 1; the drawer stays open.
    static void case_phone_drawer_factory(Runtime& runtime, TouchRun& run) {
        const auto kbot = type_of(runtime, "ARMPW");
        select_only(runtime, run.lab);
        look_at_units(runtime, {run.lab});
        steps(runtime, run, kFrameMs, kFrameMs);
        empty_factory(runtime, run.lab, kbot);
        tap_control(runtime, run, hud::Control::build_drawer, -1, "BUILD");
        require(
            touch(runtime).hud.sheet == hud::Sheet::drawer,
            Lane::dispatch,
            "BUILD did not open the drawer"
        );
        step(runtime, run, kFrameMs);
        tap(runtime, run, drawer_cell(runtime, "ARMPW"));
        require(
            runtime.match_->queued_build_count(run.lab, kbot) == 1,
            Lane::dispatch,
            "a tap on the drawer's ARMPW cell did not queue one"
        );
        require(
            touch(runtime).hud.sheet == hud::Sheet::drawer,
            Lane::dispatch,
            "a factory's cell closed the drawer"
        );
        tap_control(runtime, run, hud::Control::drawer_close, -1, "the drawer's close");
        require(
            touch(runtime).hud.sheet == hud::Sheet::none,
            Lane::dispatch,
            "the drawer's close did not close it"
        );
        empty_factory(runtime, run.lab, kbot);
    }

    /// P6. A building tile in the drawer starts placement and closes the drawer; the placement
    /// and the armed build survive until the cross; then check_placement's gestures, each
    /// placement started from the drawer.
    static void case_phone_drawer_placement(Runtime& runtime, TouchRun& run) {
        const auto solar = type_of(runtime, "ARMSOLAR");
        select_only(runtime, run.commander);
        look_at_units(runtime, {run.commander, run.lab});
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto open_tile = [&] {
            tap_control(runtime, run, hud::Control::build_drawer, -1, "BUILD");
            require(
                touch(runtime).hud.sheet == hud::Sheet::drawer,
                Lane::dispatch,
                "BUILD did not open the drawer"
            );
            step(runtime, run, kFrameMs);
            tap(runtime, run, drawer_cell(runtime, "ARMSOLAR"));
            require(
                runtime.pending_build_type_ == solar &&
                    runtime.match_command_ == MatchCommand::build,
                Lane::dispatch,
                "the drawer's ARMSOLAR tile did not arm the building"
            );
            require(
                touch(runtime).hud.sheet == hud::Sheet::none,
                Lane::dispatch,
                "choosing a building did not close the drawer"
            );
            require(
                gadget_named(runtime, "ARMSOLAR").has_value(),
                Lane::dispatch,
                "closing the drawer on a building reloaded the panel (the build page is gone)"
            );
            step(runtime, run, kFrameMs);
            require(
                touch(runtime).hud.placement.active,
                Lane::dispatch,
                "the armed building shows no touch placement (HudState::placement)"
            );
        };
        open_tile();
        steps(runtime, run, 200, kFrameMs * 2);
        require(
            runtime.pending_build_type_ == solar && touch(runtime).hud.placement.active &&
                gadget_named(runtime, "ARMSOLAR").has_value(),
            Lane::dispatch,
            "the placement or the armed build did not survive the drawer closing"
        );
        tap_control(runtime, run, hud::Control::place_cancel, -1, "the cross (CANCEL)");
        require(
            runtime.pending_build_type_ == 0 && runtime.match_command_ == MatchCommand::none &&
                !touch(runtime).hud.placement.active,
            Lane::dispatch,
            "the cross did not end the placement"
        );
        check_placement(runtime, run, open_tile);
    }

    /// P7. Zoom + raises the zoom target.
    static void case_phone_zoom(Runtime& runtime, TouchRun& run) {
        const auto before = runtime.match_zoom_target_;
        tap_control(runtime, run, hud::Control::zoom_in, -1, "zoom +");
        require(
            runtime.match_zoom_target_ > before,
            Lane::dispatch,
            "zoom + did not raise the zoom target"
        );
        const auto raised = runtime.match_zoom_target_;
        tap_control(runtime, run, hud::Control::zoom_out, -1, "zoom -");
        require(
            runtime.match_zoom_target_ < raised,
            Lane::dispatch,
            "zoom - did not lower the zoom target"
        );
    }

    /// P8. A tap on a placed region or a touch control never reaches the battlefield.
    static void case_phone_no_reach(Runtime& runtime, TouchRun& run) {
        select_only(runtime, run.commander);
        steps(runtime, run, kFrameMs, kFrameMs);
        const auto& view = runtime.match_layout_;
        const layout::PlacedRegion* readout = nullptr;
        for (std::size_t at = 0; at < view.placed_count; ++at)
            if (view.placed[at].role == layout::RegionRole::readout)
                readout = &view.placed[at];
        require(readout != nullptr, Lane::phone, "no placed region shows the resources");
        // What a tap that reached the battlefield did, or nothing.
        const auto reached = [&]() -> std::string {
            if (!queue_of(runtime, run.commander).empty())
                return " (the commander was given an order)";
            if (!selected(runtime, run.commander))
                return " (the selection changed)";
            if (runtime.match_drag_)
                return " (a selection box started)";
            return {};
        };
        const auto strip = centre_of(readout->canvas);
        tap(runtime, run, strip);
        const auto strip_result = reached();
        require(
            strip_result.empty(),
            Lane::dispatch,
            "a tap on the resource strip at " + rect_text(readout->canvas) +
                " reached the battlefield" + strip_result
        );
        tap_control(runtime, run, hud::Control::select_menu, -1, "SELECT");
        require(
            touch(runtime).hud.sheet == hud::Sheet::select_menu,
            Lane::dispatch,
            "SELECT did not open its menu"
        );
        tap_control(runtime, run, hud::Control::select_menu, -1, "SELECT");
        const auto select_result = reached();
        require(
            select_result.empty(),
            Lane::dispatch,
            "a tap on SELECT reached the battlefield" + select_result
        );
    }

    /// P9. MORE's INFO opens the unit info panel.
    static void case_phone_more_info(Runtime& runtime, TouchRun& run) {
        select_only(runtime, run.peewees[0]);
        steps(runtime, run, kFrameMs, kFrameMs);
        tap_control(runtime, run, hud::Control::more, -1, "MORE");
        require(
            touch(runtime).hud.sheet == hud::Sheet::more,
            Lane::dispatch,
            "MORE did not open its sheet"
        );
        step(runtime, run, kFrameMs);
        tap_control(
            runtime,
            run,
            hud::Control::more_item,
            static_cast<int>(hud::MoreItem::info),
            "MORE's INFO"
        );
        require(
            runtime.unit_info_panel_.has_value(),
            Lane::dispatch,
            "MORE's INFO did not open the unit info panel"
        );
    }

    /// P10. The left-handed setting puts the minimap on the right.
    static void case_phone_left_handed(Runtime& runtime, TouchRun& run) {
        set_touch_settings(
            runtime, settings::TouchDrag::automatic, settings::TouchLatches::stay_on, true
        );
        steps(runtime, run, 2 * kFrameMs, kFrameMs);
        require_frame(runtime);
        const auto half = runtime.match_layout_.width / 2;
        require(
            touch(runtime).frame.minimap.x > half,
            Lane::touch_ui,
            "the left-handed layout keeps the minimap on the left"
        );
        bool region_right = false;
        for (std::size_t at = 0; at < runtime.match_layout_.placed_count; ++at)
            if (runtime.match_layout_.placed[at].role == layout::RegionRole::minimap)
                region_right = runtime.match_layout_.placed[at].canvas.x > half;
        require(region_right, Lane::phone, "the left-handed minimap region stays on the left");
    }

    /// P11. With safe insets of 59, 0, 59 and 21 points every control and region keeps inside
    /// the safe area, the minimap at 67 pt.
    static void case_phone_safe_area(Runtime& runtime, TouchRun& run) {
        touch(runtime).safe_override = layout::Insets{59, 0, 59, 21};
        runtime.apply_output_mode();
        steps(runtime, run, 2 * kFrameMs, kFrameMs);
        const auto& view = runtime.match_layout_;
        const double ppp = view.px_per_point;
        const auto px = [ppp](int points) {
            return static_cast<int>(std::lround(static_cast<double>(points) * ppp));
        };
        require(
            view.safe.left == px(59) && view.safe.top == 0 && view.safe.right == px(59) &&
                view.safe.bottom == px(21),
            Lane::phone,
            "match_layout_.safe is not the safe area's insets in canvas pixels"
        );
        const layout::Rect safe{
            view.safe.left,
            view.safe.top,
            view.width - view.safe.left - view.safe.right,
            view.height - view.safe.top - view.safe.bottom
        };
        require_frame(runtime);
        const auto& frame = touch(runtime).frame;
        for (std::size_t at = 0; at < frame.control_count; ++at)
            require(
                rect_inside(frame.controls[at].rect, safe),
                Lane::touch_ui,
                "a control at " + rect_text(frame.controls[at].rect) +
                    " lies outside the safe area " + rect_text(safe)
            );
        bool minimap_found = false;
        for (std::size_t at = 0; at < view.placed_count; ++at) {
            const auto& region = view.placed[at];
            require(
                rect_inside(region.canvas, safe),
                Lane::phone,
                "a placed region at " + rect_text(region.canvas) + " lies outside the safe area"
            );
            if (region.role == layout::RegionRole::minimap) {
                minimap_found = true;
                require(
                    std::abs(region.canvas.x - px(67)) <= 1,
                    Lane::phone,
                    "the minimap region does not start at 67 pt but at x " +
                        std::to_string(region.canvas.x)
                );
            }
        }
        require(minimap_found, Lane::phone, "no placed region shows the minimap");
        snapshot(runtime, "touch-phone-safe.ppm");
    }

    /// Returns the right edge of the phone's left column: the minimap and the controls under
    /// it.
    ///
    /// @param runtime the runtime
    /// @return canvas column
    static int column_right(Runtime& runtime) {
        const auto& frame = touch(runtime).frame;
        int right = frame.minimap.x + frame.minimap.width;
        for (std::size_t at = 0; at < frame.control_count; ++at) {
            const auto control = frame.controls[at].control;
            if (control == hud::Control::build_drawer || control == hud::Control::queue ||
                control == hud::Control::add || control == hud::Control::clear ||
                control == hud::Control::select_menu || control == hud::Control::zoom_in ||
                control == hud::Control::zoom_out)
                right = std::max(right, frame.controls[at].rect.x + frame.controls[at].rect.width);
        }
        return right;
    }

    /// P12. The phone menu's Chat opens the chat line, drawn on the screen inside the
    /// overlays' area.
    static void case_phone_chat_line(Runtime& runtime, TouchRun& run) {
        steps(runtime, run, kFrameMs, kFrameMs);
        require_frame(runtime);
        tap_control(runtime, run, hud::Control::menu, -1, "MENU");
        require(
            touch(runtime).hud.sheet == hud::Sheet::phone_menu,
            Lane::dispatch,
            "MENU did not open the phone's menu"
        );
        tap_control(
            runtime,
            run,
            hud::Control::menu_item,
            static_cast<int>(hud::PhoneMenuItem::chat),
            "the phone menu's Chat"
        );
        require(
            runtime.chat_composing_, Lane::dispatch, "the phone menu's Chat opened no chat line"
        );
        runtime.close_chat_line();
        steps(runtime, run, 2 * kFrameMs, kFrameMs);
        const auto area = runtime.overlay_area();
        const auto chat = chat_line_box(runtime);
        require(
            holds_text(chat),
            Lane::fullbleed,
            "the chat line drew no line of text on the canvas (changed " + rect_text(chat) +
                "; placed below the screen?)"
        );
        require(
            chat.y >= 0 && chat.y + chat.height <= runtime.match_layout_.height &&
                rect_inside(chat, area, 1),
            Lane::fullbleed,
            "the chat line " + rect_text(chat) + " is not drawn inside overlay_area " +
                rect_text(area)
        );
    }

    /// P13. On a phone the message log's first line lies right of the left column and under
    /// the status pill.
    static void case_phone_message_log(Runtime& runtime, TouchRun& run) {
        steps(runtime, run, kFrameMs, kFrameMs);
        require_frame(runtime);
        const auto& frame = touch(runtime).frame;
        const int right = column_right(runtime);
        const int pill_bottom = frame.status.y + frame.status.height;
        const auto message = message_box(runtime);
        require(
            holds_text(message),
            Lane::fullbleed,
            "a posted message drew no line of text on the canvas (changed " + rect_text(message) +
                ")"
        );
        require(
            message.x >= right && message.y >= pill_bottom,
            Lane::fullbleed,
            "the message log's line " + rect_text(message) +
                " is not right of the left column and under the status pill"
        );
    }

    /// P14. On a phone the kill board lies left of the rail and under PAUSE and MENU.
    static void case_phone_kill_board(Runtime& runtime, TouchRun& run) {
        select_only(runtime, run.commander);
        steps(runtime, run, 2 * kFrameMs, kFrameMs);
        require_frame(runtime);
        const auto& view = runtime.match_layout_;
        const auto& frame = touch(runtime).frame;
        int rail_left = view.width;
        int buttons_bottom = 0;
        for (std::size_t at = 0; at < frame.control_count; ++at) {
            const auto& control = frame.controls[at];
            if (control.control == hud::Control::order_slot ||
                control.control == hud::Control::more)
                rail_left = std::min(rail_left, control.rect.x);
            if (control.control == hud::Control::pause || control.control == hud::Control::menu)
                buttons_bottom = std::max(buttons_bottom, control.rect.y + control.rect.height);
        }
        require(rail_left < view.width, Lane::dispatch, "the commander's rail is empty");
        const auto& game = runtime.match_->state().game;
        const int left = layout::kSourceWidth - oa::ui::hud::kBoardWidth;
        const int bottom = game.player_count * oa::ui::hud::kBoardRowHeight + 0x2e;
        const auto corner = runtime.board_canvas(left, oa::ui::hud::kBoardTop);
        const auto end = runtime.board_canvas(layout::kSourceWidth, bottom + 1);
        const layout::Rect board{corner.x, corner.y, end.x - corner.x, end.y - corner.y};
        require(
            board.x + board.width <= rail_left && board.y >= buttons_bottom,
            Lane::fullbleed,
            "the kill board " + rect_text(board) +
                " is not left of the rail and under PAUSE and MENU"
        );
    }
};

void Runtime::check_touch_controls() {
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        throw std::runtime_error("touch controls check: needs the SDL renderer");
    TouchRun run;
    run.clock_ns = SDL_GetTicksNS() + frame_pacing::kNanosecondsPerSecond;
    auto& dispatch = touch_state().dispatch;
    dispatch.forced = true;
    dispatch.accept_unregistered_touch = true;
    dispatch.check_clock_ns = run.clock_ns;
    start_benchmark_skirmish();
    apply_output_mode();
    TouchCheckAccess::prepare(*this, run);
    TouchCheckAccess::write_snapshots(*this, run);
    using Access = TouchCheckAccess;
    const auto run_case = [&](std::string name, Access::CaseBody body) {
        Access::run_case(*this, run, std::move(name), body);
    };
    if (run.phone) {
        run_case("P0 start zoom", Access::case_phone_start_zoom);
        run_case("P1 phone layout", Access::case_phone_layout);
        run_case("P11 safe area", Access::case_phone_safe_area);
        run_case("P2 drag scrolls", Access::case_phone_scroll);
        run_case("P3 hold box", Access::case_phone_hold_box);
        run_case("P4 rail", Access::case_phone_rail);
        run_case("P5 drawer factory", Access::case_phone_drawer_factory);
        run_case("P6 drawer placement", Access::case_phone_drawer_placement);
        run_case("P7 zoom buttons", Access::case_phone_zoom);
        run_case("P8 controls keep taps", Access::case_phone_no_reach);
        run_case("P9 MORE info", Access::case_phone_more_info);
        run_case("P10 left-handed", Access::case_phone_left_handed);
        run_case("P15 overlay area", Access::case_overlay_area);
        run_case("P12 chat line", Access::case_phone_chat_line);
        run_case("P13 message log", Access::case_phone_message_log);
        run_case("P14 kill board", Access::case_phone_kill_board);
        run_case("P16 placement and megamap", Access::case_placement_and_megamap);
        run_case("13 ignored devices", Access::case_ignored_devices);
        run_case("15 modifiers", Access::case_modifiers);
        run_case("16 pause and lifecycle", Access::case_pause_and_lifecycle);
        run_case("19 help", Access::case_help_tips);
        run_case("20 groups", Access::case_groups);
        run_case("21 SELECT menu", Access::case_select_menu);
        run_case("22 banner languages", Access::case_banner_languages);
    } else {
        run_case("0 tablet layout", Access::case_tablet_layout);
        run_case("1 tap selects", Access::case_tap_selects);
        run_case("1b double tap", Access::case_double_tap);
        run_case("2 tap moves", Access::case_tap_moves);
        run_case("3 box", Access::case_box);
        run_case("4 hold box and scroll", Access::case_hold_box_and_scroll);
        run_case("5 QUEUE", Access::case_queue);
        run_case("6 factory", Access::case_factory);
        run_case("7 two-finger tap", Access::case_two_finger_clear);
        run_case("8 pinch and pan", Access::case_pinch_and_pan);
        run_case("9 radial", Access::case_radial);
        run_case("9b unit info panel", Access::case_unit_info);
        run_case("10 placement", Access::case_placement);
        run_case("11 minimap", Access::case_minimap);
        run_case("12 edges", Access::case_edges);
        run_case("13 ignored devices", Access::case_ignored_devices);
        run_case("14 Cmd keys", Access::case_command_keys);
        run_case("15 modifiers", Access::case_modifiers);
        run_case("16 pause and lifecycle", Access::case_pause_and_lifecycle);
        run_case("17 same as the mouse", Access::case_same_as_mouse);
        run_case("18 overlay area", Access::case_overlay_area);
        run_case("18b chat line", Access::case_chat_line);
        run_case("18c message log", Access::case_message_log);
        run_case("18d placement and megamap", Access::case_placement_and_megamap);
        run_case("19 help", Access::case_help_tips);
        run_case("20 groups", Access::case_groups);
        run_case("21 SELECT menu", Access::case_select_menu);
        run_case("22 banner languages", Access::case_banner_languages);
    }
    for (const auto& note : run.notes)
        std::cout << "touch note: " << note << '\n';
    std::size_t passed = 0;
    std::string failed;
    for (const auto& result : run.results) {
        if (result.passed) {
            ++passed;
            continue;
        }
        if (!failed.empty())
            failed += "; ";
        failed += result.name;
    }
    std::cout << "touch controls check: " << (run.phone ? "phone" : "tablet") << " run at "
              << match_layout_.width << 'x' << match_layout_.height << ", " << passed << " of "
              << run.results.size() << " cases passed"
              << (failed.empty() ? std::string() : ", failed: " + failed) << '\n';
    if (passed != run.results.size())
        throw std::runtime_error(
            "the touch controls check failed " + std::to_string(run.results.size() - passed) +
            " of " + std::to_string(run.results.size()) + " cases"
        );
}

} // namespace oa::app
