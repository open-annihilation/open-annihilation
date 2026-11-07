// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/base/game_math.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <cstdio>

namespace movement = oa::sim::unit_movement;
namespace ground = oa::sim::ground_orders;

namespace {
constexpr movement::Fixed fixed_one = 65536;
constexpr movement::Fixed arrival_radius = 5 * fixed_one;
constexpr movement::Fixed acceleration = 6553;
constexpr int16_t map_width = 128;
constexpr int16_t map_height = 64;
constexpr size_t agent_count = 8;
constexpr size_t history_count = 61;
constexpr uint32_t tick_limit = 900;
constexpr uint32_t progress_measurement_tick = 450;
constexpr uint16_t east = 49152;
constexpr uint16_t west = 16384;
constexpr uint16_t south = 32768;
constexpr uint16_t north = 0;
constexpr uint32_t crossing_count = 4;

uint32_t
planar_distance(const std::array<movement::Fixed, 3>& a, const std::array<movement::Fixed, 3>& b) {
    return oa::base::game_math::distance(a[0] - b[0], a[2] - b[2]);
}

bool radius_only_reached(const movement::Unit& unit, const movement::LocalFlowHead& head) {
    return planar_distance(
               unit.position,
               {int32_t(head.points[1][0]) * fixed_one, 0, int32_t(head.points[1][1]) * fixed_one}
           ) < arrival_radius;
}

struct Agent {
    movement::Unit unit{};
    movement::Movement motion{};
    ground::Navigation navigation{};
    movement::LocalFlowHead head{};
    std::array<movement::Fixed, 3> initial{};
    std::array<movement::Fixed, 3> destination{};
    std::array<std::array<movement::Fixed, 3>, history_count> positions{};
    std::array<uint64_t, history_count> path_lengths{};
    uint64_t path_length{};
    uint32_t arrival_tick{};
    bool travelling{true};
};

struct Arena : movement::Host {
    std::array<uint16_t, size_t(map_width) * map_height> occupants{};
    std::array<Agent, agent_count> agents{};
    std::array<uint32_t, agent_count + 1> progress{};
    size_t count{};
    uint32_t tick{};
    unsigned overlap_count{};
    bool idle_units_are_mobile{};

    static size_t plot(int16_t x, int16_t z) { return size_t(z) * map_width + size_t(x); }

    Agent& add(int16_t x, int16_t destination_x, uint16_t heading) {
        return add_at(x, 32, destination_x, 32, heading);
    }

    /// Places a mover on an axis-aligned route through an intersection.
    Agent&
    add_at(int16_t x, int16_t z, int16_t destination_x, int16_t destination_z, uint16_t heading) {
        auto& agent = agents[count++];
        agent.unit.id = static_cast<int16_t>(count);
        agent.unit.cell = {x, z};
        agent.unit.position = {
            (int32_t(x) * 16 + 8) * fixed_one, 0, (int32_t(z) * 16 + 8) * fixed_one
        };
        agent.unit.footprint = {1, 1};
        agent.unit.flags = 1;
        agent.unit.heading = heading;
        agent.unit.type.maximum_speed = 3 * fixed_one;
        agent.unit.type.maximum_turn = 1024;
        agent.motion.flags = 1;
        agent.motion.speed = agent.unit.type.maximum_speed;
        agent.initial = agent.unit.position;
        agent.destination = {
            (int32_t(destination_x) * 16 + 8) * fixed_one,
            0,
            (int32_t(destination_z) * 16 + 8) * fixed_one
        };
        agent.navigation.count = 3;
        agent.navigation.flags = ground::route_present_flag;
        agent.navigation.points[0] = {
            static_cast<int16_t>(agent.initial[0] >> 16),
            static_cast<int16_t>(agent.initial[2] >> 16)
        };
        agent.navigation.points[1] = {
            static_cast<int16_t>(agent.destination[0] >> 16),
            static_cast<int16_t>(agent.destination[2] >> 16)
        };
        agent.navigation.points[2] = agent.navigation.points[1];
        agent.positions.fill(agent.initial);
        insert_occupancy(agent.unit);
        return agent;
    }

    bool can_occupy(
        const movement::Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy
    ) override {
        return occupancy == 1 && cell[0] >= 0 && cell[1] >= 0 && cell[0] < map_width &&
               cell[1] < map_height &&
               (occupants[plot(cell[0], cell[1])] == 0 ||
                occupants[plot(cell[0], cell[1])] == uint16_t(unit.id));
    }

    void remove_occupancy(movement::Unit& unit) override {
        auto& occupant = occupants[plot(unit.cell[0], unit.cell[1])];
        if (occupant != uint16_t(unit.id))
            ++overlap_count;
        occupant = 0;
    }

    void insert_occupancy(movement::Unit& unit) override {
        if (!can_occupy(unit, unit.cell, 1)) {
            ++overlap_count;
            return;
        }
        occupants[plot(unit.cell[0], unit.cell[1])] = static_cast<uint16_t>(unit.id);
    }

    void update_spatial_membership(movement::Unit&) override {}

    movement::LocalFlowProbe probe() {
        return {
            this,
            [](void* context,
               const movement::Unit& unit,
               std::array<int16_t, 2> cell,
               uint8_t occupancy) {
                auto& arena = *static_cast<Arena*>(context);
                using movement::LocalFlowBlocker;
                using movement::LocalFlowObstacle;
                if (occupancy != 1 || cell[0] < 0 || cell[1] < 0 || cell[0] >= map_width ||
                    cell[1] >= map_height)
                    return LocalFlowBlocker{LocalFlowObstacle::stationary};
                const auto id = arena.occupants[plot(cell[0], cell[1])];
                if (id == 0 || id == uint16_t(unit.id))
                    return LocalFlowBlocker{};
                const auto& other = arena.agents[id - 1];
                if (!other.travelling && !arena.idle_units_are_mobile)
                    return LocalFlowBlocker{LocalFlowObstacle::stationary};
                return LocalFlowBlocker{
                    LocalFlowObstacle::moving,
                    id,
                    other.unit.heading,
                    arena.tick - arena.progress[id]
                };
            },
            progress
        };
    }
};

struct Result {
    uint64_t turn{};
    uint64_t backtrack{};
    uint64_t closed_orbit_windows{};
    uint64_t progress{};
    uint64_t progress_at_450{};
    uint64_t blocked{};
    uint64_t adjustments{};
    uint64_t history_transitions{};
    uint32_t arrived{};
    uint32_t last_arrival{};
    uint32_t end_tick{};
    unsigned overlaps{};
    uint64_t digest{14695981039346656037ull};
    bool operator==(const Result&) const = default;
};

enum class CohortKind { opposing, crossing };

Result
run_cohorts(bool passed_plane, bool history = false, CohortKind kind = CohortKind::opposing) {
    Arena arena;
    if (kind == CohortKind::opposing) {
        for (int16_t i = 0; i < 4; ++i)
            arena.add(static_cast<int16_t>(16 + i * 4), static_cast<int16_t>(100 + i * 4), east);
        for (int16_t i = 0; i < 4; ++i)
            arena.add(static_cast<int16_t>(100 + i * 4), static_cast<int16_t>(16 + i * 4), west);
    } else {
        arena.idle_units_are_mobile = true;
        arena.add_at(40, 32, 88, 32, east);
        arena.add_at(88, 32, 40, 32, west);
        arena.add_at(64, 8, 64, 56, south);
        arena.add_at(64, 56, 64, 8, north);
    }
    movement::LocalFlowCounters counters{};
    Result result{};
    for (arena.tick = 1; arena.tick <= tick_limit; ++arena.tick) {
        for (size_t i = 0; i < arena.count; ++i)
            arena.progress[i + 1] = arena.agents[i].motion.last_motion_tick;
        bool travelling = false;
        for (auto& agent : std::span{arena.agents}.first(arena.count)) {
            if (!agent.travelling)
                continue;
            if (planar_distance(agent.unit.position, agent.destination) < arrival_radius) {
                agent.travelling = false;
                agent.arrival_tick = arena.tick;
                agent.motion.speed = 0;
                agent.motion.velocity = {};
                ++result.arrived;
                result.last_arrival = arena.tick;
                continue;
            }
            travelling = true;
            const bool recovery =
                movement::local_flow_recovery_due(agent.head, arena.tick, uint16_t(agent.unit.id));
            const bool just_reached =
                agent.head.active &&
                (passed_plane ? movement::local_flow_head_reached(agent.unit, agent.head)
                              : radius_only_reached(agent.unit, agent.head));
            const bool previous_tick =
                movement::local_flow_completed_last_tick(agent.head, arena.tick);
            if (just_reached) {
                agent.head.active = false;
                agent.head.count = movement::local_flow_completed_count;
                agent.head.tick = arena.tick;
            } else if (agent.head.active && recovery) {
                agent.head.active = false;
                agent.head.count = 0;
            } else if (
                !agent.head.active && agent.head.count == movement::local_flow_completed_count &&
                !previous_tick
            )
                agent.head.count = 0;
            if (!agent.head.active &&
                (agent.head.blocked_ticks < movement::local_flow_retry_ticks || recovery)) {
                auto probe = arena.probe();
                probe.previous_head =
                    history && (just_reached || previous_tick) ? &agent.head : nullptr;
                probe.allow_reverse = recovery;
                if (probe.previous_head != nullptr)
                    ++result.history_transitions;
                const auto decision = movement::choose_local_flow(
                    agent.unit, agent.motion, agent.destination, arena.tick, arena, probe, counters
                );
                agent.head.mobile_blocked = decision.mobile_blocked;
                if (decision.adjusted) {
                    agent.head.points = {
                        ground::RoutePoint{
                            static_cast<int16_t>(agent.unit.position[0] >> 16),
                            static_cast<int16_t>(agent.unit.position[2] >> 16)
                        },
                        ground::RoutePoint{
                            static_cast<int16_t>(decision.target[0] >> 16),
                            static_cast<int16_t>(decision.target[2] >> 16)
                        },
                        agent.navigation.points[2]
                    };
                    agent.head.count = 3;
                    agent.head.active = true;
                }
            }
            if (agent.head.active)
                agent.head.tick = arena.tick;
            auto effective = agent.navigation;
            if (agent.head.active)
                std::copy(
                    agent.head.points.begin(), agent.head.points.end(), effective.points.begin()
                );
            const auto before = agent.unit.position;
            const auto heading = agent.unit.heading;
            ground::steer_ground(
                agent.unit,
                agent.motion,
                effective,
                acceleration,
                acceleration,
                0,
                ground::FastUnitSteering::as_3_1c
            );
            result.turn += std::abs(int16_t(agent.unit.heading - heading));
            movement::integrate_local_flow(
                agent.unit, agent.motion, arena.tick, true, arena, counters
            );
            movement::record_local_flow_block(
                agent.head, (agent.motion.flags & movement::collision_blocked) != 0
            );
            const auto directed = int64_t(agent.unit.position[0]) - before[0];
            if ((agent.initial[0] < agent.destination[0] && directed < 0) ||
                (agent.initial[0] > agent.destination[0] && directed > 0))
                result.backtrack += static_cast<uint64_t>(std::abs(directed));
            agent.path_length += planar_distance(before, agent.unit.position);
            const auto previous = size_t(arena.tick % history_count);
            if (arena.tick >= history_count - 1) {
                const auto start = size_t((arena.tick - (history_count - 1)) % history_count);
                if (agent.path_length - agent.path_lengths[start] > 24 * fixed_one &&
                    planar_distance(agent.unit.position, agent.positions[start]) < 5 * fixed_one)
                    ++result.closed_orbit_windows;
            }
            agent.positions[previous] = agent.unit.position;
            agent.path_lengths[previous] = agent.path_length;
        }
        result.end_tick = arena.tick;
        if (arena.tick == progress_measurement_tick)
            for (const auto& agent : std::span{arena.agents}.first(arena.count))
                result.progress_at_450 += static_cast<uint64_t>(
                    std::abs(int64_t(agent.unit.position[0]) - agent.initial[0])
                );
        if (!travelling)
            break;
    }
    result.blocked = counters.blocked_ticks;
    result.adjustments = counters.steering_adjustments;
    result.overlaps = arena.overlap_count;
    for (const auto& agent : std::span{arena.agents}.first(arena.count)) {
        result.progress +=
            static_cast<uint64_t>(std::abs(int64_t(agent.unit.position[0]) - agent.initial[0]));
        for (const auto value : agent.unit.position) {
            result.digest ^= static_cast<uint32_t>(value);
            result.digest *= 1099511628211ull;
        }
        result.digest ^= agent.arrival_tick;
        result.digest *= 1099511628211ull;
    }
    return result;
}

struct OpenGround : movement::Host {
    bool can_occupy(const movement::Unit&, std::array<int16_t, 2>, uint8_t) override {
        return true;
    }

    void remove_occupancy(movement::Unit&) override {}

    void insert_occupancy(movement::Unit&) override {}

    void update_spatial_membership(movement::Unit&) override {}
};

Result run_one_head(bool passed_plane) {
    OpenGround host;
    movement::Unit unit{};
    unit.id = 1;
    unit.position = {72 * fixed_one, 0, 72 * fixed_one};
    unit.cell = {4, 4};
    unit.footprint = {1, 1};
    unit.flags = 1;
    unit.heading = east;
    unit.type.maximum_speed = 3 * fixed_one;
    unit.type.maximum_turn = 1024;
    movement::Movement motion{};
    motion.flags = 1;
    motion.speed = unit.type.maximum_speed;
    movement::LocalFlowHead head{};
    head.active = true;
    head.count = 3;
    head.points = {{{72, 72}, {83, 83}, {400, 72}}};
    ground::Navigation route{};
    route.count = 3;
    route.flags = ground::route_present_flag;
    std::copy(head.points.begin(), head.points.end(), route.points.begin());
    movement::LocalFlowCounters counters{};
    Result result{};
    for (uint32_t tick = 1; tick <= 120; ++tick) {
        if (passed_plane ? movement::local_flow_head_reached(unit, head)
                         : radius_only_reached(unit, head)) {
            result.arrived = 1;
            result.last_arrival = tick;
            break;
        }
        const auto heading = unit.heading;
        const auto before = unit.position;
        ground::steer_ground(
            unit, motion, route, acceleration, acceleration, 0, ground::FastUnitSteering::as_3_1c
        );
        result.turn += std::abs(int16_t(unit.heading - heading));
        movement::integrate_local_flow(unit, motion, tick, true, host, counters);
        result.progress += planar_distance(unit.position, before);
    }
    return result;
}

void test_ground_steering_does_not_orbit_a_passed_head() {
    const auto current = run_one_head(true);
    const auto radius_only = run_one_head(false);
    OA_CHECK(current.arrived == 1 && radius_only.arrived == 1);
    OA_CHECK(current.last_arrival < radius_only.last_arrival / 2);
    OA_CHECK(current.turn < radius_only.turn / 2);
    OA_CHECK(current.progress < radius_only.progress / 2);
}

void test_waypoint_completion_geometry() {
    movement::Unit unit{};
    movement::LocalFlowHead head{};
    head.count = 3;
    head.active = true;
    head.points = {{{0, 0}, {16, 0}, {100, 0}}};
    unit.position = {10 * fixed_one, 0, 8 * fixed_one};
    OA_CHECK(!movement::local_flow_head_reached(unit, head));
    unit.position[0] = 17 * fixed_one;
    OA_CHECK(movement::local_flow_head_reached(unit, head));
    head.active = false;
    OA_CHECK(!movement::local_flow_head_reached(unit, head));
    head.active = true;
    head.count = 0;
    OA_CHECK(!movement::local_flow_head_reached(unit, head));
    head.count = 3;
    unit.position = {13 * fixed_one, 0, fixed_one};
    OA_CHECK(movement::local_flow_head_reached(unit, head));

    // The lateral offset keeps these outside the arrival radius, exercising
    // the full signed map range in the passed-plane dot product.
    head.points = {{{-32768, 0}, {32767, 0}, {32767, 0}}};
    unit.position = {std::numeric_limits<movement::Fixed>::max(), 0, 6 * fixed_one};
    OA_CHECK(movement::local_flow_head_reached(unit, head));
    unit.position[0] = std::numeric_limits<movement::Fixed>::min();
    OA_CHECK(!movement::local_flow_head_reached(unit, head));
    head.points = {{{32767, 0}, {-32768, 0}, {-32768, 0}}};
    OA_CHECK(movement::local_flow_head_reached(unit, head));
    unit.position[0] = std::numeric_limits<movement::Fixed>::max();
    OA_CHECK(!movement::local_flow_head_reached(unit, head));
}

void test_sustained_opposing_cohorts() {
    const auto current = run_cohorts(true);
    const auto repeated = run_cohorts(true);
    const auto radius_only = run_cohorts(false);
    OA_CHECK(current == repeated);
    OA_CHECK(radius_only == run_cohorts(false));
    OA_CHECK(current.overlaps == 0 && radius_only.overlaps == 0);
    OA_CHECK(current.arrived == agent_count);
    OA_CHECK(current.closed_orbit_windows < radius_only.closed_orbit_windows);
    OA_CHECK(current.turn < radius_only.turn);
    OA_CHECK(current.backtrack < radius_only.backtrack);
    OA_CHECK(current.progress_at_450 > radius_only.progress_at_450);
    OA_CHECK(
        current.arrived > radius_only.arrived || current.last_arrival < radius_only.last_arrival
    );
}

/// Checks that immediate history preserves complete opposing and crossing trips.
void test_history_in_actual_opposing_and_crossing_steering() {
    for (const auto kind : {CohortKind::opposing, CohortKind::crossing}) {
        const auto current = run_cohorts(true, false, kind);
        const auto history = run_cohorts(true, true, kind);
        std::printf(
            "kind%d old arrivals%u last%u orbits%llu turn%llu blocked%llu overlap%u; history "
            "arrivals%u last%u orbits%llu turn%llu blocked%llu overlap%u\n",
            kind == CohortKind::crossing,
            current.arrived,
            current.last_arrival,
            (unsigned long long)current.closed_orbit_windows,
            (unsigned long long)current.turn,
            (unsigned long long)current.blocked,
            current.overlaps,
            history.arrived,
            history.last_arrival,
            (unsigned long long)history.closed_orbit_windows,
            (unsigned long long)history.turn,
            (unsigned long long)history.blocked,
            history.overlaps
        );
        OA_CHECK(history == run_cohorts(true, true, kind));
        OA_CHECK(history.arrived == (kind == CohortKind::opposing ? agent_count : crossing_count));
        OA_CHECK(history.history_transitions != 0);
        OA_CHECK(history.overlaps == 0);
        OA_CHECK(history.last_arrival <= current.last_arrival);
        OA_CHECK(history.closed_orbit_windows <= current.closed_orbit_windows);
        OA_CHECK(history.turn <= current.turn);
        OA_CHECK(history.blocked <= current.blocked);
    }
}
} // namespace

int main() {
    test_ground_steering_does_not_orbit_a_passed_head();
    test_waypoint_completion_geometry();
    test_sustained_opposing_cohorts();
    test_history_in_actual_opposing_and_crossing_steering();
    return oa::test::check_exit_status();
}
