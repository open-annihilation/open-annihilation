// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// brain.grid. install_grids is called from install_libraries and does not
// read the seat's match. Outside a match the function returns nil.
#pragma once

struct lua_State;

namespace oa::sim::brains {

struct Seat;

/// Adds `brain.grid`.
///
/// Works when the seat has no match: the function is registered and a later
/// call returns nil. May raise only if the state cannot hold the function.
///
/// @param state sandbox state
/// @param seat the seat being opened; its match is not read
void install_grids(lua_State* state, Seat& seat);

} // namespace oa::sim::brains
