// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/sim/spatial_state/cell_classifier.hpp"

#include <cstdint>
#include <span>

namespace oa::app {

/// Resolves a raw ground command before any member of its batch is ordered.
/// @param world canonical input and unit state
/// @param centre complete group's centre before the command
/// @param command requested ground command
/// @param actor canonical unit receiving the command
/// @param click original signed 16.16 command point
/// @param hooks existing order-table classification services
/// @return actor, resolved order and untranslated-or-shaped raw destination
[[nodiscard]] sim::gameplay_input::SelectionOrder group_order_intent(
    const oa::World& world,
    const sim::gameplay_input::GroupCentre& centre,
    sim::gameplay_input::OrderCommand command,
    const oa::Unit& actor,
    const oa::FixedVec3& click,
    const sim::gameplay_input::OrderCursorHooks& hooks
) noexcept;

/// Supplies the movement limits already resolved for a canonical actor.
struct GroupDestinationHooks {
    void* context{};
    /// Copies static classifier limits; false retains the bounds-only fallback.
    bool (*movement)(
        void* context, const oa::Unit& actor, sim::spatial_state::CellClassifier& out
    ) noexcept {};
};

/// Bounds command-local static validation, reservations and connected expansion.
struct GroupDestinationLimits {
    uint32_t classes{8};
    uint32_t origins{262144};
    uint32_t cells{1048576};
};

/// Reports allocation separately from the legacy fallback retained on failure.
struct GroupDestinationReport {
    uint32_t preserved{};
    uint32_t repaired{};
    uint32_t assigned{};
    uint32_t unsupported{};
    uint32_t classes{};
    uint32_t origins{};
    uint32_t cells{};
    uint32_t reservation_cells{};
    uint32_t scratch_bytes{};
    uint32_t frontiers{};
    bool abandoned{};
    bool exhausted{};
};

/// Distributes only repaired formation destinations without issuing any order.
/// Every usable raw XYZ stays exact. All their native arrival rectangles are
/// reserved before repaired actors receive disjoint rectangles in stable ID
/// order. A four-neighbour static component connects each candidate to the
/// click for its complete movement/order key; this does not prove a route from
/// the actor or account for mobile congestion. RepairPatrol conservatively
/// reserves the bounding rectangle of its five possible arrival footprints.
/// Static knowledge includes canonical obstacles regardless of player fog,
/// matching the previous arrival checks rather than unknown-is-clear routing.
/// Each admitted class retains a bounded frontier so interleaved actors are
/// allocated in global ID order. Command-local heap scratch uses about 300 KiB
/// for one class and at most 1.25 MiB for eight, independent of map size.
/// Exhaustion before assignment leaves the whole command at the existing
/// bounds/static fallback; later exhaustion retains unassigned fallbacks.
/// The fallback itself retains the existing policy for unsupported footprints.
/// @param[in,out] orders resolved command intents, in their original issue order
/// @param click original signed 16.16 ground command point
/// @param world current canonical static spatial data; mobiles are ignored
/// @param map_width playable map width in world pixels, excluding padding
/// @param map_height playable map height in world pixels, excluding padding
/// @param hooks resolved actor movement limits
/// @param limits upper bounds, clamped to the fixed implementation capacities
/// @return assignment, fallback and charged-work counts; no canonical state changes
[[nodiscard]] GroupDestinationReport distribute_group_order_points(
    std::span<sim::gameplay_input::SelectionOrder> orders,
    const oa::FixedVec3& click,
    const sim::spatial_state::World& world,
    int32_t map_width,
    int32_t map_height,
    GroupDestinationHooks hooks,
    GroupDestinationLimits limits = {}
);

} // namespace oa::app
