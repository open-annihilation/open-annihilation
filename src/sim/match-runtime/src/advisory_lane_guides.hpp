// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/ground_orders/corridors.hpp"

namespace oa::sim::match_runtime {

/// The temporary steering leg; neither phase grants occupancy or reservation rights.
enum class AdvisoryLanePhase : uint8_t { none, approach, crossing };

/// Caller-preallocated identity and route progress for one advisory crossing.
struct AdvisoryLaneState {
    const ground_orders::Goal* goal{};
    const simulation_state::Order* order{};
    uint64_t generation{}, goal_geometry{};
    ground_orders::CorridorGeometry corridor{};
    ground_orders::CorridorDirection direction{};
    int16_t lane{};
    AdvisoryLanePhase phase{};
    bool entered{}, steering_active{};
    std::array<ground_orders::RoutePoint, ground_orders::navigation_capacity> admitted_route{};
    uint32_t admitted_count{};
    uint16_t entry_segment{}, exit_segment{};
};

/// Temporary steering and an optional ordinary publication after physical egress.
struct AdvisoryLaneDecision {
    ground_orders::Navigation steering{};
    bool active{}, reset_local_head{}, released{}, request_search{};
    uint16_t corridor{}, consumed_prefix{};
    AdvisoryLanePhase phase{};
    std::array<ground_orders::RoutePoint, ground_orders::navigation_capacity> rejoin{};
    uint32_t rejoin_count{};
};

/// Chooses optional lane geometry before ordinary local avoidance and native steering.
///
/// The caller obtains the current route contact and geometry from CorridorCatalog.
/// Inside an entered corridor, supply the retained corridor's current geometry;
/// same-Goal reroutes do not change the retained direction or lane. New Goal/order
/// identities withdraw advice. No canonical navigation, Goal, order, kinematics,
/// occupancy, retry clock or shared random state is changed by this function.
/// A release returns either a statically clear ordinary suffix or a fresh-search
/// request; the caller publishes it through the normal navigation seam.
///
/// @param unit actual current native pose
/// @param navigation original owned route and Goal
/// @param generation caller's current unit/mission generation
/// @param approach current upcoming contact, or null
/// @param geometry current geometry of the selected or retained corridor, or null
/// @param sampler fully known static full-footprint queries
/// @param[in,out] budget shared finite query allowance
/// @param[in,out] state this unit's preallocated advisory state
/// @return temporary geometry and explicit ordinary handoff actions
[[nodiscard]] AdvisoryLaneDecision choose_advisory_lane(
    const unit_movement::Unit& unit,
    const ground_orders::Navigation& navigation,
    uint64_t generation,
    const ground_orders::CorridorApproach* approach,
    const ground_orders::CorridorGeometry* geometry,
    const ground_orders::CorridorSampler& sampler,
    ground_orders::CorridorBudget& budget,
    AdvisoryLaneState& state
) noexcept;

} // namespace oa::sim::match_runtime
