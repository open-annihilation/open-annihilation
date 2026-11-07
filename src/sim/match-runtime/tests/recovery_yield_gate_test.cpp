// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Mature traffic recovery is admitted by the real Match tick's phase and
// tick-start progress snapshot, independently of later live-clock changes.
#include "combat_fixture.hpp"
#include "tick_internal.hpp"

#include "oa/sim/match_runtime/enhanced_counters.hpp"
#include "oa/sim/match_runtime/rule_state.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

namespace {
using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;
namespace simulation = oa::sim::simulation_state;
namespace movement = oa::sim::unit_movement;
namespace ground = oa::sim::ground_orders;

constexpr int32_t pixel = 65536;
constexpr movement::Fixed maximum_speed = 117964;
constexpr uint16_t maximum_turn = 1120;
constexpr uint16_t east = 49152;
constexpr uint8_t ground_occupancy = 1;
constexpr uint8_t land_height = 60;
constexpr uint8_t sea_level = 30;
constexpr uint32_t recovery_tick = 58;

/// Rebuilds the fixture with flat known land and one full two-by-two ground class.
void configure_ground(Fixture& fixture, uint32_t version) {
    fixture.match.reset();
    fixture.map.sea_level = sea_level;
    for (size_t index = 0; index < fixture.map.attributes.size(); ++index) {
        fixture.map.attributes[index].height = land_height;
        fixture.collision_plots[index].low_height = land_height;
        fixture.collision_plots[index].high_height = land_height;
    }
    fixture.types[1].footprint_x = fixture.types[1].footprint_z = 2;
    fixture.loaded[1].type = fixture.types[1];
    fixture.fields[1].movement_class = 1;
    fixture.metadata.footprint_x = fixture.metadata.footprint_z = 2;
    fixture.metadata.min_water_depth = -10000;
    fixture.metadata.max_water_depth = 0;
    fixture.metadata.max_slope = fixture.metadata.bad_slope = 255;
    fixture.metadata.max_water_slope = fixture.metadata.bad_water_slope = 255;
    fixture.def.max_velocity_fixed = maximum_speed;
    fixture.def.turn_rate = maximum_turn;
    fixture.def.acceleration_fixed = fixture.def.brake_rate_fixed = 6553;
    runtime::OfflineInputs input{
        fixture.map,
        fixture.loaded,
        fixture.types,
        fixture.fields,
        fixture.weapons,
        fixture.terrain_values,
        fixture.masks,
        8,
        8,
        4,
        2,
        0,
        sea_level,
        1,
        &fixture.scenario,
        {},
        fixture.collision_plots,
        oa::sim::visibility_state::AltitudeSightData{
            8, 8, fixture.altitude_cells, fixture.altitude_patterns
        }
    };
    input.rules.setup.enhanced_mode.enabled = version != 0;
    input.rules.setup.enhanced_mode.version = version;
    input.unit_player_mask = 3;
    fixture.match = std::make_unique<runtime::Match>(input, fixture.services);
    fixture.match->configure_strategic_environment({0, 0.5F, 0});
    for (uint8_t player = 0; player < 2; ++player) {
        fixture.match->simulation().players[player].present = true;
        fixture.match->simulation().players[player].status = 1;
        std::array<uint8_t, OA_PLAYER_COUNT> allies{};
        allies[player] = 1;
        fixture.match->configure_player_alliances(player, allies);
        if (player == 0)
            fixture.match->configure_outcomes(0, allies, false);
    }
    CHECK(fixture.match->fault() == nullptr);
    CHECK(fixture.match->movement_map(1) != nullptr);
}

/// Restores a mature inactive head through the same rule table used by saved matches.
void restore_blocked_head(runtime::Match& match, uint16_t unit, uint8_t blocked_ticks) {
    std::vector<movement::LocalFlowHead> heads(match.state().unit_slot_count);
    heads[unit].mobile_blocked = true;
    heads[unit].blocked_ticks = blocked_ticks;
    std::vector<uint8_t> bytes(heads.size() * movement::local_flow_head_bytes);
    CHECK(movement::write_local_flow_heads(heads, bytes));
    const auto* table = runtime::find_rule_state(match.rule_state(), "local-flow");
    CHECK(table != nullptr && table->restore != nullptr);
    CHECK(table->restore(table->context, bytes));
}

/// Forwards the real simulation services and changes one clock just before movement.
struct ClockChangingHost final : simulation::Host {
    runtime::Match& match;
    runtime::TickHost actual;
    uint16_t target{};
    uint32_t replacement{};
    uint32_t before{};
    uint32_t after{};
    uint32_t calls{};

    /// Binds the ordinary services and the selected later clock replacement.
    ClockChangingHost(runtime::Match& match, uint16_t unit, uint32_t clock)
        : match(match), actual(match), target(unit), replacement(clock) {
        const auto& simulation_host = static_cast<const simulation::Host&>(actual);
        rules = simulation_host.rules;
        order_ends = simulation_host.order_ends;
    }

    uint32_t dispatch_mission(
        oa::World& world, oa::Unit& unit, simulation::Order& order, uint32_t events
    ) override {
        return actual.dispatch_mission(world, unit, order, events);
    }

    uint32_t random_bounded(uint32_t limit) override { return actual.random_bounded(limit); }

    void queue_default_mission(oa::World& world, oa::Unit& unit) override {
        actual.queue_default_mission(world, unit);
    }

    void clear_weapon_target(oa::Unit& unit, uint32_t slot) override {
        actual.clear_weapon_target(unit, slot);
    }

    void destroy_order(oa::Unit& unit, simulation::Order& order) override {
        actual.destroy_order(unit, order);
    }

    void tick_script(oa::Unit& unit, uint32_t steps) override { actual.tick_script(unit, steps); }

    void apply_scaled_damage(oa::Unit& unit, int32_t amount, uint32_t kind) override {
        actual.apply_scaled_damage(unit, amount, kind);
    }

    void regenerate_health(oa::Unit& unit) override { actual.regenerate_health(unit); }

    /// Replaces the live clock after Match captured its snapshot, then moves normally.
    void movement_tick(oa::Unit& unit) override {
        if (unit.id == target) {
            auto& motion = match.ground_runtime(target)->movement;
            before = motion.last_motion_tick;
            motion.last_motion_tick = replacement;
            after = motion.last_motion_tick;
            ++calls;
        }
        actual.movement_tick(unit);
    }

    int32_t terrain_height_under(oa::Unit& unit) override {
        return actual.terrain_height_under(unit);
    }

    void settle_on_ground(oa::Unit& unit) override { actual.settle_on_ground(unit); }

    void kill_unit(oa::Unit& unit, uint8_t kind) override { actual.kill_unit(unit, kind); }

    void update_wind_generator(oa::Unit& unit) override { actual.update_wind_generator(unit); }

    void tick_weapon_aim(oa::Unit& unit) override { actual.tick_weapon_aim(unit); }

    void local_player_ticked(oa::Player& player) override { actual.local_player_ticked(player); }

    bool is_key_down(uint32_t key) override { return actual.is_key_down(key); }

    void select_next_viewpoint_unit() override { actual.select_next_viewpoint_unit(); }

    void follow_next_selected(uint32_t backward) override { actual.follow_next_selected(backward); }
};

struct MovementResult {
    std::array<movement::Fixed, 3> position{};
    std::array<movement::Fixed, 3> velocity{};
    std::array<int16_t, 2> cell{};
    std::array<std::array<int16_t, 2>, 3> points{};
    movement::Fixed speed{};
    uint16_t heading{};
    int16_t turn{};
    uint8_t flags{};
    bool active{};
    bool mobile_blocked{};
    bool operator==(const MovementResult&) const = default;
};

/// Runs a real Match unit tick against recent coherent traffic at a physical clamp.
MovementResult run_gate_case(
    uint32_t tick,
    uint32_t snapshot_age,
    uint8_t blocked_ticks,
    std::optional<uint32_t> later_age = std::nullopt,
    uint32_t version = 2
) {
    Options options;
    options.sight_cells = 9;
    Fixture fixture(options);
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    fixture.fields[1].yard_mask = yard;
    configure_ground(fixture, version);
    auto& blocker = fixture.spawn(0, 136, 64);
    auto& mover = fixture.spawn(0, 112, 64);
    auto& match = *fixture.match;
    CHECK(mover.unit_index == 2);
    blocker.record.heading = mover.record.heading = east;
    CHECK(!match.order_refused(
        match.issue_ground_move(mover.unit_index, {200 * pixel, 0, 64 * pixel}, false)
    ));
    fixture.run(1);
    auto& ground = *match.ground_runtime(mover.unit_index);
    auto& other = *match.ground_runtime(blocker.unit_index);
    match.place_unit_at(
        mover.unit_index, 120 * pixel - 1, 60 * pixel, 64 * pixel, ground_occupancy
    );
    mover.record.heading = blocker.record.heading = east;
    ground.project_slot();
    other.project_slot();
    ground.movement.speed = maximum_speed;
    ground.movement.velocity = movement::aim_velocity(east, 0, maximum_speed);
    ground.movement.turn = 0;
    ground.movement.flags = ground_occupancy | movement::collision_blocked;
    ground.movement.last_motion_tick = tick - snapshot_age;
    other.movement.speed = 0;
    other.movement.velocity = {};
    other.movement.last_motion_tick = tick - 1;
    ground.navigation.count = 2;
    ground.navigation.points[0] = {112, 64};
    ground.navigation.points[1] = {200, 64};
    ground.navigation.flags = ground::route_present_flag;
    ground.navigation.last_search_tick = 0;
    CHECK(ground.navigation.goal != nullptr);
    if (version >= 2)
        restore_blocked_head(match, mover.unit_index, blocked_ticks);
    else
        CHECK(runtime::find_rule_state(match.rule_state(), "local-flow") == nullptr);
    const auto before_searches = match.enhanced_counters().full_route_searches;
    const auto idle_x = blocker.record.position.x, idle_z = blocker.record.position.z;
    match.simulation().tick = tick - 1;
    if (later_age) {
        ++match.simulation().tick;
        ClockChangingHost host(match, mover.unit_index, tick - *later_age);
        match.tick(host);
        CHECK(host.calls == 1);
        CHECK(host.before == tick - snapshot_age);
        CHECK(host.after == tick - *later_age);
        CHECK(match.fault() == nullptr);
    } else
        fixture.run(1);
    CHECK(match.simulation().tick == tick);
    CHECK(tick < ground::search_retry_ticks);
    CHECK(match.enhanced_counters().full_route_searches == before_searches);
    CHECK(blocker.record.position.x == idle_x && blocker.record.position.z == idle_z);
    CHECK(ground.geometry.footprint == (std::array<int16_t, 2>{2, 2}));
    CHECK(ground.geometry.cell == (std::array<int16_t, 2>{6, 3}));
    CHECK(ground.movement.speed == maximum_speed / 2);
    CHECK(ground.geometry.heading == static_cast<uint16_t>(east + ground.movement.turn));
    CHECK(std::abs(int(ground.movement.turn)) <= maximum_turn);
    CHECK(
        ground.movement.velocity ==
        movement::aim_velocity(ground.geometry.heading, 0, ground.movement.speed)
    );
    CHECK((ground.movement.flags & movement::collision_blocked) != 0);
    CHECK(ground.movement.flags == (ground_occupancy | movement::collision_blocked));
    CHECK(ground.navigation.goal != nullptr);
    CHECK(ground.navigation.points[1] == (ground::RoutePoint{200, 64}));
    CHECK(ground.geometry.position[0] < 120 * pixel);
    const bool overlap = mover.record.cell_x < blocker.record.cell_x + blocker.record.footprint_x &&
                         mover.record.cell_x + mover.record.footprint_x > blocker.record.cell_x &&
                         mover.record.cell_z < blocker.record.cell_z + blocker.record.footprint_z &&
                         mover.record.cell_z + mover.record.footprint_z > blocker.record.cell_z;
    CHECK(!overlap);
    MovementResult result;
    result.position = ground.geometry.position;
    result.velocity = ground.movement.velocity;
    result.cell = ground.geometry.cell;
    result.speed = ground.movement.speed;
    result.heading = ground.geometry.heading;
    result.turn = ground.movement.turn;
    result.flags = ground.movement.flags;
    if (const auto* head = match.local_flow_head(mover.unit_index)) {
        result.active = head->active;
        result.mobile_blocked = head->mobile_blocked;
        result.points = head->points;
        CHECK(
            head->blocked_ticks ==
            std::min<unsigned>(blocked_ticks + 1, movement::local_flow_retry_ticks + 1)
        );
        if (head->active) {
            CHECK(head->count == 3 && head->tick == tick);
            CHECK(head->points[2] == (ground::RoutePoint{200, 64}));
        }
    }
    // Eligibility reads the snapshot; it must not restore the live clock
    // changed by another callback. A clamped non-yielding move goes nowhere.
    if (later_age && version >= 2 && !result.active)
        CHECK(ground.movement.last_motion_tick == tick - *later_age);
    return result;
}

/// Requires both maturity gates and the scheduled unit phase.
void test_recovery_phase_and_age_boundaries() {
    const auto mature = run_gate_case(recovery_tick, 30, 31);
    CHECK(mature.active && mature.mobile_blocked);
    CHECK(mature == run_gate_case(recovery_tick, 30, 31));
    CHECK(!run_gate_case(recovery_tick, 29, 31).active);
    CHECK(!run_gate_case(recovery_tick - 1, 30, 31).active);
    CHECK(!run_gate_case(recovery_tick, 30, 29).active);
    CHECK(run_gate_case(recovery_tick, 30, 30).active);
    CHECK(!run_gate_case(recovery_tick, 30, 31, std::nullopt, 0).active);
    CHECK(!run_gate_case(recovery_tick, 30, 31, std::nullopt, 1).active);
}

/// Uses the captured age even when a later callback makes the live clock fresh or old.
void test_tick_start_snapshot_wins_over_later_clocks() {
    const auto old_snapshot = run_gate_case(recovery_tick, 30, 31);
    const auto fresh_snapshot = run_gate_case(recovery_tick, 29, 31);
    CHECK(old_snapshot.active && !fresh_snapshot.active);
    CHECK(run_gate_case(recovery_tick, 30, 31, 0) == old_snapshot);
    CHECK(run_gate_case(recovery_tick, 29, 31, 30) == fresh_snapshot);
    CHECK(run_gate_case(recovery_tick, 30, 31, 30) == old_snapshot);
    CHECK(run_gate_case(recovery_tick - 1, 30, 31, 0) == run_gate_case(recovery_tick - 1, 30, 31));
}
} // namespace

int main() {
    try {
        test_recovery_phase_and_age_boundaries();
        test_tick_start_snapshot_wins_over_later_clocks();
        std::cout << "recovery yield gate: ok\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
