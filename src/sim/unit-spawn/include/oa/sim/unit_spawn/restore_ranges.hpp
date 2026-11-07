// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/core/world.h"

#include <cstdint>
#include <span>

namespace oa::sim::unit_spawn {

/// Restores the saved owner order of an empty Enhanced Mode unit pool.
///
/// Assigns equal ranges in the supplied order, clearing other players'
/// ranges. Unit ids stay unchanged. Call before restoring unit records,
/// then rebind any views of the player ranges.
///
/// @param[in,out] world empty pool with canonical unit ids and its original per-player limit
/// @param order unique player slots, 0 through 9, in physical range order
/// @return false without changes for invalid order, mismatched capacity,
///         noncanonical ids, or a pool containing any occupied or live unit
bool restore_unit_player_order(oa::World& world, std::span<const uint8_t> order) noexcept;

} // namespace oa::sim::unit_spawn
