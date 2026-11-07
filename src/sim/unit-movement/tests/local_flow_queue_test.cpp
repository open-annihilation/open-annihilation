// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/base/game_math.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

namespace movement = oa::sim::unit_movement;
namespace ground = oa::sim::ground_orders;

namespace {
constexpr movement::Fixed fixed_one = 65536;
constexpr int16_t cell_width = 16;
constexpr int16_t half_cell_width = cell_width / 2;
constexpr int16_t map_width = 96;
constexpr int16_t map_height = 64;
constexpr int16_t lane_row = 32;
constexpr int16_t start_column = 20;
constexpr int16_t blocked_column = 22;
constexpr int16_t footprint = 2;
constexpr int16_t destination_x = 1200;
constexpr uint8_t ground_occupancy = 1;
constexpr uint16_t mover_id = 2;
constexpr uint16_t blocker_id = 1;
constexpr size_t maximum_movers = 4;
constexpr uint16_t east = 49152;
constexpr uint16_t west = 16384;
constexpr movement::Fixed maximum_speed = 117964;
constexpr uint16_t maximum_turn = 1120;
constexpr movement::Fixed acceleration = 6553;
constexpr uint32_t lane_opens_at = 90;
constexpr uint32_t tick_limit = 1800;
constexpr movement::Fixed arrival_radius = cell_width * fixed_one;

/// Returns the centre coordinate of a two-cell footprint at its origin.
constexpr int16_t centre(int16_t cell) {
    return static_cast<int16_t>(cell * cell_width + footprint * half_cell_width);
}

/// Holds opposing traffic at the lane's mouth while its passing area opens.
struct QueueArena : movement::Host {
    std::array<uint16_t, size_t(map_width) * map_height> occupants{};
    std::array<uint32_t, maximum_movers + mover_id> progress{};
    std::array<movement::Unit, maximum_movers> movers{};
    std::array<bool, maximum_movers> travelling{};
    size_t mover_count{};
    bool idle_units_are_mobile{true};
    int16_t back_column{};
    movement::Unit blocker{};
    uint32_t tick{};
    uint32_t overlaps{};

    explicit QueueArena(size_t count = 1) : mover_count(count) {
        OA_CHECK(count >= 1 && count <= maximum_movers);
        back_column = static_cast<int16_t>(start_column - (count - 1) * footprint);
        for (size_t index = 0; index < mover_count; ++index) {
            auto& mover = movers[index];
            mover.id = static_cast<int16_t>(mover_id + index);
            mover.cell = {static_cast<int16_t>(start_column - index * footprint), lane_row};
            mover.footprint = {footprint, footprint};
            mover.position = {centre(mover.cell[0]) * fixed_one, 0, centre(lane_row) * fixed_one};
            mover.flags = ground_occupancy;
            mover.heading = east;
            mover.type.maximum_speed = maximum_speed;
            mover.type.maximum_turn = maximum_turn;
            travelling[index] = true;
            insert_occupancy(mover);
        }
        blocker = movers[0];
        blocker.id = blocker_id;
        blocker.cell[0] = blocked_column;
        blocker.position[0] = centre(blocked_column) * fixed_one;
        blocker.heading = west;
        insert_occupancy(blocker);
    }

    /// Locates a cell in the complete footprint occupancy table.
    static size_t plot(int16_t x, int16_t z) { return size_t(z) * map_width + size_t(x); }

    /// Checks map bounds and the temporary lane boundaries.
    bool terrain_clear(const movement::Unit& unit, std::array<int16_t, 2> cell) const {
        if (cell[0] < 0 || cell[1] < 0 || cell[0] + unit.footprint[0] > map_width ||
            cell[1] + unit.footprint[1] > map_height)
            return false;
        return tick >= lane_opens_at || (cell[0] >= back_column && cell[1] == lane_row);
    }

    bool can_occupy(
        const movement::Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy
    ) override {
        if (occupancy != ground_occupancy || !terrain_clear(unit, cell))
            return false;
        for (int16_t z = 0; z < unit.footprint[1]; ++z)
            for (int16_t x = 0; x < unit.footprint[0]; ++x) {
                const auto occupant = occupants[plot(cell[0] + x, cell[1] + z)];
                if (occupant != 0 && occupant != uint16_t(unit.id))
                    return false;
            }
        return true;
    }

    void remove_occupancy(movement::Unit& unit) override {
        for (int16_t z = 0; z < unit.footprint[1]; ++z)
            for (int16_t x = 0; x < unit.footprint[0]; ++x) {
                auto& occupant = occupants[plot(unit.cell[0] + x, unit.cell[1] + z)];
                if (occupant != uint16_t(unit.id))
                    ++overlaps;
                occupant = 0;
            }
    }

    void insert_occupancy(movement::Unit& unit) override {
        for (int16_t z = 0; z < unit.footprint[1]; ++z)
            for (int16_t x = 0; x < unit.footprint[0]; ++x) {
                auto& occupant = occupants[plot(unit.cell[0] + x, unit.cell[1] + z)];
                if (occupant != 0 && occupant != uint16_t(unit.id))
                    ++overlaps;
                occupant = static_cast<uint16_t>(unit.id);
            }
    }

    void update_spatial_membership(movement::Unit&) override {}

    /// Reads complete footprints using progress clocks captured at tick start.
    movement::LocalFlowProbe probe() {
        return {
            this,
            [](void* context,
               const movement::Unit& unit,
               std::array<int16_t, 2> cell,
               uint8_t occupancy) {
                auto& arena = *static_cast<QueueArena*>(context);
                using movement::LocalFlowBlocker;
                using movement::LocalFlowObstacle;
                if (occupancy != ground_occupancy || !arena.terrain_clear(unit, cell))
                    return LocalFlowBlocker{LocalFlowObstacle::stationary};
                for (int16_t z = 0; z < unit.footprint[1]; ++z)
                    for (int16_t x = 0; x < unit.footprint[0]; ++x) {
                        const auto id = arena.occupants[plot(cell[0] + x, cell[1] + z)];
                        if (id != 0 && id != uint16_t(unit.id)) {
                            if (id != blocker_id && !arena.travelling[id - mover_id] &&
                                !arena.idle_units_are_mobile)
                                return LocalFlowBlocker{LocalFlowObstacle::stationary};
                            const auto heading = id == blocker_id
                                                     ? arena.blocker.heading
                                                     : arena.movers[id - mover_id].heading;
                            return LocalFlowBlocker{
                                LocalFlowObstacle::moving,
                                id,
                                heading,
                                arena.tick - arena.progress[id]
                            };
                        }
                    }
                return LocalFlowBlocker{};
            },
            progress
        };
    }
};

struct QueueResult {
    std::array<movement::Fixed, 3> final_position{};
    uint64_t blocked_ticks{};
    uint64_t adjustments{};
    uint32_t arrival_tick{};
    uint32_t recoveries{};
    uint32_t first_attempt_after_opening{};
    uint32_t overlaps{};
    bool mature_when_lane_opens{};
    bool replan_deferred_after_maturity{};
    bool operator==(const QueueResult&) const = default;
};

/// Drives a stock-sized unit around traffic while its full route stays pending.
QueueResult run_queue(bool renew_local_attempts, bool filter_previous = true) {
    QueueArena arena;
    movement::Movement motion{};
    motion.flags = ground_occupancy;
    motion.speed = maximum_speed;
    movement::LocalFlowHead head{};
    movement::LocalFlowCounters counters{};
    const std::array<movement::Fixed, 3> destination{
        destination_x * fixed_one, 0, centre(lane_row) * fixed_one
    };
    ground::Navigation route{};
    route.count = 3;
    route.flags = ground::route_present_flag | ground::search_pending_flag;
    route.points[0] = {centre(start_column), centre(lane_row)};
    route.points[1] = route.points[2] = {destination_x, centre(lane_row)};
    QueueResult result{};
    for (arena.tick = 1; arena.tick <= tick_limit; ++arena.tick) {
        auto& unit = arena.movers[0];
        arena.progress[mover_id] = motion.last_motion_tick;
        if (arena.tick == lane_opens_at)
            result.mature_when_lane_opens = head.blocked_ticks >= movement::local_flow_retry_ticks;
        const bool recovery =
            renew_local_attempts &&
            movement::local_flow_recovery_due(head, arena.tick, uint16_t(unit.id));
        if (recovery) {
            ++result.recoveries;
            if (arena.tick >= lane_opens_at && result.first_attempt_after_opening == 0)
                result.first_attempt_after_opening = arena.tick;
        }
        if (head.blocked_ticks >= movement::local_flow_retry_ticks &&
            movement::local_flow_defer_replan(head, motion))
            result.replan_deferred_after_maturity = true;
        const bool just_reached = head.active && movement::local_flow_head_reached(unit, head);
        const bool previous_tick = movement::local_flow_completed_last_tick(head, arena.tick);
        if (just_reached) {
            head.active = false;
            head.count = movement::local_flow_completed_count;
            head.tick = arena.tick;
        } else if (
            head.active &&
            (renew_local_attempts ? recovery
                                  : head.blocked_ticks >= movement::local_flow_retry_ticks)
        ) {
            head.active = false;
            head.count = 0;
        }
        if (!head.active && (head.blocked_ticks < movement::local_flow_retry_ticks || recovery)) {
            auto probe = arena.probe();
            probe.previous_head =
                renew_local_attempts && filter_previous && (just_reached || previous_tick)
                    ? &head
                    : nullptr;
            probe.allow_reverse = recovery;
            const auto decision = movement::choose_local_flow(
                unit, motion, destination, arena.tick, arena, probe, counters
            );
            head.mobile_blocked = decision.mobile_blocked;
            if (decision.adjusted) {
                head.points = {
                    ground::RoutePoint{
                        static_cast<int16_t>(unit.position[0] >> 16),
                        static_cast<int16_t>(unit.position[2] >> 16)
                    },
                    ground::RoutePoint{
                        static_cast<int16_t>(decision.target[0] >> 16),
                        static_cast<int16_t>(decision.target[2] >> 16)
                    },
                    route.points[2]
                };
                head.count = 3;
                head.active = true;
            }
        }
        if (head.active)
            head.tick = arena.tick;
        auto effective = route;
        if (head.active)
            std::copy(head.points.begin(), head.points.end(), effective.points.begin());
        ground::steer_ground(
            unit,
            motion,
            effective,
            acceleration,
            acceleration,
            0,
            ground::FastUnitSteering::as_3_1c
        );
        movement::integrate_local_flow(unit, motion, arena.tick, true, arena, counters);
        movement::record_local_flow_block(head, (motion.flags & movement::collision_blocked) != 0);
        if (oa::base::game_math::distance(
                unit.position[0] - destination[0], unit.position[2] - destination[2]
            ) < uint32_t(arrival_radius)) {
            result.arrival_tick = arena.tick;
            break;
        }
    }
    result.final_position = arena.movers[0].position;
    result.blocked_ticks = counters.blocked_ticks;
    result.adjustments = counters.steering_adjustments;
    result.overlaps = arena.overlaps;
    return result;
}

/// Checks that a mature queue can use a newly opened passing place.
void test_mature_queue_recovers_without_renewing_replan_grace() {
    const auto recovered = run_queue(true);
    const auto old_gate = run_queue(false);
    OA_CHECK(recovered == run_queue(true));
    OA_CHECK(recovered.mature_when_lane_opens && old_gate.mature_when_lane_opens);
    OA_CHECK(!recovered.replan_deferred_after_maturity);
    OA_CHECK(!old_gate.replan_deferred_after_maturity);
    OA_CHECK(recovered.overlaps == 0 && old_gate.overlaps == 0);
    OA_CHECK(recovered.arrival_tick != 0 && recovered.arrival_tick < tick_limit / 2);
    OA_CHECK(old_gate.arrival_tick == 0);
    OA_CHECK(recovered.adjustments != 0 && old_gate.adjustments == 0);
    OA_CHECK(recovered.blocked_ticks < old_gate.blocked_ticks / 2);
    OA_CHECK(recovered.first_attempt_after_opening >= lane_opens_at);
    OA_CHECK(
        recovered.first_attempt_after_opening < lane_opens_at + movement::local_flow_retry_ticks
    );
    std::printf(
        "queue recovery: arrival %u, old gate %u; blocked %llu / %llu; overlaps %u / %u\n",
        recovered.arrival_tick,
        old_gate.arrival_tick,
        static_cast<unsigned long long>(recovered.blocked_ticks),
        static_cast<unsigned long long>(old_gate.blocked_ticks),
        recovered.overlaps,
        old_gate.overlaps
    );
}

struct TrainResult {
    uint64_t blocked_ticks{};
    uint64_t orbit_windows{};
    uint64_t turn{};
    uint32_t arrived{};
    uint32_t last_arrival{};
    uint32_t overlaps{};
    uint64_t digest{14695981039346656037ull};
    bool operator==(const TrainResult&) const = default;
};

enum class ArrivalLayout { spread, column };

/// Drives every mover in a touching two-cell queue through the new passing area.
TrainResult run_train(
    bool renew_local_attempts,
    bool idle_mobile = true,
    ArrivalLayout arrivals = ArrivalLayout::spread,
    bool filter_previous = true
) {
    constexpr size_t history_size = 61;
    constexpr int16_t goal_spacing = 48;
    constexpr int16_t first_goal_offset = (maximum_movers - 1) * goal_spacing / 2;
    constexpr uint64_t orbit_travel = 24 * fixed_one;
    constexpr uint32_t orbit_radius = 5 * fixed_one;
    QueueArena arena(maximum_movers);
    arena.idle_units_are_mobile = idle_mobile;
    std::array<movement::Movement, maximum_movers> motions{};
    std::array<movement::LocalFlowHead, maximum_movers> heads{};
    std::array<ground::Navigation, maximum_movers> routes{};
    std::array<std::array<movement::Fixed, 3>, maximum_movers> destinations{};
    std::array<std::array<std::array<movement::Fixed, 3>, history_size>, maximum_movers>
        positions{};
    std::array<std::array<uint64_t, history_size>, maximum_movers> distances{};
    std::array<uint64_t, maximum_movers> total_distance{};
    movement::LocalFlowCounters counters{};
    for (size_t index = 0; index < maximum_movers; ++index) {
        const auto& unit = arena.movers[index];
        motions[index].flags = ground_occupancy;
        motions[index].speed = maximum_speed;
        const auto goal_x = arrivals == ArrivalLayout::spread
                                ? destination_x
                                : destination_x - int(index) * goal_spacing;
        const auto goal_z =
            centre(lane_row) +
            (arrivals == ArrivalLayout::spread ? int(index) * goal_spacing - first_goal_offset : 0);
        destinations[index] = {goal_x * fixed_one, 0, goal_z * fixed_one};
        auto& route = routes[index];
        route.count = 3;
        route.flags = ground::route_present_flag | ground::search_pending_flag;
        route.points[0] = {centre(unit.cell[0]), centre(lane_row)};
        route.points[1] = route.points[2] = {
            static_cast<int16_t>(destinations[index][0] >> 16),
            static_cast<int16_t>(destinations[index][2] >> 16)
        };
        positions[index].fill(unit.position);
    }
    TrainResult result{};
    for (arena.tick = 1; arena.tick <= tick_limit; ++arena.tick) {
        for (size_t index = 0; index < maximum_movers; ++index)
            arena.progress[mover_id + index] = motions[index].last_motion_tick;
        for (size_t index = 0; index < maximum_movers; ++index) {
            if (!arena.travelling[index])
                continue;
            auto& unit = arena.movers[index];
            auto& motion = motions[index];
            auto& head = heads[index];
            const auto recovery =
                renew_local_attempts &&
                movement::local_flow_recovery_due(head, arena.tick, uint16_t(unit.id));
            const bool just_reached = head.active && movement::local_flow_head_reached(unit, head);
            const bool previous_tick = movement::local_flow_completed_last_tick(head, arena.tick);
            if (just_reached) {
                head.active = false;
                head.count = movement::local_flow_completed_count;
                head.tick = arena.tick;
            } else if (
                head.active &&
                (renew_local_attempts ? recovery
                                      : head.blocked_ticks >= movement::local_flow_retry_ticks)
            ) {
                head.active = false;
                head.count = 0;
            }
            if (!head.active &&
                (head.blocked_ticks < movement::local_flow_retry_ticks || recovery)) {
                auto probe = arena.probe();
                probe.previous_head =
                    renew_local_attempts && filter_previous && (just_reached || previous_tick)
                        ? &head
                        : nullptr;
                probe.allow_reverse = recovery;
                const auto decision = movement::choose_local_flow(
                    unit, motion, destinations[index], arena.tick, arena, probe, counters
                );
                head.mobile_blocked = decision.mobile_blocked;
                if (decision.adjusted) {
                    head.points = {
                        ground::RoutePoint{
                            static_cast<int16_t>(unit.position[0] >> 16),
                            static_cast<int16_t>(unit.position[2] >> 16)
                        },
                        ground::RoutePoint{
                            static_cast<int16_t>(decision.target[0] >> 16),
                            static_cast<int16_t>(decision.target[2] >> 16)
                        },
                        routes[index].points[2]
                    };
                    head.count = 3;
                    head.active = true;
                }
            }
            if (head.active)
                head.tick = arena.tick;
            auto effective = routes[index];
            if (head.active)
                std::copy(head.points.begin(), head.points.end(), effective.points.begin());
            const auto before = unit.position;
            const auto heading = unit.heading;
            ground::steer_ground(
                unit,
                motion,
                effective,
                acceleration,
                acceleration,
                0,
                ground::FastUnitSteering::as_3_1c
            );
            result.turn += std::abs(int16_t(unit.heading - heading));
            movement::integrate_local_flow(unit, motion, arena.tick, true, arena, counters);
            movement::record_local_flow_block(
                head, (motion.flags & movement::collision_blocked) != 0
            );
            total_distance[index] += oa::base::game_math::distance(
                unit.position[0] - before[0], unit.position[2] - before[2]
            );
            const auto slot = size_t(arena.tick % history_size);
            if (arena.tick >= history_size - 1) {
                const auto start = size_t((arena.tick - (history_size - 1)) % history_size);
                if (total_distance[index] - distances[index][start] > orbit_travel &&
                    oa::base::game_math::distance(
                        unit.position[0] - positions[index][start][0],
                        unit.position[2] - positions[index][start][2]
                    ) < orbit_radius)
                    ++result.orbit_windows;
            }
            positions[index][slot] = unit.position;
            distances[index][slot] = total_distance[index];
            if (oa::base::game_math::distance(
                    unit.position[0] - destinations[index][0],
                    unit.position[2] - destinations[index][2]
                ) < uint32_t(arrival_radius)) {
                arena.travelling[index] = false;
                motion.speed = 0;
                motion.velocity = {};
                ++result.arrived;
                result.last_arrival = arena.tick;
            }
        }
        if (result.arrived == maximum_movers)
            break;
    }
    result.blocked_ticks = counters.blocked_ticks;
    result.overlaps = arena.overlaps;
    for (size_t index = 0; index < maximum_movers; ++index)
        for (const auto coordinate : arena.movers[index].position) {
            result.digest ^= static_cast<uint32_t>(coordinate);
            result.digest *= 1099511628211ull;
        }
    return result;
}

/// Checks sustained progress for every mover at the back of a mature queue.
void test_whole_mature_train_recovers() {
    const auto recovered = run_train(true);
    const auto old_gate = run_train(false);
    OA_CHECK(recovered == run_train(true));
    OA_CHECK(recovered.arrived == maximum_movers);
    OA_CHECK(recovered.last_arrival < tick_limit / 2);
    OA_CHECK(recovered.overlaps == 0 && old_gate.overlaps == 0);
    OA_CHECK(recovered.arrived > old_gate.arrived);
    OA_CHECK(recovered.blocked_ticks < old_gate.blocked_ticks / 2);
    OA_CHECK(recovered.orbit_windows < movement::local_flow_retry_ticks * 2);
    std::printf(
        "train recovery: arrivals %u / %u, last %u / %u, blocked %llu / %llu, orbits %llu / %llu, "
        "turn %llu / %llu\n",
        recovered.arrived,
        old_gate.arrived,
        recovered.last_arrival,
        old_gate.last_arrival,
        static_cast<unsigned long long>(recovered.blocked_ticks),
        static_cast<unsigned long long>(old_gate.blocked_ticks),
        static_cast<unsigned long long>(recovered.orbit_windows),
        static_cast<unsigned long long>(old_gate.orbit_windows),
        static_cast<unsigned long long>(recovered.turn),
        static_cast<unsigned long long>(old_gate.turn)
    );
}

/// Checks that completed movable units remain fixed while followers pass them.
void test_completed_mobile_units_do_not_wall_off_followers() {
    const auto bypass = run_train(true, true, ArrivalLayout::column, false);
    const auto parked_walls = run_train(true, false, ArrivalLayout::column, false);
    OA_CHECK(bypass == run_train(true, true, ArrivalLayout::column, false));
    OA_CHECK(bypass.arrived == maximum_movers);
    OA_CHECK(bypass.arrived > parked_walls.arrived);
    OA_CHECK(bypass.overlaps == 0 && parked_walls.overlaps == 0);
    OA_CHECK(bypass.blocked_ticks < parked_walls.blocked_ticks / 2);
    OA_CHECK(bypass.orbit_windows < parked_walls.orbit_windows / 4);
    std::printf(
        "idle bypass: arrivals %u / %u, last %u / %u, blocked %llu / %llu, orbits %llu / %llu\n",
        bypass.arrived,
        parked_walls.arrived,
        bypass.last_arrival,
        parked_walls.last_arrival,
        static_cast<unsigned long long>(bypass.blocked_ticks),
        static_cast<unsigned long long>(parked_walls.blocked_ticks),
        static_cast<unsigned long long>(bypass.orbit_windows),
        static_cast<unsigned long long>(parked_walls.orbit_windows)
    );
}

/// Checks that a large mature cohort does not retry in one burst.
void test_mature_recovery_is_staggered_and_keeps_full_replanning_enabled() {
    constexpr uint16_t population = 20000;
    movement::LocalFlowHead head{};
    head.mobile_blocked = true;
    head.blocked_ticks = movement::local_flow_retry_ticks + 1;
    movement::Movement motion{};
    motion.flags = ground_occupancy | movement::collision_blocked;
    std::array<uint32_t, movement::local_flow_retry_ticks> due{};
    for (uint32_t tick = 0; tick < movement::local_flow_retry_ticks; ++tick) {
        for (uint16_t id = 1; id <= population; ++id)
            if (movement::local_flow_recovery_due(head, tick, id))
                ++due[tick];
        OA_CHECK(!movement::local_flow_defer_replan(head, motion));
        OA_CHECK(due[tick] >= population / movement::local_flow_retry_ticks);
        OA_CHECK(due[tick] <= population / movement::local_flow_retry_ticks + 1);
    }
    uint32_t total{};
    for (const auto count : due)
        total += count;
    OA_CHECK(total == population);
    head.blocked_ticks = movement::local_flow_retry_ticks - 1;
    for (uint32_t tick = 0; tick < movement::local_flow_retry_ticks; ++tick)
        OA_CHECK(!movement::local_flow_recovery_due(head, tick, mover_id));
}

/// Checks that a completed-head history preserves ordinary queue trips.
void test_immediate_history_preserves_queue_arrivals() {
    constexpr uint64_t parked_column_additional_vetoes = 1;
    for (const auto layout : {ArrivalLayout::spread, ArrivalLayout::column}) {
        const auto baseline = run_train(true, true, layout, false);
        const auto history = run_train(true, true, layout, true);
        OA_CHECK(history == run_train(true, true, layout, true));
        OA_CHECK(history.arrived == maximum_movers);
        OA_CHECK(history.overlaps == 0);
        OA_CHECK(history.last_arrival <= baseline.last_arrival);
        const auto additional_vetoes =
            layout == ArrivalLayout::column ? parked_column_additional_vetoes : uint64_t{0};
        OA_CHECK(history.blocked_ticks <= baseline.blocked_ticks + additional_vetoes);
        OA_CHECK(history.orbit_windows <= baseline.orbit_windows);
    }
}

/// Leaves only the north passing lane open beside a held two-cell blocker.
struct NorthPassingArena : QueueArena {
    static constexpr int16_t south_wall_row = lane_row + footprint;
    static constexpr int16_t south_wall_end = blocked_column + 8;

    bool can_occupy(
        const movement::Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy
    ) override {
        if (cell[0] < south_wall_end && cell[1] + unit.footprint[1] > south_wall_row)
            return false;
        return QueueArena::can_occupy(unit, cell, occupancy);
    }
};

struct ReversalResult {
    uint64_t distance{};
    uint64_t closed_orbit_windows{};
    uint32_t arrival{};
    uint32_t legs{};
    uint32_t opposite_legs{};
    uint32_t peak_queries{};
    uint32_t overlaps{};
    bool operator==(const ReversalResult&) const = default;
};

/// Drives a two-cell mover through a passing lane without changing its route.
ReversalResult run_reversal_history(bool history) {
    constexpr size_t history_size = 61;
    constexpr uint64_t orbit_travel = 24 * fixed_one;
    constexpr uint32_t orbit_radius = 5 * fixed_one;
    NorthPassingArena arena;
    auto& unit = arena.movers[0];
    movement::Movement motion{};
    motion.flags = ground_occupancy;
    motion.speed = maximum_speed;
    movement::LocalFlowHead head{};
    movement::LocalFlowCounters counters{};
    const std::array<movement::Fixed, 3> destination{
        destination_x * fixed_one, 0, centre(lane_row) * fixed_one
    };
    ground::Navigation route{};
    route.count = 3;
    route.flags = ground::route_present_flag | ground::search_pending_flag;
    route.points[0] = {centre(start_column), centre(lane_row)};
    route.points[1] = route.points[2] = {destination_x, centre(lane_row)};
    std::array<std::array<movement::Fixed, 3>, history_size> positions{};
    positions.fill(unit.position);
    std::array<uint64_t, history_size> distances{};
    std::array<int32_t, 2> completed_leg{};
    bool completed{};
    ReversalResult result{};
    for (arena.tick = 1; arena.tick <= tick_limit; ++arena.tick) {
        arena.progress[mover_id] = motion.last_motion_tick;
        const auto recovery =
            movement::local_flow_recovery_due(head, arena.tick, uint16_t(unit.id));
        const bool just_reached = head.active && movement::local_flow_head_reached(unit, head);
        const bool previous_tick = movement::local_flow_completed_last_tick(head, arena.tick);
        if (just_reached) {
            completed_leg = {
                int32_t(head.points[1][0]) - head.points[0][0],
                int32_t(head.points[1][1]) - head.points[0][1]
            };
            completed = true;
        }
        if (just_reached) {
            head.active = false;
            head.count = movement::local_flow_completed_count;
            head.tick = arena.tick;
        } else if (head.active && recovery) {
            head.active = false;
            head.count = 0;
        }
        if (!head.active && (head.blocked_ticks < movement::local_flow_retry_ticks || recovery)) {
            auto probe = arena.probe();
            probe.previous_head = history && (just_reached || previous_tick) ? &head : nullptr;
            probe.allow_reverse = recovery;
            const auto queries = counters.probes;
            const auto decision = movement::choose_local_flow(
                unit, motion, destination, arena.tick, arena, probe, counters
            );
            result.peak_queries =
                std::max(result.peak_queries, static_cast<uint32_t>(counters.probes - queries));
            head.mobile_blocked = decision.mobile_blocked;
            if (decision.adjusted) {
                const ground::RoutePoint origin{
                    static_cast<int16_t>(unit.position[0] >> 16),
                    static_cast<int16_t>(unit.position[2] >> 16)
                };
                const ground::RoutePoint next{
                    static_cast<int16_t>(decision.target[0] >> 16),
                    static_cast<int16_t>(decision.target[2] >> 16)
                };
                const auto dot = int64_t(next[0] - origin[0]) * completed_leg[0] +
                                 int64_t(next[1] - origin[1]) * completed_leg[1];
                if (completed && dot < 0)
                    ++result.opposite_legs;
                ++result.legs;
                head.points = {origin, next, route.points[2]};
                head.count = 3;
                head.active = true;
                completed = false;
            }
        }
        if (head.active)
            head.tick = arena.tick;
        auto effective = route;
        if (head.active)
            std::copy(head.points.begin(), head.points.end(), effective.points.begin());
        const auto before = unit.position;
        ground::steer_ground(
            unit,
            motion,
            effective,
            acceleration,
            acceleration,
            0,
            ground::FastUnitSteering::as_3_1c
        );
        movement::integrate_local_flow(unit, motion, arena.tick, true, arena, counters);
        movement::record_local_flow_block(head, (motion.flags & movement::collision_blocked) != 0);
        result.distance += oa::base::game_math::distance(
            unit.position[0] - before[0], unit.position[2] - before[2]
        );
        const auto slot = size_t(arena.tick % history_size);
        if (arena.tick >= history_size - 1) {
            const auto from = size_t((arena.tick - (history_size - 1)) % history_size);
            if (result.distance - distances[from] > orbit_travel &&
                oa::base::game_math::distance(
                    unit.position[0] - positions[from][0], unit.position[2] - positions[from][2]
                ) < orbit_radius)
                ++result.closed_orbit_windows;
        }
        positions[slot] = unit.position;
        distances[slot] = result.distance;
        if (oa::base::game_math::distance(
                unit.position[0] - destination[0], unit.position[2] - destination[2]
            ) < uint32_t(arrival_radius)) {
            result.arrival = arena.tick;
            break;
        }
    }
    result.overlaps = arena.overlaps;
    return result;
}

/// Checks that successive local legs clear a full footprint instead of undoing each other.
void test_completed_local_legs_keep_the_north_passing_lane() {
    constexpr uint32_t maximum_queries = 128;
    const auto history = run_reversal_history(true);
    const auto control = run_reversal_history(false);
    OA_CHECK(history == run_reversal_history(true));
    OA_CHECK(history.overlaps == 0 && control.overlaps == 0);
    OA_CHECK(history.arrival != 0 && history.arrival < tick_limit / 2);
    OA_CHECK(control.arrival == 0);
    OA_CHECK(history.opposite_legs == 0 && control.opposite_legs > history.opposite_legs);
    OA_CHECK(history.closed_orbit_windows == 0);
    OA_CHECK(control.closed_orbit_windows > history.closed_orbit_windows);
    OA_CHECK(history.legs < control.legs / 2);
    OA_CHECK(history.distance < control.distance);
    OA_CHECK(history.peak_queries <= maximum_queries && control.peak_queries <= maximum_queries);
    std::printf(
        "local history: arrival %u / %u, legs %u / %u, opposite %u / %u, orbit windows %llu / "
        "%llu\n",
        history.arrival,
        control.arrival,
        history.legs,
        control.legs,
        history.opposite_legs,
        control.opposite_legs,
        static_cast<unsigned long long>(history.closed_orbit_windows),
        static_cast<unsigned long long>(control.closed_orbit_windows)
    );
}

/// Checks that unreached history, forward rejoining and recovery remain usable.
void test_history_does_not_block_forward_or_recovery_legs() {
    constexpr uint32_t maximum_queries = 128;
    constexpr uint32_t recovery_tick = 118;
    QueueArena arena;
    arena.tick = recovery_tick;
    auto& unit = arena.movers[0];
    movement::Movement motion{};
    motion.flags = ground_occupancy;
    motion.last_motion_tick = arena.tick;
    arena.progress[mover_id] = motion.last_motion_tick;
    movement::LocalFlowHead previous{};
    previous.count = 3;
    previous.points = {
        ground::RoutePoint{centre(start_column), centre(lane_row) + cell_width},
        ground::RoutePoint{centre(start_column), centre(lane_row)},
        ground::RoutePoint{destination_x, centre(lane_row)}
    };
    const std::array<movement::Fixed, 3> destination{
        destination_x * fixed_one, 0, centre(lane_row) * fixed_one
    };
    const auto query = [&](const movement::LocalFlowHead* history, bool allow_reverse) {
        auto probe = arena.probe();
        probe.previous_head = history;
        probe.allow_reverse = allow_reverse;
        movement::LocalFlowCounters counters{};
        const auto result = movement::choose_local_flow(
            unit, motion, destination, arena.tick, arena, probe, counters
        );
        OA_CHECK(counters.probes <= maximum_queries);
        return result;
    };
    const auto no_history = query(nullptr, false);
    OA_CHECK(no_history.adjusted && no_history.target[2] > unit.position[2]);
    const auto continued = query(&previous, false);
    OA_CHECK(continued.adjusted && continued.target[2] < unit.position[2]);
    previous.points[1][1] -= cell_width;
    const auto unreached = query(&previous, false);
    OA_CHECK(unreached.adjusted && unreached.target == no_history.target);
    previous.points[1][1] += cell_width;
    previous.blocked_ticks = movement::local_flow_retry_ticks + 1;
    const bool recovery =
        movement::local_flow_recovery_due(previous, arena.tick, uint16_t(unit.id));
    OA_CHECK(recovery);
    const auto released = query(&previous, recovery);
    OA_CHECK(released.adjusted && released.target == no_history.target);

    // The first diagonal step can rejoin the canonical route with real forward
    // progress even though its lateral component opposes the previous leg.
    arena.remove_occupancy(arena.blocker);
    arena.blocker.cell[0] += footprint;
    arena.blocker.position[0] = centre(arena.blocker.cell[0]) * fixed_one;
    arena.insert_occupancy(arena.blocker);
    const auto forward = query(&previous, false);
    OA_CHECK(forward.adjusted);
    OA_CHECK(forward.target[0] > unit.position[0] && forward.target[2] > unit.position[2]);
    OA_CHECK(arena.overlaps == 0);
}

/// Lets a higher-id blocker make a real subcell progress pulse at a passing boundary.
struct CreepingArena : NorthPassingArena {
    static constexpr uint16_t creeping_id = mover_id + 1;
    static constexpr movement::Fixed creep_speed = fixed_one / 16;
    movement::Movement creeping{};

    CreepingArena() {
        remove_occupancy(blocker);
        blocker.id = creeping_id;
        insert_occupancy(blocker);
        creeping.flags = ground_occupancy;
        creeping.speed = creep_speed;
        creeping.velocity = movement::aim_velocity(blocker.heading, 0, creep_speed);
    }

    /// Reads the two real actors and their tick-start progress snapshots.
    movement::LocalFlowProbe probe() {
        return {
            this,
            [](void* context,
               const movement::Unit& unit,
               std::array<int16_t, 2> cell,
               uint8_t occupancy) {
                auto& arena = *static_cast<CreepingArena*>(context);
                using movement::LocalFlowBlocker;
                using movement::LocalFlowObstacle;
                if (occupancy != ground_occupancy || !arena.terrain_clear(unit, cell))
                    return LocalFlowBlocker{LocalFlowObstacle::stationary};
                for (int16_t z = 0; z < unit.footprint[1]; ++z)
                    for (int16_t x = 0; x < unit.footprint[0]; ++x) {
                        const auto id = arena.occupants[plot(cell[0] + x, cell[1] + z)];
                        if (id != 0 && id != uint16_t(unit.id))
                            return LocalFlowBlocker{
                                LocalFlowObstacle::moving,
                                id,
                                arena.blocker.heading,
                                arena.tick - arena.progress[id]
                            };
                    }
                return LocalFlowBlocker{};
            },
            progress
        };
    }
};

struct GapResult {
    uint64_t distance{};
    uint64_t closed_orbit_windows{};
    uint32_t arrival{};
    uint32_t legs{};
    uint32_t completion_gaps{};
    uint32_t tied_priority_gaps{};
    uint32_t bridged_gaps{};
    uint32_t opposite_legs{};
    uint32_t peak_queries{};
    uint32_t overlaps{};
    bool operator==(const GapResult&) const = default;
};

/// Reproduces a one-tick choice gap caused by real progress-clock priority.
GapResult run_completion_gap(bool bridge_previous_tick) {
    constexpr size_t history_size = 61;
    constexpr uint64_t orbit_travel = 24 * fixed_one;
    constexpr uint32_t orbit_radius = 5 * fixed_one;
    CreepingArena arena;
    auto& unit = arena.movers[0];
    movement::Movement motion{};
    motion.flags = ground_occupancy;
    motion.speed = maximum_speed;
    movement::LocalFlowHead head{};
    movement::LocalFlowCounters counters{};
    const std::array<movement::Fixed, 3> destination{
        destination_x * fixed_one, 0, centre(lane_row) * fixed_one
    };
    ground::Navigation route{};
    route.count = 3;
    route.flags = ground::route_present_flag | ground::search_pending_flag;
    route.points[0] = {centre(start_column), centre(lane_row)};
    route.points[1] = route.points[2] = {destination_x, centre(lane_row)};
    std::array<std::array<movement::Fixed, 3>, history_size> positions{};
    positions.fill(unit.position);
    std::array<uint64_t, history_size> distances{};
    std::array<int32_t, 2> completed_leg{};
    bool completed{};
    GapResult result{};
    for (arena.tick = 1; arena.tick <= tick_limit; ++arena.tick) {
        arena.progress[mover_id] = motion.last_motion_tick;
        arena.progress[CreepingArena::creeping_id] = arena.creeping.last_motion_tick;
        const bool recovery =
            movement::local_flow_recovery_due(head, arena.tick, uint16_t(unit.id));
        const bool just_reached = head.active && movement::local_flow_head_reached(unit, head);
        const bool previous_tick = movement::local_flow_completed_last_tick(head, arena.tick);
        if (just_reached) {
            completed_leg = {
                int32_t(head.points[1][0]) - head.points[0][0],
                int32_t(head.points[1][1]) - head.points[0][1]
            };
            completed = true;
            head.active = false;
            head.count = movement::local_flow_completed_count;
            head.tick = arena.tick;
        } else if (head.active && recovery) {
            head.active = false;
            head.count = 0;
        }
        if (!head.active && (head.blocked_ticks < movement::local_flow_retry_ticks || recovery)) {
            auto probe = arena.probe();
            probe.previous_head =
                just_reached || (bridge_previous_tick && previous_tick) ? &head : nullptr;
            probe.allow_reverse = recovery;
            if (bridge_previous_tick && previous_tick)
                ++result.bridged_gaps;
            const auto queries = counters.probes;
            const auto decision = movement::choose_local_flow(
                unit, motion, destination, arena.tick, arena, probe, counters
            );
            result.peak_queries =
                std::max(result.peak_queries, static_cast<uint32_t>(counters.probes - queries));
            head.mobile_blocked = decision.mobile_blocked;
            if (decision.adjusted) {
                const ground::RoutePoint origin{
                    static_cast<int16_t>(unit.position[0] >> 16),
                    static_cast<int16_t>(unit.position[2] >> 16)
                };
                const ground::RoutePoint next{
                    static_cast<int16_t>(decision.target[0] >> 16),
                    static_cast<int16_t>(decision.target[2] >> 16)
                };
                const auto dot = int64_t(next[0] - origin[0]) * completed_leg[0] +
                                 int64_t(next[1] - origin[1]) * completed_leg[1];
                if (completed && dot < 0)
                    ++result.opposite_legs;
                ++result.legs;
                head.points = {origin, next, route.points[2]};
                head.count = 3;
                head.active = true;
                completed = false;
            } else if (just_reached) {
                ++result.completion_gaps;
                if (motion.last_motion_tick == arena.creeping.last_motion_tick)
                    ++result.tied_priority_gaps;
            }
        }
        if (head.active)
            head.tick = arena.tick;
        auto effective = route;
        if (head.active)
            std::copy(head.points.begin(), head.points.end(), effective.points.begin());
        const auto before = unit.position;
        ground::steer_ground(
            unit,
            motion,
            effective,
            acceleration,
            acceleration,
            0,
            ground::FastUnitSteering::as_3_1c
        );
        movement::integrate_local_flow(unit, motion, arena.tick, true, arena, counters);
        movement::record_local_flow_block(head, (motion.flags & movement::collision_blocked) != 0);
        result.distance += oa::base::game_math::distance(
            unit.position[0] - before[0], unit.position[2] - before[2]
        );
        const auto slot = size_t(arena.tick % history_size);
        if (arena.tick >= history_size - 1) {
            const auto from = size_t((arena.tick - (history_size - 1)) % history_size);
            if (result.distance - distances[from] > orbit_travel &&
                oa::base::game_math::distance(
                    unit.position[0] - positions[from][0], unit.position[2] - positions[from][2]
                ) < orbit_radius)
                ++result.closed_orbit_windows;
        }
        positions[slot] = unit.position;
        distances[slot] = result.distance;
        // The later actor really moves, so both next-tick priority clocks tie.
        // On the following tick it holds position and loses that fresh age.
        if (head.active && movement::local_flow_head_reached(unit, head))
            movement::integrate_local_flow(
                arena.blocker, arena.creeping, arena.tick, true, arena, counters
            );
        if (oa::base::game_math::distance(
                unit.position[0] - destination[0], unit.position[2] - destination[2]
            ) < uint32_t(arrival_radius)) {
            result.arrival = arena.tick;
            break;
        }
    }
    result.overlaps = arena.overlaps;
    return result;
}

/// Checks that a single canonical-route tick cannot erase a completed passing leg.
void test_one_tick_priority_gap_keeps_the_completed_passing_leg() {
    constexpr uint32_t maximum_queries = 128;
    const auto bridged = run_completion_gap(true);
    const auto immediate_only = run_completion_gap(false);
    OA_CHECK(bridged == run_completion_gap(true));
    OA_CHECK(bridged.overlaps == 0 && immediate_only.overlaps == 0);
    OA_CHECK(bridged.arrival != 0 && bridged.arrival < tick_limit / 2);
    OA_CHECK(immediate_only.arrival == 0);
    OA_CHECK(bridged.completion_gaps != 0 && bridged.tied_priority_gaps != 0);
    OA_CHECK(bridged.bridged_gaps != 0);
    OA_CHECK(bridged.opposite_legs == 0 && immediate_only.opposite_legs != 0);
    OA_CHECK(bridged.closed_orbit_windows == 0 && immediate_only.closed_orbit_windows != 0);
    OA_CHECK(bridged.legs < immediate_only.legs / 2);
    OA_CHECK(bridged.distance < immediate_only.distance);
    OA_CHECK(bridged.peak_queries <= maximum_queries);
    OA_CHECK(immediate_only.peak_queries <= maximum_queries);
    std::printf(
        "one-tick history: arrival %u / %u, gaps %u / %u, opposite %u / %u, orbit windows %llu / "
        "%llu\n",
        bridged.arrival,
        immediate_only.arrival,
        bridged.completion_gaps,
        immediate_only.completion_gaps,
        bridged.opposite_legs,
        immediate_only.opposite_legs,
        static_cast<unsigned long long>(bridged.closed_orbit_windows),
        static_cast<unsigned long long>(immediate_only.closed_orbit_windows)
    );
}

} // namespace

int main() {
    test_mature_queue_recovers_without_renewing_replan_grace();
    test_whole_mature_train_recovers();
    test_completed_mobile_units_do_not_wall_off_followers();
    test_mature_recovery_is_staggered_and_keeps_full_replanning_enabled();
    test_immediate_history_preserves_queue_arrivals();
    test_completed_local_legs_keep_the_north_passing_lane();
    test_history_does_not_block_forward_or_recovery_legs();
    test_one_tick_priority_gap_keeps_the_completed_passing_leg();
    return oa::test::check_exit_status();
}
