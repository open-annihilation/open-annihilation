// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/unit_movement/local_flow.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
bool watch_allocations{};
size_t allocation_count{};
} // namespace

void* operator new(size_t size) {
    if (watch_allocations)
        ++allocation_count;
    if (void* result = std::malloc(size == 0 ? 1 : size))
        return result;
    throw std::bad_alloc{};
}

void* operator new[](size_t size) {
    return ::operator new(size);
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, size_t) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, size_t) noexcept {
    std::free(pointer);
}

namespace {
namespace movement = oa::sim::unit_movement;
using movement::Fixed;
using Cell = std::array<int16_t, 2>;
using Status = movement::GroundSweepStatus;
using Refusal = movement::GroundSweepRefusal;

constexpr Fixed pixel = 65536;
constexpr int64_t cell_width = 16 * int64_t{pixel};
constexpr int64_t half_cell = 8 * int64_t{pixel};
constexpr uint32_t tick = 155;
constexpr size_t maximum_queries = 256;

int64_t floor_divide(int64_t numerator, int64_t denominator) {
    const auto quotient = numerator / denominator;
    return quotient - (numerator < 0 && numerator % denominator != 0);
}

int16_t origin(Fixed actual, int16_t footprint) {
    return static_cast<int16_t>(
        floor_divide(int64_t(actual) - int64_t(footprint) * half_cell + half_cell, cell_width)
    );
}

Fixed centre(int16_t cell, int16_t footprint) {
    return static_cast<Fixed>((int64_t(cell) * 2 + footprint) * half_cell);
}

Fixed add(Fixed a, Fixed b) {
    return std::bit_cast<Fixed>(uint32_t(a) + uint32_t(b));
}

movement::Unit unit_at(Fixed x, Fixed z, int16_t width = 2, int16_t depth = 2) {
    movement::Unit unit{};
    unit.position = {x, 3 * pixel, z};
    unit.cell = {origin(x, width), origin(z, depth)};
    unit.footprint = {width, depth};
    unit.id = 11;
    unit.heading = 0x1234;
    unit.pitch = 153;
    unit.flags = 1;
    unit.type = {4 * pixel, 900, 0};
    return unit;
}

movement::Movement motion(Fixed x, Fixed z, Fixed y = 0) {
    movement::Movement result{};
    result.velocity = {x, y, z};
    result.speed = 3 * pixel;
    result.turn = 31;
    result.last_motion_tick = 17;
    result.flags = 1 | 0x80;
    return result;
}

bool same(const movement::Unit& a, const movement::Unit& b) {
    return a.position == b.position && a.cell == b.cell && a.footprint == b.footprint &&
           a.id == b.id && a.heading == b.heading && a.pitch == b.pitch && a.flags == b.flags &&
           a.type.maximum_speed == b.type.maximum_speed &&
           a.type.maximum_turn == b.type.maximum_turn && a.type.flags == b.type.flags;
}

bool same(const movement::Movement& a, const movement::Movement& b) {
    return a.velocity == b.velocity && a.speed == b.speed && a.turn == b.turn &&
           a.last_motion_tick == b.last_motion_tick && a.flags == b.flags;
}

/// Holds plot obstacles and callback records without allocating inside a movement step.
struct Host final : movement::Host {
    struct Block {
        Cell plot{};
        int16_t owner{};
    };

    std::array<Block, 32> blocks{};
    size_t block_count{};
    std::array<Cell, maximum_queries> queries{};
    size_t query_count{};
    uint32_t logical_plots{}, actual_plot_reads{};
    std::array<unsigned, 3> changes{};
    std::array<movement::Unit, 3> changed_units{};
    size_t change_count{};
    const movement::Unit* expected_address{};
    bool deny_all{};
    bool refuse_one_origin{};
    Cell refused{};

    void block(Cell plot, int16_t owner = 0) {
        OA_CHECK(block_count < blocks.size());
        if (block_count < blocks.size())
            blocks[block_count++] = {plot, owner};
    }

    bool can_occupy(const movement::Unit& unit, Cell cell, uint8_t occupancy) override {
        OA_CHECK(change_count == 0);
        OA_CHECK(occupancy <= movement::occupancy_mask);
        OA_CHECK(expected_address == nullptr || expected_address == &unit);
        OA_CHECK(query_count < queries.size());
        if (query_count < queries.size())
            queries[query_count] = cell;
        ++query_count;
        logical_plots += uint32_t(unit.footprint[0]) * uint32_t(unit.footprint[1]);
        if (deny_all || (refuse_one_origin && cell == refused))
            return false;
        for (int32_t z = 0; z < unit.footprint[1]; ++z)
            for (int32_t x = 0; x < unit.footprint[0]; ++x) {
                ++actual_plot_reads;
                for (size_t index = 0; index < block_count; ++index)
                    if (int32_t(blocks[index].plot[0]) == int32_t(cell[0]) + x &&
                        int32_t(blocks[index].plot[1]) == int32_t(cell[1]) + z &&
                        blocks[index].owner != unit.id)
                        return false;
            }
        return true;
    }

    void change(unsigned kind, const movement::Unit& unit) {
        OA_CHECK(change_count < changes.size());
        if (change_count < changes.size()) {
            changes[change_count] = kind;
            changed_units[change_count] = unit;
        }
        ++change_count;
    }

    void remove_occupancy(movement::Unit& unit) override { change(1, unit); }

    void insert_occupancy(movement::Unit& unit) override { change(2, unit); }

    void update_spatial_membership(movement::Unit& unit) override { change(3, unit); }
};

struct Rational {
    int64_t numerator{}, denominator{1};
};

Rational ratio(int64_t numerator, int64_t denominator) {
    return denominator < 0 ? Rational{-numerator, -denominator} : Rational{numerator, denominator};
}

int compare(Rational a, Rational b) {
    const auto left = a.numerator * b.denominator;
    const auto right = b.numerator * a.denominator;
    return (left > right) - (left < right);
}

/// Intersects time slabs, independently of any grid-boundary traversal order.
bool occupies_origin(std::array<int64_t, 2> start, std::array<int64_t, 2> velocity, Cell cell) {
    Rational lower{0, 1}, upper{1, 1};
    bool lower_inclusive = true, upper_inclusive = true;
    const auto clip_lower = [&](Rational value, bool inclusive) {
        const auto order = compare(value, lower);
        if (order > 0) {
            lower = value;
            lower_inclusive = inclusive;
        } else if (order == 0)
            lower_inclusive &= inclusive;
    };
    const auto clip_upper = [&](Rational value, bool inclusive) {
        const auto order = compare(value, upper);
        if (order < 0) {
            upper = value;
            upper_inclusive = inclusive;
        } else if (order == 0)
            upper_inclusive &= inclusive;
    };
    for (size_t axis = 0; axis < 2; ++axis) {
        const auto first = int64_t(cell[axis]) * cell_width;
        const auto last = first + cell_width;
        if (velocity[axis] == 0) {
            if (start[axis] < first || start[axis] >= last)
                return false;
        } else if (velocity[axis] > 0) {
            clip_lower(ratio(first - start[axis], velocity[axis]), true);
            clip_upper(ratio(last - start[axis], velocity[axis]), false);
        } else {
            clip_lower(ratio(last - start[axis], velocity[axis]), false);
            clip_upper(ratio(first - start[axis], velocity[axis]), true);
        }
    }
    const auto order = compare(lower, upper);
    return order < 0 || (order == 0 && lower_inclusive && upper_inclusive);
}

/// Adds only genuine simultaneous transitions, preserving native half-open endpoints.
bool touches_diagonal_transition(
    std::array<int64_t, 2> start, std::array<int64_t, 2> velocity, Cell cell
) {
    if (velocity[0] == 0 || velocity[1] == 0)
        return false;
    for (int32_t x = 0; x <= 1; ++x)
        for (int32_t z = 0; z <= 1; ++z) {
            const auto tx = ratio((int64_t(cell[0]) + x) * cell_width - start[0], velocity[0]);
            const auto tz = ratio((int64_t(cell[1]) + z) * cell_width - start[1], velocity[1]);
            if (compare(tx, tz) != 0 || compare(tx, {0, 1}) < 0 || compare(tx, {1, 1}) > 0)
                continue;
            if (compare(tx, {0, 1}) == 0 && !(velocity[0] < 0 && velocity[1] < 0))
                continue;
            if (compare(tx, {1, 1}) == 0 && !(velocity[0] > 0 && velocity[1] > 0))
                continue;
            return true;
        }
    return false;
}

struct OriginSet {
    std::array<Cell, maximum_queries> cells{};
    size_t count{};

    bool contains(Cell cell) const {
        return std::find(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(count), cell) !=
               cells.begin() + static_cast<std::ptrdiff_t>(count);
    }
};

/// Enumerates all intersected origin rectangles with an exact rational oracle.
OriginSet crossed_origins(const movement::Unit& unit, const movement::Movement& moving) {
    OriginSet result{};
    std::array<int64_t, 2> start{}, velocity{};
    Cell last{};
    for (size_t axis = 0; axis < 2; ++axis) {
        start[axis] = int64_t(unit.position[axis * 2]) - int64_t(unit.footprint[axis]) * half_cell +
                      half_cell;
        velocity[axis] = moving.velocity[axis * 2];
        last[axis] = static_cast<int16_t>(floor_divide(start[axis] + velocity[axis], cell_width));
    }
    for (int32_t x = std::min(unit.cell[0], last[0]); x <= std::max(unit.cell[0], last[0]); ++x)
        for (int32_t z = std::min(unit.cell[1], last[1]); z <= std::max(unit.cell[1], last[1]);
             ++z) {
            const Cell cell{static_cast<int16_t>(x), static_cast<int16_t>(z)};
            if (occupies_origin(start, velocity, cell) ||
                touches_diagonal_transition(start, velocity, cell)) {
                OA_CHECK(result.count < result.cells.size());
                if (result.count < result.cells.size())
                    result.cells[result.count++] = cell;
            }
        }
    return result;
}

void check_query_set(const Host& host, const OriginSet& expected, Cell initial, bool complete) {
    for (size_t index = 0; index < std::min(host.query_count, host.queries.size()); ++index) {
        OA_CHECK(host.queries[index] != initial);
        OA_CHECK(expected.contains(host.queries[index]));
        for (size_t earlier = 0; earlier < index; ++earlier)
            OA_CHECK(host.queries[index] != host.queries[earlier]);
    }
    if (complete)
        OA_CHECK(host.query_count + 1 == expected.count);
}

void check_allowance(
    const movement::GroundSweepResult& result, const Host& host, const movement::Unit& before
) {
    OA_CHECK(result.origin_queries == host.query_count);
    OA_CHECK(result.plot_allowance_used == host.logical_plots);
    OA_CHECK(
        result.plot_allowance_used ==
        host.query_count * size_t(before.footprint[0]) * size_t(before.footprint[1])
    );
    OA_CHECK(host.query_count < movement::swept_ground_origin_limit);
    OA_CHECK(result.plot_allowance_used <= movement::swept_ground_plot_limit);
}

void compare_legacy_blocked(
    const movement::Unit& before,
    const movement::Movement& command,
    const movement::Unit& actual,
    const movement::Movement& actual_motion
) {
    auto legacy_unit = before;
    auto legacy_motion = command;
    Host denied{};
    denied.deny_all = true;
    movement::integrate_unattached(legacy_unit, legacy_motion, tick, true, denied);
    OA_CHECK(same(actual, legacy_unit));
    OA_CHECK(same(actual_motion, legacy_motion));
}

/// A real positive-duration intermediate footprint must not be skipped for a clear endpoint.
void positive_interval_witness_is_blocked() {
    auto before = unit_at(358103482, 351797247);
    before.id = 11;
    before.heading = 26773;
    before.type.maximum_speed = 117964;
    before.type.maximum_turn = 1120;
    auto command = motion(-35735, 54935);
    command.speed = 65535;
    command.turn = -1120;
    command.last_motion_tick = 154;
    command.flags |= movement::collision_blocked;
    OA_CHECK(before.cell == Cell({341, 334}));
    const auto expected = crossed_origins(before, command);
    OA_CHECK(expected.contains({341, 335}));
    OA_CHECK(expected.contains({340, 335}));
    // Z enters its next cell at 1/54935; X leaves at 14778/35735.
    OA_CHECK(int64_t{35735} < int64_t{14778} * 54935);
    auto legacy_unit = before;
    auto legacy_motion = command;
    Host legacy_host{};
    legacy_host.block({342, 336}, 16);
    movement::integrate_unattached(legacy_unit, legacy_motion, tick, true, legacy_host);
    OA_CHECK(legacy_unit.cell == Cell({340, 335}));
    OA_CHECK((legacy_motion.flags & movement::collision_blocked) == 0);
    auto unit = before;
    auto moving = command;
    Host host{};
    host.expected_address = &unit;
    host.block({342, 336}, 16);
    const auto result = movement::integrate_swept_ground(unit, moving, tick, true, host);
    OA_CHECK(result.status == Status::blocked && result.refusal == Refusal::none);
    OA_CHECK(result.refused_origin_valid && result.refused_origin == Cell({341, 335}));
    OA_CHECK(unit.cell == before.cell);
    OA_CHECK(unit.position[0] == 358088705 && unit.position[2] == 351797247);
    OA_CHECK(host.change_count == 0);
    OA_CHECK((moving.flags & movement::collision_blocked) != 0);
    OA_CHECK(moving.speed == 58982);
    OA_CHECK(moving.velocity == movement::aim_velocity(before.heading, 0, 58982));
    OA_CHECK(moving.last_motion_tick == tick);
    check_query_set(host, expected, before.cell, false);
    check_allowance(result, host, before);
    compare_legacy_blocked(before, command, unit, moving);

    auto wrapped_unit = before;
    auto wrapped_motion = command;
    Host wrapped_host{};
    wrapped_host.expected_address = &wrapped_unit;
    wrapped_host.block({342, 336}, 16);
    movement::LocalFlowCounters counters{};
    counters.blocked_ticks = 3;
    counters.probes = 11;
    const auto wrapped = movement::integrate_swept_local_flow(
        wrapped_unit, wrapped_motion, tick, true, wrapped_host, counters
    );
    OA_CHECK(wrapped.status == Status::blocked && wrapped.refusal == Refusal::none);
    OA_CHECK(same(wrapped_unit, unit) && same(wrapped_motion, moving));
    OA_CHECK(wrapped_unit.position[0] != before.position[0]);
    OA_CHECK(wrapped_motion.last_motion_tick == tick);
    OA_CHECK(counters.blocked_ticks == 4 && counters.probes == 11);
    OA_CHECK(wrapped.origin_queries == 1 && wrapped.plot_allowance_used == 4);
    OA_CHECK(wrapped_host.query_count == 1 && wrapped_host.change_count == 0);
}

/// Full footprints see both stationary features and held mobile plots before the endpoint.
void intermediate_static_and_mobile_plots_block() {
    for (const int16_t owner : {int16_t{0}, int16_t{16}}) {
        const auto before = unit_at(centre(2, 2), centre(3, 2));
        const auto command = motion(4 * 16 * pixel, 0, 19);
        auto unit = before;
        auto moving = command;
        Host host{};
        host.block({4, 3}, owner);
        const auto result = movement::integrate_swept_ground(unit, moving, tick, true, host);
        OA_CHECK(result.status == Status::blocked && result.refused_origin_valid);
        OA_CHECK(host.change_count == 0 && unit.cell == before.cell);
        check_allowance(result, host, before);
        compare_legacy_blocked(before, command, unit, moving);
    }
}

/// Each orthogonal footprint can block an exact corner crossing with a clear endpoint.
void both_diagonal_sides_keep_the_collision_veto() {
    const auto before = unit_at(centre(0, 2), centre(0, 2));
    const auto command = motion(16 * pixel, 16 * pixel);
    const auto expected = crossed_origins(before, command);
    for (const Cell side : {Cell{1, 0}, Cell{0, 1}}) {
        OA_CHECK(expected.contains(side));
        const Cell blocked_plot = side[0] == 1 ? Cell{2, 0} : Cell{0, 2};
        auto legacy_unit = before;
        auto legacy_motion = command;
        Host legacy{};
        legacy.block(blocked_plot, 16);
        movement::integrate_unattached(legacy_unit, legacy_motion, tick, true, legacy);
        OA_CHECK(legacy_unit.cell == Cell({1, 1}));
        OA_CHECK((legacy_motion.flags & movement::collision_blocked) == 0);
        auto unit = before;
        auto moving = command;
        Host host{};
        host.block(blocked_plot, 16);
        const auto result = movement::integrate_swept_ground(unit, moving, tick, true, host);
        OA_CHECK(result.status == Status::blocked && result.refused_origin_valid);
        OA_CHECK(result.refused_origin == side);
        OA_CHECK(host.change_count == 0);
        check_query_set(host, expected, before.cell, false);
        check_allowance(result, host, before);
        compare_legacy_blocked(before, command, unit, moving);
    }
}

/// Half-open endpoints and exact diagonal transitions agree with an enumeration oracle.
void crossed_origin_sets_match_exact_oracle() {
    constexpr std::array<Fixed, 7> phases{
        0, 1, pixel - 1, 8 * pixel, 16 * pixel - 2, 16 * pixel - 1, 7 * pixel
    };
    constexpr std::array<std::array<Fixed, 2>, 12> velocities{
        {{16 * pixel, 16 * pixel},
         {-16 * pixel, -16 * pixel},
         {16 * pixel, -16 * pixel},
         {-16 * pixel, 16 * pixel},
         {32 * pixel, 16 * pixel},
         {-32 * pixel, 16 * pixel},
         {16 * pixel - 1, 16 * pixel},
         {16 * pixel + 1, 16 * pixel},
         {0, 32 * pixel},
         {32 * pixel, 0},
         {1, -1},
         {-1, 1}}
    };
    for (const Fixed phase : phases)
        for (const auto velocity : velocities) {
            const auto before = unit_at(-24 * pixel + phase, -40 * pixel + phase);
            const auto command = motion(velocity[0], velocity[1], 7);
            const auto expected = crossed_origins(before, command);
            auto unit = before;
            auto moving = command;
            Host host{};
            const auto result = movement::integrate_swept_ground(unit, moving, tick, true, host);
            OA_CHECK(result.status == Status::clear);
            check_query_set(host, expected, before.cell, true);
            check_allowance(result, host, before);
        }
}

/// Every supported rectangular footprint retains native accepted-step state and callbacks.
void clear_steps_preserve_legacy_state_and_lifecycle() {
    for (int16_t width = 1; width <= 8; ++width)
        for (int16_t depth = 1; depth <= 8; ++depth) {
            const auto before = unit_at(centre(5, width), centre(7, depth), width, depth);
            const auto command = motion(16 * pixel + 7, -16 * pixel - 11, 37);
            auto legacy_unit = before;
            auto legacy_motion = command;
            Host legacy{};
            movement::integrate_unattached(legacy_unit, legacy_motion, tick, true, legacy);
            auto unit = before;
            auto moving = command;
            Host host{};
            const auto result = movement::integrate_swept_ground(unit, moving, tick, true, host);
            OA_CHECK(result.status == Status::clear && result.refusal == Refusal::none);
            OA_CHECK(same(unit, legacy_unit) && same(moving, legacy_motion));
            OA_CHECK(host.change_count == 3);
            OA_CHECK((host.changes == std::array<unsigned, 3>({1, 2, 3})));
            OA_CHECK(same(host.changed_units[0], before));
            OA_CHECK(host.changed_units[1].cell == unit.cell);
            OA_CHECK(host.changed_units[1].position == unit.position);
            OA_CHECK(host.changed_units[2].cell == unit.cell);
            check_query_set(host, crossed_origins(before, command), before.cell, true);
            check_allowance(result, host, before);
        }
}

/// Convex same-cell motion uses no new collision query and retains the historical blocked bit.
void same_cell_and_no_motion_keep_native_behavior() {
    for (const auto velocity : std::array<std::array<Fixed, 3>, 4>{
             {{0, 0, 0}, {1, 0, -1}, {0, 19, 0}, {pixel, -7, pixel}}
         }) {
        const auto before = unit_at(centre(3, 2), centre(4, 2));
        auto command = motion(velocity[0], velocity[2], velocity[1]);
        command.flags |= movement::collision_blocked;
        auto legacy_unit = before;
        auto legacy_motion = command;
        Host legacy{};
        movement::integrate_unattached(legacy_unit, legacy_motion, tick, true, legacy);
        auto unit = before;
        auto moving = command;
        Host host{};
        host.deny_all = true;
        const auto result = movement::integrate_swept_ground(unit, moving, tick, true, host);
        OA_CHECK(
            result.status ==
            (velocity == std::array<Fixed, 3>{0, 0, 0} ? Status::unchanged : Status::clear)
        );
        OA_CHECK(host.query_count == 0 && host.change_count == 0);
        OA_CHECK(result.origin_queries == 0 && result.plot_allowance_used == 0);
        OA_CHECK(same(unit, legacy_unit) && same(moving, legacy_motion));
    }
}

/// Query budgets count the initial origin during preflight and charge full queried footprints.
void query_budgets_are_preflighted_and_charged() {
    for (const int16_t footprint : {int16_t{1}, int16_t{2}, int16_t{8}}) {
        const auto limit = std::min<uint32_t>(128, 512 / uint32_t(footprint * footprint));
        const auto before =
            unit_at(centre(0, footprint), centre(0, footprint), footprint, footprint);
        for (const bool over_budget : {false, true}) {
            const auto steps = limit - (over_budget ? 0 : 1);
            const auto command = motion(static_cast<Fixed>(steps * 16 * pixel), 0);
            auto unit = before;
            auto moving = command;
            Host host{};
            const auto result = movement::integrate_swept_ground(unit, moving, tick, true, host);
            if (over_budget) {
                OA_CHECK(
                    result.status == Status::unsupported && result.refusal == Refusal::query_budget
                );
                OA_CHECK(host.query_count == 0 && host.change_count == 0);
                OA_CHECK(result.origin_queries == 0 && result.plot_allowance_used == 0);
                compare_legacy_blocked(before, command, unit, moving);
            } else {
                OA_CHECK(result.status == Status::clear && host.query_count == limit - 1);
                check_allowance(result, host, before);
            }
        }
    }
    auto unit = unit_at(centre(0, 2), centre(0, 2));
    auto moving = motion(-16 * pixel, 0);
    Host host{};
    host.block({-1, 0});
    const auto result = movement::integrate_swept_ground(unit, moving, tick, true, host);
    OA_CHECK(result.status == Status::blocked);
    OA_CHECK(host.actual_plot_reads == 1);
    OA_CHECK(result.origin_queries == 1 && result.plot_allowance_used == 4);
}

/// Endpoint blockage still uses the complete native blocked-step behavior.
void blocked_endpoint_preserves_flags_speed_and_attempted_y() {
    const auto before = unit_at(centre(3, 2), centre(4, 2));
    for (const Fixed speed : {pixel, 3 * pixel}) {
        auto command = motion(16 * pixel, 0, 1234);
        command.speed = speed;
        auto unit = before;
        auto moving = command;
        Host host{};
        host.block({5, 4}, 16);
        const auto result = movement::integrate_swept_ground(unit, moving, tick, true, host);
        OA_CHECK(result.status == Status::blocked && result.refused_origin == Cell({4, 4}));
        OA_CHECK(unit.position[1] == before.position[1] + 1234);
        OA_CHECK(moving.speed == std::min(speed, before.type.maximum_speed / 2));
        OA_CHECK(
            moving.velocity == (speed > before.type.maximum_speed / 2
                                    ? movement::aim_velocity(before.heading, 0, moving.speed)
                                    : command.velocity)
        );
        OA_CHECK(moving.last_motion_tick == tick && moving.turn == command.turn);
        OA_CHECK((moving.flags & 0x80) != 0);
        OA_CHECK(host.change_count == 0);
        compare_legacy_blocked(before, command, unit, moving);
    }
}

/// Unsupported local-ground work fails closed without consulting occupancy or moving membership.
void unsupported_work_is_closed_and_explicit() {
    const auto check_refusal =
        [](movement::Unit before, movement::Movement command, Refusal refusal, bool clamp) {
            auto unit = before;
            auto moving = command;
            Host host{};
            const auto result = movement::integrate_swept_ground(unit, moving, tick, true, host);
            OA_CHECK(result.status == Status::unsupported && result.refusal == refusal);
            OA_CHECK(!result.refused_origin_valid);
            OA_CHECK(host.query_count == 0 && host.change_count == 0);
            OA_CHECK(result.origin_queries == 0 && result.plot_allowance_used == 0);
            OA_CHECK(unit.cell == before.cell);
            OA_CHECK((moving.flags & movement::collision_blocked) != 0);
            OA_CHECK(moving.last_motion_tick == tick);
            OA_CHECK(unit.position[1] == add(before.position[1], command.velocity[1]));
            OA_CHECK(moving.speed == std::min(command.speed, before.type.maximum_speed / 2));
            if (clamp)
                compare_legacy_blocked(before, command, unit, moving);
            else {
                OA_CHECK(unit.position[0] == before.position[0]);
                OA_CHECK(unit.position[2] == before.position[2]);
            }
        };
    check_refusal(
        unit_at(centre(3, 9), centre(4, 2), 9, 2),
        motion(16 * pixel, 0, 17),
        Refusal::footprint,
        true
    );
    for (const int16_t width : {int16_t{0}, int16_t{-1}}) {
        auto invalid = unit_at(centre(3, 2), centre(4, 2));
        invalid.footprint[0] = width;
        check_refusal(invalid, motion(16 * pixel, 0, 19), Refusal::footprint, false);
    }
    auto mismatched = unit_at(centre(3, 2), centre(4, 2));
    ++mismatched.cell[0];
    check_refusal(mismatched, motion(16 * pixel, 0, 23), Refusal::origin_mismatch, false);
    auto bad_bias = unit_at(centre(3, 2), centre(4, 2));
    bad_bias.position[0] = std::numeric_limits<Fixed>::min();
    check_refusal(bad_bias, motion(1, 0, 29), Refusal::coordinate_range, false);
    auto wrapped = unit_at(centre(3, 2), centre(4, 2));
    wrapped.position[0] = std::numeric_limits<Fixed>::max() - 10;
    wrapped.cell[0] = origin(wrapped.position[0], 2);
    check_refusal(wrapped, motion(20, 0, 31), Refusal::coordinate_range, false);
    const auto valid = unit_at(centre(3, 2), centre(4, 2));
    check_refusal(
        valid, motion(std::numeric_limits<Fixed>::max(), 0, 37), Refusal::coordinate_range, true
    );
}

/// Remote simulation and every non-ground transition preserve the legacy boundary exactly.
void remote_and_layer_changes_delegate_unchanged() {
    for (const bool local : {false, true})
        for (const uint8_t current : {uint8_t{0}, uint8_t{1}, uint8_t{2}})
            for (const uint8_t requested : {uint8_t{0}, uint8_t{1}, uint8_t{2}}) {
                if (local && current == 1 && requested == 1)
                    continue;
                auto before = unit_at(centre(3, 2), centre(4, 2));
                before.flags = (before.flags & ~movement::occupancy_mask) | current;
                auto command = motion(16 * pixel, -pixel, 43);
                command.flags = uint8_t((command.flags & ~movement::occupancy_mask) | requested);
                for (const bool previous_blocked : {false, true}) {
                    command.flags = uint8_t(
                        (command.flags & ~movement::collision_blocked) |
                        (previous_blocked ? movement::collision_blocked : 0)
                    );
                    auto legacy_unit = before;
                    auto legacy_motion = command;
                    Host legacy{};
                    legacy.deny_all = true;
                    movement::integrate_unattached(legacy_unit, legacy_motion, tick, local, legacy);
                    auto unit = before;
                    auto moving = command;
                    Host host{};
                    host.deny_all = true;
                    const auto result =
                        movement::integrate_swept_ground(unit, moving, tick, local, host);
                    OA_CHECK(result.status == Status::delegated && result.refusal == Refusal::none);
                    OA_CHECK(same(unit, legacy_unit) && same(moving, legacy_motion));
                    OA_CHECK(host.query_count == legacy.query_count);
                    OA_CHECK(
                        host.change_count == legacy.change_count && host.changes == legacy.changes
                    );
                    OA_CHECK(result.origin_queries == 0 && result.plot_allowance_used == 0);
                }
            }
}

/// Enhanced motion clocks keep actual position changes while native clocks keep attempts.
void local_flow_wrapper_preserves_motion_clock_accounting() {
    const auto before = unit_at(centre(3, 2) + 8 * pixel - 1, centre(4, 2));
    const auto command = motion(pixel, 0);
    auto direct_unit = before;
    auto direct_motion = command;
    Host direct_host{};
    direct_host.deny_all = true;
    const auto direct =
        movement::integrate_swept_ground(direct_unit, direct_motion, tick, true, direct_host);
    OA_CHECK(direct.status == Status::blocked && direct_unit.position == before.position);
    OA_CHECK(direct_motion.last_motion_tick == tick);
    auto wrapped_unit = before;
    auto wrapped_motion = command;
    Host wrapped_host{};
    wrapped_host.deny_all = true;
    movement::LocalFlowCounters counters{};
    counters.blocked_ticks = 9;
    counters.probes = 31;
    const auto wrapped = movement::integrate_swept_local_flow(
        wrapped_unit, wrapped_motion, tick, true, wrapped_host, counters
    );
    OA_CHECK(wrapped.status == Status::blocked && wrapped_unit.position == before.position);
    OA_CHECK(wrapped_motion.last_motion_tick == command.last_motion_tick);
    OA_CHECK(counters.blocked_ticks == 10);
    OA_CHECK(counters.probes == 31);
    OA_CHECK(wrapped.origin_queries == wrapped_host.query_count);
    auto progressing = before;
    auto progressing_motion = motion(pixel, 0, 7);
    Host progressing_host{};
    progressing_host.deny_all = true;
    movement::LocalFlowCounters progressing_counters{};
    const auto progressed = movement::integrate_swept_local_flow(
        progressing, progressing_motion, tick, true, progressing_host, progressing_counters
    );
    OA_CHECK(progressed.status == Status::blocked && progressing.position != before.position);
    OA_CHECK(progressing.position[1] == before.position[1] + 7);
    OA_CHECK(progressing_motion.last_motion_tick == tick);
}

/// Repeated accepted and rejected sweeps allocate no storage inside the step.
void movement_steps_do_not_allocate() {
    const auto before = unit_at(centre(3, 2), centre(4, 2));
    const auto command = motion(16 * pixel, 16 * pixel);
    allocation_count = 0;
    watch_allocations = true;
    for (unsigned repeat = 0; repeat < 16; ++repeat) {
        auto unit = before;
        auto moving = command;
        Host host{};
        host.deny_all = repeat % 2 != 0;
        movement::LocalFlowCounters counters{};
        (void)movement::integrate_swept_local_flow(unit, moving, tick, true, host, counters);
    }
    watch_allocations = false;
    OA_CHECK(allocation_count == 0);
}

} // namespace

int main() {
    positive_interval_witness_is_blocked();
    intermediate_static_and_mobile_plots_block();
    both_diagonal_sides_keep_the_collision_veto();
    crossed_origin_sets_match_exact_oracle();
    clear_steps_preserve_legacy_state_and_lifecycle();
    same_cell_and_no_motion_keep_native_behavior();
    query_budgets_are_preflighted_and_charged();
    blocked_endpoint_preserves_flags_speed_and_attempted_y();
    unsupported_work_is_closed_and_explicit();
    remote_and_layer_changes_delegate_unchanged();
    local_flow_wrapper_preserves_motion_clock_accounting();
    movement_steps_do_not_allocate();
    return oa::test::check_exit_status();
}
