// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/unit_movement/local_flow.hpp"
#include "oa/base/game_math.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <limits>

namespace oa::sim::unit_movement {
namespace {

constexpr Fixed one_world_unit = 65536;
constexpr Fixed cell_step = 16 * one_world_unit;
constexpr unsigned maximum_probe_steps = 8;
constexpr int16_t maximum_flow_footprint = 8;
constexpr uint16_t eighth_turn = 8192;
constexpr uint16_t quarter_turn = 16384;
constexpr uint16_t half_turn = 32768;

/// Converts a unit centre to its footprint-origin cell.
///
/// @param position signed 16.16 coordinate
/// @param footprint footprint width along the coordinate
/// @return the footprint-origin cell
int16_t cell_for(Fixed position, int16_t footprint) noexcept {
    constexpr uint32_t half_cell = 8 * one_world_unit;
    const auto biased = uint32_t(position) - uint32_t(footprint) * half_cell + half_cell;
    return static_cast<int16_t>(std::bit_cast<Fixed>(biased) >> 20);
}

/// Locates the footprint at a candidate position.
///
/// @param unit unit providing its footprint
/// @param position candidate signed 16.16 position
/// @return footprint-origin cell
std::array<int16_t, 2> cell_at(const Unit& unit, const std::array<Fixed, 3>& position) noexcept {
    return {cell_for(position[0], unit.footprint[0]), cell_for(position[2], unit.footprint[1])};
}

/// Advances a bounded distance along a planar vector without wrapping.
///
/// @param origin starting position
/// @param dx vector X component
/// @param dz vector Z component
/// @param distance distance to advance
/// @param length vector length, greater than zero
/// @param[out] out candidate position, unchanged when it is outside fixed-point range
/// @return whether the candidate fits
bool offset_point(
    const std::array<Fixed, 3>& origin,
    Fixed dx,
    Fixed dz,
    Fixed distance,
    Fixed length,
    std::array<Fixed, 3>& out
) noexcept {
    if (length <= 0)
        return false;
    const int64_t x = int64_t(origin[0]) + int64_t(dx) * distance / length;
    const int64_t z = int64_t(origin[2]) + int64_t(dz) * distance / length;
    if (x < std::numeric_limits<Fixed>::min() || x > std::numeric_limits<Fixed>::max() ||
        z < std::numeric_limits<Fixed>::min() || z > std::numeric_limits<Fixed>::max())
        return false;
    out = {static_cast<Fixed>(x), origin[1], static_cast<Fixed>(z)};
    return true;
}

/// Counts and performs one final occupancy query.
///
/// @param unit moving unit
/// @param cell candidate footprint origin
/// @param occupancy movement layer
/// @param host occupancy operations
/// @param[in,out] counters footprint-query count
/// @return whether the footprint is clear
bool clear_cell(
    const Unit& unit,
    std::array<int16_t, 2> cell,
    uint8_t occupancy,
    Host& host,
    LocalFlowCounters& counters
) {
    ++counters.probes;
    return host.can_occupy(unit, cell, occupancy);
}

/// Tests a bounded short segment, including the two sides of diagonal cell crossings.
///
/// @param unit moving unit
/// @param target candidate signed 16.16 position
/// @param occupancy movement layer
/// @param host occupancy operations
/// @param[in,out] counters footprint-query count
/// @return whether all sampled footprints and adjoining diagonal cells are clear
bool clear_segment(
    const Unit& unit,
    const std::array<Fixed, 3>& target,
    uint8_t occupancy,
    Host& host,
    LocalFlowCounters& counters
) {
    const int64_t wide_x = int64_t(target[0]) - unit.position[0];
    const int64_t wide_z = int64_t(target[2]) - unit.position[2];
    if (wide_x < std::numeric_limits<Fixed>::min() || wide_x > std::numeric_limits<Fixed>::max() ||
        wide_z < std::numeric_limits<Fixed>::min() || wide_z > std::numeric_limits<Fixed>::max())
        return false;
    const auto dx = static_cast<Fixed>(wide_x), dz = static_cast<Fixed>(wide_z);
    const auto length = static_cast<Fixed>(oa::base::game_math::distance(dx, dz));
    if (length <= 0 || length > cell_step * 2 + one_world_unit)
        return false;
    // Eight-world-unit samples cannot skip an occupied footprint cell.
    constexpr Fixed segment_step = cell_step / 2;
    auto previous = unit.cell;
    for (Fixed travelled = segment_step;; travelled += segment_step) {
        std::array<Fixed, 3> point;
        if (!offset_point(unit.position, dx, dz, std::min(travelled, length), length, point))
            return false;
        const auto cell = cell_at(unit, point);
        if (cell != previous) {
            if (!clear_cell(unit, cell, occupancy, host, counters))
                return false;
            if (cell[0] != previous[0] && cell[1] != previous[1] &&
                (!clear_cell(unit, {cell[0], previous[1]}, occupancy, host, counters) ||
                 !clear_cell(unit, {previous[0], cell[1]}, occupancy, host, counters)))
                return false;
            previous = cell;
        }
        if (travelled >= length)
            return true;
    }
}

} // namespace

bool local_flow_head_reached(const Unit& unit, const LocalFlowHead& head) noexcept {
    if (!head.active || head.count < 2)
        return false;
    const auto target_x = int64_t(head.points[1][0]) * one_world_unit;
    const auto target_z = int64_t(head.points[1][1]) * one_world_unit;
    const auto dx = int64_t(unit.position[0]) - target_x;
    const auto dz = int64_t(unit.position[2]) - target_z;
    constexpr int64_t arrival_distance = 5 * one_world_unit;
    if (dx > -arrival_distance && dx < arrival_distance && dz > -arrival_distance &&
        dz < arrival_distance && dx * dx + dz * dz < arrival_distance * arrival_distance)
        return true;
    // Keep the leg in whole world units and the position in fixed point.
    // The mixed-scale dot product fits int64_t even at signed map limits.
    const auto leg_x = int64_t(head.points[1][0]) - head.points[0][0];
    const auto leg_z = int64_t(head.points[1][1]) - head.points[0][1];
    return (leg_x != 0 || leg_z != 0) && dx * leg_x + dz * leg_z >= 0;
}

bool local_flow_completed_last_tick(const LocalFlowHead& head, uint32_t tick) noexcept {
    return !head.active && head.count == local_flow_completed_count &&
           uint32_t(tick - head.tick) == 1;
}

void record_local_flow_block(LocalFlowHead& head, bool blocked) noexcept {
    constexpr uint8_t maximum_blocked_ticks = local_flow_retry_ticks + 1;
    head.blocked_ticks =
        blocked ? static_cast<uint8_t>(
                      std::min<unsigned>(unsigned(head.blocked_ticks) + 1, maximum_blocked_ticks)
                  )
                : uint8_t{0};
}

bool local_flow_defer_replan(const LocalFlowHead& head, const Movement& movement) noexcept {
    return head.mobile_blocked && (movement.flags & collision_blocked) != 0 &&
           head.blocked_ticks < local_flow_retry_ticks;
}

bool local_flow_recovery_due(const LocalFlowHead& head, uint32_t tick, uint16_t unit_id) noexcept {
    return head.blocked_ticks >= local_flow_retry_ticks &&
           (uint64_t(tick) + unit_id) % local_flow_retry_ticks == 0;
}

LocalFlowDecision choose_local_flow(
    const Unit& unit,
    const Movement& movement,
    std::array<Fixed, 3> target,
    uint32_t tick,
    Host& host,
    LocalFlowProbe probe,
    LocalFlowCounters& counters
) {
    LocalFlowDecision result{target};
    const auto occupancy = uint8_t(movement.flags & occupancy_mask);
    if (probe.at == nullptr || occupancy != 1 || unit.type.maximum_speed <= 0 ||
        unit.footprint[0] <= 0 || unit.footprint[1] <= 0 ||
        unit.footprint[0] > maximum_flow_footprint || unit.footprint[1] > maximum_flow_footprint)
        return result;
    const int64_t wide_x = int64_t(target[0]) - unit.position[0];
    const int64_t wide_z = int64_t(target[2]) - unit.position[2];
    if (wide_x <= std::numeric_limits<Fixed>::min() || wide_x > std::numeric_limits<Fixed>::max() ||
        wide_z <= std::numeric_limits<Fixed>::min() || wide_z > std::numeric_limits<Fixed>::max())
        return result;
    const auto dx = static_cast<Fixed>(wide_x), dz = static_cast<Fixed>(wide_z);
    const auto unsigned_length = oa::base::game_math::distance(dx, dz);
    if (unsigned_length > uint32_t(std::numeric_limits<Fixed>::max()))
        return result;
    const auto length = static_cast<Fixed>(unsigned_length);
    if (length < cell_step)
        return result;
    LocalFlowBlocker blocker;
    Fixed blocker_distance = 0;
    auto previous = unit.cell;
    for (unsigned step = 1; step <= maximum_probe_steps; ++step) {
        const auto travelled = std::min(length, cell_step * static_cast<Fixed>(step));
        std::array<Fixed, 3> point;
        if (!offset_point(unit.position, dx, dz, travelled, length, point))
            break;
        const auto cell = cell_at(unit, point);
        if (cell != previous) {
            ++counters.probes;
            blocker = probe.at(probe.context, unit, cell, occupancy);
            if (blocker.obstacle == LocalFlowObstacle::stationary)
                return result;
            if (blocker.obstacle == LocalFlowObstacle::moving) {
                blocker_distance = travelled;
                break;
            }
            previous = cell;
        }
        if (travelled >= length)
            break;
    }
    if (blocker.obstacle != LocalFlowObstacle::moving)
        return result;
    ++counters.moving_blockers;
    result.mobile_blocked = true;
    // Traffic turning together still follows the same direction even while
    // its current headings differ from the route ahead.
    const int difference = std::bit_cast<int16_t>(uint16_t(unit.heading - blocker.heading));
    if (std::abs(difference) < eighth_turn && blocker.waiting_ticks <= 1)
        return result;
    const auto unit_id = static_cast<uint16_t>(unit.id);
    const auto last_progress = unit_id < probe.progress_at_tick_start.size()
                                   ? probe.progress_at_tick_start[unit_id]
                                   : movement.last_motion_tick;
    const auto waiting = tick - last_progress;
    if (waiting > blocker.waiting_ticks ||
        (waiting == blocker.waiting_ticks && unit_id < blocker.unit_id))
        return result;
    ++counters.yielded_ticks;
    const auto heading = oa::base::game_math::direction(-dx, -dz);
    std::array<int32_t, 2> previous_leg{};
    if (probe.previous_head != nullptr && !probe.allow_reverse) {
        auto previous = *probe.previous_head;
        previous.active = true;
        if (local_flow_head_reached(unit, previous))
            previous_leg = {
                int32_t(previous.points[1][0]) - previous.points[0][0],
                int32_t(previous.points[1][1]) - previous.points[0][1]
            };
    }
    const auto reverses_leg = [&](const std::array<Fixed, 3>& candidate) {
        if (previous_leg[0] == 0 && previous_leg[1] == 0)
            return false;
        const auto step_x = int64_t(candidate[0]) - unit.position[0];
        const auto step_z = int64_t(candidate[2]) - unit.position[2];
        if (step_x * previous_leg[0] + step_z * previous_leg[1] >= 0)
            return false;
        const auto forward = step_x * dx + step_z * dz;
        const auto cross = step_x * dz - step_z * dx;
        // A sideways return must not undo the previous passing leg. Forward
        // rejoining still works, and mature recovery may reverse when needed.
        constexpr int64_t minimum_forward_ratio_denominator = 4;
        return forward * minimum_forward_ratio_denominator <= std::abs(cross);
    };
    constexpr std::array<int32_t, 4> turns{-eighth_turn, eighth_turn, -quarter_turn, quarter_turn};
    for (const auto reach : {cell_step, cell_step * 2})
        for (const auto angle : turns) {
            const auto velocity = aim_velocity(static_cast<uint16_t>(heading + angle), 0, reach);
            std::array<Fixed, 3> candidate;
            if (!offset_point(unit.position, velocity[0], velocity[2], reach, reach, candidate) ||
                reverses_leg(candidate) ||
                !clear_segment(unit, candidate, occupancy, host, counters))
                continue;
            result.target = candidate;
            result.adjusted = true;
            ++counters.steering_adjustments;
            return result;
        }
    // Do not back away from distant traffic: approach a passing place first.
    if (blocker_distance <= cell_step * 2) {
        const auto velocity =
            aim_velocity(static_cast<uint16_t>(heading + half_turn), 0, cell_step);
        std::array<Fixed, 3> candidate;
        if (offset_point(
                unit.position, velocity[0], velocity[2], cell_step, cell_step, candidate
            ) &&
            !reverses_leg(candidate) && clear_segment(unit, candidate, occupancy, host, counters)) {
            result.reverse = true;
            result.adjusted = true;
            result.target = candidate;
            ++counters.steering_adjustments;
        }
    }
    return result;
}

void integrate_local_flow(
    Unit& unit,
    Movement& movement,
    uint32_t tick,
    bool local_simulation,
    Host& host,
    LocalFlowCounters& counters
) {
    const auto before = unit.position;
    const auto last_progress = movement.last_motion_tick;
    integrate_unattached(unit, movement, tick, local_simulation, host);
    if (before == unit.position)
        movement.last_motion_tick = last_progress;
    if (local_simulation && (movement.flags & collision_blocked) != 0) {
        ++counters.blocked_ticks;
    }
}

bool write_local_flow_heads(
    std::span<const LocalFlowHead> heads, std::span<uint8_t> bytes
) noexcept {
    if (bytes.size() / local_flow_head_bytes != heads.size() ||
        bytes.size() % local_flow_head_bytes != 0)
        return false;
    size_t offset = 0;
    const auto write = [&](uint64_t value, unsigned count) {
        for (unsigned byte = 0; byte < count; ++byte)
            bytes[offset++] = static_cast<uint8_t>(value >> (byte * 8));
    };
    for (const auto& head : heads) {
        for (const auto& point : head.points)
            for (const auto coordinate : point)
                write(static_cast<uint16_t>(coordinate), 2);
        write(head.goal_key, 8);
        write(head.tick, 4);
        write(head.count, 1);
        write((head.active ? 1u : 0u) | (head.mobile_blocked ? 2u : 0u), 1);
        write(head.blocked_ticks, 1);
    }
    return true;
}

bool read_local_flow_heads(
    std::span<LocalFlowHead> heads, std::span<const uint8_t> bytes
) noexcept {
    if (bytes.size() / local_flow_head_bytes != heads.size() ||
        bytes.size() % local_flow_head_bytes != 0)
        return false;
    // Validate the complete table before changing any live head.
    for (size_t offset = 0; offset < bytes.size(); offset += local_flow_head_bytes)
        if (bytes[offset + 24] > 3 || bytes[offset + 24] == 1 || bytes[offset + 25] > 3 ||
            bytes[offset + 26] > local_flow_retry_ticks + 1)
            return false;
    size_t offset = 0;
    const auto read = [&](unsigned count) {
        uint64_t value = 0;
        for (unsigned byte = 0; byte < count; ++byte)
            value |= uint64_t(bytes[offset++]) << (byte * 8);
        return value;
    };
    for (auto& head : heads) {
        for (auto& point : head.points)
            for (auto& coordinate : point)
                coordinate = std::bit_cast<int16_t>(static_cast<uint16_t>(read(2)));
        head.goal_key = read(8);
        head.tick = static_cast<uint32_t>(read(4));
        head.count = static_cast<uint8_t>(read(1));
        const auto flags = read(1);
        head.active = (flags & 1) != 0;
        head.mobile_blocked = (flags & 2) != 0;
        head.blocked_ticks = static_cast<uint8_t>(read(1));
    }
    return true;
}

} // namespace oa::sim::unit_movement
