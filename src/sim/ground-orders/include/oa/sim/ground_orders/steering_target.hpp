// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/ground_orders/goals.hpp"

namespace oa::sim::ground_orders {

/// Holds the controller's projected aim and distance to the original next point.
struct SteeringTarget {
    Point point{};    ///< projected signed 16.16 steering target
    Fixed distance{}; ///< signed 16.16 distance to the unprojected next point
};

/// Retains one exact projection and the complete inputs that produced it.
///
/// This transient value belongs to one movement update, not the saved match.
struct SteeringTargetCache {
    Point position{};              ///< signed 16.16 unit position before steering
    std::array<Point, 3> points{}; ///< original origin, target and braking point
    SteeringTarget target{};       ///< exact projected aim and original target distance
};

/// Projects a distant route target onto the controller's eighty-world-unit lookahead.
///
/// The projection follows the original first-to-second route segment. The
/// returned distance remains the distance to the original second point, as
/// required by the steering driver's speed limits.
///
/// @param unit moving unit
/// @param points original origin, target and braking point in signed 16.16
/// @return projected aim and original target distance
[[nodiscard]] SteeringTarget
steering_target(const sim::unit_movement::Unit& unit, const std::array<Point, 3>& points) noexcept;

/// Projects a route target while retaining its complete position and point inputs.
///
/// @param unit moving unit
/// @param points original origin, target and braking point in signed 16.16
/// @return exact projection and value snapshots for later steering in this update
[[nodiscard]] SteeringTargetCache cache_steering_target(
    const sim::unit_movement::Unit& unit, const std::array<Point, 3>& points
) noexcept;

} // namespace oa::sim::ground_orders
