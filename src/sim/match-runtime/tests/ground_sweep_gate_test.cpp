// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Version four checks the actual ground step through the real Match collision
// host. Earlier presets retain the destination-only integration behavior.
#include "combat_fixture.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/match_runtime/rule_state.hpp"
#include "oa/sim/spatial_state/spatial.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace {
using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;
namespace ground = oa::sim::ground_orders;
namespace movement = oa::sim::unit_movement;
namespace spatial = oa::sim::spatial_state;

constexpr int32_t pixel = 65536;
constexpr int32_t maximum_speed = 117964;
constexpr int32_t acceleration = 6553;
constexpr uint16_t maximum_turn = 1120;
constexpr uint16_t initial_heading = 27872;
constexpr uint16_t step_heading = 26752;
constexpr uint8_t ground_occupancy = 1;
constexpr uint8_t land_height = 60;
constexpr uint8_t sea_level = 30;
constexpr uint32_t movement_tick = 155;
constexpr uint32_t prior_motion_tick = movement_tick - 41;
constexpr movement::Fixed witness_x = 5781946;
constexpr movement::Fixed witness_z = 5767167;
constexpr ground::RoutePoint destination{32, 160};
constexpr std::array<int16_t, 2> initial_cell{5, 4};
constexpr std::array<int16_t, 2> intermediate_cell{5, 5};
constexpr std::array<int16_t, 2> destination_cell{4, 5};
constexpr std::array<movement::Fixed, 3> witness_velocity{-35735, 0, 54935};

enum class Obstacle { none, mobile, terrain };
enum class Driver { owned, remote, mirrored, carried, aircraft };

struct Case {
    uint32_t version = 4;
    Obstacle obstacle = Obstacle::mobile;
    Driver driver = Driver::owned;
    bool within_cell{};
    bool clamp_at_rest{};
    bool historical_block{};
    bool no_route{};
};

struct Result {
    std::array<movement::Fixed, 3> position{}, velocity{};
    std::array<int16_t, 2> cell{};
    movement::Fixed speed{};
    uint32_t last_motion{}, navigation_flags{};
    uint16_t heading{};
    int16_t turn{};
    uint8_t motion_flags{}, blocked_ticks{};
    bool goal_preserved{}, head_active{};
    std::array<uint16_t, 256> ground_occupants{};
    bool operator==(const Result&) const = default;
};

/// Rebuilds the actual match with one flat, full-footprint ground movement class.
void configure_ground(Fixture& fixture, const Case& input) {
    fixture.match.reset();
    fixture.map.sea_level = sea_level;
    for (size_t index = 0; index < fixture.map.attributes.size(); ++index) {
        const auto height =
            input.obstacle == Obstacle::terrain && index == 6 * 16 + 6 ? uint8_t{0} : land_height;
        fixture.map.attributes[index].height = height;
        fixture.collision_plots[index].low_height = height;
        fixture.collision_plots[index].high_height = height;
    }
    fixture.types[1].footprint_x = fixture.types[1].footprint_z = 2;
    if (input.driver == Driver::aircraft) {
        fixture.types[1].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_FLY;
        fixture.def.can_fly = true;
        fixture.def.cruise_altitude = 80;
    }
    fixture.loaded[1].type = fixture.types[1];
    fixture.fields[1].movement_class = 1;
    fixture.metadata.footprint_x = fixture.metadata.footprint_z = 2;
    fixture.metadata.min_water_depth = -10000;
    fixture.metadata.max_water_depth = 0;
    fixture.metadata.max_slope = fixture.metadata.bad_slope = 255;
    fixture.metadata.max_water_slope = fixture.metadata.bad_water_slope = 255;
    fixture.def.max_velocity_fixed = maximum_speed;
    fixture.def.turn_rate = maximum_turn;
    fixture.def.acceleration_fixed = fixture.def.brake_rate_fixed = acceleration;
    runtime::OfflineInputs inputs{
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
    inputs.rules.setup.enhanced_mode.enabled = input.version != 0;
    inputs.rules.setup.enhanced_mode.version = input.version;
    inputs.unit_player_mask = 3;
    fixture.match = std::make_unique<runtime::Match>(inputs, fixture.services);
    fixture.match->configure_strategic_environment({0, 0.5F, 0});
    for (uint8_t owner = 0; owner < 2; ++owner) {
        fixture.match->simulation().players[owner].present = true;
        fixture.match->simulation().players[owner].status = 1;
        std::array<uint8_t, OA_PLAYER_COUNT> allies{};
        allies[owner] = 1;
        fixture.match->configure_player_alliances(owner, allies);
        if (owner == 0)
            fixture.match->configure_outcomes(owner, allies, false);
    }
    CHECK(fixture.match->fault() == nullptr);
    CHECK(fixture.match->movement_map(1) != nullptr);
}

/// Clears traffic history through the match's saved owner-local table.
void clear_head(runtime::Match& match, uint32_t version) {
    if (version < 2)
        return;
    std::vector<movement::LocalFlowHead> heads(match.state().unit_slot_count);
    std::vector<uint8_t> bytes(heads.size() * movement::local_flow_head_bytes);
    CHECK(movement::write_local_flow_heads(heads, bytes));
    const auto* table = runtime::find_rule_state(match.rule_state(), "local-flow");
    CHECK(table != nullptr && table->restore != nullptr);
    CHECK(table->restore(table->context, bytes));
}

/// Checks each canonical plot against a unit's actual registered two-cell footprint.
void check_registered_body(runtime::Match& match, const oa::sim::unit_spawn::Slot& slot) {
    for (int16_t z = 0; z < 2; ++z)
        for (int16_t x = 0; x < 2; ++x) {
            const auto index = size_t(slot.record.cell_z + z) * match.spatial().terrain_width +
                               size_t(slot.record.cell_x + x);
            CHECK(match.spatial().plots[index].ground == slot.unit_index);
        }
}

/// Runs one real TickHost movement step after constructing its native input state.
Result run(const Case& input) {
    Options options;
    options.sight_cells = 9;
    Fixture fixture(options);
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    fixture.fields[1].yard_mask = yard;
    configure_ground(fixture, input);
    auto& blocker = fixture.spawn(0, 224, 224);
    auto& mover = fixture.spawn(0, 64, 64);
    auto& match = *fixture.match;
    CHECK(mover.unit_index == 2);
    CHECK(!match.order_refused(match.issue_ground_move(
        mover.unit_index, {destination[0] * pixel, 0, destination[1] * pixel}, false
    )));
    fixture.run(1);
    match.place_unit_at(
        blocker.unit_index,
        (input.obstacle == Obstacle::mobile ? 112 : 224) * pixel,
        land_height * pixel,
        (input.obstacle == Obstacle::mobile ? 112 : 224) * pixel,
        ground_occupancy
    );
    const auto x = input.within_cell     ? 96 * pixel
                   : input.clamp_at_rest ? 88 * pixel + 1
                                         : witness_x;
    const auto z = input.within_cell ? 80 * pixel : witness_z;
    match.place_unit_at(mover.unit_index, x, land_height * pixel, z, ground_occupancy);
    mover.record.heading = initial_heading;
    mover.record.pitch = 0;
    auto& state = *match.ground_runtime(mover.unit_index);
    state.project_slot();
    state.movement.speed = input.no_route ? 0 : maximum_speed / 2;
    state.movement.velocity = movement::aim_velocity(initial_heading, 0, state.movement.speed);
    state.movement.turn = 0;
    state.movement.flags =
        ground_occupancy | (input.historical_block ? movement::collision_blocked : 0);
    state.movement.last_motion_tick = prior_motion_tick;
    state.navigation.points = {};
    state.navigation.points[0] = {static_cast<int16_t>(x >> 16), static_cast<int16_t>(z >> 16)};
    state.navigation.points[1] = destination;
    state.navigation.count = input.no_route ? 0 : 2;
    state.navigation.flags = input.no_route ? 0 : ground::route_present_flag;
    state.navigation.last_search_tick = movement_tick - 1;
    if (input.no_route)
        state.navigation.goal = nullptr;
    clear_head(match, input.version);
    if (input.driver == Driver::remote)
        match.simulation().players[0].status = 3;
    if (input.driver == Driver::mirrored) {
        state.mirrored_driver = true;
        state.mirrored_navigation.count = 2;
        std::copy_n(state.navigation.points.begin(), 2, state.mirrored_navigation.points.begin());
    }
    if (input.driver == Driver::carried) {
        mover.record.attach_parent = oa::oa_unit_ref_from_slot(blocker.unit_index);
        mover.record.attach_piece = 0xff;
    }
    const auto before_geometry = state.geometry;
    const auto before_motion = state.movement;
    const auto* before_goal = state.navigation.goal;
    const auto* before_order = mover.unit->primary;
    const auto before_navigation = state.navigation;
    if (input.driver == Driver::owned && !input.no_route && !input.within_cell) {
        auto projected = match.project_spatial_slot(mover);
        CHECK(
            spatial::can_occupy(
                projected, mover.unit_index, destination_cell, ground_occupancy, match.spatial()
            ) == true
        );
        CHECK(
            spatial::can_occupy(
                projected, mover.unit_index, intermediate_cell, ground_occupancy, match.spatial()
            ) == (input.obstacle == Obstacle::none)
        );
    }
    auto predicted_geometry = before_geometry;
    auto predicted_motion = before_motion;
    if (input.driver == Driver::owned && !input.no_route && !input.within_cell) {
        ground::steer_ground(
            predicted_geometry,
            predicted_motion,
            before_navigation,
            state.acceleration,
            state.deceleration,
            sea_level,
            ground::FastUnitSteering::as_3_1c
        );
        CHECK(predicted_geometry.heading == step_heading);
        CHECK(predicted_motion.turn == -int16_t(maximum_turn));
        CHECK(predicted_motion.speed == maximum_speed / 2 + acceleration);
        CHECK(predicted_motion.velocity == witness_velocity);
    }
    match.simulation().tick = movement_tick;
    // This public entry invokes the same TickHost::movement_tick used by the
    // match's unit loop, including for locally owned units; it does not inject
    // a pose or bypass the spatial collision host.
    match.step_mirrored_movement(mover.unit_index);
    CHECK(match.fault() == nullptr);
    CHECK(state.navigation.goal == before_goal);
    CHECK(mover.unit->primary == before_order);
    CHECK(state.navigation.last_search_tick == before_navigation.last_search_tick);
    if (input.driver != Driver::aircraft)
        CHECK(
            state.movement.last_motion_tick ==
            (input.driver == Driver::carried || state.geometry.position == before_geometry.position
                 ? prior_motion_tick
                 : movement_tick)
        );
    CHECK(state.movement.speed <= maximum_speed);
    CHECK(std::abs(int(state.movement.turn)) <= maximum_turn);
    if (input.driver == Driver::owned && !input.no_route && !input.within_cell) {
        CHECK(state.geometry.heading == predicted_geometry.heading);
        CHECK(state.movement.turn == predicted_motion.turn);
        CHECK(
            state.movement.speed == (input.version >= 4 && input.obstacle != Obstacle::none
                                         ? maximum_speed / 2
                                         : predicted_motion.speed)
        );
    }
    if (input.driver != Driver::carried && input.driver != Driver::aircraft) {
        CHECK(
            state.geometry.cell ==
            (std::array<int16_t, 2>{mover.record.cell_x, mover.record.cell_z})
        );
        check_registered_body(match, mover);
        check_registered_body(match, blocker);
    }
    Result result;
    result.position = state.geometry.position;
    result.velocity = state.movement.velocity;
    result.cell = state.geometry.cell;
    result.speed = state.movement.speed;
    result.last_motion = state.movement.last_motion_tick;
    result.navigation_flags = state.navigation.flags;
    result.heading = state.geometry.heading;
    result.turn = state.movement.turn;
    result.motion_flags = state.movement.flags;
    result.goal_preserved = state.navigation.goal == before_goal;
    if (const auto* head = match.local_flow_head(mover.unit_index)) {
        result.blocked_ticks = head->blocked_ticks;
        result.head_active = head->active;
    }
    for (size_t index = 0; index < result.ground_occupants.size(); ++index)
        result.ground_occupants[index] = match.spatial().plots[index].ground;
    return result;
}

/// Retains old movement and rejects the positive-duration intermediate collision only in four.
void versioned_ground_sweep() {
    Case input;
    input.version = 0;
    const auto legacy = run(input);
    CHECK(legacy.cell == destination_cell);
    CHECK(legacy.position[0] == witness_x + witness_velocity[0]);
    CHECK(legacy.position[2] == witness_z + witness_velocity[2]);
    CHECK((legacy.motion_flags & movement::collision_blocked) == 0);
    for (const auto version : {1u, 2u, 3u}) {
        input.version = version;
        CHECK(run(input) == legacy);
    }
    input.version = 4;
    const auto swept = run(input);
    CHECK(swept.cell == initial_cell);
    CHECK(swept.position[0] == 88 * pixel + 1);
    CHECK(swept.position[2] == witness_z);
    CHECK((swept.motion_flags & movement::collision_blocked) != 0);
    CHECK(swept.speed == maximum_speed / 2);
    CHECK(swept.velocity == movement::aim_velocity(step_heading, 0, maximum_speed / 2));
    CHECK(swept.blocked_ticks == 1);
    CHECK(!swept.head_active);
    input.obstacle = Obstacle::terrain;
    const auto terrain = run(input);
    CHECK(terrain.cell == initial_cell);
    CHECK(terrain.position == swept.position);
    CHECK(terrain.motion_flags == swept.motion_flags);
    input.version = 3;
    CHECK(run(input).cell == destination_cell);
}

/// Keeps clear steps, historical collision bits and actual-motion clock accounting unchanged.
void clear_steps_and_progress_clock() {
    Case input;
    input.obstacle = Obstacle::none;
    input.version = 3;
    const auto legacy = run(input);
    input.version = 4;
    CHECK(run(input) == legacy);
    input.within_cell = true;
    input.historical_block = true;
    input.version = 3;
    const auto same_cell = run(input);
    CHECK((same_cell.motion_flags & movement::collision_blocked) != 0);
    input.version = 4;
    CHECK(run(input) == same_cell);
    input = {};
    input.clamp_at_rest = true;
    const auto stationary = run(input);
    CHECK(stationary.last_motion == prior_motion_tick);
    CHECK(stationary.position[0] == 88 * pixel + 1);
    CHECK(stationary.position[2] == witness_z);
    input.no_route = true;
    input.version = 3;
    const auto stopped = run(input);
    input.version = 4;
    CHECK(run(input) == stopped);
}

/// Leaves mirrored, remote, attached and aircraft integration on their original paths.
void excluded_drivers_keep_native_motion() {
    for (const auto driver :
         {Driver::remote, Driver::mirrored, Driver::carried, Driver::aircraft}) {
        Case input;
        input.driver = driver;
        input.version = 3;
        const auto legacy = run(input);
        input.version = 4;
        CHECK(run(input) == legacy);
    }
}
} // namespace

int main() {
    try {
        versioned_ground_sweep();
        clear_steps_and_progress_clock();
        excluded_drivers_keep_native_motion();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
