// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "live_route_corner.hpp"
#include "oa/sim/ground_orders/corridors.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace oa::sim::match_runtime {

inline constexpr uint32_t local_throat_maximum_probes = 512;
inline constexpr int32_t local_throat_maximum_length = 64;

namespace local_throat_detail {
namespace ground = sim::ground_orders;
namespace movement = sim::unit_movement;

enum class SectionKind : uint8_t { blocked, unknown, open, narrow };

/// Holds one known full-footprint transverse opening between blocked banks.
struct Section {
    SectionKind kind{SectionKind::unknown};
    int16_t lower{}, upper{};
};

/// Charges one full-footprint static query within the caller's fixed allowance.
struct Probe {
    const ground::CorridorSampler& sampler;
    ground::CorridorBudget& budget;
    uint32_t remaining{};

    /// Reads one native origin, treating exhausted work as incomplete knowledge.
    /// @param origin full-footprint origin in native cells
    /// @return clear, blocked, or unknown without inferring passability
    ground::CorridorCell at(ground::CorridorOrigin origin) noexcept {
        if (remaining == 0 || budget.remaining == 0 || !sampler.origin)
            return ground::CorridorCell::unknown;
        --remaining;
        --budget.remaining;
        ++budget.used;
        return sampler.origin(sampler.context, origin, {2, 2});
    }
};

/// Converts longitudinal/transverse coordinates into the native X/Z order.
/// @param axis longitudinal axis, zero for X and one for Z
/// @param major longitudinal native origin
/// @param minor transverse native origin
/// @return native X/Z origin, or a negative sentinel when it cannot fit
inline ground::CorridorOrigin origin(unsigned axis, int32_t major, int32_t minor) noexcept {
    if (major < 0 || minor < 0 || major > std::numeric_limits<int16_t>::max() ||
        minor > std::numeric_limits<int16_t>::max())
        return {-1, -1};
    ground::CorridorOrigin result{};
    result[axis] = static_cast<int16_t>(major);
    result[1 - axis] = static_cast<int16_t>(minor);
    return result;
}

/// Finds at most four clear origins and the known blocked bank on either side.
/// @param probe charged known-static sampler
/// @param axis longitudinal native coordinate index
/// @param major sampled longitudinal origin
/// @param minor clear-origin candidate near the current route
/// @return a closed narrow section, a wider opening, or an incomplete section
inline Section section(Probe& probe, unsigned axis, int32_t major, int32_t minor) noexcept {
    const auto centre = probe.at(origin(axis, major, minor));
    if (centre != ground::CorridorCell::clear)
        return {
            centre == ground::CorridorCell::blocked ? SectionKind::blocked : SectionKind::unknown
        };
    int32_t lower = minor, upper = minor;
    for (;;) {
        const auto value = probe.at(origin(axis, major, lower - 1));
        if (value == ground::CorridorCell::unknown)
            return {};
        if (value == ground::CorridorCell::blocked)
            break;
        --lower;
        if (upper - lower + 1 > 4)
            return {SectionKind::open};
    }
    for (;;) {
        const auto value = probe.at(origin(axis, major, upper + 1));
        if (value == ground::CorridorCell::unknown)
            return {};
        if (value == ground::CorridorCell::blocked)
            break;
        ++upper;
        if (upper - lower + 1 > 4)
            return {SectionKind::open};
    }
    return {SectionKind::narrow, static_cast<int16_t>(lower), static_cast<int16_t>(upper)};
}

/// Writes a native rectangle from longitudinal and transverse inclusive bounds.
/// @param axis longitudinal native coordinate index
/// @param first first longitudinal native origin
/// @param last last longitudinal native origin
/// @param lower first transverse native origin
/// @param upper last transverse native origin
/// @return rectangle in native X/Z order
inline ground::CorridorRectangle
rectangle(unsigned axis, int16_t first, int16_t last, int16_t lower, int16_t upper) noexcept {
    return axis == 0 ? ground::CorridorRectangle{first, lower, last, upper}
                     : ground::CorridorRectangle{lower, first, upper, last};
}

/// Proves a complete constant-width throat with two wider, known-clear mouths.
/// @param probe charged known-static sampler
/// @param axis longitudinal native coordinate index
/// @param seed native seed with the already proven initial section
/// @param initial initial closed transverse section
/// @param[out] result complete physical geometry on success only
/// @return whether both complete FP2 bodies and both mouths were proven
inline bool prove(
    Probe& probe,
    unsigned axis,
    ground::CorridorOrigin seed,
    Section initial,
    ground::CorridorGeometry& result
) noexcept {
    if (initial.kind != SectionKind::narrow || initial.upper - initial.lower + 2 < 4)
        return false;
    int32_t first = seed[axis], last = first;
    for (const int32_t step : {-1, 1}) {
        auto major = int32_t(seed[axis]);
        bool found_mouth = false;
        for (int32_t distance = 1; distance <= local_throat_maximum_length; ++distance) {
            major += step;
            const auto current = section(probe, axis, major, seed[1 - axis]);
            if (current.kind == SectionKind::open) {
                found_mouth = true;
                break;
            }
            if (current.kind != SectionKind::narrow || current.lower != initial.lower ||
                current.upper != initial.upper)
                return false;
            if (step < 0)
                first = major;
            else
                last = major;
            if (last - first + 1 > local_throat_maximum_length)
                return false;
        }
        if (!found_mouth)
            return false;
    }
    if (last - first + 1 < 4 || first < 2 || last > std::numeric_limits<int16_t>::max() - 2)
        return false;
    for (const int32_t major : {first - 2, first - 1, last + 1, last + 2})
        for (int32_t minor = initial.lower; minor <= initial.upper; ++minor)
            if (probe.at(origin(axis, major, minor)) != ground::CorridorCell::clear)
                return false;

    ground::CorridorGeometry candidate{};
    candidate.id = 1;
    candidate.axis = axis == 0 ? ground::CorridorAxis::x : ground::CorridorAxis::z;
    candidate.footprint = {2, 2};
    candidate.origins = rectangle(
        axis, static_cast<int16_t>(first), static_cast<int16_t>(last), initial.lower, initial.upper
    );
    candidate.plots = candidate.origins;
    ++candidate.plots.right;
    ++candidate.plots.bottom;
    candidate.mouths = {
        rectangle(axis, int16_t(first - 2), int16_t(first - 1), initial.lower, initial.upper),
        rectangle(axis, int16_t(last + 1), int16_t(last + 2), initial.lower, initial.upper)
    };
    const int32_t physical_width = int32_t(initial.upper) - initial.lower + 2;
    candidate.lane_origins = {
        int16_t(initial.lower + (physical_width - 4) / 2),
        int16_t(initial.lower + (physical_width - 4) / 2 + 2)
    };
    candidate.lane_count = 2;
    result = candidate;
    return true;
}

/// Checks an actual fixed-point chord with native full-footprint origin quantisation.
/// @param unit current pose with a matched two-cell footprint
/// @param target whole-world chord endpoint
/// @param probe charged known-static sampler
/// @return whether every crossed origin and exact-corner side is known clear
inline bool chord(const movement::Unit& unit, ground::RoutePoint target, Probe& probe) noexcept {
    live_corner_detail::Segment segment{};
    for (unsigned axis = 0; axis < 2; ++axis) {
        if (!live_corner_detail::origin(
                unit.position[axis == 0 ? 0 : 2], 2, segment.from[axis], segment.first[axis]
            ) ||
            !live_corner_detail::origin(
                int64_t(target[axis]) * 65536, 2, segment.to[axis], segment.last[axis]
            ) ||
            segment.first[axis] != unit.cell[axis])
            return false;
    }
    uint32_t count{};
    if (!live_corner_detail::visit_origins(
            segment, probe.remaining, [](int16_t, int16_t) { return true; }, count
        ) ||
        count > probe.budget.remaining)
        return false;
    return live_corner_detail::visit_origins(
        segment,
        count,
        [&](int16_t x, int16_t z) { return probe.at({x, z}) == ground::CorridorCell::clear; },
        count
    );
}

/// Tests whether P1 lies strictly forward along a bounded current-to-P2 chord.
/// The origin-span bound limits products before projection arithmetic.
/// @param unit actual fixed-point ground pose
/// @param navigation canonical route with a real third point
/// @param maximum_origins fixed full-footprint origin allowance
/// @return whether discarding P1 can make forward progress toward P2
inline bool forward_corner(
    const movement::Unit& unit, const ground::Navigation& navigation, uint32_t maximum_origins
) noexcept {
    if (navigation.count < 3)
        return false;
    live_corner_detail::Segment segment{};
    for (unsigned axis = 0; axis < 2; ++axis) {
        const auto position_axis = axis == 0 ? 0 : 2;
        if (!live_corner_detail::origin(
                unit.position[position_axis], 2, segment.from[axis], segment.first[axis]
            ) ||
            !live_corner_detail::origin(
                int64_t(navigation.points[2][axis]) * live_corner_detail::one_world_unit,
                2,
                segment.to[axis],
                segment.last[axis]
            ))
            return false;
    }
    const int32_t cells_x = int32_t(segment.last[0]) - segment.first[0];
    const int32_t cells_z = int32_t(segment.last[1]) - segment.first[1];
    const uint32_t minimum_origins = 1u + uint32_t(std::abs(cells_x)) + uint32_t(std::abs(cells_z));
    if (minimum_origins > maximum_origins)
        return false;
    const int64_t dx = segment.to[0] - segment.from[0];
    const int64_t dz = segment.to[1] - segment.from[1];
    const int64_t px =
        int64_t(navigation.points[1][0]) * live_corner_detail::one_world_unit - unit.position[0];
    const int64_t pz =
        int64_t(navigation.points[1][1]) * live_corner_detail::one_world_unit - unit.position[2];
    const auto length = dx * dx + dz * dz;
    const auto dot = px * dx + pz * dz;
    return dot > 0 && dot < length;
}

/// Converts a legal FP2 native origin into a representable whole-world centre point.
/// @param origin complete native footprint origin
/// @param[out] result whole-world X/Z point, assigned only on success
/// @return whether the origin and its centre fit positive route coordinates
inline bool point(ground::CorridorOrigin origin, ground::RoutePoint& result) noexcept {
    ground::RoutePoint candidate{};
    for (unsigned axis = 0; axis < 2; ++axis) {
        const int32_t coordinate = int32_t(origin[axis]) * 16 + 16;
        if (origin[axis] < 0 || coordinate > std::numeric_limits<int16_t>::max())
            return false;
        candidate[axis] = static_cast<int16_t>(coordinate);
    }
    result = candidate;
    return true;
}
} // namespace local_throat_detail

/// Builds temporary right-hand guidance after proving a short, fully known FP2 throat.
///
/// The current, one footprint back and two fixed goalward lookahead sections
/// are the only seeds on either axis. Open terrain takes at most forty-eight queries.
/// A complete constant-width body and both wider mouths must fit one synchronous
/// 512-query proof. No partial proof is retained. The returned route belongs only
/// to the caller's steering scratch; the canonical Goal and navigator are read.
/// Native movement and the final swept footprint test still decide every step.
///
/// @param unit actual ground pose and matched FP2 footprint
/// @param navigation canonical route and current circle Goal
/// @param sampler known-only static full-footprint classifier
/// @param[in,out] budget caller's query allowance and actual query count
/// @param[out] effective temporary steering route; unchanged when no proof is available
/// @param[out] consume_first optional route-safe P1 consumption proposal; false on failure
/// @return whether complete two-lane guidance was produced
[[nodiscard]] inline bool try_local_throat_guide(
    const sim::unit_movement::Unit& unit,
    const sim::ground_orders::Navigation& navigation,
    const sim::ground_orders::CorridorSampler& sampler,
    sim::ground_orders::CorridorBudget& budget,
    sim::ground_orders::Navigation& effective,
    bool* consume_first = nullptr
) noexcept {
    using namespace local_throat_detail;
    if (consume_first)
        *consume_first = false;
    if (unit.footprint != ground::CorridorOrigin{2, 2} || !navigation.goal ||
        navigation.goal->shape != ground::GoalShape::circle || !ground::route_present(navigation) ||
        navigation.count < 2 || navigation.count > navigation.points.size() || !sampler.origin)
        return false;
    ground::CorridorOrigin target{};
    for (unsigned axis = 0; axis < 2; ++axis) {
        const auto coordinate = int32_t(navigation.points[1][axis]) - 16;
        if (coordinate < 0)
            return false;
        target[axis] = static_cast<int16_t>(coordinate / 16);
    }
    ground::CorridorOrigin following{};
    bool can_consume = consume_first && navigation.count >= 3;
    if (can_consume) {
        for (unsigned axis = 0; axis < 2; ++axis) {
            const int32_t coordinate = int32_t(navigation.points[2][axis]) - 16;
            if (coordinate < 0) {
                can_consume = false;
                break;
            }
            following[axis] = static_cast<int16_t>(coordinate / 16);
        }
        can_consume = can_consume &&
                      forward_corner(
                          unit, navigation, std::min(budget.remaining, local_throat_maximum_probes)
                      );
    }
    const int32_t dx = int32_t(target[0]) - unit.cell[0];
    const int32_t dz = int32_t(target[1]) - unit.cell[1];
    Probe probe{sampler, budget, std::min(budget.remaining, local_throat_maximum_probes)};
    const unsigned preferred_axis = std::abs(dx) >= std::abs(dz) ? 0u : 1u;
    for (const unsigned axis : {preferred_axis, 1u - preferred_axis}) {
        const unsigned minor_axis = 1 - axis;
        const int32_t native_delta = int32_t(target[axis]) - unit.cell[axis];
        const int32_t following_delta = int32_t(following[axis]) - unit.cell[axis];
        const int32_t delta = native_delta != 0 ? native_delta : can_consume ? following_delta : 0;
        const int32_t step = delta > 0 ? 1 : -1;
        if (delta == 0 || (int32_t(navigation.goal->cell[axis]) - unit.cell[axis]) * step <= 0)
            continue;

        ground::CorridorGeometry geometry{};
        bool proven = false;
        for (const int32_t distance : {0, -2, 8, 16}) {
            auto ray = target;
            int32_t ray_delta = native_delta;
            if (distance > std::abs(ray_delta)) {
                if (!can_consume || following_delta * step <= 0 ||
                    distance > std::abs(following_delta))
                    continue;
                ray = following;
                ray_delta = following_delta;
            }
            // The lookback stays in the current lane; bounded forward probes
            // may use P2 when a short P1 hides the entrance ahead.
            const int32_t minor = distance <= 0
                                      ? unit.cell[minor_axis]
                                      : int32_t(unit.cell[minor_axis]) +
                                            (int32_t(ray[minor_axis]) - unit.cell[minor_axis]) *
                                                distance / std::abs(ray_delta);
            const auto seed = origin(axis, int32_t(unit.cell[axis]) + step * distance, minor);
            const auto opening = section(probe, axis, seed[axis], seed[minor_axis]);
            if (opening.kind == SectionKind::unknown)
                break;
            if (opening.kind != SectionKind::narrow)
                continue;
            proven = prove(probe, axis, seed, opening, geometry);
            break;
        }
        if (!proven)
            continue;
        const int32_t first = axis == 0 ? geometry.origins.left : geometry.origins.top;
        const int32_t last = axis == 0 ? geometry.origins.right : geometry.origins.bottom;
        const int32_t exit = step > 0 ? last + 2 : first - 2;
        if ((int32_t(navigation.goal->cell[axis]) - exit) * step < 0 ||
            (int32_t(unit.cell[axis]) - exit) * step >= 0)
            continue;
        bool advance_first = false;
        auto native_next = navigation.points[1];
        if ((int32_t(target[axis]) - exit) * step < 0) {
            const auto& r = geometry.origins;
            if (!can_consume || target[axis] <= first || target[axis] >= last ||
                target[0] < r.left || target[0] > r.right || target[1] < r.top ||
                target[1] > r.bottom || (int32_t(following[axis]) - exit) * step < 0 ||
                !chord(unit, navigation.points[2], probe))
                continue;
            advance_first = true;
            native_next = navigation.points[2];
        }
        int16_t lane{};
        if (!ground::corridor_lane(
                geometry,
                step > 0 ? ground::CorridorDirection::increasing
                         : ground::CorridorDirection::decreasing,
                lane
            ))
            continue;

        const int32_t entry = step > 0 ? first : last;
        const int32_t alignment = entry - step * 4;
        const bool inside = (int32_t(unit.cell[axis]) - entry) * step >= 0;
        const bool aligned =
            unit.cell[minor_axis] == lane && (int32_t(unit.cell[axis]) - alignment) * step >= 0;
        // A body already using the other lane keeps its ordinary route.
        // Steering it across existing traffic cannot establish a new lane.
        if (inside && unit.cell[minor_axis] != lane)
            return false;
        // A late off-lane approach must not reverse toward an alignment point
        // already behind the actual pose. Ordinary steering keeps that approach.
        if (!inside && !aligned && (int32_t(unit.cell[axis]) - alignment) * step > 0)
            return false;
        ground::RoutePoint exit_point{}, next{};
        if (!point(origin(axis, exit, lane), exit_point))
            continue;
        if (inside || aligned)
            next = exit_point;
        else if (!point(origin(axis, alignment, lane), next))
            continue;
        if (!chord(unit, next, probe))
            continue;
        auto result = navigation;
        result.points[0] = {
            static_cast<int16_t>(unit.position[0] >> 16),
            static_cast<int16_t>(unit.position[2] >> 16)
        };
        result.points[1] = next;
        result.points[2] = inside || aligned ? native_next : exit_point;
        result.count = 3;
        result.flags |= ground::route_present_flag;
        effective = result;
        if (consume_first)
            *consume_first = advance_first;
        return true;
    }
    return false;
}

} // namespace oa::sim::match_runtime
