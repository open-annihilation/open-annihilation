// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The gamepad controls' model: timelines of pad input and what each piece
// gives (the pad pointer, the gyro, the stick cursor, triggers, holds, menu
// repeat, double clicks, wedge aim), the maps from buttons to actions in
// every layer and map, the chords prompts name, the effective scheme, the
// glyphs and the feels.
#include "oa/test/check.hpp"
#include "oa/ui/pad_controls.hpp"
#include "oa/ui/touch_hud.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <utility>
#include <vector>

namespace {

namespace pad = oa::ui::pad_controls;
using pad::Action;
using pad::Layer;
using pad::PadButton;

/// Nanoseconds in a millisecond.
constexpr uint64_t ms = 1'000'000;
/// The canvas the pointer tests use: a Deck's screen.
constexpr pad::Vec2 deck_canvas{1280.0f, 800.0f};
/// Canvas pixels a pad width moves at speed 100 % and ×1: half the canvas's width.
constexpr float pixels_per_pad = 640.0f;
/// How often the timelines' trackpad samples come, in milliseconds.
constexpr uint64_t sample_ms = 10;

/// Returns whether two floats agree within a tolerance.
///
/// @param a one value
/// @param b the other
/// @param tolerance the largest difference allowed
/// @return whether they agree
bool near(float a, float b, float tolerance = 0.01f) {
    return std::fabs(a - b) <= tolerance;
}

/// Returns settings with the pointer's choices.
///
/// @param speed Pointer speed, percent
/// @param acceleration Pointer acceleration
/// @param glide Trackpad glide
/// @return the settings
pad::PadSettings pointer_settings(uint32_t speed, pad::Acceleration acceleration, bool glide) {
    pad::PadSettings settings{};
    settings.pointer_speed = speed;
    settings.acceleration = acceleration;
    settings.glide = glide;
    return settings;
}

/// A thumb on a pad pointer, sampled every sample_ms.
struct Thumb {
    pad::PadPointer pointer;
    pad::Vec2 at{};    ///< the thumb's pad point
    uint64_t now_ms{}; ///< the timeline's clock
    pad::Vec2 moved{}; ///< every move given since the last landing
    uint32_t ticks{};  ///< every tick given

    /// Lands the thumb.
    void land(pad::Vec2 point) {
        at = point;
        moved = {};
        pointer.touch_down(point, now_ms * ms);
    }

    /// Moves the thumb by a pad delta after sample_ms and returns the step.
    pad::PointerStep slide(pad::Vec2 delta) {
        now_ms += sample_ms;
        at = {at.x + delta.x, at.y + delta.y};
        return keep(pointer.touch_move(at, now_ms * ms));
    }

    /// Moves the thumb at a speed across for a time.
    void slide_for(float pads_per_second, uint64_t duration_ms) {
        const float step = pads_per_second * static_cast<float>(sample_ms) / 1000.0f;
        for (uint64_t elapsed = 0; elapsed < duration_ms; elapsed += sample_ms)
            slide({step, 0.0f});
    }

    /// Moves time on and returns the glide's step.
    pad::PointerStep advance(uint64_t step_ms) {
        now_ms += step_ms;
        return keep(pointer.advance(now_ms * ms));
    }

    /// Adds a step to the totals.
    pad::PointerStep keep(const pad::PointerStep& step) {
        moved.x += step.move.x;
        moved.y += step.move.y;
        ticks += step.ticks;
        return step;
    }
};

/// Returns the move one sample gives at a steady thumb speed across, after a slide at that
/// speed long enough to be past the landing.
///
/// @param settings the settings
/// @param pads_per_second the thumb's speed
/// @return the move across of one sample, canvas pixels
float steady_move(const pad::PadSettings& settings, float pads_per_second) {
    Thumb thumb;
    thumb.pointer.configure(settings, deck_canvas, {});
    thumb.land({0.1f, 0.5f});
    thumb.slide_for(pads_per_second, 50);
    const float step = pads_per_second * static_cast<float>(sample_ms) / 1000.0f;
    return thumb.slide({step, 0.0f}).move.x;
}

/// Checks acceleration Off, Low and High at a quarter, one and three pad widths a second.
void acceleration_follows_the_thumb_speed() {
    const auto gain = [](pad::Acceleration acceleration, float speed) {
        const float step = speed * static_cast<float>(sample_ms) / 1000.0f;
        return steady_move(pointer_settings(100, acceleration, false), speed) /
               (step * pixels_per_pad);
    };
    for (const auto acceleration :
         {pad::Acceleration::off, pad::Acceleration::low, pad::Acceleration::high})
        OA_CHECK(near(gain(acceleration, 0.25f), 1.0f));
    OA_CHECK(near(gain(pad::Acceleration::off, 1.0f), 1.0f));
    OA_CHECK(near(gain(pad::Acceleration::low, 1.0f), 1.3f));
    OA_CHECK(near(gain(pad::Acceleration::high, 1.0f), 1.6f));
    OA_CHECK(near(gain(pad::Acceleration::off, 3.0f), 1.0f));
    OA_CHECK(near(gain(pad::Acceleration::low, 3.0f), pad::low_acceleration_gain));
    OA_CHECK(near(gain(pad::Acceleration::high, 3.0f), pad::high_acceleration_gain));
    // Past the top the gain stays at its greatest.
    OA_CHECK(near(gain(pad::Acceleration::low, 5.0f), pad::low_acceleration_gain));
}

/// Checks that one pad width of slow motion moves half the canvas at speed 100 %, and that
/// Pointer speed scales it.
void pointer_speed_scales_the_gain() {
    Thumb thumb;
    thumb.pointer.configure(pointer_settings(100, pad::Acceleration::low, false), deck_canvas, {});
    thumb.land({0.0f, 0.5f});
    thumb.slide_for(0.25f, 4000);
    OA_CHECK(near(thumb.moved.x, deck_canvas.x / 2.0f, 0.5f));
    OA_CHECK(near(thumb.moved.y, 0.0f));
    const float at_100 = steady_move(pointer_settings(100, pad::Acceleration::low, false), 0.25f);
    const float at_50 = steady_move(pointer_settings(50, pad::Acceleration::low, false), 0.25f);
    const float at_300 = steady_move(pointer_settings(300, pad::Acceleration::low, false), 0.25f);
    OA_CHECK(near(at_100, 1.6f));
    OA_CHECK(near(at_50, at_100 / 2.0f));
    OA_CHECK(near(at_300, at_100 * 3.0f));
    // A speed outside the setting's range is held to it.
    const float at_1000 = steady_move(pointer_settings(1000, pad::Acceleration::low, false), 0.25f);
    OA_CHECK(near(at_1000, at_300));
}

/// Checks the landing dead band: tiny motion in the first 20 ms after landing moves nothing,
/// larger motion and later motion move.
void landing_ignores_a_nudge() {
    pad::PadPointer pointer;
    pointer.configure(pointer_settings(100, pad::Acceleration::off, false), deck_canvas, {});
    pointer.touch_down({0.5f, 0.5f}, 0);
    OA_CHECK(pointer.touched());
    OA_CHECK(near(pointer.touch_move({0.503f, 0.5f}, 10 * ms).move.x, 0.0f));
    // Past the band from the landing point: the whole travel counts.
    OA_CHECK(near(pointer.touch_move({0.506f, 0.5f}, 15 * ms).move.x, 0.006f * pixels_per_pad));
    // After 20 ms a motion smaller than the band moves.
    pointer.touch_down({0.5f, 0.5f}, 100 * ms);
    OA_CHECK(near(pointer.touch_move({0.503f, 0.5f}, 125 * ms).move.x, 0.003f * pixels_per_pad));
    OA_CHECK(near(pointer.touch().x, 0.503f, 0.0001f));
    pointer.touch_up(130 * ms);
    OA_CHECK(!pointer.touched());
    OA_CHECK(near(pointer.touch_move({0.6f, 0.5f}, 140 * ms).move.x, 0.0f));
}

/// Checks the click lock: for 40 ms after the pad's click goes down or comes up the pointer
/// holds still and the motion is dropped.
void click_lock_holds_the_pointer() {
    Thumb thumb;
    thumb.pointer.configure(pointer_settings(100, pad::Acceleration::off, false), deck_canvas, {});
    thumb.land({0.5f, 0.5f});
    thumb.slide_for(0.2f, 100);
    thumb.pointer.press(thumb.now_ms * ms);
    OA_CHECK(near(thumb.slide({0.01f, 0.0f}).move.x, 0.0f));
    OA_CHECK(near(thumb.slide({0.01f, 0.0f}).move.x, 0.0f));
    OA_CHECK(near(thumb.slide({0.01f, 0.0f}).move.x, 0.0f));
    // 40 ms after the press: the lock has ended; only the motion after it counts.
    OA_CHECK(near(thumb.slide({0.002f, 0.0f}).move.x, 0.002f * pixels_per_pad));
    thumb.slide_for(0.2f, 50);
    thumb.pointer.release(thumb.now_ms * ms);
    OA_CHECK(near(thumb.slide({0.01f, 0.0f}).move.x, 0.0f));
    thumb.now_ms += 40;
    OA_CHECK(near(thumb.slide({0.002f, 0.0f}).move.x, 0.002f * pixels_per_pad));
}

/// Checks glide: off by default; on, a flick coasts, slows with the time constant, stops under
/// 20 px a second, and a landing stops it; a thumb that rested before lifting does not glide.
void glide_coasts_and_stops() {
    // Off: nothing after the lift.
    {
        Thumb thumb;
        thumb.pointer.configure(
            pointer_settings(100, pad::Acceleration::off, false), deck_canvas, {}
        );
        thumb.land({0.1f, 0.5f});
        thumb.slide_for(2.0f, 100);
        thumb.pointer.touch_up(thumb.now_ms * ms);
        OA_CHECK(!thumb.pointer.gliding());
        OA_CHECK(near(thumb.advance(16).move.x, 0.0f));
    }
    Thumb thumb;
    thumb.pointer.configure(pointer_settings(100, pad::Acceleration::off, true), deck_canvas, {});
    thumb.land({0.1f, 0.5f});
    thumb.slide_for(2.0f, 100);
    thumb.pointer.touch_up(thumb.now_ms * ms);
    OA_CHECK(thumb.pointer.gliding());
    // At 2 pad widths a second and ×1 the pointer moved 1280 px a second when it lifted.
    const float speed = 2.0f * pixels_per_pad;
    const auto first = thumb.advance(16);
    const float expected = speed * pad::glide_time_constant_s * (1.0f - std::exp(-0.016f / 0.25f));
    OA_CHECK(near(first.move.x, expected, 0.05f));
    // It slows: each later step moves less.
    float last = first.move.x;
    for (int step = 0; step < 5; ++step) {
        const float moved = thumb.advance(16).move.x;
        OA_CHECK(moved < last);
        last = moved;
    }
    // It stops once under the stop speed: after τ·ln(speed / stop) seconds.
    const float stop_s = pad::glide_time_constant_s * std::log(speed / pad::glide_stop_px_per_s);
    thumb.moved = {};
    while (thumb.pointer.gliding() && thumb.now_ms < 5000)
        thumb.advance(16);
    OA_CHECK(near(static_cast<float>(thumb.now_ms - 100) / 1000.0f, stop_s, 0.05f));
    OA_CHECK(near(thumb.advance(16).move.x, 0.0f));
    // The whole glide is close to speed × τ.
    // A landing stops a glide.
    thumb.land({0.1f, 0.5f});
    thumb.slide_for(2.0f, 100);
    thumb.pointer.touch_up(thumb.now_ms * ms);
    OA_CHECK(thumb.pointer.gliding());
    thumb.advance(16);
    thumb.pointer.touch_down({0.5f, 0.5f}, thumb.now_ms * ms);
    OA_CHECK(!thumb.pointer.gliding());
    OA_CHECK(near(thumb.advance(16).move.x, 0.0f));
    // A thumb that rested before lifting does not glide.
    thumb.land({0.1f, 0.5f});
    thumb.slide_for(2.0f, 100);
    thumb.now_ms += pad::glide_rest_ms + 10;
    thumb.pointer.touch_up(thumb.now_ms * ms);
    OA_CHECK(!thumb.pointer.gliding());
    // stop() ends a glide too.
    thumb.land({0.1f, 0.5f});
    thumb.slide_for(2.0f, 100);
    thumb.pointer.touch_up(thumb.now_ms * ms);
    thumb.pointer.stop();
    OA_CHECK(!thumb.pointer.gliding() && !thumb.pointer.touched());
}

/// Checks the whole travel of a glide: the speed at the lift times the time constant, less what
/// the stop leaves.
void glide_travels_speed_times_tau() {
    Thumb thumb;
    thumb.pointer.configure(pointer_settings(100, pad::Acceleration::off, true), deck_canvas, {});
    thumb.land({0.1f, 0.5f});
    thumb.slide_for(2.0f, 100);
    thumb.pointer.touch_up(thumb.now_ms * ms);
    thumb.moved = {};
    while (thumb.pointer.gliding() && thumb.now_ms < 5000)
        thumb.advance(16);
    const float speed = 2.0f * pixels_per_pad;
    const float whole = (speed - pad::glide_stop_px_per_s) * pad::glide_time_constant_s;
    OA_CHECK(near(thumb.moved.x, whole, 6.0f));
}

/// Checks the pointer ticks: one each 32 px of travel, the remainder carried.
void ticks_count_the_travel() {
    pad::TickCounter counter;
    OA_CHECK(counter.add(20.0f) == 0);
    OA_CHECK(counter.add(20.0f) == 1);
    OA_CHECK(counter.add(23.0f) == 0);
    OA_CHECK(counter.add(1.0f) == 1);
    OA_CHECK(counter.add(96.0f) == 3);
    counter.reset();
    OA_CHECK(counter.add(31.0f) == 0);
    OA_CHECK(counter.add(100000.0f) == 255);
    // On the pointer: 10 px a sample for 32 samples is 320 px, 10 ticks.
    Thumb thumb;
    thumb.pointer.configure(pointer_settings(100, pad::Acceleration::off, false), deck_canvas, {});
    thumb.land({0.1f, 0.5f});
    for (int sample = 0; sample < 3; ++sample)
        thumb.slide({0.0f, 0.0f});
    thumb.ticks = 0;
    for (int sample = 0; sample < 32; ++sample)
        thumb.slide({10.0f / pixels_per_pad, 0.0f});
    OA_CHECK(thumb.ticks == 10);
}

/// Checks the absolute mapping: an area fitted to the middle of the square pad with its own
/// shape, the pad's edges beyond it held to the area's.
void absolute_maps_the_pad_onto_an_area() {
    const pad::Area wide{100.0f, 50.0f, 1000.0f, 500.0f};
    auto point = pad::pad_to_area({0.5f, 0.5f}, wide);
    OA_CHECK(near(point.x, 600.0f) && near(point.y, 300.0f));
    point = pad::pad_to_area({0.0f, 0.25f}, wide);
    OA_CHECK(near(point.x, 100.0f) && near(point.y, 50.0f));
    point = pad::pad_to_area({1.0f, 0.75f}, wide);
    OA_CHECK(near(point.x, 1100.0f) && near(point.y, 550.0f));
    point = pad::pad_to_area({0.5f, 0.05f}, wide);
    OA_CHECK(near(point.x, 600.0f) && near(point.y, 50.0f));
    point = pad::pad_to_area({0.75f, 0.5f}, wide);
    OA_CHECK(near(point.x, 850.0f) && near(point.y, 300.0f));
    const pad::Area tall{0.0f, 0.0f, 400.0f, 800.0f};
    point = pad::pad_to_area({0.25f, 0.0f}, tall);
    OA_CHECK(near(point.x, 0.0f) && near(point.y, 0.0f));
    point = pad::pad_to_area({0.5f, 1.0f}, tall);
    OA_CHECK(near(point.x, 200.0f) && near(point.y, 800.0f));
    point = pad::pad_to_area({0.0f, 0.5f}, tall);
    OA_CHECK(near(point.x, 0.0f) && near(point.y, 400.0f));
    // An empty area gives its corner.
    point = pad::pad_to_area({0.5f, 0.5f}, {10.0f, 20.0f, 0.0f, 0.0f});
    OA_CHECK(near(point.x, 10.0f) && near(point.y, 20.0f));
    // The pad pointer in the absolute mode places the pointer.
    pad::PadSettings settings{};
    settings.right_trackpad = pad::RightTrackpad::absolute;
    pad::PadPointer pointer;
    pointer.configure(settings, deck_canvas, wide);
    pointer.touch_down({0.5f, 0.5f}, 0);
    auto step = pointer.touch_move({0.5f, 0.5f}, 0);
    OA_CHECK(step.place.has_value() && near(step.place->x, 600.0f) && near(step.place->y, 300.0f));
    // The landing band holds the place; later motion places anew.
    step = pointer.touch_move({0.502f, 0.5f}, 10 * ms);
    OA_CHECK(step.place.has_value() && near(step.place->x, 600.0f));
    step = pointer.touch_move({0.75f, 0.5f}, 30 * ms);
    OA_CHECK(step.place.has_value() && near(step.place->x, 850.0f) && near(step.place->y, 300.0f));
    OA_CHECK(step.ticks == 7);
    // The click lock holds the place too.
    pointer.press(40 * ms);
    step = pointer.touch_move({0.9f, 0.5f}, 50 * ms);
    OA_CHECK(step.place.has_value() && near(step.place->x, 850.0f));
    // No glide in the absolute mode.
    settings.glide = true;
    pointer.configure(settings, deck_canvas, wide);
    pointer.touch_up(60 * ms);
    OA_CHECK(!pointer.gliding());
}

/// Checks the gyro pointer: half the canvas a radian at speed 100 %, the gate, Off and the
/// first call, which only starts the clock.
void gyro_moves_by_turn() {
    pad::PadSettings settings{};
    settings.gyro = pad::Gyro::always;
    pad::GyroPointer gyro;
    gyro.configure(settings, deck_canvas);
    OA_CHECK(near(gyro.advance(0.0f, 1.0f, true, 1000 * ms).move.x, 0.0f));
    // A radian a second of yaw for 100 ms: a tenth of half the canvas, to the left.
    auto step = gyro.advance(0.0f, 1.0f, true, 1100 * ms);
    OA_CHECK(near(step.move.x, -64.0f) && near(step.move.y, 0.0f));
    OA_CHECK(step.ticks == 2);
    step = gyro.advance(0.5f, 0.0f, true, 1150 * ms);
    OA_CHECK(near(step.move.x, 0.0f) && near(step.move.y, -16.0f));
    // Without the gate nothing moves, and the time still passes.
    step = gyro.advance(1.0f, 1.0f, false, 1200 * ms);
    OA_CHECK(near(step.move.x, 0.0f) && near(step.move.y, 0.0f));
    step = gyro.advance(0.0f, -1.0f, true, 1210 * ms);
    OA_CHECK(near(step.move.x, 6.4f));
    // A long gap counts as the longest step.
    step = gyro.advance(0.0f, -1.0f, true, 5000 * ms);
    OA_CHECK(near(step.move.x, 64.0f));
    // Gyro speed doubles it.
    settings.gyro_speed = 200;
    gyro.configure(settings, deck_canvas);
    step = gyro.advance(0.0f, -1.0f, true, 5010 * ms);
    OA_CHECK(near(step.move.x, 12.8f));
    // Off gives nothing even with the gate.
    settings.gyro = pad::Gyro::off;
    gyro.configure(settings, deck_canvas);
    step = gyro.advance(1.0f, 1.0f, true, 5020 * ms);
    OA_CHECK(near(step.move.x, 0.0f) && near(step.move.y, 0.0f));
    gyro.reset();
    settings.gyro = pad::Gyro::right_pad_touched;
    gyro.configure(settings, deck_canvas);
    OA_CHECK(near(gyro.advance(0.0f, 1.0f, true, 6000 * ms).move.x, 0.0f));
}

/// Checks a trigger read as a button: down at 30 %, up below 20 %.
void trigger_has_hysteresis() {
    pad::TriggerButton trigger;
    OA_CHECK(!trigger.update(0.29f));
    OA_CHECK(trigger.update(0.30f));
    OA_CHECK(trigger.update(0.25f));
    OA_CHECK(trigger.update(0.20f));
    OA_CHECK(!trigger.update(0.19f));
    OA_CHECK(!trigger.down());
    OA_CHECK(!trigger.update(0.25f));
    OA_CHECK(trigger.update(1.0f) && trigger.down());
}

/// Checks a hold timer: tap, hold, used, cancel and progress.
void hold_timer_tells_tap_from_hold() {
    pad::HoldTimer timer;
    OA_CHECK(timer.release(0, 350) == pad::HoldEvent::none);
    timer.press(1000);
    OA_CHECK(timer.down() && !timer.holding() && !timer.used());
    OA_CHECK(timer.advance(1349, 350) == pad::HoldEvent::none);
    OA_CHECK(timer.release(1100, 350) == pad::HoldEvent::tap);
    OA_CHECK(!timer.down());
    // A hold.
    timer.press(2000);
    OA_CHECK(near(timer.progress(2175, 350), 0.5f));
    OA_CHECK(timer.advance(2349, 350) == pad::HoldEvent::none);
    OA_CHECK(timer.advance(2350, 350) == pad::HoldEvent::hold_started);
    OA_CHECK(timer.holding());
    OA_CHECK(timer.advance(2400, 350) == pad::HoldEvent::none);
    OA_CHECK(near(timer.progress(2400, 350), 1.0f));
    OA_CHECK(timer.release(2500, 350) == pad::HoldEvent::hold_ended);
    OA_CHECK(near(timer.progress(2600, 350), 0.0f));
    // A press another action used is neither.
    timer.press(3000);
    timer.use();
    OA_CHECK(timer.used());
    OA_CHECK(near(timer.progress(3100, 350), 0.0f));
    OA_CHECK(timer.advance(3400, 350) == pad::HoldEvent::none);
    OA_CHECK(timer.release(3500, 350) == pad::HoldEvent::none);
    // Used after the hold started still ends the hold.
    timer.press(4000);
    OA_CHECK(timer.advance(4400, 350) == pad::HoldEvent::hold_started);
    timer.use();
    OA_CHECK(timer.release(4500, 350) == pad::HoldEvent::hold_ended);
    // A release past the delay with no advance is a hold, not a tap.
    timer.press(5000);
    OA_CHECK(timer.release(5400, 350) == pad::HoldEvent::hold_ended);
    // Cancel forgets the press.
    timer.press(6000);
    timer.cancel();
    OA_CHECK(!timer.down());
    OA_CHECK(timer.release(6100, 350) == pad::HoldEvent::none);
    // The self-destruct hold's own delay.
    timer.press(7000);
    OA_CHECK(timer.advance(7999, pad::self_destruct_hold_ms) == pad::HoldEvent::none);
    OA_CHECK(near(timer.progress(7500, pad::self_destruct_hold_ms), 0.5f));
    OA_CHECK(timer.advance(8000, pad::self_destruct_hold_ms) == pad::HoldEvent::hold_started);
}

/// Checks menu repeat: a step at once, again at 400 ms, then every 120 ms.
void menu_repeat_steps_on_time() {
    pad::MenuRepeat repeat;
    OA_CHECK(repeat.advance(0) == 0);
    OA_CHECK(repeat.press(0) == 1);
    OA_CHECK(repeat.advance(100) == 0);
    OA_CHECK(repeat.advance(399) == 0);
    OA_CHECK(repeat.advance(400) == 1);
    OA_CHECK(repeat.advance(519) == 0);
    OA_CHECK(repeat.advance(520) == 1);
    OA_CHECK(repeat.advance(640) == 1);
    // A late frame catches up, never past the most steps.
    OA_CHECK(repeat.advance(880) == 2);
    OA_CHECK(repeat.advance(5000) == pad::menu_repeat_most_steps);
    OA_CHECK(repeat.advance(5001) == 0);
    repeat.release();
    OA_CHECK(repeat.advance(10000) == 0);
    OA_CHECK(repeat.press(20000) == 1);
    OA_CHECK(repeat.advance(20400) == 1);
}

/// Checks double clicks: two presses within 300 ms and 12 points; a third counts 1.
void double_click_needs_time_and_place() {
    pad::DoubleClick clicks;
    OA_CHECK(clicks.press(1000, {100.0f, 100.0f}, 1.0f) == 1);
    OA_CHECK(clicks.press(1300, {110.0f, 100.0f}, 1.0f) == 2);
    OA_CHECK(clicks.press(1400, {110.0f, 100.0f}, 1.0f) == 1);
    OA_CHECK(clicks.press(1701, {110.0f, 100.0f}, 1.0f) == 1);
    OA_CHECK(clicks.press(1800, {123.0f, 100.0f}, 1.0f) == 1);
    // 12 points at 2 pixels a point is 24 pixels.
    OA_CHECK(clicks.press(1900, {143.0f, 100.0f}, 2.0f) == 2);
    OA_CHECK(clicks.press(3000, {0.0f, 0.0f}, 1.0f) == 1);
    clicks.reset();
    OA_CHECK(clicks.press(3100, {0.0f, 0.0f}, 1.0f) == 1);
}

/// Returns the offset of an angle clockwise from the top, at a length.
///
/// @param degrees the angle
/// @param length the length
/// @return the offset, y down
pad::Vec2 at_angle(float degrees, float length = 1.0f) {
    const double radians = static_cast<double>(degrees) * 3.14159265358979323846 / 180.0;
    return {
        static_cast<float>(std::sin(radians)) * length,
        -static_cast<float>(std::cos(radians)) * length
    };
}

/// Checks wedge aim: slots clockwise from the top, the dead zone and the 8° hysteresis.
void wedge_aim_picks_with_hysteresis() {
    pad::WedgeAim aim;
    aim.configure(12);
    OA_CHECK(!aim.slot());
    OA_CHECK(aim.aim({0.0f, -1.0f}, pad::pad_ring_dead_zone) == 0);
    OA_CHECK(aim.aim({1.0f, 0.0f}, pad::pad_ring_dead_zone) == 3);
    OA_CHECK(aim.aim({0.0f, 1.0f}, pad::pad_ring_dead_zone) == 6);
    OA_CHECK(aim.aim({-1.0f, 0.0f}, pad::pad_ring_dead_zone) == 9);
    OA_CHECK(!aim.aim({0.1f, 0.0f}, pad::pad_ring_dead_zone));
    OA_CHECK(!aim.slot());
    // Slot 0 spans ±15°; it is left only past 23°.
    OA_CHECK(aim.aim(at_angle(0.0f), pad::pad_ring_dead_zone) == 0);
    OA_CHECK(aim.aim(at_angle(20.0f), pad::pad_ring_dead_zone) == 0);
    OA_CHECK(aim.aim(at_angle(22.0f), pad::pad_ring_dead_zone) == 0);
    OA_CHECK(aim.aim(at_angle(24.0f), pad::pad_ring_dead_zone) == 1);
    OA_CHECK(aim.aim(at_angle(8.0f), pad::pad_ring_dead_zone) == 1);
    OA_CHECK(aim.aim(at_angle(6.0f), pad::pad_ring_dead_zone) == 0);
    OA_CHECK(aim.aim(at_angle(340.0f), pad::pad_ring_dead_zone) == 0);
    OA_CHECK(aim.aim(at_angle(336.0f), pad::pad_ring_dead_zone) == 11);
    // Without a slot the nearest wedge is taken at once.
    aim.reset();
    OA_CHECK(aim.aim(at_angle(16.0f), pad::pad_ring_dead_zone) == 1);
    // The stick's dead zone is half deflection.
    aim.reset();
    OA_CHECK(!aim.aim(at_angle(90.0f, 0.4f), pad::stick_ring_dead_zone));
    OA_CHECK(aim.aim(at_angle(90.0f, 0.6f), pad::stick_ring_dead_zone) == 3);
    // The build ring's 8 and the group ring's 9.
    aim.configure(8);
    OA_CHECK(!aim.slot());
    OA_CHECK(aim.aim(at_angle(90.0f), pad::pad_ring_dead_zone) == 2);
    OA_CHECK(aim.aim(at_angle(270.0f), pad::pad_ring_dead_zone) == 6);
    OA_CHECK(aim.aim(at_angle(315.0f), pad::pad_ring_dead_zone) == 7);
    aim.configure(9);
    OA_CHECK(aim.aim(at_angle(40.0f), pad::pad_ring_dead_zone) == 1);
    OA_CHECK(aim.aim(at_angle(67.0f), pad::pad_ring_dead_zone) == 1);
    OA_CHECK(aim.aim(at_angle(69.0f), pad::pad_ring_dead_zone) == 2);
}

/// Checks that wedge_point lies in its wedge for the aim and for the touch radial's hit test.
void wedge_point_hits_the_same_slot() {
    for (const uint8_t slots : {uint8_t{8}, uint8_t{9}, uint8_t{12}})
        for (uint8_t slot = 0; slot < slots; ++slot) {
            const pad::Vec2 centre{400.0f, 300.0f};
            const auto point = pad::wedge_point(centre, 44.0f, 128.0f, slot, slots);
            OA_CHECK(near(std::hypot(point.x - centre.x, point.y - centre.y), 86.0f));
            pad::WedgeAim aim;
            aim.configure(slots);
            const pad::Vec2 offset{(point.x - centre.x) / 128.0f, (point.y - centre.y) / 128.0f};
            OA_CHECK(aim.aim(offset, pad::pad_ring_dead_zone) == slot);
        }
    // The touch radial picks the wedge whose point it is given.
    namespace hud = oa::ui::touch_hud;
    hud::Radial radial{};
    radial.centre = {640, 400};
    radial.inner_radius = 44;
    radial.outer_radius = 128;
    for (std::size_t slot = 0; slot < hud::radial_slot_count; ++slot)
        radial.wedges[slot].item = static_cast<hud::RadialItem>(slot < 4 ? slot : slot + 1);
    const hud::Viewport viewport{};
    for (uint8_t slot = 0; slot < hud::radial_slot_count; ++slot) {
        const auto point = pad::wedge_point(
            {640.0f, 400.0f},
            static_cast<float>(radial.inner_radius),
            static_cast<float>(radial.outer_radius),
            slot,
            static_cast<uint8_t>(hud::radial_slot_count)
        );
        const auto hit = hud::radial_hit(
            radial,
            {static_cast<int>(std::lround(point.x)), static_cast<int>(std::lround(point.y))},
            viewport
        );
        OA_CHECK(hit.has_value() && *hit == radial.wedges[slot].item);
    }
    OA_CHECK(near(pad::wedge_point({5.0f, 6.0f}, 1.0f, 2.0f, 0, 0).x, 5.0f));
}

/// Checks the stick cursor's dead zones and curve.
void stick_cursor_follows_the_curve() {
    OA_CHECK(near(pad::stick_deflection({0.1f, 0.0f}), 0.0f));
    OA_CHECK(near(pad::stick_deflection({0.12f, 0.0f}), 0.0f));
    OA_CHECK(near(pad::stick_deflection({0.96f, 0.0f}), 1.0f));
    OA_CHECK(near(pad::stick_deflection({0.0f, -0.535f}), 0.5f));
    pad::StickCursor cursor;
    cursor.configure(pad::PadSettings{}, 1.0f);
    const std::vector<pad::Vec2> none;
    OA_CHECK(near(cursor.advance({0.5f, 0.0f}, {}, none, false, 1000 * ms).move.x, 0.0f));
    auto step = cursor.advance({0.5f, 0.0f}, {}, none, false, 1100 * ms);
    const float deflection = (0.5f - 0.12f) / (0.95f - 0.12f);
    OA_CHECK(near(step.move.x, 1200.0f * std::pow(deflection, 2.2f) * 0.1f, 0.05f));
    OA_CHECK(near(step.move.y, 0.0f));
    // A stick within the inner dead zone moves nothing.
    step = cursor.advance({0.1f, 0.05f}, {}, none, false, 1200 * ms);
    OA_CHECK(near(step.move.x, 0.0f) && near(step.move.y, 0.0f));
    // Full deflection up: 1200 points a second, at 2 pixels a point.
    cursor.configure(pad::PadSettings{}, 2.0f);
    step = cursor.advance({0.0f, -1.0f}, {}, none, false, 1250 * ms);
    OA_CHECK(near(step.move.y, -1200.0f * 2.0f * 0.05f, 0.05f) && near(step.move.x, 0.0f));
    // A diagonal keeps its direction.
    step = cursor.advance({0.6f, 0.6f}, {}, none, false, 1260 * ms);
    OA_CHECK(near(step.move.x, step.move.y, 0.001f) && step.move.x > 0.0f);
}

/// Checks the ramp: after 0.4 s at 90 % or more the speed rises to ×1.8 over 0.3 s.
void stick_cursor_ramps_when_held_full() {
    pad::StickCursor cursor;
    cursor.configure(pad::PadSettings{}, 1.0f);
    const std::vector<pad::Vec2> none;
    uint64_t now_ms = 1000;
    cursor.advance({1.0f, 0.0f}, {}, none, false, now_ms * ms);
    const auto speed_at = [&](uint64_t until_ms) {
        float moved = 0.0f;
        while (now_ms < until_ms) {
            now_ms += 10;
            moved = cursor.advance({1.0f, 0.0f}, {}, none, false, now_ms * ms).move.x;
        }
        return moved / 0.01f;
    };
    OA_CHECK(near(speed_at(1400), 1200.0f, 1.0f));
    OA_CHECK(near(speed_at(1550), 1200.0f * 1.4f, 1.0f));
    OA_CHECK(near(speed_at(1700), 1200.0f * 1.8f, 1.0f));
    OA_CHECK(near(speed_at(2500), 1200.0f * 1.8f, 1.0f));
    // Under 90 % the ramp starts over.
    now_ms += 10;
    cursor.advance({0.85f, 0.0f}, {}, none, false, now_ms * ms);
    OA_CHECK(near(speed_at(now_ms + 100), 1200.0f, 1.0f));
}

/// Checks friction: within 24 points of a target the speed is ×0.45, unless blocked.
void stick_cursor_slows_near_a_target() {
    pad::StickCursor cursor;
    cursor.configure(pad::PadSettings{}, 2.0f);
    const std::vector<pad::Vec2> target{{140.0f, 100.0f}};
    cursor.advance({1.0f, 0.0f}, {100.0f, 100.0f}, target, false, 1000 * ms);
    auto step = cursor.advance({1.0f, 0.0f}, {100.0f, 100.0f}, target, false, 1010 * ms);
    OA_CHECK(near(step.move.x, 1200.0f * 2.0f * 0.01f * 0.45f, 0.01f));
    step = cursor.advance({1.0f, 0.0f}, {100.0f, 100.0f}, target, true, 1020 * ms);
    OA_CHECK(near(step.move.x, 1200.0f * 2.0f * 0.01f, 0.01f));
    // 50 pixels off at 2 pixels a point is 25 points: no friction.
    step = cursor.advance({1.0f, 0.0f}, {90.0f, 100.0f}, target, false, 1030 * ms);
    OA_CHECK(near(step.move.x, 1200.0f * 2.0f * 0.01f, 0.01f));
}

/// Moves a stick cursor, then lets the stick rest, and returns where the pointer goes in the
/// next ms_after milliseconds.
///
/// @param settings the settings
/// @param targets the targets
/// @param blocked the block while resting
/// @param ms_after how long the stick rests
/// @return the pointer, from 100,100 at rest
pad::Vec2 rest_near(
    const pad::PadSettings& settings,
    const std::vector<pad::Vec2>& targets,
    bool blocked,
    uint64_t ms_after
) {
    pad::StickCursor cursor;
    cursor.configure(settings, 1.0f);
    pad::Vec2 pointer{100.0f, 100.0f};
    cursor.advance({0.5f, 0.0f}, pointer, targets, false, 1000 * ms);
    // The stick comes back to rest at once: the pointer stays at 100,100 until magnetism.
    for (uint64_t elapsed = 0; elapsed <= ms_after; elapsed += 10) {
        const auto step =
            cursor.advance({0.0f, 0.0f}, pointer, targets, blocked, (1000 + elapsed) * ms);
        pointer.x += step.move.x;
        pointer.y += step.move.y;
    }
    return pointer;
}

/// Checks magnetism: a lone target within 16 points draws the resting pointer in over 80 ms;
/// a rival within 8 points of it, a block, a farther target or Magnetism off stop it.
void stick_cursor_magnetism() {
    const pad::PadSettings on{};
    auto pointer = rest_near(on, {{110.0f, 100.0f}}, false, 40);
    OA_CHECK(near(pointer.x, 105.0f, 0.1f));
    pointer = rest_near(on, {{110.0f, 100.0f}}, false, 80);
    OA_CHECK(near(pointer.x, 110.0f, 0.01f) && near(pointer.y, 100.0f, 0.01f));
    pointer = rest_near(on, {{110.0f, 100.0f}}, false, 200);
    OA_CHECK(near(pointer.x, 110.0f, 0.01f));
    // The nearest of two far apart.
    pointer = rest_near(on, {{110.0f, 100.0f}, {85.0f, 100.0f}}, false, 100);
    OA_CHECK(near(pointer.x, 110.0f, 0.01f));
    // A rival within 8 points of the target: no guessing.
    pointer = rest_near(on, {{110.0f, 100.0f}, {116.0f, 100.0f}}, false, 100);
    OA_CHECK(near(pointer.x, 100.0f, 0.01f));
    // Blocked (R2 held, placing, over the HUD).
    pointer = rest_near(on, {{110.0f, 100.0f}}, true, 100);
    OA_CHECK(near(pointer.x, 100.0f, 0.01f));
    // Farther than 16 points.
    pointer = rest_near(on, {{117.0f, 100.0f}}, false, 100);
    OA_CHECK(near(pointer.x, 100.0f, 0.01f));
    // Off.
    pad::PadSettings off{};
    off.magnetism = false;
    pointer = rest_near(off, {{110.0f, 100.0f}}, false, 100);
    OA_CHECK(near(pointer.x, 100.0f, 0.01f));
    // A stick that never moved draws nothing in; moving again ends an ease.
    pad::StickCursor cursor;
    cursor.configure(on, 1.0f);
    const std::vector<pad::Vec2> target{{110.0f, 100.0f}};
    cursor.advance({0.0f, 0.0f}, {100.0f, 100.0f}, target, false, 1000 * ms);
    OA_CHECK(!cursor.easing());
    OA_CHECK(
        near(cursor.advance({0.0f, 0.0f}, {100.0f, 100.0f}, target, false, 1050 * ms).move.x, 0.0f)
    );
    cursor.advance({0.5f, 0.0f}, {100.0f, 100.0f}, target, false, 1060 * ms);
    cursor.advance({0.0f, 0.0f}, {100.0f, 100.0f}, target, false, 1070 * ms);
    OA_CHECK(cursor.easing());
    cursor.advance({0.0f, 0.5f}, {100.0f, 100.0f}, target, false, 1080 * ms);
    OA_CHECK(!cursor.easing());
    cursor.reset();
    OA_CHECK(!cursor.easing());
}

/// Checks flicks: past 0.7 one way gives it once, until the stick comes back under 0.3.
void flicks_fire_once() {
    pad::FlickDetector flick;
    OA_CHECK(flick.update({0.5f, 0.0f}) == pad::Flick::none);
    OA_CHECK(flick.update({0.8f, 0.1f}) == pad::Flick::right);
    OA_CHECK(flick.update({0.95f, 0.0f}) == pad::Flick::none);
    OA_CHECK(flick.update({0.4f, 0.0f}) == pad::Flick::none);
    OA_CHECK(flick.update({-0.8f, 0.0f}) == pad::Flick::none);
    OA_CHECK(flick.update({0.1f, 0.1f}) == pad::Flick::none);
    OA_CHECK(flick.update({-0.8f, 0.0f}) == pad::Flick::left);
    OA_CHECK(flick.update({0.0f, 0.0f}) == pad::Flick::none);
    OA_CHECK(flick.update({0.1f, -0.9f}) == pad::Flick::up);
    flick.reset();
    OA_CHECK(flick.update({0.0f, 0.75f}) == pad::Flick::down);
}

/// One row of a binding table: a button and what it does.
struct Row {
    PadButton button{PadButton::none};
    pad::Binding hold{};
    pad::Binding tap{};
};

/// Returns a row of a button that does one thing.
Row row(PadButton button, Action action) {
    return {button, {action, 0, false}, {}};
}

/// Returns a row of a button with a tap and a hold.
Row tap_hold(PadButton button, Action tap, Action hold) {
    return {button, {hold, 0, true}, {tap, 0, false}};
}

/// Returns a row of a group's button.
Row group(PadButton button, uint8_t index) {
    return {button, {Action::group, index, false}, {}};
}

/// The buttons a table checks for none when it does not list them.
constexpr std::array<PadButton, 25> every_button{
    PadButton::none,
    PadButton::a,
    PadButton::b,
    PadButton::x,
    PadButton::y,
    PadButton::view,
    PadButton::menu,
    PadButton::l1,
    PadButton::r1,
    PadButton::l2,
    PadButton::r2,
    PadButton::l3,
    PadButton::r3,
    PadButton::l4,
    PadButton::l5,
    PadButton::r4,
    PadButton::r5,
    PadButton::dpad_up,
    PadButton::dpad_right,
    PadButton::dpad_down,
    PadButton::dpad_left,
    PadButton::left_pad,
    PadButton::right_pad,
    PadButton::left_stick_touch,
    PadButton::right_stick_touch,
};

/// Returns whether two bindings are the same.
bool same(const pad::Binding& a, const pad::Binding& b) {
    return a.action == b.action && a.index == b.index && a.on_hold == b.on_hold;
}

/// Checks a layer's whole table in a map: each listed button gives its binding and tap, every
/// other button none; and the left-handed mirror gives each row from the mirrored button.
///
/// @param ctx the map
/// @param layer the layer
/// @param rows the listed buttons
void check_table(pad::MapContext ctx, Layer layer, const std::vector<Row>& rows) {
    for (const bool left_handed : {false, true}) {
        ctx.left_handed = left_handed;
        for (const PadButton button : every_button) {
            Row expected{button};
            for (const Row& listed : rows)
                if (listed.button == button)
                    expected = listed;
            const PadButton pressed = left_handed ? pad::mirrored(button) : button;
            const bool hold_ok = same(pad::binding_for(ctx, layer, pressed), expected.hold);
            const bool tap_ok = same(pad::tap_binding_for(ctx, layer, pressed), expected.tap);
            OA_CHECK(hold_ok);
            OA_CHECK(tap_ok);
            if (!hold_ok || !tap_ok)
                std::fprintf(
                    stderr,
                    "  layer %d button %d left-handed %d scheme %d fallback %d\n",
                    static_cast<int>(layer),
                    static_cast<int>(button),
                    left_handed ? 1 : 0,
                    static_cast<int>(ctx.scheme),
                    ctx.fallback ? 1 : 0
                );
        }
    }
}

/// Returns a map.
pad::MapContext map_of(pad::Scheme scheme, bool fallback, bool trackpads) {
    pad::MapContext ctx{};
    ctx.scheme = scheme;
    ctx.fallback = fallback;
    ctx.trackpads = trackpads;
    return ctx;
}

/// Returns the grips' rows with grips: kept in every layer of the match.
std::vector<Row> grip_rows() {
    return {
        row(PadButton::r4, Action::queue),
        row(PadButton::l4, Action::add),
        row(PadButton::r5, Action::standing_layer),
        row(PadButton::l5, Action::groups_layer),
    };
}

/// Returns rows with the grips' rows added when the map has grips.
std::vector<Row> with_grips(std::vector<Row> rows, bool grips) {
    if (grips)
        for (const Row& grip : grip_rows())
            rows.push_back(grip);
    return rows;
}

/// Checks the base layer's table in the trackpads, sticks and fallback maps.
void base_tables() {
    const std::vector<Row> shared{
        row(PadButton::a, Action::left_button),
        row(PadButton::b, Action::clear),
        row(PadButton::x, Action::stop),
        row(PadButton::y, Action::select_type),
        row(PadButton::l2, Action::right_button),
        row(PadButton::r2, Action::left_button),
        row(PadButton::l3, Action::centre),
        row(PadButton::r3, Action::follow),
        row(PadButton::dpad_left, Action::select_menu),
        row(PadButton::left_pad, Action::minimap),
        row(PadButton::right_pad, Action::left_button),
    };
    const auto grips_map = [&] {
        auto rows = with_grips(shared, true);
        rows.push_back({PadButton::view, {}, {Action::keys_overlay, 0, false}});
        rows.push_back(row(PadButton::menu, Action::game_menu));
        rows.push_back(row(PadButton::l1, Action::build_ring));
        rows.push_back(row(PadButton::r1, Action::order_ring));
        return rows;
    };
    const auto fallback_map = [&] {
        auto rows = shared;
        rows.push_back(tap_hold(PadButton::view, Action::unit_info, Action::groups_layer));
        rows.push_back(tap_hold(PadButton::menu, Action::game_menu, Action::game_layer));
        rows.push_back(tap_hold(PadButton::l1, Action::add_toggle, Action::build_ring));
        rows.push_back(tap_hold(PadButton::r1, Action::queue_toggle, Action::order_ring));
        return rows;
    };
    const std::vector<Row> trackpads_dpad{
        row(PadButton::dpad_up, Action::commander),
        row(PadButton::dpad_right, Action::next_unit),
        row(PadButton::dpad_down, Action::next_report),
    };
    const std::vector<Row> sticks_dpad{
        row(PadButton::dpad_up, Action::zoom_in),
        row(PadButton::dpad_right, Action::next_report),
        row(PadButton::dpad_down, Action::zoom_out),
    };
    const auto join = [](std::vector<Row> rows, const std::vector<Row>& more) {
        rows.insert(rows.end(), more.begin(), more.end());
        return rows;
    };
    check_table(
        map_of(pad::Scheme::trackpads, false, true), Layer::base, join(grips_map(), trackpads_dpad)
    );
    check_table(
        map_of(pad::Scheme::sticks, false, true), Layer::base, join(grips_map(), sticks_dpad)
    );
    check_table(
        map_of(pad::Scheme::trackpads, true, false),
        Layer::base,
        join(fallback_map(), trackpads_dpad)
    );
    check_table(
        map_of(pad::Scheme::sticks, true, false), Layer::base, join(fallback_map(), sticks_dpad)
    );
}

/// Checks the held layers, the rings, SELECT ▾ and the menus in every map.
void layer_tables() {
    for (const bool fallback : {false, true})
        for (const auto scheme : {pad::Scheme::trackpads, pad::Scheme::sticks})
            for (const bool trackpads : {false, true}) {
                const auto ctx = map_of(scheme, fallback, trackpads);
                const bool grips = !fallback;
                auto groups = with_grips(
                    {
                        group(PadButton::dpad_up, 1),
                        group(PadButton::dpad_right, 2),
                        group(PadButton::dpad_down, 3),
                        group(PadButton::dpad_left, 4),
                        group(PadButton::y, 5),
                        group(PadButton::b, 6),
                        group(PadButton::a, 7),
                        group(PadButton::x, 8),
                    },
                    grips
                );
                if (trackpads)
                    groups.push_back(group(PadButton::left_pad, 0));
                check_table(ctx, Layer::groups, groups);
                check_table(
                    ctx,
                    Layer::game,
                    with_grips(
                        {
                            row(PadButton::a, Action::chat),
                            row(PadButton::b, Action::kill_board),
                            row(PadButton::x, Action::pause),
                            row(PadButton::y, Action::health_bars),
                            row(PadButton::dpad_up, Action::faster),
                            row(PadButton::dpad_down, Action::slower),
                            row(PadButton::dpad_left, Action::clear_messages),
                            row(PadButton::dpad_right, Action::team_menu),
                            row(PadButton::l1, Action::share_panel),
                            row(PadButton::r2, Action::screenshot),
                        },
                        grips
                    )
                );
                check_table(
                    ctx,
                    Layer::standing_orders,
                    with_grips(
                        {
                            row(PadButton::y, Action::fire_orders),
                            row(PadButton::b, Action::move_orders),
                            row(PadButton::dpad_up, Action::cloak),
                            row(PadButton::dpad_down, Action::on_off),
                            {PadButton::x, {Action::self_destruct, 0, true}, {}},
                            row(PadButton::r2, Action::left_button),
                            row(PadButton::a, Action::left_button),
                            row(PadButton::right_pad, Action::left_button),
                            row(PadButton::l2, Action::right_button),
                        },
                        grips
                    )
                );
                const std::vector<Row> ring{
                    row(PadButton::a, Action::ring_arm),
                    row(PadButton::b, Action::ring_close),
                    row(PadButton::r2, Action::ring_give),
                    row(PadButton::right_pad, Action::ring_give),
                };
                check_table(ctx, Layer::order_ring, with_grips(ring, grips));
                auto build = with_grips(ring, grips);
                build.push_back(row(PadButton::l2, Action::ring_reduce));
                check_table(ctx, Layer::build_ring, build);
                check_table(
                    ctx,
                    Layer::select_sheet,
                    with_grips(
                        {
                            row(PadButton::dpad_up, Action::sheet_up),
                            row(PadButton::dpad_down, Action::sheet_down),
                            row(PadButton::a, Action::sheet_pick),
                            row(PadButton::b, Action::sheet_close),
                            row(PadButton::dpad_left, Action::sheet_close),
                        },
                        grips
                    )
                );
                check_table(
                    ctx,
                    Layer::menus,
                    {
                        row(PadButton::dpad_up, Action::focus_up),
                        row(PadButton::dpad_right, Action::focus_right),
                        row(PadButton::dpad_down, Action::focus_down),
                        row(PadButton::dpad_left, Action::focus_left),
                        row(PadButton::l1, Action::focus_previous),
                        row(PadButton::r1, Action::focus_next),
                        row(PadButton::a, Action::press),
                        row(PadButton::b, Action::back),
                        row(PadButton::x, Action::library_action),
                        row(PadButton::y, Action::library_search),
                        row(PadButton::menu, Action::default_button),
                        row(PadButton::view, Action::info),
                        row(PadButton::r2, Action::left_button),
                        row(PadButton::right_pad, Action::left_button),
                        row(PadButton::l2, Action::right_button),
                    }
                );
            }
}

/// Checks the left-handed mirror: L and R swap for the pads, sticks, triggers, bumpers and
/// grips; the face buttons, D-pad, View and Menu stay; mirroring twice gives the button back.
void mirror_swaps_the_sides() {
    const std::array<std::pair<PadButton, PadButton>, 7> swapped{{
        {PadButton::l1, PadButton::r1},
        {PadButton::l2, PadButton::r2},
        {PadButton::l3, PadButton::r3},
        {PadButton::l4, PadButton::r4},
        {PadButton::l5, PadButton::r5},
        {PadButton::left_pad, PadButton::right_pad},
        {PadButton::left_stick_touch, PadButton::right_stick_touch},
    }};
    for (const auto& [left, right] : swapped) {
        OA_CHECK(pad::mirrored(left) == right);
        OA_CHECK(pad::mirrored(right) == left);
    }
    for (const PadButton kept :
         {PadButton::a,
          PadButton::b,
          PadButton::x,
          PadButton::y,
          PadButton::view,
          PadButton::menu,
          PadButton::dpad_up,
          PadButton::dpad_right,
          PadButton::dpad_down,
          PadButton::dpad_left,
          PadButton::none})
        OA_CHECK(pad::mirrored(kept) == kept);
    for (const PadButton button : every_button)
        OA_CHECK(pad::mirrored(pad::mirrored(button)) == button);
    // Left-handed: the left trackpad is the pointer's click, L2 the left button.
    auto ctx = map_of(pad::Scheme::trackpads, false, true);
    ctx.left_handed = true;
    OA_CHECK(pad::binding_for(ctx, Layer::base, PadButton::left_pad).action == Action::left_button);
    OA_CHECK(pad::binding_for(ctx, Layer::base, PadButton::right_pad).action == Action::minimap);
    OA_CHECK(pad::binding_for(ctx, Layer::base, PadButton::l2).action == Action::left_button);
    OA_CHECK(pad::binding_for(ctx, Layer::base, PadButton::l4).action == Action::queue);
    OA_CHECK(pad::binding_for(ctx, Layer::base, PadButton::a).action == Action::left_button);
}

/// Checks the layer order: menus off the match; rings, then SELECT ▾, then the held layers.
void layer_follows_what_is_held() {
    pad::Held held{};
    held.groups = true;
    OA_CHECK(pad::layer_for(held) == Layer::menus);
    held = {};
    held.in_match = true;
    OA_CHECK(pad::layer_for(held) == Layer::base);
    held.standing = true;
    OA_CHECK(pad::layer_for(held) == Layer::standing_orders);
    held.game = true;
    OA_CHECK(pad::layer_for(held) == Layer::game);
    held.groups = true;
    OA_CHECK(pad::layer_for(held) == Layer::groups);
    held.select_sheet = true;
    OA_CHECK(pad::layer_for(held) == Layer::select_sheet);
    held.build_ring = true;
    OA_CHECK(pad::layer_for(held) == Layer::build_ring);
    held.order_ring = true;
    OA_CHECK(pad::layer_for(held) == Layer::order_ring);
}

/// Returns whether a chord is the one expected.
bool chord_is(
    const std::optional<pad::Chord>& chord,
    PadButton held,
    PadButton button,
    bool tap = false,
    bool hold = false
) {
    return chord.has_value() && chord->held == held && chord->button == button &&
           chord->tap == tap && chord->hold == hold;
}

/// Checks the chords prompts name: each badge of the touch controls, the status line's R2 and
/// L2, the rings' hints, in the grips map, the fallback and left-handed.
void chords_for_every_badge() {
    const auto deck = map_of(pad::Scheme::trackpads, false, true);
    OA_CHECK(chord_is(pad::chord_for(deck, Action::queue), PadButton::none, PadButton::r4));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::add), PadButton::none, PadButton::l4));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::force), PadButton::none, PadButton::r5));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::clear), PadButton::none, PadButton::b));
    OA_CHECK(
        chord_is(pad::chord_for(deck, Action::select_menu), PadButton::none, PadButton::dpad_left)
    );
    OA_CHECK(!pad::chord_for(deck, Action::pause));
    OA_CHECK(!pad::chord_for(deck, Action::chat));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::centre), PadButton::none, PadButton::l3));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::follow), PadButton::none, PadButton::r3));
    OA_CHECK(
        chord_is(pad::chord_for(deck, Action::next_unit), PadButton::none, PadButton::dpad_right)
    );
    OA_CHECK(
        chord_is(pad::chord_for(deck, Action::keys_overlay), PadButton::none, PadButton::view, true)
    );
    OA_CHECK(!pad::chord_for(deck, Action::unit_info));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::left_button), PadButton::none, PadButton::r2));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::right_button), PadButton::none, PadButton::l2));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::order_ring), PadButton::none, PadButton::r1));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::build_ring), PadButton::none, PadButton::l1));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::ring_arm), PadButton::none, PadButton::a));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::ring_close), PadButton::none, PadButton::b));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::ring_give), PadButton::none, PadButton::r2));
    OA_CHECK(!pad::chord_for(deck, Action::game_layer));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::groups_layer), PadButton::none, PadButton::l5));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::group, 1), PadButton::l5, PadButton::dpad_up));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::group, 6), PadButton::l5, PadButton::b));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::group, 8), PadButton::l5, PadButton::x));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::group, 9), PadButton::l5, PadButton::left_pad));
    OA_CHECK(chord_is(
        pad::chord_for(deck, Action::self_destruct), PadButton::r5, PadButton::x, false, true
    ));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::fire_orders), PadButton::r5, PadButton::y));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::minimap), PadButton::none, PadButton::left_pad));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::game_menu), PadButton::none, PadButton::menu));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::queue_toggle), PadButton::none, PadButton::r4));
    OA_CHECK(chord_is(pad::chord_for(deck, Action::focus_next), PadButton::none, PadButton::r1));
    OA_CHECK(
        chord_is(pad::chord_for(deck, Action::default_button), PadButton::none, PadButton::menu)
    );
    OA_CHECK(!pad::chord_for(deck, Action::none));
    OA_CHECK(!pad::chord_for(deck, Action::zoom_in));
    OA_CHECK(!pad::chord_for(deck, Action::group, 10));
    // Sticks: the D-pad zooms and the next unit has no button of its own.
    const auto sticks = map_of(pad::Scheme::sticks, false, true);
    OA_CHECK(
        chord_is(pad::chord_for(sticks, Action::zoom_in), PadButton::none, PadButton::dpad_up)
    );
    OA_CHECK(chord_is(
        pad::chord_for(sticks, Action::next_report), PadButton::none, PadButton::dpad_right
    ));
    OA_CHECK(!pad::chord_for(sticks, Action::next_unit));
    OA_CHECK(!pad::chord_for(sticks, Action::commander));
    // The fallback: the latches are taps of R1 and L1, the rings their holds, the groups under
    // View and the game layer under Menu; no FORCE, no trackpads.
    const auto xbox = map_of(pad::Scheme::sticks, true, false);
    OA_CHECK(chord_is(pad::chord_for(xbox, Action::queue), PadButton::none, PadButton::r1, true));
    OA_CHECK(chord_is(pad::chord_for(xbox, Action::add), PadButton::none, PadButton::l1, true));
    OA_CHECK(chord_is(
        pad::chord_for(xbox, Action::order_ring), PadButton::none, PadButton::r1, false, true
    ));
    OA_CHECK(chord_is(
        pad::chord_for(xbox, Action::build_ring), PadButton::none, PadButton::l1, false, true
    ));
    OA_CHECK(!pad::chord_for(xbox, Action::force));
    OA_CHECK(!pad::chord_for(xbox, Action::self_destruct));
    OA_CHECK(chord_is(pad::chord_for(xbox, Action::pause), PadButton::menu, PadButton::x));
    OA_CHECK(chord_is(pad::chord_for(xbox, Action::group, 1), PadButton::view, PadButton::dpad_up));
    OA_CHECK(!pad::chord_for(xbox, Action::group, 9));
    OA_CHECK(!pad::chord_for(xbox, Action::minimap));
    OA_CHECK(
        chord_is(pad::chord_for(xbox, Action::game_menu), PadButton::none, PadButton::menu, true)
    );
    OA_CHECK(chord_is(pad::chord_for(xbox, Action::left_button), PadButton::none, PadButton::r2));
    OA_CHECK(chord_is(pad::chord_for(xbox, Action::ring_give), PadButton::none, PadButton::r2));
    // Left-handed: the physical buttons that took the roles.
    auto left = deck;
    left.left_handed = true;
    OA_CHECK(chord_is(pad::chord_for(left, Action::queue), PadButton::none, PadButton::l4));
    OA_CHECK(chord_is(pad::chord_for(left, Action::force), PadButton::none, PadButton::l5));
    OA_CHECK(chord_is(pad::chord_for(left, Action::left_button), PadButton::none, PadButton::l2));
    OA_CHECK(chord_is(pad::chord_for(left, Action::group, 1), PadButton::r5, PadButton::dpad_up));
    OA_CHECK(chord_is(pad::chord_for(left, Action::group, 9), PadButton::r5, PadButton::right_pad));
    OA_CHECK(!pad::chord_for(left, Action::pause));
    OA_CHECK(
        chord_is(pad::chord_for(left, Action::keys_overlay), PadButton::none, PadButton::view, true)
    );
    OA_CHECK(chord_is(pad::chord_for(left, Action::centre), PadButton::none, PadButton::r3));
}

/// Checks the effective scheme: Trackpads needs two trackpads or a Deck behind Steam Input.
void effective_scheme_needs_a_pointer() {
    pad::PadTraits deck{};
    deck.type = pad::PadType::steam_deck;
    deck.trackpads = 2;
    deck.grips = true;
    OA_CHECK(pad::effective_scheme(pad::Scheme::trackpads, deck) == pad::Scheme::trackpads);
    OA_CHECK(pad::effective_scheme(pad::Scheme::sticks, deck) == pad::Scheme::sticks);
    pad::PadTraits steam{};
    steam.type = pad::PadType::steam_deck;
    steam.steam_input = true;
    OA_CHECK(pad::effective_scheme(pad::Scheme::trackpads, steam) == pad::Scheme::trackpads);
    OA_CHECK(pad::effective_scheme(pad::Scheme::sticks, steam) == pad::Scheme::sticks);
    pad::PadTraits xbox{};
    xbox.type = pad::PadType::xbox;
    OA_CHECK(pad::effective_scheme(pad::Scheme::trackpads, xbox) == pad::Scheme::sticks);
    xbox.steam_input = true;
    OA_CHECK(pad::effective_scheme(pad::Scheme::trackpads, xbox) == pad::Scheme::sticks);
    pad::PadTraits playstation{};
    playstation.type = pad::PadType::playstation;
    playstation.trackpads = 1;
    OA_CHECK(pad::effective_scheme(pad::Scheme::trackpads, playstation) == pad::Scheme::sticks);
}

/// Checks the pad's type and the glyph set: Automatic by VID/PID, then the reported type;
/// each forced choice; Off shows nothing.
void glyph_style_follows_the_pad() {
    OA_CHECK(
        pad::pad_type_of(pad::valve_vendor, pad::steam_deck_product, pad::PadType::standard) ==
        pad::PadType::steam_deck
    );
    OA_CHECK(
        pad::pad_type_of(pad::valve_vendor, pad::steam_deck_product, pad::PadType::xbox) ==
        pad::PadType::steam_deck
    );
    OA_CHECK(pad::pad_type_of(0x045e, 0x02ea, pad::PadType::xbox) == pad::PadType::xbox);
    OA_CHECK(
        pad::pad_type_of(pad::valve_vendor, 0x1102, pad::PadType::unknown) == pad::PadType::unknown
    );
    const auto automatic = [](pad::PadType type) {
        return pad::glyph_style_for(pad::Prompts::automatic, type);
    };
    OA_CHECK(automatic(pad::PadType::steam_deck) == pad::GlyphStyle::steam_deck);
    OA_CHECK(automatic(pad::PadType::playstation) == pad::GlyphStyle::playstation);
    OA_CHECK(automatic(pad::PadType::nintendo) == pad::GlyphStyle::nintendo);
    OA_CHECK(automatic(pad::PadType::xbox) == pad::GlyphStyle::xbox);
    OA_CHECK(automatic(pad::PadType::standard) == pad::GlyphStyle::xbox);
    OA_CHECK(automatic(pad::PadType::unknown) == pad::GlyphStyle::xbox);
    for (const auto type :
         {pad::PadType::unknown,
          pad::PadType::steam_deck,
          pad::PadType::playstation,
          pad::PadType::nintendo,
          pad::PadType::xbox}) {
        OA_CHECK(
            pad::glyph_style_for(pad::Prompts::steam_deck, type) == pad::GlyphStyle::steam_deck
        );
        OA_CHECK(pad::glyph_style_for(pad::Prompts::xbox, type) == pad::GlyphStyle::xbox);
        OA_CHECK(
            pad::glyph_style_for(pad::Prompts::playstation, type) == pad::GlyphStyle::playstation
        );
        OA_CHECK(pad::glyph_style_for(pad::Prompts::nintendo, type) == pad::GlyphStyle::nintendo);
    }
    OA_CHECK(!pad::prompts_shown(pad::Prompts::off));
    for (const auto prompts :
         {pad::Prompts::automatic,
          pad::Prompts::steam_deck,
          pad::Prompts::xbox,
          pad::Prompts::playstation,
          pad::Prompts::nintendo})
        OA_CHECK(pad::prompts_shown(prompts));
}

/// Returns whether a glyph is the one expected.
bool glyph_is(const pad::GlyphSpec& glyph, pad::GlyphShape shape, std::string_view text) {
    return glyph.shape == shape && glyph.text == text;
}

/// Checks the glyphs of every style: letters, Nintendo's by place, PlayStation's marks, the
/// pills, View and Menu, the D-pad, trackpads and sticks.
void glyph_specs_by_style() {
    using pad::GlyphShape;
    using pad::GlyphStyle;
    for (const auto style : {GlyphStyle::steam_deck, GlyphStyle::xbox}) {
        OA_CHECK(glyph_is(pad::glyph_spec(PadButton::a, style), GlyphShape::letter_circle, "A"));
        OA_CHECK(glyph_is(pad::glyph_spec(PadButton::b, style), GlyphShape::letter_circle, "B"));
        OA_CHECK(glyph_is(pad::glyph_spec(PadButton::x, style), GlyphShape::letter_circle, "X"));
        OA_CHECK(glyph_is(pad::glyph_spec(PadButton::y, style), GlyphShape::letter_circle, "Y"));
        OA_CHECK(glyph_is(pad::glyph_spec(PadButton::view, style), GlyphShape::view, ""));
        OA_CHECK(glyph_is(pad::glyph_spec(PadButton::menu, style), GlyphShape::menu, ""));
    }
    // Nintendo labels by place: B at the bottom, A on the right, Y on the left, X on top.
    OA_CHECK(glyph_is(
        pad::glyph_spec(PadButton::a, GlyphStyle::nintendo), GlyphShape::letter_circle, "B"
    ));
    OA_CHECK(glyph_is(
        pad::glyph_spec(PadButton::b, GlyphStyle::nintendo), GlyphShape::letter_circle, "A"
    ));
    OA_CHECK(glyph_is(
        pad::glyph_spec(PadButton::x, GlyphStyle::nintendo), GlyphShape::letter_circle, "Y"
    ));
    OA_CHECK(glyph_is(
        pad::glyph_spec(PadButton::y, GlyphStyle::nintendo), GlyphShape::letter_circle, "X"
    ));
    OA_CHECK(
        glyph_is(pad::glyph_spec(PadButton::view, GlyphStyle::nintendo), GlyphShape::view, "-")
    );
    OA_CHECK(
        glyph_is(pad::glyph_spec(PadButton::menu, GlyphStyle::nintendo), GlyphShape::menu, "+")
    );
    OA_CHECK(
        glyph_is(pad::glyph_spec(PadButton::a, GlyphStyle::playstation), GlyphShape::cross, "")
    );
    OA_CHECK(
        glyph_is(pad::glyph_spec(PadButton::b, GlyphStyle::playstation), GlyphShape::circle, "")
    );
    OA_CHECK(
        glyph_is(pad::glyph_spec(PadButton::x, GlyphStyle::playstation), GlyphShape::square, "")
    );
    OA_CHECK(
        glyph_is(pad::glyph_spec(PadButton::y, GlyphStyle::playstation), GlyphShape::triangle, "")
    );
    const std::array<std::pair<PadButton, std::array<std::string_view, 4>>, 8> pills{{
        {PadButton::l1, {"L1", "LB", "L1", "L"}},
        {PadButton::r1, {"R1", "RB", "R1", "R"}},
        {PadButton::l2, {"L2", "LT", "L2", "ZL"}},
        {PadButton::r2, {"R2", "RT", "R2", "ZR"}},
        {PadButton::l4, {"L4", "P3", "L4", "L4"}},
        {PadButton::l5, {"L5", "P4", "L5", "L5"}},
        {PadButton::r4, {"R4", "P1", "R4", "R4"}},
        {PadButton::r5, {"R5", "P2", "R5", "R5"}},
    }};
    const std::array<GlyphStyle, 4> styles{
        GlyphStyle::steam_deck, GlyphStyle::xbox, GlyphStyle::playstation, GlyphStyle::nintendo
    };
    for (const auto& [button, labels] : pills)
        for (std::size_t at = 0; at < styles.size(); ++at)
            OA_CHECK(glyph_is(pad::glyph_spec(button, styles[at]), GlyphShape::pill, labels[at]));
    for (const auto style : styles) {
        const auto up = pad::glyph_spec(PadButton::dpad_up, style);
        const auto right = pad::glyph_spec(PadButton::dpad_right, style);
        const auto down = pad::glyph_spec(PadButton::dpad_down, style);
        const auto left = pad::glyph_spec(PadButton::dpad_left, style);
        OA_CHECK(up.shape == GlyphShape::dpad && up.direction == 0);
        OA_CHECK(right.shape == GlyphShape::dpad && right.direction == 1);
        OA_CHECK(down.shape == GlyphShape::dpad && down.direction == 2);
        OA_CHECK(left.shape == GlyphShape::dpad && left.direction == 3);
        const auto left_pad = pad::glyph_spec(PadButton::left_pad, style);
        const auto right_pad = pad::glyph_spec(PadButton::right_pad, style);
        OA_CHECK(
            left_pad.shape == GlyphShape::trackpad && left_pad.side == pad::Side::left &&
            left_pad.pressed
        );
        OA_CHECK(right_pad.shape == GlyphShape::trackpad && right_pad.side == pad::Side::right);
        const auto l3 = pad::glyph_spec(PadButton::l3, style);
        const auto r3 = pad::glyph_spec(PadButton::r3, style);
        const auto touch = pad::glyph_spec(PadButton::right_stick_touch, style);
        OA_CHECK(l3.shape == GlyphShape::stick && l3.side == pad::Side::left && l3.pressed);
        OA_CHECK(r3.shape == GlyphShape::stick && r3.side == pad::Side::right && r3.pressed);
        OA_CHECK(
            touch.shape == GlyphShape::stick && touch.side == pad::Side::right && !touch.pressed
        );
        OA_CHECK(pad::glyph_spec(PadButton::none, style).text.empty());
    }
}

/// Checks the feels: none with Haptics Off; Strong stronger than Light; the sides (low motor
/// the left pad, high the right); the pointer tick the lightest and shortest, a detent firmer,
/// a refused site a thump on the right; the trackpad pulses' sides, kinds and gains.
void feels_by_strength() {
    const std::array<pad::Feel, 7> feels{
        pad::Feel::pointer_tick,
        pad::Feel::target_detent,
        pad::Feel::wedge_change,
        pad::Feel::hold_started,
        pad::Feel::box_started,
        pad::Feel::site_refused,
        pad::Feel::queue_reduced,
    };
    for (const auto feel : feels) {
        OA_CHECK(!pad::rumble_for(feel, pad::Haptics::off));
        OA_CHECK(!pad::trackpad_pulse_for(feel, pad::Haptics::off));
        const auto light = pad::rumble_for(feel, pad::Haptics::light);
        const auto strong = pad::rumble_for(feel, pad::Haptics::strong);
        OA_CHECK(light.has_value() && strong.has_value());
        if (!light || !strong)
            continue;
        OA_CHECK(strong->low >= light->low && strong->high >= light->high);
        OA_CHECK(uint32_t{strong->low} + strong->high > uint32_t{light->low} + light->high);
        OA_CHECK(strong->ms >= light->ms && light->ms > 0);
        const bool both = feel == pad::Feel::hold_started;
        OA_CHECK(light->high > 0);
        OA_CHECK((light->low > 0) == both);
        const auto light_pulse = pad::trackpad_pulse_for(feel, pad::Haptics::light);
        const auto strong_pulse = pad::trackpad_pulse_for(feel, pad::Haptics::strong);
        OA_CHECK(light_pulse.has_value() && strong_pulse.has_value());
        if (!light_pulse || !strong_pulse)
            continue;
        OA_CHECK(light_pulse->side == (both ? pad::PadSide::both : pad::PadSide::right));
        OA_CHECK(
            strong_pulse->side == light_pulse->side && strong_pulse->kind == light_pulse->kind
        );
        OA_CHECK(strong_pulse->gain_db > light_pulse->gain_db);
    }
    const auto tick = *pad::rumble_for(pad::Feel::pointer_tick, pad::Haptics::light);
    const auto detent = *pad::rumble_for(pad::Feel::target_detent, pad::Haptics::light);
    const auto thump = *pad::rumble_for(pad::Feel::site_refused, pad::Haptics::light);
    for (const auto feel : feels) {
        const auto other = *pad::rumble_for(feel, pad::Haptics::light);
        OA_CHECK(tick.high <= other.high && tick.ms <= other.ms);
        OA_CHECK(thump.high >= other.high && thump.ms >= other.ms);
    }
    OA_CHECK(detent.high > tick.high);
    OA_CHECK(
        pad::trackpad_pulse_for(pad::Feel::pointer_tick, pad::Haptics::light)->kind ==
        pad::PulseKind::tick
    );
    OA_CHECK(
        pad::trackpad_pulse_for(pad::Feel::wedge_change, pad::Haptics::light)->kind ==
        pad::PulseKind::tick
    );
    OA_CHECK(
        pad::trackpad_pulse_for(pad::Feel::target_detent, pad::Haptics::light)->kind ==
        pad::PulseKind::click
    );
    OA_CHECK(
        pad::trackpad_pulse_for(pad::Feel::site_refused, pad::Haptics::light)->kind ==
        pad::PulseKind::click
    );
}

/// Checks the View button: in the menus it is the information key, as F1, left-handed too.
/// In a match on the default map a tap toggles the keys overlay and the button holds no
/// layer. On the fallback map a tap is still the unit's information.
void view_is_information_in_the_menus() {
    for (const bool left_handed : {false, true})
        for (const bool fallback : {false, true})
            for (const auto scheme : {pad::Scheme::trackpads, pad::Scheme::sticks})
                for (const bool trackpads : {false, true}) {
                    auto ctx = map_of(scheme, fallback, trackpads);
                    ctx.left_handed = left_handed;
                    OA_CHECK(
                        pad::binding_for(ctx, Layer::menus, PadButton::view).action == Action::info
                    );
                    const Action match_tap = fallback ? Action::unit_info : Action::keys_overlay;
                    OA_CHECK(
                        pad::tap_binding_for(ctx, Layer::base, PadButton::view).action == match_tap
                    );
                    const Action match_hold = fallback ? Action::groups_layer : Action::none;
                    OA_CHECK(
                        pad::binding_for(ctx, Layer::base, PadButton::view).action == match_hold
                    );
                }
    const auto deck = map_of(pad::Scheme::trackpads, false, true);
    OA_CHECK(chord_is(pad::chord_for(deck, Action::info), PadButton::none, PadButton::view));
}

/// Checks Y and X in the menus: the Library's search and its entry's first action, in every
/// map and both hands. In a match they keep the meanings they had.
void library_buttons_in_the_menus() {
    for (const bool left_handed : {false, true})
        for (const bool fallback : {false, true})
            for (const auto scheme : {pad::Scheme::trackpads, pad::Scheme::sticks})
                for (const bool trackpads : {false, true}) {
                    auto ctx = map_of(scheme, fallback, trackpads);
                    ctx.left_handed = left_handed;
                    OA_CHECK(
                        pad::binding_for(ctx, Layer::menus, PadButton::y).action ==
                        Action::library_search
                    );
                    OA_CHECK(
                        pad::binding_for(ctx, Layer::menus, PadButton::x).action ==
                        Action::library_action
                    );
                    OA_CHECK(
                        pad::binding_for(ctx, Layer::base, PadButton::y).action !=
                        Action::library_search
                    );
                    OA_CHECK(
                        pad::binding_for(ctx, Layer::base, PadButton::x).action !=
                        Action::library_action
                    );
                }
    for (const bool left_handed : {false, true}) {
        auto ctx = map_of(pad::Scheme::trackpads, false, true);
        ctx.left_handed = left_handed;
        OA_CHECK(
            chord_is(pad::chord_for(ctx, Action::library_search), PadButton::none, PadButton::y)
        );
        OA_CHECK(
            chord_is(pad::chord_for(ctx, Action::library_action), PadButton::none, PadButton::x)
        );
    }
}

} // namespace

/// Runs the checks.
int main() {
    acceleration_follows_the_thumb_speed();
    pointer_speed_scales_the_gain();
    landing_ignores_a_nudge();
    click_lock_holds_the_pointer();
    glide_coasts_and_stops();
    glide_travels_speed_times_tau();
    ticks_count_the_travel();
    absolute_maps_the_pad_onto_an_area();
    gyro_moves_by_turn();
    trigger_has_hysteresis();
    hold_timer_tells_tap_from_hold();
    menu_repeat_steps_on_time();
    double_click_needs_time_and_place();
    wedge_aim_picks_with_hysteresis();
    wedge_point_hits_the_same_slot();
    stick_cursor_follows_the_curve();
    stick_cursor_ramps_when_held_full();
    stick_cursor_slows_near_a_target();
    stick_cursor_magnetism();
    flicks_fire_once();
    base_tables();
    layer_tables();
    mirror_swaps_the_sides();
    view_is_information_in_the_menus();
    library_buttons_in_the_menus();
    layer_follows_what_is_held();
    chords_for_every_badge();
    effective_scheme_needs_a_pointer();
    glyph_style_follows_the_pad();
    glyph_specs_by_style();
    feels_by_strength();
    return oa::test::check_exit_status();
}
