// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The touch controls' gesture recogniser: finger reports in, taps, holds,
// drags, pans, pinches, twists and two-finger taps out. Pure: no SDL and no
// clock of its own (docs/touch-controls.md).
#pragma once

#include <array>
#include <cstddef>
#include <stdint.h>

namespace oa::ui::touch_gestures {

/// Identifies one finger: the touch device and the finger on it.
struct FingerId {
    uint64_t device{}; ///< the touch device the finger is on
    uint64_t finger{}; ///< the finger on that device
    /// Returns whether two ids name the same finger of the same device.
    friend bool operator==(const FingerId&, const FingerId&) = default;
};

/// What happened to a finger.
enum class FingerPhase : uint8_t {
    down,   ///< the finger landed
    move,   ///< the finger moved while down
    up,     ///< the finger lifted
    cancel, ///< the system took the finger away
};

/// One finger report: canvas pixels, nanoseconds on any steady clock.
struct FingerSample {
    FingerId id{};
    float x{};          ///< canvas pixels
    float y{};          ///< canvas pixels
    uint64_t time_ns{}; ///< nanoseconds on the clock the caller uses throughout
};

/// How a one-finger drag that starts without a hold is read.
enum class OneFingerDrag : uint8_t {
    box,    ///< a selection box
    scroll, ///< the map follows the finger
};

/// Thresholds; distances in points, converted with px_per_point.
struct Thresholds {
    float px_per_point{1.0f};        ///< canvas pixels per point, > 0
    float slop_points{10.0f};        ///< travel that turns a press into a drag
    uint32_t double_tap_ms{300};     ///< second tap within this of the first's lift
    float double_tap_points{24.0f};  ///< and within this of the first tap
    uint32_t hold_ms{350};           ///< still this long: a hold (250..700 from the settings)
    uint32_t two_finger_tap_ms{250}; ///< both fingers up within this of the second landing
    float pinch_points{8.0f};        ///< spread change that starts a pinch
    /// Turn, in degrees, that starts a twist. 0 leaves twist off, so a turn
    /// of two fingers is only a pan and a pinch.
    float twist_degrees{0.0f};
    OneFingerDrag one_finger_drag{OneFingerDrag::box}; ///< a drag begun without a hold
};

/// What a gesture is.
enum class GestureKind : uint8_t {
    press,          ///< first finger landed at x,y
    rest_moved,     ///< the single finger moved within the slop (hover replacement) to x,y
    tap,            ///< lifted within the slop before a hold; taps = 1, or 2 for a double tap
    hold_started,   ///< still for hold_ms at x,y; the finger is still down
    hold_released,  ///< lifted after a hold without moving, at x,y (opens the radial)
    drag_began,     ///< one finger passed the slop: role box or scroll; start_x/y = press point;
                    ///< a drag_moved with the travel since the press follows in the same batch
    drag_moved,     ///< x,y now; dx,dy since the previous drag event
    drag_ended,     ///< lifted at x,y
    pan_began,      ///< two fingers' centroid passed the slop; x,y = centroid; a pan_moved with
                    ///< the travel since the second landing follows in the same batch
    pan_moved,      ///< dx,dy of the centroid since the previous pan event
    pan_ended,      ///< velocity_x/y of the centroid at lift, pixels per second
    pinch_began,    ///< the fingers' spread changed by pinch_points; x,y = centroid; a
                    ///< pinch_moved with the change since the second landing follows
    pinch_moved,    ///< scale = spread now / spread at the previous pinch event; x,y = centroid
    pinch_ended,    ///< one of the two fingers lifted while a pinch ran
    twist_began,    ///< the fingers' angle changed by twist_degrees; x,y = centroid; a
                    ///< twist_moved with the turn since the second landing follows
    twist_moved,    ///< turn = degrees since the previous twist event, positive one way
    twist_ended,    ///< one of the two fingers lifted while a twist ran
    two_finger_tap, ///< two fingers landed and lifted quickly without travelling, pinching or
                    ///< twisting
    cancelled,      ///< the sequence was dropped: cancel, a second finger during a drag, reset
};

/// What a one-finger drag does.
enum class DragRole : uint8_t {
    box,    ///< draws a selection box (or moves a ghost)
    scroll, ///< moves the map under the finger
};

/// One recognised gesture.
struct Gesture {
    GestureKind kind{GestureKind::press};
    float x{};          ///< canvas pixels: the finger, or the centroid of two
    float y{};          ///< canvas pixels
    float start_x{};    ///< drag_began: the press point
    float start_y{};    ///< drag_began: the press point
    float dx{};         ///< drag_moved, pan_moved: canvas pixels since the previous event
    float dy{};         ///< drag_moved, pan_moved: canvas pixels since the previous event
    float scale{1.0f};  ///< pinch_moved: spread now over spread at the previous pinch event
    float turn{};       ///< twist_moved: degrees since the previous twist event
    float velocity_x{}; ///< pan_ended: canvas pixels per second
    float velocity_y{}; ///< pan_ended: canvas pixels per second
    uint8_t taps{};     ///< tap: 1, or 2 for a double tap
    DragRole role{DragRole::box}; ///< drag_*: what the drag does
    bool from_hold{};             ///< drag_began after hold_started (always role box)
    uint64_t time_ns{};           ///< when it happened, on the samples' clock
};

/// The most gestures one input can produce (a pan, a pinch and a twist beginning together).
inline constexpr std::size_t max_gestures_per_step = 6;

/// Gestures produced by one input, in order.
struct GestureBatch {
    std::array<Gesture, max_gestures_per_step> items{}; ///< the first `count` are used
    uint8_t count{};                                    ///< gestures in items
    /// Appends a gesture; one past the capacity is dropped.
    ///
    /// @param gesture the gesture to append
    void push(const Gesture& gesture) noexcept;
};

/// Turns finger reports into gestures. No SDL, no clock of its own: time comes in the samples
/// and in advance(). At most two fingers are tracked; a third is ignored until one lifts.
class Recogniser {
  public:

    /// Replaces the thresholds; takes effect for the next finger that lands.
    ///
    /// @param thresholds the new thresholds
    void set_thresholds(const Thresholds& thresholds) noexcept;
    /// Returns the thresholds in use.
    ///
    /// @return the thresholds
    [[nodiscard]] const Thresholds& thresholds() const noexcept;
    /// Feeds one finger report and returns what it completed.
    ///
    /// @param phase what happened to the finger
    /// @param sample the finger, its canvas point and the time
    /// @return the gestures the report completed, in order
    GestureBatch finger(FingerPhase phase, const FingerSample& sample) noexcept;
    /// Advances time with no finger report: fires hold_started when due.
    ///
    /// @param now_ns the time now, on the samples' clock
    /// @return the gestures that came due, in order
    GestureBatch advance(uint64_t now_ns) noexcept;
    /// Drops every finger without producing gestures (screen change, flushed events).
    void reset() noexcept;
    /// Returns the number of fingers down.
    ///
    /// @return fingers tracked now, 0 to 2
    [[nodiscard]] uint8_t fingers_down() const noexcept;
    /// Returns whether a one-finger drag is running and its role.
    ///
    /// @param[out] role the running drag's role, when not null and a drag runs
    /// @return whether a one-finger drag runs
    [[nodiscard]] bool dragging(DragRole* role = nullptr) const noexcept;

  private:

    /// The most fingers tracked at once.
    static constexpr std::size_t max_fingers = 2;
    /// Centroid samples kept for a pan's lift velocity.
    static constexpr std::size_t pan_history = 8;

    /// One tracked finger.
    struct Finger {
        FingerId id{};      ///< the finger
        float x{};          ///< canvas pixels now
        float y{};          ///< canvas pixels now
        float start_x{};    ///< canvas pixels where it landed
        float start_y{};    ///< canvas pixels where it landed
        uint64_t down_ns{}; ///< when it landed
    };

    /// What the tracked fingers are doing.
    enum class Mode : uint8_t {
        idle,        ///< no finger down
        pressing,    ///< one finger down, within the slop, before the hold
        held,        ///< one finger down after hold_started, within the slop
        dragging,    ///< one finger dragging
        two,         ///< two fingers down
        two_lifting, ///< one of two lifted; the other gives nothing until it lifts
    };

    /// One centroid sample of two fingers.
    struct PanSample {
        float x{};          ///< canvas pixels
        float y{};          ///< canvas pixels
        uint64_t time_ns{}; ///< when the centroid was there
    };

    /// Returns the slot of a tracked finger.
    ///
    /// @param id the finger
    /// @return its slot in fingers_, or -1 when it is not tracked
    [[nodiscard]] int slot_of(const FingerId& id) const noexcept;
    /// Fires hold_started when the single finger has rested for the hold delay by `now_ns`.
    ///
    /// @param now_ns the time now
    /// @param[in,out] batch receives hold_started
    void fire_hold_if_due(uint64_t now_ns, GestureBatch& batch) noexcept;
    /// Reads a move of the single finger: rest_moved within the slop, else the drag's start.
    ///
    /// @param now_ns the time of the move
    /// @param[in,out] batch receives the gestures
    void move_single(uint64_t now_ns, GestureBatch& batch) noexcept;
    /// Reads a move of either of two fingers: the pan at every report, the pinch and the twist
    /// once both fingers have reported (or one has twice), so a pan whose fingers report one
    /// after the other never reads as a pinch or a twist.
    ///
    /// @param slot the slot of the finger that moved
    /// @param now_ns the time of the move
    /// @param[in,out] batch receives the gestures
    void move_pair(int slot, uint64_t now_ns, GestureBatch& batch) noexcept;
    /// Reads the single finger lifting before a hold: a tap or a double tap.
    ///
    /// @param now_ns the time of the lift
    /// @param[in,out] batch receives the tap
    void lift_tap(uint64_t now_ns, GestureBatch& batch) noexcept;
    /// Starts reading two fingers from where they are now.
    ///
    /// @param now_ns the time the second finger landed
    /// @param tap_allowed whether lifting both quickly may give two_finger_tap
    void start_pair(uint64_t now_ns, bool tap_allowed) noexcept;
    /// Keeps the two fingers' centroid for the pan's lift velocity.
    ///
    /// @param now_ns the time of the centroid
    void note_centroid(uint64_t now_ns) noexcept;
    /// Returns the pan's velocity at a lift from the recent centroid samples.
    ///
    /// @param now_ns the time of the lift
    /// @param[out] velocity_x canvas pixels per second
    /// @param[out] velocity_y canvas pixels per second
    void pan_velocity(uint64_t now_ns, float& velocity_x, float& velocity_y) const noexcept;
    /// Returns the centroid of the tracked fingers.
    ///
    /// @param[out] x canvas pixels
    /// @param[out] y canvas pixels
    void centroid(float& x, float& y) const noexcept;
    /// Returns the distance between the two tracked fingers.
    ///
    /// @return canvas pixels; 0 with fewer than two fingers
    [[nodiscard]] float spread() const noexcept;
    /// Returns the angle of the line from the first tracked finger to the second.
    ///
    /// @return radians, from the positive x axis; 0 with fewer than two fingers
    [[nodiscard]] float pair_angle() const noexcept;
    /// Stops tracking a finger.
    ///
    /// @param slot its slot in fingers_
    void drop_finger(int slot) noexcept;
    /// Drops every finger and the double-tap chain, keeping the thresholds.
    void clear_state() noexcept;

    Thresholds thresholds_{};                   ///< the thresholds set
    Thresholds active_{};                       ///< the thresholds the last landing took
    std::array<Finger, max_fingers> fingers_{}; ///< the first finger_count_ are tracked
    uint8_t finger_count_{};                    ///< fingers tracked
    Mode mode_{Mode::idle};                     ///< what the fingers are doing
    uint64_t last_ns_{};                        ///< the latest time seen while fingers are down
    float rest_x_{};                            ///< canvas pixels last reported by press/rest_moved
    float rest_y_{};                            ///< canvas pixels last reported by press/rest_moved
    DragRole drag_role_{DragRole::box};         ///< the running drag's role
    float drag_x_{};                            ///< canvas pixels at the previous drag event
    float drag_y_{};                            ///< canvas pixels at the previous drag event
    bool tap_chain_{};                          ///< the previous tap may start a double tap
    float tap_x_{};                             ///< canvas pixels of the previous tap
    float tap_y_{};                             ///< canvas pixels of the previous tap
    uint64_t tap_lift_ns_{};                    ///< when the previous tap lifted
    float pair_start_x_{};                      ///< the pair's centroid at the second landing
    float pair_start_y_{};                      ///< the pair's centroid at the second landing
    float pair_start_spread_{};                 ///< the pair's spread at the second landing
    float pan_x_{};                             ///< the centroid at the previous pan event
    float pan_y_{};                             ///< the centroid at the previous pan event
    float pinch_spread_{};                      ///< the spread at the previous pinch event
    bool panning_{};                            ///< pan_began was given for this pair
    bool pinching_{};                           ///< pinch_began was given for this pair
    bool twisting_{};                           ///< twist_began was given for this pair
    bool pair_tap_allowed_{};                   ///< the pair may still give two_finger_tap
    float pair_start_angle_{};                  ///< the pair's angle at the second landing
    float twist_angle_{};                       ///< the angle at the previous twist event
    std::array<bool, max_fingers> pinch_pending_{}; ///< by slot: moved since the pinch was read
    uint64_t pair_down_ns_{};                       ///< when the second finger landed
    float pair_lift_x_{};                           ///< the centroid when the first of two lifted
    float pair_lift_y_{};                           ///< the centroid when the first of two lifted
    std::array<PanSample, pan_history> pan_samples_{}; ///< ring of recent centroids
    uint8_t pan_sample_count_{};                       ///< samples held, up to pan_history
    uint8_t pan_sample_next_{};                        ///< the ring slot written next
};

} // namespace oa::ui::touch_gestures
