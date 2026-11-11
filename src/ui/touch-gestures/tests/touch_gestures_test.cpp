// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The gesture recogniser's timelines: taps, double taps, holds, drags of
// both roles, two-finger pans, pinches and taps, and cancels.
#include "oa/test/check.hpp"
#include "oa/ui/touch_gestures.hpp"

#include <cmath>
#include <vector>

namespace {

namespace gestures = oa::ui::touch_gestures;
using gestures::FingerPhase;
using gestures::GestureKind;

/// Nanoseconds in a millisecond.
constexpr uint64_t ms = 1'000'000;

/// Returns whether two floats agree within a tolerance.
bool near(float a, float b, float tolerance = 0.001f) {
    return std::fabs(a - b) <= tolerance;
}

/// Feeds finger reports to one recogniser and keeps every gesture it gives.
struct Timeline {
    gestures::Recogniser recogniser;
    std::vector<gestures::Gesture> all;

    /// Feeds one report of finger `finger` and returns the gestures of that step.
    std::vector<gestures::Gesture>
    step(FingerPhase phase, uint64_t finger, float x, float y, uint64_t time_ms) {
        gestures::FingerSample sample{};
        sample.id = {1, finger};
        sample.x = x;
        sample.y = y;
        sample.time_ns = time_ms * ms;
        return keep(recogniser.finger(phase, sample));
    }

    /// Lands finger `finger` at x,y.
    std::vector<gestures::Gesture> down(uint64_t finger, float x, float y, uint64_t time_ms) {
        return step(FingerPhase::down, finger, x, y, time_ms);
    }

    /// Moves finger `finger` to x,y.
    std::vector<gestures::Gesture> move(uint64_t finger, float x, float y, uint64_t time_ms) {
        return step(FingerPhase::move, finger, x, y, time_ms);
    }

    /// Lifts finger `finger` at x,y.
    std::vector<gestures::Gesture> up(uint64_t finger, float x, float y, uint64_t time_ms) {
        return step(FingerPhase::up, finger, x, y, time_ms);
    }

    /// Cancels finger `finger`.
    std::vector<gestures::Gesture> cancel(uint64_t finger, uint64_t time_ms) {
        return step(FingerPhase::cancel, finger, 0.0f, 0.0f, time_ms);
    }

    /// Moves time on without a report.
    std::vector<gestures::Gesture> advance(uint64_t time_ms) {
        return keep(recogniser.advance(time_ms * ms));
    }

    /// Appends a batch to `all` and returns it as a list.
    std::vector<gestures::Gesture> keep(const gestures::GestureBatch& batch) {
        std::vector<gestures::Gesture> list;
        for (uint8_t index = 0; index < batch.count; ++index) {
            list.push_back(batch.items[index]);
            all.push_back(batch.items[index]);
        }
        return list;
    }

    /// Returns how many gestures of a kind were given so far.
    [[nodiscard]] int count(GestureKind kind) const {
        int total = 0;
        for (const auto& gesture : all) {
            if (gesture.kind == kind)
                ++total;
        }
        return total;
    }
};

/// Returns whether a step gave exactly these kinds, in order.
bool kinds_are(
    const std::vector<gestures::Gesture>& list, std::initializer_list<GestureKind> kinds
) {
    if (list.size() != kinds.size())
        return false;
    std::size_t index = 0;
    for (const GestureKind kind : kinds) {
        if (list[index].kind != kind)
            return false;
        ++index;
    }
    return true;
}

/// Returns a recogniser's thresholds with one change.
gestures::Thresholds thresholds_with_drag(gestures::OneFingerDrag drag) {
    gestures::Thresholds thresholds{};
    thresholds.one_finger_drag = drag;
    return thresholds;
}

/// Checks that a batch keeps at most max_gestures_per_step gestures, in order.
void batch_keeps_its_capacity() {
    gestures::GestureBatch batch{};
    for (std::size_t index = 0; index <= gestures::max_gestures_per_step; ++index) {
        gestures::Gesture gesture{};
        gesture.taps = static_cast<uint8_t>(index);
        batch.push(gesture);
    }
    OA_CHECK(batch.count == gestures::max_gestures_per_step);
    OA_CHECK(batch.items[0].taps == 0);
    OA_CHECK(
        batch.items[gestures::max_gestures_per_step - 1].taps == gestures::max_gestures_per_step - 1
    );
}

/// Checks that the thresholds given are the thresholds kept.
void thresholds_are_kept() {
    gestures::Recogniser recogniser;
    gestures::Thresholds thresholds{};
    thresholds.hold_ms = 500;
    thresholds.one_finger_drag = gestures::OneFingerDrag::scroll;
    recogniser.set_thresholds(thresholds);
    OA_CHECK(recogniser.thresholds().hold_ms == 500);
    OA_CHECK(recogniser.thresholds().one_finger_drag == gestures::OneFingerDrag::scroll);
}

/// Checks a short press with a little travel: press, then one tap where it lifted.
void a_quick_press_is_a_tap() {
    Timeline line;
    const auto landed = line.down(1, 100.0f, 100.0f, 0);
    OA_CHECK(kinds_are(landed, {GestureKind::press}));
    OA_CHECK(landed[0].x == 100.0f && landed[0].y == 100.0f);
    OA_CHECK(line.recogniser.fingers_down() == 1);
    const auto lifted = line.up(1, 103.0f, 100.0f, 100);
    OA_CHECK(kinds_are(lifted, {GestureKind::tap}));
    OA_CHECK(lifted.size() == 1 && lifted[0].taps == 1);
    OA_CHECK(lifted.size() == 1 && lifted[0].x == 103.0f && lifted[0].y == 100.0f);
    OA_CHECK(lifted.size() == 1 && lifted[0].time_ns == 100 * ms);
    OA_CHECK(line.recogniser.fingers_down() == 0);
    OA_CHECK(line.advance(1000).empty());
    OA_CHECK(line.all.size() == 2);
}

/// Taps finger 1 at x,y from down_ms to down_ms + 50 and returns the tap's count.
int tap_count(Timeline& line, float x, float y, uint64_t down_ms) {
    line.down(1, x, y, down_ms);
    const auto lifted = line.up(1, x, y, down_ms + 50);
    return lifted.size() == 1 && lifted[0].kind == GestureKind::tap ? lifted[0].taps : -1;
}

/// Checks the double tap's time and distance windows, and that a third quick tap counts one.
void a_second_quick_tap_nearby_is_a_double_tap() {
    {
        Timeline line;
        line.down(1, 100.0f, 100.0f, 0);
        line.up(1, 100.0f, 100.0f, 100);
        OA_CHECK(tap_count(line, 110.0f, 100.0f, 300) == 2);
    }
    {
        Timeline line;
        line.down(1, 100.0f, 100.0f, 0);
        line.up(1, 100.0f, 100.0f, 100);
        OA_CHECK(tap_count(line, 110.0f, 100.0f, 500) == 1);
    }
    {
        Timeline line;
        line.down(1, 100.0f, 100.0f, 0);
        line.up(1, 100.0f, 100.0f, 100);
        OA_CHECK(tap_count(line, 130.0f, 100.0f, 300) == 1);
    }
    {
        Timeline line;
        OA_CHECK(tap_count(line, 100.0f, 100.0f, 0) == 1);
        OA_CHECK(tap_count(line, 100.0f, 100.0f, 150) == 2);
        OA_CHECK(tap_count(line, 100.0f, 100.0f, 300) == 1);
        OA_CHECK(tap_count(line, 100.0f, 100.0f, 450) == 2);
    }
    {
        // The windows are in points: 3 pixels a point make 24 pt 72 pixels.
        Timeline line;
        gestures::Thresholds thresholds{};
        thresholds.px_per_point = 3.0f;
        line.recogniser.set_thresholds(thresholds);
        OA_CHECK(tap_count(line, 100.0f, 100.0f, 0) == 1);
        OA_CHECK(tap_count(line, 160.0f, 100.0f, 200) == 2);
    }
}

/// Checks the slop: travel within it rests, travel past it drags with the configured role.
void travel_past_the_slop_begins_a_drag() {
    {
        Timeline line;
        line.down(1, 100.0f, 100.0f, 0);
        const auto rested = line.move(1, 109.0f, 100.0f, 50);
        OA_CHECK(kinds_are(rested, {GestureKind::rest_moved}));
        OA_CHECK(rested.size() == 1 && rested[0].x == 109.0f);
        OA_CHECK(line.move(1, 109.0f, 100.0f, 60).empty());
        OA_CHECK(!line.recogniser.dragging());
        const auto lifted = line.up(1, 109.0f, 100.0f, 100);
        OA_CHECK(kinds_are(lifted, {GestureKind::tap}));
    }
    for (const auto drag : {gestures::OneFingerDrag::box, gestures::OneFingerDrag::scroll}) {
        const auto role = drag == gestures::OneFingerDrag::box ? gestures::DragRole::box
                                                               : gestures::DragRole::scroll;
        Timeline line;
        line.recogniser.set_thresholds(thresholds_with_drag(drag));
        line.down(1, 100.0f, 100.0f, 0);
        const auto began = line.move(1, 111.0f, 100.0f, 50);
        OA_CHECK(kinds_are(began, {GestureKind::drag_began, GestureKind::drag_moved}));
        if (began.size() == 2) {
            OA_CHECK(began[0].role == role && !began[0].from_hold);
            OA_CHECK(began[0].start_x == 100.0f && began[0].start_y == 100.0f);
            OA_CHECK(began[0].x == 111.0f);
            OA_CHECK(began[1].dx == 11.0f && began[1].dy == 0.0f);
            OA_CHECK(began[1].role == role);
        }
        gestures::DragRole running =
            role == gestures::DragRole::box ? gestures::DragRole::scroll : gestures::DragRole::box;
        OA_CHECK(line.recogniser.dragging(&running) && running == role);
        const auto moved = line.move(1, 120.0f, 95.0f, 60);
        OA_CHECK(kinds_are(moved, {GestureKind::drag_moved}));
        OA_CHECK(moved.size() == 1 && moved[0].dx == 9.0f && moved[0].dy == -5.0f);
        // A drag never turns into a hold.
        OA_CHECK(line.advance(2000).empty());
        const auto ended = line.up(1, 121.0f, 95.0f, 2100);
        OA_CHECK(kinds_are(ended, {GestureKind::drag_ended}));
        OA_CHECK(ended.size() == 1 && ended[0].x == 121.0f && ended[0].role == role);
        OA_CHECK(!line.recogniser.dragging());
        OA_CHECK(line.count(GestureKind::tap) == 0);
    }
    {
        // With 3 pixels a point the slop is 30 pixels.
        Timeline line;
        gestures::Thresholds thresholds{};
        thresholds.px_per_point = 3.0f;
        line.recogniser.set_thresholds(thresholds);
        line.down(1, 100.0f, 100.0f, 0);
        OA_CHECK(kinds_are(line.move(1, 129.0f, 100.0f, 20), {GestureKind::rest_moved}));
        OA_CHECK(kinds_are(
            line.move(1, 131.0f, 100.0f, 40), {GestureKind::drag_began, GestureKind::drag_moved}
        ));
    }
}

/// Checks the hold: due at hold_ms exactly, once, then released or dragged as a box.
void resting_for_the_hold_delay_is_a_hold() {
    {
        Timeline line;
        line.down(1, 100.0f, 100.0f, 0);
        OA_CHECK(line.advance(349).empty());
        const auto held = line.advance(350);
        OA_CHECK(kinds_are(held, {GestureKind::hold_started}));
        OA_CHECK(held.size() == 1 && held[0].x == 100.0f && held[0].y == 100.0f);
        OA_CHECK(line.advance(400).empty());
        OA_CHECK(line.advance(2000).empty());
        const auto released = line.up(1, 102.0f, 101.0f, 2100);
        OA_CHECK(kinds_are(released, {GestureKind::hold_released}));
        OA_CHECK(released.size() == 1 && released[0].x == 102.0f && released[0].y == 101.0f);
        OA_CHECK(line.count(GestureKind::hold_started) == 1);
        OA_CHECK(line.count(GestureKind::tap) == 0);
    }
    {
        // A hold then a drag is always a box, whatever one_finger_drag says.
        Timeline line;
        line.recogniser.set_thresholds(thresholds_with_drag(gestures::OneFingerDrag::scroll));
        line.down(1, 100.0f, 100.0f, 0);
        OA_CHECK(kinds_are(line.advance(360), {GestureKind::hold_started}));
        OA_CHECK(kinds_are(line.move(1, 105.0f, 100.0f, 380), {GestureKind::rest_moved}));
        const auto began = line.move(1, 111.0f, 100.0f, 400);
        OA_CHECK(kinds_are(began, {GestureKind::drag_began, GestureKind::drag_moved}));
        if (!began.empty()) {
            OA_CHECK(began[0].role == gestures::DragRole::box);
            OA_CHECK(began[0].from_hold);
        }
        OA_CHECK(kinds_are(line.up(1, 140.0f, 100.0f, 500), {GestureKind::drag_ended}));
        OA_CHECK(line.count(GestureKind::hold_released) == 0);
    }
    for (const uint32_t hold_ms : {250u, 700u}) {
        Timeline line;
        gestures::Thresholds thresholds{};
        thresholds.hold_ms = hold_ms;
        line.recogniser.set_thresholds(thresholds);
        line.down(1, 50.0f, 50.0f, 1000);
        OA_CHECK(line.advance(1000 + hold_ms - 1).empty());
        OA_CHECK(kinds_are(line.advance(1000 + hold_ms), {GestureKind::hold_started}));
    }
    {
        // A lift after the delay with no advance in between is still a hold.
        Timeline line;
        line.down(1, 100.0f, 100.0f, 0);
        OA_CHECK(kinds_are(
            line.up(1, 100.0f, 100.0f, 400), {GestureKind::hold_started, GestureKind::hold_released}
        ));
    }
    {
        // A move report after the delay fires the hold before it reads the move.
        Timeline line;
        line.down(1, 100.0f, 100.0f, 0);
        OA_CHECK(kinds_are(
            line.move(1, 104.0f, 100.0f, 360), {GestureKind::hold_started, GestureKind::rest_moved}
        ));
    }
    {
        // New thresholds take effect for the next finger that lands.
        Timeline line;
        line.down(1, 100.0f, 100.0f, 0);
        gestures::Thresholds thresholds{};
        thresholds.hold_ms = 600;
        line.recogniser.set_thresholds(thresholds);
        OA_CHECK(kinds_are(line.advance(350), {GestureKind::hold_started}));
        line.up(1, 100.0f, 100.0f, 400);
        line.down(1, 100.0f, 100.0f, 1000);
        OA_CHECK(line.advance(1350).empty());
        OA_CHECK(kinds_are(line.advance(1600), {GestureKind::hold_started}));
    }
}

/// Checks the two-finger tap's time window.
void two_quick_fingers_are_a_two_finger_tap() {
    {
        Timeline line;
        line.down(1, 100.0f, 100.0f, 0);
        OA_CHECK(line.down(2, 200.0f, 100.0f, 10).empty());
        OA_CHECK(line.recogniser.fingers_down() == 2);
        OA_CHECK(line.up(1, 100.0f, 100.0f, 150).empty());
        const auto lifted = line.up(2, 200.0f, 100.0f, 210);
        OA_CHECK(kinds_are(lifted, {GestureKind::two_finger_tap}));
        OA_CHECK(lifted.size() == 1 && lifted[0].x == 150.0f && lifted[0].y == 100.0f);
        OA_CHECK(line.count(GestureKind::tap) == 0);
        OA_CHECK(line.recogniser.fingers_down() == 0);
    }
    {
        Timeline line;
        line.down(1, 100.0f, 100.0f, 0);
        line.down(2, 200.0f, 100.0f, 10);
        OA_CHECK(line.up(1, 100.0f, 100.0f, 200).empty());
        OA_CHECK(line.up(2, 200.0f, 100.0f, 310).empty());
        OA_CHECK(line.count(GestureKind::two_finger_tap) == 0);
        OA_CHECK(line.count(GestureKind::tap) == 0);
    }
}

/// Checks the pan: the centroid passing the slop, its moves and its lift velocity.
void two_fingers_moving_together_pan() {
    Timeline line;
    line.down(1, 100.0f, 100.0f, 0);
    line.down(2, 200.0f, 100.0f, 10);
    // Finger 1 alone moves the centroid 10, within the slop.
    OA_CHECK(line.move(1, 120.0f, 100.0f, 50).empty());
    const auto began = line.move(2, 220.0f, 100.0f, 50);
    OA_CHECK(kinds_are(began, {GestureKind::pan_began, GestureKind::pan_moved}));
    if (began.size() == 2) {
        OA_CHECK(began[0].x == 170.0f && began[0].y == 100.0f);
        OA_CHECK(began[1].dx == 20.0f && began[1].dy == 0.0f);
    }
    const auto moved = line.move(1, 130.0f, 110.0f, 55);
    OA_CHECK(kinds_are(moved, {GestureKind::pan_moved}));
    OA_CHECK(moved.size() == 1 && near(moved[0].dx, 5.0f) && near(moved[0].dy, 5.0f));
    const auto ended = line.up(1, 130.0f, 110.0f, 60);
    OA_CHECK(kinds_are(ended, {GestureKind::pan_ended}));
    if (ended.size() == 1) {
        // 25 pixels in 50 ms across, 5 pixels down.
        OA_CHECK(near(ended[0].velocity_x, 500.0f, 1.0f));
        OA_CHECK(near(ended[0].velocity_y, 100.0f, 1.0f));
    }
    // The remaining finger gives nothing until it lifts.
    OA_CHECK(line.move(2, 300.0f, 300.0f, 80).empty());
    OA_CHECK(line.advance(2000).empty());
    OA_CHECK(line.up(2, 300.0f, 300.0f, 2100).empty());
    OA_CHECK(line.count(GestureKind::two_finger_tap) == 0);
    OA_CHECK(line.recogniser.fingers_down() == 0);
}

/// Checks the pinch: the spread changing, scales whose product is the spread's ratio.
void two_fingers_spreading_pinch() {
    Timeline line;
    line.down(1, 100.0f, 100.0f, 0);
    line.down(2, 200.0f, 100.0f, 10);
    float product = 1.0f;
    float left = 100.0f;
    float right = 200.0f;
    uint64_t time_ms = 20;
    while (right < 225.0f) {
        left -= 5.0f;
        for (const auto& gesture : line.move(1, left, 100.0f, time_ms)) {
            if (gesture.kind == GestureKind::pinch_moved)
                product *= gesture.scale;
            OA_CHECK(gesture.kind != GestureKind::pan_began);
        }
        right += 5.0f;
        for (const auto& gesture : line.move(2, right, 100.0f, time_ms + 5)) {
            if (gesture.kind == GestureKind::pinch_moved) {
                product *= gesture.scale;
                OA_CHECK(near(gesture.x, 150.0f) && near(gesture.y, 100.0f));
            }
        }
        time_ms += 10;
    }
    OA_CHECK(right - left == 150.0f);
    OA_CHECK(line.count(GestureKind::pinch_began) == 1);
    OA_CHECK(near(product, 1.5f));
    OA_CHECK(line.count(GestureKind::pan_began) == 0);
    const auto ended = line.up(2, right, 100.0f, time_ms);
    OA_CHECK(kinds_are(ended, {GestureKind::pinch_ended}));
    line.up(1, left, 100.0f, time_ms + 10);
    OA_CHECK(line.count(GestureKind::two_finger_tap) == 0);
}

/// Checks a pan and a pinch beginning in one report: the pan first, then the pinch, which is
/// read once both fingers have reported.
void pan_and_pinch_run_together() {
    Timeline line;
    line.down(1, 100.0f, 100.0f, 0);
    line.down(2, 200.0f, 100.0f, 10);
    OA_CHECK(line.move(1, 98.0f, 100.0f, 20).empty());
    const auto both = line.move(2, 240.0f, 100.0f, 30);
    OA_CHECK(kinds_are(
        both,
        {GestureKind::pan_began,
         GestureKind::pan_moved,
         GestureKind::pinch_began,
         GestureKind::pinch_moved}
    ));
    if (both.size() == 4) {
        OA_CHECK(near(both[1].dx, 19.0f));
        OA_CHECK(near(both[3].scale, 1.42f));
        OA_CHECK(near(both[3].x, 169.0f) && near(both[3].y, 100.0f));
    }
    // One finger's report moves the pan at once; the pinch waits for the other's.
    const auto first = line.move(1, 78.0f, 100.0f, 40);
    OA_CHECK(kinds_are(first, {GestureKind::pan_moved}));
    OA_CHECK(first.size() == 1 && near(first[0].dx, -10.0f));
    const auto second = line.move(2, 242.0f, 100.0f, 45);
    OA_CHECK(kinds_are(second, {GestureKind::pan_moved, GestureKind::pinch_moved}));
    if (second.size() == 2) {
        OA_CHECK(near(second[0].dx, 1.0f));
        OA_CHECK(near(second[1].scale, 164.0f / 142.0f));
    }
    OA_CHECK(
        kinds_are(line.up(1, 78.0f, 100.0f, 50), {GestureKind::pan_ended, GestureKind::pinch_ended})
    );
}

/// Checks that a pan whose fingers report one after the other in long steps is no pinch.
void a_fast_pan_is_no_pinch() {
    Timeline line;
    line.down(1, 100.0f, 100.0f, 0);
    line.down(2, 200.0f, 100.0f, 10);
    float offset = 0.0f;
    for (uint64_t step = 1; step <= 10; ++step) {
        offset += 30.0f;
        line.move(1, 100.0f + offset, 100.0f, 10 + step * 8);
        line.move(2, 200.0f + offset, 100.0f, 14 + step * 8);
    }
    OA_CHECK(line.count(GestureKind::pan_began) == 1);
    OA_CHECK(line.count(GestureKind::pinch_began) == 0);
    float panned = 0.0f;
    for (const auto& gesture : line.all) {
        if (gesture.kind == GestureKind::pan_moved)
            panned += gesture.dx;
    }
    OA_CHECK(near(panned, 300.0f));
}

/// Checks a second finger during a drag or after a hold: cancelled, then two-finger gestures.
void a_second_finger_during_a_drag_cancels_it() {
    {
        Timeline line;
        line.down(1, 100.0f, 100.0f, 0);
        line.move(1, 130.0f, 100.0f, 30);
        OA_CHECK(line.recogniser.dragging());
        const auto second = line.down(2, 300.0f, 100.0f, 40);
        OA_CHECK(kinds_are(second, {GestureKind::cancelled}));
        OA_CHECK(!line.recogniser.dragging());
        OA_CHECK(line.move(1, 150.0f, 100.0f, 50).empty());
        OA_CHECK(kinds_are(
            line.move(2, 320.0f, 100.0f, 60), {GestureKind::pan_began, GestureKind::pan_moved}
        ));
        OA_CHECK(line.count(GestureKind::drag_ended) == 0);
    }
    {
        Timeline line;
        line.down(1, 100.0f, 100.0f, 0);
        line.advance(400);
        OA_CHECK(kinds_are(line.down(2, 300.0f, 100.0f, 450), {GestureKind::cancelled}));
        line.up(1, 100.0f, 100.0f, 500);
        line.up(2, 300.0f, 100.0f, 510);
        OA_CHECK(line.count(GestureKind::hold_released) == 0);
        OA_CHECK(line.count(GestureKind::two_finger_tap) == 0);
    }
    {
        // A hold that came due before the second finger landed counts as a hold.
        Timeline line;
        line.down(1, 100.0f, 100.0f, 0);
        OA_CHECK(kinds_are(
            line.down(2, 300.0f, 100.0f, 400), {GestureKind::hold_started, GestureKind::cancelled}
        ));
    }
}

/// Checks that a third finger is ignored until one of the two lifts.
void a_third_finger_is_ignored() {
    Timeline line;
    line.down(1, 100.0f, 100.0f, 0);
    line.down(2, 200.0f, 100.0f, 10);
    OA_CHECK(line.down(3, 300.0f, 100.0f, 20).empty());
    OA_CHECK(line.recogniser.fingers_down() == 2);
    OA_CHECK(line.move(3, 400.0f, 300.0f, 30).empty());
    OA_CHECK(line.up(3, 400.0f, 300.0f, 40).empty());
    OA_CHECK(line.recogniser.fingers_down() == 2);
    OA_CHECK(line.all.size() == 1);
}

/// Checks that a cancelled finger gives cancelled and drops every finger.
void a_cancelled_finger_drops_the_sequence() {
    Timeline line;
    line.down(1, 100.0f, 100.0f, 0);
    line.move(1, 130.0f, 100.0f, 20);
    const auto cancelled = line.cancel(1, 30);
    OA_CHECK(kinds_are(cancelled, {GestureKind::cancelled}));
    OA_CHECK(line.recogniser.fingers_down() == 0);
    OA_CHECK(!line.recogniser.dragging());
    OA_CHECK(line.move(1, 160.0f, 100.0f, 40).empty());
    OA_CHECK(line.up(1, 160.0f, 100.0f, 50).empty());
    OA_CHECK(line.count(GestureKind::drag_ended) == 0);
    // A cancel during a pair drops both.
    line.down(1, 100.0f, 100.0f, 100);
    line.down(2, 200.0f, 100.0f, 110);
    OA_CHECK(kinds_are(line.cancel(2, 120), {GestureKind::cancelled}));
    OA_CHECK(line.recogniser.fingers_down() == 0);
    OA_CHECK(line.up(1, 100.0f, 100.0f, 130).empty());
    // A cancel of a finger never tracked gives nothing.
    OA_CHECK(line.cancel(9, 140).empty());
}

/// Checks that reset is silent and leaves no finger.
void reset_is_silent() {
    Timeline line;
    line.down(1, 100.0f, 100.0f, 0);
    line.down(2, 200.0f, 100.0f, 10);
    line.recogniser.reset();
    OA_CHECK(line.recogniser.fingers_down() == 0);
    OA_CHECK(line.advance(1000).empty());
    OA_CHECK(line.up(1, 100.0f, 100.0f, 1100).empty());
    OA_CHECK(line.up(2, 200.0f, 100.0f, 1100).empty());
    OA_CHECK(line.all.size() == 1);
    // The next finger starts afresh, on a clock that may restart.
    OA_CHECK(kinds_are(line.down(1, 10.0f, 10.0f, 0), {GestureKind::press}));
    OA_CHECK(kinds_are(line.up(1, 10.0f, 10.0f, 50), {GestureKind::tap}));
}

/// Checks a twist: a turn past the threshold, and none from a slide, a spread, or while off.
void a_turn_of_two_fingers_is_a_twist() {
    gestures::Thresholds thresholds{};
    thresholds.twist_degrees = 8.0f;
    Timeline line;
    line.recogniser.set_thresholds(thresholds);
    line.down(1, 100.0f, 100.0f, 0);
    line.down(2, 200.0f, 100.0f, 10);
    // One finger first, so the turn is read once both have reported. A quarter
    // turn about the centroid is 90 degrees.
    line.move(1, 150.0f, 50.0f, 20);
    const auto turned = line.move(2, 150.0f, 150.0f, 30);
    bool saw_began = false;
    float turn = 0.0f;
    for (const auto& gesture : turned) {
        if (gesture.kind == GestureKind::twist_began)
            saw_began = true;
        if (gesture.kind == GestureKind::twist_moved)
            turn += gesture.turn;
    }
    OA_CHECK(saw_began);
    OA_CHECK(near(turn, 90.0f, 1.0f));
    const auto ended = line.up(2, 150.0f, 150.0f, 40);
    bool saw_ended = false;
    for (const auto& gesture : ended)
        saw_ended = saw_ended || gesture.kind == GestureKind::twist_ended;
    OA_CHECK(saw_ended);
    line.up(1, 150.0f, 50.0f, 50);
    OA_CHECK(line.count(GestureKind::two_finger_tap) == 0);

    Timeline slide;
    slide.recogniser.set_thresholds(thresholds);
    slide.down(1, 100.0f, 100.0f, 0);
    slide.down(2, 200.0f, 100.0f, 10);
    slide.move(1, 140.0f, 100.0f, 20);
    slide.move(2, 240.0f, 100.0f, 30);
    OA_CHECK(slide.count(GestureKind::twist_began) == 0);

    Timeline spread;
    spread.recogniser.set_thresholds(thresholds);
    spread.down(1, 100.0f, 100.0f, 0);
    spread.down(2, 200.0f, 100.0f, 10);
    spread.move(1, 50.0f, 100.0f, 20);
    spread.move(2, 250.0f, 100.0f, 30);
    OA_CHECK(spread.count(GestureKind::twist_began) == 0);

    Timeline off;
    off.down(1, 100.0f, 100.0f, 0);
    off.down(2, 200.0f, 100.0f, 10);
    off.move(1, 150.0f, 50.0f, 20);
    off.move(2, 150.0f, 150.0f, 30);
    OA_CHECK(off.count(GestureKind::twist_began) == 0);
}

/// Checks that a finger put back during a pan resumes it without a two-finger tap.
void a_finger_put_back_resumes_the_pair() {
    Timeline line;
    line.down(1, 100.0f, 100.0f, 0);
    line.down(2, 200.0f, 100.0f, 10);
    line.up(1, 100.0f, 100.0f, 50);
    OA_CHECK(line.down(1, 100.0f, 100.0f, 60).empty());
    OA_CHECK(line.recogniser.fingers_down() == 2);
    line.up(1, 100.0f, 100.0f, 70);
    OA_CHECK(line.up(2, 200.0f, 100.0f, 80).empty());
    OA_CHECK(line.count(GestureKind::two_finger_tap) == 0);
}

} // namespace

int main() {
    batch_keeps_its_capacity();
    thresholds_are_kept();
    a_quick_press_is_a_tap();
    a_second_quick_tap_nearby_is_a_double_tap();
    travel_past_the_slop_begins_a_drag();
    resting_for_the_hold_delay_is_a_hold();
    two_quick_fingers_are_a_two_finger_tap();
    two_fingers_moving_together_pan();
    two_fingers_spreading_pinch();
    pan_and_pinch_run_together();
    a_turn_of_two_fingers_is_a_twist();
    a_fast_pan_is_no_pinch();
    a_second_finger_during_a_drag_cancels_it();
    a_third_finger_is_ignored();
    a_cancelled_finger_drops_the_sequence();
    reset_is_silent();
    a_finger_put_back_resumes_the_pair();
    return oa::test::check_exit_status();
}
