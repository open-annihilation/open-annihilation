// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A mouse button held back until it is a click, a drag or a long press.
// The caller passes the time and the threshold.
#include "oa/ui/key_bindings/helpers.hpp"

#include <cmath>
#include <cstdint>

namespace oa::ui::key_bindings {
namespace {

/// How far the pointer has moved from where the press went down.
///
/// @param origin the press
/// @param current the pointer now
/// @return the distance, in the points' units
float travel(WheelButton::Point origin, WheelButton::Point current) {
    return std::hypot(current.x - origin.x, current.y - origin.y);
}

} // namespace

void WheelButton::press(Point point, uint64_t now_ms) {
    origin_ = point;
    current_ = point;
    started_ms_ = now_ms;
    down_ = true;
    released_ = false;
    remembered_ = true;
}

void WheelButton::motion(Point point) {
    if (down_)
        current_ = point;
}

void WheelButton::release(Point point) {
    if (!down_)
        return;
    current_ = point;
    released_ = true;
}

WheelDecision WheelButton::tick(uint64_t now_ms, uint32_t long_press_ms, float drag_threshold) {
    if (!down_)
        return WheelDecision::waiting;
    const bool dragged = travel(origin_, current_) > drag_threshold;
    // A move past the threshold is a drag, including when the long press
    // has also passed: the pointer left the click before the wheel opens.
    if (dragged) {
        down_ = false;
        released_ = false;
        return WheelDecision::pass_drag;
    }
    const uint64_t elapsed = now_ms > started_ms_ ? now_ms - started_ms_ : 0;
    if (!released_ && elapsed >= long_press_ms) {
        down_ = false;
        return WheelDecision::open_wheel;
    }
    if (released_) {
        down_ = false;
        released_ = false;
        return WheelDecision::pass_click;
    }
    return WheelDecision::waiting;
}

void WheelButton::cancel() {
    down_ = false;
    released_ = false;
    remembered_ = false;
    origin_ = {};
    current_ = {};
    started_ms_ = 0;
}

WheelButton::Point WheelButton::press_point() const {
    return remembered_ ? origin_ : Point{};
}

WheelButton::Point WheelButton::point() const {
    return remembered_ ? current_ : Point{};
}

} // namespace oa::ui::key_bindings
