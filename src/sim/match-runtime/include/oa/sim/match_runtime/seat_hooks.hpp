// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A computer seat's turn, installed by whoever plays that seat in place of
// the built-in computer player. The match holds the hooks and nothing else:
// it does not know what they point at, and it does not save, hash or share
// them.
#pragma once

#include <array>
#include <cstdint>

namespace oa::sim::match_runtime {

class Match;

/// One draw the built-in player took while creating a seat's controller.
struct SetUpDraw {
    /// The bound passed to the shared stream.
    uint32_t bound{};
    /// The value the stream returned.
    uint32_t value{};
};

/// The built-in player's set-up of one seat: the stream just before its first
/// set-up draw, and the draws that followed.
struct SetUpRecord {
    /// `Match::random_state` just before this seat's first set-up draw.
    uint32_t stream_state{};
    /// The draws, in order. `draw_count` says how many are filled.
    std::array<SetUpDraw, 16> draws{};
    /// How many `draws` are filled. The set-up takes eight.
    uint8_t draw_count{};
    /// True when +AI handed the seat to the computer after the start.
    bool handed_over{};
};

/// One computer seat's turn, called at its controller point.
///
/// Every member starts null. A null `play` plays nothing, and the built-in
/// computer player plays the tick.
struct SeatHooks {
    /// Pointer the callbacks receive. Null when the seat has no hooks.
    void* context{};

    /// Plays the seat's turn at its controller point.
    ///
    /// Null plays nothing. The built-in computer player then plays this tick.
    ///
    /// @param context SeatHooks::context
    /// @param match the match
    /// @param player player index, 0 to 9
    /// @return true when the seat played this tick; false lets the built-in
    ///     computer player play it
    bool (*play)(void* context, Match& match, uint8_t player){};

    /// Called when the built-in player has created its controller for the seat.
    ///
    /// `record` holds the stream's state just before that seat's first set-up
    /// draw, and the draws. Null is not called.
    ///
    /// @param context SeatHooks::context
    /// @param match the match
    /// @param player player index, 0 to 9
    /// @param record the seat's set-up draws
    void (*set_up)(void* context, Match& match, uint8_t player, const SetUpRecord& record){};
};

} // namespace oa::sim::match_runtime
