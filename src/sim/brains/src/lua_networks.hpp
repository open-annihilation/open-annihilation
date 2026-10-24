// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// brain.network, brain.tensor and the tensor methods. install_networks is
// called from install_libraries and does not read the seat's match.
#pragma once

#include <cstdint>

struct lua_State;

namespace oa::sim::brains {

struct Seat;

/// Adds `brain.network` and `brain.tensor`.
///
/// Works when the seat has no match. The functions read networks the load
/// already built. May raise only if the state cannot hold the metatables.
///
/// @param state sandbox state
/// @param seat the seat being opened; its match is not read
void install_networks(lua_State* state, Seat& seat);

/// Pushes a zero tensor and returns its elements.
///
/// The elements are `int32` counts of 65536ths, in one immutable userdata.
/// `rank` is 1 to 4. The product of the first `rank` dimensions is the
/// element count, at most 2^22. The pointer addresses that userdata and
/// stays valid while the value remains on the stack. Allocation may raise.
///
/// @param state sandbox state
/// @param rank 1 to 4
/// @param dims the shape; the first `rank` entries are read
/// @return the elements, or null after a raising call
int32_t* push_zero_tensor(lua_State* state, uint8_t rank, const uint32_t dims[4]);

} // namespace oa::sim::brains
