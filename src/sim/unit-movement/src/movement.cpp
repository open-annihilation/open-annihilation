// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/unit_movement/movement.hpp"
#include "oa/base/game_math.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <optional>

namespace oa::sim::unit_movement {
namespace {
constexpr std::array<int16_t, 512> sine_table = {
#include "oa/base/game_math/trig_table.inc"
};
constexpr std::array<int8_t, 11> slope_speed_percent{25, 55, 70, 85, 100, 100, 75, 50, 25, 20, 15};
constexpr int fraction_bits = 16, trig_fraction_bits = 13, slope_shift = 11,
              maximum_slope_bucket = 5;
constexpr uint32_t trig_index_bias = 0x20, quarter_turn = 0x4000;
constexpr unsigned trig_index_shift = 7, trig_index_mask = 511;
constexpr int64_t trig_rounding_bias = 0x1000, fixed_one = 65536;
constexpr uint32_t half_cell = 0x80000, cell_interior_radius = half_cell - 1;

Fixed bits(uint32_t v) noexcept {
    return std::bit_cast<Fixed>(v);
}

Fixed add(Fixed a, Fixed b) noexcept {
    return bits(uint32_t(a) + uint32_t(b));
}

Fixed negate(Fixed a) noexcept {
    return bits(0u - uint32_t(a));
}

Fixed low(int64_t a) noexcept {
    return bits(static_cast<uint32_t>(a));
}

void velocity(Unit& u, Movement& m) noexcept {
    m.velocity = aim_velocity(u.heading, 0, m.speed);
}

int16_t narrow16(uint32_t a) noexcept {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(a));
}

int16_t cell_for(Fixed position, int16_t footprint) noexcept {
    const auto biased = uint32_t(position) - uint32_t(footprint) * half_cell + half_cell;
    return narrow16(uint32_t(bits(biased) >> 20));
}
} // namespace

Fixed sine_scaled(uint16_t heading, Fixed magnitude) noexcept {
    const auto sample =
        ((uint32_t(heading) + trig_index_bias) >> trig_index_shift) & trig_index_mask;
    return low(
        (int64_t(sine_table[sample]) * magnitude + trig_rounding_bias) >> trig_fraction_bits
    );
}

Fixed cosine_scaled(uint16_t heading, Fixed magnitude) noexcept {
    return sine_scaled(static_cast<uint16_t>(heading + quarter_turn), magnitude);
}

std::array<Fixed, 3> aim_velocity(uint16_t heading, uint16_t pitch, Fixed speed) noexcept {
    const auto vertical = sine_scaled(pitch, speed);
    const auto level = cosine_scaled(pitch, speed);
    return {negate(sine_scaled(heading, level)), vertical, negate(cosine_scaled(heading, level))};
}

void turn(Unit& u, Movement& m, int16_t requested) noexcept {
    if (requested == 0) {
        m.turn = 0;
        return;
    }
    const int limit = u.type.maximum_turn;
    m.turn = narrow16(uint32_t(std::clamp(int(requested), -limit, limit)));
    u.heading = static_cast<uint16_t>(u.heading + static_cast<uint16_t>(m.turn));
    u.flags |= position_dirty;
}

void accelerate(Unit& u, Movement& m, Fixed acceleration, uint8_t sea_level) noexcept {
    m.speed = std::max(add(m.speed, acceleration), 0);
    const int slope =
        std::clamp(int(u.pitch) >> slope_shift, -maximum_slope_bucket, maximum_slope_bucket);
    // The game narrows the product to 32 bits between its two 64-bit operations.
    const auto weighted =
        low(int64_t(u.type.maximum_speed) *
            slope_speed_percent[std::size_t(slope + maximum_slope_bucket)]);
    Fixed maximum = low(int64_t(weighted) / 100);
    const auto height = narrow16(uint32_t(u.position[1]) >> fraction_bits);
    if (height < sea_level && (u.type.flags & water_speed_exemption_mask) == 0)
        maximum >>= 1;
    m.speed = std::min(m.speed, maximum);
    velocity(u, m);
}

bool facing_toward(
    int32_t from_x, int32_t from_z, int32_t to_x, int32_t to_z, uint16_t facing
) noexcept {
    const auto bearing = oa::base::game_math::direction(to_x - from_x, to_z - from_z);
    constexpr Fixed scale = static_cast<Fixed>(0x140000);
    const auto high_neg = [](Fixed value) {
        const auto negated = static_cast<uint32_t>(-value);
        return static_cast<int16_t>(static_cast<uint16_t>(negated >> 16));
    };
    const auto sin_facing = high_neg(sine_scaled(facing, scale));
    const auto cos_facing = high_neg(cosine_scaled(facing, scale));
    const auto sin_bearing = high_neg(sine_scaled(bearing, scale));
    const auto cos_bearing = high_neg(cosine_scaled(bearing, scale));
    const auto dot = static_cast<int16_t>(sin_facing * sin_bearing + cos_facing * cos_bearing);
    return dot > 0;
}

AimAngles aim_angles(std::array<Fixed, 3> from, std::array<Fixed, 3> to) noexcept {
    const auto dx = bits(static_cast<uint32_t>(from[0]) - static_cast<uint32_t>(to[0]));
    const auto dy = bits(static_cast<uint32_t>(from[1]) - static_cast<uint32_t>(to[1]));
    const auto dz = bits(static_cast<uint32_t>(from[2]) - static_cast<uint32_t>(to[2]));
    const auto horizontal = oa::base::game_math::distance(dx, dz);
    const auto dy_high = narrow16(static_cast<uint32_t>(dy) >> 16);
    const auto range_high = narrow16(horizontal >> 16);
    return {
        oa::base::game_math::direction(dx, dz),
        oa::base::game_math::direction(
            -static_cast<int32_t>(dy_high), static_cast<int32_t>(range_high)
        )
    };
}

std::optional<int64_t> braking_distance(Fixed speed, Fixed deceleration) noexcept {
    const auto denominator = add(deceleration, deceleration);
    if (denominator == 0)
        return std::nullopt;
    const auto squared = low((int64_t(speed) * speed) >> fraction_bits);
    return (int64_t(squared) * fixed_one) / denominator;
}

void integrate_unattached(Unit& u, Movement& m, uint32_t tick, bool local_simulation, Host& host) {
    std::array<Fixed, 3> next;
    for (std::size_t i = 0; i < next.size(); ++i)
        next[i] = add(u.position[i], m.velocity[i]);
    const auto occupancy = uint8_t(m.flags & occupancy_mask);
    if (next == u.position && occupancy == (u.flags & occupancy_mask))
        return;
    m.last_motion_tick = tick;
    const std::array<int16_t, 2> cell{
        cell_for(next[0], u.footprint[0]), cell_for(next[2], u.footprint[1])
    };
    if (cell == u.cell && occupancy == (u.flags & occupancy_mask)) {
        u.position = next;
        u.flags |= position_dirty;
        return;
    }
    if (local_simulation) {
        const bool blocked = !host.can_occupy(u, cell, occupancy);
        m.flags = uint8_t((m.flags & ~collision_blocked) | (blocked ? collision_blocked : 0));
    }
    if ((m.flags & collision_blocked) == 0) {
        host.remove_occupancy(u);
        u.position = next;
        u.cell = cell;
        u.flags = (u.flags & ~occupancy_mask) | occupancy;
        host.insert_occupancy(u);
        u.flags |= position_dirty;
        host.update_spatial_membership(u);
    } else {
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const auto center =
                bits((uint32_t(u.footprint[axis]) + uint32_t(u.cell[axis]) * 2u) * half_cell);
            const auto lower = bits(uint32_t(center) - cell_interior_radius);
            const auto upper = bits(uint32_t(center) + cell_interior_radius);
            // Keep the game's upper-first signed comparisons, including wraparound.
            auto& coordinate = next[axis * 2];
            if (coordinate > upper)
                coordinate = upper;
            else if (coordinate < lower)
                coordinate = lower;
        }
        const Fixed reduced = u.type.maximum_speed / 2;
        if (m.speed > reduced) {
            m.speed = reduced;
            velocity(u, m);
        }
        u.position = next;
        u.flags |= position_dirty;
    }
}

namespace {
constexpr int64_t sweep_cell_step = 16 * int64_t{65536};

struct GroundSweepSegment {
    std::array<int64_t, 2> from{}, to{};
    std::array<int16_t, 2> first{}, last{};
};

struct GroundSweepPreparation {
    GroundSweepResult result{};
    GroundSweepSegment segment{};
    std::array<Fixed, 2> lower{}, upper{};
    uint16_t limit{}, area{};
    bool clamp_current{};
};

/// Converts a signed biased fixed coordinate to its native origin, rounding down.
int64_t sweep_floor(int64_t numerator) noexcept {
    const auto quotient = numerator / sweep_cell_step;
    return quotient - (numerator % sweep_cell_step < 0 ? 1 : 0);
}

/// Tells whether a wide coordinate retains the signed native fixed-point range.
bool sweep_fixed_range(int64_t value) noexcept {
    return value >= std::numeric_limits<Fixed>::min() && value <= std::numeric_limits<Fixed>::max();
}

/// Visits actual native origins; simultaneous crossings include both side origins.
template <class Visit>
bool visit_ground_sweep(
    const GroundSweepSegment& segment, uint16_t limit, Visit&& visit, uint16_t& count
) {
    count = 0;
    const auto emit = [&](int32_t x, int32_t z) {
        ++count;
        return count <= limit && visit(std::array<int16_t, 2>{int16_t(x), int16_t(z)});
    };
    int32_t x = segment.first[0], z = segment.first[1];
    if (!emit(x, z))
        return false;
    const int64_t dx = segment.to[0] - segment.from[0];
    const int64_t dz = segment.to[1] - segment.from[1];
    const int32_t step_x = (dx > 0) - (dx < 0), step_z = (dz > 0) - (dz < 0);
    const auto absolute_x = dx < 0 ? -dx : dx, absolute_z = dz < 0 ? -dz : dz;
    int64_t next_x = step_x > 0 ? int64_t(x + 1) * sweep_cell_step - segment.from[0]
                                : segment.from[0] - int64_t(x) * sweep_cell_step;
    int64_t next_z = step_z > 0 ? int64_t(z + 1) * sweep_cell_step - segment.from[1]
                                : segment.from[1] - int64_t(z) * sweep_cell_step;
    while (x != segment.last[0] || z != segment.last[1]) {
        if (x == segment.last[0] ||
            (z != segment.last[1] && next_z * absolute_x < next_x * absolute_z)) {
            z += step_z;
            next_z += sweep_cell_step;
            if (!emit(x, z))
                return false;
        } else if (z == segment.last[1] || next_x * absolute_z < next_z * absolute_x) {
            x += step_x;
            next_x += sweep_cell_step;
            if (!emit(x, z))
                return false;
        } else {
            if (!emit(x + step_x, z) || !emit(x, z + step_z))
                return false;
            x += step_x;
            z += step_z;
            next_x += sweep_cell_step;
            next_z += sweep_cell_step;
            if (!emit(x, z))
                return false;
        }
    }
    return true;
}

/// Validates the current cell and bounds the complete desired chord before queries.
GroundSweepPreparation prepare_ground_sweep(const Unit& unit, const Movement& movement) noexcept {
    GroundSweepPreparation prepared{};
    auto& result = prepared.result;
    for (const auto footprint : unit.footprint)
        if (footprint <= 0) {
            result.refusal = GroundSweepRefusal::footprint;
            return prepared;
        }
    for (size_t axis = 0; axis < 2; ++axis) {
        const auto biased = int64_t(unit.position[axis * 2]) -
                            int64_t(unit.footprint[axis]) * half_cell + half_cell;
        if (!sweep_fixed_range(biased)) {
            result.refusal = GroundSweepRefusal::coordinate_range;
            return prepared;
        }
        const auto current = sweep_floor(biased);
        if (current != unit.cell[axis]) {
            result.refusal = GroundSweepRefusal::origin_mismatch;
            return prepared;
        }
        const auto centre =
            (int64_t(unit.footprint[axis]) + int64_t(unit.cell[axis]) * 2) * half_cell;
        const auto lower = centre - cell_interior_radius;
        const auto upper = centre + cell_interior_radius;
        if (!sweep_fixed_range(lower) || !sweep_fixed_range(upper)) {
            result.refusal = GroundSweepRefusal::coordinate_range;
            return prepared;
        }
        prepared.segment.from[axis] = biased;
        prepared.segment.first[axis] = unit.cell[axis];
        prepared.lower[axis] = static_cast<Fixed>(lower);
        prepared.upper[axis] = static_cast<Fixed>(upper);
    }
    prepared.clamp_current = true;
    if (unit.footprint[0] > swept_ground_maximum_footprint ||
        unit.footprint[1] > swept_ground_maximum_footprint) {
        result.refusal = GroundSweepRefusal::footprint;
        return prepared;
    }
    prepared.area = uint16_t(unit.footprint[0] * unit.footprint[1]);
    prepared.limit = std::min<uint16_t>(
        swept_ground_origin_limit, uint16_t(swept_ground_plot_limit / prepared.area)
    );
    for (size_t axis = 0; axis < 2; ++axis) {
        const auto next = int64_t(unit.position[axis * 2]) + movement.velocity[axis * 2];
        const auto biased = next - int64_t(unit.footprint[axis]) * half_cell + half_cell;
        if (!sweep_fixed_range(next) || !sweep_fixed_range(biased)) {
            result.refusal = GroundSweepRefusal::coordinate_range;
            return prepared;
        }
        prepared.segment.to[axis] = biased;
        prepared.segment.last[axis] = static_cast<int16_t>(sweep_floor(biased));
    }
    const int32_t cells_x = int32_t(prepared.segment.last[0]) - prepared.segment.first[0];
    const int32_t cells_z = int32_t(prepared.segment.last[1]) - prepared.segment.first[1];
    const auto minimum_origins =
        1 + (cells_x < 0 ? -cells_x : cells_x) + (cells_z < 0 ? -cells_z : cells_z);
    if (minimum_origins > prepared.limit) {
        result.refusal = GroundSweepRefusal::query_budget;
        return prepared;
    }
    // The axis span is now below 128 cells. Boundary-distance products in
    // the count and query traversals are below 2^55 and fit signed int64_t.
    uint16_t count{};
    if (!visit_ground_sweep(
            prepared.segment, prepared.limit, [](std::array<int16_t, 2>) { return true; }, count
        )) {
        result.refusal = GroundSweepRefusal::query_budget;
        return prepared;
    }
    result.status = GroundSweepStatus::clear;
    return prepared;
}

/// Applies native blocked-step effects without changing registered occupancy.
void block_swept_ground(
    Unit& unit,
    Movement& movement,
    std::array<Fixed, 3> next,
    const GroundSweepPreparation& prepared
) noexcept {
    movement.flags |= collision_blocked;
    for (size_t axis = 0; axis < 2; ++axis) {
        auto& coordinate = next[axis * 2];
        if (!prepared.clamp_current)
            coordinate = unit.position[axis * 2];
        else if (coordinate > prepared.upper[axis])
            coordinate = prepared.upper[axis];
        else if (coordinate < prepared.lower[axis])
            coordinate = prepared.lower[axis];
    }
    const Fixed reduced = unit.type.maximum_speed / 2;
    if (movement.speed > reduced) {
        movement.speed = reduced;
        velocity(unit, movement);
    }
    unit.position = next;
    unit.flags |= position_dirty;
}
} // namespace

GroundSweepResult integrate_swept_ground(
    Unit& unit, Movement& movement, uint32_t tick, bool local_simulation, Host& host
) {
    const auto occupancy = uint8_t(movement.flags & occupancy_mask);
    if (!local_simulation || occupancy != 1 || (unit.flags & occupancy_mask) != 1) {
        integrate_unattached(unit, movement, tick, local_simulation, host);
        return {GroundSweepStatus::delegated};
    }
    std::array<Fixed, 3> next{};
    for (size_t axis = 0; axis < next.size(); ++axis)
        next[axis] = add(unit.position[axis], movement.velocity[axis]);
    if (next == unit.position)
        return {GroundSweepStatus::unchanged};
    movement.last_motion_tick = tick;
    auto prepared = prepare_ground_sweep(unit, movement);
    auto& result = prepared.result;
    if (result.status != GroundSweepStatus::clear) {
        block_swept_ground(unit, movement, next, prepared);
        return result;
    }
    if (prepared.segment.last == unit.cell) {
        unit.position = next;
        unit.flags |= position_dirty;
        return result;
    }
    uint16_t visited{};
    const auto clear_origin = [&](std::array<int16_t, 2> origin) {
        if (origin == unit.cell)
            return true;
        ++result.origin_queries;
        result.plot_allowance_used += prepared.area;
        if (host.can_occupy(unit, origin, occupancy))
            return true;
        result.status = GroundSweepStatus::blocked;
        result.refused_origin = origin;
        result.refused_origin_valid = true;
        return false;
    };
    if (!visit_ground_sweep(prepared.segment, prepared.limit, clear_origin, visited)) {
        block_swept_ground(unit, movement, next, prepared);
        return result;
    }
    movement.flags &= uint8_t(~collision_blocked);
    host.remove_occupancy(unit);
    unit.position = next;
    unit.cell = prepared.segment.last;
    unit.flags = (unit.flags & ~occupancy_mask) | occupancy;
    host.insert_occupancy(unit);
    unit.flags |= position_dirty;
    host.update_spatial_membership(unit);
    return result;
}

void write_position(
    Unit& u, Movement&, std::array<Fixed, 3> position, uint8_t occupancy, Host& host
) {
    occupancy = static_cast<uint8_t>(occupancy & occupancy_mask);
    const std::array<int16_t, 2> cell{
        cell_for(position[0], u.footprint[0]), cell_for(position[2], u.footprint[1])
    };
    if (cell == u.cell && occupancy == (u.flags & occupancy_mask))
        u.position = position;
    else {
        host.remove_occupancy(u);
        u.position = position;
        u.cell = cell;
        u.flags = (u.flags & ~occupancy_mask) | occupancy;
        host.insert_occupancy(u);
        host.update_spatial_membership(u);
    }
    u.flags |= position_dirty;
}
} // namespace oa::sim::unit_movement
