// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// When the running match's clock steps: the rules that hold a match on
// this machine alone for its menus and its outcome, the pause bit of
// Game.sim_run_flags that holds every match, whoever set it, a frame whose
// clock reading lies behind the clock's last step, and what a match does
// while the window is away.
#pragma once

#include <cstdint>

namespace oa::app {

/// Tells whether the running match's clock steps this frame.
///
/// A match played on this machine alone holds while a menu is open or once
/// it is finished. A match shared with other players' machines never holds
/// for a menu, and keeps stepping while it waits on its outcome, until it
/// leaves for the end-of-game screen. The pause bit is apart from this: it
/// holds the steps inside the clock (clock_flags_with_pause).
///
/// @param shared_match the match is played with other players' machines
///        (extension_state::shared_match)
/// @param menu_open an in-game menu or panel is open over the match
/// @param finished the match shows its victory or defeat
/// @return true when the clock advances and offers its pending steps
[[nodiscard]] bool match_clock_runs(bool shared_match, bool menu_open, bool finished) noexcept;

/// Returns the match clock's run flags with the pause bit of Game.sim_run_flags.
///
/// The clock offers no step while bit 0 of its run flags is set and keeps
/// its time moving, so a match that resumes does not catch up the paused
/// time. The other bits, which the clock keeps itself, are kept.
///
/// @param clock_flags the clock's run flags (base::game_loop::Timing::flags)
/// @param sim_run_flags the running match's Game.sim_run_flags
/// @return clock_flags with bit 0 taken from sim_run_flags
[[nodiscard]] uint16_t
clock_flags_with_pause(uint16_t clock_flags, uint16_t sim_run_flags) noexcept;

/// Tells whether a frame's clock reading lies behind the match clock's last step.
///
/// A clock set during a frame past the frame's own time (a load, a
/// screenshot or a film frame sets it to the moment it ends) reads behind
/// it, and the frame holds its step, so that the next frame steps from there
/// and the time left out stays out. The reading turns over to 0 about every
/// 39.8 hours (base::game_loop::scaled_clock_turn); one that has turned over
/// since the last step lies after it (base::game_loop::scaled_clock_before):
/// the frame steps, the step runs no tick for that frame, and the frames
/// after it step as before, as in 3.1c.
///
/// @param reading the frame's clock reading, in clock units
/// @param previous_clock the reading the clock last stepped at
///        (base::game_loop::Timing::previous_clock)
/// @return true when the frame holds its step
[[nodiscard]] bool clock_reading_behind(uint32_t reading, uint32_t previous_clock) noexcept;

/// What decides what a match does while the window is away: without the
/// focus, minimised, or with the application in the background.
struct AwayFacts {
    /// Settings › Controls › When the window loses focus is Pause single
    /// player; Keep playing otherwise
    bool pause_chosen{};
    bool match_shown{}; ///< the match screen shows a running match
    bool finished{};    ///< the match shows its victory or defeat
    /// a multiplayer session is open (extension_state::multiplayer or the
    /// Game block's live-game bit), hosting with only computer players
    /// included, or the match is shared with other machines
    bool network{};
    bool replay{}; ///< the match replays a recording (extension_state::replay)
    /// the run is frame-limited, remote-controlled, a director's film or a
    /// bench, which runs on without the focus
    bool scripted{};
};

/// What a match does while the window is away.
enum class AwayAction : uint8_t {
    /// nothing changes: the menus, a finished match, a multiplayer game, a
    /// replay or a scripted run
    none,
    /// a game played alone pauses, and goes on a moment after the window
    /// is back
    pause,
    /// a game played alone plays on, the main loop running every frame
    keep_playing,
};

/// Tells what a match does while the window is away.
///
/// Only a game played on this machine alone, a campaign mission or a
/// skirmish against computer players, answers to When the window loses
/// focus. A multiplayer game never pauses for it, nor does a replay, and
/// the menus have no match to pause.
///
/// @param facts the match and the setting
/// @return pause or keep_playing for a running game played alone, as the
///         setting says; none for anything else
[[nodiscard]] AwayAction away_action(const AwayFacts& facts) noexcept;

} // namespace oa::app
