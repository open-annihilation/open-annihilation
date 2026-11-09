// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The gesture recogniser (touch_gestures.hpp): one finger gives presses,
// taps, double taps, holds and drags; two give pans, pinches, twists and
// two-finger taps. Time comes only from the reports and advance().
#include "oa/ui/touch_gestures.hpp"

#include <algorithm>
#include <cmath>

namespace oa::ui::touch_gestures {

namespace {

/// Nanoseconds in a millisecond.
constexpr uint64_t ns_per_ms = 1'000'000;
/// Nanoseconds in a second.
constexpr double ns_per_second = 1'000'000'000.0;
/// How far back a pan's lift velocity looks, in milliseconds.
constexpr uint64_t pan_velocity_window_ms = 100;
/// The smallest spread a pinch scale divides by, in canvas pixels.
constexpr float min_spread_px = 1.0f;
/// Pi, for wrapping a turn into half a circle either way.
constexpr float kPi = 3.14159265f;
/// A full turn, in radians.
constexpr float kTwoPi = 6.28318531f;

/// Wraps a radian delta into the range -pi..pi.
///
/// @param radians the delta
/// @return the wrapped delta
float wrap_radians(float radians) noexcept {
    float wrapped = std::fmod(radians + kPi, kTwoPi);
    if (wrapped < 0.0f)
        wrapped += kTwoPi;
    return wrapped - kPi;
}

/// Returns the distance between two points.
///
/// @param ax first point x
/// @param ay first point y
/// @param bx second point x
/// @param by second point y
/// @return the distance, in the points' units
float distance(float ax, float ay, float bx, float by) noexcept {
    return std::hypot(ax - bx, ay - by);
}

/// Returns a gesture of a kind at a point and time.
///
/// @param kind what the gesture is
/// @param x canvas pixels
/// @param y canvas pixels
/// @param time_ns when it happened
/// @return the gesture, its other fields at their defaults
Gesture gesture_at(GestureKind kind, float x, float y, uint64_t time_ns) noexcept {
    Gesture gesture{};
    gesture.kind = kind;
    gesture.x = x;
    gesture.y = y;
    gesture.time_ns = time_ns;
    return gesture;
}

/// Returns a threshold in points as canvas pixels.
///
/// @param thresholds the thresholds whose px_per_point converts
/// @param points the distance in points
/// @return the distance in canvas pixels
float points_to_px(const Thresholds& thresholds, float points) noexcept {
    const float px_per_point = thresholds.px_per_point > 0.0f ? thresholds.px_per_point : 1.0f;
    return points * px_per_point;
}

} // namespace

void GestureBatch::push(const Gesture& gesture) noexcept {
    if (count >= items.size())
        return;
    items[count] = gesture;
    ++count;
}

void Recogniser::set_thresholds(const Thresholds& thresholds) noexcept {
    thresholds_ = thresholds;
}

const Thresholds& Recogniser::thresholds() const noexcept {
    return thresholds_;
}

int Recogniser::slot_of(const FingerId& id) const noexcept {
    for (uint8_t slot = 0; slot < finger_count_; ++slot) {
        if (fingers_[slot].id == id)
            return slot;
    }
    return -1;
}

void Recogniser::fire_hold_if_due(uint64_t now_ns, GestureBatch& batch) noexcept {
    if (mode_ != Mode::pressing || finger_count_ != 1)
        return;
    const uint64_t due_ns = fingers_[0].down_ns + uint64_t{active_.hold_ms} * ns_per_ms;
    if (now_ns < due_ns)
        return;
    mode_ = Mode::held;
    tap_chain_ = false;
    batch.push(gesture_at(GestureKind::hold_started, fingers_[0].x, fingers_[0].y, now_ns));
}

void Recogniser::move_single(uint64_t now_ns, GestureBatch& batch) noexcept {
    fire_hold_if_due(now_ns, batch);
    const Finger& finger = fingers_[0];
    if (mode_ == Mode::dragging) {
        const float dx = finger.x - drag_x_;
        const float dy = finger.y - drag_y_;
        if (dx == 0.0f && dy == 0.0f)
            return;
        Gesture moved = gesture_at(GestureKind::drag_moved, finger.x, finger.y, now_ns);
        moved.start_x = finger.start_x;
        moved.start_y = finger.start_y;
        moved.dx = dx;
        moved.dy = dy;
        moved.role = drag_role_;
        batch.push(moved);
        drag_x_ = finger.x;
        drag_y_ = finger.y;
        return;
    }
    const float travel = distance(finger.x, finger.y, finger.start_x, finger.start_y);
    if (travel > points_to_px(active_, active_.slop_points)) {
        const bool from_hold = mode_ == Mode::held;
        drag_role_ = from_hold || active_.one_finger_drag == OneFingerDrag::box ? DragRole::box
                                                                                : DragRole::scroll;
        mode_ = Mode::dragging;
        tap_chain_ = false;
        Gesture began = gesture_at(GestureKind::drag_began, finger.x, finger.y, now_ns);
        began.start_x = finger.start_x;
        began.start_y = finger.start_y;
        began.role = drag_role_;
        began.from_hold = from_hold;
        batch.push(began);
        Gesture moved = began;
        moved.kind = GestureKind::drag_moved;
        moved.from_hold = false;
        moved.dx = finger.x - finger.start_x;
        moved.dy = finger.y - finger.start_y;
        batch.push(moved);
        drag_x_ = finger.x;
        drag_y_ = finger.y;
        return;
    }
    if (finger.x == rest_x_ && finger.y == rest_y_)
        return;
    rest_x_ = finger.x;
    rest_y_ = finger.y;
    batch.push(gesture_at(GestureKind::rest_moved, finger.x, finger.y, now_ns));
}

void Recogniser::move_pair(int slot, uint64_t now_ns, GestureBatch& batch) noexcept {
    float centre_x = 0.0f;
    float centre_y = 0.0f;
    centroid(centre_x, centre_y);
    const float now_spread = spread();
    note_centroid(now_ns);
    if (!panning_) {
        const float travel = distance(centre_x, centre_y, pair_start_x_, pair_start_y_);
        if (travel > points_to_px(active_, active_.slop_points)) {
            panning_ = true;
            pair_tap_allowed_ = false;
            batch.push(gesture_at(GestureKind::pan_began, centre_x, centre_y, now_ns));
            Gesture moved = gesture_at(GestureKind::pan_moved, centre_x, centre_y, now_ns);
            moved.dx = centre_x - pair_start_x_;
            moved.dy = centre_y - pair_start_y_;
            batch.push(moved);
            pan_x_ = centre_x;
            pan_y_ = centre_y;
        }
    } else if (centre_x != pan_x_ || centre_y != pan_y_) {
        Gesture moved = gesture_at(GestureKind::pan_moved, centre_x, centre_y, now_ns);
        moved.dx = centre_x - pan_x_;
        moved.dy = centre_y - pan_y_;
        batch.push(moved);
        pan_x_ = centre_x;
        pan_y_ = centre_y;
    }
    // The spread is read only once both fingers have reported since it was last read, or
    // one has reported twice (the other is still): one finger's report alone changes the
    // spread even when both fingers move together.
    const auto mover = static_cast<std::size_t>(slot);
    const std::size_t other = 1 - mover;
    if (!pinch_pending_[mover] && !pinch_pending_[other]) {
        pinch_pending_[mover] = true;
        return;
    }
    pinch_pending_ = {};
    if (!pinching_) {
        const float change = std::fabs(now_spread - pair_start_spread_);
        if (change >= points_to_px(active_, active_.pinch_points)) {
            pinching_ = true;
            pair_tap_allowed_ = false;
            batch.push(gesture_at(GestureKind::pinch_began, centre_x, centre_y, now_ns));
            Gesture moved = gesture_at(GestureKind::pinch_moved, centre_x, centre_y, now_ns);
            moved.scale = now_spread / std::max(pair_start_spread_, min_spread_px);
            batch.push(moved);
            pinch_spread_ = now_spread;
        }
    } else if (now_spread != pinch_spread_) {
        Gesture moved = gesture_at(GestureKind::pinch_moved, centre_x, centre_y, now_ns);
        moved.scale = now_spread / std::max(pinch_spread_, min_spread_px);
        batch.push(moved);
        pinch_spread_ = now_spread;
    }
    // Twist is off unless a caller asks for it. The same both-fingers gate as
    // the pinch: one finger's report alone is not a turn.
    if (!(active_.twist_degrees > 0.0f))
        return;
    const float now_angle = pair_angle();
    if (!twisting_) {
        const float degrees = wrap_radians(now_angle - pair_start_angle_) * (180.0f / kPi);
        if (std::fabs(degrees) >= active_.twist_degrees) {
            twisting_ = true;
            pair_tap_allowed_ = false;
            batch.push(gesture_at(GestureKind::twist_began, centre_x, centre_y, now_ns));
            Gesture moved = gesture_at(GestureKind::twist_moved, centre_x, centre_y, now_ns);
            moved.turn = degrees;
            batch.push(moved);
            twist_angle_ = now_angle;
        }
    } else {
        const float degrees = wrap_radians(now_angle - twist_angle_) * (180.0f / kPi);
        if (degrees != 0.0f) {
            Gesture moved = gesture_at(GestureKind::twist_moved, centre_x, centre_y, now_ns);
            moved.turn = degrees;
            batch.push(moved);
            twist_angle_ = now_angle;
        }
    }
}

void Recogniser::lift_tap(uint64_t now_ns, GestureBatch& batch) noexcept {
    const Finger& finger = fingers_[0];
    Gesture tap = gesture_at(GestureKind::tap, finger.x, finger.y, now_ns);
    tap.taps = 1;
    if (tap_chain_) {
        const uint64_t window_ns = uint64_t{active_.double_tap_ms} * ns_per_ms;
        const bool soon =
            finger.down_ns >= tap_lift_ns_ && finger.down_ns - tap_lift_ns_ <= window_ns;
        const bool near = distance(finger.x, finger.y, tap_x_, tap_y_) <=
                          points_to_px(active_, active_.double_tap_points);
        if (soon && near)
            tap.taps = 2;
    }
    batch.push(tap);
    // A double tap ends the chain, so a third quick tap counts one again.
    tap_chain_ = tap.taps == 1;
    tap_x_ = finger.x;
    tap_y_ = finger.y;
    tap_lift_ns_ = now_ns;
}

void Recogniser::start_pair(uint64_t now_ns, bool tap_allowed) noexcept {
    mode_ = Mode::two;
    tap_chain_ = false;
    centroid(pair_start_x_, pair_start_y_);
    pair_start_spread_ = spread();
    pan_x_ = pair_start_x_;
    pan_y_ = pair_start_y_;
    pinch_spread_ = pair_start_spread_;
    panning_ = false;
    pinching_ = false;
    twisting_ = false;
    pair_start_angle_ = pair_angle();
    twist_angle_ = pair_start_angle_;
    pair_tap_allowed_ = tap_allowed;
    pinch_pending_ = {};
    pair_down_ns_ = now_ns;
    pan_sample_count_ = 0;
    pan_sample_next_ = 0;
    note_centroid(now_ns);
}

void Recogniser::note_centroid(uint64_t now_ns) noexcept {
    PanSample sample{};
    centroid(sample.x, sample.y);
    sample.time_ns = now_ns;
    pan_samples_[pan_sample_next_] = sample;
    pan_sample_next_ = static_cast<uint8_t>((pan_sample_next_ + 1) % pan_history);
    if (pan_sample_count_ < pan_history)
        ++pan_sample_count_;
}

void Recogniser::pan_velocity(
    uint64_t now_ns, float& velocity_x, float& velocity_y
) const noexcept {
    velocity_x = 0.0f;
    velocity_y = 0.0f;
    const uint64_t window_ns = pan_velocity_window_ms * ns_per_ms;
    const uint64_t since_ns = now_ns > window_ns ? now_ns - window_ns : 0;
    const PanSample* oldest = nullptr;
    const PanSample* newest = nullptr;
    for (uint8_t index = 0; index < pan_sample_count_; ++index) {
        const PanSample& sample = pan_samples_[index];
        if (sample.time_ns < since_ns || sample.time_ns > now_ns)
            continue;
        if (oldest == nullptr || sample.time_ns < oldest->time_ns)
            oldest = &sample;
        if (newest == nullptr || sample.time_ns >= newest->time_ns)
            newest = &sample;
    }
    if (oldest == nullptr || newest == nullptr || newest->time_ns <= oldest->time_ns)
        return;
    const double seconds = static_cast<double>(newest->time_ns - oldest->time_ns) / ns_per_second;
    velocity_x = static_cast<float>((newest->x - oldest->x) / seconds);
    velocity_y = static_cast<float>((newest->y - oldest->y) / seconds);
}

void Recogniser::centroid(float& x, float& y) const noexcept {
    x = 0.0f;
    y = 0.0f;
    if (finger_count_ == 0)
        return;
    for (uint8_t slot = 0; slot < finger_count_; ++slot) {
        x += fingers_[slot].x;
        y += fingers_[slot].y;
    }
    x /= static_cast<float>(finger_count_);
    y /= static_cast<float>(finger_count_);
}

float Recogniser::spread() const noexcept {
    if (finger_count_ < 2)
        return 0.0f;
    return distance(fingers_[0].x, fingers_[0].y, fingers_[1].x, fingers_[1].y);
}

float Recogniser::pair_angle() const noexcept {
    if (finger_count_ < 2)
        return 0.0f;
    return std::atan2(fingers_[1].y - fingers_[0].y, fingers_[1].x - fingers_[0].x);
}

void Recogniser::drop_finger(int slot) noexcept {
    if (slot < 0 || slot >= finger_count_)
        return;
    for (int index = slot; index + 1 < finger_count_; ++index)
        fingers_[static_cast<std::size_t>(index)] = fingers_[static_cast<std::size_t>(index) + 1];
    --finger_count_;
    fingers_[finger_count_] = {};
}

void Recogniser::clear_state() noexcept {
    const Thresholds kept = thresholds_;
    *this = Recogniser{};
    thresholds_ = kept;
    active_ = kept;
}

GestureBatch Recogniser::finger(FingerPhase phase, const FingerSample& sample) noexcept {
    GestureBatch batch{};
    const int slot = slot_of(sample.id);
    // Time never runs backwards inside one sequence of fingers.
    const uint64_t now_ns =
        finger_count_ != 0 ? std::max(sample.time_ns, last_ns_) : sample.time_ns;
    switch (phase) {
    case FingerPhase::down: {
        if (slot >= 0 || finger_count_ >= max_fingers)
            return batch;
        last_ns_ = now_ns;
        active_ = thresholds_;
        Finger finger{};
        finger.id = sample.id;
        finger.x = sample.x;
        finger.y = sample.y;
        finger.start_x = sample.x;
        finger.start_y = sample.y;
        finger.down_ns = now_ns;
        if (finger_count_ == 0) {
            fingers_[0] = finger;
            finger_count_ = 1;
            mode_ = Mode::pressing;
            rest_x_ = sample.x;
            rest_y_ = sample.y;
            batch.push(gesture_at(GestureKind::press, sample.x, sample.y, now_ns));
            return batch;
        }
        fire_hold_if_due(now_ns, batch);
        const Mode before = mode_;
        if (before == Mode::held || before == Mode::dragging)
            batch.push(gesture_at(GestureKind::cancelled, fingers_[0].x, fingers_[0].y, now_ns));
        fingers_[finger_count_] = finger;
        ++finger_count_;
        start_pair(now_ns, before == Mode::pressing);
        return batch;
    }
    case FingerPhase::move: {
        if (slot < 0)
            return batch;
        last_ns_ = now_ns;
        fingers_[static_cast<std::size_t>(slot)].x = sample.x;
        fingers_[static_cast<std::size_t>(slot)].y = sample.y;
        if (mode_ == Mode::two)
            move_pair(slot, now_ns, batch);
        else if (mode_ == Mode::pressing || mode_ == Mode::held || mode_ == Mode::dragging)
            move_single(now_ns, batch);
        return batch;
    }
    case FingerPhase::up: {
        if (slot < 0)
            return batch;
        last_ns_ = now_ns;
        fingers_[static_cast<std::size_t>(slot)].x = sample.x;
        fingers_[static_cast<std::size_t>(slot)].y = sample.y;
        switch (mode_) {
        case Mode::pressing:
            fire_hold_if_due(now_ns, batch);
            if (mode_ == Mode::held)
                batch.push(gesture_at(GestureKind::hold_released, sample.x, sample.y, now_ns));
            else
                lift_tap(now_ns, batch);
            break;
        case Mode::held:
            batch.push(gesture_at(GestureKind::hold_released, sample.x, sample.y, now_ns));
            break;
        case Mode::dragging: {
            Gesture ended = gesture_at(GestureKind::drag_ended, sample.x, sample.y, now_ns);
            ended.start_x = fingers_[0].start_x;
            ended.start_y = fingers_[0].start_y;
            ended.dx = sample.x - drag_x_;
            ended.dy = sample.y - drag_y_;
            ended.role = drag_role_;
            batch.push(ended);
            break;
        }
        case Mode::two: {
            note_centroid(now_ns);
            centroid(pair_lift_x_, pair_lift_y_);
            if (panning_) {
                Gesture ended =
                    gesture_at(GestureKind::pan_ended, pair_lift_x_, pair_lift_y_, now_ns);
                pan_velocity(now_ns, ended.velocity_x, ended.velocity_y);
                batch.push(ended);
            }
            if (pinching_) {
                const float now_spread = spread();
                if (now_spread != pinch_spread_) {
                    Gesture moved =
                        gesture_at(GestureKind::pinch_moved, pair_lift_x_, pair_lift_y_, now_ns);
                    moved.scale = now_spread / std::max(pinch_spread_, min_spread_px);
                    batch.push(moved);
                }
                batch.push(
                    gesture_at(GestureKind::pinch_ended, pair_lift_x_, pair_lift_y_, now_ns)
                );
            }
            if (twisting_)
                batch.push(
                    gesture_at(GestureKind::twist_ended, pair_lift_x_, pair_lift_y_, now_ns)
                );
            const uint64_t window_ns = uint64_t{active_.two_finger_tap_ms} * ns_per_ms;
            pair_tap_allowed_ = pair_tap_allowed_ && !panning_ && !pinching_ && !twisting_ &&
                                now_ns - pair_down_ns_ <= window_ns;
            drop_finger(slot);
            mode_ = Mode::two_lifting;
            return batch;
        }
        case Mode::two_lifting: {
            const uint64_t window_ns = uint64_t{active_.two_finger_tap_ms} * ns_per_ms;
            if (pair_tap_allowed_ && now_ns - pair_down_ns_ <= window_ns) {
                batch.push(
                    gesture_at(GestureKind::two_finger_tap, pair_lift_x_, pair_lift_y_, now_ns)
                );
            }
            break;
        }
        case Mode::idle:
            break;
        }
        drop_finger(slot);
        if (finger_count_ == 0) {
            mode_ = Mode::idle;
            last_ns_ = 0;
        }
        return batch;
    }
    case FingerPhase::cancel: {
        if (slot < 0)
            return batch;
        const Finger& finger = fingers_[static_cast<std::size_t>(slot)];
        batch.push(gesture_at(GestureKind::cancelled, finger.x, finger.y, now_ns));
        clear_state();
        return batch;
    }
    }
    return batch;
}

GestureBatch Recogniser::advance(uint64_t now_ns) noexcept {
    GestureBatch batch{};
    if (finger_count_ == 0)
        return batch;
    now_ns = std::max(now_ns, last_ns_);
    last_ns_ = now_ns;
    fire_hold_if_due(now_ns, batch);
    return batch;
}

void Recogniser::reset() noexcept {
    clear_state();
}

uint8_t Recogniser::fingers_down() const noexcept {
    return finger_count_;
}

bool Recogniser::dragging(DragRole* role) const noexcept {
    if (mode_ != Mode::dragging)
        return false;
    if (role != nullptr)
        *role = drag_role_;
    return true;
}

} // namespace oa::ui::touch_gestures
