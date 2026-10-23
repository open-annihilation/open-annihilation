// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Connected regions of one movement map. A reachability question builds them
// once per tick per class. It does not search for a path.
#pragma once

#include "oa/sim/ground_orders/movement_map.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace oa::sim::brains {

/// How many classes keep labels at once. A further class frees the oldest.
inline constexpr std::size_t region_slots = 4;

/// Labels of one class for one tick. Label 0 is not a region.
struct ClassRegions {
    /// Movement-class handle. 0 means the slot is empty.
    uint32_t handle{};
    /// Match tick the labels were built for.
    uint32_t tick{};
    /// Map width in cells.
    uint32_t width{};
    /// Map height in cells.
    uint32_t height{};
    /// One label per cell, row z then column x. Null when the slot is empty.
    std::unique_ptr<uint32_t[]> labels{};
};

/// One seat's region labels and the fill queue, grown to the largest map.
struct RegionCache {
    /// Slots in no particular order. An empty slot has handle 0.
    ClassRegions slot[region_slots]{};
    /// Breadth-first queue, one entry per cell. Made once and grown if a
    /// later map has more cells.
    std::unique_ptr<uint32_t[]> queue{};
    /// Cells `queue` can hold.
    uint32_t queue_cells{};
};

/// Reports whether cell (x, z) is passable for this fill.
///
/// A null pass function reads the movement map: a cell is passable when its
/// value is at least 1.
using CellPass = bool (*)(void* context, uint32_t x, uint32_t z);

/// What one fill reads. The map and the context must outlive the call.
struct RegionBuild {
    /// The class's movement map.
    const sim::ground_orders::MovementMap* map{};
    /// Class handle stored with the labels.
    uint32_t handle{};
    /// Tick the labels belong to.
    uint32_t tick{};
    /// Passed to `pass`. When `pass` is null, this is ignored and the map is read.
    void* context{};
    /// Passability for this seat. Null reads the map as it is.
    CellPass pass{};
};

/// Builds labels for `build` when this tick has none for that class.
///
/// The first passable cell in row order starts region 1, then the next
/// unlabelled passable cell starts region 2, and so on. Eight neighbours
/// share a region, with no corner check. At most four classes are kept; a
/// fifth frees the labels with the oldest tick, and the lower handle when
/// the ticks match. A class asked again on a later tick is rebuilt in its
/// own slot.
///
/// @param cache the seat's labels and queue
/// @param build the map, the class, the tick and the pass test
/// @return the build charge `(width * height + 15) / 16`, or 0 when the
///     labels were already built for this tick
uint64_t ensure_regions(RegionCache& cache, const RegionBuild& build);

/// Reports whether two anchor neighbourhoods meet in one region.
///
/// True when some passable cell of the 3 by 3 around `a` and some passable
/// cell of the 3 by 3 around `b` carry the same label. Cells off the map are
/// skipped. False when the class has no labels.
///
/// @param cache the seat's labels
/// @param handle the class
/// @param ax anchor column of the first position
/// @param az anchor row of the first position
/// @param bx anchor column of the second position
/// @param bz anchor row of the second position
/// @return true when the neighbourhoods share a region
[[nodiscard]] bool regions_meet(
    const RegionCache& cache, uint32_t handle, int32_t ax, int32_t az, int32_t bx, int32_t bz
) noexcept;

/// Returns the labels kept for `handle`, or null when that class has none.
///
/// @param cache the seat's labels
/// @param handle the class
/// @return the slot, or null
[[nodiscard]] const ClassRegions* regions_for(const RegionCache& cache, uint32_t handle) noexcept;

/// Returns one cell's label, or 0 when the class has none or the cell is off the map.
///
/// @param cache the seat's labels
/// @param handle the class
/// @param x cell column
/// @param z cell row
/// @return the label
[[nodiscard]] uint32_t
region_label(const RegionCache& cache, uint32_t handle, uint32_t x, uint32_t z) noexcept;

/// Returns how many cells the fill queue can hold.
///
/// @param cache the seat's labels
/// @return the queue length
[[nodiscard]] uint32_t region_queue_cells(const RegionCache& cache) noexcept;

/// Returns the anchor cell of a position for a footprint.
///
/// Each axis is `(position - footprint * 524288 + 524288)` with wrapping
/// arithmetic and an arithmetic shift of 20, the cell a spawned unit records.
///
/// @param x position x, 16.16
/// @param z position z, 16.16
/// @param footprint_x footprint width in cells
/// @param footprint_z footprint depth in cells
/// @param cx receives the anchor column
/// @param cz receives the anchor row
void anchor_cell(
    int32_t x, int32_t z, int16_t footprint_x, int16_t footprint_z, int32_t* cx, int32_t* cz
) noexcept;

} // namespace oa::sim::brains
