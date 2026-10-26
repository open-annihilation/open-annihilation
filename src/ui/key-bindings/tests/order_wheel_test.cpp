// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A held mouse button: a short click at its press, a drag of 40 points, a
// long press that opens the wheel, and a cancel that drops the press.
// The threshold is 32, K21's drag: 32 points is still a click.

#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/helpers.hpp"

#include <cstdint>

namespace {

namespace keys = oa::ui::key_bindings;

constexpr uint32_t long_press_ms = 500;
constexpr float drag_threshold = 32.0F;

} // namespace

int main() {
    keys::WheelButton click;
    click.press({10.0F, 20.0F}, 0);
    OA_CHECK(click.tick(100, long_press_ms, drag_threshold) == keys::WheelDecision::waiting);
    click.release({14.0F, 20.0F});
    OA_CHECK(click.tick(100, long_press_ms, drag_threshold) == keys::WheelDecision::pass_click);
    OA_CHECK(click.press_point().x == 10.0F);
    OA_CHECK(click.press_point().y == 20.0F);

    keys::WheelButton on_threshold;
    on_threshold.press({0.0F, 0.0F}, 0);
    on_threshold.release({drag_threshold, 0.0F});
    OA_CHECK(
        on_threshold.tick(100, long_press_ms, drag_threshold) == keys::WheelDecision::pass_click
    );
    OA_CHECK(on_threshold.press_point().x == 0.0F);

    keys::WheelButton drag;
    drag.press({3.0F, 4.0F}, 0);
    drag.motion({43.0F, 4.0F});
    OA_CHECK(drag.tick(100, long_press_ms, drag_threshold) == keys::WheelDecision::pass_drag);
    OA_CHECK(drag.press_point().x == 3.0F);
    OA_CHECK(drag.press_point().y == 4.0F);
    OA_CHECK(drag.point().x == 43.0F);
    OA_CHECK(drag.point().y == 4.0F);

    keys::WheelButton held;
    held.press({5.0F, 6.0F}, 0);
    OA_CHECK(held.tick(499, long_press_ms, drag_threshold) == keys::WheelDecision::waiting);
    OA_CHECK(held.tick(600, long_press_ms, drag_threshold) == keys::WheelDecision::open_wheel);
    OA_CHECK(held.press_point().x == 5.0F);
    OA_CHECK(held.press_point().y == 6.0F);

    keys::WheelButton nudged;
    nudged.press({0.0F, 0.0F}, 0);
    nudged.motion({31.0F, 0.0F});
    OA_CHECK(nudged.tick(100, long_press_ms, drag_threshold) == keys::WheelDecision::waiting);
    OA_CHECK(nudged.tick(600, long_press_ms, drag_threshold) == keys::WheelDecision::open_wheel);
    OA_CHECK(nudged.press_point().x == 0.0F);

    keys::WheelButton dropped;
    dropped.press({1.0F, 2.0F}, 0);
    dropped.cancel();
    OA_CHECK(dropped.tick(600, long_press_ms, drag_threshold) == keys::WheelDecision::waiting);
    OA_CHECK(dropped.press_point().x == 0.0F);
    OA_CHECK(dropped.press_point().y == 0.0F);

    return oa::test::check_exit_status();
}
