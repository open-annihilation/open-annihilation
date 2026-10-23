// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A computer seat's turn, installed by whoever plays that seat in place of
// the built-in computer player. The match holds the hooks and nothing else:
// it does not know what they point at, and it does not save, hash or share
// them.
#pragma once

#include <cstdint>

namespace oa::sim::match_runtime {

class Match;

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
};

} // namespace oa::sim::match_runtime
