// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/ground_orders/navigator.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace oa::sim::match_runtime {

namespace route_queue_detail {

inline constexpr int64_t one_pixel = 65536;

/// Bounds vector components so two products and their sum fit signed 64 bits.
/// @param vector fixed-point vector
/// @return whether each component lies between negative and positive INT32_MAX
[[nodiscard]] inline bool bounded_vector(const std::array<int64_t, 2>& vector) noexcept {
    constexpr int64_t limit = std::numeric_limits<int32_t>::max();
    return vector[0] >= -limit && vector[0] <= limit && vector[1] >= -limit && vector[1] <= limit;
}

} // namespace route_queue_detail

/// Recognizes a leader ahead on a shared directed native route prefix.
/// The caller separately checks ownership, local ground eligibility and the
/// existing traffic grace period. This predicate changes no route or physics.
/// @param follower following unit's current native geometry
/// @param route follower's active canonical navigation
/// @param leader selected blocker's current native geometry
/// @param leader_route selected blocker's active canonical navigation
/// @return whether the leader is strictly ahead and nearer their shared next point
[[nodiscard]] inline bool same_forward_route(
    const unit_movement::Unit& follower,
    const ground_orders::Navigation& route,
    const unit_movement::Unit& leader,
    const ground_orders::Navigation& leader_route
) noexcept {
    using namespace route_queue_detail;
    if (!route.goal || !leader_route.goal || !ground_orders::route_present(route) ||
        !ground_orders::route_present(leader_route) || route.count < 2 || leader_route.count < 2 ||
        route.count > route.points.size() || leader_route.count > leader_route.points.size() ||
        follower.footprint != leader.footprint || route.points[1] != leader_route.points[1])
        return false;
    const auto& next = route.points[route.count > 2 ? 2 : 1];
    const auto& leader_next = leader_route.points[leader_route.count > 2 ? 2 : 1];
    if (next != leader_next)
        return false;
    const std::array<int64_t, 2> direction{
        int64_t(route.points[1][0]) * one_pixel - follower.position[0],
        int64_t(route.points[1][1]) * one_pixel - follower.position[2]
    };
    const std::array<int64_t, 2> separation{
        int64_t(leader.position[0]) - follower.position[0],
        int64_t(leader.position[2]) - follower.position[2]
    };
    const std::array<int64_t, 2> remaining{
        int64_t(route.points[1][0]) * one_pixel - leader.position[0],
        int64_t(route.points[1][1]) * one_pixel - leader.position[2]
    };
    if (!bounded_vector(direction) || !bounded_vector(separation) || !bounded_vector(remaining))
        return false;
    const auto ahead = direction[0] * separation[0] + direction[1] * separation[1];
    const auto same_direction = direction[0] * remaining[0] + direction[1] * remaining[1];
    const auto follower_distance = direction[0] * direction[0] + direction[1] * direction[1];
    const auto leader_distance = remaining[0] * remaining[0] + remaining[1] * remaining[1];
    return ahead > 0 && same_direction > 0 && leader_distance < follower_distance;
}

} // namespace oa::sim::match_runtime
