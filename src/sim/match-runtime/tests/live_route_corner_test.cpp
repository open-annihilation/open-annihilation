// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Live corner advances retain the canonical goal and native movement while
// requiring mature local blockage, the scheduled phase and known clear terrain.
#include "oa/data/enhanced_mode.hpp"
#include "combat_fixture.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/match_runtime/enhanced_counters.hpp"
#include "oa/sim/match_runtime/rule_state.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace {
using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;
namespace ground = oa::sim::ground_orders;
namespace movement = oa::sim::unit_movement;

constexpr int32_t pixel = 65536;
constexpr int32_t maximum_speed = 117964;
constexpr uint16_t maximum_turn = 1120;
constexpr uint16_t east = 49152;
constexpr uint16_t west = 16384;
constexpr uint8_t ground_occupancy = 1;
constexpr uint8_t land_height = 60;
constexpr uint8_t sea_level = 30;
constexpr uint32_t corner_tick = 118;

/// Rebuilds the fixture with flat known land and one full two-by-two ground class.
void configure_ground(Fixture& fixture, uint32_t version, bool water, bool aircraft) {
    fixture.match.reset();
    fixture.map.sea_level = sea_level;
    for (size_t index = 0; index < fixture.map.attributes.size(); ++index) {
        const auto height = static_cast<uint8_t>(
            water && index == 5 * fixture.map.attribute_width + 5 ? 0 : land_height
        );
        fixture.map.attributes[index].height = height;
        fixture.collision_plots[index].low_height = height;
        fixture.collision_plots[index].high_height = height;
    }
    fixture.types[1].footprint_x = fixture.types[1].footprint_z = 2;
    if (aircraft) {
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
    oa::data::enhanced_mode::apply_alias(
        input.rules.setup.enhanced_mode, static_cast<uint32_t>(version)
    );
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

/// Restores a head through the canonical rule table used by saved matches.
void restore_head(runtime::Match& match, uint16_t id, const movement::LocalFlowHead& head) {
    std::vector<movement::LocalFlowHead> heads(match.state().unit_slot_count);
    heads[id] = head;
    std::vector<uint8_t> bytes(heads.size() * movement::local_flow_head_bytes);
    CHECK(movement::write_local_flow_heads(heads, bytes));
    const auto* table = runtime::find_rule_state(match.rule_state(), "local-flow");
    CHECK(table != nullptr && table->restore != nullptr);
    CHECK(table->restore(table->context, bytes));
}

enum class Driver { owned, mirrored, carried, aircraft };
enum class Head { inactive, active, completed, replaced_goal };

struct Case {
    uint32_t version = 3;
    uint32_t tick = corner_tick;
    uint8_t blocked = 30;
    uint32_t count = 4;
    bool known = true;
    bool water{};
    bool collinear{};
    bool has_goal = true;
    bool has_route = true;
    bool pending = true;
    bool clamp{};
    bool matching_join = true;
    uint8_t head_count = 3;
    Driver driver = Driver::owned;
    Head head = Head::inactive;
};

struct Result {
    std::array<ground::RoutePoint, ground::navigation_capacity> points{};
    std::array<movement::Fixed, 3> position{};
    std::array<movement::Fixed, 3> velocity{};
    uint32_t count{};
    movement::Fixed speed{};
    uint16_t heading{};
    int16_t turn{};
    uint8_t motion_flags{};
    bool head_active{};
    uint8_t head_count{};
    bool operator==(const Result&) const = default;
};

/// Runs the actual Match tick after restoring a mature route on stock ground units.
Result run(const Case& input, bool advances) {
    ground::Goal air_goal{};
    air_goal.cell = {12, 7};
    Options options;
    options.sight_cells = input.known ? 9 : 1;
    Fixture fixture(options);
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    fixture.fields[1].yard_mask = yard;
    configure_ground(fixture, input.version, input.water, input.driver == Driver::aircraft);
    auto& blocker = fixture.spawn(0, 96, 64);
    auto& mover = fixture.spawn(0, 64, 64);
    auto& match = *fixture.match;
    CHECK(mover.unit_index == 2);
    blocker.record.heading = west;
    mover.record.heading = east;
    CHECK(!match.order_refused(
        match.issue_ground_move(mover.unit_index, {208 * pixel, 0, 128 * pixel}, false)
    ));
    fixture.run(1);
    auto& state = *match.ground_runtime(mover.unit_index);
    movement::LocalFlowHead restored{};
    if (const auto* warm = match.local_flow_head(mover.unit_index))
        restored = *warm;
    if (input.head != Head::inactive)
        CHECK(restored.active && restored.goal_key != 0);
    match.place_unit_at(blocker.unit_index, 224 * pixel, 60 * pixel, 224 * pixel, ground_occupancy);
    match.place_unit_at(
        mover.unit_index,
        input.clamp ? 120 * pixel - 1 : 64 * pixel + 3 * pixel / 8,
        60 * pixel,
        input.clamp || input.collinear ? 64 * pixel : 64 * pixel + 5 * pixel / 8,
        ground_occupancy
    );
    if (input.clamp)
        match.place_unit_at(
            blocker.unit_index, 136 * pixel, 60 * pixel, 64 * pixel, ground_occupancy
        );
    mover.record.heading = east;
    state.project_slot();
    state.movement.speed = maximum_speed;
    state.movement.velocity = movement::aim_velocity(east, 0, maximum_speed);
    state.movement.turn = 0;
    state.movement.flags = ground_occupancy;
    state.movement.last_motion_tick = input.tick - 40;
    if (input.driver == Driver::aircraft) {
        CHECK(match.air_driver(mover.unit_index) != nullptr);
        state.navigation.goal = &air_goal;
    }
    CHECK(state.navigation.goal != nullptr);
    state.navigation.points = {};
    state.navigation.points[0] =
        input.clamp ? ground::RoutePoint{80, 64} : ground::RoutePoint{32, 64};
    state.navigation.points[1] =
        input.clamp ? ground::RoutePoint{144, 64} : ground::RoutePoint{96, 64};
    state.navigation.points[2] = input.collinear ? ground::RoutePoint{128, 64}
                                 : input.clamp   ? ground::RoutePoint{144, 128}
                                                 : ground::RoutePoint{96, 128};
    for (size_t index = 3; index < state.navigation.points.size(); ++index)
        state.navigation.points[index] = {208, static_cast<int16_t>(128 + index - 3)};
    state.navigation.count = input.count;
    state.navigation.flags = (input.has_route ? ground::route_present_flag : 0) |
                             (input.pending ? ground::search_pending_flag : 0);
    state.navigation.last_search_tick = input.tick - 1;
    if (!input.has_goal)
        state.navigation.goal = nullptr;
    restored.active = input.head != Head::inactive;
    restored.count = restored.active ? input.head_count : 0;
    restored.mobile_blocked = true;
    restored.blocked_ticks = input.blocked;
    restored.tick = input.tick - 1;
    restored.points = input.clamp
                          ? std::array<ground::RoutePoint, 3>{{{119, 64}, {135, 64}, {144, 128}}}
                          : std::array<ground::RoutePoint, 3>{{{64, 64}, {80, 80}, {96, 128}}};
    if (input.head == Head::completed)
        restored.points[1] = {64, 64};
    if (input.head == Head::replaced_goal)
        ++restored.goal_key;
    if (!input.matching_join)
        restored.points[2][0] += 16;
    if (input.version >= 2)
        restore_head(match, mover.unit_index, restored);
    if (input.driver == Driver::mirrored) {
        state.mirrored_driver = true;
        state.mirrored_navigation.count = 3;
        std::copy_n(state.navigation.points.begin(), 3, state.mirrored_navigation.points.begin());
    }
    if (input.driver == Driver::carried) {
        mover.record.attach_parent = oa::oa_unit_ref_from_slot(blocker.unit_index);
        mover.record.attach_piece = 0xff;
    }
    const auto original_navigation = state.navigation;
    const auto original_geometry = state.geometry;
    const auto original_motion = state.movement;
    const auto searches = match.enhanced_counters().full_route_searches;
    match.simulation().tick = input.tick - 1;
    fixture.run(1);
    CHECK(match.simulation().tick == input.tick);
    CHECK(match.enhanced_counters().full_route_searches == searches);
    CHECK(state.navigation.goal == original_navigation.goal);
    CHECK(state.navigation.last_search_tick == original_navigation.last_search_tick);
    CHECK(state.navigation.needs_search() == input.pending);
    CHECK(ground::route_present(state.navigation) == input.has_route);
    CHECK(state.navigation.count == original_navigation.count - (advances ? 1u : 0u));
    if (advances) {
        CHECK((state.navigation.flags & ground::route_changed_flag) != 0);
        CHECK(
            state.navigation.points[0] ==
            (ground::RoutePoint{
                static_cast<int16_t>(original_geometry.position[0] >> 16),
                static_cast<int16_t>(original_geometry.position[2] >> 16)
            })
        );
        for (uint32_t index = 1; index < state.navigation.count; ++index)
            CHECK(state.navigation.points[index] == original_navigation.points[index + 1]);
    } else {
        CHECK(state.navigation.points == original_navigation.points);
    }
    if (input.driver == Driver::owned) {
        CHECK(state.geometry.footprint == (std::array<int16_t, 2>{2, 2}));
        CHECK(std::abs(int(state.movement.turn)) <= maximum_turn);
        CHECK(state.geometry.heading == static_cast<uint16_t>(east + state.movement.turn));
        CHECK(
            state.movement.velocity ==
            movement::aim_velocity(state.geometry.heading, 0, state.movement.speed)
        );
        CHECK(state.movement.speed <= maximum_speed);
        const bool overlap =
            mover.record.cell_x < blocker.record.cell_x + blocker.record.footprint_x &&
            mover.record.cell_x + mover.record.footprint_x > blocker.record.cell_x &&
            mover.record.cell_z < blocker.record.cell_z + blocker.record.footprint_z &&
            mover.record.cell_z + mover.record.footprint_z > blocker.record.cell_z;
        CHECK(!overlap);
        if (input.clamp) {
            CHECK(state.geometry.position == original_geometry.position);
            CHECK(state.geometry.cell == original_geometry.cell);
            CHECK(state.movement.speed == maximum_speed / 2);
            CHECK((state.movement.flags & movement::collision_blocked) != 0);
        } else {
            auto native_geometry = original_geometry;
            auto native_motion = original_motion;
            auto native_navigation = state.navigation;
            if (const auto* head = match.local_flow_head(mover.unit_index); head && head->active) {
                std::copy(
                    head->points.begin(), head->points.end(), native_navigation.points.begin()
                );
                native_navigation.count = head->count;
            }
            ground::steer_ground(
                native_geometry,
                native_motion,
                native_navigation,
                state.acceleration,
                state.deceleration,
                sea_level,
                ground::FastUnitSteering::as_3_1c
            );
            CHECK(state.geometry.heading == native_geometry.heading);
            CHECK(state.movement.turn == native_motion.turn);
            CHECK(state.movement.speed == native_motion.speed);
            CHECK(state.movement.velocity == native_motion.velocity);
            CHECK(
                state.geometry.position[0] ==
                original_geometry.position[0] + native_motion.velocity[0]
            );
            CHECK(
                state.geometry.position[2] ==
                original_geometry.position[2] + native_motion.velocity[2]
            );
        }
    }
    Result result;
    result.points = state.navigation.points;
    result.position = state.geometry.position;
    result.velocity = state.movement.velocity;
    result.count = state.navigation.count;
    result.speed = state.movement.speed;
    result.heading = state.geometry.heading;
    result.turn = state.movement.turn;
    result.motion_flags = state.movement.flags;
    if (const auto* head = match.local_flow_head(mover.unit_index)) {
        result.head_active = head->active;
        result.head_count = head->count;
        if (advances && input.head == Head::active) {
            CHECK(head->active && head->count == restored.count);
            CHECK(head->points == restored.points);
            CHECK(head->goal_key == restored.goal_key);
            CHECK(head->tick == input.tick);
        }
        if (input.head == Head::completed || input.head == Head::replaced_goal)
            CHECK(!head->active);
    }
    return result;
}

/// Requires maturity, phase, valid geometry and an owned Enhanced ground driver.
void eligibility_and_legacy_controls() {
    const auto expected = run({}, true);
    CHECK(expected == run({}, true));
    Case input;
    input.tick = corner_tick - 1;
    run(input, false);
    input = {};
    input.blocked = 29;
    run(input, false);
    input.blocked = 31;
    run(input, true);
    input = {};
    input.count = 3;
    run(input, true);
    input.count = ground::navigation_capacity;
    run(input, true);
    input.count = 2;
    run(input, false);
    input.count = ground::navigation_capacity + 1;
    run(input, false);
    input = {};
    input.collinear = true;
    run(input, false);
    input = {};
    input.has_goal = false;
    run(input, false);
    input = {};
    input.has_route = false;
    run(input, false);
    input = {};
    input.pending = false;
    run(input, true);
    input = {};
    input.version = 0;
    const auto disabled = run(input, false);
    input.version = 1;
    CHECK(disabled == run(input, false));
    input.version = 2;
    run(input, false);
    for (const auto driver : {Driver::mirrored, Driver::carried, Driver::aircraft}) {
        input = {};
        input.driver = driver;
        run(input, false);
    }
}

/// Rejects unseen clear space and known impassable footprint cells.
void known_static_clearance_is_required() {
    Case input;
    input.known = false;
    run(input, false);
    input = {};
    input.water = true;
    run(input, false);
}

/// Keeps a useful local leg through simultaneous corner and recovery phases.
void active_head_and_native_collision_are_preserved() {
    Case input;
    input.head = Head::active;
    CHECK(run(input, true).head_active);
    input.clamp = true;
    CHECK(run(input, true).head_active);
    input = {};
    input.head = Head::active;
    input.collinear = true;
    CHECK(!run(input, false).head_active);
    input = {};
    input.head = Head::active;
    input.matching_join = false;
    CHECK(!run(input, false).head_active);
    input.matching_join = true;
    input.head_count = 2;
    CHECK(!run(input, false).head_active);
    input = {};
    input.head = Head::completed;
    CHECK(!run(input, true).head_active);
    input.head = Head::replaced_goal;
    CHECK(!run(input, true).head_active);
}

/// Hashes explicit integer words and optionally writes the complete trajectory.
struct LegacyDigest {
    uint64_t value = 14695981039346656037ull;
    bool trace{};

    /// Adds one little-endian canonical word without native padding.
    void add(uint32_t word) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            value ^= static_cast<uint8_t>(word >> (byte * 8));
            value *= 1099511628211ull;
        }
        if (trace)
            std::cout << word << ',';
    }
};

/// Records native motion, route, goal and saved flow state after a real tick.
void record_legacy_tick(
    LegacyDigest& digest, runtime::Match& match, std::array<uint16_t, 2> units
) {
    digest.add(match.simulation().tick);
    for (const auto id : units) {
        const auto& record = match.state().units[id];
        const auto& state = *match.ground_runtime(id);
        digest.add(id);
        digest.add(record.position.x);
        digest.add(record.position.y);
        digest.add(record.position.z);
        digest.add(record.heading);
        digest.add(static_cast<uint16_t>(record.pitch));
        digest.add(static_cast<uint16_t>(record.bank));
        digest.add(static_cast<uint16_t>(record.cell_x));
        digest.add(static_cast<uint16_t>(record.cell_z));
        digest.add(record.flags);
        for (const auto value : state.geometry.position)
            digest.add(static_cast<uint32_t>(value));
        for (const auto value : state.geometry.cell)
            digest.add(static_cast<uint16_t>(value));
        for (const auto value : state.geometry.footprint)
            digest.add(static_cast<uint16_t>(value));
        digest.add(state.geometry.heading);
        digest.add(static_cast<uint16_t>(state.geometry.pitch));
        digest.add(state.geometry.flags);
        for (const auto value : state.movement.velocity)
            digest.add(static_cast<uint32_t>(value));
        digest.add(static_cast<uint32_t>(state.movement.speed));
        digest.add(static_cast<uint16_t>(state.movement.turn));
        digest.add(state.movement.last_motion_tick);
        digest.add(state.movement.flags);
        digest.add(state.navigation.count);
        digest.add(state.navigation.last_search_tick);
        digest.add(state.navigation.flags);
        for (const auto& point : state.navigation.points)
            for (const auto value : point)
                digest.add(static_cast<uint16_t>(value));
        const auto* goal = state.navigation.goal;
        digest.add(goal != nullptr);
        if (goal) {
            digest.add(static_cast<uint8_t>(goal->shape));
            digest.add(static_cast<uint16_t>(goal->cell[0]));
            digest.add(static_cast<uint16_t>(goal->cell[1]));
            digest.add(static_cast<uint32_t>(goal->tolerance));
            digest.add(static_cast<uint32_t>(goal->radius_squared));
            digest.add(static_cast<uint32_t>(goal->inner_range));
            digest.add(static_cast<uint32_t>(goal->inner_radius_squared));
            digest.add(static_cast<uint32_t>(goal->left));
            digest.add(static_cast<uint32_t>(goal->right));
            digest.add(static_cast<uint32_t>(goal->top));
            digest.add(static_cast<uint32_t>(goal->bottom));
            digest.add(goal->order ? goal->order->raised_events : 0);
        }
        const auto* head = match.local_flow_head(id);
        digest.add(head != nullptr);
        if (head) {
            std::array<uint8_t, movement::local_flow_head_bytes> bytes{};
            CHECK(movement::write_local_flow_heads({head, 1}, bytes));
            for (const auto byte : bytes)
                digest.add(byte);
        }
    }
    const auto rules = match.fold_rule_state(digest.value);
    digest.add(static_cast<uint32_t>(rules));
    digest.add(static_cast<uint32_t>(rules >> 32));
    if (digest.trace)
        std::cout << '\n';
}

/// Replays ordinary corners, a physical traffic clamp and a known water corner.
uint64_t legacy_trajectory(uint32_t version, uint32_t scene, bool trace) {
    Options options;
    options.sight_cells = 9;
    Fixture fixture(options);
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    fixture.fields[1].yard_mask = yard;
    configure_ground(fixture, version, scene == 2, false);
    auto& blocker = fixture.spawn(0, 96, 64);
    auto& mover = fixture.spawn(0, 64, 64);
    auto& match = *fixture.match;
    blocker.record.heading = west;
    mover.record.heading = east;
    CHECK(!match.order_refused(
        match.issue_ground_move(mover.unit_index, {208 * pixel, 0, 128 * pixel}, false)
    ));
    fixture.run(1);
    auto& state = *match.ground_runtime(mover.unit_index);
    movement::LocalFlowHead restored{};
    if (const auto* head = match.local_flow_head(mover.unit_index)) {
        restored = *head;
        CHECK(restored.active && restored.goal_key != 0);
    }
    const bool clamp = scene == 1;
    match.place_unit_at(
        blocker.unit_index,
        (clamp ? 136 : 224) * pixel,
        60 * pixel,
        (clamp ? 64 : 224) * pixel,
        ground_occupancy
    );
    match.place_unit_at(
        mover.unit_index,
        clamp ? 120 * pixel - 1 : 64 * pixel + 3 * pixel / 8,
        60 * pixel,
        clamp ? 64 * pixel : 64 * pixel + 5 * pixel / 8,
        ground_occupancy
    );
    mover.record.heading = east;
    state.project_slot();
    state.movement.speed = maximum_speed;
    state.movement.velocity = movement::aim_velocity(east, 0, maximum_speed);
    state.movement.turn = 0;
    state.movement.flags = ground_occupancy;
    state.movement.last_motion_tick = corner_tick - 40;
    state.navigation.count = 4;
    state.navigation.points = {};
    state.navigation.points[0] = clamp ? ground::RoutePoint{80, 64} : ground::RoutePoint{32, 64};
    state.navigation.points[1] = clamp ? ground::RoutePoint{144, 64} : ground::RoutePoint{96, 64};
    state.navigation.points[2] = clamp ? ground::RoutePoint{144, 128} : ground::RoutePoint{96, 128};
    state.navigation.points[3] = {208, 128};
    state.navigation.flags = ground::route_present_flag | ground::search_pending_flag;
    state.navigation.last_search_tick = corner_tick - 1;
    if (version >= 2) {
        restored.active = true;
        restored.count = 3;
        restored.mobile_blocked = true;
        restored.blocked_ticks = 31;
        restored.tick = corner_tick - 1;
        restored.points =
            clamp ? std::array<ground::RoutePoint, 3>{{{119, 64}, {135, 64}, {144, 128}}}
                  : std::array<ground::RoutePoint, 3>{{{64, 64}, {80, 80}, {96, 128}}};
        restore_head(match, mover.unit_index, restored);
    }
    match.simulation().tick = corner_tick - 1;
    LegacyDigest digest;
    digest.trace = trace;
    constexpr uint32_t trajectory_ticks = 360;
    for (uint32_t tick = 0; tick < trajectory_ticks; ++tick) {
        fixture.run(1);
        record_legacy_tick(digest, match, {blocker.unit_index, mover.unit_index});
    }
    if (trace)
        std::cout << "legacy " << version << ' ' << scene << ' ' << std::hex << digest.value
                  << std::dec << '\n';
    return digest.value;
}

/// Pins all trajectory words to the accepted pre-corner behavior of presets 0, 1 and 2.
void legacy_trajectories_remain_exact() {
    // Each row covers 360 real ticks of both stock units, including all route
    // points, goal fields, native motion clocks and canonical saved rule state.
    // Update only when an agreed legacy behavior change explains the difference.
    constexpr std::array<std::array<uint64_t, 3>, 3> expected{
        {{0xee445ee95c8c1bc6ull, 0xd26438bc5520bdd8ull, 0x9c8da14e224b3672ull},
         {0xee445ee95c8c1bc6ull, 0xd26438bc5520bdd8ull, 0x9c8da14e224b3672ull},
         {0x89b0c0c6c6b37ab6ull, 0x2934a602980d13dcull, 0xc4067e3b0bb5713full}}
    };
    for (uint32_t version = 0; version < expected.size(); ++version)
        for (uint32_t scene = 0; scene < expected[version].size(); ++scene)
            CHECK(legacy_trajectory(version, scene, false) == expected[version][scene]);
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--legacy-traces") {
        for (uint32_t version = 0; version < 3; ++version)
            for (uint32_t scene = 0; scene < 3; ++scene)
                (void)legacy_trajectory(version, scene, true);
        return 0;
    }

    try {
        legacy_trajectories_remain_exact();
        eligibility_and_legacy_controls();
        known_static_clearance_is_required();
        active_head_and_native_collision_are_preserved();
        std::cout << "live route corner: ok\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
