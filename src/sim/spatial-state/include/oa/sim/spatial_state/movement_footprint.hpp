// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

namespace oa::sim::spatial_state {

struct CellClassifier;
struct World;

/// Classifies a complete movement footprint without its perimeter.
///
/// Plots are checked in row-major order. A slower plot does not hide a later
/// blocked plot. The footprint's far edge must remain strictly inside the map.
///
/// @param movement footprint, water depths, slope bands and occupancy tick
/// @param x footprint origin column
/// @param z footprint origin row
/// @param world world holding the plots, units and sea level
/// @return 0 for blocked, invalid or off-map footprints, 1 when any plot is
///     within only the maximum slope, 3 when all plots meet the bad-slope limit
[[nodiscard]] uint8_t classify_movement_footprint(
    const CellClassifier& movement, int32_t x, int32_t z, const World& world
) noexcept;

} // namespace oa::sim::spatial_state
