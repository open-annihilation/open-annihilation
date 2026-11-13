// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/unit_movement/local_flow.hpp"
#include "oa/base/game_math.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>

using namespace oa::sim::unit_movement;

namespace {
constexpr Fixed fixed_one = 65536;
constexpr Fixed cell_width = 16 * fixed_one;
constexpr Fixed arrival_radius = 5 * fixed_one;
constexpr uint16_t east = 49152;
constexpr uint16_t west = 16384;
constexpr int16_t map_width = 32;
constexpr int16_t map_height = 24;
constexpr std::size_t maximum_agents = 8;
constexpr uint32_t battle_tick_limit = 1200;
constexpr uint64_t maximum_queries_per_choice = 128;

struct Agent {
    Unit unit{};
    Movement movement{};
    std::array<Fixed, 3> destination{};
    std::array<Fixed, 3> detour{};
    bool has_detour{};
    bool travelling{true};
    uint32_t arrival_tick{};
};

struct Grid : Host {
    std::array<int16_t, std::size_t(map_width) * map_height> occupants{};
    std::array<bool, std::size_t(map_width) * map_height> walls{};
    std::array<Agent, maximum_agents> agents{};
    std::array<uint32_t, maximum_agents + 1> progress_at_tick_start{};
    std::size_t count{};
    uint32_t tick{};
    unsigned overlap_count{};
    bool progress_snapshot_enabled{};

    void snapshot_progress() {
        for (std::size_t i = 0; i < count; ++i)
            progress_at_tick_start[static_cast<uint16_t>(agents[i].unit.id)] =
                agents[i].movement.last_motion_tick;
        progress_snapshot_enabled = true;
    }

    static std::size_t index(int16_t x, int16_t z) {
        return std::size_t(z) * map_width + std::size_t(x);
    }

    bool inside(std::array<int16_t, 2> cell, std::array<int16_t, 2> footprint) const {
        return cell[0] >= 0 && cell[1] >= 0 && cell[0] + footprint[0] <= map_width &&
               cell[1] + footprint[1] <= map_height;
    }

    Agent&
    add(int16_t x,
        int16_t z,
        uint16_t heading,
        int16_t goal_x,
        int16_t goal_z,
        std::array<int16_t, 2> footprint = {1, 1}) {
        if (count == agents.size())
            std::abort();
        auto& agent = agents[count++];
        auto& unit = agent.unit;
        unit.id = static_cast<int16_t>(count);
        unit.cell = {x, z};
        unit.footprint = footprint;
        unit.position = {
            (x * 16 + footprint[0] * 8) * fixed_one, 0, (z * 16 + footprint[1] * 8) * fixed_one
        };
        unit.heading = heading;
        unit.flags = 1;
        unit.type.maximum_speed = 2 * fixed_one;
        unit.type.maximum_turn = 4096;
        agent.movement.flags = 1;
        agent.destination = {
            (goal_x * 16 + footprint[0] * 8) * fixed_one,
            0,
            (goal_z * 16 + footprint[1] * 8) * fixed_one
        };
        insert_occupancy(unit);
        return agent;
    }

    bool can_occupy(const Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy) override {
        if (occupancy != 1 || !inside(cell, unit.footprint))
            return false;
        for (int16_t z = cell[1]; z < cell[1] + unit.footprint[1]; ++z)
            for (int16_t x = cell[0]; x < cell[0] + unit.footprint[0]; ++x) {
                const auto plot = index(x, z);
                if (walls[plot] || (occupants[plot] != 0 && occupants[plot] != unit.id))
                    return false;
            }
        return true;
    }

    void remove_occupancy(Unit& unit) override {
        for (int16_t z = unit.cell[1]; z < unit.cell[1] + unit.footprint[1]; ++z)
            for (int16_t x = unit.cell[0]; x < unit.cell[0] + unit.footprint[0]; ++x) {
                const auto plot = index(x, z);
                if (occupants[plot] != unit.id)
                    ++overlap_count;
                occupants[plot] = 0;
            }
    }

    void insert_occupancy(Unit& unit) override {
        if (!inside(unit.cell, unit.footprint)) {
            ++overlap_count;
            return;
        }
        for (int16_t z = unit.cell[1]; z < unit.cell[1] + unit.footprint[1]; ++z)
            for (int16_t x = unit.cell[0]; x < unit.cell[0] + unit.footprint[0]; ++x) {
                const auto plot = index(x, z);
                if (walls[plot] || (occupants[plot] != 0 && occupants[plot] != unit.id))
                    ++overlap_count;
                occupants[plot] = unit.id;
            }
    }

    void update_spatial_membership(Unit&) override {}

    LocalFlowBlocker probe(const Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy) {
        if (occupancy != 1 || !inside(cell, unit.footprint))
            return {LocalFlowObstacle::stationary};
        LocalFlowBlocker selected;
        for (int16_t z = cell[1]; z < cell[1] + unit.footprint[1]; ++z)
            for (int16_t x = cell[0]; x < cell[0] + unit.footprint[0]; ++x) {
                const auto plot = index(x, z);
                if (walls[plot])
                    return {LocalFlowObstacle::stationary};
                const auto id = occupants[plot];
                if (id == 0 || id == unit.id)
                    continue;
                const auto& other = agents[std::size_t(id - 1)];
                if (!other.travelling)
                    return {LocalFlowObstacle::stationary};
                const auto last_motion_tick = progress_snapshot_enabled
                                                  ? progress_at_tick_start[std::size_t(id)]
                                                  : other.movement.last_motion_tick;
                const auto waiting_ticks = tick - last_motion_tick;
                if (selected.obstacle == LocalFlowObstacle::clear ||
                    waiting_ticks > selected.waiting_ticks ||
                    (waiting_ticks == selected.waiting_ticks && id < selected.unit_id))
                    selected = {
                        LocalFlowObstacle::moving,
                        static_cast<uint16_t>(id),
                        other.unit.heading,
                        waiting_ticks
                    };
            }
        return selected;
    }

    LocalFlowProbe query() {
        return {
            this,
            [](void* context, const Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy) {
                return static_cast<Grid*>(context)->probe(unit, cell, occupancy);
            },
            progress_snapshot_enabled ? std::span<const uint32_t>(progress_at_tick_start)
                                      : std::span<const uint32_t>{}
        };
    }
};

Fixed distance(std::array<Fixed, 3> from, std::array<Fixed, 3> to) {
    return oa::base::game_math::distance(to[0] - from[0], to[2] - from[2]);
}

void test_sequential_progress_preserves_tick_start_priority() {
    for (const uint32_t earlier_progress_tick : {uint32_t{19}, uint32_t{5}}) {
        Grid grid;
        auto& earlier = grid.add(8, 6, west, 2, 6);
        auto& later = grid.add(6, 6, east, 14, 6);
        grid.tick = 20;
        earlier.movement.last_motion_tick = earlier_progress_tick;
        later.movement.last_motion_tick = grid.tick - 1;
        grid.snapshot_progress();
        LocalFlowCounters counters{};
        const auto earlier_decision = choose_local_flow(
            earlier.unit,
            earlier.movement,
            earlier.destination,
            grid.tick,
            grid,
            grid.query(),
            counters
        );
        OA_CHECK(earlier_decision.mobile_blocked);
        OA_CHECK(!earlier_decision.adjusted);
        const auto before_integration = choose_local_flow(
            later.unit, later.movement, later.destination, grid.tick, grid, grid.query(), counters
        );
        OA_CHECK(before_integration.mobile_blocked);
        OA_CHECK(before_integration.adjusted);

        // The lower id moves first in the owner update. Its live clock now
        // reports this tick even when it had waited much longer at tick start.
        earlier.movement.speed = fixed_one;
        earlier.movement.velocity = {-fixed_one, 0, 0};
        integrate_local_flow(earlier.unit, earlier.movement, grid.tick, true, grid, counters);
        OA_CHECK(earlier.movement.last_motion_tick == grid.tick);
        const auto after_integration = choose_local_flow(
            later.unit, later.movement, later.destination, grid.tick, grid, grid.query(), counters
        );
        OA_CHECK(after_integration.mobile_blocked);
        OA_CHECK(after_integration.adjusted);
        OA_CHECK(after_integration.target == before_integration.target);
        OA_CHECK(grid.overlap_count == 0);
    }
}

void test_older_higher_id_keeps_priority_after_moving() {
    Grid grid;
    auto& lower_id = grid.add(6, 6, east, 14, 6);
    auto& older_waiter = grid.add(8, 6, west, 2, 6);
    grid.tick = 20;
    lower_id.movement.last_motion_tick = grid.tick - 1;
    older_waiter.movement.last_motion_tick = 5;
    grid.snapshot_progress();
    LocalFlowCounters counters{};
    const auto before_integration = choose_local_flow(
        lower_id.unit,
        lower_id.movement,
        lower_id.destination,
        grid.tick,
        grid,
        grid.query(),
        counters
    );
    OA_CHECK(before_integration.mobile_blocked);
    OA_CHECK(before_integration.adjusted);
    older_waiter.movement.speed = fixed_one;
    older_waiter.movement.velocity = {-fixed_one, 0, 0};
    integrate_local_flow(older_waiter.unit, older_waiter.movement, grid.tick, true, grid, counters);
    OA_CHECK(older_waiter.movement.last_motion_tick == grid.tick);
    const auto after_integration = choose_local_flow(
        lower_id.unit,
        lower_id.movement,
        lower_id.destination,
        grid.tick,
        grid,
        grid.query(),
        counters
    );
    OA_CHECK(after_integration.mobile_blocked);
    OA_CHECK(after_integration.adjusted);
    OA_CHECK(after_integration.target == before_integration.target);
    // The snapshot also keeps the older unit's own priority after it moved.
    const auto older_decision = choose_local_flow(
        older_waiter.unit,
        older_waiter.movement,
        older_waiter.destination,
        grid.tick,
        grid,
        grid.query(),
        counters
    );
    OA_CHECK(older_decision.mobile_blocked);
    OA_CHECK(!older_decision.adjusted);
    OA_CHECK(grid.overlap_count == 0);
}

void test_priority_and_stationary_obstacles() {
    Grid grid;
    auto& first = grid.add(8, 6, west, 2, 6);
    auto& second = grid.add(6, 6, east, 14, 6);
    grid.tick = 20;
    first.movement.last_motion_tick = grid.tick;
    second.movement.last_motion_tick = grid.tick;
    LocalFlowCounters counters;
    auto decision = choose_local_flow(
        second.unit, second.movement, second.destination, grid.tick, grid, grid.query(), counters
    );
    OA_CHECK(decision.mobile_blocked);
    OA_CHECK(decision.adjusted);
    OA_CHECK(counters.yielded_ticks == 1);
    OA_CHECK(counters.probes <= maximum_queries_per_choice);
    const auto repeated = choose_local_flow(
        second.unit, second.movement, second.destination, grid.tick, grid, grid.query(), counters
    );
    OA_CHECK(repeated.target == decision.target);
    second.movement.last_motion_tick = 10;
    decision = choose_local_flow(
        second.unit, second.movement, second.destination, grid.tick, grid, grid.query(), counters
    );
    OA_CHECK(decision.mobile_blocked);
    OA_CHECK(!decision.adjusted);
    second.movement.last_motion_tick = grid.tick;
    first.travelling = false;
    decision = choose_local_flow(
        second.unit, second.movement, second.destination, grid.tick, grid, grid.query(), counters
    );
    OA_CHECK(!decision.mobile_blocked);
    OA_CHECK(!decision.adjusted);
    first.travelling = true;
    first.unit.heading = east;
    decision = choose_local_flow(
        second.unit, second.movement, second.destination, grid.tick, grid, grid.query(), counters
    );
    OA_CHECK(decision.mobile_blocked);
    OA_CHECK(!decision.adjusted);
}

void test_units_turning_together_keep_their_route() {
    Grid grid;
    auto& blocker = grid.add(8, 6, 0, 14, 6);
    auto& following = grid.add(6, 6, 0, 14, 6);
    grid.tick = 20;
    blocker.movement.last_motion_tick = grid.tick - 1;
    following.movement.last_motion_tick = grid.tick - 1;
    grid.snapshot_progress();
    LocalFlowCounters counters{};
    const auto coherent_turn = choose_local_flow(
        following.unit,
        following.movement,
        following.destination,
        grid.tick,
        grid,
        grid.query(),
        counters
    );
    // Both units face north while turning onto the same eastbound route.
    OA_CHECK(coherent_turn.mobile_blocked);
    OA_CHECK(!coherent_turn.adjusted);
    OA_CHECK(coherent_turn.target == following.destination);
    OA_CHECK(counters.yielded_ticks == 0);
    OA_CHECK(counters.steering_adjustments == 0);

    blocker.unit.heading = 32768;
    const auto opposing_turn = choose_local_flow(
        following.unit,
        following.movement,
        following.destination,
        grid.tick,
        grid,
        grid.query(),
        counters
    );
    OA_CHECK(opposing_turn.mobile_blocked);
    OA_CHECK(opposing_turn.adjusted);
    OA_CHECK(counters.yielded_ticks == 1);
    // Yield forward and right of the eastbound route, even while facing north.
    OA_CHECK(opposing_turn.target[0] > following.unit.position[0]);
    OA_CHECK(opposing_turn.target[2] > following.unit.position[2]);

    blocker.unit.heading = following.unit.heading;
    blocker.movement.last_motion_tick = grid.tick - 5;
    grid.snapshot_progress();
    const auto stalled_leader = choose_local_flow(
        following.unit,
        following.movement,
        following.destination,
        grid.tick,
        grid,
        grid.query(),
        counters
    );
    OA_CHECK(stalled_leader.mobile_blocked);
    OA_CHECK(stalled_leader.adjusted);
    OA_CHECK(counters.yielded_ticks == 2);
    OA_CHECK(grid.overlap_count == 0);
}

void test_verified_queue_keeps_native_target_until_recovery() {
    Grid grid;
    auto& blocker = grid.add(8, 6, 0, 14, 6);
    auto& follower = grid.add(6, 6, 0, 14, 6);
    grid.tick = 20;
    blocker.movement.last_motion_tick = grid.tick - 5;
    follower.movement.last_motion_tick = grid.tick - 1;
    grid.snapshot_progress();
    OA_CHECK(!LocalFlowBlocker{}.same_route_queue);
    auto verified = grid.query();
    verified.at =
        [](void* context, const Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy) {
            auto result = static_cast<Grid*>(context)->probe(unit, cell, occupancy);
            result.same_route_queue = result.obstacle == LocalFlowObstacle::moving;
            return result;
        };
    LocalFlowCounters original_counters{}, queue_counters{};
    const auto original = choose_local_flow(
        follower.unit,
        follower.movement,
        follower.destination,
        grid.tick,
        grid,
        grid.query(),
        original_counters
    );
    const auto queued = choose_local_flow(
        follower.unit,
        follower.movement,
        follower.destination,
        grid.tick,
        grid,
        verified,
        queue_counters
    );
    OA_CHECK(original.mobile_blocked && original.adjusted);
    OA_CHECK(queued.mobile_blocked && !queued.adjusted && !queued.reverse);
    OA_CHECK(queued.target == follower.destination);
    OA_CHECK(queue_counters.yielded_ticks == 0 && queue_counters.steering_adjustments == 0);
    OA_CHECK(queue_counters.probes < original_counters.probes);
    verified.recovery_yield = true;
    auto recovery = grid.query();
    recovery.recovery_yield = true;
    LocalFlowCounters verified_recovery_counters{}, recovery_counters{};
    const auto checked = choose_local_flow(
        follower.unit,
        follower.movement,
        follower.destination,
        grid.tick,
        grid,
        verified,
        verified_recovery_counters
    );
    const auto ordinary = choose_local_flow(
        follower.unit,
        follower.movement,
        follower.destination,
        grid.tick,
        grid,
        recovery,
        recovery_counters
    );
    OA_CHECK(checked.adjusted && checked.target == ordinary.target);
    OA_CHECK(
        checked.reverse == ordinary.reverse && checked.mobile_blocked == ordinary.mobile_blocked
    );
    OA_CHECK(verified_recovery_counters.probes == recovery_counters.probes);
    OA_CHECK(verified_recovery_counters.yielded_ticks == recovery_counters.yielded_ticks);
    OA_CHECK(
        verified_recovery_counters.steering_adjustments == recovery_counters.steering_adjustments
    );
    OA_CHECK(grid.overlap_count == 0);
}

void test_diagonal_corner_clearance() {
    Grid grid;
    grid.add(8, 6, west, 2, 6);
    auto& yielding = grid.add(6, 6, east, 14, 6);
    // The forward-right diagonal is empty, but its adjoining south cell is a wall.
    grid.walls[Grid::index(6, 7)] = true;
    LocalFlowCounters counters;
    const auto decision = choose_local_flow(
        yielding.unit, yielding.movement, yielding.destination, 0, grid, grid.query(), counters
    );
    OA_CHECK(decision.adjusted);
    OA_CHECK(decision.target[2] < yielding.unit.position[2]);
    OA_CHECK(counters.probes <= maximum_queries_per_choice);
}

void test_collision_veto_and_last_progress() {
    Grid grid;
    auto& agent = grid.add(4, 4, east, 10, 4);
    grid.walls[Grid::index(5, 4)] = true;
    agent.unit.position[0] = 5 * cell_width - 1;
    agent.movement.last_motion_tick = 4;
    agent.movement.speed = fixed_one;
    agent.movement.velocity = {fixed_one, 0, 0};
    LocalFlowCounters counters;
    const auto before = agent.unit.position;
    integrate_local_flow(agent.unit, agent.movement, 10, true, grid, counters);
    OA_CHECK(agent.unit.position == before);
    OA_CHECK(agent.movement.last_motion_tick == 4);
    OA_CHECK((agent.movement.flags & collision_blocked) != 0);
    OA_CHECK(counters.blocked_ticks == 1);
    integrate_local_flow(agent.unit, agent.movement, 34, true, grid, counters);
    OA_CHECK((agent.movement.flags & collision_blocked) != 0);
    OA_CHECK(counters.blocked_ticks == 2);
    OA_CHECK(grid.overlap_count == 0);
    agent.movement.flags = 1;
    integrate_local_flow(agent.unit, agent.movement, 35, true, grid, counters);
    OA_CHECK((agent.movement.flags & collision_blocked) != 0);
}

void test_blocked_boundary_creep_cannot_defer_forever() {
    Grid grid;
    auto& agent = grid.add(4, 4, east, 10, 4);
    grid.walls[Grid::index(5, 4)] = true;
    agent.unit.position[0] = 5 * cell_width - 1;
    agent.movement.speed = fixed_one;
    // Each veto clamps X at the occupied boundary while Z still advances by
    // one fixed-point fraction. The last-motion clock therefore keeps moving.
    agent.movement.velocity = {fixed_one, 0, 1};
    LocalFlowHead head{};
    head.mobile_blocked = true;
    LocalFlowCounters counters{};
    constexpr uint32_t retry_tick = 30;
    constexpr uint8_t saturated_blocked_ticks = 31;
    for (uint32_t tick = 1; tick <= 90; ++tick) {
        const auto before = agent.unit.position;
        integrate_local_flow(agent.unit, agent.movement, tick, true, grid, counters);
        OA_CHECK(agent.unit.position[0] == before[0]);
        OA_CHECK(agent.unit.position[2] == before[2] + 1);
        OA_CHECK(agent.movement.last_motion_tick == tick);
        const bool blocked = (agent.movement.flags & collision_blocked) != 0;
        OA_CHECK(blocked);
        record_local_flow_block(head, blocked);
        OA_CHECK(local_flow_defer_replan(head, agent.movement) == (tick < retry_tick));
        OA_CHECK(head.blocked_ticks == std::min<uint32_t>(tick, saturated_blocked_ticks));
    }
    OA_CHECK(head.blocked_ticks == saturated_blocked_ticks);
    OA_CHECK(counters.blocked_ticks == 90);
    OA_CHECK(grid.overlap_count == 0);
    grid.walls[Grid::index(5, 4)] = false;
    integrate_local_flow(agent.unit, agent.movement, 91, true, grid, counters);
    OA_CHECK(agent.unit.cell[0] == 5);
    const bool blocked = (agent.movement.flags & collision_blocked) != 0;
    OA_CHECK(!blocked);
    record_local_flow_block(head, blocked);
    OA_CHECK(head.blocked_ticks == 0);
    OA_CHECK(!local_flow_defer_replan(head, agent.movement));
    OA_CHECK(grid.overlap_count == 0);
}

struct BattleResult {
    uint64_t blocked{};
    uint64_t deferred{};
    uint64_t adjustments{};
    uint64_t probes{};
    uint32_t arrived{};
    uint32_t last_arrival{};
    uint64_t digest{};
    unsigned overlaps{};
    bool operator==(const BattleResult&) const = default;
};

enum class Traffic { counterflow, crossing, choke, mixed_footprints };

BattleResult run_traffic(Traffic traffic, bool enhanced) {
    Grid grid;
    if (traffic == Traffic::crossing) {
        grid.add(4, 11, east, 24, 11);
        grid.add(24, 11, west, 4, 11);
        grid.add(14, 1, 32768, 14, 21);
        grid.add(14, 21, 0, 14, 1);
    } else if (traffic == Traffic::mixed_footprints) {
        grid.add(4, 11, east, 24, 11, {2, 2});
        grid.add(24, 11, west, 4, 11);
    } else {
        grid.add(4, 11, east, 24, 11);
        grid.add(24, 11, west, 4, 11);
        if (traffic == Traffic::choke)
            for (int16_t z = 0; z < map_height; ++z)
                if (z < 9 || z > 13)
                    for (int16_t x = 11; x <= 17; ++x)
                        grid.walls[Grid::index(x, z)] = true;
    }
    LocalFlowCounters counters;
    for (grid.tick = 1; grid.tick <= battle_tick_limit; ++grid.tick) {
        if (enhanced)
            grid.snapshot_progress();
        bool travelling = false;
        for (std::size_t i = 0; i < grid.count; ++i) {
            auto& agent = grid.agents[i];
            if (!agent.travelling)
                continue;
            travelling = true;
            if (distance(agent.unit.position, agent.destination) < arrival_radius) {
                agent.travelling = false;
                agent.arrival_tick = grid.tick;
                agent.movement.speed = 0;
                agent.movement.velocity = {};
                continue;
            }
            if (agent.has_detour && distance(agent.unit.position, agent.detour) < arrival_radius)
                agent.has_detour = false;
            if (enhanced && !agent.has_detour) {
                const auto before_queries = counters.probes;
                const auto decision = choose_local_flow(
                    agent.unit,
                    agent.movement,
                    agent.destination,
                    grid.tick,
                    grid,
                    grid.query(),
                    counters
                );
                OA_CHECK(counters.probes - before_queries <= maximum_queries_per_choice);
                if (decision.adjusted) {
                    agent.detour = decision.target;
                    agent.has_detour = true;
                }
            }
            const auto target = agent.has_detour ? agent.detour : agent.destination;
            const auto heading = aim_angles(agent.unit.position, target).heading;
            const auto difference = std::bit_cast<int16_t>(uint16_t(heading - agent.unit.heading));
            turn(agent.unit, agent.movement, difference);
            accelerate(agent.unit, agent.movement, fixed_one / 2, 0);
            if (enhanced)
                integrate_local_flow(agent.unit, agent.movement, grid.tick, true, grid, counters);
            else {
                integrate_unattached(agent.unit, agent.movement, grid.tick, true, grid);
                if ((agent.movement.flags & collision_blocked) != 0)
                    ++counters.blocked_ticks;
            }
        }
        if (!travelling)
            break;
    }
    BattleResult result;
    result.blocked = counters.blocked_ticks;
    result.deferred = counters.deferred_replans;
    result.adjustments = counters.steering_adjustments;
    result.probes = counters.probes;
    result.overlaps = grid.overlap_count;
    constexpr uint64_t hash_multiplier = 1099511628211;
    result.digest = 1469598103934665603;
    for (std::size_t i = 0; i < grid.count; ++i) {
        const auto& agent = grid.agents[i];
        if (agent.arrival_tick != 0)
            ++result.arrived;
        result.last_arrival = std::max(result.last_arrival, agent.arrival_tick);
        for (const auto coordinate : agent.unit.position) {
            result.digest ^= uint32_t(coordinate);
            result.digest *= hash_multiplier;
        }
        result.digest ^= agent.arrival_tick;
        result.digest *= hash_multiplier;
    }
    return result;
}

void test_retained_head_save_bytes() {
    std::array<LocalFlowHead, 2> heads{};
    heads[0].points = {{{-2, 7}, {1234, -2345}, {32767, -32768}}};
    heads[0].goal_key = 0x0123456789abcdef;
    heads[0].tick = 0x76543210;
    heads[0].count = 3;
    heads[0].active = true;
    heads[0].mobile_blocked = true;
    heads[0].blocked_ticks = 29;
    heads[1].count = 0;
    heads[1].active = true;
    heads[1].blocked_ticks = 31;
    std::array<uint8_t, local_flow_head_bytes * heads.size()> bytes{};
    OA_CHECK(write_local_flow_heads(heads, bytes));
    // Negative coordinates and the geometry key use explicit little-endian bytes.
    OA_CHECK(bytes[0] == 0xfe && bytes[1] == 0xff);
    OA_CHECK(bytes[12] == 0xef && bytes[19] == 0x01);
    OA_CHECK(bytes[20] == 0x10 && bytes[23] == 0x76);
    OA_CHECK(bytes[24] == 3 && bytes[25] == 3);
    OA_CHECK(bytes[26] == 29 && bytes[local_flow_head_bytes + 26] == 31);
    std::array<LocalFlowHead, heads.size()> restored{};
    OA_CHECK(read_local_flow_heads(restored, bytes));
    std::array<uint8_t, bytes.size()> round_trip{};
    OA_CHECK(write_local_flow_heads(restored, round_trip));
    OA_CHECK(bytes == round_trip);
    OA_CHECK(restored[0].points == heads[0].points);
    OA_CHECK(restored[0].goal_key == heads[0].goal_key);
    OA_CHECK(restored[0].tick == heads[0].tick);
    OA_CHECK(restored[0].active && restored[0].mobile_blocked);
    OA_CHECK(restored[0].blocked_ticks == heads[0].blocked_ticks);
    OA_CHECK(restored[1].active && restored[1].count == 0);
    OA_CHECK(restored[1].blocked_ticks == heads[1].blocked_ticks);
    OA_CHECK(!write_local_flow_heads(heads, std::span(bytes).first(bytes.size() - 1)));
    OA_CHECK(!read_local_flow_heads(restored, std::span(bytes).first(bytes.size() - 1)));
    constexpr size_t saved_count_offset = 24;
    constexpr size_t saved_flags_offset = 25;
    for (const uint8_t invalid_count : {uint8_t{1}, uint8_t{4}}) {
        auto invalid = bytes;
        invalid[local_flow_head_bytes + saved_count_offset] = invalid_count;
        OA_CHECK(!read_local_flow_heads(restored, invalid));
        OA_CHECK(write_local_flow_heads(restored, round_trip));
        OA_CHECK(bytes == round_trip);
    }
    auto invalid = bytes;
    invalid[local_flow_head_bytes + saved_flags_offset] = 4;
    OA_CHECK(!read_local_flow_heads(restored, invalid));
    OA_CHECK(write_local_flow_heads(restored, round_trip));
    OA_CHECK(bytes == round_trip);
    constexpr size_t saved_blocked_ticks_offset = 26;
    for (const uint8_t invalid_ticks : {uint8_t{32}, uint8_t{255}}) {
        invalid = bytes;
        invalid[local_flow_head_bytes + saved_blocked_ticks_offset] = invalid_ticks;
        OA_CHECK(!read_local_flow_heads(restored, invalid));
        OA_CHECK(write_local_flow_heads(restored, round_trip));
        OA_CHECK(bytes == round_trip);
    }
}

struct PassingHistoryResult {
    LocalFlowDecision decision{};
    LocalFlowCounters counters{};
};

/// Offers only a backwards exit after a completed forward passing leg.
PassingHistoryResult passing_history_backout(
    bool retained, uint32_t gap, uint32_t motion_age, bool same_goal = true,
    bool have_history = true, bool restored = false
) {
    Grid grid;
    auto& blocker = grid.add(12, 10, west, 2, 10, {2, 2});
    auto& follower = grid.add(10, 10, east, 24, 10, {2, 2});
    // Two physical rows admit exactly one full FP2 body across the corridor.
    for (int16_t x = 0; x < map_width; ++x) {
        grid.walls[Grid::index(x, 9)] = true;
        grid.walls[Grid::index(x, 12)] = true;
    }
    grid.tick = 208; // The follower's ordinary recovery phase: (208 + 2) % 30 == 0.
    blocker.movement.last_motion_tick = grid.tick - 5;
    follower.movement.last_motion_tick = grid.tick - motion_age;
    grid.snapshot_progress();
    LocalFlowHead history{};
    const auto x = static_cast<int16_t>(follower.unit.position[0] / fixed_one);
    const auto z = static_cast<int16_t>(follower.unit.position[2] / fixed_one);
    history.points = {{{static_cast<int16_t>(x - 16), z}, {x, z}, {400, z}}};
    history.goal_key = 0x1234;
    history.tick = grid.tick - gap;
    history.count = have_history ? local_flow_completed_count : 0;
    history.blocked_ticks = local_flow_retry_ticks;
    if (restored) {
        std::array<uint8_t, local_flow_head_bytes> saved{};
        OA_CHECK(write_local_flow_heads(std::span(&history, 1), saved));
        LocalFlowHead loaded{};
        OA_CHECK(read_local_flow_heads(std::span(&loaded, 1), saved));
        history = loaded;
    }
    auto probe = grid.query();
    const bool recover = local_flow_recovery_due(history, grid.tick, uint16_t(follower.unit.id));
    OA_CHECK(recover);
    const auto progress = probe.progress_at_tick_start[uint16_t(follower.unit.id)];
    const bool recent = retained ? local_flow_completed_within_history(history, grid.tick)
                                 : local_flow_completed_last_tick(history, grid.tick);
    if (same_goal && recent)
        probe.previous_head = &history;
    probe.allow_reverse = recover && (!retained || local_flow_stationary_history_due(grid.tick, progress));
    probe.recovery_yield = recover && grid.tick - progress >= local_flow_retry_ticks;
    PassingHistoryResult result;
    result.decision = choose_local_flow(
        follower.unit, follower.movement, follower.destination, grid.tick, grid, probe,
        result.counters
    );
    OA_CHECK(result.decision.mobile_blocked);
    OA_CHECK(result.counters.probes <= maximum_queries_per_choice);
    OA_CHECK(grid.overlap_count == 0);
    if (result.decision.adjusted) {
        OA_CHECK(result.decision.reverse);
        OA_CHECK(result.decision.target[0] < follower.unit.position[0]);
        OA_CHECK(result.decision.target[2] == follower.unit.position[2]);
    }
    return result;
}

/// Keeps first yields and mature escapes while refusing repeated short undo legs.
void test_retained_passing_history_bounds_backout() {
    OA_CHECK(passing_history_backout(true, 0, 1, true, false).decision.reverse);
    OA_CHECK(passing_history_backout(false, 43, 1).decision.reverse);
    for (const uint32_t gap : {1u, 2u, 43u, local_flow_completed_history_ticks}) {
        const auto guarded = passing_history_backout(true, gap, 1);
        OA_CHECK(!guarded.decision.adjusted && !guarded.decision.reverse);
        const auto loaded = passing_history_backout(true, gap, 1, true, true, true);
        OA_CHECK(loaded.decision.target == guarded.decision.target);
        OA_CHECK(loaded.decision.adjusted == guarded.decision.adjusted);
        OA_CHECK(loaded.decision.reverse == guarded.decision.reverse);
        OA_CHECK(loaded.counters.probes == guarded.counters.probes);
        OA_CHECK(loaded.counters.yielded_ticks == guarded.counters.yielded_ticks);
        OA_CHECK(loaded.counters.steering_adjustments == guarded.counters.steering_adjustments);
    }
    OA_CHECK(passing_history_backout(true, local_flow_completed_history_ticks + 1, 1).decision.reverse);
    OA_CHECK(passing_history_backout(true, 43, 1, false).decision.reverse);
    OA_CHECK(!passing_history_backout(true, 43, local_flow_completed_history_ticks - 1).decision.adjusted);
    OA_CHECK(passing_history_backout(true, 43, local_flow_completed_history_ticks).decision.reverse);
}

/// Allows a forward rejoin even when it opposes the previous short passing leg.
void test_retained_passing_history_keeps_forward_rejoin() {
    Grid grid;
    auto& blocker = grid.add(14, 10, west, 2, 10, {2, 2});
    auto& follower = grid.add(10, 10, east, 24, 10, {2, 2});
    grid.tick = 208;
    blocker.movement.last_motion_tick = grid.tick - 5;
    follower.movement.last_motion_tick = grid.tick - 1;
    grid.snapshot_progress();
    LocalFlowHead previous{};
    const auto x = static_cast<int16_t>(follower.unit.position[0] / fixed_one);
    const auto z = static_cast<int16_t>(follower.unit.position[2] / fixed_one);
    previous.points = {{{static_cast<int16_t>(x + 16), z}, {x, z}, {400, z}}};
    previous.tick = grid.tick - 43;
    previous.count = local_flow_completed_count;
    auto baseline = grid.query();
    baseline.allow_reverse = false;
    auto guarded = baseline;
    guarded.previous_head = &previous;
    LocalFlowCounters before{}, after{};
    const auto ordinary = choose_local_flow(
        follower.unit, follower.movement, follower.destination, grid.tick, grid, baseline, before
    );
    const auto resumed = choose_local_flow(
        follower.unit, follower.movement, follower.destination, grid.tick, grid, guarded, after
    );
    OA_CHECK(ordinary.adjusted && resumed.adjusted && !resumed.reverse);
    OA_CHECK(resumed.target == ordinary.target && resumed.target[0] > follower.unit.position[0]);
    OA_CHECK(before.probes == after.probes && after.probes <= maximum_queries_per_choice);
    OA_CHECK(grid.overlap_count == 0);
}

/// Bounds history and actual-stationary clocks across future stamps and tick wrap.
void test_retained_passing_history_clocks() {
    LocalFlowHead head{};
    head.count = local_flow_completed_count;
    head.tick = 100;
    OA_CHECK(local_flow_completed_within_history(head, 100));
    OA_CHECK(local_flow_completed_within_history(head, 220));
    OA_CHECK(!local_flow_completed_within_history(head, 221));
    OA_CHECK(!local_flow_completed_within_history(head, 99));
    head.active = true;
    OA_CHECK(!local_flow_completed_within_history(head, 101));
    head.active = false;
    head.tick = UINT32_MAX;
    OA_CHECK(local_flow_completed_within_history(head, 0));
    OA_CHECK(!local_flow_stationary_history_due(220, 101));
    OA_CHECK(local_flow_stationary_history_due(220, 100));
    OA_CHECK(!local_flow_stationary_history_due(220, 221));
    OA_CHECK(local_flow_stationary_history_due(0, UINT32_MAX - 119));
}

/// Keeps a completed leg through one tick and the unchanged saved-head format.
void test_completed_head_history_is_bounded_and_restored() {
    constexpr uint32_t completion_tick = 120;
    std::array<LocalFlowHead, 1> heads{};
    auto& completed = heads[0];
    completed.points = {{{320, 528}, {320, 512}, {960, 528}}};
    completed.goal_key = 0x0123456789abcdef;
    completed.tick = completion_tick;
    completed.count = local_flow_completed_count;
    completed.mobile_blocked = true;
    OA_CHECK(!local_flow_completed_last_tick(completed, completion_tick));
    OA_CHECK(local_flow_completed_last_tick(completed, completion_tick + 1));
    OA_CHECK(!local_flow_completed_last_tick(completed, completion_tick + 2));
    OA_CHECK(!local_flow_completed_last_tick(completed, completion_tick - 1));

    std::array<uint8_t, local_flow_head_bytes> bytes{};
    OA_CHECK(write_local_flow_heads(heads, bytes));
    std::array<LocalFlowHead, 1> restored{};
    OA_CHECK(read_local_flow_heads(restored, bytes));
    OA_CHECK(restored[0].points == completed.points);
    OA_CHECK(restored[0].goal_key == completed.goal_key);
    OA_CHECK(local_flow_completed_last_tick(restored[0], completion_tick + 1));
    std::array<uint8_t, local_flow_head_bytes> round_trip{};
    OA_CHECK(write_local_flow_heads(restored, round_trip));
    OA_CHECK(bytes == round_trip);

    completed.active = true;
    OA_CHECK(!local_flow_completed_last_tick(completed, completion_tick + 1));
    completed.active = false;
    for (const uint8_t count : {uint8_t{0}, uint8_t{3}}) {
        completed.count = count;
        OA_CHECK(!local_flow_completed_last_tick(completed, completion_tick + 1));
    }
    completed.count = local_flow_completed_count;
    completed.tick = std::numeric_limits<uint32_t>::max();
    OA_CHECK(local_flow_completed_last_tick(completed, 0));
    OA_CHECK(!local_flow_completed_last_tick(completed, 1));
}

void test_counterflow_crossing_and_choke() {
    for (const auto traffic :
         {Traffic::counterflow, Traffic::crossing, Traffic::choke, Traffic::mixed_footprints}) {
        const auto enhanced = run_traffic(traffic, true);
        const auto repeated = run_traffic(traffic, true);
        const auto legacy = run_traffic(traffic, false);
        const uint32_t expected_arrivals = traffic == Traffic::crossing ? 4 : 2;
        OA_CHECK(enhanced == repeated);
        OA_CHECK(enhanced.overlaps == 0);
        OA_CHECK(enhanced.arrived == expected_arrivals);
        OA_CHECK(enhanced.last_arrival < battle_tick_limit);
        OA_CHECK(enhanced.blocked < legacy.blocked);
        OA_CHECK(enhanced.adjustments > 0);
    }
}
} // namespace

int main() {
    test_priority_and_stationary_obstacles();
    test_units_turning_together_keep_their_route();
    test_verified_queue_keeps_native_target_until_recovery();
    test_sequential_progress_preserves_tick_start_priority();
    test_older_higher_id_keeps_priority_after_moving();
    test_diagonal_corner_clearance();
    test_collision_veto_and_last_progress();
    test_blocked_boundary_creep_cannot_defer_forever();
    test_retained_head_save_bytes();
    test_completed_head_history_is_bounded_and_restored();
    test_retained_passing_history_bounds_backout();
    test_retained_passing_history_keeps_forward_rejoin();
    test_retained_passing_history_clocks();
    test_counterflow_crossing_and_choke();
    return oa::test::check_exit_status();
}
