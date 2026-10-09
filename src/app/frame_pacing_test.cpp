// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The application loop's pacing over a fake clock: frames evenly spaced at
// the rate asked for, never bunched after a late frame; at the tick rate a
// frame at the middle of each clock unit, one tick a frame at normal speed,
// shown whole; the match clock running 30 ticks a second at any frame rate;
// the presentation fraction in [0, 1], rising evenly between ticks, holding
// while the match waits or catches up; frames paced across the turn of the
// clock's reading to 0 at 2^32 milliseconds; the cap just below the
// display's rate while Vertical sync is in effect; the camera's scroll for
// a frame's real time; and the "+stats" overlay's figures, its frame
// history, its grades of each time and its table.
#include "oa/app/frame_pacing.hpp"

#include "oa/base/game_loop.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

using oa::app::frame_pacing::FrameStatsRow;
using oa::app::frame_pacing::FrameStatsRowKind;
using oa::app::frame_pacing::FrameStatsTable;
using oa::app::frame_pacing::FrameTicks;
using oa::app::frame_pacing::kNanosecondsPerMillisecond;
using oa::app::frame_pacing::kNanosecondsPerSecond;
using oa::app::frame_pacing::TickPresentation;
using oa::app::frame_pacing::TimeSeverity;
using oa::base::game_loop::Timing;

// A fake clock's start, away from 0 so that a start at 0 is not special.
constexpr uint64_t kClockStart = 5 * kNanosecondsPerSecond + 123'456;
// Seconds each simulated run of the loop lasts.
constexpr uint64_t kRunSeconds = 10;
// The match clock's units a second.
constexpr uint32_t kUnits = oa::app::frame_pacing::kTicksPerSecond;

/// Returns the match clock's reading at a fake clock time.
uint32_t clock_units(uint64_t now_ns) {
    return oa::base::game_loop::scaled_clock(
        static_cast<uint32_t>(now_ns / kNanosecondsPerMillisecond), kUnits
    );
}

/// Returns how far into its unit the match clock stands at a fake clock
/// time, as next_presentation_alpha reads it: 0 at a unit's start, up to 1.
double clock_unit_fraction(uint64_t now_ns) {
    constexpr uint32_t kThousandths = 1000;
    const auto milliseconds = static_cast<uint32_t>(now_ns / kNanosecondsPerMillisecond);
    const uint32_t into = milliseconds * kUnits % kThousandths;
    const double within_ms = static_cast<double>(now_ns % kNanosecondsPerMillisecond) /
                             static_cast<double>(kNanosecondsPerMillisecond);
    return (static_cast<double>(into) + within_ms * kUnits) / kThousandths;
}

/// Returns a match clock at a speed, stepped once at a time.
Timing started_timing(uint16_t speed, uint64_t now_ns) {
    Timing timing{};
    timing.requested_rate = speed;
    timing.actual_rate = speed;
    timing.previous_clock = clock_units(now_ns);
    return timing;
}

// What a simulated run of the loop saw.
struct LoopRun {
    uint64_t frames = 0;
    uint64_t ticks = 0;
    // Time and clock units between the first frame's time and the last's.
    uint64_t time_spanned_ns = 0;
    uint64_t clock_units_spanned = 0;
    // The time between the first frame and the second, and the shortest and
    // longest between two frames after it.
    uint64_t first_interval_ns = 0;
    uint64_t shortest_interval_ns = UINT64_MAX;
    uint64_t longest_interval_ns = 0;
    bool alpha_in_range = true;
    bool alpha_steady_between_ticks = true;
    // The shown time, in ticks (tick - 1 + fraction), from frame to frame.
    std::vector<double> shown;
    // Each frame's clock units stepped, ticks run and fraction.
    std::vector<uint32_t> units_stepped;
    std::vector<uint32_t> ticks_run;
    std::vector<float> alphas;
};

/// Returns a wait's oversleep for a frame: up to 0.9 ms, varying from frame
/// to frame as a real sleep's does.
uint64_t oversleep_ns(uint64_t frame) {
    return (frame * 7919 % 10) * 90'000;
}

/// Runs the loop's frame cycle over a fake clock for kRunSeconds from
/// `start_ns`: each frame takes its time from the pacer, steps the match
/// clock to it as idle_tick does and takes the presentation fraction for it,
/// drawn one a clock unit when the pacer keeps the tick rate, works for
/// `work_ns` (`slow_work_ns` on a frame that runs ticks), then waits as the
/// pacer says and oversleeps by up to 0.9 ms when `oversleep` is set.
LoopRun run_loop(
    uint32_t frames_per_second,
    uint16_t speed,
    uint64_t work_ns,
    uint64_t slow_work_ns,
    bool oversleep = false,
    uint64_t start_ns = kClockStart
) {
    LoopRun run;
    oa::app::frame_pacing::FramePacer pacer{};
    TickPresentation presentation{};
    uint64_t now = start_ns;
    Timing timing = started_timing(speed, now);
    uint64_t previous_time = 0;
    uint64_t first_time = 0;
    uint64_t last_time = 0;
    const uint64_t end = start_ns + kRunSeconds * kNanosecondsPerSecond;
    float previous_alpha = 1.0F;
    while (now < end) {
        const uint64_t time = oa::app::frame_pacing::begin_paced_frame(pacer, now);
        if (previous_time == 0) {
            first_time = time;
        } else if (run.frames == 1) {
            run.first_interval_ns = time - previous_time;
        } else {
            const uint64_t interval = time - previous_time;
            run.shortest_interval_ns = std::min(run.shortest_interval_ns, interval);
            run.longest_interval_ns = std::max(run.longest_interval_ns, interval);
        }
        previous_time = time;
        last_time = time;
        const uint32_t units_before = timing.previous_clock;
        CHECK(
            oa::base::game_loop::update_timing(timing, clock_units(time)) ==
            oa::base::game_loop::LoopError::none
        );
        const auto owed = timing.pending_steps;
        timing.tick += static_cast<uint32_t>(owed);
        run.ticks += static_cast<uint64_t>(owed);
        FrameTicks ticks{false, owed, static_cast<uint32_t>(owed)};
        ticks.unit_frames = pacer.frames_per_second == kUnits;
        const float alpha =
            oa::app::frame_pacing::next_presentation_alpha(presentation, timing, ticks, time);
        run.units_stepped.push_back(timing.previous_clock - units_before);
        run.ticks_run.push_back(static_cast<uint32_t>(owed));
        run.alphas.push_back(alpha);
        if (!(alpha >= 0.0F && alpha <= 1.0F))
            run.alpha_in_range = false;
        if (owed == 0 && alpha < previous_alpha)
            run.alpha_steady_between_ticks = false;
        previous_alpha = alpha;
        run.shown.push_back(static_cast<double>(timing.tick) - 1.0 + static_cast<double>(alpha));
        now += owed > 0 ? slow_work_ns : work_ns;
        now += oa::app::frame_pacing::end_paced_frame(pacer, now, frames_per_second);
        if (oversleep)
            now += oversleep_ns(run.frames);
        ++run.frames;
    }
    run.time_spanned_ns = last_time - first_time;
    run.clock_units_spanned = clock_units(last_time) - clock_units(first_time);
    return run;
}

/// Returns the largest departure of the shown time's step from frame to
/// frame from `step`, over the frames after the first `skip`.
double largest_step_error(const LoopRun& run, double step, uint64_t skip) {
    double largest = 0.0;
    for (std::size_t frame = skip + 1; frame < run.shown.size(); ++frame)
        largest = std::max(largest, std::abs(run.shown[frame] - run.shown[frame - 1] - step));
    return largest;
}

void test_frames_keep_their_rate() {
    for (const uint32_t rate : {30U, 60U, 120U, 144U, 240U}) {
        for (const bool oversleep : {false, true}) {
            const auto run = run_loop(
                rate,
                oa::base::game_loop::normal_game_speed,
                2 * kNanosecondsPerMillisecond,
                3 * kNanosecondsPerMillisecond,
                oversleep
            );
            const uint64_t period = kNanosecondsPerSecond / rate;
            CHECK(run.frames >= rate * kRunSeconds - 1 && run.frames <= rate * kRunSeconds + 1);
            // Evenly spaced, oversleeping or not: never closer than a period,
            // never further than a period and a nanosecond of rounding.
            CHECK(run.shortest_interval_ns + 1 >= period);
            CHECK(run.longest_interval_ns <= period + 1);
            // The second frame is due a period after the first; at the tick
            // rate, at the middle of the next clock unit, which lies from
            // about half a period to a period and a half later.
            if (rate == kUnits)
                CHECK(
                    run.first_interval_ns + kNanosecondsPerMillisecond >= period / 2 &&
                    run.first_interval_ns <= period * 3 / 2
                );
            else
                CHECK(run.first_interval_ns + 1 >= period && run.first_interval_ns <= period + 1);
        }
    }
}

void test_ticks_stay_at_thirty_a_second() {
    // Whatever the frame rate, a tick for each clock unit the frames span.
    for (const uint32_t rate : {30U, 60U, 120U, 144U, 240U, 0U}) {
        const auto run = run_loop(
            rate,
            oa::base::game_loop::normal_game_speed,
            kNanosecondsPerMillisecond,
            2 * kNanosecondsPerMillisecond,
            rate != 0
        );
        const uint64_t expected = run.time_spanned_ns * kUnits / kNanosecondsPerSecond;
        CHECK(run.clock_units_spanned + 1 >= expected && run.clock_units_spanned <= expected + 1);
        CHECK(
            run.time_spanned_ns + kNanosecondsPerSecond / 10 >= kRunSeconds * kNanosecondsPerSecond
        );
        CHECK(run.ticks == run.clock_units_spanned);
        CHECK(run.alpha_in_range);
        CHECK(run.alpha_steady_between_ticks);
    }
    // At double and half speed too.
    const auto fast = run_loop(
        120,
        2 * oa::base::game_loop::normal_game_speed,
        kNanosecondsPerMillisecond,
        kNanosecondsPerMillisecond
    );
    CHECK(fast.ticks == 2 * fast.clock_units_spanned);
    CHECK(fast.alpha_in_range && fast.alpha_steady_between_ticks);
    const auto slow = run_loop(
        120,
        oa::base::game_loop::normal_game_speed / 2,
        kNanosecondsPerMillisecond,
        kNanosecondsPerMillisecond
    );
    CHECK(
        slow.ticks + 1 >= slow.clock_units_spanned / 2 &&
        slow.ticks <= slow.clock_units_spanned / 2 + 1
    );
    CHECK(slow.alpha_in_range && slow.alpha_steady_between_ticks);
}

void test_frames_show_even_motion() {
    // At 120 frames a second each frame shows a quarter of a tick more than
    // the one before: no step at the ticks, where 30 Hz drawing shows a
    // whole tick at once. The clock steps on whole milliseconds, so a tick
    // can come up to a millisecond after its time: 0.03 of a tick.
    constexpr double kMillisecondOfTicks = 0.03;
    const auto run = run_loop(
        120,
        oa::base::game_loop::normal_game_speed,
        2 * kNanosecondsPerMillisecond,
        2 * kNanosecondsPerMillisecond
    );
    CHECK(largest_step_error(run, 0.25, 120) <= kMillisecondOfTicks);
    const auto sixty = run_loop(
        60,
        oa::base::game_loop::normal_game_speed,
        2 * kNanosecondsPerMillisecond,
        2 * kNanosecondsPerMillisecond
    );
    CHECK(largest_step_error(sixty, 0.5, 60) <= kMillisecondOfTicks);
    // Half speed: an eighth of a tick a frame.
    const auto slow = run_loop(
        120,
        oa::base::game_loop::normal_game_speed / 2,
        2 * kNanosecondsPerMillisecond,
        2 * kNanosecondsPerMillisecond
    );
    CHECK(largest_step_error(slow, 0.125, 120) <= kMillisecondOfTicks / 2);
    // Frames that run ticks take longer, and the waits oversleep: each frame
    // still shows the time it stands for on the pacer's even grid.
    const auto uneven = run_loop(
        120,
        oa::base::game_loop::normal_game_speed,
        kNanosecondsPerMillisecond,
        5 * kNanosecondsPerMillisecond,
        true
    );
    CHECK(uneven.alpha_in_range && uneven.alpha_steady_between_ticks);
    CHECK(largest_step_error(uneven, 0.25, 120) <= kMillisecondOfTicks);
}

void test_clock_unit_middles() {
    using oa::app::frame_pacing::next_clock_unit_middle;
    constexpr uint64_t kUnitNs = kNanosecondsPerSecond / kUnits;
    // From any time through three units, the next unit's middle: the clock
    // reads one unit more there, half of it gone, from about half a unit to
    // a unit and a half later.
    for (uint64_t offset = 0; offset < 3 * kUnitNs; offset += 250'000) {
        const uint64_t time = kClockStart + offset;
        const uint64_t middle = next_clock_unit_middle(time);
        CHECK(clock_units(middle) == clock_units(time) + 1);
        CHECK(std::abs(clock_unit_fraction(middle) - 0.5) < 1e-6);
        CHECK(middle + kNanosecondsPerMillisecond >= time + kUnitNs / 2);
        CHECK(middle <= time + kUnitNs * 3 / 2);
    }
    // Middle after middle lie a unit apart, to the nanosecond's rounding,
    // without drift: 3000 units take 100 seconds.
    const uint64_t first = next_clock_unit_middle(kClockStart);
    uint64_t middle = first;
    for (uint32_t unit = 0; unit < 3000; ++unit) {
        const uint64_t next = next_clock_unit_middle(middle);
        CHECK(next - middle + 1 >= kUnitNs && next - middle <= kUnitNs + 1);
        CHECK(clock_units(next) == clock_units(middle) + 1);
        middle = next;
    }
    CHECK(middle - first + 1 >= 100 * kNanosecondsPerSecond);
    CHECK(middle - first <= 100 * kNanosecondsPerSecond + 1);
    // Past the clock's wrap-around (its milliseconds times 30 past 32
    // bits), middles follow the units the clock reads from there.
    const uint64_t wrap = (uint64_t{UINT32_MAX} / kUnits + 1) * kNanosecondsPerMillisecond;
    middle = next_clock_unit_middle(wrap);
    CHECK(clock_units(middle) == clock_units(wrap) + 1);
    for (uint32_t unit = 0; unit < 100; ++unit) {
        const uint64_t next = next_clock_unit_middle(middle);
        CHECK(clock_units(next) == clock_units(middle) + 1);
        CHECK(std::abs(clock_unit_fraction(next) - 0.5) < 1e-6);
        middle = next;
    }
}

/// Tells whether every frame of a run after the first stepped the match
/// clock by one unit, ran `ticks` ticks and showed the state they reached
/// whole.
///
/// @param run the run
/// @param ticks the ticks each frame ran
/// @return true when each did
bool whole_tick_frames(const LoopRun& run, uint32_t ticks) {
    for (std::size_t frame = 1; frame < run.alphas.size(); ++frame)
        if (run.units_stepped[frame] != 1 || run.ticks_run[frame] != ticks ||
            run.alphas[frame] != 1.0F)
            return false;
    return true;
}

void test_thirty_frames_draw_each_tick_once() {
    constexpr uint64_t kUnitNs = kNanosecondsPerSecond / kUnits;
    constexpr uint16_t kNormal = oa::base::game_loop::normal_game_speed;
    // Frames a period apart from the fake clock's start, at the tick rate,
    // step the clock by none and then two: they fall within a millisecond
    // of where the clock's whole milliseconds turn its units over.
    uint32_t none = 0;
    uint32_t two = 0;
    for (uint64_t frame = 1; frame <= kUnits; ++frame) {
        const uint32_t step =
            clock_units(oa::app::frame_pacing::paced_frame_due(kClockStart, frame, kUnits)) -
            clock_units(oa::app::frame_pacing::paced_frame_due(kClockStart, frame - 1, kUnits));
        none += step == 0 ? 1 : 0;
        two += step == 2 ? 1 : 0;
    }
    CHECK(none > 0 && two > 0);
    // Paced at the tick rate from any start through a clock unit, each
    // frame after the first steps the clock by one unit, runs one tick at
    // normal speed and shows it whole: the shown time steps a whole tick a
    // frame. Oversleeping, and with frames that take most of their period.
    for (uint64_t offset = 0; offset <= kUnitNs + kNanosecondsPerMillisecond; offset += 250'000)
        for (const bool oversleep : {false, true})
            for (const uint64_t work :
                 {kNanosecondsPerMillisecond, 30 * kNanosecondsPerMillisecond}) {
                const auto run =
                    run_loop(kUnits, kNormal, work, work, oversleep, kClockStart + offset);
                CHECK(run.frames >= kUnits * kRunSeconds - 1);
                CHECK(run.frames <= kUnits * kRunSeconds + 1);
                CHECK(whole_tick_frames(run, 1));
                CHECK(run.ticks + 1 == run.frames);
                CHECK(largest_step_error(run, 1.0, 1) == 0.0);
            }
    // Double speed: two ticks a frame, the later shown whole.
    const auto fast =
        run_loop(kUnits, 2 * kNormal, kNanosecondsPerMillisecond, kNanosecondsPerMillisecond);
    CHECK(whole_tick_frames(fast, 2));
    // One and a half times normal speed: one tick and two by turns, the
    // state each frame's ticks reached shown whole.
    const auto faster =
        run_loop(kUnits, kNormal * 3 / 2, kNanosecondsPerMillisecond, kNanosecondsPerMillisecond);
    bool by_turns = true;
    for (std::size_t frame = 2; frame < faster.alphas.size(); ++frame)
        by_turns = by_turns && faster.units_stepped[frame] == 1 &&
                   faster.ticks_run[frame] + faster.ticks_run[frame - 1] == 3 &&
                   faster.alphas[frame] == 1.0F;
    CHECK(by_turns);
    // Half speed: a tick every other frame, and each frame half a tick more
    // than the one before, so that motion stays even and each tick is
    // shown whole on one frame.
    const auto slow =
        run_loop(kUnits, kNormal / 2, kNanosecondsPerMillisecond, kNanosecondsPerMillisecond);
    CHECK(largest_step_error(slow, 0.5, 1) < 1e-6);
    uint64_t whole = 0;
    for (std::size_t frame = 1; frame < slow.alphas.size(); ++frame)
        whole += slow.alphas[frame] == 1.0F ? 1 : 0;
    CHECK(whole + 1 >= slow.ticks && whole <= slow.ticks + 1);
}

void test_paced_frames_across_the_clock_turn() {
    constexpr uint64_t kUnitNs = kNanosecondsPerSecond / kUnits;
    constexpr uint16_t kNormal = oa::base::game_loop::normal_game_speed;
    // From 2 seconds before 2^32 milliseconds, where the clock's
    // milliseconds, and its reading with them, turn over to 0.
    constexpr uint64_t kTurn = (uint64_t{1} << 32U) * kNanosecondsPerMillisecond;
    constexpr uint64_t kStart = kTurn - 2 * kNanosecondsPerSecond;
    // At the tick rate the frames keep to the middles of the units through
    // the turn, but for the first past it, which stands early in its unit
    // and runs no tick; every other frame runs one and shows it whole.
    const auto tick_rate = run_loop(
        kUnits, kNormal, kNanosecondsPerMillisecond, kNanosecondsPerMillisecond, false, kStart
    );
    uint32_t idle = 0;
    bool whole = true;
    for (std::size_t frame = 1; frame < tick_rate.alphas.size(); ++frame) {
        idle += tick_rate.ticks_run[frame] == 0 ? 1 : 0;
        whole = whole && tick_rate.ticks_run[frame] <= 1 && tick_rate.alphas[frame] == 1.0F;
    }
    CHECK(idle == 1);
    CHECK(whole);
    CHECK(tick_rate.ticks + 2 == tick_rate.frames);
    CHECK(tick_rate.longest_interval_ns < 2 * kUnitNs);
    // At 120 frames a second the clock keeps 30 ticks a second through the
    // turn, short by a tick at most, the fraction in [0, 1] and never
    // falling between ticks.
    const auto quick = run_loop(
        120, kNormal, kNanosecondsPerMillisecond, kNanosecondsPerMillisecond, false, kStart
    );
    const auto away =
        run_loop(120, kNormal, kNanosecondsPerMillisecond, kNanosecondsPerMillisecond);
    CHECK(quick.alpha_in_range && quick.alpha_steady_between_ticks);
    CHECK(quick.ticks + 1 >= away.ticks && quick.ticks <= away.ticks);
}

void test_late_frame_at_the_tick_rate() {
    // A frame at the tick rate that takes a unit and a half: the next starts
    // at once and stands for its start, stepping the clock by one unit or
    // two; the frames after it stand at the middles of their units again,
    // a unit each.
    oa::app::frame_pacing::FramePacer pacer{};
    uint64_t now = kClockStart;
    uint32_t units = 0;
    for (uint32_t frame = 0; frame < 40; ++frame) {
        const uint64_t time = oa::app::frame_pacing::begin_paced_frame(pacer, now);
        const uint32_t step = clock_units(time) - units;
        units = clock_units(time);
        if (frame == 11)
            CHECK(step == 1 || step == 2);
        else if (frame > 0)
            CHECK(step == 1);
        if (frame > 0 && frame != 11)
            CHECK(std::abs(clock_unit_fraction(time) - 0.5) < 1e-6);
        now += frame == 10 ? 50 * kNanosecondsPerMillisecond : kNanosecondsPerMillisecond;
        now += oa::app::frame_pacing::end_paced_frame(pacer, now, kUnits);
    }
}

void test_late_frame_is_not_followed_by_a_burst() {
    oa::app::frame_pacing::FramePacer pacer{};
    constexpr uint32_t rate = 120;
    const uint64_t period = kNanosecondsPerSecond / rate;
    uint64_t now = kClockStart;
    std::vector<uint64_t> starts;
    for (int frame = 0; frame < 20; ++frame) {
        (void)oa::app::frame_pacing::begin_paced_frame(pacer, now);
        starts.push_back(now);
        // Frame 5 takes three periods.
        now += frame == 5 ? 3 * period : kNanosecondsPerMillisecond;
        now += oa::app::frame_pacing::end_paced_frame(pacer, now, rate);
    }
    for (std::size_t frame = 1; frame < starts.size(); ++frame)
        CHECK(starts[frame] - starts[frame - 1] + 1 >= period);
    // The late frame's successor starts at once, and the run goes on from it.
    CHECK(starts[6] - starts[5] == 3 * period);
    CHECK(starts[7] - starts[6] >= period && starts[7] - starts[6] <= period + 1);
}

void test_pacer_edges() {
    oa::app::frame_pacing::FramePacer pacer{};
    // No limit: never waits.
    (void)oa::app::frame_pacing::begin_paced_frame(pacer, kClockStart);
    CHECK(oa::app::frame_pacing::end_paced_frame(pacer, kClockStart + 1000, 0) == 0);
    // A frame woken early by input begins a new run from its start. At the
    // idle rate, the tick rate, the frame before waits for the middle of
    // the next clock unit.
    pacer = {};
    (void)oa::app::frame_pacing::begin_paced_frame(pacer, kClockStart);
    const auto wait = oa::app::frame_pacing::end_paced_frame(pacer, kClockStart + 1000, 30);
    CHECK(wait == oa::app::frame_pacing::next_clock_unit_middle(kClockStart) - kClockStart - 1000);
    const uint64_t woken = kClockStart + 5 * kNanosecondsPerMillisecond;
    (void)oa::app::frame_pacing::begin_paced_frame(pacer, woken);
    CHECK(pacer.run_start_ns == woken && pacer.frames_in_run == 0);
    // A new rate starts its run from the frame's start.
    CHECK(
        oa::app::frame_pacing::end_paced_frame(pacer, woken + 1000, 120) ==
        kNanosecondsPerSecond / 120 - 1000
    );
    // An oversleep keeps the run: the next frame is still due on its time.
    const uint64_t due =
        oa::app::frame_pacing::paced_frame_due(pacer.run_start_ns, pacer.frames_in_run, 120);
    (void)oa::app::frame_pacing::begin_paced_frame(pacer, due + 300'000);
    CHECK(pacer.run_start_ns == woken);
    CHECK(
        oa::app::frame_pacing::end_paced_frame(pacer, due + 400'000, 120) ==
        oa::app::frame_pacing::paced_frame_due(woken, 2, 120) - (due + 400'000)
    );
    // Due times count from the run's start without drift.
    CHECK(oa::app::frame_pacing::paced_frame_due(0, 120, 120) == kNanosecondsPerSecond);
    CHECK(oa::app::frame_pacing::paced_frame_due(7, 3, 120) == 7 + 25'000'000);
}

void test_frame_rate_choice() {
    using oa::app::frame_pacing::FrameActivity;
    using oa::app::frame_pacing::paced_frame_rate;
    CHECK(paced_frame_rate(120, FrameActivity{true, false, false, false}) == 120);
    CHECK(paced_frame_rate(120, FrameActivity{false, true, false, false}) == 120);
    CHECK(paced_frame_rate(120, FrameActivity{false, false, true, false}) == 120);
    CHECK(paced_frame_rate(120, FrameActivity{false, false, false, true}) == 120);
    // Idle: 30 frames a second, or fewer when the limit is lower.
    CHECK(paced_frame_rate(120, FrameActivity{}) == oa::app::frame_pacing::kIdleFramesPerSecond);
    CHECK(paced_frame_rate(0, FrameActivity{}) == oa::app::frame_pacing::kIdleFramesPerSecond);
    CHECK(paced_frame_rate(20, FrameActivity{}) == 20);
    CHECK(paced_frame_rate(0, FrameActivity{true, false, false, false}) == 0);
}

void test_vertical_sync_cap() {
    using oa::app::frame_pacing::capped_frame_rate;
    using oa::app::frame_pacing::FrameActivity;
    using oa::app::frame_pacing::paced_frame_rate;
    using oa::app::frame_pacing::vsync_frame_cap;
    // The largest whole rate below the display's.
    CHECK(vsync_frame_cap(60.0F) == 59);
    CHECK(vsync_frame_cap(59.94F) == 59);
    CHECK(vsync_frame_cap(75.0F) == 74);
    CHECK(vsync_frame_cap(120.0F) == 119);
    CHECK(vsync_frame_cap(144.0F) == 143);
    CHECK(vsync_frame_cap(143.856F) == 143);
    CHECK(vsync_frame_cap(240.0F) == 239);
    // Never under the tick rate, so each tick is still drawn.
    CHECK(vsync_frame_cap(30.0F) == oa::app::frame_pacing::kTicksPerSecond);
    CHECK(vsync_frame_cap(31.0F) == 30);
    CHECK(vsync_frame_cap(24.0F) == 30);
    CHECK(vsync_frame_cap(0.5F) == 30);
    // No rate reported: no cap; a rate past any limit is held to the highest.
    CHECK(vsync_frame_cap(0.0F) == 0);
    CHECK(vsync_frame_cap(-60.0F) == 0);
    CHECK(vsync_frame_cap(std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK(
        vsync_frame_cap(std::numeric_limits<float>::infinity()) ==
        oa::app::frame_pacing::kHighestVsyncCap
    );
    CHECK(vsync_frame_cap(5000.0F) == oa::app::frame_pacing::kHighestVsyncCap);

    // The paced rate is the lowest of the maximum, the idle rate and the cap.
    const uint32_t cap = vsync_frame_cap(60.0F);
    CHECK(
        capped_frame_rate(paced_frame_rate(120, FrameActivity{true, false, false, false}), cap) ==
        59
    );
    CHECK(
        capped_frame_rate(paced_frame_rate(40, FrameActivity{true, false, false, false}), cap) == 40
    );
    CHECK(
        capped_frame_rate(paced_frame_rate(0, FrameActivity{true, false, false, false}), cap) == 59
    );
    CHECK(capped_frame_rate(paced_frame_rate(120, FrameActivity{}), cap) == 30);
    // With no cap, Vertical sync Off or a display without a rate, the rate
    // is as without it, no limit included.
    for (const uint32_t rate : {0U, 30U, 60U, 120U, 1000U})
        CHECK(capped_frame_rate(rate, 0) == rate);
    // A display at the tick rate keeps frames at the tick rate, where each
    // frame is due at the middle of a clock unit.
    CHECK(capped_frame_rate(120, vsync_frame_cap(30.0F)) == oa::app::frame_pacing::kTicksPerSecond);
}

void test_presentation_holds() {
    const uint64_t now = kClockStart;
    Timing timing = started_timing(oa::base::game_loop::normal_game_speed, now);
    TickPresentation presentation{};
    const uint64_t unit = kNanosecondsPerSecond / kUnits;
    // Until a batch runs, frames show the current state whole.
    CHECK(
        oa::app::frame_pacing::next_presentation_alpha(
            presentation, timing, {false, 0, 0}, now + unit / 2
        ) == 1.0F
    );
    // A batch at a unit's start: the fraction follows the time since.
    const uint64_t boundary =
        static_cast<uint64_t>(clock_units(now) + 1) * kNanosecondsPerSecond / kUnits +
        kNanosecondsPerMillisecond;
    CHECK(
        oa::base::game_loop::update_timing(timing, clock_units(boundary)) ==
        oa::base::game_loop::LoopError::none
    );
    CHECK(timing.pending_steps == 1);
    const float first = oa::app::frame_pacing::next_presentation_alpha(
        presentation, timing, {false, 1, 1}, boundary
    );
    CHECK(first >= 0.0F && first < 0.1F);
    const float later = oa::app::frame_pacing::next_presentation_alpha(
        presentation, timing, {false, 0, 0}, boundary + unit / 2
    );
    CHECK(later > 0.45F && later < 0.6F);
    // A step owed that did not run (the match waits on another machine):
    // whole until the next batch, even as the time goes on.
    CHECK(
        oa::app::frame_pacing::next_presentation_alpha(
            presentation, timing, {false, 1, 0}, boundary + unit
        ) == 1.0F
    );
    CHECK(
        oa::app::frame_pacing::next_presentation_alpha(
            presentation, timing, {false, 0, 0}, boundary + unit + 1
        ) == 1.0F
    );
    // The batch that follows starts the fraction again.
    timing.previous_clock = clock_units(boundary + 2 * unit);
    CHECK(
        oa::app::frame_pacing::next_presentation_alpha(
            presentation, timing, {false, 1, 1}, boundary + 2 * unit
        ) < 0.2F
    );
    // A batch of five, catching up after a long frame, is shown whole.
    CHECK(
        oa::app::frame_pacing::next_presentation_alpha(
            presentation, timing, {false, 5, 5}, boundary + 2 * unit
        ) == 1.0F
    );
    timing.previous_clock = clock_units(boundary + 3 * unit);
    CHECK(
        oa::app::frame_pacing::next_presentation_alpha(
            presentation, timing, {false, 1, 1}, boundary + 3 * unit
        ) < 0.2F
    );
    // The clock set back (a screenshot resets it): the fraction does not go back.
    const float before = oa::app::frame_pacing::next_presentation_alpha(
        presentation, timing, {false, 0, 0}, boundary + 3 * unit + unit * 3 / 4
    );
    timing.previous_clock = clock_units(boundary + 3 * unit + unit * 3 / 4);
    CHECK(
        oa::app::frame_pacing::next_presentation_alpha(
            presentation, timing, {false, 0, 0}, boundary + 3 * unit + unit * 4 / 5
        ) >= before
    );
    // A paused match, a menu or a whole-tick frame: whole, until a batch.
    CHECK(
        oa::app::frame_pacing::next_presentation_alpha(
            presentation, timing, {true, 0, 0}, boundary
        ) == 1.0F
    );
    CHECK(presentation.hold);
    CHECK(
        oa::app::frame_pacing::next_presentation_alpha(
            presentation, timing, {false, 0, 0}, boundary + unit / 2
        ) == 1.0F
    );
    // Double speed runs two ticks a batch, shown across the unit.
    Timing fast = started_timing(2 * oa::base::game_loop::normal_game_speed, now);
    TickPresentation fast_presentation{};
    CHECK(
        oa::base::game_loop::update_timing(fast, clock_units(boundary)) ==
        oa::base::game_loop::LoopError::none
    );
    CHECK(fast.pending_steps == 2);
    CHECK(
        oa::app::frame_pacing::next_presentation_alpha(
            fast_presentation, fast, {false, 2, 2}, boundary
        ) < 0.1F
    );
    // The clock's time wrapping past 32 bits of milliseconds times 30.
    const uint64_t wrap = (uint64_t{UINT32_MAX} / kUnits + 1) * kNanosecondsPerMillisecond;
    Timing wrapped = started_timing(oa::base::game_loop::normal_game_speed, wrap - unit / 2);
    TickPresentation wrapped_presentation{};
    // The clock's reading goes back to 0 as the product wraps, as 3.1c's
    // does, so no tick is owed; the fraction stays whole and in range.
    CHECK(
        oa::base::game_loop::update_timing(wrapped, clock_units(wrap + unit)) ==
        oa::base::game_loop::LoopError::none
    );
    const float wrapped_alpha = oa::app::frame_pacing::next_presentation_alpha(
        wrapped_presentation,
        wrapped,
        {false, wrapped.pending_steps, static_cast<uint32_t>(wrapped.pending_steps)},
        wrap + unit
    );
    CHECK(wrapped_alpha >= 0.0F && wrapped_alpha <= 1.0F);
}

void test_scroll_distance() {
    // Scroll speed 32 is 32 pixels a clock unit: 8 a frame at 120 frames a
    // second, 960 a second at any rate.
    CHECK(
        std::abs(oa::app::frame_pacing::scroll_distance(32, kNanosecondsPerSecond / 120) - 8.0) <
        1e-5
    );
    for (const uint32_t rate : {30U, 60U, 120U, 144U, 240U}) {
        double carry = 0.0;
        int64_t moved = 0;
        for (uint32_t frame = 0; frame < rate; ++frame) {
            const uint64_t start = frame * kNanosecondsPerSecond / rate;
            const uint64_t end = (frame + 1) * kNanosecondsPerSecond / rate;
            carry += oa::app::frame_pacing::scroll_distance(32, end - start);
            const auto step = static_cast<int64_t>(std::floor(carry));
            carry -= static_cast<double>(step);
            moved += step;
        }
        CHECK(moved >= 959 && moved <= 960);
    }
    // Capped at the most a frame may move, and a frame shorter than a clock
    // unit at its share of that: speed 255 covers 30 * 128 pixels a second
    // at every rate.
    CHECK(oa::app::frame_pacing::scroll_distance(255, kNanosecondsPerSecond) == 128.0);
    CHECK(oa::app::frame_pacing::scroll_distance(32, 0) == 0.0);
    for (const uint32_t rate : {30U, 60U, 120U, 144U}) {
        double moved = 0.0;
        for (uint32_t frame = 0; frame < rate; ++frame)
            moved += oa::app::frame_pacing::scroll_distance(
                255,
                (frame + 1) * kNanosecondsPerSecond / rate - frame * kNanosecondsPerSecond / rate
            );
        CHECK(std::abs(moved - 30.0 * 128.0) < 1e-3);
    }
}

/// Tells whether a row shows a label, its three values and a note.
///
/// @param row the row
/// @param label its label
/// @param least the least column's text
/// @param mean the mean column's text
/// @param most the most column's text
/// @param note its note
/// @return true when every text matches
bool row_reads(
    const FrameStatsRow& row,
    std::string_view label,
    std::string_view least,
    std::string_view mean,
    std::string_view most,
    std::string_view note
) {
    return row.label.view() == label && row.values[0].view() == least &&
           row.values[1].view() == mean && row.values[2].view() == most && row.note.view() == note;
}

void test_frame_stats() {
    using oa::app::frame_pacing::FrameMeasure;
    using oa::app::frame_pacing::frame_allowance_ns;
    using oa::app::frame_pacing::FrameWait;
    using oa::app::frame_pacing::note_frame_measure;
    using oa::app::frame_pacing::roll_frame_stats;
    oa::app::frame_pacing::FrameStatsWindow window{};
    // Until the loop sets one, samples are graded against 120 frames a
    // second and a precise wait.
    CHECK(window.allowance_ns == frame_allowance_ns(120, FrameWait::precise));
    const oa::app::frame_pacing::FrameStatsNotes notes{120, 120, 0, 0};
    oa::app::frame_pacing::FrameStatsRenderer renderer{};
    renderer.tier = "standard";
    renderer.driver = "metal";
    renderer.adapter = "Apple M2";
    CHECK(!roll_frame_stats(window, kClockStart));
    FrameStatsTable table = oa::app::frame_pacing::frame_stats_table(window, notes, renderer);
    CHECK(table.row_count == oa::app::frame_pacing::kFrameStatsRowsMost);
    CHECK(table.row_count == 11);
    CHECK(table.rows[0].kind == FrameStatsRowKind::title);
    CHECK(row_reads(table.rows[0], "Frame stats (ms)", "", "", "", ""));
    CHECK(table.rows[1].kind == FrameStatsRowKind::heading);
    CHECK(row_reads(table.rows[1], "", "min", "avg", "max", ""));
    CHECK(table.rows[2].kind == FrameStatsRowKind::rate);
    CHECK(row_reads(table.rows[2], "FPS", "", "--", "", "limit 120"));
    CHECK(table.rows[2].values[1].severity == TimeSeverity::none);
    CHECK(table.rows[3].kind == FrameStatsRowKind::measure);
    CHECK(row_reads(table.rows[3], "frame", "", "--", "", ""));
    CHECK(row_reads(table.rows[5], "tick", "", "--", "", ""));
    CHECK(row_reads(table.rows[7], "present", "", "--", "", ""));
    // The units row shows from the start, so the panel keeps its height.
    CHECK(table.rows[8].kind == FrameStatsRowKind::count);
    CHECK(row_reads(table.rows[8], "units", "", "0", "", "0 between ticks"));
    // The renderer row: the tier and the driver, and the adapter as its note.
    CHECK(table.rows[9].kind == FrameStatsRowKind::renderer);
    CHECK(row_reads(table.rows[9], "standard: metal", "", "", "", "Apple M2"));
    // The display row, last: no window was named.
    CHECK(table.rows[10].kind == FrameStatsRowKind::display);
    CHECK(row_reads(table.rows[10], "no window", "", "", "", ""));
    // A second of 120 frames 8 to 9 ms apart, 30 ticks of 2 to 4 ms.
    for (uint32_t frame = 0; frame < 120; ++frame) {
        note_frame_measure(
            window,
            FrameMeasure::frame,
            frame % 2 == 0 ? 8 * kNanosecondsPerMillisecond : 9 * kNanosecondsPerMillisecond
        );
        note_frame_measure(window, FrameMeasure::work, 5 * kNanosecondsPerMillisecond);
        note_frame_measure(window, FrameMeasure::draw, 3 * kNanosecondsPerMillisecond);
        note_frame_measure(window, FrameMeasure::present, kNanosecondsPerMillisecond);
    }
    for (uint32_t tick = 0; tick < 30; ++tick)
        note_frame_measure(
            window, FrameMeasure::tick, (tick % 2 == 0 ? 2 : 4) * kNanosecondsPerMillisecond
        );
    CHECK(!roll_frame_stats(window, kClockStart + kNanosecondsPerSecond - 1));
    CHECK(roll_frame_stats(window, kClockStart + kNanosecondsPerSecond));
    table = oa::app::frame_pacing::frame_stats_table(window, notes, renderer);
    CHECK(row_reads(table.rows[2], "FPS", "", "120", "", "limit 120"));
    CHECK(row_reads(table.rows[3], "frame", "8.00", "8.50", "9.00", ""));
    CHECK(row_reads(table.rows[4], "work", "5.00", "5.00", "5.00", ""));
    CHECK(row_reads(table.rows[5], "tick", "2.00", "3.00", "4.00", "30/s"));
    CHECK(row_reads(table.rows[6], "draw", "3.00", "3.00", "3.00", ""));
    CHECK(row_reads(table.rows[7], "present", "1.00", "1.00", "1.00", ""));
    // Each time graded against 8.33 ms and its half a millisecond of slack:
    // the longest frame, 9 ms, is over it, and every other time within it.
    CHECK(table.rows[2].values[1].severity == TimeSeverity::within_frame);
    for (std::size_t row = 3; row < 8; ++row)
        for (std::size_t column = 0; column < table.rows[row].values.size(); ++column)
            CHECK(
                table.rows[row].values[column].severity ==
                (row == 3 && column == 2 ? TimeSeverity::within_tick : TimeSeverity::within_frame)
            );
    CHECK(table.rows[1].values[0].severity == TimeSeverity::none);
    // The next second starts empty; the shown one stays until it ends.
    CHECK(window.measuring[0].count == 0);
    // Idle, no limit, and the units the drawing reports.
    table = oa::app::frame_pacing::frame_stats_table(window, {120, 30, 0, 0}, renderer);
    CHECK(table.rows[2].note.view() == "idle 30");
    table = oa::app::frame_pacing::frame_stats_table(window, {0, 0, 0, 0}, renderer);
    CHECK(table.rows[2].note.view() == "no limit");
    table = oa::app::frame_pacing::frame_stats_table(window, {120, 120, 40, 38}, renderer);
    CHECK(row_reads(table.rows[8], "units", "", "40", "", "38 between ticks"));
    CHECK(table.rows[8].values[1].severity == TimeSeverity::none);
    // The rate shown beside them grades nothing: each time keeps the grade
    // it had when it was taken.
    table = oa::app::frame_pacing::frame_stats_table(window, {240, 240, 0, 0}, renderer);
    CHECK(table.rows[4].values[1].severity == TimeSeverity::within_frame);
    CHECK(table.rows[3].values[2].severity == TimeSeverity::within_tick);
    // A second with no tick (a paused match) shows none.
    CHECK(roll_frame_stats(window, kClockStart + 2 * kNanosecondsPerSecond));
    table = oa::app::frame_pacing::frame_stats_table(window, notes, renderer);
    CHECK(row_reads(table.rows[5], "tick", "", "--", "", "0/s"));
    CHECK(row_reads(table.rows[2], "FPS", "", "0", "", "limit 120"));
    // A slow second: frames over a tick grade over it, in every column.
    for (uint32_t frame = 0; frame < 10; ++frame)
        note_frame_measure(window, FrameMeasure::frame, 50 * kNanosecondsPerMillisecond);
    CHECK(roll_frame_stats(window, kClockStart + 3 * kNanosecondsPerSecond));
    table = oa::app::frame_pacing::frame_stats_table(window, notes, renderer);
    CHECK(row_reads(table.rows[2], "FPS", "", "10", "", "limit 120"));
    CHECK(table.rows[2].values[1].severity == TimeSeverity::over_tick);
    CHECK(row_reads(table.rows[3], "frame", "50.00", "50.00", "50.00", ""));
    for (const auto& value : table.rows[3].values)
        CHECK(value.severity == TimeSeverity::over_tick);
    // A second that goes idle and back: ten idle frames of 34 ms graded
    // against 30 a second and the idle wait's slack, then 8 ms frames and a
    // late one of 20 ms graded against 120 a second. The longest, 34 ms,
    // keeps its grade within its frame's allowance; the late one shows in
    // the graph; the mean is graded against the mean allowance.
    window.allowance_ns = frame_allowance_ns(30, FrameWait::idle);
    for (uint32_t frame = 0; frame < 10; ++frame)
        note_frame_measure(window, FrameMeasure::frame, 34 * kNanosecondsPerMillisecond);
    window.allowance_ns = frame_allowance_ns(120, FrameWait::precise);
    for (uint32_t frame = 0; frame < 60; ++frame)
        note_frame_measure(window, FrameMeasure::frame, 8 * kNanosecondsPerMillisecond);
    note_frame_measure(window, FrameMeasure::frame, 20 * kNanosecondsPerMillisecond);
    CHECK(roll_frame_stats(window, kClockStart + 4 * kNanosecondsPerSecond));
    table = oa::app::frame_pacing::frame_stats_table(window, notes, renderer);
    CHECK(row_reads(table.rows[3], "frame", "8.00", "11.83", "34.00", ""));
    CHECK(table.rows[3].values[0].severity == TimeSeverity::within_frame);
    CHECK(table.rows[3].values[1].severity == TimeSeverity::within_frame);
    CHECK(table.rows[3].values[2].severity == TimeSeverity::within_frame);
    const auto newest = oa::app::frame_pacing::frame_history_column(window.history, 0);
    CHECK(newest.frame_ns == 20 * kNanosecondsPerMillisecond);
    CHECK(newest.severity == TimeSeverity::within_tick);
}

// The renderer row names the tier alone without a driver; cuts the
// tier and driver, and the adapter, each to one of the table's texts,
// never inside a character; and the widest table keeps it empty, since
// the panel cuts it to its width.
void test_renderer_row() {
    using oa::app::frame_pacing::FrameStatsRenderer;
    using oa::app::frame_pacing::kFrameStatsRowsMost;
    using oa::app::frame_pacing::kFrameStatsTextBytes;
    const oa::app::frame_pacing::FrameStatsWindow window{};
    const oa::app::frame_pacing::FrameStatsNotes notes{120, 120, 0, 0};
    constexpr std::size_t kRendererRow = kFrameStatsRowsMost - 2;
    constexpr std::size_t kTextCharacters = kFrameStatsTextBytes - 1;
    FrameStatsRenderer renderer{};
    renderer.tier = "standard";
    auto row = oa::app::frame_pacing::frame_stats_table(window, notes, renderer).rows[kRendererRow];
    CHECK(row.kind == FrameStatsRowKind::renderer);
    CHECK(row_reads(row, "standard", "", "", "", ""));
    renderer.driver = "direct3d11";
    renderer.adapter = "NVIDIA GeForce RTX 3060 Laptop GPU";
    row = oa::app::frame_pacing::frame_stats_table(window, notes, renderer).rows[kRendererRow];
    CHECK(row_reads(row, "standard: direct3d11", "", "", "", "NVIDIA GeForce RTX 3060"));
    CHECK(row.note.view().size() == kTextCharacters);
    // The full tier's anti-aliasing follows the tier's name.
    renderer.tier = "full";
    renderer.anti_aliasing = "2x";
    row = oa::app::frame_pacing::frame_stats_table(window, notes, renderer).rows[kRendererRow];
    CHECK(row_reads(row, "full 2x: direct3d11", "", "", "", "NVIDIA GeForce RTX 3060"));
    renderer.anti_aliasing = {};
    row = oa::app::frame_pacing::frame_stats_table(window, notes, renderer).rows[kRendererRow];
    CHECK(row_reads(row, "full: direct3d11", "", "", "", "NVIDIA GeForce RTX 3060"));
    // A limit the tier draws within takes the adapter's place.
    renderer.limit = "sprite memory full";
    row = oa::app::frame_pacing::frame_stats_table(window, notes, renderer).rows[kRendererRow];
    CHECK(row_reads(row, "full: direct3d11", "", "", "", "sprite memory full"));
    renderer.limit = {};
    renderer.tier = "standard";
    // A two-byte character across the cut is left out whole; one that ends
    // at the cut is kept.
    renderer.adapter = "Radeon Graphics 012345\xc3\xa9t";
    row = oa::app::frame_pacing::frame_stats_table(window, notes, renderer).rows[kRendererRow];
    CHECK(row.note.view() == "Radeon Graphics 012345");
    renderer.adapter = "Radeon Graphics 01234\xc3\xa9t";
    row = oa::app::frame_pacing::frame_stats_table(window, notes, renderer).rows[kRendererRow];
    CHECK(row.note.view() == "Radeon Graphics 01234\xc3\xa9");
    // A driver longer than the text holds is cut where the text ends.
    renderer.driver = "a-render-driver-with-a-long-name";
    row = oa::app::frame_pacing::frame_stats_table(window, notes, renderer).rows[kRendererRow];
    CHECK(row.label.view() == "standard: a-render-driv");
    CHECK(row.label.view().size() == kTextCharacters);
    renderer.driver = "a-render-dri\xc3\xa9";
    row = oa::app::frame_pacing::frame_stats_table(window, notes, renderer).rows[kRendererRow];
    CHECK(row.label.view() == "standard: a-render-dri");
    const auto widest = oa::app::frame_pacing::frame_stats_widest_table().rows[kRendererRow];
    CHECK(widest.kind == FrameStatsRowKind::renderer);
    CHECK(row_reads(widest, "", "", "", "", ""));
}

// The display row names a window, full screen on the desktop's mode and
// at a mode of the game's own, the frame the match is drawn at, and the
// display's mode, refresh rate and scale, leaving out what is not known;
// the widest table keeps it empty, since the panel cuts it to its width.
void test_display_row() {
    using oa::app::frame_pacing::FrameStatsDisplay;
    using oa::app::frame_pacing::FrameStatsScreen;
    using oa::app::frame_pacing::kFrameStatsRowsMost;
    using oa::app::frame_pacing::runs_on;
    const oa::app::frame_pacing::FrameStatsWindow window{};
    const oa::app::frame_pacing::FrameStatsNotes notes{120, 120, 0, 0};
    constexpr std::size_t kDisplayRow = kFrameStatsRowsMost - 1;
    const auto row_of = [&](const FrameStatsDisplay& display) {
        return oa::app::frame_pacing::frame_stats_table(window, notes, {}, display)
            .rows[kDisplayRow];
    };
    // A window on a 14-inch Retina MacBook Pro's display.
    FrameStatsDisplay display{};
    display.screen = FrameStatsScreen::window;
    display.frame_width = 1512;
    display.frame_height = 982;
    display.mode_width = 1728;
    display.mode_height = 1117;
    display.refresh_rate = 120.0F;
    display.display_scale = 2.0F;
    auto row = row_of(display);
    CHECK(row.kind == FrameStatsRowKind::display);
    CHECK(runs_on(row.kind));
    CHECK(row_reads(row, "window 1512x982", "", "", "", "1728x1117@120 2x"));
    // Full screen on the desktop's mode, the match drawn at a chosen size
    // and scaled to the screen.
    display.screen = FrameStatsScreen::full_screen;
    display.frame_width = 1280;
    display.frame_height = 720;
    row = row_of(display);
    CHECK(row_reads(row, "full screen 1280x720", "", "", "", "1728x1117@120 2x"));
    // Full screen at a mode of its own on a monitor of Windows at 125%,
    // whose rate is a fraction.
    display.screen = FrameStatsScreen::exclusive;
    display.mode_width = 1280;
    display.mode_height = 720;
    display.refresh_rate = 59.94F;
    display.display_scale = 1.25F;
    row = row_of(display);
    CHECK(row_reads(row, "exclusive 1280x720", "", "", "", "1280x720@60 1.25x"));
    display.display_scale = 1.5F;
    CHECK(row_of(display).note.view() == "1280x720@60 1.5x");
    display.display_scale = 1.0F;
    CHECK(row_of(display).note.view() == "1280x720@60 1x");
    // A rate, a mode or a scale not known is left out.
    display.refresh_rate = 0.0F;
    CHECK(row_of(display).note.view() == "1280x720 1x");
    display.display_scale = 0.0F;
    CHECK(row_of(display).note.view() == "1280x720");
    display.mode_width = 0;
    display.display_scale = 2.0F;
    CHECK(row_of(display).note.view() == "2x");
    display.display_scale = 0.0F;
    CHECK(row_of(display).note.view().empty());
    // The longest label and note fit the table's texts.
    display.screen = FrameStatsScreen::full_screen;
    display.frame_width = 16384;
    display.frame_height = 16384;
    display.mode_width = 16384;
    display.mode_height = 16384;
    display.refresh_rate = 240.0F;
    display.display_scale = 1.75F;
    row = row_of(display);
    CHECK(row_reads(row, "full screen 16384x16384", "", "", "", "16384x16384@240 1.75x"));
    // Without a window.
    CHECK(row_reads(row_of({}), "no window", "", "", "", ""));
    const auto widest = oa::app::frame_pacing::frame_stats_widest_table().rows[kDisplayRow];
    CHECK(widest.kind == FrameStatsRowKind::display);
    CHECK(row_reads(widest, "", "", "", "", ""));
    CHECK(!runs_on(FrameStatsRowKind::measure));
}

// A cut keeps whole characters: the whole text when it fits, and else
// backs off to the start of the character the limit falls in.
void test_whole_characters() {
    using oa::app::frame_pacing::whole_characters;
    CHECK(whole_characters("Apple M2", 23) == "Apple M2");
    CHECK(whole_characters("Apple M2", 5) == "Apple");
    CHECK(whole_characters("Apple M2", 0).empty());
    CHECK(whole_characters("", 4).empty());
    // A two-byte, a three-byte and a four-byte character, the limit inside
    // each.
    CHECK(whole_characters("ab\xc3\xa9", 3) == "ab");
    CHECK(whole_characters("ab\xc3\xa9", 4) == "ab\xc3\xa9");
    CHECK(whole_characters("a\xe2\x84\xa2z", 2) == "a");
    CHECK(whole_characters("a\xe2\x84\xa2z", 3) == "a");
    CHECK(whole_characters("a\xe2\x84\xa2z", 4) == "a\xe2\x84\xa2");
    CHECK(whole_characters("\xf0\x9f\x96\xa5x", 3).empty());
}

void test_time_severity() {
    using oa::app::frame_pacing::frame_allowance_ns;
    using oa::app::frame_pacing::frame_budget_ns;
    using oa::app::frame_pacing::FrameWait;
    using oa::app::frame_pacing::kFrameBudgetSlackNs;
    using oa::app::frame_pacing::kIdleWaitSlackNs;
    using oa::app::frame_pacing::kTickBudgetNs;
    using oa::app::frame_pacing::time_severity;
    CHECK(kTickBudgetNs == 33'333'333);
    CHECK(frame_budget_ns(120) == 8'333'333);
    CHECK(frame_budget_ns(60) == 16'666'666);
    CHECK(frame_budget_ns(0) == kTickBudgetNs);
    // The allowance: the budget and half a millisecond after a precise
    // wait, two after an idle one.
    CHECK(frame_allowance_ns(120, FrameWait::precise) == 8'833'333);
    CHECK(frame_allowance_ns(120, FrameWait::precise) == 8'333'333 + kFrameBudgetSlackNs);
    CHECK(frame_allowance_ns(30, FrameWait::idle) == 35'333'333);
    CHECK(frame_allowance_ns(30, FrameWait::idle) == kTickBudgetNs + kIdleWaitSlackNs);
    CHECK(frame_allowance_ns(0, FrameWait::precise) == kTickBudgetNs + kFrameBudgetSlackNs);
    // At 120 frames a second: up to the allowance, within the frame; then
    // up to a tick, within the tick; then over it.
    const uint64_t at_120 = frame_allowance_ns(120, FrameWait::precise);
    CHECK(time_severity(0, at_120) == TimeSeverity::within_frame);
    CHECK(time_severity(8'333'333, at_120) == TimeSeverity::within_frame);
    CHECK(time_severity(at_120, at_120) == TimeSeverity::within_frame);
    CHECK(time_severity(at_120 + 1, at_120) == TimeSeverity::within_tick);
    CHECK(time_severity(kTickBudgetNs, at_120) == TimeSeverity::within_tick);
    CHECK(time_severity(kTickBudgetNs + 1, at_120) == TimeSeverity::over_tick);
    CHECK(time_severity(kNanosecondsPerSecond, at_120) == TimeSeverity::over_tick);
    // 20 ms is over 60's allowance but within a tick; 16.9 ms within it.
    const uint64_t at_60 = frame_allowance_ns(60, FrameWait::precise);
    CHECK(time_severity(20 * kNanosecondsPerMillisecond, at_60) == TimeSeverity::within_tick);
    CHECK(time_severity(16'900'000, at_60) == TimeSeverity::within_frame);
    // With no limit, or idle at 30 a second, the allowance is a tick or
    // more: no time grades within a tick. An idle frame that oversleeps its
    // whole-millisecond wait stays within its allowance.
    const uint64_t idle = frame_allowance_ns(30, FrameWait::idle);
    CHECK(
        time_severity(kTickBudgetNs, frame_allowance_ns(0, FrameWait::precise)) ==
        TimeSeverity::within_frame
    );
    CHECK(time_severity(34'500'000, idle) == TimeSeverity::within_frame);
    CHECK(time_severity(idle, idle) == TimeSeverity::within_frame);
    CHECK(time_severity(idle + 1, idle) == TimeSeverity::over_tick);
}

void test_frame_history() {
    using oa::app::frame_pacing::frame_history_column;
    using oa::app::frame_pacing::kFrameGraphColumns;
    using oa::app::frame_pacing::note_frame_history;
    constexpr uint64_t kMs = kNanosecondsPerMillisecond;
    // Two seconds of 120 columns each.
    CHECK(kFrameGraphColumns == 240);
    oa::app::frame_pacing::FrameHistory history{};
    CHECK(frame_history_column(history, 0).severity == TimeSeverity::none);
    // A frame of 25 ms covers the three columns whose middles it holds
    // (4.17, 12.5 and 20.83 ms).
    note_frame_history(history, 25 * kMs, TimeSeverity::within_tick);
    CHECK(history.newest_column == 2);
    for (std::size_t age = 0; age < 3; ++age) {
        CHECK(frame_history_column(history, age).frame_ns == 25 * kMs);
        CHECK(frame_history_column(history, age).severity == TimeSeverity::within_tick);
    }
    CHECK(frame_history_column(history, 3).severity == TimeSeverity::none);
    // Two frames of 4 ms (240 a second): the first holds no middle and
    // covers the column its end lies in, the second that column's middle
    // (29.17 ms); the column keeps the first of two equal frames.
    note_frame_history(history, 4 * kMs, TimeSeverity::within_frame);
    CHECK(history.newest_column == 3);
    note_frame_history(history, 4 * kMs, TimeSeverity::within_tick);
    CHECK(history.newest_column == 3);
    CHECK(frame_history_column(history, 0).frame_ns == 4 * kMs);
    CHECK(frame_history_column(history, 0).severity == TimeSeverity::within_frame);
    // A longer frame sharing a column replaces a shorter one, and keeps its
    // grade; a shorter one does not.
    note_frame_history(history, 6 * kMs, TimeSeverity::within_frame); // 33 to 39 ms: column 4
    note_frame_history(history, 2 * kMs, TimeSeverity::within_frame); // to 41 ms: column 4
    CHECK(history.newest_column == 4);
    CHECK(frame_history_column(history, 0).frame_ns == 6 * kMs);
    note_frame_history(history, 7 * kMs, TimeSeverity::over_tick); // to 48 ms: column 5
    CHECK(frame_history_column(history, 0).frame_ns == 7 * kMs);
    CHECK(frame_history_column(history, 0).severity == TimeSeverity::over_tick);
    CHECK(frame_history_column(history, 5).frame_ns == 25 * kMs);
    CHECK(frame_history_column(history, 6).severity == TimeSeverity::none);
    // Frames of exactly a column each fill one column apiece; past two
    // seconds the oldest go and the newest stay in order.
    oa::app::frame_pacing::FrameHistory steady{};
    for (uint64_t frame = 1; frame <= kFrameGraphColumns + 10; ++frame)
        note_frame_history(
            steady, kNanosecondsPerSecond / 120 + frame % 2, TimeSeverity::within_frame
        );
    CHECK(steady.newest_column == kFrameGraphColumns + 9);
    for (std::size_t age = 0; age < kFrameGraphColumns; ++age)
        CHECK(frame_history_column(steady, age).severity == TimeSeverity::within_frame);
    CHECK(frame_history_column(steady, kFrameGraphColumns).severity == TimeSeverity::none);
    // A frame longer than the graph covers all of it.
    note_frame_history(steady, 3 * kNanosecondsPerSecond, TimeSeverity::over_tick);
    for (std::size_t age = 0; age < kFrameGraphColumns; ++age)
        CHECK(frame_history_column(steady, age).frame_ns == 3 * kNanosecondsPerSecond);
    // Idle frames of 33.3 ms cover four columns each, so two seconds of
    // them fill the graph too.
    oa::app::frame_pacing::FrameHistory idle{};
    for (uint32_t frame = 0; frame < 60; ++frame)
        note_frame_history(idle, kNanosecondsPerSecond / 30, TimeSeverity::within_frame);
    std::size_t held = 0;
    for (std::size_t age = 0; age < kFrameGraphColumns; ++age)
        held += frame_history_column(idle, age).severity != TimeSeverity::none ? 1 : 0;
    CHECK(held == kFrameGraphColumns);
    // The statistics put each frame, and only a frame, in it, graded
    // against the window's allowance.
    oa::app::frame_pacing::FrameStatsWindow window{};
    oa::app::frame_pacing::note_frame_measure(
        window, oa::app::frame_pacing::FrameMeasure::work, 7 * kMs
    );
    CHECK(frame_history_column(window.history, 0).severity == TimeSeverity::none);
    oa::app::frame_pacing::note_frame_measure(
        window, oa::app::frame_pacing::FrameMeasure::frame, 9 * kMs
    );
    CHECK(frame_history_column(window.history, 0).frame_ns == 9 * kMs);
    CHECK(frame_history_column(window.history, 0).severity == TimeSeverity::within_tick);
}

} // namespace

int main() {
    test_frames_keep_their_rate();
    test_ticks_stay_at_thirty_a_second();
    test_frames_show_even_motion();
    test_clock_unit_middles();
    test_thirty_frames_draw_each_tick_once();
    test_paced_frames_across_the_clock_turn();
    test_late_frame_at_the_tick_rate();
    test_late_frame_is_not_followed_by_a_burst();
    test_pacer_edges();
    test_frame_rate_choice();
    test_vertical_sync_cap();
    test_presentation_holds();
    test_scroll_distance();
    test_frame_stats();
    test_renderer_row();
    test_display_row();
    test_whole_characters();
    test_time_severity();
    test_frame_history();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
