// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Mature stationary traffic may voluntarily take a clear passing leg without
// changing its route, native turning, acceleration or final occupancy veto.
#include "oa/base/game_math.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/ground_orders/steering_target.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
namespace movement = oa::sim::unit_movement;
namespace ground = oa::sim::ground_orders;

constexpr movement::Fixed pixel = 65536;
constexpr int16_t map_width = 96;
constexpr int16_t map_height = 64;
constexpr int16_t footprint = 2;
constexpr uint8_t ground_layer = 1;
constexpr uint16_t east = 49152;
constexpr uint16_t west = 16384;
constexpr movement::Fixed maximum_speed = 117964;
constexpr uint16_t maximum_turn = 1120;
constexpr movement::Fixed acceleration = 6553;
constexpr uint32_t tick_limit = 1800;
constexpr uint32_t recovery_tick = 59;
constexpr uint64_t maximum_queries = 128;

/// Holds full-sized footprints and tick-start clocks for a small traffic scene.
struct Scene : movement::Host {
    std::array<uint16_t, size_t(map_width) * map_height> occupants{};
    std::array<bool, size_t(map_width) * map_height> walls{};
    std::array<movement::Unit, 3> units{};
    std::array<uint32_t, 4> progress{};
    uint32_t tick{};
    uint32_t overlaps{};

    Scene() {
        add(1, 20, 32, east);
        add(2, 22, 32, west);
    }

    static size_t plot(int16_t x, int16_t z) { return size_t(z) * map_width + size_t(x); }

    void add(uint16_t id, int16_t x, int16_t z, uint16_t heading) {
        auto& unit = units[id - 1];
        unit.id = static_cast<int16_t>(id);
        unit.cell = {x, z};
        unit.footprint = {footprint, footprint};
        unit.position = {(x * 16 + footprint * 8) * pixel, 0, (z * 16 + footprint * 8) * pixel};
        unit.heading = heading;
        unit.flags = ground_layer;
        unit.type.maximum_speed = maximum_speed;
        unit.type.maximum_turn = maximum_turn;
        insert_occupancy(unit);
    }

    bool inside(std::array<int16_t, 2> cell, const movement::Unit& unit) const {
        return cell[0] >= 0 && cell[1] >= 0 && cell[0] + unit.footprint[0] <= map_width &&
               cell[1] + unit.footprint[1] <= map_height;
    }

    bool
    can_occupy(const movement::Unit& unit, std::array<int16_t, 2> cell, uint8_t layer) override {
        if (layer != ground_layer || !inside(cell, unit))
            return false;
        for (int16_t z = 0; z < unit.footprint[1]; ++z)
            for (int16_t x = 0; x < unit.footprint[0]; ++x) {
                const auto at = plot(cell[0] + x, cell[1] + z);
                if (walls[at] || (occupants[at] != 0 && occupants[at] != uint16_t(unit.id)))
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
        OA_CHECK(inside(unit.cell, unit));
        if (!inside(unit.cell, unit))
            return;
        for (int16_t z = 0; z < unit.footprint[1]; ++z)
            for (int16_t x = 0; x < unit.footprint[0]; ++x) {
                auto& occupant = occupants[plot(unit.cell[0] + x, unit.cell[1] + z)];
                if (occupant != 0 && occupant != uint16_t(unit.id))
                    ++overlaps;
                occupant = static_cast<uint16_t>(unit.id);
            }
    }

    void update_spatial_membership(movement::Unit&) override {}

    void relocate_blocker(int16_t x, int16_t z) {
        remove_occupancy(units[1]);
        add(2, x, z, units[1].heading);
    }

    movement::LocalFlowProbe probe() {
        return {
            this,
            [](void* context,
               const movement::Unit& moving,
               std::array<int16_t, 2> cell,
               uint8_t layer) {
                auto& scene = *static_cast<Scene*>(context);
                if (layer != ground_layer || !scene.inside(cell, moving))
                    return movement::LocalFlowBlocker{movement::LocalFlowObstacle::stationary};
                movement::LocalFlowBlocker selected{};
                for (int16_t z = 0; z < moving.footprint[1]; ++z)
                    for (int16_t x = 0; x < moving.footprint[0]; ++x) {
                        const auto at = plot(cell[0] + x, cell[1] + z);
                        if (scene.walls[at])
                            return movement::LocalFlowBlocker{
                                movement::LocalFlowObstacle::stationary
                            };
                        const auto id = scene.occupants[at];
                        if (id == 0 || id == uint16_t(moving.id))
                            continue;
                        const auto& other = scene.units[id - 1];
                        if (other.type.maximum_speed <= 0)
                            return movement::LocalFlowBlocker{
                                movement::LocalFlowObstacle::stationary
                            };
                        const auto age = scene.tick - scene.progress[id];
                        if (selected.obstacle != movement::LocalFlowObstacle::moving ||
                            age > selected.waiting_ticks ||
                            (age == selected.waiting_ticks && id < selected.unit_id))
                            selected = {
                                movement::LocalFlowObstacle::moving, id, other.heading, age
                            };
                    }
                return selected;
            },
            progress
        };
    }
};

/// Applies the owner's mature-phase gate to the immutable tick-start clock.
bool eligible(
    const movement::LocalFlowHead& head,
    const movement::Unit& unit,
    const movement::Movement& motion,
    uint32_t tick,
    const movement::LocalFlowProbe& probe
) {
    const auto id = static_cast<uint16_t>(unit.id);
    const auto last_progress = id < probe.progress_at_tick_start.size()
                                   ? probe.progress_at_tick_start[id]
                                   : motion.last_motion_tick;
    return movement::local_flow_recovery_due(head, tick, id) &&
           tick - last_progress >= movement::local_flow_retry_ticks;
}

movement::LocalFlowDecision choose(Scene& scene, movement::LocalFlowProbe probe) {
    movement::Movement motion{};
    motion.flags = ground_layer | movement::collision_blocked;
    movement::LocalFlowCounters counters{};
    const auto result = movement::choose_local_flow(
        scene.units[0], motion, {1200 * pixel, 0, 528 * pixel}, scene.tick, scene, probe, counters
    );
    OA_CHECK(counters.probes <= maximum_queries);
    return result;
}

/// Locates an observed target using the native footprint-origin quantisation.
std::array<int16_t, 2>
point_cell(const movement::Unit& unit, const std::array<movement::Fixed, 3>& point) {
    const auto coordinate = [](movement::Fixed value, int16_t width) {
        const auto biased = uint32_t(value) - uint32_t(width) * (8 * pixel) + 8 * pixel;
        return static_cast<int16_t>(std::bit_cast<movement::Fixed>(biased) >> 20);
    };
    return {coordinate(point[0], unit.footprint[0]), coordinate(point[2], unit.footprint[1])};
}

/// Counts unfiltered sixteen-pixel diagonal candidates that stay in the current origin.
uint32_t diagonal_candidates_in_current_origin(
    const movement::Unit& unit, const std::array<movement::Fixed, 3>& target
) {
    const int64_t dx = int64_t(target[0]) - unit.position[0];
    const int64_t dz = int64_t(target[2]) - unit.position[2];
    if (dx <= std::numeric_limits<movement::Fixed>::min() ||
        dx > std::numeric_limits<movement::Fixed>::max() ||
        dz <= std::numeric_limits<movement::Fixed>::min() ||
        dz > std::numeric_limits<movement::Fixed>::max())
        return 0;
    const auto heading = oa::base::game_math::direction(
        -static_cast<movement::Fixed>(dx), -static_cast<movement::Fixed>(dz)
    );
    uint32_t count = 0;
    for (const int angle : {-8192, 8192}) {
        const auto velocity = movement::aim_velocity(uint16_t(heading + angle), 0, 16 * pixel);
        const auto x = int64_t(unit.position[0]) + velocity[0];
        const auto z = int64_t(unit.position[2]) + velocity[2];
        if (x < std::numeric_limits<movement::Fixed>::min() ||
            x > std::numeric_limits<movement::Fixed>::max() ||
            z < std::numeric_limits<movement::Fixed>::min() ||
            z > std::numeric_limits<movement::Fixed>::max())
            continue;
        const std::array<movement::Fixed, 3> candidate{
            static_cast<movement::Fixed>(x), unit.position[1], static_cast<movement::Fixed>(z)
        };
        if (point_cell(unit, candidate) == unit.cell)
            ++count;
    }
    return count;
}

/// Leaves ordinary priority intact and permits a voluntary step at the mature phase.
void test_phase_age_and_priority() {
    Scene scene;
    scene.tick = recovery_tick;
    movement::Movement motion{};
    motion.flags = ground_layer | movement::collision_blocked;
    movement::LocalFlowHead head{};
    head.blocked_ticks = movement::local_flow_retry_ticks + 1;
    head.mobile_blocked = true;
    auto probe = scene.probe();
    OA_CHECK(!probe.recovery_yield);
    const auto ordinary = choose(scene, probe);
    OA_CHECK(ordinary.mobile_blocked && !ordinary.adjusted);
    probe.recovery_yield = eligible(head, scene.units[0], motion, scene.tick, probe);
    OA_CHECK(probe.recovery_yield);
    const auto emergency = choose(scene, probe);
    OA_CHECK(emergency.mobile_blocked && emergency.adjusted);
    OA_CHECK(scene.can_occupy(scene.units[0], scene.units[0].cell, ground_layer));
    OA_CHECK(head.blocked_ticks == movement::local_flow_retry_ticks + 1);
    OA_CHECK(!movement::local_flow_defer_replan(head, motion));

    OA_CHECK(!eligible(head, scene.units[0], motion, recovery_tick + 1, probe));
    head.blocked_ticks = movement::local_flow_retry_ticks - 1;
    OA_CHECK(!eligible(head, scene.units[0], motion, recovery_tick, probe));
    head.blocked_ticks = movement::local_flow_retry_ticks + 1;
    scene.progress[1] = recovery_tick - movement::local_flow_retry_ticks + 1;
    OA_CHECK(!eligible(head, scene.units[0], motion, recovery_tick, probe));
    scene.progress[1] = 0;
    motion.last_motion_tick = recovery_tick;
    OA_CHECK(eligible(head, scene.units[0], motion, recovery_tick, probe));
    OA_CHECK(scene.overlaps == 0);
}

/// Retains the ordinary coherent-traffic rule until a qualified stationary retry.
void test_coherent_traffic_can_voluntarily_yield() {
    Scene scene;
    scene.tick = recovery_tick;
    scene.units[1].heading = east;
    scene.progress[2] = scene.tick - 1;
    auto probe = scene.probe();
    OA_CHECK(!choose(scene, probe).adjusted);
    probe.recovery_yield = true;
    OA_CHECK(choose(scene, probe).adjusted);
    OA_CHECK(scene.overlaps == 0);
}

/// Keeps static terrain, occupied targets and diagonal adjoining cells authoritative.
void test_clearance_vetoes_remain_authoritative() {
    {
        Scene scene;
        scene.tick = recovery_tick;
        scene.units[1].type.maximum_speed = 0;
        auto probe = scene.probe();
        probe.recovery_yield = true;
        const auto result = choose(scene, probe);
        OA_CHECK(!result.adjusted && !result.mobile_blocked);
    }
    {
        Scene scene;
        scene.tick = recovery_tick;
        for (const int16_t x : {int16_t{20}, int16_t{21}}) {
            scene.walls[Scene::plot(x, 31)] = true;
            scene.walls[Scene::plot(x, 34)] = true;
        }
        scene.walls[Scene::plot(19, 32)] = scene.walls[Scene::plot(19, 33)] = true;
        auto probe = scene.probe();
        probe.recovery_yield = true;
        const auto result = choose(scene, probe);
        OA_CHECK(!result.adjusted && result.mobile_blocked);
    }
    for (const bool occupied : {false, true}) {
        Scene scene;
        scene.tick = recovery_tick;
        scene.relocate_blocker(24, 32);
        if (occupied)
            scene.add(3, 22, 34, west);
        else
            scene.walls[Scene::plot(20, 34)] = true;
        auto probe = scene.probe();
        probe.recovery_yield = true;
        const auto result = choose(scene, probe);
        OA_CHECK(result.adjusted && result.mobile_blocked);
        OA_CHECK(result.target[0] > scene.units[0].position[0]);
        OA_CHECK(result.target[2] < scene.units[0].position[2]);
        OA_CHECK(scene.overlaps == 0);
    }
}

struct EscapeResult {
    uint32_t arrival{};
    uint32_t first_emergency{};
    uint32_t first_emergency_head_tick{};
    uint32_t emergency_ticks{};
    uint32_t full_retry_ticks{};
    uint32_t overlaps{};
    std::array<int16_t, 2> first_origin{};
    std::array<int16_t, 2> first_raw_target_origin{};
    std::array<int16_t, 2> first_target_origin{};
    uint32_t first_origin_change_tick{};
    uint32_t first_head_complete_tick{};
    uint32_t first_head_expiry_tick{};
    uint32_t same_origin_diagonal_candidates{};
    uint32_t emergency_same_origin_diagonal_candidates{};
    uint32_t same_origin_selected_targets{};
    uint32_t emergency_same_origin_selected_targets{};
    bool completed_before_origin_change{};
    bool first_complete_by_radius{};
    bool first_complete_by_plane{};
    uint64_t blocked{};
    uint64_t adjustments{};
    uint64_t digest{14695981039346656037ull};
    bool operator==(const EscapeResult&) const = default;
};

/// Lets ordinary native motion complete the original route after one voluntary escape.
EscapeResult run_escape(bool emergency_enabled) {
    Scene scene;
    auto& unit = scene.units[0];
    unit.position[0] += 8 * pixel - 1;
    OA_CHECK(scene.can_occupy(unit, unit.cell, ground_layer));
    movement::Movement motion{};
    motion.flags = ground_layer;
    movement::LocalFlowHead head{};
    movement::LocalFlowCounters counters{};
    ground::Navigation route{};
    route.flags = ground::route_present_flag | ground::search_pending_flag;
    route.count = 3;
    route.points = {};
    route.points[0] = {int16_t(unit.position[0] >> 16), 528};
    route.points[1] = route.points[2] = {1200, 528};
    EscapeResult result{};
    bool tracking_first_head = false;
    for (scene.tick = 1; scene.tick <= tick_limit; ++scene.tick) {
        scene.progress[1] = motion.last_motion_tick;
        const bool recovery =
            movement::local_flow_recovery_due(head, scene.tick, uint16_t(unit.id));
        const bool reached = head.active && movement::local_flow_head_reached(unit, head);
        const bool previous = movement::local_flow_completed_last_tick(head, scene.tick);
        if (tracking_first_head && reached) {
            result.first_head_complete_tick = scene.tick;
            result.completed_before_origin_change = result.first_origin_change_tick == 0;
            const auto dx = int64_t(unit.position[0]) - int64_t(head.points[1][0]) * pixel;
            const auto dz = int64_t(unit.position[2]) - int64_t(head.points[1][1]) * pixel;
            constexpr int64_t radius = 5 * pixel;
            result.first_complete_by_radius = dx > -radius && dx < radius && dz > -radius &&
                                              dz < radius && dx * dx + dz * dz < radius * radius;
            const auto leg_x = int64_t(head.points[1][0]) - head.points[0][0];
            const auto leg_z = int64_t(head.points[1][1]) - head.points[0][1];
            result.first_complete_by_plane =
                (leg_x != 0 || leg_z != 0) && dx * leg_x + dz * leg_z >= 0;
            tracking_first_head = false;
        } else if (tracking_first_head && head.active && recovery) {
            result.first_head_expiry_tick = scene.tick;
            tracking_first_head = false;
        }
        if (ground::search_ready(route, scene.tick))
            ++result.full_retry_ticks;
        if (head.blocked_ticks >= movement::local_flow_retry_ticks)
            OA_CHECK(!movement::local_flow_defer_replan(head, motion));
        if (reached) {
            head.active = false;
            head.count = movement::local_flow_completed_count;
            head.tick = scene.tick;
        } else if (head.active && recovery) {
            head.active = false;
            head.count = 0;
        }
        if (!head.active && (head.blocked_ticks < movement::local_flow_retry_ticks || recovery)) {
            auto probe = scene.probe();
            probe.previous_head = reached || previous ? &head : nullptr;
            probe.allow_reverse = recovery;
            probe.recovery_yield =
                emergency_enabled && eligible(head, unit, motion, scene.tick, probe);
            if (probe.recovery_yield) {
                if (result.first_emergency == 0)
                    result.first_emergency = scene.tick;
                ++result.emergency_ticks;
            }
            const auto before_queries = counters.probes;
            const auto before_speed = motion.speed;
            const auto before_heading = unit.heading;
            const auto before_turn = motion.turn;
            const auto before_flags = motion.flags;
            const auto before_clock = route.last_search_tick;
            const auto points = ground::steering_points(route);
            const auto same_origin_candidates =
                diagonal_candidates_in_current_origin(unit, points[1]);
            result.same_origin_diagonal_candidates += same_origin_candidates;
            if (probe.recovery_yield)
                result.emergency_same_origin_diagonal_candidates += same_origin_candidates;
            const auto decision = movement::choose_local_flow(
                unit,
                motion,
                points[1],
                ground::steering_target(unit, points).point,
                scene.tick,
                scene,
                probe,
                counters
            );
            OA_CHECK(counters.probes - before_queries <= maximum_queries);
            OA_CHECK(motion.speed == before_speed && unit.heading == before_heading);
            OA_CHECK(motion.turn == before_turn && motion.flags == before_flags);
            OA_CHECK(route.last_search_tick == before_clock);
            head.mobile_blocked = decision.mobile_blocked;
            if (decision.adjusted) {
                if (point_cell(unit, decision.target) == unit.cell) {
                    ++result.same_origin_selected_targets;
                    if (probe.recovery_yield)
                        ++result.emergency_same_origin_selected_targets;
                }
                head.points = {
                    ground::RoutePoint{
                        int16_t(unit.position[0] >> 16), int16_t(unit.position[2] >> 16)
                    },
                    ground::RoutePoint{
                        int16_t(decision.target[0] >> 16), int16_t(decision.target[2] >> 16)
                    },
                    route.points[2]
                };
                head.count = 3;
                head.active = true;
                if (probe.recovery_yield && result.first_origin == std::array<int16_t, 2>{}) {
                    result.first_emergency_head_tick = scene.tick;
                    result.first_origin = unit.cell;
                    result.first_raw_target_origin = point_cell(unit, decision.target);
                    result.first_target_origin = point_cell(
                        unit,
                        {movement::Fixed(head.points[1][0]) * pixel,
                         0,
                         movement::Fixed(head.points[1][1]) * pixel}
                    );
                    tracking_first_head = true;
                }
            }
        }
        if (head.active)
            head.tick = scene.tick;
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
        OA_CHECK(motion.speed >= 0 && motion.speed <= maximum_speed);
        OA_CHECK(std::abs(int(motion.turn)) <= maximum_turn);
        movement::integrate_local_flow(unit, motion, scene.tick, true, scene, counters);
        movement::record_local_flow_block(head, (motion.flags & movement::collision_blocked) != 0);
        if (result.first_emergency_head_tick != 0 && result.first_origin_change_tick == 0 &&
            unit.cell != result.first_origin)
            result.first_origin_change_tick = scene.tick;
        OA_CHECK(scene.can_occupy(unit, unit.cell, ground_layer));
        OA_CHECK(route.points[1] == (ground::RoutePoint{1200, 528}));
        for (const auto value :
             {uint32_t(unit.position[0]),
              uint32_t(unit.position[2]),
              uint32_t(unit.heading),
              uint32_t(motion.speed),
              motion.last_motion_tick}) {
            result.digest ^= value;
            result.digest *= 1099511628211ull;
        }
        if (oa::base::game_math::distance(
                unit.position[0] - 1200 * pixel, unit.position[2] - 528 * pixel
            ) < uint32_t(16 * pixel)) {
            result.arrival = scene.tick;
            break;
        }
    }
    result.overlaps = scene.overlaps;
    result.blocked = counters.blocked_ticks;
    result.adjustments = counters.steering_adjustments;
    return result;
}

void test_native_motion_completes_after_recovery_yield() {
    const auto ordinary = run_escape(false);
    const auto escaped = run_escape(true);
    OA_CHECK(ordinary == run_escape(false));
    OA_CHECK(escaped == run_escape(true));
    OA_CHECK(ordinary.arrival == 0 && ordinary.adjustments == 0);
    OA_CHECK(escaped.arrival != 0 && escaped.arrival < tick_limit / 2);
    OA_CHECK(escaped.first_emergency >= movement::local_flow_retry_ticks);
    OA_CHECK((escaped.first_emergency + 1) % movement::local_flow_retry_ticks == 0);
    OA_CHECK(escaped.emergency_ticks != 0 && escaped.adjustments != 0);
    OA_CHECK(ordinary.overlaps == 0 && escaped.overlaps == 0);
    OA_CHECK(escaped.full_retry_ticks != 0 && ordinary.full_retry_ticks != 0);
    std::printf(
        "recovery yield: arrival %u / %u, vetoes %llu / %llu, emergency %u, retries %u / %u\n",
        escaped.arrival,
        ordinary.arrival,
        static_cast<unsigned long long>(escaped.blocked),
        static_cast<unsigned long long>(ordinary.blocked),
        escaped.emergency_ticks,
        escaped.full_retry_ticks,
        ordinary.full_retry_ticks
    );
    std::printf(
        "first emergency: phase %u, head started %u, origin %d,%d, raw target %d,%d, stored target "
        "%d,%d, "
        "origin changed %u, completed %u, expired %u, completed before change %d, radius %d, plane "
        "%d; "
        "same-origin diagonal candidates %u/%u emergency, selected %u/%u emergency\n",
        escaped.first_emergency,
        escaped.first_emergency_head_tick,
        escaped.first_origin[0],
        escaped.first_origin[1],
        escaped.first_raw_target_origin[0],
        escaped.first_raw_target_origin[1],
        escaped.first_target_origin[0],
        escaped.first_target_origin[1],
        escaped.first_origin_change_tick,
        escaped.first_head_complete_tick,
        escaped.first_head_expiry_tick,
        escaped.completed_before_origin_change,
        escaped.first_complete_by_radius,
        escaped.first_complete_by_plane,
        escaped.same_origin_diagonal_candidates,
        escaped.emergency_same_origin_diagonal_candidates,
        escaped.same_origin_selected_targets,
        escaped.emergency_same_origin_selected_targets
    );
}
} // namespace

int main() {
    test_phase_age_and_priority();
    test_coherent_traffic_can_voluntarily_yield();
    test_clearance_vetoes_remain_authoritative();
    test_native_motion_completes_after_recovery_yield();
    return oa::test::check_exit_status();
}
