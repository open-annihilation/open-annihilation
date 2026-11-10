// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/core/unit.h"
#include "oa/sim/ground_orders/goals.hpp"
#include "oa/sim/ground_orders/navigator.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace oa::sim::spatial_state {
struct CellClassifier;
struct World;
} // namespace oa::sim::spatial_state

namespace oa::sim::ground_orders {

/// Tests the static footprint without scanning its speed-class perimeter.
///
/// This has the same blocked/passable result as classify_movement_cell with
/// occupancy_before_tick zero: buildings and features block; mobiles do not.
///
/// @param movement movement class footprint, slopes and water limits
/// @param x footprint origin column
/// @param z footprint origin row
/// @param world canonical spatial plots and occupants
/// @return whether the complete static footprint is passable
[[nodiscard]] bool static_flow_passable(
    const sim::spatial_state::CellClassifier& movement,
    int32_t x,
    int32_t z,
    const sim::spatial_state::World& world
) noexcept;

/// Bounded shared-route storage and work. Construction allocates; ticks do not.
struct FlowLimits {
    uint32_t fields{64};
    uint32_t total_cells{2'097'152};
    uint32_t work_per_tick{2048};
    uint32_t route_probes_per_tick{65'536};
};

/// Selects immutable shared-route grid, key, prefix and blocked-field behavior.
/// The original preset retains the version-two match behavior and format.
enum class FlowPreset : uint8_t { original, large_regions, fine_regions };

/// A movement class and a player's knowledge of the static map.
struct FlowContext {
    uint32_t movement_class{}; ///< stable movement-class table index, 0 through 31
    uint32_t player{};         ///< player slot, 0 through 9
    uint32_t static_revision{};
    uint64_t knowledge_epoch{};
    uint32_t footprint_x{};
    uint32_t footprint_z{};
};

/// Reads static passability without treating moving units as walls.
struct FlowSampler {
    void* context{};
    /// Returns 0 blocked, otherwise passable; unknown terrain must not reveal a wall.
    uint8_t (*cell)(
        void* context, uint32_t movement_class, uint32_t player, int32_t x, int32_t z
    ){};
};

/// Whether a shared route can replace a full per-unit search.
enum class FlowResult : uint8_t { fallback, pending, ready };

/// Cumulative work counters, excluding ordinary full searches.
struct FlowCounters {
    uint64_t requests{}, hits{}, fields_started{}, cells_cleared{}, cells_expanded{};
    uint64_t waits{}, invalidations{}, fallbacks{};
};

/// Shares bounded reverse fields between nearby destinations of one movement class/player.
///
/// A coarse node lies every two or four map cells. Every connection is checked at
/// footprint-cell resolution, including diagonal corner clearance. Narrow
/// corridors that miss the coarse graph use the ordinary search. Fields are
/// admitted only after a second matching request. Construction/clearing work
/// is divided between ticks; building fields are never evicted by new goals.
/// Fine regions may retain two representatives for one regional key. A second
/// uses empty or failed storage when completed trees do not cover a known goal;
/// it shares the same construction and extraction budgets. Unknown or blocked
/// goal centres and full representative tables fall back to native search.
/// The state is canonical byte storage with no pointers and may be saved, but
/// is owner-local navigation state and must not enter shared simulation hashes.
class FlowCache {
  public:

    /// Allocates bounded storage before simulation ticks.
    ///
    /// @param width map width in footprint cells
    /// @param height map height in footprint cells
    /// @param limits maximum fields, total coarse cells and work per tick
    FlowCache(uint32_t width, uint32_t height, FlowLimits limits = {});

    /// Allocates a specific shared-route preset without changing legacy defaults.
    ///
    /// @param width map width in footprint cells
    /// @param height map height in footprint cells
    /// @param limits maximum fields, total coarse cells and work per tick
    /// @param preset immutable grid, key, prefix and blocked-field policy
    FlowCache(uint32_t width, uint32_t height, FlowLimits limits, FlowPreset preset);

    /// Clears at most 65,536 direction bytes and spends the configured expansion budget.
    /// Seed connections consume expansion credits; clearing uses contiguous slices.
    ///
    /// @param tick deterministic simulation tick
    /// @param static_revision current buildings/features/terrain revision
    /// @param sampler current passability and player knowledge
    void advance(uint32_t tick, uint32_t static_revision, const FlowSampler& sampler) noexcept;

    /// Looks up a circle-goal route, admitting repeated destinations when needed.
    ///
    /// Pending suppresses a full search without consuming its retry timestamp.
    /// Extraction shares a separate per-tick cell-probe budget. Exhaustion
    /// returns pending so large cohorts cannot burst unbounded validation work.
    /// Fine regional fields charge bounded goal-branch nodes to the same budget.
    /// Ready writes at least two points; a route may be a prefix when the output
    /// fills. Every returned edge is checked against current static knowledge.
    ///
    /// @param context movement class, player, revisions and footprint
    /// @param unit canonical unit supplying the start cell and position
    /// @param goal circle goal; other shapes fall back
    /// @param[out] points navigator's preallocated route storage
    /// @param[out] count points written, zero unless ready
    /// @param sampler current static passability
    /// @return ready, pending, or fallback to the ordinary search
    [[nodiscard]] FlowResult route(
        const FlowContext& context,
        const oa::Unit& unit,
        const Goal& goal,
        std::span<RoutePoint> points,
        uint32_t& count,
        const FlowSampler& sampler
    ) noexcept;

    /// Returns cumulative bounded-work counters.
    [[nodiscard]] FlowCounters counters() const noexcept;
    /// Returns the static revision restored with the cache.
    [[nodiscard]] uint32_t static_revision() const noexcept;
    /// Returns canonical little-endian state; no encoding or allocation is needed.
    [[nodiscard]] std::span<const uint8_t> state_bytes() const noexcept;
    /// Restores a validated state of the same map, bounds and format.
    ///
    /// @param bytes saved state, including pending frontiers and admission history
    /// @return false without changes when the bytes are malformed or incompatible
    [[nodiscard]] bool restore(std::span<const uint8_t> bytes) noexcept;

  private:

    std::unique_ptr<uint8_t[]> state_{}, restore_marks_{};
    std::size_t state_size_{};
    uint32_t width_{}, height_{}, stride_{}, columns_{}, rows_{}, cells_{}, fields_{},
        work_per_tick_{};
    uint32_t route_probes_per_tick_{};
};

} // namespace oa::sim::ground_orders
