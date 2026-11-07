// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "advisory_lane_guides.hpp"
#include "live_route_corner.hpp"

#include <algorithm>
#include <limits>

namespace oa::sim::match_runtime {
namespace {
namespace ground = ground_orders;
constexpr int32_t cell_width = 16, half_cell = 8;
constexpr int64_t fixed_one = 65536;

uint64_t goal_geometry(const ground::Goal& goal) noexcept {
    uint64_t key = 14695981039346656037ull;
    const auto add = [&key](uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            key ^= static_cast<uint8_t>(value >> (byte * 8));
            key *= 1099511628211ull;
        }
    };
    add(static_cast<uint32_t>(goal.shape));
    add(static_cast<uint16_t>(goal.cell[0]));
    add(static_cast<uint16_t>(goal.cell[1]));
    for (const auto value :
         {goal.tolerance,
          goal.radius_squared,
          goal.inner_range,
          goal.inner_radius_squared,
          goal.left,
          goal.right,
          goal.top,
          goal.bottom})
        add(static_cast<uint32_t>(value));
    return key;
}

bool contains(ground::CorridorRectangle r, ground::CorridorOrigin p) noexcept {
    return p[0] >= r.left && p[0] <= r.right && p[1] >= r.top && p[1] <= r.bottom;
}

bool touches(const unit_movement::Unit& unit, const ground::CorridorGeometry& c) noexcept {
    return unit.cell[0] <= c.plots.right &&
           int32_t(unit.cell[0]) + unit.footprint[0] - 1 >= c.plots.left &&
           unit.cell[1] <= c.plots.bottom &&
           int32_t(unit.cell[1]) + unit.footprint[1] - 1 >= c.plots.top;
}

bool beyond_exit(
    const unit_movement::Unit& unit,
    const ground::CorridorGeometry& corridor,
    ground::CorridorDirection direction
) noexcept {
    const auto axis = corridor.axis == ground::CorridorAxis::x ? 0u : 1u;
    const auto first = axis == 0 ? corridor.plots.left : corridor.plots.top;
    const auto last = axis == 0 ? corridor.plots.right : corridor.plots.bottom;
    return direction == ground::CorridorDirection::increasing
               ? unit.cell[axis] > last
               : int32_t(unit.cell[axis]) + unit.footprint[axis] - 1 < first;
}

bool entered_body(
    const unit_movement::Unit& unit,
    const ground::CorridorGeometry& corridor,
    ground::CorridorDirection direction
) noexcept {
    if (contains(corridor.origins, unit.cell))
        return true;
    if (!touches(unit, corridor))
        return false;
    const auto axis = corridor.axis == ground::CorridorAxis::x ? 0u : 1u;
    const auto lower = axis == 0 ? corridor.origins.top : corridor.origins.left;
    const auto upper = axis == 0 ? corridor.origins.bottom : corridor.origins.right;
    if (unit.cell[1 - axis] < lower || unit.cell[1 - axis] > upper)
        return false;
    const auto first = axis == 0 ? corridor.origins.left : corridor.origins.top;
    const auto last = axis == 0 ? corridor.origins.right : corridor.origins.bottom;
    return direction == ground::CorridorDirection::increasing ? unit.cell[axis] > last
                                                              : unit.cell[axis] < first;
}

bool point_at(
    const ground::CorridorGeometry& c,
    ground::CorridorDirection direction,
    int16_t lane,
    bool exit,
    ground::RoutePoint& point
) noexcept {
    const bool increasing = direction == ground::CorridorDirection::increasing;
    const auto& mouth = c.mouths[(increasing == exit) ? 1 : 0];
    const auto axis = c.axis == ground::CorridorAxis::x ? 0u : 1u;
    const int32_t major = increasing == exit ? (axis == 0 ? mouth.right : mouth.bottom)
                                             : (axis == 0 ? mouth.left : mouth.top);
    std::array<int32_t, 2> origin{};
    origin[axis] = major;
    origin[1 - axis] = lane;
    for (unsigned at = 0; at < 2; ++at) {
        const auto world = origin[at] * cell_width + int32_t(c.footprint[at]) * half_cell;
        if (world < std::numeric_limits<int16_t>::min() ||
            world > std::numeric_limits<int16_t>::max())
            return false;
        point[at] = static_cast<int16_t>(world);
    }
    return true;
}

bool known_segment(
    const unit_movement::Unit& unit,
    ground::RoutePoint to,
    const ground::CorridorSampler& sampler,
    ground::CorridorBudget& budget
) noexcept {
    if (!sampler.origin || unit.footprint[0] <= 0 || unit.footprint[1] <= 0 ||
        unit.footprint[0] > live_route_corner_maximum_footprint ||
        unit.footprint[1] > live_route_corner_maximum_footprint)
        return false;
    live_corner_detail::Segment segment{};
    for (unsigned axis = 0; axis < 2; ++axis)
        if (!live_corner_detail::origin(
                unit.position[axis == 0 ? 0 : 2],
                unit.footprint[axis],
                segment.from[axis],
                segment.first[axis]
            ) ||
            !live_corner_detail::origin(
                int64_t(to[axis]) * fixed_one,
                unit.footprint[axis],
                segment.to[axis],
                segment.last[axis]
            ))
            return false;
    const auto limit = std::min(
        live_route_corner_maximum_origins,
        live_route_corner_maximum_plots / uint32_t(unit.footprint[0] * unit.footprint[1])
    );
    uint32_t count{};
    if (!live_corner_detail::visit_origins(
            segment, limit, [](int16_t, int16_t) { return true; }, count
        ) ||
        count > budget.remaining)
        return false;
    uint32_t queried{};
    return live_corner_detail::visit_origins(
        segment,
        count,
        [&](int16_t x, int16_t z) {
            --budget.remaining;
            ++budget.used;
            return sampler.origin(sampler.context, {x, z}, unit.footprint) ==
                   ground::CorridorCell::clear;
        },
        queried
    );
}

int suffix_offset(const ground::Navigation& n, const AdvisoryLaneState& s) noexcept {
    if (!ground::route_present(n) || n.count < 2 || n.count > n.points.size() ||
        n.count > s.admitted_count)
        return -1;
    for (uint32_t offset = 0; offset + n.count <= s.admitted_count; ++offset) {
        bool same = true;
        for (uint32_t at = 1; at < n.count; ++at)
            same &= n.points[at] == s.admitted_route[offset + at];
        if (same)
            return static_cast<int>(offset);
    }
    return -1;
}

bool passed_point(
    const unit_movement::Unit& unit, ground::RoutePoint point, const AdvisoryLaneState& s
) noexcept {
    ground::CorridorOrigin origin{};
    int64_t biased{};
    for (unsigned axis = 0; axis < 2; ++axis)
        if (!live_corner_detail::origin(
                int64_t(point[axis]) * fixed_one, unit.footprint[axis], biased, origin[axis]
            ))
            return false;
    if (!contains(s.corridor.origins, origin) && !contains(s.corridor.mouths[0], origin) &&
        !contains(s.corridor.mouths[1], origin))
        return false;
    const auto axis = s.corridor.axis == ground::CorridorAxis::x ? 0u : 1u;
    const auto actual = unit.position[axis == 0 ? 0 : 2];
    return s.direction == ground::CorridorDirection::increasing
               ? actual >= int64_t(point[axis]) * fixed_one
               : actual <= int64_t(point[axis]) * fixed_one;
}

void rejoin(
    const unit_movement::Unit& unit,
    const ground::Navigation& n,
    const ground::CorridorSampler& sampler,
    ground::CorridorBudget& budget,
    const AdvisoryLaneState& s,
    AdvisoryLaneDecision& out
) noexcept {
    const auto offset = suffix_offset(n, s);
    if (offset >= 0) {
        for (uint32_t target = 1; target < n.count; ++target) {
            bool traversed = true;
            for (uint32_t at = 1; at < target; ++at) {
                const auto original = uint32_t(offset) + at;
                traversed &= original + 1 >= s.entry_segment &&
                             (!s.exit_segment || original < s.exit_segment) &&
                             passed_point(unit, n.points[at], s);
            }
            if (!traversed)
                break;
            auto target_unit = unit;
            for (unsigned axis = 0; axis < 2; ++axis) {
                int64_t biased{};
                if (!live_corner_detail::origin(
                        int64_t(n.points[target][axis]) * fixed_one,
                        unit.footprint[axis],
                        biased,
                        target_unit.cell[axis]
                    )) {
                    out.request_search = true;
                    return;
                }
            }
            const auto axis = s.corridor.axis == ground::CorridorAxis::x ? 0u : 1u;
            const auto target_plane = int64_t(n.points[target][axis]) * fixed_one;
            const auto actual_plane = unit.position[axis == 0 ? 0 : 2];
            const bool ahead = s.direction == ground::CorridorDirection::increasing
                                   ? target_plane > actual_plane
                                   : target_plane < actual_plane;
            if (!ahead || !beyond_exit(target_unit, s.corridor, s.direction) ||
                !known_segment(unit, n.points[target], sampler, budget))
                continue;
            out.rejoin[0] = {
                static_cast<int16_t>(unit.position[0] >> 16),
                static_cast<int16_t>(unit.position[2] >> 16)
            };
            out.rejoin_count = 1 + n.count - target;
            std::copy_n(n.points.begin() + target, n.count - target, out.rejoin.begin() + 1);
            out.consumed_prefix = static_cast<uint16_t>(target - 1);
            return;
        }
    }
    out.request_search = true;
}
} // namespace

AdvisoryLaneDecision choose_advisory_lane(
    const unit_movement::Unit& unit,
    const ground_orders::Navigation& n,
    uint64_t generation,
    const ground_orders::CorridorApproach* approach,
    const ground_orders::CorridorGeometry* geometry,
    const ground_orders::CorridorSampler& sampler,
    ground_orders::CorridorBudget& budget,
    AdvisoryLaneState& state
) noexcept {
    AdvisoryLaneDecision out;
    out.steering = n;
    const bool was_active = state.steering_active;
    const auto old_phase = state.phase;
    const auto* old_goal = state.goal;
    const auto* old_order = state.order;
    const auto old_generation = state.generation;
    const auto old_goal_geometry = state.goal_geometry;
    const auto old_corridor = state.corridor.id;
    const auto old_revision = state.corridor.revision;
    const auto old_direction = state.direction;
    const auto key = n.goal ? goal_geometry(*n.goal) : 0;
    if (n.goal && ground::goal_contains_unit(*n.goal, unit)) {
        state = {};
        out.reset_local_head = was_active;
        return out;
    }
    const bool identity = n.goal && n.goal == state.goal && n.goal->order == state.order &&
                          generation == state.generation && key == state.goal_geometry;
    if (!identity || !geometry || geometry->id != state.corridor.id ||
        geometry->revision != state.corridor.revision || geometry->footprint != unit.footprint)
        state = {};
    if (state.phase != AdvisoryLanePhase::none && state.entered &&
        beyond_exit(unit, state.corridor, state.direction)) {
        out.released = true;
        out.corridor = state.corridor.id;
        rejoin(unit, n, sampler, budget, state, out);
        state = {};
        out.reset_local_head = true;
        return out;
    }
    const bool inside =
        state.phase != AdvisoryLanePhase::none && state.entered && touches(unit, state.corridor);
    if (!inside && state.phase != AdvisoryLanePhase::none &&
        (!approach || approach->id != state.corridor.id || approach->direction != state.direction))
        state = {};
    if (state.phase == AdvisoryLanePhase::none && n.goal && n.goal->order && geometry && approach &&
        geometry->id != 0 && approach->id == geometry->id && approach->exits_other_end &&
        geometry->footprint == unit.footprint && n.count >= 2 && n.count <= n.points.size() &&
        ground::route_present(n)) {
        int16_t lane{};
        if (ground::corridor_lane(*geometry, approach->direction, lane) &&
            !beyond_exit(unit, *geometry, approach->direction)) {
            state.goal = n.goal;
            state.order = n.goal->order;
            state.generation = generation;
            state.goal_geometry = key;
            state.corridor = *geometry;
            state.direction = approach->direction;
            state.lane = lane;
            state.entered = entered_body(unit, *geometry, approach->direction);
            state.phase = state.entered ? AdvisoryLanePhase::crossing : AdvisoryLanePhase::approach;
            // First observed inside units have no traversed prefix recorded by this guide.
            if (!state.entered) {
                state.admitted_route = n.points;
                state.admitted_count = n.count;
                state.entry_segment = approach->entry_segment;
                state.exit_segment = approach->exit_segment;
            }
        }
    }
    if (state.phase != AdvisoryLanePhase::none) {
        if (entered_body(unit, state.corridor, state.direction)) {
            state.entered = true;
            state.phase = AdvisoryLanePhase::crossing;
        }
        ground::RoutePoint entrance{}, exit{};
        if (point_at(state.corridor, state.direction, state.lane, false, entrance) &&
            point_at(state.corridor, state.direction, state.lane, true, exit)) {
            // Match ordinary waypoint consumption before choosing the next temporary leg.
            const int64_t dx = (unit.position[0] >> 16) - entrance[0];
            const int64_t dz = (unit.position[2] >> 16) - entrance[1];
            constexpr int64_t waypoint_reached_squared = 26;
            const bool entrance_reached = state.phase == AdvisoryLanePhase::approach &&
                                          dx * dx + dz * dz < waypoint_reached_squared;
            bool crossing = state.phase == AdvisoryLanePhase::crossing || entrance_reached;
            bool visible = false;
            if (crossing) {
                visible = known_segment(unit, exit, sampler, budget);
                if (visible)
                    state.phase = AdvisoryLanePhase::crossing;
                else if (!state.entered) {
                    state.phase = AdvisoryLanePhase::approach;
                    crossing = false;
                }
            }
            if (!crossing)
                visible = known_segment(unit, entrance, sampler, budget);
            if (visible) {
                out.steering.points[0] = {
                    static_cast<int16_t>(unit.position[0] >> 16),
                    static_cast<int16_t>(unit.position[2] >> 16)
                };
                out.steering.points[1] = crossing ? exit : entrance;
                out.steering.points[2] = exit;
                out.steering.count = 3;
                out.steering.flags |= ground::route_present_flag;
                out.active = true;
                out.corridor = state.corridor.id;
                out.phase = state.phase;
            }
        }
    }
    state.steering_active = out.active;
    out.reset_local_head =
        was_active != out.active ||
        ((was_active || out.active) &&
         (old_phase != state.phase || old_goal != state.goal || old_order != state.order ||
          old_generation != state.generation || old_goal_geometry != state.goal_geometry ||
          old_corridor != state.corridor.id || old_revision != state.corridor.revision ||
          old_direction != state.direction));
    return out;
}

} // namespace oa::sim::match_runtime
