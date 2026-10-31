// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// When the match clock steps: every combination of a shared match, an open
// menu and a finished match, the pause bit of Game.sim_run_flags taken
// into the clock's run flags, whose clock then offers no step, and frames
// whose reading lies behind the clock's last step, or has turned over to 0
// on a clock just before 2^32 milliseconds, where the match keeps stepping;
// and what a match does while the window is away, for every kind of match.
#include "match_clock.hpp"

#include "oa/base/game_loop.hpp"

#include <cstdint>
#include <cstdio>
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

void test_single_machine_match_holds() {
    using oa::app::match_clock_runs;
    CHECK(match_clock_runs(false, false, false));
    CHECK(!match_clock_runs(false, true, false));
    CHECK(!match_clock_runs(false, false, true));
    CHECK(!match_clock_runs(false, true, true));
}

void test_shared_match_runs_on() {
    using oa::app::match_clock_runs;
    CHECK(match_clock_runs(true, false, false));
    CHECK(match_clock_runs(true, true, false));
    CHECK(match_clock_runs(true, false, true));
    CHECK(match_clock_runs(true, true, true));
}

void test_pause_bit_is_taken() {
    using oa::app::clock_flags_with_pause;
    CHECK(clock_flags_with_pause(0x0000, 0x0001) == 0x0001);
    CHECK(clock_flags_with_pause(0x0001, 0x0000) == 0x0000);
    // The clock's own bits stay; the match's other bits do not come across.
    CHECK(clock_flags_with_pause(0x0006, 0x00f1) == 0x0007);
    CHECK(clock_flags_with_pause(0x0007, 0x00f0) == 0x0006);
}

void test_paused_clock_offers_no_step() {
    oa::base::game_loop::Timing timing{};
    timing.requested_rate = 10;
    timing.actual_rate = 10;
    CHECK(oa::base::game_loop::update_timing(timing, 1000) == oa::base::game_loop::LoopError::none);
    // Paused: a second of clock units offers nothing, and the time moves on.
    timing.flags = oa::app::clock_flags_with_pause(timing.flags, 0x0001);
    CHECK(oa::base::game_loop::update_timing(timing, 1030) == oa::base::game_loop::LoopError::none);
    CHECK(timing.pending_steps == 0);
    CHECK(timing.previous_clock == 1030);
    // Resumed: the next frame steps for its own time only.
    timing.flags = oa::app::clock_flags_with_pause(timing.flags, 0x0000);
    CHECK(oa::base::game_loop::update_timing(timing, 1031) == oa::base::game_loop::LoopError::none);
    CHECK(timing.pending_steps == 1);
}

/// The match clock's units a second.
constexpr uint32_t kUnitsPerSecond = 30;
/// Milliseconds at which the millisecond clock turns over, with the reading.
constexpr uint64_t kMillisecondTurn = uint64_t{1} << 32U;

/// Returns the match clock's reading at a time on the steady clock, from
/// its milliseconds' low 32 bits, as the application loop reads it.
///
/// @param time_ns nanoseconds on the steady clock
/// @return the reading, in clock units
uint32_t reading_at(uint64_t time_ns) {
    return oa::base::game_loop::scaled_clock(
        static_cast<uint32_t>(time_ns / 1'000'000U), kUnitsPerSecond
    );
}

/// Ticks each frame ran.
struct FrameRun {
    std::vector<int32_t> ticks; ///< ticks each frame ran, in order
    bool turned = false;        ///< a frame's reading lay below the one before
};

/// Plays frames evenly spaced on a steady clock, each stepping the match
/// clock unless its reading lies behind the last step, as the application
/// loop does at normal speed.
///
/// @param start_ns the first frame's period's start, nanoseconds
/// @param frames_per_second the frames' rate
/// @param frames frames to play
/// @return the ticks each frame ran
FrameRun play_frames(uint64_t start_ns, uint32_t frames_per_second, uint32_t frames) {
    oa::base::game_loop::Timing timing{};
    timing.requested_rate = oa::base::game_loop::normal_game_speed;
    timing.actual_rate = oa::base::game_loop::normal_game_speed;
    // The clock last stepped at the middle of the frame before.
    const uint64_t half_frame_ns = 1'000'000'000U / (2 * uint64_t{frames_per_second});
    timing.previous_clock = reading_at(start_ns - half_frame_ns);
    FrameRun run{};
    uint32_t last_reading = timing.previous_clock;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        // Each frame stands for the middle of its period.
        const uint64_t time_ns = start_ns + (2 * uint64_t{frame} + 1) * 1'000'000'000U /
                                                (2 * uint64_t{frames_per_second});
        const uint32_t reading = reading_at(time_ns);
        run.turned = run.turned || reading < last_reading;
        last_reading = reading;
        if (oa::app::clock_reading_behind(reading, timing.previous_clock)) {
            run.ticks.push_back(0);
            continue;
        }
        CHECK(
            oa::base::game_loop::update_timing(timing, reading) ==
            oa::base::game_loop::LoopError::none
        );
        run.ticks.push_back(timing.pending_steps);
    }
    return run;
}

void test_reading_behind_the_last_step() {
    using oa::app::clock_reading_behind;
    constexpr uint32_t kTurn = oa::base::game_loop::scaled_clock_turn;
    // A clock set past the frame's time holds the frame's step.
    CHECK(clock_reading_behind(1000, 1003));
    CHECK(clock_reading_behind(0, 1));
    CHECK(clock_reading_behind(kTurn - 5, kTurn));
    // At or after the last step, the frame steps.
    CHECK(!clock_reading_behind(1003, 1003));
    CHECK(!clock_reading_behind(1004, 1003));
    CHECK(!clock_reading_behind(kTurn, 0));
    // A reading turned over to 0 lies after the last step.
    CHECK(!clock_reading_behind(0, kTurn));
    CHECK(!clock_reading_behind(3, kTurn - 2));
    // Half a turn behind is as far back as a reading lies before the step.
    CHECK(clock_reading_behind(0, kTurn / 2 - 1));
    CHECK(!clock_reading_behind(0, kTurn / 2));
}

void test_clock_set_past_the_frame_holds_one_frame() {
    oa::base::game_loop::Timing timing{};
    timing.requested_rate = oa::base::game_loop::normal_game_speed;
    timing.actual_rate = oa::base::game_loop::normal_game_speed;
    // A load during the frame read 1000 set the clock to the moment it
    // ended, 90 units later: the frame holds, and the next frame steps for
    // its own time only.
    timing.previous_clock = 1090;
    CHECK(oa::app::clock_reading_behind(1000, timing.previous_clock));
    CHECK(!oa::app::clock_reading_behind(1091, timing.previous_clock));
    CHECK(oa::base::game_loop::update_timing(timing, 1091) == oa::base::game_loop::LoopError::none);
    CHECK(timing.pending_steps == 1);
}

/// Returns the ticks a run of frames ran.
///
/// @param run the run
/// @param from the first frame counted
/// @return the ticks the frames from `from` on ran
int32_t ticks_from(const FrameRun& run, std::size_t from) {
    int32_t total = 0;
    for (std::size_t frame = from; frame < run.ticks.size(); ++frame)
        total += run.ticks[frame];
    return total;
}

void test_match_steps_across_the_turn() {
    // Frames from 2 seconds before the clock turns over, as the milliseconds
    // turn over at 2^32 and as the reading alone does at the first turn,
    // against the same frames far from either.
    constexpr uint64_t kLead = 2'000'000'000;
    constexpr uint64_t kAway = 1'000'000'000'000;
    for (const uint64_t turn_ns : {kMillisecondTurn * 1'000'000U, uint64_t{143'165'577'000'000}}) {
        // At 30 frames a second each frame runs a tick, but for the one whose
        // reading turned over, which runs none; the frames after it step as
        // before.
        const FrameRun away = play_frames(kAway, kUnitsPerSecond, 150);
        const FrameRun one = play_frames(turn_ns - kLead, kUnitsPerSecond, 150);
        CHECK(!away.turned && one.turned);
        CHECK(ticks_from(away, 0) == 150);
        uint32_t idle = 0;
        std::size_t idle_frame = 0;
        for (std::size_t frame = 0; frame < one.ticks.size(); ++frame) {
            CHECK(one.ticks[frame] == 0 || one.ticks[frame] == 1);
            if (one.ticks[frame] == 0) {
                ++idle;
                idle_frame = frame;
            }
        }
        CHECK(idle == 1);
        CHECK(idle_frame == 60);
        CHECK(ticks_from(one, 0) == 149);
        // At 120 frames a second the clock keeps 30 ticks a second through
        // the turn, one fewer than far from it.
        const FrameRun quick_away = play_frames(kAway, 120, 600);
        const FrameRun quick = play_frames(turn_ns - kLead, 120, 600);
        CHECK(quick.turned);
        CHECK(ticks_from(quick_away, 0) == 150);
        CHECK(ticks_from(quick, 0) == 149);
        CHECK(ticks_from(quick, 480) == 30);
    }
}

void test_away_answers_only_a_game_played_alone() {
    using oa::app::away_action;
    using oa::app::AwayAction;
    using oa::app::AwayFacts;
    // A running campaign mission or skirmish against computer players
    // pauses or plays on, as the setting says.
    AwayFacts alone;
    alone.match_shown = true;
    alone.pause_chosen = true;
    CHECK(away_action(alone) == AwayAction::pause);
    alone.pause_chosen = false;
    CHECK(away_action(alone) == AwayAction::keep_playing);
    // Anything else is left alone under either choice: the menus, a finished
    // match, a multiplayer game (hosting with only computer players
    // included), a replay and a scripted run.
    for (const bool pause_chosen : {true, false}) {
        AwayFacts facts;
        facts.pause_chosen = pause_chosen;
        CHECK(away_action(facts) == AwayAction::none);
        facts.match_shown = true;
        AwayFacts finished = facts;
        finished.finished = true;
        CHECK(away_action(finished) == AwayAction::none);
        AwayFacts network = facts;
        network.network = true;
        CHECK(away_action(network) == AwayAction::none);
        AwayFacts replay = facts;
        replay.replay = true;
        CHECK(away_action(replay) == AwayAction::none);
        AwayFacts scripted = facts;
        scripted.scripted = true;
        CHECK(away_action(scripted) == AwayAction::none);
    }
}

} // namespace

int main() {
    test_single_machine_match_holds();
    test_shared_match_runs_on();
    test_pause_bit_is_taken();
    test_paused_clock_offers_no_step();
    test_reading_behind_the_last_step();
    test_clock_set_past_the_frame_holds_one_frame();
    test_match_steps_across_the_turn();
    test_away_answers_only_a_game_played_alone();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
