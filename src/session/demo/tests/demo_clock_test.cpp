// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The viewer's playback clock, with no match and no recording: the ticks one
// frame owes at each speed, frame skipping, the speed a short frame reached,
// the keyframe schedule and eviction under the budget.
#include "oa/session/demo/clock.hpp"

#include "oa/session/demo.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ++failures;                                                                            \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
        }                                                                                          \
    } while (0)

using oa::session::demo::fold_jump_pause;
using oa::session::demo::FramePlan;
using oa::session::demo::KeyframeReel;
using oa::session::demo::PlaybackClock;
using oa::session::demo::PlaybackSpeed;
using oa::session::demo::SeekPlan;
using oa::session::demo::SoundGate;

/// About one frame at 60 frames a second. 1000/60 is not a whole number of
/// milliseconds, and 17 ms is the slice that owes the old per-frame counts.
constexpr uint32_t kFrameMs = 17;
constexpr uint32_t kRecorded = 10;

FramePlan frame_at(PlaybackClock& clock, PlaybackSpeed speed) {
    clock.set_speed(speed);
    clock.set_recorded_speed(kRecorded);
    return clock.begin_frame(kFrameMs, false);
}

uint32_t ticks_over(PlaybackClock& clock, const uint32_t* slices, std::size_t count) {
    uint32_t owed = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const FramePlan plan = clock.begin_frame(slices[index], false);
        owed += plan.ticks;
        (void)clock.note_frame(plan.ticks);
    }
    return owed;
}

void check_speeds() {
    // A 17 ms frame at the normal recorded speed owes the old 60-frame counts:
    // a quarter tick at one half, half a tick at 1x, then 1, 2, 4 and 8.
    PlaybackClock half;
    uint32_t owed = 0;
    for (int step = 0; step < 4; ++step) {
        const FramePlan plan = frame_at(half, PlaybackSpeed::half);
        owed += plan.ticks;
        CHECK(plan.paced);
        CHECK(!plan.skipping);
        CHECK(plan.asked_milli == 500);
        CHECK(plan.undrawn == (plan.ticks > 0 ? plan.ticks - 1 : 0));
        CHECK(plan.draw_last == (plan.ticks > 0));
        (void)half.note_frame(plan.ticks);
    }
    CHECK(owed == 1);

    PlaybackClock normal;
    owed = 0;
    for (int step = 0; step < 2; ++step) {
        const FramePlan plan = frame_at(normal, PlaybackSpeed::normal);
        owed += plan.ticks;
        (void)normal.note_frame(plan.ticks);
    }
    CHECK(owed == 1);
    CHECK(frame_at(normal, PlaybackSpeed::normal).asked_milli == 1000);

    PlaybackClock steady;
    const FramePlan twice = frame_at(steady, PlaybackSpeed::twice);
    CHECK(twice.ticks == 1 && twice.draw_last && twice.undrawn == 0);

    PlaybackClock fast;
    const FramePlan quadruple = frame_at(fast, PlaybackSpeed::quadruple);
    CHECK(quadruple.ticks == 2 && quadruple.draw_last && quadruple.undrawn == 1);
    const FramePlan octuple = frame_at(fast, PlaybackSpeed::octuple);
    CHECK(octuple.ticks == 4 && octuple.undrawn == 3);
    const FramePlan sixteen = frame_at(fast, PlaybackSpeed::sixteen);
    CHECK(sixteen.ticks == 8 && sixteen.draw_last && sixteen.undrawn == 7);
    CHECK(sixteen.asked_milli == 16000);

    // A recorded speed change multiplies the viewer's speed and does not reset it.
    fast.set_speed(PlaybackSpeed::twice);
    fast.set_recorded_speed(20);
    const FramePlan doubled = fast.begin_frame(kFrameMs, false);
    CHECK(doubled.ticks == 2);
    CHECK(fast.speed() == PlaybackSpeed::twice);

    // One second, however it is split, owes 15, 30, 60, 120, 240 and 480 ticks.
    const uint32_t second[] = {250, 250, 250, 250};
    const PlaybackSpeed ladder[] = {
        PlaybackSpeed::half,
        PlaybackSpeed::normal,
        PlaybackSpeed::twice,
        PlaybackSpeed::quadruple,
        PlaybackSpeed::octuple,
        PlaybackSpeed::sixteen,
    };
    const uint32_t expected[] = {15, 30, 60, 120, 240, 480};
    for (std::size_t index = 0; index < std::size(ladder); ++index) {
        PlaybackClock clock;
        clock.set_speed(ladder[index]);
        clock.set_recorded_speed(kRecorded);
        CHECK(ticks_over(clock, second, std::size(second)) == expected[index]);
    }

    // 1x stays at 30 ticks a second at 30, 60, 120 and 250 frames a second.
    // A steady slower rate is not late against itself.
    uint32_t at_30[30];
    uint32_t at_60[60];
    uint32_t at_120[120];
    uint32_t at_250[250];
    for (int step = 0; step < 30; ++step)
        at_30[step] = (step % 3 == 2) ? 34 : 33;
    for (int step = 0; step < 60; ++step)
        at_60[step] = (step % 3 == 2) ? 16 : 17;
    for (int step = 0; step < 120; ++step)
        at_120[step] = (step % 3 == 2) ? 9 : 8;
    for (int step = 0; step < 250; ++step)
        at_250[step] = 4;
    const uint32_t* rates[] = {at_30, at_60, at_120, at_250};
    const std::size_t counts[] = {30, 60, 120, 250};
    for (std::size_t index = 0; index < std::size(rates); ++index) {
        PlaybackClock clock;
        clock.set_recorded_speed(kRecorded);
        CHECK(ticks_over(clock, rates[index], counts[index]) == 30);
        CHECK(clock.speed_line() == "1×");
    }

    // + and - walk the ladder and stop at the ends.
    PlaybackClock stepped;
    stepped.step_speed(-1);
    CHECK(stepped.speed() == PlaybackSpeed::half);
    stepped.step_speed(-1);
    CHECK(stepped.speed() == PlaybackSpeed::half);
    for (int step = 0; step < 8; ++step)
        stepped.step_speed(1);
    CHECK(stepped.speed() == PlaybackSpeed::sixteen);
}

void check_pause_and_skip() {
    PlaybackClock clock;
    clock.set_recorded_speed(kRecorded);
    clock.set_speed(PlaybackSpeed::twice);
    CHECK(clock.begin_frame(kFrameMs, false).ticks == 1);
    (void)clock.note_frame(1);
    clock.pause();
    const FramePlan held = clock.begin_frame(5000, false);
    CHECK(held.ticks == 0 && !held.paced);
    (void)clock.note_frame(0);
    CHECK(clock.paused());
    CHECK(clock.speed_line() == "2×");
    clock.play();
    // The paused time is not played later, and it is not the next cadence.
    CHECK(clock.begin_frame(kFrameMs, false).ticks == 1);
    (void)clock.note_frame(1);
    CHECK(clock.speed_line() == "2×");
    const FramePlan menu = clock.begin_frame(5000, true);
    CHECK(menu.ticks == 0 && !menu.paced);
    (void)clock.note_frame(0);
    CHECK(clock.begin_frame(kFrameMs, false).ticks == 1);

    clock.set_skipping(true);
    const FramePlan skipped = clock.begin_frame(kFrameMs, false);
    CHECK(skipped.skipping && skipped.ticks == 0 && !skipped.draw_last && !skipped.paced);
    CHECK(clock.speed_line() == "Skip");
    clock.set_skipping(false);
    CHECK(clock.begin_frame(kFrameMs, false).ticks == 1);
}

void check_reached_speed() {
    // 16x over 100 ms owes 48 ticks. 27 of them is 9x.
    PlaybackClock clock;
    clock.set_speed(PlaybackSpeed::sixteen);
    clock.set_recorded_speed(kRecorded);
    const FramePlan plan = clock.begin_frame(100, false);
    CHECK(plan.ticks == 48);
    const auto result = clock.note_frame(27);
    CHECK(result.reached_lower);
    CHECK(result.asked_milli == 16000);
    CHECK(result.reached_milli == 9000);
    CHECK(clock.speed_line() == "16× · 9× now");
    // A pause does not wipe the speed the last paced frame reached.
    clock.pause();
    CHECK(!clock.begin_frame(17, false).paced);
    (void)clock.note_frame(0);
    CHECK(clock.speed_line() == "16× · 9× now");

    PlaybackClock full;
    full.set_speed(PlaybackSpeed::sixteen);
    full.set_recorded_speed(kRecorded);
    CHECK(full.begin_frame(kFrameMs, false).ticks == 8);
    const auto ran_all = full.note_frame(8);
    CHECK(!ran_all.reached_lower);
    CHECK(full.speed_line() == "16×");

    PlaybackClock half;
    half.set_speed(PlaybackSpeed::half);
    CHECK(half.speed_line() == "½×");

    // A frame twice as long plans twice the ticks. Running the tick it planned
    // kept the asked speed: the lateness is not itself a shortfall.
    PlaybackClock late;
    late.set_recorded_speed(kRecorded);
    CHECK(late.begin_frame(17, false).ticks == 0);
    (void)late.note_frame(0);
    CHECK(late.speed_line() == "1×");
    CHECK(late.begin_frame(34, false).ticks == 1);
    const auto late_result = late.note_frame(1);
    CHECK(!late_result.reached_lower);
    CHECK(late_result.reached_milli == 1000);
    CHECK(late.speed_line() == "1×");

    // A stall past the cap plans only the cap, and the speed reached falls.
    PlaybackClock stall;
    stall.set_recorded_speed(kRecorded);
    const FramePlan stalled = stall.begin_frame(500, false);
    CHECK(stalled.ticks == 7);
    const auto stall_result = stall.note_frame(7);
    CHECK(stall_result.reached_lower);
    CHECK(stall_result.reached_milli == 500);
    CHECK(stall.speed_line() == "1× · ½× now");

    // The first frame carries a fraction. Pause holds it, so the next frame
    // after play owes one tick and not the time the pause held.
    PlaybackClock carried;
    carried.set_recorded_speed(kRecorded);
    CHECK(carried.begin_frame(17, false).ticks == 0);
    (void)carried.note_frame(0);
    carried.pause();
    CHECK(carried.begin_frame(5000, false).ticks == 0);
    (void)carried.note_frame(0);
    carried.play();
    CHECK(carried.begin_frame(17, false).ticks == 1);
}

void check_pause_during_jump() {
    // A Pause during a jump sets the world bit. The landing keeps that pause,
    // and the caller clears the bit so the skipped ticks do not.
    bool clear = false;
    CHECK(fold_jump_pause(false, true, &clear));
    CHECK(clear);
    // The next press sets the bit again, and the landing plays.
    clear = false;
    CHECK(!fold_jump_pause(true, true, &clear));
    CHECK(clear);
    // A frame whose bit is clear leaves the pause the jump already held.
    clear = true;
    CHECK(fold_jump_pause(true, false, &clear));
    CHECK(!clear);
    CHECK(!fold_jump_pause(false, false, nullptr));
}

void check_sound() {
    SoundGate gate;
    CHECK(gate.allow("laser", 0));
    CHECK(!gate.allow("laser", 999));
    CHECK(gate.allow("cannon", 999));
    CHECK(gate.allow("laser", 1000));
    CHECK(!gate.allow("laser", 1999));
    CHECK(gate.allow("laser", 2000));

    PlaybackClock quiet;
    quiet.set_speed(PlaybackSpeed::quadruple);
    CHECK(!quiet.thins_sound());
    CHECK(quiet.allows_unit_sound("laser", 0));
    CHECK(quiet.allows_unit_sound("laser", 0));
    quiet.set_speed(PlaybackSpeed::octuple);
    CHECK(quiet.thins_sound());
    CHECK(quiet.allows_unit_sound("laser", 0));
    CHECK(!quiet.allows_unit_sound("laser", 500));
    CHECK(quiet.allows_unit_sound("cannon", 500));
    quiet.set_speed(PlaybackSpeed::normal);
    quiet.set_skipping(true);
    CHECK(quiet.thins_sound());
    CHECK(quiet.allows_unit_sound("missile", 0));
    CHECK(!quiet.allows_unit_sound("missile", 10));
}

void check_schedule() {
    CHECK(oa::session::demo::keyframe_due(0));
    CHECK(oa::session::demo::keyframe_due(oa::session::demo::keyframe_near_interval_ticks));
    CHECK(!oa::session::demo::keyframe_due(1));
    CHECK(oa::session::demo::keyframe_sparse(0));
    CHECK(oa::session::demo::keyframe_sparse(oa::session::demo::keyframe_far_interval_ticks));
    CHECK(!oa::session::demo::keyframe_sparse(oa::session::demo::keyframe_near_interval_ticks));

    constexpr uint32_t kHead = 10000;
    KeyframeReel reel;
    for (uint32_t play_tick = 0; play_tick <= kHead; play_tick += 300)
        reel.add(play_tick, 10);
    const std::vector<uint32_t> dropped = reel.thin(kHead);
    CHECK(!reel.contains(300));
    CHECK(!reel.contains(600));
    CHECK(!reel.contains(6000));
    CHECK(reel.contains(0));
    CHECK(reel.contains(1800));
    CHECK(reel.contains(3600));
    CHECK(reel.contains(5400));
    CHECK(reel.contains(7200));
    CHECK(reel.contains(9000));
    CHECK(reel.contains(9900));
    bool dropped_near = false;
    for (const uint32_t play_tick : dropped) {
        CHECK(!oa::session::demo::keyframe_kept(play_tick, kHead));
        if (play_tick == 300)
            dropped_near = true;
    }
    CHECK(dropped_near);
}

void check_eviction() {
    // The head sits between a sparse keyframe and a dense one at the same
    // distance. The budget holds the base and two keyframes, so the dense one
    // goes and the sparse one stays. A tighter budget then drops the further
    // of what remains.
    constexpr uint32_t kHead = 1500;
    KeyframeReel reel;
    reel.set_base_bytes(100);
    reel.set_budget(200);
    reel.add(0, 50);
    reel.add(1800, 50);
    reel.add(3000, 50);
    CHECK(reel.total_bytes() == 250);
    const std::vector<uint32_t> first = reel.evict(kHead);
    CHECK(first.size() == 1);
    CHECK(first[0] == 3000);
    CHECK(reel.contains(0));
    CHECK(reel.contains(1800));
    CHECK(!reel.contains(3000));
    CHECK(reel.total_bytes() == 200);

    reel.set_budget(150);
    const std::vector<uint32_t> second = reel.evict(kHead);
    CHECK(second.size() == 1);
    CHECK(second[0] == 0);
    CHECK(!reel.contains(0));
    CHECK(reel.contains(1800));

    // Working memory counts once, and forget drops one tick.
    reel.set_working_bytes(40);
    CHECK(reel.working_bytes() == 40);
    CHECK(reel.total_bytes() == reel.base_bytes() + 40 + 50);
    reel.forget(1800);
    CHECK(!reel.contains(1800));
    CHECK(reel.total_bytes() == reel.base_bytes() + 40);
    reel.forget(1800);
    CHECK(reel.size() == 0);
}

void check_seek_plan() {
    KeyframeReel reel;
    reel.add(0, 1);
    reel.add(300, 1);
    reel.add(600, 1);
    reel.add(900, 1);
    const SeekPlan back = reel.plan_seek(900, 100, 900);
    CHECK(back.kind == SeekPlan::Kind::restore);
    CHECK(back.restore_tick == 0);
    CHECK(!back.past_furthest);
    const SeekPlan within = reel.plan_seek(300, 800, 900);
    CHECK(within.kind == SeekPlan::Kind::restore);
    CHECK(within.restore_tick == 600);
    const SeekPlan ahead = reel.plan_seek(100, 5000, 900);
    CHECK(ahead.past_furthest);
    CHECK(ahead.kind == SeekPlan::Kind::restore);
    CHECK(ahead.restore_tick == 900);
    const SeekPlan same = reel.plan_seek(300, 300, 900);
    CHECK(same.kind == SeekPlan::Kind::stay);
    CHECK(oa::session::demo::catch_up_percent(0, 62, 100) == 62);
    CHECK(oa::session::demo::catch_up_percent(1000, 1000, 2000) == 0);
    CHECK(oa::session::demo::catch_up_percent(1000, 2000, 2000) == 100);

    PlaybackClock clock;
    clock.note_tick(0);
    clock.reel().add(0, 1);
    clock.note_tick(900);
    clock.reel().add(900, 1);
    clock.note_length(900);
    const auto state = clock.state(300);
    CHECK(state.tick == 300);
    CHECK(state.furthest == 900);
    CHECK(state.length_known == 900);
    CHECK(state.ready.size() == 2);
    CHECK(state.ready[0].begin == 0 && state.ready[0].end == 899);
    CHECK(state.ready[1].begin == 900 && state.ready[1].end == 900);
    clock.set_catch_up(62);
    CHECK(clock.state(300).catch_up_percent == 62);
    clock.set_unavailable("Jumping back isn't available in this replay.");
    CHECK(clock.state(300).unavailable == "Jumping back isn't available in this replay.");
    clock.set_unavailable({});
    CHECK(clock.state(300).unavailable.empty());
}

} // namespace

int main() {
    check_speeds();
    check_pause_and_skip();
    check_reached_speed();
    check_pause_during_jump();
    check_sound();
    check_schedule();
    check_eviction();
    check_seek_plan();
    if (failures != 0) {
        std::fprintf(stderr, "session-demo-clock: %d checks failed\n", failures);
        return 1;
    }
    std::printf("session-demo-clock: ok\n");
    return 0;
}
