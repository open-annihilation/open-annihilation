// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-pad-controls: the gamepad controls driven through the event path
// by SDL virtual pads standing in for a Steam Deck and an Xbox pad, on a
// skirmish (docs/development/testing.md). The stand-ins' buttons are pressed
// through the joystick inputs SDL's own gamepad bindings name for them, their
// trackpads, sticks, triggers and gyro set, and every event SDL then queues
// goes through dispatch_event as a frame of the game does. It writes
// pad-<case>.ppm snapshots of the composed frame into its working directory
// for people to look at.
//
// Each case starts from a known state and fails on its own, naming the piece
// that is missing and the part of the gamepad controls that owns it:
// pad-model (the pure pad model), dispatch (the gamepad dispatcher and its
// actions, pointer, camera and feels), hud (the slim pad HUD, the rings'
// layout and the touch layer's handover), settings (the Controller section),
// check (this check's own setup).
#include "oa/app/runtime.hpp"
#include "engine_settings_state.hpp"
#include "pad_state.hpp"
#include "touch_state.hpp"
#include "oa/app/frame_pacing.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/present/world_renderer.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/match_runtime/construction_orders.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/hud/camera_scroll.hpp"
#include "oa/ui/hud/kill_board.hpp"
#include "oa/ui/pad_controls.hpp"
#include "oa/ui/touch_hud.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace pad = oa::ui::pad_controls;
namespace hud = oa::ui::touch_hud;
namespace layout = oa::ui::display_layout;
namespace orders = oa::sim::match_runtime;
namespace input = oa::sim::gameplay_input;
namespace console = oa::ui::console;
namespace settings = oa::ui::engine_settings;

/// Nanoseconds in a millisecond.
constexpr uint64_t kNanosecondsPerMillisecond = 1'000'000;
/// One frame of the check's clock, in milliseconds.
constexpr uint64_t kFrameMs = 16;
/// How long a tapped button rests down, in milliseconds.
constexpr uint64_t kTapMs = 60;
/// The pause after a click, longer than a double click's window, so the next counts once.
constexpr uint64_t kAfterTapMs = 400;
/// How long a held button rests: past the 350 ms default hold delay.
constexpr uint64_t kHoldMs = 450;
/// The pause between the frames of a hold.
constexpr uint64_t kHoldStepMs = 50;
/// How long a stick or the left trackpad is held for the camera, in milliseconds.
constexpr uint64_t kCameraMs = 400;
/// Frames run after a stand-in is attached, for SDL's added event to arrive.
constexpr int kAttachFrames = 3;
/// Frames run to settle the known state.
constexpr int kSettleFrames = 3;
/// A trigger pulled all the way, and at rest.
constexpr float kTriggerPulled = 1.0F;
constexpr float kTriggerRest = 0.0F;
/// A stick pushed for the camera and the stick cursor.
constexpr float kStickPush = 0.9F;
/// The thumb's distance from a trackpad's middle that aims a ring, in pad widths (0.7 of
/// the half width: past the 15 % dead zone).
constexpr float kRingAimPads = 0.35F;
/// The middle of a trackpad.
constexpr float kPadMiddle = 0.5F;
/// The thumb lifts and lands at the middle again before it would come nearer a trackpad's
/// edge than this, in pad widths.
constexpr float kPadEdgeMargin = 0.08F;
/// The most thumb travel in one frame while pointing, in pad widths.
constexpr float kMostPointStepPads = 0.2F;
/// How near the engine's pointer must come to a target, in canvas pixels.
constexpr float kPointTolerancePx = 1.5F;
/// The most frames a pointing takes before it gives up.
constexpr int kMostPointFrames = 200;
/// Frames the thumb rests after landing before it moves: past the landing dead band.
constexpr int kLandingFrames = 2;
/// The trackpad samples of the slide K1 compares with the model.
constexpr int kSlideSamples = 10;
/// The thumb's travel in each of those samples, in pad widths.
constexpr float kSlideStepPads = 0.01F;
/// How far the pointer may differ from the model's slide: this share of it, or the pixels.
constexpr float kSlideToleranceShare = 0.15F;
constexpr float kSlideTolerancePx = 3.0F;
/// The thumb's travel for the left trackpad's drag, in pad widths, and its samples.
constexpr float kDragTravelPads = 0.4F;
constexpr int kDragSamples = 8;
/// The left trackpad point the minimap hold presses: toward the map's lower right.
constexpr float kMinimapPadPoint = 0.85F;
/// The gyro probe's turn, in radians a second of yaw (to the left), and its frames.
constexpr float kGyroProbeYaw = 1.0F;
constexpr int kGyroProbeFrames = 6;
/// The most frames a SELECT ▾ focus walk takes.
constexpr int kMostFocusSteps = 12;
/// The Steam Deck's trackpad haptic report type, and where it and the side sit in a report.
constexpr uint8_t kHapticReportType = 0xea;
constexpr std::size_t kHapticTypeByte = 1;
constexpr std::size_t kHapticSideByte = 3;
/// The side byte's values for the right pad and for both (SDL's controller_structs.h).
constexpr uint8_t kHapticSideRight = 0x02;
constexpr uint8_t kHapticSideBoth = 0x03;
/// Points from a unit to the ground points the order cases point at.
constexpr float kGroundOffsetPoints = 96.0F;
/// Points around two units that a selection box takes in.
constexpr float kBoxMarginPoints = 36.0F;
/// The unit pick's reach, in points, that a click with no unit under it searches.
constexpr float kPickReachPoints = 12.0F;
/// Pixels a ring opens away from the pointer at most.
constexpr float kRingAnchorSlackPx = 2.0F;
/// Map pixels the units the check places keep from the commander and from each other.
constexpr int32_t kUnitSpacing = 48;
constexpr int32_t kUnitReach = 160;
constexpr int32_t kEdgeMargin = 96;
/// Map pixels in a map tile, and in a footprint cell, and half a cell.
constexpr uint32_t kMapPixelsPerTile = 32;
constexpr int32_t kMapPixelsPerCell = 16;
constexpr int32_t kHalfCellPixels = 8;
/// The fraction bits of the game's 16.16 map positions.
constexpr uint32_t kFixedPointShift = 16;
/// Footprint cells a site search steps by: a building's corner sits on an even cell.
constexpr int32_t kSiteCellStep = 2;
/// The rings, in cells, a building site is searched in around the commander.
constexpr int32_t kSiteFirstRing = 4;
constexpr int32_t kSiteLastRing = 24;
/// The reaches, in cells, the lab's site is searched in beside the commander.
constexpr int32_t kLabFirstReach = 6;
constexpr int32_t kLabLastReach = 40;
/// The open-ground search: its step in points, its rings, the points on the first ring
/// past the middle and how many more each ring has.
constexpr float kGroundSearchStepPoints = 8.0F;
constexpr int kGroundSearchRings = 24;
constexpr int kGroundSearchFirstRingPoints = 8;
constexpr int kGroundSearchRingGrowth = 4;
/// The refused-site search on the lab: its step in points, its rings and the points each ring
/// adds.
constexpr float kRefusedSearchStepPoints = 4.0F;
constexpr int kRefusedSearchRings = 12;
constexpr int kRefusedSearchRingGrowth = 8;
/// The distances, in points, an open point's unit probe looks at, and its directions.
constexpr std::array<float, 3> kPickProbePoints{4.0F, 8.0F, kPickReachPoints + 2.0F};
constexpr int kProbeDirections = 8;
/// A full turn, in radians.
constexpr float kFullTurnRadians = 6.2831853F;
/// The fewest canvas pixels a point the check reads.
constexpr float kLeastPxPerPoint = 0.25F;
/// The most right presses that empty a factory's queue of one type.
constexpr int kMostQueueSteps = 64;
/// What QUEUE gives a build button: five at a press.
constexpr std::size_t kTimesFive = 5;
/// Where the thumb starts the slide K1 compares with the model, in pad widths from the left.
constexpr float kSlideStartPads = 0.3F;
/// The thumb's travel each sample while it pushes the pointer past the screen's edge.
constexpr float kEdgePushStepPads = 0.02F;
/// The buttons the virtual Steam Deck has (its face, View, Menu, guide, sticks, shoulders,
/// D-pad, ··· (MISC1), four grips, the left trackpad's press (TOUCHPAD), the right trackpad's
/// press (MISC2) and the sticks' touches (MISC3, MISC4)), as SDL's Deck driver reports them.
constexpr std::array<SDL_GamepadButton, 24> kDeckButtons{
    SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,           SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK,           SDL_GAMEPAD_BUTTON_GUIDE,
    SDL_GAMEPAD_BUTTON_START,          SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,      SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     SDL_GAMEPAD_BUTTON_MISC1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,  SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,  SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,
    SDL_GAMEPAD_BUTTON_TOUCHPAD,       SDL_GAMEPAD_BUTTON_MISC2,
    SDL_GAMEPAD_BUTTON_MISC3,          SDL_GAMEPAD_BUTTON_MISC4,
};
/// The buttons the virtual Xbox pad has: face, View, Menu, guide, sticks, shoulders, D-pad.
constexpr std::array<SDL_GamepadButton, 15> kXboxButtons{
    SDL_GAMEPAD_BUTTON_SOUTH,
    SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_GUIDE,
    SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
};
/// The USB numbers of the Xbox pad the second stand-in imitates.
constexpr uint16_t kXboxVendor = 0x045e;
constexpr uint16_t kXboxProduct = 0x02ea;
/// The rate the stand-in Deck's sensors report at, in hertz.
constexpr float kSensorRateHz = 250.0F;
/// The trackpads of the stand-in Deck: one finger each, left (0) then right (1).
constexpr std::array<SDL_VirtualJoystickTouchpadDesc, 2> kDeckTouchpads{{{1, {}}, {1, {}}}};
/// The trackpad SDL numbers 0 (the left) and 1 (the right).
constexpr int kLeftTrackpad = 0;
constexpr int kRightTrackpad = 1;
/// The stand-in Deck's gyro and accelerometer.
constexpr std::array<SDL_VirtualJoystickSensorDesc, 2> kDeckSensors{
    {{SDL_SENSOR_GYRO, kSensorRateHz}, {SDL_SENSOR_ACCEL, kSensorRateHz}}
};
/// The largest and smallest joystick axis values.
constexpr int kAxisMost = SDL_JOYSTICK_AXIS_MAX;
constexpr int kAxisLeast = SDL_JOYSTICK_AXIS_MIN;

/// The parts of the gamepad controls a failure points at.
enum class Part : uint8_t {
    pad_model, ///< the pure pad model: maps, pointers, timing, glyphs and feels
    dispatch,  ///< the gamepad dispatcher, its actions, pointer, camera and feels
    hud,       ///< the slim pad HUD, the rings' layout and the touch layer's handover
    settings,  ///< the Controller section and its wiring
    check,     ///< this check's own setup
};

/// Returns the name a failure message gives a part.
///
/// @param part the part
/// @return its name
const char* part_name(Part part) noexcept {
    switch (part) {
    case Part::pad_model:
        return "pad-model";
    case Part::dispatch:
        return "dispatch";
    case Part::hud:
        return "hud";
    case Part::settings:
        return "settings";
    case Part::check:
        return "check";
    }
    return "check";
}

/// A case's failure: what is missing and the part that owns it.
class CaseFailure : public std::runtime_error {
  public:

    /// Makes the failure.
    ///
    /// @param part the part that owns the missing piece
    /// @param what what went wrong
    CaseFailure(Part part, const std::string& what)
        : std::runtime_error(what + " [" + part_name(part) + "]") {}
};

/// Fails the running case.
///
/// @param part the part that owns the missing piece
/// @param what what went wrong
[[noreturn]] void fail(Part part, std::string_view what) {
    throw CaseFailure(part, std::string(what));
}

/// Fails the running case unless a condition holds.
///
/// @param ok the condition
/// @param part the part that owns the missing piece
/// @param what what went wrong when it does not hold
void require(bool ok, Part part, std::string_view what) {
    if (!ok)
        fail(part, what);
}

/// A point on the canvas, in pixels.
struct CanvasPoint {
    float x{}; ///< across
    float y{}; ///< down
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

/// Returns whether one rectangle lies inside another.
///
/// @param inner the rectangle that should lie inside
/// @param outer the rectangle it should lie in
/// @return whether it does
bool rect_inside(const layout::Rect& inner, const layout::Rect& outer) noexcept {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

/// Formats a number with one decimal.
///
/// @param value the number
/// @return the text
std::string number_text(double value) {
    std::ostringstream text;
    text.setf(std::ios::fixed);
    text.precision(1);
    text << value;
    return text.str();
}

/// What a stand-in pad's callbacks saw the game send.
struct PadRecord {
    std::vector<std::array<uint16_t, 2>> rumbles{}; ///< each rumble's low and high motors
    std::vector<std::vector<uint8_t>> effects{};    ///< each effect's bytes
    bool sensors_on{};                              ///< the game turned the sensors on
};

/// Records a rumble the game sent a stand-in.
///
/// @param userdata the stand-in's record
/// @param low the low motor's speed
/// @param high the high motor's speed
/// @return true: the stand-in plays it
bool SDLCALL record_rumble(void* userdata, Uint16 low, Uint16 high) {
    static_cast<PadRecord*>(userdata)->rumbles.push_back({low, high});
    return true;
}

/// Records an effect the game sent a stand-in.
///
/// @param userdata the stand-in's record
/// @param data the effect's bytes
/// @param size how many
/// @return true: the stand-in plays it
bool SDLCALL record_effect(void* userdata, const void* data, int size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    static_cast<PadRecord*>(userdata)->effects.emplace_back(bytes, bytes + std::max(size, 0));
    return true;
}

/// Records the game turning a stand-in's sensors on or off.
///
/// @param userdata the stand-in's record
/// @param enabled whether they are on
/// @return true: the stand-in has them
bool SDLCALL record_sensors(void* userdata, bool enabled) {
    static_cast<PadRecord*>(userdata)->sensors_on = enabled;
    return true;
}

/// Which joystick input a gamepad button or axis is bound to.
struct InputBinding {
    SDL_GamepadBindingType type{SDL_GAMEPAD_BINDTYPE_NONE}; ///< button, axis or hat
    int index{-1};                                          ///< the joystick button, axis or hat
    int hat_mask{};                                         ///< the hat's direction, for a hat
    int input_least{};  ///< the joystick axis's value at the bound range's start
    int input_most{};   ///< the joystick axis's value at its end
    int output_least{}; ///< the gamepad axis's value at the start
    int output_most{};  ///< the gamepad axis's value at the end
};

/// A virtual pad the check attached.
struct StandIn {
    std::string name;         ///< for messages: "the Steam Deck stand-in"
    SDL_JoystickID id{};      ///< 0 before it is attached
    SDL_Joystick* joystick{}; ///< the check's own handle, for the virtual inputs
    std::array<InputBinding, SDL_GAMEPAD_BUTTON_COUNT> buttons{};     ///< by gamepad button
    std::array<InputBinding, SDL_GAMEPAD_AXIS_COUNT> axes{};          ///< by gamepad axis
    std::unique_ptr<PadRecord> record{std::make_unique<PadRecord>()}; ///< what the game sent
    bool trackpads{};           ///< it has the Deck's trackpads and sensors
    uint64_t sensor_clock_ns{}; ///< the last sensor sample's time
};

/// One case's outcome.
struct CaseResult {
    std::string name;    ///< the case's number and name
    bool passed{};       ///< whether every assertion held
    std::string message; ///< the failure, empty when it passed
};

/// The check's own state through the run.
struct PadRun {
    bool running{true};                ///< cleared when an event ends the run
    uint64_t clock_ns{};               ///< the check's clock
    SDL_WindowID window{};             ///< the window the mouse's events name
    float px_per_point{1.0F};          ///< canvas pixels per window point
    float start_zoom_target{1.0F};     ///< the zoom target the match started with
    uint16_t commander{};              ///< the local commander
    std::array<uint16_t, 3> peewees{}; ///< three Peewees beside the commander
    uint16_t lab{};                    ///< a finished Kbot Lab beside the commander
    StandIn deck{};                    ///< the stand-in Steam Deck
    StandIn xbox{};                    ///< the stand-in Xbox pad
    bool touch_controls{};             ///< touch controls are on (always on a phone or tablet)
    bool side_panel{true};             ///< the 3.1c side panel shows (none on the phone layout)
    std::vector<CaseResult> results;   ///< every case's outcome, in order
    std::vector<std::string> notes;    ///< what the snapshots show, and cases left with a note
};

/// Returns the gamepad button a pad button stands for on the stand-ins, or none for the
/// triggers, which are axes.
///
/// @param button the pad button
/// @return the SDL gamepad button
std::optional<SDL_GamepadButton> sdl_button(pad::PadButton button) noexcept {
    switch (button) {
    case pad::PadButton::a:
        return SDL_GAMEPAD_BUTTON_SOUTH;
    case pad::PadButton::b:
        return SDL_GAMEPAD_BUTTON_EAST;
    case pad::PadButton::x:
        return SDL_GAMEPAD_BUTTON_WEST;
    case pad::PadButton::y:
        return SDL_GAMEPAD_BUTTON_NORTH;
    case pad::PadButton::view:
        return SDL_GAMEPAD_BUTTON_BACK;
    case pad::PadButton::menu:
        return SDL_GAMEPAD_BUTTON_START;
    case pad::PadButton::l1:
        return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case pad::PadButton::r1:
        return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case pad::PadButton::l3:
        return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case pad::PadButton::r3:
        return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case pad::PadButton::r4:
        return SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1;
    case pad::PadButton::l4:
        return SDL_GAMEPAD_BUTTON_LEFT_PADDLE1;
    case pad::PadButton::r5:
        return SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2;
    case pad::PadButton::l5:
        return SDL_GAMEPAD_BUTTON_LEFT_PADDLE2;
    case pad::PadButton::dpad_up:
        return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case pad::PadButton::dpad_right:
        return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    case pad::PadButton::dpad_down:
        return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case pad::PadButton::dpad_left:
        return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case pad::PadButton::left_pad:
        return SDL_GAMEPAD_BUTTON_TOUCHPAD;
    case pad::PadButton::right_pad:
        return SDL_GAMEPAD_BUTTON_MISC2;
    case pad::PadButton::left_stick_touch:
        return SDL_GAMEPAD_BUTTON_MISC3;
    case pad::PadButton::right_stick_touch:
        return SDL_GAMEPAD_BUTTON_MISC4;
    case pad::PadButton::none:
    case pad::PadButton::l2:
    case pad::PadButton::r2:
        break;
    }
    return std::nullopt;
}

/// Returns the trigger axis a pad button is, or none.
///
/// @param button the pad button
/// @return L2's or R2's axis
std::optional<SDL_GamepadAxis> trigger_axis(pad::PadButton button) noexcept {
    if (button == pad::PadButton::l2)
        return SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
    if (button == pad::PadButton::r2)
        return SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    return std::nullopt;
}

/// Returns a pad button's name for messages.
///
/// @param button the pad button
/// @return its name in the Steam Deck's labels
std::string button_name(pad::PadButton button) {
    const auto glyph = pad::glyph_spec(button, pad::GlyphStyle::steam_deck);
    if (!glyph.text.empty())
        return std::string(glyph.text);
    switch (button) {
    case pad::PadButton::view:
        return "View";
    case pad::PadButton::menu:
        return "Menu";
    case pad::PadButton::l3:
        return "L3";
    case pad::PadButton::r3:
        return "R3";
    case pad::PadButton::dpad_up:
        return "D-pad up";
    case pad::PadButton::dpad_right:
        return "D-pad right";
    case pad::PadButton::dpad_down:
        return "D-pad down";
    case pad::PadButton::dpad_left:
        return "D-pad left";
    case pad::PadButton::left_pad:
        return "a left trackpad press";
    case pad::PadButton::right_pad:
        return "a right trackpad press";
    default:
        return "a button";
    }
}

/// Returns the trackpad point that aims at a wedge of a ring.
///
/// @param slot the wedge, clockwise from the top
/// @param slots the ring's wedges
/// @return the point, 0..1 each way
CanvasPoint wedge_pad_point(uint8_t slot, uint8_t slots) {
    const auto point =
        pad::wedge_point({kPadMiddle, kPadMiddle}, kRingAimPads, kRingAimPads, slot, slots);
    return {point.x, point.y};
}

} // namespace

/// The pad check's helpers that reach the runtime's private members: static functions that take
/// Runtime&.
struct PadCheckAccess {
    using CaseBody = void (*)(Runtime&, PadRun&);

    // ---- The clock and the frame --------------------------------------------------------

    /// Advances the check's clock and runs one frame as run_frame does: the stand-ins' inputs
    /// pumped, the queued events through dispatch_event, then idle_tick. The pad dispatcher and
    /// the touch dispatcher read the same clock, so the latches agree.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param ms milliseconds the frame advances the clock
    static void step(Runtime& runtime, PadRun& run, uint64_t ms) {
        run.clock_ns += ms * kNanosecondsPerMillisecond;
        runtime.pad_state().check_clock_ns = run.clock_ns;
        runtime.touch_state().dispatch.check_clock_ns = run.clock_ns;
        SDL_PumpEvents();
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
    static void steps(Runtime& runtime, PadRun& run, uint64_t total_ms, uint64_t each_ms) {
        for (uint64_t elapsed = 0; elapsed < total_ms; elapsed += each_ms)
            step(runtime, run, std::min(each_ms, total_ms - elapsed));
    }

    // ---- The stand-in pads ----------------------------------------------------------------

    /// Attaches a virtual pad, opens the check's joystick handle on it and reads the joystick
    /// inputs SDL binds each gamepad button and axis to.
    ///
    /// @param stand_in the stand-in, its name set
    /// @param vendor its USB vendor number
    /// @param product its USB product number
    /// @param buttons its buttons
    /// @param trackpads whether it has the Deck's trackpads and sensors
    static void attach(
        StandIn& stand_in,
        uint16_t vendor,
        uint16_t product,
        std::span<const SDL_GamepadButton> buttons,
        bool trackpads
    ) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id = vendor;
        desc.product_id = product;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis)
            desc.axis_mask |= 1U << static_cast<unsigned>(axis);
        for (const auto button : buttons)
            desc.button_mask |= 1U << static_cast<unsigned>(button);
        desc.nbuttons = static_cast<Uint16>(buttons.size());
        if (trackpads) {
            desc.ntouchpads = static_cast<Uint16>(kDeckTouchpads.size());
            desc.touchpads = kDeckTouchpads.data();
            desc.nsensors = static_cast<Uint16>(kDeckSensors.size());
            desc.sensors = kDeckSensors.data();
        }
        desc.name = stand_in.name.c_str();
        desc.userdata = stand_in.record.get();
        desc.Rumble = record_rumble;
        desc.SendEffect = record_effect;
        desc.SetSensorsEnabled = record_sensors;
        stand_in.id = SDL_AttachVirtualJoystick(&desc);
        require(
            stand_in.id != 0,
            Part::check,
            "SDL refused to attach " + stand_in.name + ": " + SDL_GetError()
        );
        stand_in.trackpads = trackpads;
        stand_in.joystick = SDL_OpenJoystick(stand_in.id);
        require(
            stand_in.joystick != nullptr,
            Part::check,
            "cannot open " + stand_in.name + "'s joystick: " + SDL_GetError()
        );
        SDL_Gamepad* gamepad = SDL_OpenGamepad(stand_in.id);
        require(
            gamepad != nullptr,
            Part::check,
            "SDL does not take " + stand_in.name + " as a gamepad: " + SDL_GetError()
        );
        int count = 0;
        SDL_GamepadBinding** bindings = SDL_GetGamepadBindings(gamepad, &count);
        for (int at = 0; bindings != nullptr && at < count; ++at) {
            const SDL_GamepadBinding& binding = *bindings[at];
            InputBinding found{};
            found.type = binding.input_type;
            if (binding.input_type == SDL_GAMEPAD_BINDTYPE_BUTTON) {
                found.index = binding.input.button;
            } else if (binding.input_type == SDL_GAMEPAD_BINDTYPE_HAT) {
                found.index = binding.input.hat.hat;
                found.hat_mask = binding.input.hat.hat_mask;
            } else if (binding.input_type == SDL_GAMEPAD_BINDTYPE_AXIS) {
                found.index = binding.input.axis.axis;
                found.input_least = binding.input.axis.axis_min;
                found.input_most = binding.input.axis.axis_max;
            }
            if (binding.output_type == SDL_GAMEPAD_BINDTYPE_BUTTON && binding.output.button >= 0 &&
                binding.output.button < SDL_GAMEPAD_BUTTON_COUNT) {
                stand_in.buttons[static_cast<std::size_t>(binding.output.button)] = found;
            } else if (
                binding.output_type == SDL_GAMEPAD_BINDTYPE_AXIS && binding.output.axis.axis >= 0 &&
                binding.output.axis.axis < SDL_GAMEPAD_AXIS_COUNT
            ) {
                found.output_least = binding.output.axis.axis_min;
                found.output_most = binding.output.axis.axis_max;
                stand_in.axes[static_cast<std::size_t>(binding.output.axis.axis)] = found;
            }
        }
        SDL_free(bindings);
        SDL_CloseGamepad(gamepad);
        for (const auto button : buttons)
            require(
                stand_in.buttons[static_cast<std::size_t>(button)].type !=
                    SDL_GAMEPAD_BINDTYPE_NONE,
                Part::check,
                stand_in.name + " has no binding for gamepad button " +
                    std::to_string(static_cast<int>(button))
            );
    }

    /// Attaches the stand-in Steam Deck: Valve's USB numbers, two trackpads, gyro and
    /// accelerometer, the grips and the Deck's other buttons.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    static void attach_deck(Runtime& runtime, PadRun& run) {
        if (run.deck.id != 0)
            return;
        run.deck.name = "the Steam Deck stand-in";
        attach(run.deck, pad::valve_vendor, pad::steam_deck_product, kDeckButtons, true);
        for (int frame = 0; frame < kAttachFrames; ++frame)
            step(runtime, run, kFrameMs);
    }

    /// Attaches the stand-in Xbox pad: no trackpads, grips or sensors.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    static void attach_xbox(Runtime& runtime, PadRun& run) {
        if (run.xbox.id != 0)
            return;
        run.xbox.name = "the Xbox pad stand-in";
        attach(run.xbox, kXboxVendor, kXboxProduct, kXboxButtons, false);
        for (int frame = 0; frame < kAttachFrames; ++frame)
            step(runtime, run, kFrameMs);
    }

    /// Detaches a stand-in and closes the check's handle.
    ///
    /// @param stand_in the stand-in
    static void detach(StandIn& stand_in) {
        if (stand_in.joystick != nullptr)
            SDL_CloseJoystick(stand_in.joystick);
        stand_in.joystick = nullptr;
        if (stand_in.id != 0)
            (void)SDL_DetachVirtualJoystick(stand_in.id);
        stand_in.id = 0;
    }

    /// Requires a stand-in to be attached.
    ///
    /// @param stand_in the stand-in
    static void require_attached(const StandIn& stand_in) {
        require(
            stand_in.id != 0 && stand_in.joystick != nullptr,
            Part::check,
            (stand_in.name.empty() ? std::string("a stand-in pad") : stand_in.name) +
                " is not attached"
        );
    }

    /// Sets a gamepad button of a stand-in through the joystick input it is bound to.
    ///
    /// @param stand_in the stand-in
    /// @param button the gamepad button
    /// @param down whether it is down
    static void set_sdl_button(StandIn& stand_in, SDL_GamepadButton button, bool down) {
        require_attached(stand_in);
        const InputBinding& binding = stand_in.buttons[static_cast<std::size_t>(button)];
        bool ok = false;
        switch (binding.type) {
        case SDL_GAMEPAD_BINDTYPE_BUTTON:
            ok = SDL_SetJoystickVirtualButton(stand_in.joystick, binding.index, down);
            break;
        case SDL_GAMEPAD_BINDTYPE_HAT:
            ok = SDL_SetJoystickVirtualHat(
                stand_in.joystick,
                binding.index,
                static_cast<Uint8>(down ? binding.hat_mask : SDL_HAT_CENTERED)
            );
            break;
        case SDL_GAMEPAD_BINDTYPE_AXIS:
            ok = SDL_SetJoystickVirtualAxis(
                stand_in.joystick,
                binding.index,
                static_cast<Sint16>(down ? binding.input_most : binding.input_least)
            );
            break;
        case SDL_GAMEPAD_BINDTYPE_NONE:
            fail(
                Part::check,
                stand_in.name + " has no binding for gamepad button " +
                    std::to_string(static_cast<int>(button))
            );
        }
        require(ok, Part::check, "cannot set " + stand_in.name + "'s button: " + SDL_GetError());
    }

    /// Sets a gamepad axis of a stand-in through the joystick axis it is bound to, inverting
    /// the binding's ranges.
    ///
    /// @param stand_in the stand-in
    /// @param axis the gamepad axis
    /// @param value -1..1 for a stick (y down), 0..1 for a trigger
    static void set_sdl_axis(StandIn& stand_in, SDL_GamepadAxis axis, float value) {
        require_attached(stand_in);
        const InputBinding& binding = stand_in.axes[static_cast<std::size_t>(axis)];
        require(
            binding.type == SDL_GAMEPAD_BINDTYPE_AXIS,
            Part::check,
            stand_in.name + " has no axis binding for gamepad axis " +
                std::to_string(static_cast<int>(axis))
        );
        const double wanted = value >= 0.0F ? static_cast<double>(value) * kAxisMost
                                            : static_cast<double>(-value) * kAxisLeast;
        double part = 0.0;
        if (binding.output_most != binding.output_least)
            part = (wanted - binding.output_least) /
                   static_cast<double>(binding.output_most - binding.output_least);
        const double raw = binding.input_least +
                           part * static_cast<double>(binding.input_most - binding.input_least);
        const auto clamped = static_cast<Sint16>(
            std::clamp(static_cast<int>(std::lround(raw)), kAxisLeast, kAxisMost)
        );
        require(
            SDL_SetJoystickVirtualAxis(stand_in.joystick, binding.index, clamped),
            Part::check,
            "cannot set " + stand_in.name + "'s axis: " + SDL_GetError()
        );
    }

    /// Sets a pad button of a stand-in: a button, or a trigger pulled or at rest.
    ///
    /// @param stand_in the stand-in
    /// @param button the pad button
    /// @param down whether it is down
    static void set(StandIn& stand_in, pad::PadButton button, bool down) {
        if (const auto axis = trigger_axis(button)) {
            set_sdl_axis(stand_in, *axis, down ? kTriggerPulled : kTriggerRest);
            return;
        }
        const auto sdl = sdl_button(button);
        require(sdl.has_value(), Part::check, "a pad button the stand-ins cannot press");
        set_sdl_button(stand_in, *sdl, down);
    }

    /// Sets a stick of a stand-in.
    ///
    /// @param stand_in the stand-in
    /// @param side which stick
    /// @param x across, -1..1
    /// @param y down, -1..1
    static void set_stick(StandIn& stand_in, pad::Side side, float x, float y) {
        const bool left = side == pad::Side::left;
        set_sdl_axis(stand_in, left ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX, x);
        set_sdl_axis(stand_in, left ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_RIGHTY, y);
    }

    /// Puts a thumb on a stand-in's trackpad, moves it, or lifts it.
    ///
    /// @param stand_in the stand-in
    /// @param side which trackpad
    /// @param down whether the thumb is on it
    /// @param point where, 0..1 each way, y down
    static void set_thumb(StandIn& stand_in, pad::Side side, bool down, CanvasPoint point) {
        require_attached(stand_in);
        require(stand_in.trackpads, Part::check, stand_in.name + " has no trackpads");
        const int touchpad = side == pad::Side::left ? kLeftTrackpad : kRightTrackpad;
        require(
            SDL_SetJoystickVirtualTouchpad(
                stand_in.joystick, touchpad, 0, down, point.x, point.y, down ? 1.0F : 0.0F
            ),
            Part::check,
            "cannot touch " + stand_in.name + "'s trackpad: " + SDL_GetError()
        );
    }

    /// Sends a gyro sample to a stand-in: angular velocity about each axis.
    ///
    /// @param stand_in the stand-in
    /// @param pitch radians a second about the pad's x axis
    /// @param yaw radians a second about the pad's y axis
    /// @param roll radians a second about the pad's z axis
    static void send_gyro(StandIn& stand_in, float pitch, float yaw, float roll) {
        require_attached(stand_in);
        require(stand_in.trackpads, Part::check, stand_in.name + " has no gyro");
        stand_in.sensor_clock_ns += kFrameMs * kNanosecondsPerMillisecond;
        const std::array<float, 3> rates{pitch, yaw, roll};
        require(
            SDL_SendJoystickVirtualSensorData(
                stand_in.joystick,
                SDL_SENSOR_GYRO,
                stand_in.sensor_clock_ns,
                rates.data(),
                static_cast<int>(rates.size())
            ),
            Part::check,
            "cannot send " + stand_in.name + "'s gyro: " + SDL_GetError()
        );
    }

    /// Lets go of everything on a stand-in: buttons up, triggers and sticks at rest, thumbs off.
    ///
    /// @param stand_in the stand-in
    static void let_go(StandIn& stand_in) {
        if (stand_in.id == 0 || stand_in.joystick == nullptr)
            return;
        for (std::size_t button = 0; button < stand_in.buttons.size(); ++button)
            if (stand_in.buttons[button].type != SDL_GAMEPAD_BINDTYPE_NONE)
                set_sdl_button(stand_in, static_cast<SDL_GamepadButton>(button), false);
        set_sdl_axis(stand_in, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, kTriggerRest);
        set_sdl_axis(stand_in, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, kTriggerRest);
        set_stick(stand_in, pad::Side::left, 0.0F, 0.0F);
        set_stick(stand_in, pad::Side::right, 0.0F, 0.0F);
        if (stand_in.trackpads) {
            set_thumb(stand_in, pad::Side::left, false, {kPadMiddle, kPadMiddle});
            set_thumb(stand_in, pad::Side::right, false, {kPadMiddle, kPadMiddle});
        }
    }

    /// Presses a pad button and runs a frame.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param stand_in the stand-in
    /// @param button the pad button
    static void push(Runtime& runtime, PadRun& run, StandIn& stand_in, pad::PadButton button) {
        set(stand_in, button, true);
        step(runtime, run, kFrameMs);
    }

    /// Lets go of a pad button and runs a frame.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param stand_in the stand-in
    /// @param button the pad button
    static void lift(Runtime& runtime, PadRun& run, StandIn& stand_in, pad::PadButton button) {
        set(stand_in, button, false);
        step(runtime, run, kFrameMs);
    }

    /// Taps a pad button: down, rests, up, one frame after.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param stand_in the stand-in
    /// @param button the pad button
    static void tap(Runtime& runtime, PadRun& run, StandIn& stand_in, pad::PadButton button) {
        push(runtime, run, stand_in, button);
        step(runtime, run, kTapMs - kFrameMs);
        lift(runtime, run, stand_in, button);
    }

    /// Clicks with a pad button: a tap, then the double click's window waited out.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param stand_in the stand-in
    /// @param button the pad button
    static void click(Runtime& runtime, PadRun& run, StandIn& stand_in, pad::PadButton button) {
        tap(runtime, run, stand_in, button);
        step(runtime, run, kAfterTapMs);
    }

    /// Holds a pad button past the hold delay; it stays down.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param stand_in the stand-in
    /// @param button the pad button
    static void hold(Runtime& runtime, PadRun& run, StandIn& stand_in, pad::PadButton button) {
        push(runtime, run, stand_in, button);
        steps(runtime, run, kHoldMs, kHoldStepMs);
    }

    /// Presses the right trackpad where the thumb rests: thumb on, the pad's press, off.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param stand_in the stand-in
    static void pad_press(Runtime& runtime, PadRun& run, StandIn& stand_in) {
        set_thumb(stand_in, pad::Side::right, true, {kPadMiddle, kPadMiddle});
        step(runtime, run, kFrameMs);
        tap(runtime, run, stand_in, pad::PadButton::right_pad);
        set_thumb(stand_in, pad::Side::right, false, {kPadMiddle, kPadMiddle});
        step(runtime, run, kAfterTapMs);
    }

    /// Makes a key event of the check's window, down or up.
    ///
    /// @param run the check's state
    /// @param key the key
    /// @param scancode its scancode
    /// @param down whether it goes down
    /// @return the event
    static SDL_Event key_event(PadRun& run, SDL_Keycode key, SDL_Scancode scancode, bool down) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.windowID = run.window;
        event.key.key = key;
        event.key.scancode = scancode;
        event.key.down = down;
        return event;
    }

    /// Sends a key through dispatch_event, down or up, and runs a frame.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param key the key
    /// @param scancode its scancode
    /// @param down whether it goes down
    static void
    send_key(Runtime& runtime, PadRun& run, SDL_Keycode key, SDL_Scancode scancode, bool down) {
        SDL_Event event = key_event(run, key, scancode, down);
        runtime.dispatch_event(event, run.running);
        step(runtime, run, kFrameMs);
    }

    /// Clicks the mouse's left button at a canvas point as a real mouse does, with modifiers
    /// held.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param point canvas point
    /// @param mods modifiers held
    static void mouse_click(Runtime& runtime, PadRun& run, CanvasPoint point, SDL_Keymod mods) {
        float window_x = 0.0F;
        float window_y = 0.0F;
        if (!SDL_RenderCoordinatesToWindow(
                runtime.sdl_.renderer, point.x, point.y, &window_x, &window_y
            ))
            fail(Part::check, SDL_GetError());
        for (const auto type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event{};
            event.type = type;
            if (type == SDL_EVENT_MOUSE_MOTION) {
                event.motion.windowID = run.window;
                event.motion.x = window_x;
                event.motion.y = window_y;
            } else {
                event.button.windowID = run.window;
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
        step(runtime, run, kFrameMs);
    }

    // ---- The pad pointer ----------------------------------------------------------------

    /// Returns the engine's pointer on the match.
    ///
    /// @param runtime the runtime
    /// @return canvas point
    static CanvasPoint pointer(const Runtime& runtime) noexcept {
        return {runtime.match_pointer_x_, runtime.match_pointer_y_};
    }

    /// Moves the pad pointer onto a canvas point with the stand-in Deck's right trackpad, as a
    /// thumb does: it lands, slides toward the point (lifting and landing at the middle again
    /// before an edge), and lifts. The thumb's travel each frame is the distance left over the
    /// most the pointer can travel a pad width, so the pointer never passes the point and the
    /// engine's pointer closes the rest.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param target canvas point
    /// @param what what the point is, for the message
    static void point_at(Runtime& runtime, PadRun& run, CanvasPoint target, std::string_view what) {
        StandIn& deck = run.deck;
        const auto chosen = runtime.pad_settings();
        float greatest = 1.0F;
        if (chosen.acceleration == pad::Acceleration::low)
            greatest = pad::low_acceleration_gain;
        else if (chosen.acceleration == pad::Acceleration::high)
            greatest = pad::high_acceleration_gain;
        const float per_pad = static_cast<float>(runtime.match_layout_.width) *
                              pad::pad_width_canvas_share *
                              static_cast<float>(chosen.pointer_speed) /
                              static_cast<float>(pad::full_speed_percent) * greatest;
        CanvasPoint thumb{kPadMiddle, kPadMiddle};
        const auto land = [&] {
            thumb = {kPadMiddle, kPadMiddle};
            set_thumb(deck, pad::Side::right, true, thumb);
            for (int frame = 0; frame < kLandingFrames; ++frame)
                step(runtime, run, kFrameMs);
        };
        land();
        for (int frame = 0; frame < kMostPointFrames; ++frame) {
            const auto now = pointer(runtime);
            const float dx = target.x - now.x;
            const float dy = target.y - now.y;
            if (std::fabs(dx) <= kPointTolerancePx && std::fabs(dy) <= kPointTolerancePx)
                break;
            const float move_x = std::clamp(dx / per_pad, -kMostPointStepPads, kMostPointStepPads);
            const float move_y = std::clamp(dy / per_pad, -kMostPointStepPads, kMostPointStepPads);
            const CanvasPoint next{thumb.x + move_x, thumb.y + move_y};
            if (next.x < kPadEdgeMargin || next.x > 1.0F - kPadEdgeMargin ||
                next.y < kPadEdgeMargin || next.y > 1.0F - kPadEdgeMargin) {
                set_thumb(deck, pad::Side::right, false, thumb);
                step(runtime, run, kFrameMs);
                land();
                continue;
            }
            thumb = next;
            set_thumb(deck, pad::Side::right, true, thumb);
            step(runtime, run, kFrameMs);
        }
        set_thumb(deck, pad::Side::right, false, thumb);
        step(runtime, run, kFrameMs);
        const auto reached = pointer(runtime);
        require(
            std::fabs(target.x - reached.x) <= kPointTolerancePx &&
                std::fabs(target.y - reached.y) <= kPointTolerancePx,
            Part::dispatch,
            "the right trackpad did not bring the pointer onto " + std::string(what) + " (at " +
                number_text(reached.x) + "," + number_text(reached.y) + ", wanted " +
                number_text(target.x) + "," + number_text(target.y) +
                "; the pad pointer's synthetic mouse motion)"
        );
    }

    /// Returns whether the camera scrolls by the screen's edges now: one second of the edge
    /// scroll run by the engine's own rule, then put back.
    ///
    /// @param runtime the runtime
    /// @return whether the camera moved
    static bool edge_scrolls(Runtime& runtime) {
        const auto x = runtime.match_camera_x_;
        const auto z = runtime.match_camera_z_;
        const auto saved_frame_time = runtime.frame_time_ns_;
        const auto saved_scroll_clock = runtime.scroll_clock_;
        runtime.scroll_clock_ = frame_pacing::kNanosecondsPerSecond;
        runtime.frame_time_ns_ = 2 * frame_pacing::kNanosecondsPerSecond;
        runtime.pan_match_camera();
        const bool moved = runtime.match_camera_x_ != x || runtime.match_camera_z_ != z;
        runtime.match_camera_x_ = x;
        runtime.match_camera_z_ = z;
        runtime.frame_time_ns_ = saved_frame_time;
        runtime.scroll_clock_ = saved_scroll_clock;
        return moved;
    }

    /// Moves a real mouse to a canvas point.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param point canvas point
    static void mouse_move(Runtime& runtime, PadRun& run, CanvasPoint point) {
        float window_x = 0.0F;
        float window_y = 0.0F;
        if (!SDL_RenderCoordinatesToWindow(
                runtime.sdl_.renderer, point.x, point.y, &window_x, &window_y
            ))
            fail(Part::check, SDL_GetError());
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.windowID = run.window;
        event.motion.x = window_x;
        event.motion.y = window_y;
        runtime.dispatch_event(event, run.running);
        step(runtime, run, kFrameMs);
    }

    // ---- The controls ---------------------------------------------------------------------

    /// Returns the touch state.
    ///
    /// @param runtime the runtime
    /// @return the state
    static Runtime::TouchState& touch(Runtime& runtime) { return runtime.touch_state(); }

    /// Returns a laid-out control, if the frame has it.
    ///
    /// @param runtime the runtime
    /// @param control the control
    /// @param index its index, or -1 for any
    /// @return the control, or null
    static const hud::ControlRect* find_control(Runtime& runtime, hud::Control control, int index) {
        const auto& state = touch(runtime);
        if (!state.frame_ready)
            return nullptr;
        const auto& frame = state.frame;
        for (std::size_t at = 0; at < frame.control_count && at < frame.controls.size(); ++at)
            if (frame.controls[at].control == control &&
                (index < 0 || frame.controls[at].index == index))
                return &frame.controls[at];
        return nullptr;
    }

    /// Returns the battlefield rectangle.
    ///
    /// @param runtime the runtime
    /// @return canvas pixels
    static layout::Rect battlefield(Runtime& runtime) {
        const auto& view = runtime.match_layout_;
        return {
            view.battlefield_x(),
            view.battlefield_y(),
            view.battlefield_width(),
            view.battlefield_height()
        };
    }

    /// Returns the open stand-in pad the dispatcher keeps for an id, or null.
    ///
    /// @param runtime the runtime
    /// @param id the joystick
    /// @return the open pad
    static const OpenPad* open_pad(Runtime& runtime, SDL_JoystickID id) {
        for (const auto& open : runtime.pad_state().pads)
            if (open.id == id && id != 0)
                return &open;
        return nullptr;
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

    /// Returns whether two queues hold the same orders.
    ///
    /// @param a one queue
    /// @param b the other
    /// @return whether they agree order by order
    static bool same_orders(
        const std::vector<orders::Match::QueuedCommandView>& a,
        const std::vector<orders::Match::QueuedCommandView>& b
    ) {
        if (a.size() != b.size())
            return false;
        for (std::size_t at = 0; at < a.size(); ++at)
            if (a[at].kind != b[at].kind || a[at].destination != b[at].destination ||
                a[at].build_type != b[at].build_type)
                return false;
        return true;
    }

    /// Formats a queue for a message: each order's kind and destination's map pixels.
    ///
    /// @param queue the queue
    /// @return the text, "[]" when empty
    static std::string queue_text(const std::vector<orders::Match::QueuedCommandView>& queue) {
        std::string text = "[";
        for (std::size_t at = 0; at < queue.size(); ++at) {
            if (at > 0)
                text += ", ";
            text += "kind " + std::to_string(queue[at].kind) + " at " +
                    std::to_string(queue[at].destination[0] >> kFixedPointShift) + "," +
                    std::to_string(queue[at].destination[2] >> kFixedPointShift);
        }
        return text + "]";
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

    /// Returns a unit's patrol destinations.
    ///
    /// @param runtime the runtime
    /// @param id the unit
    /// @return the destinations, in order
    static std::vector<oa::sim::ground_orders::Point> patrols_of(Runtime& runtime, uint16_t id) {
        std::vector<oa::sim::ground_orders::Point> points;
        for (const auto& view : queue_of(runtime, id))
            if (view.kind == orders::patrol_kind || view.kind == orders::repair_patrol_kind)
                points.push_back(view.destination);
        return points;
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

    /// Clears the selection through the engine.
    ///
    /// @param runtime the runtime
    static void select_none(Runtime& runtime) {
        runtime.clear_local_selection();
        runtime.apply_match_hud_for_selection();
    }

    /// Returns the local player's selected units.
    ///
    /// @param runtime the runtime
    /// @return their slots, in slot order
    static std::vector<uint16_t> selection_of(Runtime& runtime) {
        std::vector<uint16_t> ids;
        for (const auto& slot : runtime.match_->world().slots)
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                slot.record.owner_index == runtime.match_local_player_ &&
                (slot.unit->flags & OA_UNIT_FLAG_SELECTED) != 0)
                ids.push_back(slot.unit_index);
        return ids;
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
            Part::check,
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
        const auto map_width =
            static_cast<int32_t>(runtime.selected_tnt_->tile_width * kMapPixelsPerTile);
        const auto map_height =
            static_cast<int32_t>(runtime.selected_tnt_->tile_height * kMapPixelsPerTile);
        x = std::clamp(x, kEdgeMargin, map_width - kEdgeMargin);
        z = std::clamp(z, kEdgeMargin, map_height - kEdgeMargin);
        oa::sim::unit_spawn::Request request;
        request.player = runtime.match_local_player_;
        request.type = type_of(runtime, name);
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>(x) << kFixedPointShift,
            static_cast<uint32_t>(runtime.match_->map_height(
                static_cast<uint32_t>(x) << kFixedPointShift,
                static_cast<uint32_t>(z) << kFixedPointShift
            )) << kFixedPointShift,
            static_cast<uint32_t>(z) << kFixedPointShift
        };
        auto* slot = runtime.match_->create(request);
        require(
            slot != nullptr && slot->unit != nullptr,
            Part::check,
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
            static_cast<int32_t>(unit.position[0] >> kFixedPointShift),
            static_cast<int32_t>(unit.position[2] >> kFixedPointShift)
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

    /// Centres the view on the map's middle.
    ///
    /// @param runtime the runtime
    static void look_at_middle(Runtime& runtime) {
        const auto map_width =
            static_cast<int32_t>(runtime.selected_tnt_->tile_width * kMapPixelsPerTile);
        const auto map_height =
            static_cast<int32_t>(runtime.selected_tnt_->tile_height * kMapPixelsPerTile);
        look_at(runtime, map_width / 2, map_height / 2);
    }

    /// Returns whether a canvas point is open battlefield: on the battlefield, clear of the pad
    /// HUD and the placed regions, open ground under it and, when asked, no unit within the
    /// pick's reach.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param point canvas point
    /// @param unit_free whether no unit may lie within the pick's reach
    /// @return whether it is
    static bool
    clear_ground(Runtime& runtime, const PadRun& run, CanvasPoint point, bool unit_free) {
        if (!runtime.battlefield_contains(point.x, point.y) ||
            runtime.placed_hud_covers(point.x, point.y))
            return false;
        const auto& state = touch(runtime);
        const layout::Point at{static_cast<int>(point.x), static_cast<int>(point.y)};
        if (state.frame_ready && hud::covers(state.frame, at))
            return false;
        runtime.update_pointer(point.x, point.y);
        if (runtime.hovered_match_unit_ != 0 || !runtime.match_world_point(point.x, point.y))
            return false;
        if (!unit_free)
            return true;
        for (const float reach : kPickProbePoints)
            for (int direction = 0; direction < kProbeDirections; ++direction) {
                const float angle = kFullTurnRadians * static_cast<float>(direction) /
                                    static_cast<float>(kProbeDirections);
                const float px = point.x + std::cos(angle) * reach * run.px_per_point;
                const float py = point.y + std::sin(angle) * reach * run.px_per_point;
                runtime.update_pointer(px, py);
                if (runtime.hovered_match_unit_ != 0)
                    return false;
            }
        return true;
    }

    /// Finds open ground near a canvas point, searching rings around it.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param want the point wanted
    /// @return the nearest open point found
    static CanvasPoint open_ground_near(Runtime& runtime, const PadRun& run, CanvasPoint want) {
        const float step_px = kGroundSearchStepPoints * run.px_per_point;
        for (int ring = 0; ring < kGroundSearchRings; ++ring) {
            const int around =
                ring == 0 ? 1 : kGroundSearchFirstRingPoints + ring * kGroundSearchRingGrowth;
            for (int at = 0; at < around; ++at) {
                const float angle =
                    kFullTurnRadians * static_cast<float>(at) / static_cast<float>(around);
                const CanvasPoint point{
                    want.x + std::cos(angle) * step_px * static_cast<float>(ring),
                    want.y + std::sin(angle) * step_px * static_cast<float>(ring)
                };
                if (clear_ground(runtime, run, point, true))
                    return point;
            }
        }
        fail(Part::check, "found no open ground clear of the pad HUD near a test point");
    }

    /// Returns open ground beside a unit, a set distance away along a direction.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param id the unit
    /// @param across points across from it
    /// @param down points down from it
    /// @return canvas point
    static CanvasPoint
    ground_beside(Runtime& runtime, const PadRun& run, uint16_t id, float across, float down) {
        const auto unit = canvas_of(runtime, id);
        return open_ground_near(
            runtime, run, {unit.x + across * run.px_per_point, unit.y + down * run.px_per_point}
        );
    }

    /// Returns the map point under a canvas point, failing the case without ground.
    ///
    /// @param runtime the runtime
    /// @param point canvas point
    /// @return the point
    static oa::sim::ground_orders::Point ground_at(Runtime& runtime, CanvasPoint point) {
        const auto ground = runtime.match_world_point(point.x, point.y);
        require(ground.has_value(), Part::check, "a test point has no ground");
        return *ground;
    }

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

    /// Returns a build button of the loaded page, failing without it.
    ///
    /// @param runtime the runtime
    /// @param name the unit type's name, which the button carries
    /// @return the gadget
    static std::size_t build_button(Runtime& runtime, std::string_view name) {
        const auto button = gadget_named(runtime, name);
        require(
            button.has_value(),
            Part::check,
            "the build page has no " + std::string(name) + " button"
        );
        return *button;
    }

    /// Takes every queued unit of a type off a factory's queue through its button's right
    /// press.
    ///
    /// @param runtime the runtime
    /// @param factory the factory
    /// @param type the unit type
    static void empty_factory(Runtime& runtime, uint16_t factory, uint16_t type) {
        const auto button = gadget_named(runtime, runtime.spawn_type_names_.at(type));
        for (int guard = 0; button && guard < kMostQueueSteps &&
                            runtime.match_->queued_build_count(factory, type) > 0;
             ++guard)
            runtime.activate_match_hud(*button, false);
    }

    /// Returns the corners of a box around units, on open battlefield.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param ids the units
    /// @return the start and end corners
    static std::pair<CanvasPoint, CanvasPoint>
    box_around(Runtime& runtime, const PadRun& run, std::initializer_list<uint16_t> ids) {
        float left = std::numeric_limits<float>::max();
        float top = std::numeric_limits<float>::max();
        float right = std::numeric_limits<float>::lowest();
        float bottom = std::numeric_limits<float>::lowest();
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
            Part::check,
            "a box's corners around the units are not open battlefield"
        );
        return {start, end};
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

    /// Finds a legal site for a building near the commander whose ghost lies on open
    /// battlefield.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param type the building
    /// @return the site's canvas point
    static CanvasPoint legal_site_point(Runtime& runtime, const PadRun& run, uint16_t type) {
        const auto saved_type = runtime.pending_build_type_;
        const auto [unit_x, unit_z] = map_of(runtime, run.commander);
        const int32_t cell_x = unit_x / kMapPixelsPerCell;
        const int32_t cell_z = unit_z / kMapPixelsPerCell;
        const auto& building = runtime.spawn_types_[type];
        const auto viewport =
            runtime.live_viewport(runtime.match_camera_x_, runtime.match_camera_z_);
        std::optional<CanvasPoint> found;
        for (int32_t ring = kSiteFirstRing; ring < kSiteLastRing && !found; ++ring)
            for (int32_t dz = -ring; dz <= ring && !found; dz += kSiteCellStep)
                for (int32_t dx = -ring; dx <= ring && !found; dx += kSiteCellStep) {
                    if (dx != -ring && dx != ring && dz != -ring && dz != ring)
                        continue;
                    const oa::sim::ground_orders::Point centre{
                        ((cell_x + dx) * kMapPixelsPerCell + building.footprint_x * kHalfCellPixels)
                            << kFixedPointShift,
                        0,
                        ((cell_z + dz) * kMapPixelsPerCell + building.footprint_z * kHalfCellPixels)
                            << kFixedPointShift
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
                    runtime.pending_build_type_ = saved_type;
                    if (!clear_ground(runtime, run, point, false))
                        continue;
                    runtime.pending_build_type_ = type;
                    const auto under = runtime.build_site_under(point.x, point.y);
                    if (under && under->legal && under->world == site->world)
                        found = point;
                }
        runtime.pending_build_type_ = saved_type;
        require(found.has_value(), Part::check, "found no legal building site in view");
        return *found;
    }

    /// Finds a point on the battlefield where the building being placed is refused: on the Kbot
    /// Lab beside the commander.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @return canvas point
    static CanvasPoint refused_site_point(Runtime& runtime, const PadRun& run) {
        const auto lab = canvas_of(runtime, run.lab);
        const float step_px = kRefusedSearchStepPoints * run.px_per_point;
        for (int ring = 0; ring < kRefusedSearchRings; ++ring) {
            const int around = ring == 0 ? 1 : kRefusedSearchRingGrowth * ring;
            for (int at = 0; at < around; ++at) {
                const float angle =
                    kFullTurnRadians * static_cast<float>(at) / static_cast<float>(around);
                const CanvasPoint point{
                    lab.x + std::cos(angle) * step_px * static_cast<float>(ring),
                    lab.y + std::sin(angle) * step_px * static_cast<float>(ring)
                };
                if (!runtime.battlefield_contains(point.x, point.y))
                    continue;
                const auto& state = touch(runtime);
                const layout::Point at_point{static_cast<int>(point.x), static_cast<int>(point.y)};
                if (state.frame_ready && hud::covers(state.frame, at_point))
                    continue;
                auto site = runtime.snapped_build_site(point.x, point.y);
                if (!site)
                    site = runtime.build_site_under(point.x, point.y);
                if (site && !site->legal)
                    return point;
            }
        }
        fail(Part::check, "found no refused building site on the Kbot Lab");
    }

    /// Holds the units still with the Pause key's bit, so that a queue the case reads is the
    /// one its clicks gave; orders are given while paused as ever. reset() clears the bit.
    ///
    /// @param runtime the runtime
    static void hold_units_still(Runtime& runtime) {
        auto& game = runtime.match_->state().game;
        game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags | console::kSimRunPaused);
    }

    /// Returns whether the pause bit is set.
    ///
    /// @param runtime the runtime
    /// @return whether it is
    static bool paused(Runtime& runtime) {
        return (runtime.match_->state().game.sim_run_flags & console::kSimRunPaused) != 0;
    }

    // ---- Settings, snapshots and the known state ------------------------------------------

    /// Puts the Controller section's defaults in effect, with one change.
    ///
    /// @param runtime the runtime
    /// @param change what to change from the defaults
    template <typename Change>
    static void set_pad_settings(Runtime& runtime, const Change& change) {
        auto chosen = runtime.engine_settings();
        const settings::EngineSettings defaults{};
        auto wanted = chosen;
        wanted.pad_scheme = defaults.pad_scheme;
        wanted.pad_right_trackpad = defaults.pad_right_trackpad;
        wanted.pad_pointer_speed = defaults.pad_pointer_speed;
        wanted.pad_acceleration = defaults.pad_acceleration;
        wanted.pad_glide = defaults.pad_glide;
        wanted.pad_right_stick = defaults.pad_right_stick;
        wanted.pad_magnetism = defaults.pad_magnetism;
        wanted.pad_gyro = defaults.pad_gyro;
        wanted.pad_gyro_speed = defaults.pad_gyro_speed;
        wanted.pad_haptics = defaults.pad_haptics;
        wanted.pad_prompts = defaults.pad_prompts;
        wanted.pad_left_handed = defaults.pad_left_handed;
        wanted.touch_latches = defaults.touch_latches;
        wanted.touch_hold_ms = defaults.touch_hold_ms;
        change(wanted);
        runtime.apply_engine_settings(wanted);
    }

    /// Writes the composed match frame to a file in the working directory; a frame that cannot
    /// be composed is noted.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param name the file's name
    static void snapshot(Runtime& runtime, PadRun& run, const char* name) {
        try {
            renderer::Surface composed;
            runtime.render_match_surface();
            runtime.compose_match_frame(composed);
            write_ppm(name, composed);
        } catch (const std::exception& error) {
            run.notes.push_back(std::string(name) + ": " + error.what());
        }
    }

    /// Puts the check back in its known state: every stand-in let go, no ring, sheet or latch,
    /// the Controller section's defaults, no menu, panel or chat line, no armed order or
    /// building, no selection, the start zoom, the view on the commander, nothing recorded.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    static void reset(Runtime& runtime, PadRun& run) {
        let_go(run.deck);
        let_go(run.xbox);
        for (int frame = 0; frame < kSettleFrames; ++frame)
            step(runtime, run, kFrameMs);
        runtime.pad_screen_changed();
        auto& state = touch(runtime);
        state.hud.radial.reset();
        state.hud.build_ring.reset();
        state.hud.sheet = hud::Sheet::none;
        state.hud.sheet_focus = -1;
        state.hud.latches.clear();
        state.hud.tip = {};
        ++state.hud.revision;
        state.dispatch.placement_by_pad = false;
        set_pad_settings(runtime, [](settings::EngineSettings&) {});
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
        runtime.stop_match_tracking();
        runtime.apply_match_hud_for_selection();
        runtime.match_zoom_ = run.start_zoom_target;
        runtime.match_zoom_target_ = run.start_zoom_target;
        runtime.zoom_focus_ = {};
        SDL_SetModState(SDL_KMOD_NONE);
        look_at_units(runtime, {run.commander});
        for (int frame = 0; frame < kSettleFrames; ++frame)
            step(runtime, run, kFrameMs);
        for (StandIn* stand_in : {&run.deck, &run.xbox}) {
            stand_in->record->rumbles.clear();
            stand_in->record->effects.clear();
        }
    }

    /// Runs one case from the known state and records its outcome.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param name the case's number and name
    /// @param body the case
    static void run_case(Runtime& runtime, PadRun& run, std::string name, CaseBody body) {
        CaseResult result{std::move(name), false, {}};
        try {
            reset(runtime, run);
            body(runtime, run);
            result.passed = true;
        } catch (const std::exception& error) {
            result.message = error.what();
        }
        std::cout << "pad case " << result.name << ": "
                  << (result.passed ? std::string("passed") : "FAILED: " + result.message) << '\n';
        run.results.push_back(std::move(result));
        try {
            reset(runtime, run);
        } catch (const std::exception&) {
            // The next case reports a known state it could not reach.
        }
    }

    /// Prepares the run after the skirmish started: the window, the units the cases use and
    /// the zoom the match started with.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    static void prepare(Runtime& runtime, PadRun& run) {
        run.window = SDL_GetWindowID(runtime.sdl_.window);
        step(runtime, run, kFrameMs);
        step(runtime, run, kFrameMs);
        run.px_per_point =
            std::max(kLeastPxPerPoint, static_cast<float>(runtime.match_layout_.px_per_point));
        run.touch_controls = runtime.touch_controls_active();
        run.side_panel = !runtime.match_layout_.phone;
        run.start_zoom_target = runtime.match_zoom_target_;
        for (const auto& slot : runtime.match_->world().slots)
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                slot.record.owner_index == runtime.match_local_player_) {
                run.commander = slot.unit_index;
                break;
            }
        if (run.commander == 0)
            throw std::runtime_error("pad controls check: found no local commander");
        const auto map_width =
            static_cast<int32_t>(runtime.selected_tnt_->tile_width * kMapPixelsPerTile);
        const auto map_height =
            static_cast<int32_t>(runtime.selected_tnt_->tile_height * kMapPixelsPerTile);
        const auto [x, z] = map_of(runtime, run.commander);
        // The Peewees go toward the middle of the map, side by side, a third below them.
        const int32_t way_x = x < map_width / 2 ? 1 : -1;
        const int32_t way_z = z < map_height / 2 ? 1 : -1;
        run.peewees[0] = spawn(runtime, "ARMPW", x + way_x * kUnitReach, z);
        run.peewees[1] = spawn(runtime, "ARMPW", x + way_x * (kUnitReach + kUnitSpacing), z);
        run.peewees[2] =
            spawn(runtime, "ARMPW", x + way_x * kUnitReach, z + way_z * 2 * kUnitSpacing);
        // The lab goes on the commander's other side; anywhere near it when that side has no
        // room.
        const auto lab_type = type_of(runtime, "ARMLAB");
        const auto& lab_def = runtime.spawn_types_[lab_type];
        const int32_t cell_x = x / kMapPixelsPerCell;
        const int32_t cell_z = z / kMapPixelsPerCell;
        std::optional<std::pair<int32_t, int32_t>> lab_site;
        for (int32_t reach = kLabFirstReach; reach < kLabLastReach && !lab_site; ++reach)
            for (int32_t dz = -reach; dz <= reach && !lab_site; dz += kSiteCellStep)
                if (runtime.match_->building_site(lab_type, cell_x - way_x * reach, cell_z + dz, 0))
                    lab_site = std::pair{cell_x - way_x * reach, cell_z + dz};
        if (lab_site) {
            run.lab = spawn(
                runtime,
                "ARMLAB",
                (lab_site->first * kSiteCellStep + lab_def.footprint_x) * kHalfCellPixels,
                (lab_site->second * kSiteCellStep + lab_def.footprint_z) * kHalfCellPixels
            );
        } else {
            const auto* lab = runtime.place_finished_structure(lab_type, run.commander);
            if (lab == nullptr)
                throw std::runtime_error("pad controls check: found no site for ARMLAB");
            run.lab = lab->unit_index;
        }
        auto& player = runtime.match_->world().players[runtime.match_local_player_];
        player.metal = player.metal_cap;
        player.energy = player.energy_cap;
    }

    // ---- The cases ------------------------------------------------------------------------

    /// X0. No pad: before any stand-in, F13 is not taken, FORCE adds nothing and nothing is laid
    /// out for the pad.
    static void case_no_pad(Runtime& runtime, PadRun& run) {
        require(run.deck.id == 0 && run.xbox.id == 0, Part::check, "X0 runs before any stand-in");
        require(!runtime.pad_used(), Part::dispatch, "pad_used() is true before any gamepad");
        for (const bool down : {true, false}) {
            SDL_Event event = key_event(run, SDLK_F13, SDL_SCANCODE_F13, down);
            require(
                !runtime.take_pad_event(event, run.running),
                Part::dispatch,
                "F13 was taken with no gamepad open (the grips' keys are read only while a pad "
                "is open)"
            );
        }
        send_key(runtime, run, SDLK_F13, SDL_SCANCODE_F13, true);
        send_key(runtime, run, SDLK_F13, SDL_SCANCODE_F13, false);
        if (const auto* state = runtime.pad_state_if_made())
            require(
                !state->grip_keys_seen && !state->force && !state->used,
                Part::dispatch,
                "F13 with no gamepad open changed the pad state"
            );
        require(!runtime.pad_force_held(), Part::dispatch, "FORCE is held with no gamepad");
        SDL_SetModState(SDL_KMOD_NONE);
        for (const auto use :
             {Runtime::ModifierUse::keyboard,
              Runtime::ModifierUse::selection,
              Runtime::ModifierUse::order,
              Runtime::ModifierUse::build_button})
            require(
                runtime.input_modifiers(use) == SDL_GetModState(),
                Part::dispatch,
                "with no gamepad input_modifiers differs from SDL_GetModState"
            );
        if (const auto* state = runtime.touch_state_if_made()) {
            require(
                !state->hud.pad.hud && !state->hud.pad.badges && !state->hud.pad.force_shown,
                Part::dispatch,
                "the pad's looks are on with no gamepad (HudState::pad)"
            );
            if (state->frame_ready) {
                for (std::size_t at = 0; at < state->frame.control_count; ++at)
                    require(
                        state->frame.controls[at].control != hud::Control::force,
                        Part::hud,
                        "the touch layer lays out FORCE with no gamepad"
                    );
                // The phone layout's touch controls have a status pill of their own.
                require(
                    !run.side_panel || !has_area(state->frame.status),
                    Part::hud,
                    "the touch layer lays out the pad HUD's status pill with no gamepad"
                );
            }
        }
    }

    /// K0. Detection and route: the stand-in Deck opened with its traits, pad_used() false before
    /// its input and true after, the Steam Deck glyphs, the trackpads map, and Controller listed
    /// in a settings dialog opened after.
    static void case_detection(Runtime& runtime, PadRun& run) {
        attach_deck(runtime, run);
        const OpenPad* open = open_pad(runtime, run.deck.id);
        require(
            open != nullptr && open->gamepad != nullptr,
            Part::dispatch,
            "the dispatcher did not open the Steam Deck stand-in on SDL_EVENT_GAMEPAD_ADDED"
        );
        const auto& traits = open->traits;
        require(
            traits.type == pad::PadType::steam_deck,
            Part::dispatch,
            "the Steam Deck stand-in's type is not steam_deck (pad_type_of on 0x28de/0x1205)"
        );
        require(
            traits.trackpads == kDeckTouchpads.size() && traits.grips && traits.gyro &&
                !traits.steam_input,
            Part::dispatch,
            "the Steam Deck stand-in's traits miss its two trackpads, grips or gyro (trackpads " +
                std::to_string(traits.trackpads) + ")"
        );
        require(
            traits.vendor == pad::valve_vendor && traits.product == pad::steam_deck_product,
            Part::dispatch,
            "the Steam Deck stand-in's traits carry other USB numbers"
        );
        require(
            !runtime.pad_used(), Part::dispatch, "pad_used() is true before the pad sent input"
        );
        require(
            !touch(runtime).hud.pad.hud,
            Part::dispatch,
            "the slim pad HUD shows before the pad sent input"
        );
        require(
            pad::glyph_style_for(
                pad::Prompts::automatic,
                pad::pad_type_of(traits.vendor, traits.product, pad::PadType::standard)
            ) == pad::GlyphStyle::steam_deck,
            Part::pad_model,
            "Automatic prompts do not pick the Steam Deck glyphs for the Deck"
        );
        tap(runtime, run, run.deck, pad::PadButton::x);
        step(runtime, run, kFrameMs);
        require(runtime.pad_used(), Part::dispatch, "pad_used() is false after the pad sent input");
        require(
            runtime.pad_state().active == run.deck.id,
            Part::dispatch,
            "the Deck stand-in is not the active pad after its input"
        );
        const auto& look = touch(runtime).hud.pad;
        require(
            look.glyphs == pad::GlyphStyle::steam_deck,
            Part::dispatch,
            "HudState::pad.glyphs is not the Steam Deck set after the Deck's input"
        );
        require(
            look.map.scheme == pad::Scheme::trackpads && look.map.trackpads && !look.map.fallback,
            Part::dispatch,
            "the Deck's map is not the trackpads scheme with grips (HudState::pad.map)"
        );
        auto& dialog = runtime.open_engine_settings_dialog();
        const bool listed = dialog.controller;
        (void)runtime.take_engine_settings_action(settings::DialogAction::cancelled);
        require(
            listed,
            Part::settings,
            "a settings dialog opened after pad input does not list Controller (Dialog::controller)"
        );
    }

    /// K1. The right trackpad moves the pointer as the pad model says, hovers what it rests
    /// on, and rests at the screen's edge to scroll the camera as a mouse does.
    static void case_pointer(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        point_at(runtime, run, centre_of(runtime.overlay_area()), "the battlefield's middle");
        require(
            runtime.match_pointer_known_,
            Part::dispatch,
            "the pad pointer's motion left match_pointer_known_ false (synthetic mouse events)"
        );
        // A slow slide to the right, compared with the pad model's own pointer.
        pad::PadPointer model;
        model.configure(
            runtime.pad_settings(),
            {static_cast<float>(runtime.match_layout_.width),
             static_cast<float>(runtime.match_layout_.height)},
            {}
        );
        CanvasPoint thumb{kSlideStartPads, kPadMiddle};
        set_thumb(run.deck, pad::Side::right, true, thumb);
        step(runtime, run, kFrameMs);
        model.touch_down({thumb.x, thumb.y}, run.clock_ns);
        step(runtime, run, kFrameMs);
        const float start_x = runtime.match_pointer_x_;
        float expected = 0.0F;
        for (int sample = 0; sample < kSlideSamples; ++sample) {
            thumb.x += kSlideStepPads;
            set_thumb(run.deck, pad::Side::right, true, thumb);
            step(runtime, run, kFrameMs);
            expected += model.touch_move({thumb.x, thumb.y}, run.clock_ns).move.x;
        }
        set_thumb(run.deck, pad::Side::right, false, thumb);
        step(runtime, run, kFrameMs);
        const float moved = runtime.match_pointer_x_ - start_x;
        require(
            moved > 0.0F,
            Part::dispatch,
            "a slide to the right on the right trackpad did not move the pointer right"
        );
        require(
            std::fabs(moved - expected) <=
                std::max(kSlideTolerancePx, kSlideToleranceShare * expected),
            Part::dispatch,
            "the right trackpad moved the pointer " + number_text(moved) +
                " px where the pad model moves it " + number_text(expected) + " px"
        );
        // Hover.
        select_none(runtime);
        point_at(runtime, run, canvas_of(runtime, run.commander), "the commander");
        require(
            runtime.hovered_match_unit_ == run.commander,
            Part::dispatch,
            "the pad pointer resting on the commander does not hover it"
        );
        require(
            !selected(runtime, run.commander),
            Part::dispatch,
            "the pad pointer selected the commander with no click"
        );
        // The edge.
        look_at_middle(runtime);
        step(runtime, run, kFrameMs);
        const auto field = battlefield(runtime);
        point_at(
            runtime,
            run,
            {static_cast<float>(runtime.match_layout_.width - 1),
             static_cast<float>(field.y + field.height / 2)},
            "the screen's right edge"
        );
        // The thumb slides on past the edge: the pointer stops on the edge's outermost pixel.
        CanvasPoint push_on{kPadMiddle, kPadMiddle};
        set_thumb(run.deck, pad::Side::right, true, push_on);
        for (int frame = 0; frame < kLandingFrames; ++frame)
            step(runtime, run, kFrameMs);
        for (int sample = 0; sample < kSlideSamples; ++sample) {
            push_on.x += kEdgePushStepPads;
            set_thumb(run.deck, pad::Side::right, true, push_on);
            step(runtime, run, kFrameMs);
        }
        set_thumb(run.deck, pad::Side::right, false, push_on);
        step(runtime, run, kFrameMs);
        const auto edge = pointer(runtime);
        const auto way = oa::ui::hud::edge_scroll(
            static_cast<int32_t>(std::floor(edge.x)),
            static_cast<int32_t>(std::floor(edge.y)),
            runtime.match_layout_.width,
            runtime.match_layout_.height,
            1
        );
        require(
            runtime.match_pointer_known_ && way.x > 0 && way.y == 0,
            Part::dispatch,
            "the pad pointer pushed past the right edge does not rest on its outermost pixel, "
            "where the edge scroll reads it (at " +
                number_text(edge.x) + ")"
        );
        const auto flags = SDL_GetWindowFlags(runtime.sdl_.window);
        if ((flags & SDL_WINDOW_MOUSE_FOCUS) != 0)
            require(
                edge_scrolls(runtime),
                Part::dispatch,
                "the pad pointer resting at the screen's right edge does not scroll the camera"
            );
        else
            run.notes.push_back(
                "K1: the window has no mouse focus under this video driver, so the edge's scroll "
                "was read from the pointer's place, not run"
            );
        // The gyro has no case of its own (the pad model's tests check its gain); what a turn
        // gives through SDL's sensor events is noted for people.
        set_pad_settings(runtime, [](settings::EngineSettings& wanted) {
            wanted.pad_gyro = pad::Gyro::always;
        });
        point_at(runtime, run, centre_of(runtime.overlay_area()), "the battlefield's middle");
        const float before_turn = pointer(runtime).x;
        for (int frame = 0; frame < kGyroProbeFrames; ++frame) {
            send_gyro(run.deck, 0.0F, kGyroProbeYaw, 0.0F);
            step(runtime, run, kFrameMs);
        }
        run.notes.push_back(
            "K1: with Gyro pointer Always, a turn to the left of " + number_text(kGyroProbeYaw) +
            " rad/s for " + std::to_string(kGyroProbeFrames * kFrameMs) + " ms moved the pointer " +
            number_text(pointer(runtime).x - before_turn) + " px across" +
            (run.deck.record->sensors_on ? "" : "; the game never turned the sensors on")
        );
    }

    /// K2. R2, A and a press of the right trackpad click at the pointer; two R2 presses within
    /// the double click's window select every unit of the type.
    static void case_click(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        const std::array<pad::PadButton, 2> buttons{pad::PadButton::r2, pad::PadButton::a};
        for (const auto button : buttons) {
            select_none(runtime);
            point_at(runtime, run, canvas_of(runtime, run.commander), "the commander");
            click(runtime, run, run.deck, button);
            require(
                selected(runtime, run.commander),
                Part::dispatch,
                button_name(button) + " with the pointer on the commander did not select it"
            );
        }
        select_none(runtime);
        point_at(runtime, run, canvas_of(runtime, run.commander), "the commander");
        pad_press(runtime, run, run.deck);
        require(
            selected(runtime, run.commander),
            Part::dispatch,
            "a press of the right trackpad on the commander did not select it"
        );
        const auto [a, b, c] = run.peewees;
        look_at_units(runtime, {a, b, c});
        select_none(runtime);
        step(runtime, run, kFrameMs);
        point_at(runtime, run, canvas_of(runtime, a), "a Peewee");
        tap(runtime, run, run.deck, pad::PadButton::r2);
        step(runtime, run, kTapMs);
        tap(runtime, run, run.deck, pad::PadButton::r2);
        step(runtime, run, kAfterTapMs);
        require(
            selected(runtime, a) && selected(runtime, b) && selected(runtime, c) &&
                !selected(runtime, run.commander),
            Part::dispatch,
            "two R2 presses on a Peewee did not select every Peewee (a click of 2)"
        );
    }

    /// K3. R2 held while the pointer travels draws a box that selects the units in it.
    static void case_box(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        const auto [a, b, c] = run.peewees;
        look_at_units(runtime, {a, b, c});
        step(runtime, run, kFrameMs);
        const auto [start, end] = box_around(runtime, run, {a, b});
        point_at(runtime, run, start, "a box's first corner");
        push(runtime, run, run.deck, pad::PadButton::r2);
        point_at(runtime, run, end, "a box's far corner");
        lift(runtime, run, run.deck, pad::PadButton::r2);
        step(runtime, run, kFrameMs);
        require(
            selected(runtime, a) && selected(runtime, b) && !selected(runtime, c) &&
                !selected(runtime, run.commander),
            Part::dispatch,
            "R2 held while the pointer crossed two Peewees did not box both alone"
        );
    }

    /// K4. L2 clears the selection or takes back an armed order (left-click interface); on a
    /// factory's build button, or its wedge of the build ring on the phone layout, it takes one
    /// off the queue.
    static void case_right_button(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        select_only(runtime, run.commander);
        const auto ground = ground_beside(runtime, run, run.commander, 0.0F, -kGroundOffsetPoints);
        point_at(runtime, run, ground, "open ground");
        click(runtime, run, run.deck, pad::PadButton::l2);
        require(
            !runtime.has_local_selection(),
            Part::dispatch,
            "L2 on the battlefield did not clear the selection (left-click interface)"
        );
        select_only(runtime, run.peewees[0]);
        require(runtime.arm_match_command("MOVE", false), Part::check, "MOVE could not be armed");
        click(runtime, run, run.deck, pad::PadButton::l2);
        require(
            runtime.match_command_ == MatchCommand::none && selected(runtime, run.peewees[0]),
            Part::dispatch,
            "L2 did not take back the armed MOVE alone"
        );
        const auto kbot = type_of(runtime, "ARMPW");
        select_only(runtime, run.lab);
        look_at_units(runtime, {run.lab});
        step(runtime, run, kFrameMs);
        empty_factory(runtime, run.lab, kbot);
        const auto button = build_button(runtime, "ARMPW");
        runtime.activate_match_hud(button, true);
        runtime.activate_match_hud(button, true);
        require(
            runtime.match_->queued_build_count(run.lab, kbot) == 2,
            Part::check,
            "the lab did not queue two Peewees"
        );
        if (run.side_panel) {
            point_at(runtime, run, gadget_centre(runtime, button), "the ARMPW build button");
            click(runtime, run, run.deck, pad::PadButton::l2);
        } else {
            aim_ring_from(runtime, run, run.lab, button, "ARMPW");
            click(runtime, run, run.deck, pad::PadButton::l2);
            let_ring_go(runtime, run);
        }
        require(
            runtime.match_->queued_build_count(run.lab, kbot) == 1,
            Part::dispatch,
            run.side_panel ? "L2 on the ARMPW build button did not take one off the lab's queue"
                           : "L2 on the build ring's ARMPW did not take one off the lab's queue"
        );
        empty_factory(runtime, run.lab, kbot);
    }

    /// K5. ADD: L4 held adds a click's unit; a tap of L4 latches ADD, which the slim HUD's ADD chip
    /// turns off, and the other way round (one set of latches).
    static void case_add(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        const auto a = run.peewees[0];
        select_only(runtime, run.commander);
        look_at_units(runtime, {run.commander, a});
        step(runtime, run, kFrameMs);
        point_at(runtime, run, canvas_of(runtime, a), "a Peewee");
        push(runtime, run, run.deck, pad::PadButton::l4);
        click(runtime, run, run.deck, pad::PadButton::r2);
        lift(runtime, run, run.deck, pad::PadButton::l4);
        require(
            selected(runtime, run.commander) && selected(runtime, a),
            Part::dispatch,
            "R2 on a Peewee with L4 held did not add it to the selection"
        );
        auto& latches = touch(runtime).hud.latches;
        latches.clear();
        tap(runtime, run, run.deck, pad::PadButton::l4);
        require(latches.latched(hud::Latch::add), Part::dispatch, "a tap of L4 did not latch ADD");
        const auto* chip = find_control(runtime, hud::Control::add, -1);
        require(chip != nullptr, Part::hud, "the slim pad HUD lays out no ADD chip");
        const auto chip_point = centre_of(chip->rect);
        point_at(runtime, run, chip_point, "the ADD chip");
        const auto at_pointer = hud::hit(
            touch(runtime).frame,
            {static_cast<int>(pointer(runtime).x), static_cast<int>(pointer(runtime).y)},
            0
        );
        require(
            at_pointer.has_value() && at_pointer->control == hud::Control::add,
            Part::check,
            "the pointer rests off the ADD chip"
        );
        click(runtime, run, run.deck, pad::PadButton::a);
        require(
            !latches.latched(hud::Latch::add),
            Part::hud,
            "A with the pointer on the ADD chip did not turn off the ADD that a tap of L4 latched "
            "(the touch layer takes the pad pointer's clicks on its controls)"
        );
        click(runtime, run, run.deck, pad::PadButton::a);
        require(latches.latched(hud::Latch::add), Part::hud, "A on the ADD chip did not latch ADD");
        tap(runtime, run, run.deck, pad::PadButton::l4);
        require(
            !latches.latched(hud::Latch::add),
            Part::dispatch,
            "a tap of L4 did not turn off the ADD the chip latched"
        );
    }

    /// K6. QUEUE: R4 held queues R2's orders, a tap latches it; on a build button R4 is x5, and on
    /// the build ring's wedge of a factory (the phone layout's) R4 and R2 queue five.
    static void case_queue(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        const auto a = run.peewees[0];
        hold_units_still(runtime);
        select_only(runtime, a);
        look_at_units(runtime, {a});
        step(runtime, run, kFrameMs);
        const auto first = ground_beside(runtime, run, a, 0.0F, -kGroundOffsetPoints);
        const auto second =
            ground_beside(runtime, run, a, -kGroundOffsetPoints, -kGroundOffsetPoints);
        push(runtime, run, run.deck, pad::PadButton::r4);
        point_at(runtime, run, first, "a ground point");
        const auto first_ground = ground_at(runtime, pointer(runtime));
        click(runtime, run, run.deck, pad::PadButton::r2);
        point_at(runtime, run, second, "a second ground point");
        const auto second_ground = ground_at(runtime, pointer(runtime));
        click(runtime, run, run.deck, pad::PadButton::r2);
        lift(runtime, run, run.deck, pad::PadButton::r4);
        require(
            moves_to(queue_of(runtime, a), {first_ground, second_ground}),
            Part::dispatch,
            "two R2 clicks with R4 held did not queue two moves (queued " +
                queue_text(queue_of(runtime, a)) + ")"
        );
        auto& latches = touch(runtime).hud.latches;
        latches.clear();
        tap(runtime, run, run.deck, pad::PadButton::r4);
        require(
            latches.latched(hud::Latch::queue), Part::dispatch, "a tap of R4 did not latch QUEUE"
        );
        tap(runtime, run, run.deck, pad::PadButton::r4);
        require(
            !latches.latched(hud::Latch::queue), Part::dispatch, "a second tap of R4 left QUEUE on"
        );
        // x5 on a build button.
        const auto kbot = type_of(runtime, "ARMPW");
        select_only(runtime, run.lab);
        look_at_units(runtime, {run.lab});
        step(runtime, run, kFrameMs);
        empty_factory(runtime, run.lab, kbot);
        const auto button = build_button(runtime, "ARMPW");
        if (!run.side_panel) {
            // The phone layout's build buttons are the build ring's wedges, where R4 is QUEUE
            // and an R2 with it queues five.
            aim_ring_from(runtime, run, run.lab, button, "ARMPW");
            push(runtime, run, run.deck, pad::PadButton::r4);
            click(runtime, run, run.deck, pad::PadButton::r2);
            lift(runtime, run, run.deck, pad::PadButton::r4);
            let_ring_go(runtime, run);
            require(
                runtime.match_->queued_build_count(run.lab, kbot) == kTimesFive,
                Part::dispatch,
                "R2 on the build ring's ARMPW with R4 held did not queue five"
            );
            empty_factory(runtime, run.lab, kbot);
            return;
        }
        point_at(runtime, run, gadget_centre(runtime, button), "the ARMPW build button");
        push(runtime, run, run.deck, pad::PadButton::r4);
        require(
            latches.active(hud::Latch::times_five) && !latches.active(hud::Latch::queue),
            Part::dispatch,
            "R4 pressed on a build button did not press x5 instead of QUEUE"
        );
        require(
            touch(runtime).hud.pad.over_build_button,
            Part::dispatch,
            "HudState::pad.over_build_button is off with the pointer on a build button"
        );
        click(runtime, run, run.deck, pad::PadButton::r2);
        lift(runtime, run, run.deck, pad::PadButton::r4);
        require(
            runtime.match_->queued_build_count(run.lab, kbot) == kTimesFive,
            Part::dispatch,
            "R2 on the ARMPW build button with R4 held did not queue five"
        );
        empty_factory(runtime, run.lab, kbot);
    }

    /// K7. FORCE: R5 held gives Ctrl to orders and selection, never to keys; R5 and R2 on the
    /// ground give the orders Ctrl and a click give.
    static void case_force(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        SDL_SetModState(SDL_KMOD_NONE);
        push(runtime, run, run.deck, pad::PadButton::r5);
        const bool held = runtime.pad_force_held();
        const auto order = runtime.input_modifiers(Runtime::ModifierUse::order);
        const auto selection = runtime.input_modifiers(Runtime::ModifierUse::selection);
        const auto keyboard = runtime.input_modifiers(Runtime::ModifierUse::keyboard);
        const auto build = runtime.input_modifiers(Runtime::ModifierUse::build_button);
        const bool lit = touch(runtime).hud.pad.force_active;
        lift(runtime, run, run.deck, pad::PadButton::r5);
        require(held, Part::dispatch, "R5 held does not hold FORCE (pad_force_held)");
        require(
            (order & SDL_KMOD_LCTRL) != 0 && (selection & SDL_KMOD_LCTRL) != 0,
            Part::dispatch,
            "with R5 held input_modifiers gives orders or selection no Ctrl"
        );
        require(
            (keyboard & SDL_KMOD_CTRL) == 0 && (build & SDL_KMOD_CTRL) == 0,
            Part::dispatch,
            "with R5 held input_modifiers gives keys or build buttons Ctrl"
        );
        require(
            lit, Part::dispatch, "the FORCE chip is not lit while R5 is held (pad.force_active)"
        );
        require(
            !runtime.pad_force_held() &&
                (runtime.input_modifiers(Runtime::ModifierUse::order) & SDL_KMOD_CTRL) == 0,
            Part::dispatch,
            "FORCE stayed on after R5 came up"
        );
        const auto unit = run.peewees[0];
        hold_units_still(runtime);
        select_only(runtime, unit);
        look_at_units(runtime, {unit});
        step(runtime, run, kFrameMs);
        const auto ground = ground_beside(runtime, run, unit, kGroundOffsetPoints, 0.0F);
        push(runtime, run, run.deck, pad::PadButton::r5);
        point_at(runtime, run, ground, "open ground");
        const auto clicked = pointer(runtime);
        click(runtime, run, run.deck, pad::PadButton::r2);
        lift(runtime, run, run.deck, pad::PadButton::r5);
        const auto by_pad = queue_of(runtime, unit);
        runtime.match_->stop_orders(unit);
        mouse_click(runtime, run, clicked, SDL_KMOD_LCTRL);
        const auto by_mouse = queue_of(runtime, unit);
        require(!by_mouse.empty(), Part::check, "Ctrl and a click on the ground gave no order");
        require(
            same_orders(by_pad, by_mouse),
            Part::dispatch,
            "R5 and R2 on the ground gave " + queue_text(by_pad) + " where Ctrl and a click give " +
                queue_text(by_mouse)
        );
    }

    /// K8. The groups layer: L5 with a D-pad arm or a face button held stores the selection,
    /// tapped selects the group, with L4 adds it; L5 shows the left trackpad's ring of groups,
    /// whose wedge a press selects.
    static void case_groups(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        const auto [a, b, c] = run.peewees;
        look_at_units(runtime, {a, b, c});
        select_only(runtime, a);
        step(runtime, run, kFrameMs);
        push(runtime, run, run.deck, pad::PadButton::l5);
        hold(runtime, run, run.deck, pad::PadButton::dpad_up);
        lift(runtime, run, run.deck, pad::PadButton::dpad_up);
        lift(runtime, run, run.deck, pad::PadButton::l5);
        select_none(runtime);
        runtime.select_squad(1, false);
        require(
            selected(runtime, a) && !selected(runtime, b),
            Part::dispatch,
            "L5 with D-pad up held did not store the selection in group 1"
        );
        select_none(runtime);
        step(runtime, run, kFrameMs);
        push(runtime, run, run.deck, pad::PadButton::l5);
        tap(runtime, run, run.deck, pad::PadButton::dpad_up);
        lift(runtime, run, run.deck, pad::PadButton::l5);
        require(
            selected(runtime, a) && !selected(runtime, b) && !selected(runtime, c),
            Part::dispatch,
            "L5 with a tap of D-pad up did not select group 1"
        );
        // Group 5 by Y, added with L4.
        select_only(runtime, b);
        step(runtime, run, kFrameMs);
        push(runtime, run, run.deck, pad::PadButton::l5);
        hold(runtime, run, run.deck, pad::PadButton::y);
        lift(runtime, run, run.deck, pad::PadButton::y);
        lift(runtime, run, run.deck, pad::PadButton::l5);
        select_only(runtime, a);
        step(runtime, run, kFrameMs);
        push(runtime, run, run.deck, pad::PadButton::l5);
        push(runtime, run, run.deck, pad::PadButton::l4);
        tap(runtime, run, run.deck, pad::PadButton::y);
        lift(runtime, run, run.deck, pad::PadButton::l4);
        lift(runtime, run, run.deck, pad::PadButton::l5);
        require(
            selected(runtime, a) && selected(runtime, b) && !selected(runtime, c),
            Part::dispatch,
            "L5 and L4 with a tap of Y did not add group 5 to the selection"
        );
        // The left trackpad's ring.
        select_none(runtime);
        step(runtime, run, kFrameMs);
        push(runtime, run, run.deck, pad::PadButton::l5);
        require(
            touch(runtime).hud.pad.groups_layer,
            Part::dispatch,
            "HudState::pad.groups_layer is off while L5 is held"
        );
        require(
            touch(runtime).hud.pad.group_ring.has_value(),
            Part::dispatch,
            "holding L5 on the Deck shows no group ring (HudState::pad.group_ring)"
        );
        const auto aim = wedge_pad_point(0, static_cast<uint8_t>(hud::group_ring_slot_count));
        set_thumb(run.deck, pad::Side::left, true, aim);
        step(runtime, run, kFrameMs);
        step(runtime, run, kFrameMs);
        snapshot(runtime, run, "pad-groups.ppm");
        const auto& ring = touch(runtime).hud.pad.group_ring;
        require(
            ring.has_value() && ring->aim == 1,
            Part::dispatch,
            "the left thumb at the ring's top does not aim at group 1"
        );
        tap(runtime, run, run.deck, pad::PadButton::left_pad);
        set_thumb(run.deck, pad::Side::left, false, aim);
        step(runtime, run, kFrameMs);
        lift(runtime, run, run.deck, pad::PadButton::l5);
        require(
            selected(runtime, a) && !selected(runtime, b),
            Part::dispatch,
            "a press of the left trackpad on the ring's group 1 did not select it"
        );
    }

    /// Returns the open order ring's slot of an item, failing without it.
    ///
    /// @param runtime the runtime
    /// @param item the item
    /// @return the slot
    static uint8_t radial_slot(Runtime& runtime, hud::RadialItem item) {
        const auto& radial = touch(runtime).hud.radial;
        require(radial.has_value(), Part::dispatch, "the order ring is not open");
        for (std::size_t slot = 0; slot < radial->wedges.size(); ++slot)
            if (radial->wedges[slot].item == item) {
                require(
                    radial->wedges[slot].available,
                    Part::dispatch,
                    "the order ring greys an order a Peewee takes"
                );
                return static_cast<uint8_t>(slot);
            }
        fail(Part::hud, "the order ring has no wedge for an item it must show");
    }

    /// Aims the open order ring at a slot with the right trackpad and runs a frame.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param slot the wedge
    static void aim_radial(Runtime& runtime, PadRun& run, uint8_t slot) {
        set_thumb(
            run.deck,
            pad::Side::right,
            true,
            wedge_pad_point(slot, static_cast<uint8_t>(hud::radial_slot_count))
        );
        step(runtime, run, kFrameMs);
        step(runtime, run, kFrameMs);
        require(
            touch(runtime).hud.pad.radial_aim == slot,
            Part::dispatch,
            "the right thumb toward a wedge does not aim the order ring at it (pad.radial_aim)"
        );
    }

    /// K9. The order ring: R1 opens it at the pointer; the right thumb aims; releasing R1 gives
    /// the order at the ring's point; A arms it; B, or a release at the centre after the hold
    /// delay, closes it; a tap of R1 keeps it open for R2.
    static void case_order_ring(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        const auto unit = run.peewees[0];
        hold_units_still(runtime);
        select_only(runtime, unit);
        look_at_units(runtime, {unit});
        step(runtime, run, kFrameMs);
        const auto want = ground_beside(runtime, run, unit, kGroundOffsetPoints, 0.0F);
        point_at(runtime, run, want, "the ring's ground point");
        const auto point = pointer(runtime);
        push(runtime, run, run.deck, pad::PadButton::r1);
        const auto& radial = touch(runtime).hud.radial;
        require(radial.has_value(), Part::dispatch, "R1 did not open the order ring");
        require(
            std::fabs(static_cast<float>(radial->anchor.x) - point.x) <= kRingAnchorSlackPx &&
                std::fabs(static_cast<float>(radial->anchor.y) - point.y) <= kRingAnchorSlackPx,
            Part::dispatch,
            "the order ring opened away from the pointer"
        );
        // The ring gives at its own point, the pointer's pixel.
        const auto ground = ground_at(
            runtime, {static_cast<float>(radial->anchor.x), static_cast<float>(radial->anchor.y)}
        );
        require(
            touch(runtime).hud.pad.ring_by_pad,
            Part::dispatch,
            "HudState::pad.ring_by_pad is off for the ring R1 opened"
        );
        const auto patrol = radial_slot(runtime, hud::RadialItem::patrol);
        aim_radial(runtime, run, patrol);
        snapshot(runtime, run, "pad-order-ring.ppm");
        lift(runtime, run, run.deck, pad::PadButton::r1);
        set_thumb(run.deck, pad::Side::right, false, {kPadMiddle, kPadMiddle});
        step(runtime, run, kFrameMs);
        const auto given = patrols_of(runtime, unit);
        require(
            given.size() == 1 && given.front()[0] == ground[0] && given.front()[2] == ground[2],
            Part::dispatch,
            "releasing R1 on Patrol did not patrol the Peewee to the ring's point (queued " +
                queue_text(queue_of(runtime, unit)) + ", the point " +
                std::to_string(ground[0] >> kFixedPointShift) + "," +
                std::to_string(ground[2] >> kFixedPointShift) + ")"
        );
        require(!radial.has_value(), Part::dispatch, "the order ring stayed open after a give");
        // A arms.
        runtime.match_->stop_orders(unit);
        push(runtime, run, run.deck, pad::PadButton::r1);
        const auto attack = radial_slot(runtime, hud::RadialItem::attack);
        aim_radial(runtime, run, attack);
        tap(runtime, run, run.deck, pad::PadButton::a);
        const bool armed = runtime.match_command_ == MatchCommand::attack && !radial.has_value();
        set_thumb(run.deck, pad::Side::right, false, {kPadMiddle, kPadMiddle});
        lift(runtime, run, run.deck, pad::PadButton::r1);
        require(armed, Part::dispatch, "A on the ring's Attack did not arm ATTACK and close it");
        require(
            queue_of(runtime, unit).empty(), Part::dispatch, "arming from the ring gave an order"
        );
        runtime.reset_match_command();
        // The centre after the hold delay closes.
        hold(runtime, run, run.deck, pad::PadButton::r1);
        require(radial.has_value(), Part::dispatch, "R1 held did not keep the order ring open");
        lift(runtime, run, run.deck, pad::PadButton::r1);
        require(
            !radial.has_value() && queue_of(runtime, unit).empty(),
            Part::dispatch,
            "releasing R1 at the centre after the hold delay did not close the ring alone"
        );
        // B closes.
        push(runtime, run, run.deck, pad::PadButton::r1);
        tap(runtime, run, run.deck, pad::PadButton::b);
        const bool closed = !radial.has_value();
        lift(runtime, run, run.deck, pad::PadButton::r1);
        require(closed, Part::dispatch, "B did not close the order ring");
        // A tap keeps it open; R2 gives.
        point_at(runtime, run, want, "the ring's ground point");
        tap(runtime, run, run.deck, pad::PadButton::r1);
        step(runtime, run, kFrameMs);
        require(radial.has_value(), Part::dispatch, "a tap of R1 did not keep the order ring open");
        aim_radial(runtime, run, radial_slot(runtime, hud::RadialItem::patrol));
        click(runtime, run, run.deck, pad::PadButton::r2);
        set_thumb(run.deck, pad::Side::right, false, {kPadMiddle, kPadMiddle});
        step(runtime, run, kFrameMs);
        require(
            patrols_of(runtime, unit).size() == 1 && !radial.has_value(),
            Part::dispatch,
            "R2 in the ring a tap of R1 kept open did not give Patrol"
        );
    }

    /// Returns the open build ring's slot whose wedge presses a gadget, failing without it.
    ///
    /// @param runtime the runtime
    /// @param gadget the gadget
    /// @param name the gadget's name, for the message
    /// @return the slot
    static uint8_t build_slot(Runtime& runtime, std::size_t gadget, std::string_view name) {
        const auto& ring = touch(runtime).hud.build_ring;
        require(ring.has_value(), Part::dispatch, "the build ring is not open");
        for (std::size_t slot = 0; slot < ring->wedges.size(); ++slot)
            if (ring->wedges[slot].kind == hud::BuildWedgeKind::build &&
                ring->wedges[slot].gadget == static_cast<int16_t>(gadget))
                return static_cast<uint8_t>(slot);
        fail(Part::dispatch, "the build ring has no wedge for " + std::string(name));
    }

    /// Aims the open build ring at a slot with the right trackpad and runs a frame.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param slot the wedge
    static void aim_build(Runtime& runtime, PadRun& run, uint8_t slot) {
        set_thumb(
            run.deck,
            pad::Side::right,
            true,
            wedge_pad_point(slot, static_cast<uint8_t>(hud::build_ring_slot_count))
        );
        step(runtime, run, kFrameMs);
        step(runtime, run, kFrameMs);
        require(
            touch(runtime).hud.pad.build_aim == slot,
            Part::dispatch,
            "the right thumb toward a wedge does not aim the build ring at it (pad.build_aim)"
        );
    }

    /// Opens the build ring of the selection at open ground beside a unit, with L1 held, and aims
    /// it at a build button's wedge.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    /// @param beside the unit the ring opens beside
    /// @param button the build button
    /// @param name the unit type's name, which the button carries
    static void aim_ring_from(
        Runtime& runtime, PadRun& run, uint16_t beside, std::size_t button, std::string_view name
    ) {
        point_at(
            runtime,
            run,
            ground_beside(runtime, run, beside, 0.0F, kGroundOffsetPoints),
            "open ground"
        );
        push(runtime, run, run.deck, pad::PadButton::l1);
        require(
            touch(runtime).hud.build_ring.has_value(),
            Part::dispatch,
            "L1 did not open the build ring for " + std::string(name)
        );
        aim_build(runtime, run, build_slot(runtime, button, name));
    }

    /// Lets L1 go on the build ring's aimed wedge, then the right thumb: a builder's ring arms
    /// the building, a factory's closes.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    static void let_ring_go(Runtime& runtime, PadRun& run) {
        lift(runtime, run, run.deck, pad::PadButton::l1);
        set_thumb(run.deck, pad::Side::right, false, {kPadMiddle, kPadMiddle});
        step(runtime, run, kFrameMs);
    }

    /// K10. The build ring: L1 opens it with the builder's page, PREV at W and NEXT at E;
    /// releasing L1 on a building arms it for the pointer; on a factory R2 adds one and L2 takes
    /// one off.
    static void case_build_ring(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        const auto solar = type_of(runtime, "ARMSOLAR");
        select_only(runtime, run.commander);
        look_at_units(runtime, {run.commander});
        step(runtime, run, kFrameMs);
        const auto solar_button = build_button(runtime, "ARMSOLAR");
        const auto point = ground_beside(runtime, run, run.commander, kGroundOffsetPoints, 0.0F);
        point_at(runtime, run, point, "open ground");
        push(runtime, run, run.deck, pad::PadButton::l1);
        const auto& ring = touch(runtime).hud.build_ring;
        require(
            ring.has_value() && !ring->standing_orders,
            Part::dispatch,
            "L1 with the commander selected did not open the build ring"
        );
        require(
            ring->wedges[hud::build_ring_prev_slot].kind == hud::BuildWedgeKind::prev &&
                ring->wedges[hud::build_ring_next_slot].kind == hud::BuildWedgeKind::next,
            Part::dispatch,
            "the build ring has no PREV at W and NEXT at E"
        );
        require(
            has_area(ring->wedges[0].hit), Part::hud, "the build ring's wedges are not laid out"
        );
        const auto slot = build_slot(runtime, solar_button, "ARMSOLAR");
        aim_build(runtime, run, slot);
        snapshot(runtime, run, "pad-build-ring.ppm");
        lift(runtime, run, run.deck, pad::PadButton::l1);
        set_thumb(run.deck, pad::Side::right, false, {kPadMiddle, kPadMiddle});
        step(runtime, run, kFrameMs);
        require(
            runtime.pending_build_type_ == solar && runtime.match_command_ == MatchCommand::build,
            Part::dispatch,
            "releasing L1 on the Solar Collector did not arm it"
        );
        require(!ring.has_value(), Part::dispatch, "the build ring stayed open after a building");
        require(
            touch(runtime).dispatch.placement_by_pad,
            Part::dispatch,
            "the ring's building is not marked as the pad's placement (placement_by_pad)"
        );
        runtime.reset_match_command();
        runtime.pending_build_type_ = 0;
        // A factory.
        const auto kbot = type_of(runtime, "ARMPW");
        select_only(runtime, run.lab);
        look_at_units(runtime, {run.lab});
        step(runtime, run, kFrameMs);
        empty_factory(runtime, run.lab, kbot);
        const auto kbot_button = build_button(runtime, "ARMPW");
        point_at(
            runtime,
            run,
            ground_beside(runtime, run, run.lab, 0.0F, kGroundOffsetPoints),
            "open ground"
        );
        push(runtime, run, run.deck, pad::PadButton::l1);
        require(
            ring.has_value(), Part::dispatch, "L1 with the lab selected did not open the build ring"
        );
        aim_build(runtime, run, build_slot(runtime, kbot_button, "ARMPW"));
        click(runtime, run, run.deck, pad::PadButton::r2);
        const auto added = runtime.match_->queued_build_count(run.lab, kbot);
        click(runtime, run, run.deck, pad::PadButton::l2);
        const auto taken = runtime.match_->queued_build_count(run.lab, kbot);
        set_thumb(run.deck, pad::Side::right, false, {kPadMiddle, kPadMiddle});
        lift(runtime, run, run.deck, pad::PadButton::l1);
        require(added == 1, Part::dispatch, "R2 on the ring's ARMPW did not queue one");
        require(taken == 0, Part::dispatch, "L2 on the ring's ARMPW did not take one off");
        require(!ring.has_value(), Part::dispatch, "the factory's build ring stayed open after L1");
        empty_factory(runtime, run.lab, kbot);
    }

    /// K11. The camera: the left stick pans, the right stick zooms in, the left trackpad drags
    /// the map with the thumb, and a held press of it puts the camera on the minimap's point.
    static void case_camera(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        look_at_middle(runtime);
        step(runtime, run, kFrameMs);
        const auto start_x = runtime.match_camera_x_;
        set_stick(run.deck, pad::Side::left, kStickPush, 0.0F);
        steps(runtime, run, kCameraMs, kFrameMs);
        set_stick(run.deck, pad::Side::left, 0.0F, 0.0F);
        step(runtime, run, kFrameMs);
        require(
            runtime.match_camera_x_ > start_x,
            Part::dispatch,
            "the left stick pushed right did not pan the camera right"
        );
        const auto zoom = runtime.match_zoom_target_;
        set_stick(run.deck, pad::Side::right, 0.0F, -kStickPush);
        steps(runtime, run, kCameraMs, kFrameMs);
        set_stick(run.deck, pad::Side::right, 0.0F, 0.0F);
        step(runtime, run, kFrameMs);
        require(
            runtime.match_zoom_target_ > zoom,
            Part::dispatch,
            "the right stick pushed up did not zoom in"
        );
        runtime.match_zoom_ = run.start_zoom_target;
        runtime.match_zoom_target_ = run.start_zoom_target;
        look_at_middle(runtime);
        step(runtime, run, kFrameMs);
        // The left trackpad drags the map: the ground follows the thumb right, the view left.
        const auto before_drag = runtime.match_camera_x_;
        CanvasPoint thumb{kPadMiddle - kDragTravelPads / 2.0F, kPadMiddle};
        set_thumb(run.deck, pad::Side::left, true, thumb);
        for (int frame = 0; frame < kLandingFrames; ++frame)
            step(runtime, run, kFrameMs);
        for (int sample = 0; sample < kDragSamples; ++sample) {
            thumb.x += kDragTravelPads / static_cast<float>(kDragSamples);
            set_thumb(run.deck, pad::Side::left, true, thumb);
            step(runtime, run, kFrameMs);
        }
        set_thumb(run.deck, pad::Side::left, false, thumb);
        step(runtime, run, kFrameMs);
        require(
            runtime.match_camera_x_ < before_drag,
            Part::dispatch,
            "a slide right on the left trackpad did not drag the map with the thumb"
        );
        look_at_middle(runtime);
        step(runtime, run, kFrameMs);
        const auto middle_x = runtime.match_camera_x_;
        const auto middle_z = runtime.match_camera_z_;
        const CanvasPoint corner{kMinimapPadPoint, kMinimapPadPoint};
        set_thumb(run.deck, pad::Side::left, true, corner);
        step(runtime, run, kFrameMs);
        push(runtime, run, run.deck, pad::PadButton::left_pad);
        steps(runtime, run, kHoldMs, kHoldStepMs);
        const auto held_x = runtime.match_camera_x_;
        const auto held_z = runtime.match_camera_z_;
        lift(runtime, run, run.deck, pad::PadButton::left_pad);
        set_thumb(run.deck, pad::Side::left, false, corner);
        step(runtime, run, kFrameMs);
        require(
            held_x > middle_x && held_z > middle_z,
            Part::dispatch,
            "a held press of the left trackpad's lower right did not put the camera on the "
            "minimap's lower right"
        );
    }

    /// K12. The face buttons and the D-pad: X stops, Y selects the type under the pointer, D-pad
    /// up the commander, right the next unit, L3 centres, R3 follows; D-pad left opens SELECT ▾,
    /// whose items the D-pad marks and A picks.
    static void case_buttons(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        const auto [a, b, c] = run.peewees;
        select_only(runtime, a);
        look_at_units(runtime, {a, b, c});
        step(runtime, run, kFrameMs);
        mouse_click(
            runtime, run, ground_beside(runtime, run, a, 0.0F, -kGroundOffsetPoints), SDL_KMOD_NONE
        );
        require(
            !queue_of(runtime, a).empty(), Part::check, "a mouse click gave the Peewee no move"
        );
        const auto moving = queue_of(runtime, a);
        tap(runtime, run, run.deck, pad::PadButton::x);
        const auto stopped = queue_of(runtime, a);
        require(
            stopped.empty() || !same_orders(stopped, moving),
            Part::dispatch,
            "X did not stop the Peewee (S): its queue stayed " + queue_text(stopped)
        );
        select_none(runtime);
        point_at(runtime, run, canvas_of(runtime, a), "a Peewee");
        tap(runtime, run, run.deck, pad::PadButton::y);
        require(
            selected(runtime, a) && selected(runtime, b) && selected(runtime, c),
            Part::dispatch,
            "Y on a Peewee did not select every Peewee in view"
        );
        select_none(runtime);
        tap(runtime, run, run.deck, pad::PadButton::dpad_up);
        require(
            selected(runtime, run.commander) && runtime.tracked_match_unit_ == run.commander,
            Part::dispatch,
            "D-pad up did not select and follow the commander (Ctrl+C)"
        );
        runtime.stop_match_tracking();
        // D-pad right is N: the view goes to the next unit not yet visited.
        select_none(runtime);
        runtime.match_->state().game.cycle_unit_id = 0;
        tap(runtime, run, run.deck, pad::PadButton::dpad_right);
        require(
            runtime.match_->state().game.cycle_unit_id != 0,
            Part::dispatch,
            "D-pad right did not go to the next unit (N)"
        );
        // L3 is Home: the view centres on the selection, from the map's far corner.
        select_only(runtime, a);
        const auto look_away = [&] {
            const auto [unit_x, unit_z] = map_of(runtime, a);
            const auto map_width =
                static_cast<int32_t>(runtime.selected_tnt_->tile_width * kMapPixelsPerTile);
            const auto map_height =
                static_cast<int32_t>(runtime.selected_tnt_->tile_height * kMapPixelsPerTile);
            look_at(
                runtime,
                unit_x < map_width / 2 ? map_width - kEdgeMargin : kEdgeMargin,
                unit_z < map_height / 2 ? map_height - kEdgeMargin : kEdgeMargin
            );
            step(runtime, run, kFrameMs);
        };
        look_away();
        tap(runtime, run, run.deck, pad::PadButton::l3);
        const std::array<int32_t, 2> centred_by_pad{
            runtime.match_camera_x_, runtime.match_camera_z_
        };
        look_away();
        const std::array<int32_t, 2> away{runtime.match_camera_x_, runtime.match_camera_z_};
        send_key(runtime, run, SDLK_HOME, SDL_SCANCODE_HOME, true);
        send_key(runtime, run, SDLK_HOME, SDL_SCANCODE_HOME, false);
        const std::array<int32_t, 2> centred_by_key{
            runtime.match_camera_x_, runtime.match_camera_z_
        };
        require(
            centred_by_key != away,
            Part::check,
            "Home did not move the camera to the Peewee (camera " + std::to_string(away[0]) + "," +
                std::to_string(away[1]) + ", selected unit " +
                std::to_string(runtime.selected_match_unit_) + ", followed unit " +
                std::to_string(runtime.tracked_match_unit_) + ", pad's camera " +
                std::to_string(centred_by_pad[0]) + "," + std::to_string(centred_by_pad[1]) + ")"
        );
        require(
            centred_by_pad == centred_by_key,
            Part::dispatch,
            "L3 did not centre the camera as Home does"
        );
        tap(runtime, run, run.deck, pad::PadButton::r3);
        require(
            runtime.tracked_match_unit_ != 0, Part::dispatch, "R3 did not follow the selection (T)"
        );
        runtime.stop_match_tracking();
        // SELECT ▾.
        select_none(runtime);
        look_at_units(runtime, {run.commander});
        step(runtime, run, kFrameMs);
        tap(runtime, run, run.deck, pad::PadButton::dpad_left);
        auto& state = touch(runtime).hud;
        require(
            state.sheet == hud::Sheet::select_menu, Part::dispatch, "D-pad left did not open SELECT"
        );
        snapshot(runtime, run, "pad-select.ppm");
        if (state.sheet_focus < 0)
            tap(runtime, run, run.deck, pad::PadButton::dpad_down);
        for (int guard = 0; guard < kMostFocusSteps && state.sheet_focus > 0; ++guard)
            tap(runtime, run, run.deck, pad::PadButton::dpad_up);
        require(
            state.sheet_focus == static_cast<int8_t>(hud::SelectItem::all),
            Part::dispatch,
            "the D-pad does not mark SELECT's first item (HudState::sheet_focus)"
        );
        tap(runtime, run, run.deck, pad::PadButton::dpad_down);
        require(
            state.sheet_focus == static_cast<int8_t>(hud::SelectItem::builders),
            Part::dispatch,
            "D-pad down did not mark SELECT's next item"
        );
        tap(runtime, run, run.deck, pad::PadButton::dpad_up);
        tap(runtime, run, run.deck, pad::PadButton::a);
        require(
            selected(runtime, run.commander) && selected(runtime, a) && selected(runtime, c),
            Part::dispatch,
            "A on SELECT's All did not select every unit (Ctrl+A)"
        );
        require(state.sheet == hud::Sheet::none, Part::dispatch, "SELECT stayed open after A");
    }

    /// Returns the in-game menu's panel and focus as one value to compare.
    ///
    /// @param runtime the runtime
    /// @return the panel's file, the focus and whether the menu holds the match
    static std::tuple<std::string, int32_t, bool> menu_state(Runtime& runtime) {
        return {runtime.match_hud_panel_, runtime.match_hud_focus_, runtime.match_paused_};
    }

    /// K13. View: a tap shows the info of the unit under the pointer, held it is the game
    /// layer (View and X pause) and its release then is no tap. Menu opens the in-game menu,
    /// where the D-pad moves the focus as the arrow keys do, A presses as Space does and B goes
    /// back as Escape does.
    static void case_view_and_menu(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        const auto a = run.peewees[0];
        look_at_units(runtime, {a});
        step(runtime, run, kFrameMs);
        point_at(runtime, run, canvas_of(runtime, a), "a Peewee");
        tap(runtime, run, run.deck, pad::PadButton::view);
        require(
            runtime.unit_info_panel_.has_value(),
            Part::dispatch,
            "a tap of View on a Peewee did not show its unit info (F1)"
        );
        runtime.close_unit_info();
        step(runtime, run, kFrameMs);
        push(runtime, run, run.deck, pad::PadButton::view);
        tap(runtime, run, run.deck, pad::PadButton::x);
        const bool paused_by_pad = paused(runtime);
        lift(runtime, run, run.deck, pad::PadButton::view);
        require(paused_by_pad, Part::dispatch, "View held with X did not pause (game layer)");
        require(
            !runtime.unit_info_panel_.has_value(),
            Part::dispatch,
            "View's release after the game layer gave its tap too"
        );
        auto& game = runtime.match_->state().game;
        game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags & ~console::kSimRunPaused);
        // Menu and its focus.
        tap(runtime, run, run.deck, pad::PadButton::menu);
        require(runtime.match_paused_, Part::dispatch, "Menu did not open the in-game menu (F2)");
        const auto opened = menu_state(runtime);
        tap(runtime, run, run.deck, pad::PadButton::dpad_down);
        const auto by_pad = menu_state(runtime);
        runtime.resume_match_pause();
        step(runtime, run, kFrameMs);
        send_key(runtime, run, SDLK_F2, SDL_SCANCODE_F2, true);
        send_key(runtime, run, SDLK_F2, SDL_SCANCODE_F2, false);
        require(menu_state(runtime) == opened, Part::check, "F2 opened the menu in another state");
        send_key(runtime, run, SDLK_DOWN, SDL_SCANCODE_DOWN, true);
        send_key(runtime, run, SDLK_DOWN, SDL_SCANCODE_DOWN, false);
        const auto by_key = menu_state(runtime);
        require(
            by_pad == by_key,
            Part::dispatch,
            "D-pad down in the in-game menu did not do what the Down key does"
        );
        if (by_key == opened)
            run.notes.push_back("K13: the Down key leaves the in-game menu's focus where it is");
        // A as Space.
        runtime.resume_match_pause();
        step(runtime, run, kFrameMs);
        tap(runtime, run, run.deck, pad::PadButton::menu);
        tap(runtime, run, run.deck, pad::PadButton::a);
        const auto pressed_by_pad = menu_state(runtime);
        if (runtime.match_paused_)
            runtime.resume_match_pause();
        step(runtime, run, kFrameMs);
        send_key(runtime, run, SDLK_F2, SDL_SCANCODE_F2, true);
        send_key(runtime, run, SDLK_F2, SDL_SCANCODE_F2, false);
        send_key(runtime, run, SDLK_SPACE, SDL_SCANCODE_SPACE, true);
        send_key(runtime, run, SDLK_SPACE, SDL_SCANCODE_SPACE, false);
        const auto pressed_by_key = menu_state(runtime);
        require(
            pressed_by_pad == pressed_by_key,
            Part::dispatch,
            "A in the in-game menu did not press its focused button as Space does"
        );
        if (runtime.match_paused_)
            runtime.resume_match_pause();
        step(runtime, run, kFrameMs);
        // B as Escape.
        tap(runtime, run, run.deck, pad::PadButton::menu);
        require(runtime.match_paused_, Part::dispatch, "Menu did not open the in-game menu again");
        tap(runtime, run, run.deck, pad::PadButton::b);
        require(
            !runtime.match_paused_, Part::dispatch, "B did not close the in-game menu (Escape)"
        );
    }

    /// Returns how many feels a stand-in has been sent on its right side, by rumble (the high
    /// motor alone) or by the trackpad haptic report.
    ///
    /// @param stand_in the stand-in
    /// @return the count
    static std::size_t right_feels(const StandIn& stand_in) {
        std::size_t count = 0;
        for (const auto& rumble : stand_in.record->rumbles)
            count += rumble[1] > 0 && rumble[0] == 0 ? 1 : 0;
        for (const auto& effect : stand_in.record->effects)
            count += effect.size() > kHapticSideByte &&
                             effect[kHapticTypeByte] == kHapticReportType &&
                             (effect[kHapticSideByte] == kHapticSideRight ||
                              effect[kHapticSideByte] == kHapticSideBoth)
                         ? 1
                         : 0;
        return count;
    }

    /// Returns how many feels of any kind a stand-in has been sent.
    ///
    /// @param stand_in the stand-in
    /// @return the count
    static std::size_t all_feels(const StandIn& stand_in) {
        return stand_in.record->rumbles.size() + stand_in.record->effects.size();
    }

    /// K15. Haptics: R2 on a refused building site plays a feel on the right side (a trackpad
    /// pulse, or a rumble where the driver refuses it); with Haptics Off nothing plays.
    static void case_haptics(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        const auto solar = type_of(runtime, "ARMSOLAR");
        select_only(runtime, run.commander);
        look_at_units(runtime, {run.commander, run.lab});
        step(runtime, run, kFrameMs);
        runtime.activate_match_hud(build_button(runtime, "ARMSOLAR"), true);
        require(
            runtime.pending_build_type_ == solar, Part::check, "the Solar Collector did not arm"
        );
        const auto refused = refused_site_point(runtime, run);
        point_at(runtime, run, refused, "a refused building site");
        step(runtime, run, kAfterTapMs);
        const auto before = right_feels(run.deck);
        const auto any_before = all_feels(run.deck);
        click(runtime, run, run.deck, pad::PadButton::r2);
        require(
            runtime.pending_build_type_ == solar,
            Part::dispatch,
            "R2 on a refused site ended the placement"
        );
        require(
            right_feels(run.deck) > before,
            Part::dispatch,
            "R2 on a refused site played no feel on the right side (" +
                std::to_string(all_feels(run.deck) - any_before) + " others)"
        );
        set_pad_settings(runtime, [](settings::EngineSettings& wanted) {
            wanted.pad_haptics = pad::Haptics::off;
        });
        step(runtime, run, kFrameMs);
        point_at(runtime, run, refused, "a refused building site");
        step(runtime, run, kAfterTapMs);
        const auto off_before = all_feels(run.deck);
        click(runtime, run, run.deck, pad::PadButton::r2);
        require(
            all_feels(run.deck) == off_before,
            Part::dispatch,
            "with Haptics Off a refused site still played a feel"
        );
        runtime.reset_match_command();
        runtime.pending_build_type_ = 0;
    }

    /// K16. The slim pad HUD after the pad's input: the status pill, the QUEUE, ADD and FORCE
    /// chips inside the battlefield, a stored group's chip, the group buttons while L5 is held.
    static void case_pad_hud(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        select_only(runtime, run.peewees[0]);
        runtime.assign_squad(1);
        select_none(runtime);
        steps(runtime, run, 2 * kFrameMs, kFrameMs);
        const auto& state = touch(runtime);
        require(
            state.hud.pad.hud,
            Part::dispatch,
            "HudState::pad.hud is off after pad input with touch controls off"
        );
        require(
            state.hud.pad.badges, Part::dispatch, "HudState::pad.badges is off after pad input"
        );
        require(
            state.hud.pad.force_shown,
            Part::dispatch,
            "HudState::pad.force_shown is off for a pad with grips"
        );
        require(state.frame_ready, Part::hud, "the touch layer laid out no frame for the pad HUD");
        const auto field = battlefield(runtime);
        require(
            has_area(state.frame.status) && rect_inside(state.frame.status, field),
            Part::hud,
            "the slim pad HUD lays out no status pill inside the battlefield"
        );
        const std::array<std::pair<hud::Control, const char*>, 3> chips{{
            {hud::Control::queue, "QUEUE"},
            {hud::Control::add, "ADD"},
            {hud::Control::force, "FORCE"},
        }};
        for (const auto& [control, name] : chips) {
            const auto* found = find_control(runtime, control, -1);
            require(
                found != nullptr,
                Part::hud,
                std::string("the slim pad HUD has no ") + name + " chip"
            );
            require(
                rect_inside(found->rect, field),
                Part::hud,
                std::string("the slim pad HUD's ") + name + " chip lies off the battlefield"
            );
        }
        require(
            find_control(runtime, hud::Control::group_chip, 1) != nullptr,
            Part::hud,
            "the slim pad HUD shows no chip for a stored group"
        );
        snapshot(runtime, run, "pad-hud.ppm");
        push(runtime, run, run.deck, pad::PadButton::l5);
        const bool layer = state.hud.pad.groups_layer;
        lift(runtime, run, run.deck, pad::PadButton::l5);
        require(layer, Part::dispatch, "the group chips show no buttons while L5 is held");
    }

    /// K16 with touch controls on: after the pad's input the touch controls stay, each with its
    /// gamepad badge, and the slim pad HUD stays away; FORCE tops a tablet's thumb column, a
    /// stored group has its chip, and the group buttons show while L5 is held.
    static void case_pad_badges(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        select_only(runtime, run.peewees[0]);
        runtime.assign_squad(1);
        select_none(runtime);
        steps(runtime, run, 2 * kFrameMs, kFrameMs);
        const auto& state = touch(runtime);
        require(
            !state.hud.pad.hud,
            Part::dispatch,
            "HudState::pad.hud is on after pad input with touch controls on"
        );
        require(
            state.hud.pad.badges, Part::dispatch, "HudState::pad.badges is off after pad input"
        );
        require(
            state.hud.pad.force_shown,
            Part::dispatch,
            "HudState::pad.force_shown is off for a pad with grips"
        );
        require(state.frame_ready, Part::hud, "the touch layer laid out no frame");
        for (const auto& [control, name] : std::array<std::pair<hud::Control, const char*>, 2>{
                 {{hud::Control::queue, "QUEUE"}, {hud::Control::add, "ADD"}}
             })
            require(
                find_control(runtime, control, -1) != nullptr,
                Part::hud,
                std::string("the touch controls lost their ") + name + " after pad input"
            );
        if (run.side_panel) {
            require(
                !has_area(state.frame.status),
                Part::hud,
                "the slim pad HUD's status pill shows over a tablet's touch controls"
            );
            require(
                find_control(runtime, hud::Control::force, -1) != nullptr,
                Part::hud,
                "the tablet's thumb column has no FORCE for a pad with grips"
            );
        }
        require(
            find_control(runtime, hud::Control::group_chip, 1) != nullptr,
            Part::hud,
            "the touch controls show no chip for a stored group"
        );
        snapshot(runtime, run, "pad-badges.ppm");
        push(runtime, run, run.deck, pad::PadButton::l5);
        const bool layer = state.hud.pad.groups_layer;
        lift(runtime, run, run.deck, pad::PadButton::l5);
        require(layer, Part::dispatch, "the group chips show no buttons while L5 is held");
    }

    /// K17. The same as the mouse: a click and two clicks with R4 held give the moves a click
    /// and two Shift-clicks give, from the same state.
    static void case_same_as_mouse(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        const auto unit = run.peewees[0];
        hold_units_still(runtime);
        select_only(runtime, unit);
        look_at_units(runtime, {unit});
        step(runtime, run, kFrameMs);
        const std::array<CanvasPoint, 3> points{
            ground_beside(runtime, run, unit, 0.0F, -kGroundOffsetPoints),
            ground_beside(runtime, run, unit, -kGroundOffsetPoints, -kGroundOffsetPoints),
            ground_beside(runtime, run, unit, -kGroundOffsetPoints, kGroundOffsetPoints)
        };
        std::array<CanvasPoint, 3> clicked{};
        point_at(runtime, run, points[0], "a ground point");
        clicked[0] = pointer(runtime);
        click(runtime, run, run.deck, pad::PadButton::r2);
        push(runtime, run, run.deck, pad::PadButton::r4);
        point_at(runtime, run, points[1], "a second ground point");
        clicked[1] = pointer(runtime);
        click(runtime, run, run.deck, pad::PadButton::r2);
        point_at(runtime, run, points[2], "a third ground point");
        clicked[2] = pointer(runtime);
        click(runtime, run, run.deck, pad::PadButton::r2);
        lift(runtime, run, run.deck, pad::PadButton::r4);
        const auto by_pad = queue_of(runtime, unit);
        const auto pad_selection = selection_of(runtime);
        runtime.match_->stop_orders(unit);
        select_only(runtime, unit);
        step(runtime, run, kFrameMs);
        mouse_click(runtime, run, clicked[0], SDL_KMOD_NONE);
        mouse_click(runtime, run, clicked[1], SDL_KMOD_LSHIFT);
        mouse_click(runtime, run, clicked[2], SDL_KMOD_LSHIFT);
        const auto by_mouse = queue_of(runtime, unit);
        require(
            by_mouse.size() == points.size(),
            Part::check,
            "the mouse's clicks did not queue three moves"
        );
        require(
            same_orders(by_pad, by_mouse),
            Part::dispatch,
            "R2 and R4 gave " + queue_text(by_pad) + " where the same mouse clicks give " +
                queue_text(by_mouse)
        );
        require(
            pad_selection == selection_of(runtime),
            Part::dispatch,
            "the pad's clicks left another selection than the same mouse clicks"
        );
    }

    /// K18. Placement by the pad: R2 on a build button (on the phone layout, L1 let go on the build
    /// ring's wedge) arms the building as the pad's placement, whose ghost follows the pointer; R2
    /// on a legal site builds there; B cancels.
    static void case_placement(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        const auto solar = type_of(runtime, "ARMSOLAR");
        select_only(runtime, run.commander);
        look_at_units(runtime, {run.commander, run.lab});
        step(runtime, run, kFrameMs);
        const auto button = build_button(runtime, "ARMSOLAR");
        const auto arm = [&] {
            if (run.side_panel) {
                point_at(runtime, run, gadget_centre(runtime, button), "the ARMSOLAR build button");
                click(runtime, run, run.deck, pad::PadButton::r2);
            } else {
                aim_ring_from(runtime, run, run.commander, button, "ARMSOLAR");
                let_ring_go(runtime, run);
            }
            require(
                runtime.pending_build_type_ == solar &&
                    runtime.match_command_ == MatchCommand::build,
                Part::dispatch,
                run.side_panel ? "R2 on the ARMSOLAR build button did not arm the building"
                               : "releasing L1 on the build ring's ARMSOLAR did not arm it"
            );
            require(
                touch(runtime).dispatch.placement_by_pad,
                Part::dispatch,
                "the building the pad armed is not the pad's placement (placement_by_pad)"
            );
            require(
                !touch(runtime).dispatch.placement_touch,
                Part::hud,
                "the touch layer anchored the pad's placement (it follows the pointer)"
            );
        };
        arm();
        const auto site = legal_site_point(runtime, run, solar);
        point_at(runtime, run, site, "a legal building site");
        const auto under = runtime.build_site_under(pointer(runtime).x, pointer(runtime).y);
        require(
            under.has_value() && under->legal,
            Part::check,
            "the ghost at the pointer is not on the legal site"
        );
        click(runtime, run, run.deck, pad::PadButton::r2);
        require(
            builds_of(runtime, run.commander, solar) == 1 && runtime.pending_build_type_ == 0,
            Part::dispatch,
            "R2 on a legal site did not build the Solar Collector there and end the placement"
        );
        runtime.match_->stop_orders(run.commander);
        arm();
        tap(runtime, run, run.deck, pad::PadButton::b);
        require(
            runtime.pending_build_type_ == 0 && runtime.match_command_ == MatchCommand::none &&
                !touch(runtime).dispatch.placement_by_pad,
            Part::dispatch,
            "B did not cancel the pad's placement"
        );
    }

    /// X1. The Xbox pad's fallback: the sticks scheme with the stick cursor, a tap of R1 or L1
    /// toggles QUEUE or ADD, R1 held opens the order ring the right stick aims, View held is
    /// the groups layer, and the Xbox glyphs.
    static void case_xbox_fallback(Runtime& runtime, PadRun& run) {
        attach_xbox(runtime, run);
        const OpenPad* open = open_pad(runtime, run.xbox.id);
        require(
            open != nullptr && open->gamepad != nullptr,
            Part::dispatch,
            "the dispatcher did not open the Xbox stand-in"
        );
        require(
            open->traits.type == pad::PadType::xbox && open->traits.trackpads == 0 &&
                !open->traits.grips && !open->traits.gyro,
            Part::dispatch,
            "the Xbox stand-in's traits are not an Xbox pad's without trackpads, grips or gyro"
        );
        tap(runtime, run, run.xbox, pad::PadButton::x);
        step(runtime, run, kFrameMs);
        require(
            runtime.pad_state().active == run.xbox.id,
            Part::dispatch,
            "the Xbox stand-in is not the active pad after its input"
        );
        const auto& look = touch(runtime).hud.pad;
        require(
            look.glyphs == pad::GlyphStyle::xbox,
            Part::dispatch,
            "the Xbox pad's prompts are not the Xbox set"
        );
        require(
            look.map.fallback && look.map.scheme == pad::Scheme::sticks && !look.map.trackpads,
            Part::dispatch,
            "the Xbox pad's map is not the fallback with the sticks scheme"
        );
        require(!look.force_shown, Part::dispatch, "the FORCE chip shows for a pad without grips");
        // The stick cursor.
        point_at_with_stick_start(runtime, run);
        const auto start = pointer(runtime);
        set_stick(run.xbox, pad::Side::right, kStickPush, 0.0F);
        steps(runtime, run, kCameraMs, kFrameMs);
        set_stick(run.xbox, pad::Side::right, 0.0F, 0.0F);
        step(runtime, run, kFrameMs);
        require(
            pointer(runtime).x > start.x,
            Part::dispatch,
            "the right stick pushed right did not move the stick cursor right"
        );
        // The latches.
        auto& latches = touch(runtime).hud.latches;
        latches.clear();
        tap(runtime, run, run.xbox, pad::PadButton::r1);
        step(runtime, run, kAfterTapMs);
        require(
            latches.latched(hud::Latch::queue),
            Part::dispatch,
            "a tap of R1 did not toggle QUEUE on"
        );
        require(
            !touch(runtime).hud.radial.has_value(),
            Part::dispatch,
            "a tap of R1 opened the order ring"
        );
        tap(runtime, run, run.xbox, pad::PadButton::r1);
        step(runtime, run, kAfterTapMs);
        require(
            !latches.latched(hud::Latch::queue), Part::dispatch, "a second tap of R1 left QUEUE on"
        );
        tap(runtime, run, run.xbox, pad::PadButton::l1);
        step(runtime, run, kAfterTapMs);
        require(
            latches.latched(hud::Latch::add), Part::dispatch, "a tap of L1 did not toggle ADD on"
        );
        tap(runtime, run, run.xbox, pad::PadButton::l1);
        step(runtime, run, kAfterTapMs);
        latches.clear();
        // R1 held: the order ring, aimed with the right stick.
        const auto unit = run.peewees[0];
        hold_units_still(runtime);
        select_only(runtime, unit);
        step(runtime, run, kFrameMs);
        hold(runtime, run, run.xbox, pad::PadButton::r1);
        const auto& radial = touch(runtime).hud.radial;
        require(
            radial.has_value(),
            Part::dispatch,
            "R1 held on the Xbox pad did not open the order ring"
        );
        const auto patrol = radial_slot(runtime, hud::RadialItem::patrol);
        const auto aim = pad::wedge_point(
            {}, kStickPush, kStickPush, patrol, static_cast<uint8_t>(hud::radial_slot_count)
        );
        set_stick(run.xbox, pad::Side::right, aim.x, aim.y);
        step(runtime, run, kFrameMs);
        step(runtime, run, kFrameMs);
        const bool aimed = touch(runtime).hud.pad.radial_aim == patrol;
        lift(runtime, run, run.xbox, pad::PadButton::r1);
        set_stick(run.xbox, pad::Side::right, 0.0F, 0.0F);
        step(runtime, run, kFrameMs);
        require(aimed, Part::dispatch, "the right stick did not aim the order ring at Patrol");
        require(
            patrols_of(runtime, unit).size() == 1 && !radial.has_value(),
            Part::dispatch,
            "releasing R1 on Patrol did not give it from the Xbox pad"
        );
        // View held: the groups layer.
        select_only(runtime, run.peewees[1]);
        runtime.assign_squad(1);
        select_none(runtime);
        step(runtime, run, kFrameMs);
        push(runtime, run, run.xbox, pad::PadButton::view);
        tap(runtime, run, run.xbox, pad::PadButton::dpad_up);
        lift(runtime, run, run.xbox, pad::PadButton::view);
        require(
            selected(runtime, run.peewees[1]) && !selected(runtime, run.peewees[0]),
            Part::dispatch,
            "View held with a tap of D-pad up did not select group 1 on the Xbox pad"
        );
    }

    /// Puts the engine's pointer in the battlefield's middle for the stick cursor, with a real
    /// mouse's motion, as a start the stick moves from.
    ///
    /// @param runtime the runtime
    /// @param run the check's state
    static void point_at_with_stick_start(Runtime& runtime, PadRun& run) {
        mouse_move(runtime, run, centre_of(runtime.overlay_area()));
    }

    /// X2. F13 to F16 with a pad open act as R4, R5, L4 and L5, and end the fallback map.
    static void case_grip_keys(Runtime& runtime, PadRun& run) {
        require_attached(run.xbox);
        tap(runtime, run, run.xbox, pad::PadButton::x);
        auto& latches = touch(runtime).hud.latches;
        for (const bool down : {true, false}) {
            SDL_Event event = key_event(run, SDLK_F13, SDL_SCANCODE_F13, down);
            require(
                runtime.take_pad_event(event, run.running),
                Part::dispatch,
                "F13 with a gamepad open was not taken as R4"
            );
        }
        step(runtime, run, kAfterTapMs);
        latches.clear();
        send_key(runtime, run, SDLK_F13, SDL_SCANCODE_F13, true);
        const bool queue = latches.active(hud::Latch::queue);
        steps(runtime, run, kHoldMs, kHoldStepMs);
        send_key(runtime, run, SDLK_F13, SDL_SCANCODE_F13, false);
        require(queue, Part::dispatch, "F13 held did not hold QUEUE (R4)");
        require(
            !latches.active(hud::Latch::queue), Part::dispatch, "QUEUE stayed on after F13 came up"
        );
        send_key(runtime, run, SDLK_F14, SDL_SCANCODE_F14, true);
        const bool force = runtime.pad_force_held();
        send_key(runtime, run, SDLK_F14, SDL_SCANCODE_F14, false);
        require(force, Part::dispatch, "F14 held did not hold FORCE (R5)");
        require(!runtime.pad_force_held(), Part::dispatch, "FORCE stayed on after F14 came up");
        send_key(runtime, run, SDLK_F15, SDL_SCANCODE_F15, true);
        const bool add = latches.active(hud::Latch::add);
        steps(runtime, run, kHoldMs, kHoldStepMs);
        send_key(runtime, run, SDLK_F15, SDL_SCANCODE_F15, false);
        require(add, Part::dispatch, "F15 held did not hold ADD (L4)");
        send_key(runtime, run, SDLK_F16, SDL_SCANCODE_F16, true);
        const bool groups = touch(runtime).hud.pad.groups_layer;
        send_key(runtime, run, SDLK_F16, SDL_SCANCODE_F16, false);
        require(groups, Part::dispatch, "F16 held did not hold the groups layer (L5)");
        require(
            runtime.pad_state().grip_keys_seen, Part::dispatch, "F13 to F16 left grip_keys_seen off"
        );
        require(
            !touch(runtime).hud.pad.map.fallback,
            Part::dispatch,
            "F13 to F16 did not end the fallback map"
        );
    }

    /// K19. A layout that moves Stop to J leaves the pad's X stopping, and the raw keys follow
    /// the layout: S no longer stops the Peewee and J does.
    static void case_stop_follows_action(Runtime& runtime, PadRun& run) {
        require_attached(run.deck);
        fs::path folder = runtime.user_folder();
        if (folder.empty())
            folder = "K16-stop-on-j";
        else
            folder /= "K16-stop-on-j";
        std::error_code make_error;
        fs::create_directories(folder, make_error);
        require(!make_error, Part::check, "the Stop-on-J layout folder could not be made");
        const fs::path manifest = folder / "oakeys.yaml";
        {
            std::ofstream out(manifest, std::ios::binary);
            require(
                static_cast<bool>(out), Part::check, "the Stop-on-J layout could not be written"
            );
            out << "oakeys: 1\n"
                << "id: k16-stop-on-j\n"
                << "name: \"Stop on J\"\n"
                << "version: \"1.0\"\n"
                << "author: {name: \"Open Annihilation\"}\n"
                << "description: \"Stop is J.\"\n"
                << "tags: [orders]\n"
                << "base: oa-default\n"
                << "bindings:\n"
                << "  order.stop: [\"@KeyJ\"]\n";
            require(
                static_cast<bool>(out), Part::check, "the Stop-on-J layout could not be written"
            );
        }

        // A failed check still puts the default layout back before the next case.
        struct RestoreKeys {
            Runtime& runtime;
            std::string saved;

            ~RestoreKeys() {
                try {
                    runtime.options_.keys = saved;
                    runtime.key_bindings_refusal_.clear();
                    runtime.load_key_bindings();
                    runtime.ensure_key_bindings(false);
                } catch (...) {
                    // The next case reports a layout this one could not put back.
                }
            }
        } restore{runtime, runtime.options_.keys};

        runtime.key_bindings_refusal_.clear();
        runtime.options_.keys = path_to_utf8(folder);
        runtime.load_key_bindings();
        runtime.ensure_key_bindings(false);
        require(
            runtime.key_bindings_refusal_.empty(),
            Part::check,
            "the Stop-on-J layout was refused: " + runtime.key_bindings_refusal_
        );
        hold_units_still(runtime);
        const auto peewee = run.peewees[0];
        select_only(runtime, peewee);
        look_at_units(runtime, {peewee});
        step(runtime, run, kFrameMs);
        const auto give_move = [&] {
            mouse_click(
                runtime,
                run,
                ground_beside(runtime, run, peewee, 0.0F, -kGroundOffsetPoints),
                SDL_KMOD_NONE
            );
        };
        give_move();
        require(
            !queue_of(runtime, peewee).empty(), Part::check, "a mouse click gave the Peewee no move"
        );
        const auto moving = queue_of(runtime, peewee);
        tap(runtime, run, run.deck, pad::PadButton::x);
        const auto stopped = queue_of(runtime, peewee);
        require(
            stopped.empty() || !same_orders(stopped, moving),
            Part::dispatch,
            "X did not stop the Peewee after Stop moved to J: its queue stayed " +
                queue_text(stopped)
        );
        give_move();
        const auto again = queue_of(runtime, peewee);
        require(!again.empty(), Part::check, "a mouse click gave the Peewee no second move");
        send_key(runtime, run, SDLK_S, SDL_SCANCODE_S, true);
        send_key(runtime, run, SDLK_S, SDL_SCANCODE_S, false);
        const auto after_s = queue_of(runtime, peewee);
        require(
            same_orders(after_s, again),
            Part::dispatch,
            "S stopped the Peewee after Stop moved to J: its queue became " + queue_text(after_s)
        );
        send_key(runtime, run, SDLK_J, SDL_SCANCODE_J, true);
        send_key(runtime, run, SDLK_J, SDL_SCANCODE_J, false);
        const auto after_j = queue_of(runtime, peewee);
        require(
            after_j.empty() || !same_orders(after_j, again),
            Part::dispatch,
            "J did not stop the Peewee after Stop moved to J: its queue stayed " +
                queue_text(after_j)
        );
    }
};

void Runtime::check_pad_controls() {
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        throw std::runtime_error("pad controls check: needs the SDL renderer");
    PadRun run;
    run.clock_ns = SDL_GetTicksNS() + frame_pacing::kNanosecondsPerSecond;
    pad_state().check_clock_ns = run.clock_ns;
    // The check plays with its own virtual stand-ins alone: a gamepad the machine has, such as
    // the one a simulator adds, is let go and stays closed while it runs.
    pad_state().virtual_pads_only = true;
    for (const auto& open : pad_state().pads)
        if (const auto id = open.id; id != 0 && !SDL_IsJoystickVirtual(id))
            PadAccess::close_pad(*this, id);
    touch_state().dispatch.check_clock_ns = run.clock_ns;
    start_benchmark_skirmish();
    apply_output_mode();
    PadCheckAccess::prepare(*this, run);
    using Access = PadCheckAccess;
    const auto run_case = [&](std::string name, Access::CaseBody body) {
        Access::run_case(*this, run, std::move(name), body);
    };
    if (run.touch_controls)
        run.notes.push_back(
            run.side_panel ? "touch controls on: K16 checks the pad's badges on them"
                           : "touch controls on, the phone layout: K16 checks the pad's badges on "
                             "them, and K4, K6 and K18 press the build ring's wedges, as the "
                             "layout has no side panel"
        );
    run_case("X0 no pad", Access::case_no_pad);
    start_gamepad_subsystem();
    if ((SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) == 0)
        run.notes.push_back(
            "start_gamepad_subsystem did not start SDL's gamepad subsystem [dispatch]"
        );
    run_case("K0 detection and route", Access::case_detection);
    // From here the pad layer is on whatever K0 found, so each case stands on its own.
    pad_state().forced = true;
    if (run.touch_controls)
        run_case("K16 pad badges on the touch controls", Access::case_pad_badges);
    else
        run_case("K16 slim pad HUD", Access::case_pad_hud);
    run_case("K1 pointer, hover and edge", Access::case_pointer);
    run_case("K2 click and double press", Access::case_click);
    run_case("K3 box", Access::case_box);
    run_case("K4 L2 clear and factory -1", Access::case_right_button);
    run_case("K5 ADD grip and chip", Access::case_add);
    run_case("K6 QUEUE and x5", Access::case_queue);
    run_case("K7 FORCE", Access::case_force);
    run_case("K8 groups layer", Access::case_groups);
    run_case("K9 order ring", Access::case_order_ring);
    run_case("K10 build ring", Access::case_build_ring);
    run_case("K11 camera", Access::case_camera);
    run_case("K12 buttons, D-pad and SELECT", Access::case_buttons);
    run_case("K19 Stop follows the action", Access::case_stop_follows_action);
    run_case("K13 View, Menu and menu focus", Access::case_view_and_menu);
    run_case("K15 haptics", Access::case_haptics);
    run_case("K17 same as the mouse", Access::case_same_as_mouse);
    run_case("K18 placement by pad", Access::case_placement);
    run_case("X1 Xbox fallback", Access::case_xbox_fallback);
    run_case("X2 F13 to F16", Access::case_grip_keys);
    Access::detach(run.deck);
    Access::detach(run.xbox);
    for (const auto& note : run.notes)
        std::cout << "pad note: " << note << '\n';
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
    std::cout << "pad controls check: Steam Deck and Xbox stand-ins at " << match_layout_.width
              << 'x' << match_layout_.height << ", " << passed << " of " << run.results.size()
              << " cases passed" << (failed.empty() ? std::string() : ", failed: " + failed)
              << '\n';
    if (passed != run.results.size())
        throw std::runtime_error(
            "the pad controls check failed " + std::to_string(run.results.size() - passed) +
            " of " + std::to_string(run.results.size()) + " cases"
        );
}

} // namespace oa::app
