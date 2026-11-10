// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/ground_orders/navigator.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace oa::sim::match_runtime {

inline constexpr uint32_t live_route_corner_period = 120;
inline constexpr uint32_t live_route_corner_maximum_origins = 128;
inline constexpr uint32_t live_route_corner_maximum_plots = 512;
inline constexpr int16_t live_route_corner_maximum_footprint = 8;
inline constexpr uint32_t moving_route_corner_period = 31;
inline constexpr uint32_t moving_route_corner_maximum_plots = 64;

/// Checks a nearby route anchor without aliasing the ordinary 60-tick retry.
///
/// A short anchor that reverses the route can make a moving unit circle
/// outside the native five-pixel capture radius. Ordinary forward corners
/// remain untouched. This only schedules a bounded visibility test; the
/// caller still requires actual movement and the recorded enhanced preset.
[[nodiscard]] inline bool live_route_corner_moving_due(
    const sim::unit_movement::Unit& unit,
    const sim::ground_orders::Navigation& navigation,
    uint32_t tick,
    uint16_t unit_id
) noexcept {
    if ((uint64_t(tick) + unit_id) % moving_route_corner_period != 0 || !navigation.goal ||
        !sim::ground_orders::route_present(navigation) || navigation.count < 3 ||
        navigation.count > navigation.points.size())
        return false;
    constexpr int64_t reach = int64_t{32} * 65536;
    const auto dx = int64_t(navigation.points[1][0]) * 65536 - unit.position[0];
    const auto dz = int64_t(navigation.points[1][1]) * 65536 - unit.position[2];
    if (dx < -reach || dx > reach || dz < -reach || dz > reach || dx * dx + dz * dz > reach * reach)
        return false;
    const auto incoming_x = int64_t(navigation.points[1][0]) - navigation.points[0][0];
    const auto incoming_z = int64_t(navigation.points[1][1]) - navigation.points[0][1];
    const auto outgoing_x = int64_t(navigation.points[2][0]) - navigation.points[1][0];
    const auto outgoing_z = int64_t(navigation.points[2][1]) - navigation.points[1][1];
    return incoming_x * outgoing_x + incoming_z * outgoing_z < 0;
}

/// Staggers visible-corner checks after sustained collision blockage.
///
/// @param blocked_ticks consecutive collision ticks
/// @param tick current simulation tick
/// @param unit_id stable unit slot
/// @return whether this unit's bounded corner check is due
[[nodiscard]] inline bool
live_route_corner_due(uint8_t blocked_ticks, uint32_t tick, uint16_t unit_id) noexcept {
    return blocked_ticks >= sim::unit_movement::local_flow_retry_ticks &&
           (uint64_t(tick) + unit_id) % live_route_corner_period == 0;
}

namespace live_corner_detail {

constexpr int64_t one_world_unit = 65536;
constexpr int64_t half_cell = 8 * one_world_unit;
constexpr int64_t cell_step = 16 * one_world_unit;

/// Holds a fixed-point segment and its footprint-origin endpoint cells.
struct Segment {
    std::array<int64_t, 2> from{}, to{};
    std::array<int16_t, 2> first{}, last{};
};

/// Computes native footprint-origin coordinates without accepting fixed-point wrap.
///
/// @param position signed 16.16 centre coordinate
/// @param footprint positive supported footprint width
/// @param[out] biased centre coordinate shifted onto the footprint-origin grid
/// @param[out] cell containing footprint-origin cell
/// @return whether the bias retains the native signed fixed-point range
[[nodiscard]] inline bool
origin(int64_t position, int16_t footprint, int64_t& biased, int16_t& cell) noexcept {
    biased = position - int64_t(footprint) * half_cell + half_cell;
    if (biased < std::numeric_limits<sim::unit_movement::Fixed>::min() ||
        biased > std::numeric_limits<sim::unit_movement::Fixed>::max())
        return false;
    cell = static_cast<int16_t>(biased >> 20);
    return true;
}

/// Visits every crossed origin and both orthogonal sides of diagonal crossings.
///
/// Endpoint cells use native half-open floor quantisation. A repeated traversal
/// has the same order, allowing a pure count pass before any static query.
///
/// @param segment fixed segment and endpoint origins
/// @param limit maximum emitted origins
/// @param[in,out] visit nonthrowing visitor returning false to stop
/// @param[out] count emitted origins, including the first over-budget origin
/// @return whether the complete segment fit and every visitor accepted
template <class Visit>
[[nodiscard]] bool
visit_origins(const Segment& segment, uint32_t limit, Visit&& visit, uint32_t& count) noexcept {
    count = 0;
    const auto emit = [&](int64_t x, int64_t z) {
        ++count;
        return count <= limit && visit(static_cast<int16_t>(x), static_cast<int16_t>(z));
    };
    int64_t x = segment.first[0], z = segment.first[1];
    if (!emit(x, z))
        return false;
    const auto dx = segment.to[0] - segment.from[0];
    const auto dz = segment.to[1] - segment.from[1];
    const int64_t step_x = (dx > 0) - (dx < 0), step_z = (dz > 0) - (dz < 0);
    const auto absolute_x = dx < 0 ? -dx : dx, absolute_z = dz < 0 ? -dz : dz;
    int64_t next_x =
        step_x > 0 ? (x + 1) * cell_step - segment.from[0] : segment.from[0] - x * cell_step;
    int64_t next_z =
        step_z > 0 ? (z + 1) * cell_step - segment.from[1] : segment.from[1] - z * cell_step;
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

} // namespace live_corner_detail

/// Removes one visible route corner after a bounded full-footprint clearance check.
///
/// The sampler accepts footprint-origin cells and must report whether the
/// complete matched footprint is known and statically clear. It must not throw
/// or mutate navigation. A count-only preflight precedes every sampler call.
/// Failure leaves all navigation bytes unchanged. Successful advancement sets
/// P0 to the actual whole-world pose and preserves the goal and search state.
///
/// @param unit moving unit at its actual signed 16.16 position
/// @param[in,out] navigation original owned route, changed only on success
/// @param class_fp_x classifier footprint width, one through eight
/// @param class_fp_z classifier footprint height, one through eight
/// @param known_static_clear nonthrowing full-footprint origin query
/// @param[in,out] probes actual sampler calls, excluding the pure preflight
/// @param maximum_plots maximum full-footprint plot work for this attempt
/// @return whether exactly the original P1 was removed
template <class Sampler>
[[nodiscard]] bool try_advance_visible_corner(
    const sim::unit_movement::Unit& unit,
    sim::ground_orders::Navigation& navigation,
    int16_t class_fp_x,
    int16_t class_fp_z,
    Sampler&& known_static_clear,
    uint64_t& probes,
    uint32_t maximum_plots = live_route_corner_maximum_plots
) noexcept {
    using namespace live_corner_detail;
    if (!navigation.goal || !sim::ground_orders::route_present(navigation) ||
        navigation.count < 3 || navigation.count > navigation.points.size() || class_fp_x <= 0 ||
        class_fp_z <= 0 || class_fp_x > live_route_corner_maximum_footprint ||
        class_fp_z > live_route_corner_maximum_footprint || unit.footprint[0] != class_fp_x ||
        unit.footprint[1] != class_fp_z)
        return false;
    const auto area = uint32_t(class_fp_x) * uint32_t(class_fp_z);
    const auto limit = std::min(live_route_corner_maximum_origins, maximum_plots / area);
    Segment segment{};
    for (unsigned axis = 0; axis < 2; ++axis) {
        const auto position_axis = axis == 0 ? 0 : 2;
        const auto footprint = unit.footprint[axis];
        if (!origin(
                unit.position[position_axis], footprint, segment.from[axis], segment.first[axis]
            ) ||
            !origin(
                int64_t(navigation.points[2][axis]) * one_world_unit,
                footprint,
                segment.to[axis],
                segment.last[axis]
            ))
            return false;
    }
    const auto cells_x = int32_t(segment.last[0]) - segment.first[0];
    const auto cells_z = int32_t(segment.last[1]) - segment.first[1];
    const auto minimum_origins = 1u + uint32_t(cells_x < 0 ? -cells_x : cells_x) +
                                 uint32_t(cells_z < 0 ? -cells_z : cells_z);
    if (minimum_origins > limit)
        return false;
    uint32_t origins{};
    if (!visit_origins(segment, limit, [](int16_t, int16_t) { return true; }, origins))
        return false;
    // The preflight bounds current-to-P2 to fewer than 128 cells. Using that
    // vector in the cross product also bounds map-wide old P1 coordinates.
    const auto corner_x = int64_t(navigation.points[1][0]) * one_world_unit - unit.position[0];
    const auto corner_z = int64_t(navigation.points[1][1]) * one_world_unit - unit.position[2];
    const auto target_x = segment.to[0] - segment.from[0];
    const auto target_z = segment.to[1] - segment.from[1];
    if (corner_x * target_z - corner_z * target_x == 0)
        return false;
    uint32_t queried{};
    if (!visit_origins(
            segment,
            origins,
            [&](int16_t x, int16_t z) {
                ++probes;
                return known_static_clear(x, z);
            },
            queried
        ))
        return false;
    for (uint32_t at = 1; at + 1 < navigation.count; ++at)
        navigation.points[at] = navigation.points[at + 1];
    navigation.points[0] = {
        static_cast<int16_t>(unit.position[0] >> 16), static_cast<int16_t>(unit.position[2] >> 16)
    };
    --navigation.count;
    navigation.flags |= sim::ground_orders::route_changed_flag;
    return true;
}

} // namespace oa::sim::match_runtime
