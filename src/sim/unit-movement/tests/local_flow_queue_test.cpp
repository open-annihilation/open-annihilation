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
                            if (id != blocker_id && !arena.travelling[id - mover_id])
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
QueueResult run_queue(bool renew_local_attempts) {
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
        if (head.active &&
            (movement::local_flow_head_reached(unit, head) ||
             (renew_local_attempts ? recovery
                                   : head.blocked_ticks >= movement::local_flow_retry_ticks)))
            head.active = false;
        if (!head.active && (head.blocked_ticks < movement::local_flow_retry_ticks || recovery)) {
            const auto decision = movement::choose_local_flow(
                unit, motion, destination, arena.tick, arena, arena.probe(), counters
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

/// Drives every mover in a touching two-cell queue through the new passing area.
TrainResult run_train(bool renew_local_attempts) {
    constexpr size_t history_size = 61;
    constexpr int16_t goal_spacing = 48;
    constexpr int16_t first_goal_offset = (maximum_movers - 1) * goal_spacing / 2;
    constexpr uint64_t orbit_travel = 24 * fixed_one;
    constexpr uint32_t orbit_radius = 5 * fixed_one;
    QueueArena arena(maximum_movers);
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
        destinations[index] = {
            destination_x * fixed_one,
            0,
            static_cast<movement::Fixed>(
                (centre(lane_row) + int(index) * goal_spacing - first_goal_offset) * fixed_one
            )
        };
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
            if (head.active &&
                (movement::local_flow_head_reached(unit, head) ||
                 (renew_local_attempts ? recovery
                                       : head.blocked_ticks >= movement::local_flow_retry_ticks)))
                head.active = false;
            if (!head.active &&
                (head.blocked_ticks < movement::local_flow_retry_ticks || recovery)) {
                const auto decision = movement::choose_local_flow(
                    unit, motion, destinations[index], arena.tick, arena, arena.probe(), counters
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
} // namespace

int main() {
    test_mature_queue_recovers_without_renewing_replan_grace();
    test_whole_mature_train_recovers();
    test_mature_recovery_is_staggered_and_keeps_full_replanning_enabled();
    return oa::test::check_exit_status();
}
