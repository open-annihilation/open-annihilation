// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/ground_orders/navigator.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::sim::match_runtime {

/// Tests whether a proposed guide only follows a prefix of the remaining route.
///
/// The proposed origin is the unit's current position; the old origin is ignored.
/// Duplicate points and collinear subdivisions may differ, but every old bend
/// must still be followed in order. Both spans must hold two to navigation_capacity
/// points. This comparison allocates no memory and uses exact integer geometry.
[[nodiscard]] inline bool is_route_prefix(
    std::span<const sim::ground_orders::RoutePoint> existing,
    std::span<const sim::ground_orders::RoutePoint> proposed
) noexcept {
    if (existing.size() < 2 || proposed.size() < 2 ||
        existing.size() > sim::ground_orders::navigation_capacity ||
        proposed.size() > sim::ground_orders::navigation_capacity)
        return false;
    using Point = sim::ground_orders::RoutePoint;
    const auto contains = [](Point point, Point start, Point end) {
        if (start == end)
            return point == start;
        const int64_t dx = int64_t(end[0]) - start[0], dz = int64_t(end[1]) - start[1];
        const int64_t px = int64_t(point[0]) - start[0], pz = int64_t(point[1]) - start[1];
        const auto dot = px * dx + pz * dz;
        return px * dz == pz * dx && dot >= 0 && dot <= dx * dx + dz * dz;
    };
    Point cursor = proposed[0];
    std::size_t next = 1;
    for (std::size_t i = 1; i < proposed.size(); ++i) {
        const auto target = proposed[i];
        while (target != cursor) {
            while (next < existing.size() && existing[next] == cursor)
                ++next;
            if (next == existing.size())
                return false;
            const auto end = existing[next];
            if (contains(target, cursor, end)) {
                cursor = target;
                if (cursor == end)
                    ++next;
            } else if (contains(end, cursor, target)) {
                cursor = end;
                ++next;
            } else {
                return false;
            }
        }
    }
    return true;
}

} // namespace oa::sim::match_runtime
