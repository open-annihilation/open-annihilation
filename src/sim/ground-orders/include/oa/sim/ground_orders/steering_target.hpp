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

} // namespace oa::sim::ground_orders
