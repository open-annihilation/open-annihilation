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
