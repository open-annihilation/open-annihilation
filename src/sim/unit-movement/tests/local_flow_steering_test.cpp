// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/base/game_math.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/ground_orders/steering_target.hpp"
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
#include <vector>

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
    uint32_t peak_queries{};
    unsigned overlaps{};
    uint64_t digest{14695981039346656037ull};
    bool operator==(const Result&) const = default;
};

enum class CohortKind { opposing, crossing };

Result run_cohorts(
    bool passed_plane,
    bool history = false,
    CohortKind kind = CohortKind::opposing,
    bool controller_probe = false
) {
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
            }
            if (!agent.head.active &&
                (agent.head.blocked_ticks < movement::local_flow_retry_ticks || recovery)) {
                auto probe = arena.probe();
                probe.previous_head =
                    history && (just_reached || previous_tick) ? &agent.head : nullptr;
                probe.allow_reverse = recovery;
                if (probe.previous_head != nullptr)
                    ++result.history_transitions;
                const auto queries = counters.probes;
                const auto decision =
                    controller_probe ? movement::choose_local_flow(
                                           agent.unit,
                                           agent.motion,
                                           agent.destination,
                                           ground::steering_target(
                                               agent.unit, ground::steering_points(agent.navigation)
                                           )
                                               .point,
                                           arena.tick,
                                           arena,
                                           probe,
                                           counters
                                       )
                                     : movement::choose_local_flow(
                                           agent.unit,
                                           agent.motion,
                                           agent.destination,
                                           arena.tick,
                                           arena,
                                           probe,
                                           counters
                                       );
                result.peak_queries =
                    std::max(result.peak_queries, static_cast<uint32_t>(counters.probes - queries));
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

/// Compares every field movement exposes, including the type and unchanged inputs.
bool same_unit(const movement::Unit& a, const movement::Unit& b) {
    return a.position == b.position && a.cell == b.cell && a.footprint == b.footprint &&
           a.id == b.id && a.heading == b.heading && a.pitch == b.pitch && a.flags == b.flags &&
           a.type.maximum_speed == b.type.maximum_speed &&
           a.type.maximum_turn == b.type.maximum_turn && a.type.flags == b.type.flags;
}

/// Compares all motion fields without depending on padding bytes.
bool same_motion(const movement::Movement& a, const movement::Movement& b) {
    return a.velocity == b.velocity && a.speed == b.speed && a.turn == b.turn &&
           a.last_motion_tick == b.last_motion_tick && a.flags == b.flags;
}

/// Compares all local steering work counters, including those this call must preserve.
bool same_counters(const movement::LocalFlowCounters& a, const movement::LocalFlowCounters& b) {
    return a.probes == b.probes && a.moving_blockers == b.moving_blockers &&
           a.steering_adjustments == b.steering_adjustments && a.yielded_ticks == b.yielded_ticks &&
           a.blocked_ticks == b.blocked_ticks && a.deferred_replans == b.deferred_replans;
}

/// Builds a nonzero movement state so parity checks cover fields not changed by steering.
movement::Unit parity_unit() {
    movement::Unit unit{};
    unit.position = {72 * fixed_one + 7, 6 * fixed_one + 11, 72 * fixed_one + 13};
    unit.cell = {4, 4};
    unit.footprint = {1, 1};
    unit.id = 3;
    unit.heading = east;
    unit.pitch = -511;
    unit.flags = movement::position_dirty | 1;
    unit.type.maximum_speed = 3 * fixed_one;
    unit.type.maximum_turn = 1024;
    unit.type.flags = movement::water_speed_exemption_mask;
    return unit;
}

/// Builds a nonzero motion record with an actual progress clock.
movement::Movement parity_motion() {
    movement::Movement motion{};
    motion.velocity = {3 * fixed_one + 17, 23, -29};
    motion.speed = 2 * fixed_one;
    motion.turn = -91;
    motion.last_motion_tick = 90;
    motion.flags = 1 | movement::collision_blocked;
    return motion;
}

/// Requires the cached and ordinary controllers to change exactly the same fields.
void check_steering_pair(
    const movement::Unit& initial,
    const movement::Movement& motion,
    const ground::Navigation& navigation,
    ground::FastUnitSteering fast_units,
    const ground::SteeringTargetCache& cache
) {
    auto ordinary_unit = initial;
    auto cached_unit = initial;
    auto ordinary_motion = motion;
    auto cached_motion = motion;
    ground::steer_ground(
        ordinary_unit, ordinary_motion, navigation, acceleration, acceleration, 10, fast_units
    );
    ground::steer_ground(
        cached_unit, cached_motion, navigation, acceleration, acceleration, 10, fast_units, cache
    );
    OA_CHECK(same_unit(ordinary_unit, cached_unit));
    OA_CHECK(same_motion(ordinary_motion, cached_motion));
}

/// Keeps exact projection reuse identical across near, projected, empty and wrapping routes.
void test_cached_ground_steering_matches_all_fields() {
    constexpr int16_t lowest = std::numeric_limits<int16_t>::min();
    constexpr int16_t highest = std::numeric_limits<int16_t>::max();
    constexpr std::array<std::array<ground::RoutePoint, 3>, 7> routes{{
        {{{72, 72}, {80, 74}, {80, 74}}},
        {{{72, 72}, {400, 250}, {450, 280}}},
        {{{400, 250}, {400, 250}, {450, 280}}},
        {{{72, 72}, {72, 72}, {72, 72}}},
        {{{lowest, highest}, {highest, lowest}, {highest, highest}}},
        {{{highest, lowest}, {lowest, highest}, {lowest, lowest}}},
        {{{0, 0}, {highest, highest}, {0, 0}}},
    }};
    for (const auto& points : routes)
        for (const auto fast_units :
             {ground::FastUnitSteering::as_3_1c, ground::FastUnitSteering::held_to_route})
            for (const auto speed : {movement::Fixed{0}, 2 * fixed_one, 9 * fixed_one}) {
                auto unit = parity_unit();
                unit.type.maximum_speed = speed == 9 * fixed_one ? speed : 3 * fixed_one;
                auto motion = parity_motion();
                motion.speed = speed;
                ground::Navigation navigation{};
                std::copy(points.begin(), points.end(), navigation.points.begin());
                navigation.count = 3;
                navigation.flags = ground::route_present_flag;
                const auto cache =
                    ground::cache_steering_target(unit, ground::steering_points(navigation));
                OA_CHECK(cache.position == unit.position);
                OA_CHECK(cache.points == ground::steering_points(navigation));
                const auto direct = ground::steering_target(unit, cache.points);
                OA_CHECK(
                    cache.target.point == direct.point && cache.target.distance == direct.distance
                );
                check_steering_pair(unit, motion, navigation, fast_units, cache);
                unit.type.maximum_turn = 0;
                check_steering_pair(unit, motion, navigation, fast_units, cache);
                navigation.count = 0;
                check_steering_pair(unit, motion, navigation, fast_units, cache);
                navigation.count = ground::navigation_capacity + 1;
                check_steering_pair(unit, motion, navigation, fast_units, cache);
                navigation.count = 3;
                navigation.flags = 0;
                check_steering_pair(unit, motion, navigation, fast_units, cache);
            }

    for (const auto position :
         {std::numeric_limits<movement::Fixed>::min(),
          std::numeric_limits<movement::Fixed>::max()}) {
        auto unit = parity_unit();
        unit.position = {position, position, position};
        ground::Navigation navigation{};
        std::copy(routes[4].begin(), routes[4].end(), navigation.points.begin());
        navigation.count = 3;
        navigation.flags = ground::route_present_flag;
        const auto cache = ground::cache_steering_target(unit, ground::steering_points(navigation));
        check_steering_pair(
            unit, parity_motion(), navigation, ground::FastUnitSteering::as_3_1c, cache
        );
    }
}

/// Keeps cache inputs as values when a retained head or the unit pose changes in place.
void test_cached_ground_steering_rechecks_changed_inputs() {
    auto unit = parity_unit();
    ground::Navigation navigation{};
    navigation.points = {{{72, 72}, {400, 250}, {450, 280}}};
    navigation.count = 3;
    navigation.flags = ground::route_present_flag;
    auto points = ground::steering_points(navigation);
    const auto saved_points = points;
    const auto saved_position = unit.position;
    const auto cache = ground::cache_steering_target(unit, points);
    for (size_t point = 0; point < saved_points.size(); ++point) {
        auto changed = navigation;
        changed.points[point] = {100, 600};
        points = ground::steering_points(changed);
        OA_CHECK(cache.points == saved_points && cache.points != points);
        check_steering_pair(
            unit, parity_motion(), changed, ground::FastUnitSteering::held_to_route, cache
        );
    }
    for (size_t coordinate = 0; coordinate < unit.position.size(); ++coordinate) {
        auto changed = unit;
        changed.position[coordinate] += 33 * fixed_one;
        OA_CHECK(cache.position == saved_position && cache.position != changed.position);
        check_steering_pair(
            changed, parity_motion(), navigation, ground::FastUnitSteering::held_to_route, cache
        );
    }
}

enum class FlowCallKind { probe, clearance, remove, insert, spatial };

struct FlowCall {
    FlowCallKind kind{};
    movement::Unit unit{};
    std::array<int16_t, 2> cell{};
    uint8_t occupancy{};

    bool operator==(const FlowCall& other) const {
        return kind == other.kind && same_unit(unit, other.unit) && cell == other.cell &&
               occupancy == other.occupancy;
    }
};

enum class FlowClearance { clear, blocked, behind };

/// Records the exact traffic-query and occupancy-call order for one decision.
struct FlowParityHost : movement::Host {
    std::vector<FlowCall> calls{};
    std::array<uint32_t, 32> progress{};
    movement::LocalFlowBlocker blocker{};
    FlowClearance clearance{};

    bool can_occupy(
        const movement::Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy
    ) override {
        calls.push_back({FlowCallKind::clearance, unit, cell, occupancy});
        return clearance == FlowClearance::clear ||
               (clearance == FlowClearance::behind && cell[0] < unit.cell[0]);
    }

    void remove_occupancy(movement::Unit& unit) override {
        calls.push_back({FlowCallKind::remove, unit, unit.cell, 0});
    }

    void insert_occupancy(movement::Unit& unit) override {
        calls.push_back({FlowCallKind::insert, unit, unit.cell, 0});
    }

    void update_spatial_membership(movement::Unit& unit) override {
        calls.push_back({FlowCallKind::spatial, unit, unit.cell, 0});
    }

    movement::LocalFlowProbe probe() {
        progress.fill(90);
        return {
            this,
            [](void* context,
               const movement::Unit& unit,
               std::array<int16_t, 2> cell,
               uint8_t occupancy) {
                auto& host = *static_cast<FlowParityHost*>(context);
                host.calls.push_back({FlowCallKind::probe, unit, cell, occupancy});
                return host.blocker;
            },
            progress
        };
    }
};

/// Compares decisions, complete state and exact host/probe sequences for one cached distance.
movement::LocalFlowDecision check_flow_pair(
    const movement::Unit& unit,
    const movement::Movement& motion,
    ground::Point target,
    ground::Point probe_target,
    movement::LocalFlowBlocker blocker,
    FlowClearance clearance,
    movement::Fixed distance
) {
    FlowParityHost ordinary_host;
    FlowParityHost cached_host;
    ordinary_host.blocker = cached_host.blocker = blocker;
    ordinary_host.clearance = cached_host.clearance = clearance;
    auto ordinary_unit = unit;
    auto cached_unit = unit;
    auto ordinary_motion = motion;
    auto cached_motion = motion;
    movement::LocalFlowCounters ordinary_counters{11, 12, 13, 14, 15, 16};
    auto cached_counters = ordinary_counters;
    const auto ordinary = movement::choose_local_flow(
        ordinary_unit,
        ordinary_motion,
        target,
        probe_target,
        100,
        ordinary_host,
        ordinary_host.probe(),
        ordinary_counters
    );
    const auto cached = movement::choose_local_flow(
        cached_unit,
        cached_motion,
        target,
        probe_target,
        100,
        cached_host,
        cached_host.probe(),
        cached_counters,
        distance
    );
    OA_CHECK(ordinary.target == cached.target);
    OA_CHECK(ordinary.adjusted == cached.adjusted);
    OA_CHECK(ordinary.reverse == cached.reverse);
    OA_CHECK(ordinary.mobile_blocked == cached.mobile_blocked);
    OA_CHECK(same_unit(ordinary_unit, unit) && same_unit(cached_unit, unit));
    OA_CHECK(same_motion(ordinary_motion, motion) && same_motion(cached_motion, motion));
    OA_CHECK(same_counters(ordinary_counters, cached_counters));
    OA_CHECK(ordinary_host.calls == cached_host.calls);
    return cached;
}

/// Retains traffic outcomes and call ordering when the original target distance is reused.
void test_cached_local_flow_distance_matches_queries_and_decisions() {
    auto unit = parity_unit();
    const auto motion = parity_motion();
    const ground::Point target{400 * fixed_one, unit.position[1], 72 * fixed_one + 13};
    const std::array<ground::Point, 3> points{unit.position, target, target};
    const auto cache = ground::cache_steering_target(unit, points);
    OA_CHECK(cache.target.point != target && cache.target.distance > 80 * fixed_one);
    const movement::LocalFlowBlocker moving{movement::LocalFlowObstacle::moving, 20, west, 20};
    const std::array<movement::LocalFlowBlocker, 4> blockers{{
        {},
        {movement::LocalFlowObstacle::stationary},
        moving,
        {movement::LocalFlowObstacle::moving, 20, east, 0},
    }};
    for (const auto& blocker : blockers)
        for (const auto clearance :
             {FlowClearance::clear, FlowClearance::blocked, FlowClearance::behind})
            for (const auto distance :
                 {cache.target.distance,
                  movement::Fixed{-1},
                  movement::Fixed{-7},
                  std::numeric_limits<movement::Fixed>::min()})
                (void)check_flow_pair(
                    unit, motion, target, cache.target.point, blocker, clearance, distance
                );
    const auto adjusted = check_flow_pair(
        unit,
        motion,
        target,
        cache.target.point,
        moving,
        FlowClearance::clear,
        cache.target.distance
    );
    OA_CHECK(adjusted.mobile_blocked && adjusted.adjusted && !adjusted.reverse);
    const auto reverse = check_flow_pair(
        unit, motion, target, target, moving, FlowClearance::behind, cache.target.distance
    );
    OA_CHECK(reverse.mobile_blocked && reverse.adjusted && reverse.reverse);

    constexpr auto minimum = std::numeric_limits<movement::Fixed>::min();
    constexpr auto maximum = std::numeric_limits<movement::Fixed>::max();
    constexpr std::array<ground::Point, 6> targets{{
        {72 * fixed_one + 7, 0, 72 * fixed_one + 13},
        {80 * fixed_one, 0, 72 * fixed_one},
        {400 * fixed_one, 0, 400 * fixed_one},
        {minimum, 0, maximum},
        {maximum, 0, minimum},
        {0, 0, 0},
    }};
    for (const auto& next : targets) {
        const std::array<ground::Point, 3> route{unit.position, next, next};
        const auto projected = ground::cache_steering_target(unit, route);
        (void)check_flow_pair(
            unit,
            motion,
            next,
            projected.target.point,
            moving,
            FlowClearance::clear,
            projected.target.distance
        );
        (void)check_flow_pair(
            unit, motion, next, next, moving, FlowClearance::blocked, movement::Fixed{-7}
        );
    }
    // A supplied positive distance cannot bypass the ordinary wide-delta guard.
    unit.position = {0, 0, 0};
    for (const auto next : {ground::Point{minimum, 0, 0}, ground::Point{0, 0, minimum}}) {
        const auto decision = check_flow_pair(
            unit, motion, next, next, moving, FlowClearance::clear, 100 * fixed_one
        );
        OA_CHECK(decision.target == next && !decision.adjusted && !decision.mobile_blocked);
    }
    const ground::Point diagonal_limit{maximum, 0, maximum};
    const std::array<ground::Point, 3> limit_route{unit.position, diagonal_limit, diagonal_limit};
    const auto limit_cache = ground::cache_steering_target(unit, limit_route);
    OA_CHECK(limit_cache.target.distance < 0);
    const auto long_diagonal = check_flow_pair(
        unit,
        motion,
        diagonal_limit,
        diagonal_limit,
        moving,
        FlowClearance::clear,
        limit_cache.target.distance
    );
    OA_CHECK(!long_diagonal.adjusted && !long_diagonal.mobile_blocked);
    const ground::Point axis_limit{maximum, 0, 0};
    (void)check_flow_pair(
        unit, motion, axis_limit, axis_limit, moving, FlowClearance::clear, maximum
    );
    unit.position = {minimum, 0, maximum};
    const ground::Point across{maximum, 0, minimum};
    const auto refused = check_flow_pair(
        unit, motion, across, across, moving, FlowClearance::clear, 100 * fixed_one
    );
    OA_CHECK(!refused.adjusted && !refused.mobile_blocked);
}

/// Keeps real opposing and crossing traffic legal with the controller's projected scan.
void test_controller_probe_in_opposing_and_crossing_traffic() {
    constexpr uint32_t maximum_queries = 128;
    for (const auto kind : {CohortKind::opposing, CohortKind::crossing}) {
        const auto current = run_cohorts(true, true, kind, true);
        const auto control = run_cohorts(true, true, kind);
        OA_CHECK(current == run_cohorts(true, true, kind, true));
        OA_CHECK(current.arrived == (kind == CohortKind::opposing ? agent_count : crossing_count));
        OA_CHECK(current.last_arrival <= tick_limit && current.overlaps == 0);
        OA_CHECK(current.peak_queries <= maximum_queries);
        std::printf(
            "controller cohorts crossing %d: arrival %u / %u, vetoes %llu / %llu, "
            "orbit windows %llu / %llu, turn %llu / %llu\n",
            kind == CohortKind::crossing,
            current.last_arrival,
            control.last_arrival,
            static_cast<unsigned long long>(current.blocked),
            static_cast<unsigned long long>(control.blocked),
            static_cast<unsigned long long>(current.closed_orbit_windows),
            static_cast<unsigned long long>(control.closed_orbit_windows),
            static_cast<unsigned long long>(current.turn),
            static_cast<unsigned long long>(control.turn)
        );
    }
}
} // namespace

int main() {
    test_ground_steering_does_not_orbit_a_passed_head();
    test_waypoint_completion_geometry();
    test_sustained_opposing_cohorts();
    test_history_in_actual_opposing_and_crossing_steering();
    test_controller_probe_in_opposing_and_crossing_traffic();
    test_cached_ground_steering_matches_all_fields();
    test_cached_ground_steering_rechecks_changed_inputs();
    test_cached_local_flow_distance_matches_queries_and_decisions();
    return oa::test::check_exit_status();
}
