// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Orders a seat gives its units.
#pragma once

#include "internal.hpp"

struct lua_State;

namespace oa::sim::brains {

/// Installs the unit order methods and `brain.order_counts`.
///
/// Works when `seat` has no match: the methods are registered and the counts
/// read zero. May raise.
///
/// @param state sandbox state
/// @param seat the seat being opened
void install_orders(lua_State* state, Seat& seat);

/// Clears the turn's order counts. The match totals stay.
///
/// Called at the start of a live turn. `start` is the turn before the first
/// of these calls, so a fresh seat is already on that turn.
///
/// @param seat the seat whose turn is starting
void begin_order_turn(Seat& seat) noexcept;

} // namespace oa::sim::brains
