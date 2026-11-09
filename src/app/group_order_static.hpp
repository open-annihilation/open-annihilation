// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "group_order_bounds.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/sim/ground_orders/goals.hpp"
#include "oa/sim/ground_orders/shared_routes.hpp"
#include "oa/sim/spatial_state/cell_classifier.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace oa::app {
namespace group_order_static_detail {

// Native arrival tolerances are in whole world units. A ground Move adds
// four units to its default zero tolerance; Patrol uses zero and RepairPatrol
// uses one 16-unit cell. make_goal supplies their native quantization and shape.
inline constexpr int32_t move_arrival_tolerance = 4;
inline constexpr int32_t patrol_arrival_tolerance = 0;
inline constexpr int32_t repair_patrol_arrival_tolerance = 16;
inline constexpr int32_t maximum_arrival_radius_squared = 1;
inline constexpr std::array<std::array<int32_t, 2>, 5> arrival_offsets{
    {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}}
};

/// Builds the native arrival region for one supported ground command.
/// @param scratch local order that outlives the returned Goal
/// @param actor canonical unit whose footprint quantizes the destination
/// @param order resolved ground order
/// @param point destination in signed 16.16 world coordinates
/// @return the native Goal, or nothing for another order or invalid footprint
inline std::optional<sim::ground_orders::Goal> arrival_goal(
    sim::simulation_state::Order& scratch,
    const oa::Unit& actor,
    sim::gameplay_input::UnitOrder order,
    const oa::FixedVec3& point
) noexcept {
    if (actor.footprint_x <= 0 || actor.footprint_z <= 0)
        return std::nullopt;
    int32_t tolerance = 0;
    switch (order) {
    case sim::gameplay_input::UnitOrder::move_ground:
        tolerance = move_arrival_tolerance;
        break;
    case sim::gameplay_input::UnitOrder::patrol:
        tolerance = patrol_arrival_tolerance;
        break;
    case sim::gameplay_input::UnitOrder::repair_patrol:
        tolerance = repair_patrol_arrival_tolerance;
        break;
    default:
        return std::nullopt;
    }
    sim::unit_movement::Unit geometry{};
    geometry.footprint = {actor.footprint_x, actor.footprint_z};
    return sim::ground_orders::make_goal(scratch, geometry, {point.x, point.y, point.z}, tolerance);
}

/// Tests every possible arrival footprint for a bounded native ground Goal.
/// @param goal native circle with a radius of at most one cell
/// @param movement static footprint, water limits and slope bands
/// @param world current canonical terrain, features and occupants
/// @return whether any arrival is statically passable, or nothing for another region
inline std::optional<bool> has_static_arrival(
    const sim::ground_orders::Goal& goal,
    const sim::spatial_state::CellClassifier& movement,
    const sim::spatial_state::World& world
) noexcept {
    if (goal.shape != sim::ground_orders::GoalShape::circle || goal.radius_squared < 0 ||
        goal.radius_squared > maximum_arrival_radius_squared)
        return std::nullopt;
    for (const auto& offset : arrival_offsets) {
        const int32_t x = int32_t(goal.cell[0]) + offset[0];
        const int32_t z = int32_t(goal.cell[1]) + offset[1];
        if (sim::ground_orders::goal_contains(goal, x, z) &&
            sim::ground_orders::static_flow_passable(movement, x, z, world))
            return true;
    }
    return false;
}
} // namespace group_order_static_detail

/// Uses the original click for a formation with no usable static arrival region.
/// The caller first applies the playable-map bounds policy. This check ignores
/// mobile occupants and retains every formation with at least one legal arrival.
/// It proves footprint legality, not connectivity or a route to the destination.
/// @param formation bounds-checked formation point in signed 16.16 world coordinates
/// @param click original command point in signed 16.16 world coordinates
/// @param actor canonical unit whose footprint receives the order
/// @param order resolved ground command
/// @param movement resolved water and slope limits; actor supplies its footprint
/// @param world current canonical spatial terrain and occupants
/// @param map_width playable map width in world units, excluding padded terrain
/// @param map_height playable map height in world units, excluding padded terrain
/// @param enabled whether the enhanced ground-order policy applies
/// @return the formation point, or the original click when only its region is usable
inline oa::FixedVec3 static_group_order_point(
    const oa::FixedVec3& formation,
    const oa::FixedVec3& click,
    const oa::Unit& actor,
    sim::gameplay_input::UnitOrder order,
    const sim::spatial_state::CellClassifier& movement,
    const sim::spatial_state::World& world,
    int32_t map_width,
    int32_t map_height,
    bool enabled
) noexcept {
    if (!enabled ||
        (actor.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) != group_order_detail::ground_layer ||
        actor.attach_parent != 0 ||
        !group_order_detail::axis_fits(click.x, actor.footprint_x, map_width) ||
        !group_order_detail::axis_fits(click.z, actor.footprint_z, map_height))
        return formation;
    sim::simulation_state::Order scratch{};
    const auto formation_goal =
        group_order_static_detail::arrival_goal(scratch, actor, order, formation);
    const auto click_goal = group_order_static_detail::arrival_goal(scratch, actor, order, click);
    if (!formation_goal || !click_goal)
        return formation;
    auto classifier = movement;
    classifier.footprint_x = actor.footprint_x;
    classifier.footprint_z = actor.footprint_z;
    const auto formation_fit =
        group_order_static_detail::has_static_arrival(*formation_goal, classifier, world);
    if (!formation_fit || *formation_fit)
        return formation;
    const auto click_fit =
        group_order_static_detail::has_static_arrival(*click_goal, classifier, world);
    return click_fit && *click_fit ? click : formation;
}

} // namespace oa::app
