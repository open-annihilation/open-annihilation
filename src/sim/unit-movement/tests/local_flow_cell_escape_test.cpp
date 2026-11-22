// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/unit_movement/local_flow.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <vector>

using namespace oa::sim::unit_movement;

namespace {
constexpr Fixed fixed_one = 65536;
constexpr int16_t map_width = 20;
constexpr int16_t map_height = 20;
constexpr uint8_t ground_layer = 1;
constexpr uint16_t east = 49152;
constexpr uint16_t west = 16384;
constexpr uint32_t decision_tick = 120;
constexpr uint64_t maximum_queries = 128;

/// Locates the native origin of a two-cell footprint.
std::array<int16_t, 2> origin(std::array<Fixed, 3> point) {
    return {
        static_cast<int16_t>((point[0] - 8 * fixed_one) >> 20),
        static_cast<int16_t>((point[2] - 8 * fixed_one) >> 20)
    };
}

/// Checks every externally visible decision field without native padding.
bool equal(const LocalFlowDecision& a, const LocalFlowDecision& b) {
    return a.target == b.target && a.adjusted == b.adjusted && a.reverse == b.reverse &&
           a.mobile_blocked == b.mobile_blocked;
}

/// Checks selected-head counters, excluding additional clearance probes.
bool equal_outcomes(const LocalFlowCounters& a, const LocalFlowCounters& b) {
    return a.moving_blockers == b.moving_blockers &&
           a.steering_adjustments == b.steering_adjustments && a.yielded_ticks == b.yielded_ticks &&
           a.blocked_ticks == b.blocked_ticks && a.deferred_replans == b.deferred_replans;
}

struct Grid final : Host {
    std::array<int16_t, size_t(map_width) * map_height> occupants{};
    std::array<bool, size_t(map_width) * map_height> features{};
    std::array<Unit, 4> units{};
    size_t count{};
    uint32_t overlaps{};
    std::vector<std::array<int16_t, 2>> queries{};

    /// Maps a plot coordinate into the fixed fixture.
    static size_t index(int16_t x, int16_t z) { return size_t(z) * map_width + size_t(x); }

    /// Tests the complete footprint against the fixture's bounds.
    static bool inside(const Unit& unit, std::array<int16_t, 2> cell) {
        return cell[0] >= 0 && cell[1] >= 0 && cell[0] + unit.footprint[0] <= map_width &&
               cell[1] + unit.footprint[1] <= map_height;
    }

    /// Registers one mobile unit with its actual footprint claims.
    Unit& add(int16_t x, int16_t z, std::array<int16_t, 2> footprint) {
        auto& unit = units.at(count++);
        unit.id = static_cast<int16_t>(count);
        unit.cell = {x, z};
        unit.footprint = footprint;
        unit.position = {
            (x * 16 + footprint[0] * 8) * fixed_one, 0, (z * 16 + footprint[1] * 8) * fixed_one
        };
        unit.flags = ground_layer;
        unit.heading = west;
        unit.type.maximum_speed = 2 * fixed_one;
        unit.type.maximum_turn = 4096;
        insert_occupancy(unit);
        return unit;
    }

    bool can_occupy(const Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy) override {
        queries.push_back(cell);
        if (occupancy != ground_layer || !inside(unit, cell))
            return false;
        for (int16_t z = cell[1]; z < cell[1] + unit.footprint[1]; ++z)
            for (int16_t x = cell[0]; x < cell[0] + unit.footprint[0]; ++x) {
                const auto at = index(x, z);
                if (features[at] || (occupants[at] != 0 && occupants[at] != unit.id))
                    return false;
            }
        return true;
    }

    void remove_occupancy(Unit& unit) override {
        for (int16_t z = unit.cell[1]; z < unit.cell[1] + unit.footprint[1]; ++z)
            for (int16_t x = unit.cell[0]; x < unit.cell[0] + unit.footprint[0]; ++x) {
                const auto at = index(x, z);
                overlaps += occupants[at] != unit.id;
                occupants[at] = 0;
            }
    }

    void insert_occupancy(Unit& unit) override {
        OA_CHECK(inside(unit, unit.cell));
        for (int16_t z = unit.cell[1]; z < unit.cell[1] + unit.footprint[1]; ++z)
            for (int16_t x = unit.cell[0]; x < unit.cell[0] + unit.footprint[0]; ++x) {
                const auto at = index(x, z);
                overlaps += features[at] || (occupants[at] != 0 && occupants[at] != unit.id);
                occupants[at] = unit.id;
            }
    }

    void update_spatial_membership(Unit&) override {}

    /// Classifies the same plot claims used by the authoritative footprint check.
    LocalFlowBlocker probe(const Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy) {
        if (occupancy != ground_layer || !inside(unit, cell))
            return {LocalFlowObstacle::stationary};
        LocalFlowBlocker result{};
        for (int16_t z = cell[1]; z < cell[1] + unit.footprint[1]; ++z)
            for (int16_t x = cell[0]; x < cell[0] + unit.footprint[0]; ++x) {
                const auto at = index(x, z);
                if (features[at])
                    return {LocalFlowObstacle::stationary};
                const auto claimant = occupants[at];
                if (claimant != 0 && claimant != unit.id)
                    result = {
                        LocalFlowObstacle::moving,
                        static_cast<uint16_t>(claimant),
                        units.at(size_t(claimant) - 1).heading,
                        5
                    };
            }
        return result;
    }

    /// Supplies fresh occupancy classification and an explicit experimental gate.
    LocalFlowProbe query(bool enabled, bool counterflow = false) {
        LocalFlowProbe result{};
        result.context = this;
        result.at = [](
                        void* context, const Unit& unit, std::array<int16_t, 2> cell, uint8_t layer
                    ) { return static_cast<Grid*>(context)->probe(unit, cell, layer); };
        result.mature_cell_escape = enabled;
        result.counterflow_cell_escape = counterflow;
        return result;
    }
};

/// Places traffic beside a corner where the ordinary backing point stays inside its origin.
Unit& corner(Grid& grid) {
    grid.add(6, 4, {1, 1});
    grid.add(4, 6, {1, 1});
    grid.add(6, 6, {1, 1});
    auto& unit = grid.add(4, 4, {2, 2});
    unit.position = {87 * fixed_one, 0, 87 * fixed_one};
    unit.heading = east;
    OA_CHECK(origin(unit.position) == unit.cell && grid.overlaps == 0);
    return unit;
}

/// Chooses a shallow diagonal goal with a recently moving follower.
LocalFlowDecision choose(
    Grid& grid, const Unit& unit, bool enabled, LocalFlowCounters& counters, uint32_t motion_age = 1
) {
    Movement motion{};
    motion.flags = ground_layer | collision_blocked;
    motion.last_motion_tick = decision_tick - motion_age;
    const std::array<Fixed, 3> goal{1087 * fixed_one, 0, 501 * fixed_one};
    const auto result =
        choose_local_flow(unit, motion, goal, decision_tick, grid, grid.query(enabled), counters);
    OA_CHECK(counters.probes <= maximum_queries);
    return result;
}

/// Replaces an in-origin choice only when a legal neighbouring origin exists.
void test_legal_cell_escape_and_repeat() {
    Grid previous_grid;
    auto& previous_unit = corner(previous_grid);
    LocalFlowCounters previous_counters{};
    const auto previous = choose(previous_grid, previous_unit, false, previous_counters);
    OA_CHECK(previous.mobile_blocked && previous.adjusted);
    OA_CHECK(origin(previous.target) == previous_unit.cell);

    Grid grid;
    auto& unit = corner(grid);
    LocalFlowCounters counters{};
    const auto decision = choose(grid, unit, true, counters);
    OA_CHECK(decision.mobile_blocked && decision.adjusted);
    OA_CHECK(origin(decision.target) == (std::array<int16_t, 2>{4, 3}));
    OA_CHECK(decision.target == (std::array<Fixed, 3>{87 * fixed_one, 0, 64 * fixed_one}));
    OA_CHECK(counters.steering_adjustments == previous_counters.steering_adjustments);
    OA_CHECK(grid.queries.size() == previous_grid.queries.size() + 3);
    OA_CHECK(grid.overlaps == 0);

    Grid repeated_grid;
    auto& repeated_unit = corner(repeated_grid);
    LocalFlowCounters repeated_counters{};
    const auto repeated = choose(repeated_grid, repeated_unit, true, repeated_counters);
    OA_CHECK(equal(decision, repeated));
    OA_CHECK(equal_outcomes(counters, repeated_counters));
    OA_CHECK(counters.probes == repeated_counters.probes && grid.queries == repeated_grid.queries);

    LocalFlowHead head{};
    head.active = true;
    head.count = 3;
    head.points[0] = {87, 87};
    head.points[1] = {87, 64};
    head.points[2] = {1087, 501};
    Movement motion{};
    motion.flags = ground_layer;
    motion.speed = fixed_one;
    motion.velocity = {0, 0, -fixed_one};
    unit.heading = 0;
    for (uint32_t tick = decision_tick + 1; tick < decision_tick + 40; ++tick) {
        if (local_flow_head_reached(unit, head))
            break;
        const auto result = integrate_swept_local_flow(unit, motion, tick, true, grid, counters);
        OA_CHECK(result.status == GroundSweepStatus::clear);
        OA_CHECK(grid.overlaps == 0);
    }
    OA_CHECK(local_flow_head_reached(unit, head));
    OA_CHECK(unit.cell == (std::array<int16_t, 2>{4, 3}));
    OA_CHECK(origin(unit.position) == unit.cell && grid.overlaps == 0);
}

/// Allows a mature legal escape when the ordinary traffic priority retains the route.
void test_unadjusted_mobile_result_can_escape() {
    Grid off;
    auto& unit = corner(off);
    Grid on = off;
    LocalFlowCounters off_counters{}, on_counters{};
    const auto previous = choose(off, unit, false, off_counters, 100);
    const auto current = choose(on, on.units[3], true, on_counters, 100);
    OA_CHECK(previous.mobile_blocked && !previous.adjusted);
    OA_CHECK(off_counters.yielded_ticks == 0 && off_counters.steering_adjustments == 0);
    OA_CHECK(current.mobile_blocked && current.adjusted);
    OA_CHECK(current.target == (std::array<Fixed, 3>{87 * fixed_one, 0, 64 * fixed_one}));
    OA_CHECK(on_counters.yielded_ticks == 1 && on_counters.steering_adjustments == 1);
    OA_CHECK(on.queries.size() == off.queries.size() + 3);
    OA_CHECK(off.overlaps == 0 && on.overlaps == 0);
}

/// Keeps an existing real passing choice and all work identical when the gate is enabled.
void test_existing_crossing_is_unchanged() {
    Grid off;
    off.add(6, 4, {1, 1});
    auto& a = off.add(4, 4, {2, 2});
    a.heading = east;
    Grid on = off;
    Movement motion{};
    motion.flags = ground_layer;
    motion.last_motion_tick = decision_tick - 1;
    const std::array<Fixed, 3> goal{320 * fixed_one, 0, 80 * fixed_one};
    LocalFlowCounters off_counters{}, on_counters{};
    const auto previous =
        choose_local_flow(a, motion, goal, decision_tick, off, off.query(false), off_counters);
    const auto current = choose_local_flow(
        on.units[1], motion, goal, decision_tick, on, on.query(true), on_counters
    );
    OA_CHECK(previous.mobile_blocked && previous.adjusted && origin(previous.target) != a.cell);
    OA_CHECK(equal(previous, current) && equal_outcomes(off_counters, on_counters));
    OA_CHECK(off_counters.probes == on_counters.probes && off.queries == on.queries);
}

/// Retains the original decision when every adjacent full footprint is occupied.
void test_surrounded_keeps_original() {
    Grid off;
    auto& unit = corner(off);
    off.features[Grid::index(3, 4)] = true;
    off.features[Grid::index(4, 3)] = true;
    Grid on = off;
    LocalFlowCounters off_counters{}, on_counters{};
    const auto previous = choose(off, unit, false, off_counters);
    const auto current = choose(on, on.units[3], true, on_counters);
    OA_CHECK(previous.mobile_blocked && origin(previous.target) == unit.cell);
    OA_CHECK(equal(previous, current) && equal_outcomes(off_counters, on_counters));
    OA_CHECK(on.queries.size() == off.queries.size() + 4);
    OA_CHECK(off.overlaps == 0 && on.overlaps == 0);
}

/// Checks every plot of a neighbouring footprint rather than its empty leading corner alone.
void test_full_footprint_feature_veto() {
    Grid grid;
    auto& unit = corner(grid);
    grid.features[Grid::index(5, 3)] = true;
    LocalFlowCounters counters{};
    const auto decision = choose(grid, unit, true, counters);
    OA_CHECK(decision.mobile_blocked && decision.adjusted);
    OA_CHECK(decision.target == (std::array<Fixed, 3>{64 * fixed_one, 0, 87 * fixed_one}));
    OA_CHECK(origin(decision.target) == (std::array<int16_t, 2>{3, 4}));
    OA_CHECK(grid.overlaps == 0);
}

/// Places a two-cell unit at origin {4, 4}, heading east toward its goal, whose
/// neighbour ahead is held by moving traffic with the given heading; every
/// other neighbour is clear.
Unit& east_blocked(Grid& grid, uint16_t traffic_heading) {
    grid.add(6, 5, {1, 1}).heading = traffic_heading;
    auto& unit = grid.add(4, 4, {2, 2});
    unit.heading = east;
    OA_CHECK(origin(unit.position) == unit.cell && grid.overlaps == 0);
    return unit;
}

/// Chooses toward a goal due east for a unit that last moved long ago, so
/// ordinary passing keeps its route and only the mature escape may move it.
LocalFlowDecision choose_east(
    Grid& grid, const Unit& unit, bool enabled, bool counterflow, LocalFlowCounters& counters
) {
    Movement motion{};
    motion.flags = ground_layer | collision_blocked;
    motion.last_motion_tick = decision_tick - 100;
    const std::array<Fixed, 3> goal{1080 * fixed_one, 0, 80 * fixed_one};
    const auto result = choose_local_flow(
        unit, motion, goal, decision_tick, grid, grid.query(enabled, counterflow), counters
    );
    OA_CHECK(counters.probes <= maximum_queries);
    return result;
}

/// Escapes oncoming traffic exactly as the cardinal escape does.
void test_counterflow_escape_from_oncoming_traffic() {
    Grid cardinal;
    auto& unit = corner(cardinal);
    Grid counterflow = cardinal;
    LocalFlowCounters cardinal_counters{}, counterflow_counters{};
    const auto stepped = choose(cardinal, unit, true, cardinal_counters);
    Movement motion{};
    motion.flags = ground_layer | collision_blocked;
    motion.last_motion_tick = decision_tick - 1;
    const std::array<Fixed, 3> goal{1087 * fixed_one, 0, 501 * fixed_one};
    const auto current = choose_local_flow(
        counterflow.units[3],
        motion,
        goal,
        decision_tick,
        counterflow,
        counterflow.query(true, true),
        counterflow_counters
    );
    // The unit heads east and the traffic west, against it.
    OA_CHECK(origin(current.target) == (std::array<int16_t, 2>{4, 3}));
    OA_CHECK(equal(stepped, current) && equal_outcomes(cardinal_counters, counterflow_counters));
    OA_CHECK(cardinal_counters.probes == counterflow_counters.probes);
    OA_CHECK(cardinal.queries == counterflow.queries && counterflow.overlaps == 0);
}

/// Keeps its place behind traffic going its own way, and escapes traffic that
/// crosses its way a quarter turn or more from its heading.
void test_counterflow_escape_waits_behind_same_way() {
    // Within a quarter turn of the unit's heading the traffic goes its way.
    for (const uint16_t heading : {east, uint16_t(east - 8192), uint16_t(east + 16383)}) {
        Grid off;
        auto& unit = east_blocked(off, heading);
        Grid cardinal = off;
        Grid counterflow = off;
        LocalFlowCounters off_counters{}, cardinal_counters{}, counterflow_counters{};
        const auto previous = choose_east(off, unit, false, false, off_counters);
        const auto aside = choose_east(cardinal, cardinal.units[1], true, false, cardinal_counters);
        const auto current =
            choose_east(counterflow, counterflow.units[1], true, true, counterflow_counters);
        OA_CHECK(previous.mobile_blocked && !previous.adjusted);
        OA_CHECK(aside.adjusted && origin(aside.target) == (std::array<int16_t, 2>{4, 5}));
        OA_CHECK(equal(previous, current) && equal_outcomes(off_counters, counterflow_counters));
        OA_CHECK(off_counters.probes == counterflow_counters.probes);
        OA_CHECK(off.queries == counterflow.queries && counterflow.overlaps == 0);
    }
    // A quarter turn or more away, across the unit's way or against it, it escapes.
    for (const uint16_t heading : {uint16_t(east + 16384), uint16_t(east - 16384), west}) {
        Grid cardinal;
        auto& unit = east_blocked(cardinal, heading);
        Grid counterflow = cardinal;
        LocalFlowCounters cardinal_counters{}, counterflow_counters{};
        const auto aside = choose_east(cardinal, unit, true, false, cardinal_counters);
        const auto current =
            choose_east(counterflow, counterflow.units[1], true, true, counterflow_counters);
        OA_CHECK(current.adjusted && !current.reverse);
        OA_CHECK(current.target == (std::array<Fixed, 3>{80 * fixed_one, 0, 96 * fixed_one}));
        OA_CHECK(equal(aside, current) && equal_outcomes(cardinal_counters, counterflow_counters));
        OA_CHECK(cardinal.queries == counterflow.queries && counterflow.overlaps == 0);
    }
}

struct RangeHost final : Host {
    std::vector<std::array<int16_t, 2>> queries{};

    bool can_occupy(const Unit&, std::array<int16_t, 2> cell, uint8_t) override {
        queries.push_back(cell);
        OA_CHECK(cell[0] >= 2000 && cell[0] <= 2047);
        return false;
    }

    void remove_occupancy(Unit&) override { OA_CHECK(false); }

    void insert_occupancy(Unit&) override { OA_CHECK(false); }

    void update_spatial_membership(Unit&) override { OA_CHECK(false); }
};

/// Rejects a neighbouring centre beyond signed fixed-point range before any footprint query.
void test_fixed_point_limit_does_not_wrap() {
    Unit unit{};
    unit.id = 2;
    unit.position = {std::numeric_limits<Fixed>::max(), 0, 80 * fixed_one};
    unit.cell = {2047, 4};
    unit.footprint = {2, 2};
    unit.flags = ground_layer;
    unit.type.maximum_speed = fixed_one;
    Movement motion{};
    motion.flags = ground_layer;
    motion.last_motion_tick = decision_tick;
    LocalFlowProbe probe{};
    probe.at = [](void*, const Unit&, std::array<int16_t, 2>, uint8_t) {
        return LocalFlowBlocker{LocalFlowObstacle::moving, 1, west, 5};
    };
    RangeHost off, on;
    LocalFlowCounters off_counters{}, on_counters{};
    const std::array<Fixed, 3> goal{unit.position[0] - 100 * fixed_one, 0, 100 * fixed_one};
    const auto previous =
        choose_local_flow(unit, motion, goal, decision_tick, off, probe, off_counters);
    probe.mature_cell_escape = true;
    const auto current =
        choose_local_flow(unit, motion, goal, decision_tick, on, probe, on_counters);
    OA_CHECK(previous.mobile_blocked && !previous.adjusted);
    OA_CHECK(equal(previous, current) && equal_outcomes(off_counters, on_counters));
    OA_CHECK(on.queries.size() == off.queries.size() + 3);
    OA_CHECK(on_counters.probes <= maximum_queries);
}
} // namespace

int main() {
    test_legal_cell_escape_and_repeat();
    test_unadjusted_mobile_result_can_escape();
    test_existing_crossing_is_unchanged();
    test_surrounded_keeps_original();
    test_full_footprint_feature_veto();
    test_fixed_point_limit_does_not_wrap();
    test_counterflow_escape_from_oncoming_traffic();
    test_counterflow_escape_waits_behind_same_way();
    return oa::test::check_exit_status();
}
