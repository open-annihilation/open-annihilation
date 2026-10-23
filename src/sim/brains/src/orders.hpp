// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Orders a seat gives its units, and the building-site checks beside them.
#pragma once

#include "internal.hpp"

struct lua_State;

namespace oa::sim::brains {

/// Installs the unit order methods, `brain.order_counts`, `brain.site` and
/// `brain.place`.
///
/// Works when `seat` has no match: the methods are registered, the counts
/// read zero, and the site checks return nil. May raise.
///
/// @param state sandbox state
/// @param seat the seat being opened
void install_orders(lua_State* state, Seat& seat);

/// Installs `brain.site` and `brain.place`. `install_orders` calls this.
///
/// Works when `seat` has no match. May raise.
///
/// @param state sandbox state
/// @param seat the seat being opened
void install_site_functions(lua_State* state, Seat& seat);

/// Clears the turn's order counts. The match totals stay.
///
/// Called at the start of a live turn. `start` is the turn before the first
/// of these calls, so a fresh seat is already on that turn.
///
/// @param seat the seat whose turn is starting
void begin_order_turn(Seat& seat) noexcept;

} // namespace oa::sim::brains
