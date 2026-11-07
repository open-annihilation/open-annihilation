// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/ground_orders/navigator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace oa::sim::ground_orders {

using CorridorOrigin = std::array<int16_t, 2>;

/// Static knowledge of the complete requested footprint at one native origin.
enum class CorridorCell : uint8_t { blocked, clear, unknown };

/// The long axis of a straight corridor.
enum class CorridorAxis : uint8_t { x, z };

/// Planned travel along the corridor axis; unknown never selects a lane.
enum class CorridorDirection : uint8_t { unknown, increasing, decreasing };

/// Inclusive native cell rectangle, with X and Z bounds in world-grid order.
struct CorridorRectangle {
    int16_t left{}, top{}, right{}, bottom{};
};

/// Exact static corridor geometry for one footprint and knowledge revision.
///
/// The origin rectangle has known blocked transverse walls and wider known
/// openings at both ends. Each mouth is a complete known-clear origin strip
/// one longitudinal footprint deep outside the corridor. These are static
/// geometry proofs, not turning, occupancy or downstream-vacancy guarantees.
struct CorridorGeometry {
    uint16_t id{}; ///< revision-local identifier; zero means no corridor
    CorridorAxis axis{};
    CorridorOrigin footprint{};
    CorridorRectangle origins{}, plots{};
    std::array<CorridorRectangle, 2> mouths{}; ///< increasing-axis order
    std::array<int16_t, 2> lane_origins{};     ///< increasing transverse order
    uint8_t lane_count{}; ///< two only when both complete physical footprints are disjoint
    uint64_t revision{};
};

/// Supplies authoritative full-footprint static passability and complete knowledge.
struct CorridorSampler {
    void* context{};
    CorridorCell (*origin)(void*, CorridorOrigin, CorridorOrigin footprint){};
};

/// Caller-owned work allowance; shared budgets bound a complete simulation pass.
struct CorridorBudget {
    uint32_t remaining{}, used{};
};

/// Construction-only capacities; no query or construction step allocates.
struct CorridorLimits {
    uint32_t corridors{64}, pending_seeds{128}, total_cells{2'097'152};
};

/// Result of submitting one exact native origin and candidate axis.
/// Full means either the pending queue or published-geometry capacity is exhausted.
enum class CorridorRequest : uint8_t { queued, pending, known, full, invalid };

/// The local part of a corridor touched by a full footprint.
enum class CorridorRegion : uint8_t { body, lower_mouth, upper_mouth };

/// One local geometry contact; overlapping mouths retain separate identifiers.
struct CorridorContact {
    uint16_t id{};
    CorridorRegion region{};
};

/// Upcoming route contact, derived from the actual pose and current route.
struct CorridorApproach {
    uint16_t id{};
    CorridorDirection direction{};
    uint16_t entry_segment{}, exit_segment{}; ///< point indexes; zero means no observed exit
    bool already_inside{};
    /// A full-footprint exit in the reported direction is present in this route prefix.
    /// For an already-inside unit this does not infer its historical entrance.
    bool exits_other_end{};
};

/// Bounded query output; incomplete never means the unvisited geometry is clear.
struct CorridorQuery {
    uint32_t count{};
    bool complete{};
};

/// Construction progress and charged native sampler work.
struct CorridorStatus {
    uint32_t corridors{}, pending_seeds{};
    uint64_t revision{}, work_units{}, samples{}, rejected_seeds{};
    bool clearing{}, building{}, enabled{};
};

/// Returns the right-hand lane's native transverse origin, or false without two lanes.
///
/// World X increases east and world Z increases south. This helper selects
/// geometry only; it grants no movement permission and changes no route.
[[nodiscard]] bool corridor_lane(
    const CorridorGeometry& corridor, CorridorDirection direction, int16_t& transverse
) noexcept;

/// Builds and queries straight, fully known narrow corridors from route-derived seeds.
///
/// A narrow origin span is at most twice its transverse footprint dimension;
/// the constant rectangle is at least max(4, twice the longitudinal footprint)
/// cells long. One- and two-lane geometry are distinguished explicitly. Wider
/// rooms, unknown boundaries, dead ends and disconnected mouths are rejected.
/// A spatial bucket index keeps local queries independent of the unit count.
/// No admission, steering, Goal, order, reservation or engine-version policy
/// occurs here. A sampler revision must remain fixed while construction runs.
class CorridorCatalog {
  public:

    /// Preallocates bounded map, corridor and pending-seed storage.
    CorridorCatalog(
        uint32_t width, uint32_t height, CorridorOrigin footprint, CorridorLimits limits = {}
    );
    /// Releases the preallocated construction and spatial-index storage.
    ~CorridorCatalog();
    CorridorCatalog(const CorridorCatalog&) = delete;
    CorridorCatalog& operator=(const CorridorCatalog&) = delete;

    /// Invalidates geometry immediately; subsequent advance calls clear the index in bounded work.
    void reset(uint64_t revision) noexcept;
    /// Queues an exact origin; matching pending seeds and already catalogued bodies are shared.
    [[nodiscard]] CorridorRequest request(CorridorOrigin seed, CorridorAxis axis) noexcept;
    /// Advances detection and index publication using at most the supplied work allowance.
    void advance(CorridorBudget& budget, const CorridorSampler& sampler) noexcept;
    /// Returns published geometry, or null for a missing or invalidated identifier.
    [[nodiscard]] const CorridorGeometry* geometry(uint16_t id) const noexcept;
    /// Classifies local full-footprint contact, preserving all overlapping mouth records.
    [[nodiscard]] CorridorQuery at_origin(
        CorridorOrigin origin, std::span<CorridorContact> output, CorridorBudget& budget
    ) const noexcept;
    /// Reads current geometry from actual pose through P1 and the remaining canonical route.
    ///
    /// P0 is not treated as incoming direction: it may be an old publication
    /// pose. The query never rewrites a Goal or route. A bounded output may
    /// include a contact whose opposite-bank exit is not yet in the prefix.
    [[nodiscard]] CorridorQuery upcoming(
        const sim::unit_movement::Unit& unit,
        const Navigation& navigation,
        std::span<CorridorApproach> output,
        CorridorBudget& budget
    ) const noexcept;
    /// Returns bounded construction progress and cumulative work counters.
    [[nodiscard]] CorridorStatus status() const noexcept;
    /// Returns storage bytes including validation scratch and the encoded cache, excluding allocator overhead.
    [[nodiscard]] uint64_t allocated_bytes() const noexcept;
    /// Returns the fixed canonical byte count for these bounds, or zero when disabled.
    [[nodiscard]] std::size_t saved_size() const noexcept;
    /// Writes every pending, published and partially indexed state in canonical little-endian form.
    /// No allocation occurs; a buffer with a different size is left unchanged.
    [[nodiscard]] bool write_state(std::span<uint8_t> output) const noexcept;
    /// Encodes into the constructor-preallocated byte cache; the next call overwrites this view.
    [[nodiscard]] std::span<const uint8_t> state_bytes() const noexcept;
    /// Validates a compatible state using preallocated scratch without changing observable state.
    [[nodiscard]] bool can_restore(std::span<const uint8_t> input) const noexcept;
    /// Atomically restores exact construction timing; malformed or incompatible bytes change nothing.
    /// Successful restoration invalidates borrowed geometry pointers and performs no rebuild.
    [[nodiscard]] bool restore(std::span<const uint8_t> input) noexcept;

  private:

    struct State;
    std::unique_ptr<State> state_;
    mutable std::unique_ptr<State> restore_state_;
    mutable std::unique_ptr<uint8_t[]> saved_bytes_;
    std::size_t saved_size_{};
};

} // namespace oa::sim::ground_orders
