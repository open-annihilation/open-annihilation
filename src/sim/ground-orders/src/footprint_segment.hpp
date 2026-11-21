// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/ground_orders/shared_routes.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace oa::sim::ground_orders::footprint_segment_detail {

inline constexpr int64_t one_world_unit = 65536;
inline constexpr int64_t half_cell = 8 * one_world_unit;
inline constexpr int64_t cell_step = 16 * one_world_unit;
inline constexpr uint32_t maximum_origins = 2048;
inline constexpr uint32_t maximum_footprint = 64;

/// Holds a fixed segment on the footprint-origin grid and its endpoint cells.
struct Segment {
    std::array<int64_t, 2> from{}, to{};
    std::array<int16_t, 2> first{}, last{};
};

/// Computes native origin coordinates without allowing fixed-point wrap.
///
/// @param unit canonical current pose and footprint-origin cell
/// @param context complete matched movement-class footprint
/// @param target whole-world target point
/// @param[out] out fixed-point segment, valid only on success
/// @return whether both ends fit and the current origin matches the unit
[[nodiscard]] inline bool make_segment(
    const oa::Unit& unit, const FlowContext& context, RoutePoint target, Segment& out
) noexcept {
    if (unit.footprint_x <= 0 || unit.footprint_z <= 0 || context.footprint_x > maximum_footprint ||
        context.footprint_z > maximum_footprint ||
        static_cast<uint32_t>(unit.footprint_x) != context.footprint_x ||
        static_cast<uint32_t>(unit.footprint_z) != context.footprint_z)
        return false;
    for (unsigned axis = 0; axis < 2; ++axis) {
        const auto footprint = axis == 0 ? context.footprint_x : context.footprint_z;
        const auto position = axis == 0 ? unit.position.x : unit.position.z;
        out.from[axis] = int64_t(position) - int64_t(footprint) * half_cell + half_cell;
        out.to[axis] =
            int64_t(target[axis]) * one_world_unit - int64_t(footprint) * half_cell + half_cell;
        if (out.from[axis] < std::numeric_limits<int32_t>::min() ||
            out.from[axis] > std::numeric_limits<int32_t>::max() ||
            out.to[axis] < std::numeric_limits<int32_t>::min() ||
            out.to[axis] > std::numeric_limits<int32_t>::max())
            return false;
        out.first[axis] = static_cast<int16_t>(out.from[axis] >> 20);
        out.last[axis] = static_cast<int16_t>(out.to[axis] >> 20);
    }
    return out.first[0] == unit.cell_x && out.first[1] == unit.cell_z;
}

/// Bounds all crossed cells, including both sides of exact diagonal crossings.
[[nodiscard]] inline uint32_t probe_bound(const Segment& segment) noexcept {
    const auto width = static_cast<uint32_t>(std::abs(int32_t(segment.last[0]) - segment.first[0]));
    const auto height =
        static_cast<uint32_t>(std::abs(int32_t(segment.last[1]) - segment.first[1]));
    return 1 + std::max(width, height) + 2 * std::min(width, height);
}

/// Visits actual crossed origins in boundary order, with no sampler calls on budget rejection.
///
/// Endpoint cells use native half-open floor quantisation. Both orthogonal
/// neighbours are visited when the line crosses an exact grid corner. The
/// limit and pure preflight bound vectors below 2048 cells, so the boundary
/// products fit int64_t throughout the traversal.
///
/// @param segment valid fixed segment produced by make_segment
/// @param limit maximum emitted origins, at most maximum_origins
/// @param[in,out] visit nonthrowing visitor returning false to stop
/// @param[out] count emitted origins, including a rejected over-budget origin
/// @return whether the full segment fit and every visitor accepted
template <class Visit>
[[nodiscard]] bool
visit_origins(const Segment& segment, uint32_t limit, Visit&& visit, uint32_t& count) noexcept {
    count = 0;
    if (limit == 0 || limit > maximum_origins || probe_bound(segment) > limit)
        return false;
    const auto emit = [&](int32_t x, int32_t z) {
        ++count;
        return count <= limit && visit(x, z);
    };
    int32_t x = segment.first[0], z = segment.first[1];
    if (!emit(x, z))
        return false;
    const auto delta_x = segment.to[0] - segment.from[0], delta_z = segment.to[1] - segment.from[1];
    const int32_t step_x = (delta_x > 0) - (delta_x < 0), step_z = (delta_z > 0) - (delta_z < 0);
    const auto absolute_x = delta_x < 0 ? -delta_x : delta_x,
               absolute_z = delta_z < 0 ? -delta_z : delta_z;
    int64_t next_x = step_x > 0 ? int64_t(x + 1) * cell_step - segment.from[0]
                                : segment.from[0] - int64_t(x) * cell_step;
    int64_t next_z = step_z > 0 ? int64_t(z + 1) * cell_step - segment.from[1]
                                : segment.from[1] - int64_t(z) * cell_step;
    while (x != segment.last[0] || z != segment.last[1]) {
        if (x == segment.last[0] ||
            (z != segment.last[1] && next_z * absolute_x < next_x * absolute_z)) {
            z += step_z;
            next_z += cell_step;
            if (!emit(x, z))
                return false;
        } else if (z == segment.last[1] || next_x * absolute_z < next_z * absolute_x) {
            x += step_x;
            next_x += cell_step;
            if (!emit(x, z))
                return false;
        } else {
            if (!emit(x + step_x, z) || !emit(x, z + step_z))
                return false;
            x += step_x;
            z += step_z;
            next_x += cell_step;
            next_z += cell_step;
            if (!emit(x, z))
                return false;
        }
    }
    return true;
}

} // namespace oa::sim::ground_orders::footprint_segment_detail
