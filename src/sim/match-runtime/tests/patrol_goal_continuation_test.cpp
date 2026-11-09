// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Patrol polling retains an unchanged owned ground goal only in battle-flow
// rules, while native mission events, movement and saved continuation stay intact.
#include "combat_fixture.hpp"
#include "saved_game.hpp"
#include "support/match_tick_access.hpp"

#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/ground_orders/goals.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/match_runtime/enhanced_counters.hpp"
#include "oa/sim/match_runtime/match_trace.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

namespace {
using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;
namespace ground = oa::sim::ground_orders;
namespace movement = oa::sim::unit_movement;

constexpr int32_t pixel = 65536;
constexpr uint32_t battle_version = oa::data::enhanced_mode::battle_flow_version;
constexpr uint32_t legacy_version = 4;
constexpr uint32_t map_height = 24;
constexpr uint8_t land_height = 60;
constexpr uint8_t sea_level = 30;
constexpr uint8_t ground_layer = 1;
constexpr uint8_t patrol_kind = 29;
constexpr uint8_t cancel_event = 2;
constexpr uint32_t next_phase = 1;
constexpr uint32_t patrol_wait = 15;
constexpr uint16_t east = 49152;
constexpr int32_t maximum_speed = 117964;
constexpr int32_t acceleration = 6553;
constexpr uint16_t maximum_turn = 1120;
constexpr uint32_t long_ticks = 1800;

enum class Driver { local, remote, mirrored, carried, aircraft, other_layer };

struct Setup {
    uint32_t version = battle_version;
    Driver driver = Driver::local;
    bool bridge{};
};

Options options_for(const Setup& setup) {
    Options options;
    options.sight_cells = setup.bridge ? 25 : 9;
    return options;
}

bool bridge_land(uint32_t x, uint32_t z) {
    return (
        (((x >= 1 && x <= 4) || (x >= 11 && x <= 14)) && z >= 2 && z <= 13) ||
        (x >= 5 && x <= 10 && z >= 6 && z <= 9)
    );
}

struct Scene {
    Fixture fixture;
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    uint32_t width{};
    uint16_t actor{};
    ground::Point destination{};

    explicit Scene(const Setup& setup = {}) : fixture(options_for(setup)) {
        fixture.match.reset();
        width = setup.bridge ? 24 : 384;
        fixture.map.attribute_width = width;
        fixture.map.attribute_height = map_height;
        fixture.map.sea_level = sea_level;
        fixture.map.attributes.resize(size_t(width) * map_height);
        fixture.terrain_values.resize(size_t(width) * map_height);
        fixture.collision_plots.resize(size_t(width) * map_height);
        fixture.altitude_cells.resize(size_t(width / 2) * (map_height / 2));
        for (uint32_t z = 0; z < map_height; ++z)
            for (uint32_t x = 0; x < width; ++x) {
                const bool land = !setup.bridge || bridge_land(x, z) ||
                                  (x != 0 && bridge_land(x - 1, z)) ||
                                  (z != 0 && bridge_land(x, z - 1)) ||
                                  (x != 0 && z != 0 && bridge_land(x - 1, z - 1));
                fixture.map.attributes[size_t(z) * width + x].height = land ? land_height : 0;
            }
        for (uint32_t z = 0; z < map_height; ++z)
            for (uint32_t x = 0; x < width; ++x) {
                auto& plot = fixture.collision_plots[size_t(z) * width + x];
                plot.low_height = plot.high_height =
                    fixture.map.attributes[size_t(z) * width + x].height;
                for (uint32_t dz = 0; dz < 2 && z + dz < map_height; ++dz)
                    for (uint32_t dx = 0; dx < 2 && x + dx < width; ++dx) {
                        const auto height =
                            fixture.map.attributes[size_t(z + dz) * width + x + dx].height;
                        plot.low_height = std::min(plot.low_height, height);
                        plot.high_height = std::max(plot.high_height, height);
                    }
            }
        fixture.types[1].footprint_x = fixture.types[1].footprint_z = 2;
        if (setup.driver == Driver::aircraft) {
            fixture.types[1].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_FLY;
            fixture.def.can_fly = true;
            fixture.def.cruise_altitude = 80;
        }
        fixture.loaded[1].type = fixture.types[1];
        fixture.fields[1].movement_class = 1;
        fixture.fields[1].yard_mask = yard;
        fixture.metadata.footprint_x = fixture.metadata.footprint_z = 2;
        fixture.metadata.min_water_depth = -10000;
        fixture.metadata.max_water_depth = 0;
        fixture.metadata.max_slope = fixture.metadata.bad_slope = 255;
        fixture.metadata.max_water_slope = fixture.metadata.bad_water_slope = 255;
        fixture.def.max_velocity_fixed = maximum_speed;
        fixture.def.acceleration_fixed = fixture.def.brake_rate_fixed = acceleration;
        fixture.def.turn_rate = maximum_turn;
        runtime::OfflineInputs inputs{
            fixture.map,
            fixture.loaded,
            fixture.types,
            fixture.fields,
            fixture.weapons,
            fixture.terrain_values,
            fixture.masks,
            static_cast<int32_t>(width / 2),
            map_height / 2,
            4,
            2,
            0,
            sea_level,
            1,
            &fixture.scenario,
            {},
            fixture.collision_plots,
            oa::sim::visibility_state::AltitudeSightData{
                static_cast<int32_t>(width / 2),
                map_height / 2,
                fixture.altitude_cells,
                fixture.altitude_patterns
            }
        };
        inputs.rules.setup.enhanced_mode.enabled = setup.version != 0;
        inputs.rules.setup.enhanced_mode.version = setup.version;
        inputs.unit_player_mask = 3;
        fixture.match = std::make_unique<runtime::Match>(inputs, fixture.services);
        match().configure_strategic_environment({0, 0.5F, 0});
        for (uint8_t owner = 0; owner < 2; ++owner) {
            match().simulation().players[owner].present = true;
            match().simulation().players[owner].status = 1;
            std::array<uint8_t, OA_PLAYER_COUNT> allies{};
            allies[owner] = 1;
            match().configure_player_alliances(owner, allies);
            if (owner == 0)
                match().configure_outcomes(owner, allies, false);
        }
        auto& unit = fixture.spawn(0, setup.bridge ? 48 : 64, 128);
        actor = unit.unit_index;
        match().place_unit_at(
            actor, (setup.bridge ? 48 : 64) * pixel, land_height * pixel, 128 * pixel, ground_layer
        );
        unit.record.heading = east;
        destination = {(setup.bridge ? 224 : 6000) * pixel, 0, 128 * pixel};
        CHECK(match().fault() == nullptr);
    }

    runtime::Match& match() { return *fixture.match; }

    sim::unit_spawn::Slot& unit() { return match().spawn_slot(actor); }

    ground::GroundRuntime& movement() {
        auto* state = match().ground_runtime(actor);
        CHECK(state != nullptr);
        return *state;
    }

    void run(uint32_t ticks = 1) { fixture.run(ticks); }

    sim::simulation_state::Order& patrol() {
        auto& order = match().issue_patrol(actor, destination, false);
        CHECK(order.kind == patrol_kind);
        return order;
    }

    void begin() {
        patrol();
        for (uint32_t tick = 0; tick < 8 && movement().navigation.goal == nullptr; ++tick)
            run();
        CHECK(movement().navigation.goal != nullptr);
        CHECK(unit().unit->primary != nullptr && unit().unit->primary->kind == patrol_kind);
    }
};

struct GoalState {
    ground::GoalShape shape{};
    std::array<int16_t, 2> cell{};
    int32_t tolerance{}, radius_squared{}, inner_range{}, inner_radius_squared{};
    int32_t left{}, right{}, top{}, bottom{};
    bool operator==(const GoalState&) const = default;
};

GoalState goal_state(const ground::Goal& goal) {
    return {
        goal.shape,
        goal.cell,
        goal.tolerance,
        goal.radius_squared,
        goal.inner_range,
        goal.inner_radius_squared,
        goal.left,
        goal.right,
        goal.top,
        goal.bottom
    };
}

struct NavState {
    std::array<ground::RoutePoint, ground::navigation_capacity> points{};
    uint32_t count{}, flags{}, search_tick{};
    bool operator==(const NavState&) const = default;
};

NavState nav_state(const ground::Navigation& navigation) {
    return {navigation.points, navigation.count, navigation.flags, navigation.last_search_tick};
}

std::vector<uint8_t> rule_bytes(runtime::Match& match, const char* name) {
    const auto* table = runtime::find_rule_state(match.rule_state(), name);
    if (!table)
        return {};
    CHECK(table->bytes != nullptr);
    const auto bytes = table->bytes(table->context);
    return {bytes.begin(), bytes.end()};
}

void seed_navigation(Scene& scene) {
    auto& navigation = scene.movement().navigation;
    navigation.points = {};
    navigation.points[0] = {64, 128};
    navigation.points[1] = {128, 128};
    navigation.points[2] = {256, 128};
    navigation.points[3] = {512, 128};
    navigation.count = 4;
    navigation.flags = ground::route_present_flag | ground::search_pending_flag;
    navigation.last_search_tick = 7;
    if (scene.match().rules().setup.enhanced_mode.version >= 2) {
        std::vector<movement::LocalFlowHead> heads(scene.match().state().unit_slot_count);
        heads[scene.actor].points = {{{64, 128}, {96, 144}, {128, 128}}};
        heads[scene.actor].goal_key = 123;
        heads[scene.actor].tick = scene.match().simulation().tick;
        heads[scene.actor].count = 3;
        heads[scene.actor].active = true;
        std::vector<uint8_t> bytes(heads.size() * movement::local_flow_head_bytes);
        CHECK(movement::write_local_flow_heads(heads, bytes));
        const auto* table = runtime::find_rule_state(scene.match().rule_state(), "local-flow");
        CHECK(table != nullptr && table->restore != nullptr);
        CHECK(table->restore(table->context, bytes));
    }
}

uint32_t refresh(Scene& scene, uint32_t events = 0) {
    auto* order = scene.unit().unit->primary;
    CHECK(order != nullptr && order->kind == patrol_kind);
    order->phase = 1;
    order->wait_events = 0;
    runtime::MatchTickAccess access(scene.match());
    const auto result =
        access.dispatch_mission(scene.match().state(), scene.unit().record, *order, events);
    if (result == next_phase)
        ++order->phase;
    CHECK(scene.match().fault() == nullptr);
    return result;
}

void unchanged_goal_keeps_native_navigation() {
    for (const uint32_t version : {0u, 1u, 2u, 3u, 4u, 5u, battle_version}) {
        Scene scene({version});
        scene.begin();
        scene.match().simulation().tick = 120;
        seed_navigation(scene);
        const auto* goal = scene.movement().navigation.goal;
        const auto expected_goal = goal_state(*goal);
        const auto expected_nav = nav_state(scene.movement().navigation);
        const auto expected_head = rule_bytes(scene.match(), "local-flow");
        const auto position = scene.movement().geometry.position;
        const auto velocity = scene.movement().movement.velocity;
        const auto random = scene.match().random_state();
        CHECK(refresh(scene) == next_phase);
        CHECK(goal_state(*scene.movement().navigation.goal) == expected_goal);
        CHECK(scene.movement().geometry.position == position);
        CHECK(scene.movement().movement.velocity == velocity);
        CHECK(scene.match().random_state() == random);
        CHECK(scene.unit().unit->primary->wake_tick == 120 + patrol_wait);
        if (version >= battle_version) {
            CHECK(scene.movement().navigation.goal == goal);
            CHECK(nav_state(scene.movement().navigation) == expected_nav);
            CHECK(rule_bytes(scene.match(), "local-flow") == expected_head);
        } else {
            // The new allocation exists while the previous owned Goal is still live.
            CHECK(scene.movement().navigation.goal != goal);
            CHECK(scene.movement().navigation.last_search_tick == 0);
            CHECK(nav_state(scene.movement().navigation) != expected_nav);
            CHECK((scene.movement().navigation.flags & ground::route_changed_flag) != 0);
        }
    }
}

void real_patrol_polling_preserves_one_goal_for_1800_ticks() {
    Scene legacy({legacy_version});
    Scene continued;
    legacy.patrol();
    continued.patrol();
    const ground::Goal* old_goal{};
    const ground::Goal* new_goal{};
    uint32_t legacy_restarts{}, continued_restarts{}, observed_goals{};
    for (uint32_t tick = 1; tick <= long_ticks; ++tick) {
        legacy.run();
        continued.run();
        CHECK(legacy.match().simulation().tick == tick);
        CHECK(continued.match().simulation().tick == tick);
        CHECK(legacy.match().random_state() == continued.match().random_state());
        const auto* left = legacy.unit().unit->primary;
        const auto* right = continued.unit().unit->primary;
        CHECK(
            left != nullptr && right != nullptr && left->kind == patrol_kind &&
            right->kind == patrol_kind
        );
        CHECK(left->phase == right->phase && left->wake_tick == right->wake_tick);
        CHECK(
            left->wait_events == right->wait_events && left->raised_events == right->raised_events
        );
        const auto* a = legacy.movement().navigation.goal;
        const auto* b = continued.movement().navigation.goal;
        CHECK((a != nullptr) == (b != nullptr));
        if (a && b) {
            ++observed_goals;
            CHECK(goal_state(*a) == goal_state(*b));
            CHECK(a->order == left && b->order == right);
            if (old_goal && old_goal != a)
                ++legacy_restarts;
            if (new_goal && new_goal != b)
                ++continued_restarts;
            old_goal = a;
            new_goal = b;
        }
        for (auto* scene : {&legacy, &continued}) {
            CHECK(scene->movement().movement.speed <= maximum_speed);
            CHECK(std::abs(int(scene->movement().movement.turn)) <= maximum_turn);
            CHECK(
                scene->movement().geometry.cell ==
                (std::array<int16_t, 2>{scene->unit().record.cell_x, scene->unit().record.cell_z})
            );
        }
    }
    CHECK(observed_goals >= long_ticks - 2);
    CHECK(legacy_restarts >= 20 && continued_restarts == 0);
    CHECK(continued.movement().geometry.position[0] > (64 + 500) * pixel);
    CHECK(
        legacy.movement().navigation.goal != nullptr &&
        continued.movement().navigation.goal != nullptr
    );
    CHECK(
        continued.match().enhanced_counters().full_route_searches <=
        legacy.match().enhanced_counters().full_route_searches
    );
    std::cout << "Patrol 1800 ticks: legacy restarts=" << legacy_restarts
              << ", battle-flow restarts=" << continued_restarts << '\n';
}

void changed_goal_fields_restart_the_native_goal() {
    using Change = void (*)(ground::Goal&);
    const std::array<Change, 11> changes{
        [](ground::Goal& goal) { goal.shape = ground::GoalShape::ring; },
        [](ground::Goal& goal) { ++goal.cell[0]; },
        [](ground::Goal& goal) { ++goal.cell[1]; },
        [](ground::Goal& goal) { ++goal.tolerance; },
        [](ground::Goal& goal) { ++goal.radius_squared; },
        [](ground::Goal& goal) { ++goal.inner_range; },
        [](ground::Goal& goal) { ++goal.inner_radius_squared; },
        [](ground::Goal& goal) { ++goal.left; },
        [](ground::Goal& goal) { ++goal.right; },
        [](ground::Goal& goal) { ++goal.top; },
        [](ground::Goal& goal) { ++goal.bottom; }
    };
    for (const auto change : changes) {
        Scene scene;
        scene.begin();
        scene.match().simulation().tick = 120;
        seed_navigation(scene);
        auto* previous = scene.movement().navigation.goal;
        const auto expected = goal_state(*previous);
        change(*previous);
        CHECK(refresh(scene) == next_phase);
        CHECK(scene.movement().navigation.goal != previous);
        CHECK(goal_state(*scene.movement().navigation.goal) == expected);
        CHECK(scene.movement().navigation.last_search_tick == 0);
    }
}

void terminal_events_restart_even_after_the_sweep_consumes_them() {
    for (const uint32_t event :
         {ground::arrived_event,
          ground::path_failed_event,
          ground::goal_replaced_event,
          ground::search_goal_contact_flag,
          ground::search_seed_unresolved_flag,
          uint32_t(cancel_event)})
        for (const bool pending : {false, true}) {
            Scene scene;
            scene.begin();
            scene.match().simulation().tick = 120;
            seed_navigation(scene);
            const auto* previous = scene.movement().navigation.goal;
            scene.unit().unit->primary->raised_events = pending ? event : 0;
            CHECK(refresh(scene, pending ? 0 : event) == next_phase);
            CHECK(scene.movement().navigation.goal != previous);
            CHECK(scene.movement().navigation.last_search_tick == 0);
        }
}

void foreign_goal_and_foreign_order_are_not_retained() {
    for (const bool foreign_navigation : {false, true}) {
        sim::simulation_state::Order foreign_order{};
        ground::Goal foreign_goal{};
        Scene scene;
        scene.begin();
        scene.match().simulation().tick = 120;
        seed_navigation(scene);
        auto* owned = scene.movement().navigation.goal;
        const auto expected = goal_state(*owned);
        if (foreign_navigation) {
            foreign_goal = *owned;
            foreign_goal.order = &foreign_order;
            scene.movement().navigation.goal = &foreign_goal;
        } else
            owned->order = &foreign_order;
        CHECK(refresh(scene) == next_phase);
        CHECK(scene.movement().navigation.goal != owned);
        CHECK(scene.movement().navigation.goal != &foreign_goal);
        CHECK(scene.movement().navigation.goal->order == scene.unit().unit->primary);
        CHECK(goal_state(*scene.movement().navigation.goal) == expected);
        CHECK((foreign_order.raised_events & ground::goal_replaced_event) != 0);
    }
}

void excluded_ground_states_use_the_previous_refresh() {
    for (const auto driver :
         {Driver::remote, Driver::mirrored, Driver::carried, Driver::other_layer}) {
        Scene scene;
        scene.begin();
        scene.match().simulation().tick = 120;
        seed_navigation(scene);
        const auto* previous = scene.movement().navigation.goal;
        if (driver == Driver::remote)
            scene.match().simulation().players[0].status = ground::mirrored_player_status;
        if (driver == Driver::mirrored)
            scene.movement().mirrored_driver = true;
        if (driver == Driver::carried) {
            auto& carrier = scene.fixture.spawn(0, 64, 64);
            scene.unit().record.attach_parent = oa::oa_unit_ref_from_slot(carrier.unit_index);
            scene.unit().record.attach_piece = 0xff;
        }
        if (driver == Driver::other_layer)
            scene.movement().movement.flags =
                uint8_t((scene.movement().movement.flags & ~movement::occupancy_mask) | 2);
        CHECK(refresh(scene) == next_phase);
        CHECK(scene.movement().navigation.goal != previous);
        CHECK(scene.movement().navigation.last_search_tick == 0);
    }
    for (const auto version : {legacy_version, battle_version}) {
        Scene scene({version, Driver::aircraft});
        auto& order =
            scene.match().insert_ground_order(scene.actor, patrol_kind, scene.destination);
        CHECK(order.kind == patrol_kind);
        CHECK(refresh(scene) == next_phase);
        CHECK(scene.movement().navigation.goal == nullptr);
        CHECK(scene.match().air_driver(scene.actor) != nullptr);
    }
}

void new_leg_and_cancellation_keep_normal_order_lifecycle() {
    Scene scene;
    scene.begin();
    const auto first = goal_state(*scene.movement().navigation.goal);
    scene.destination = {5504 * pixel, 0, 160 * pixel};
    scene.patrol();
    for (uint32_t tick = 0; tick < 8 && scene.movement().navigation.goal == nullptr; ++tick)
        scene.run();
    CHECK(scene.movement().navigation.goal != nullptr);
    CHECK(goal_state(*scene.movement().navigation.goal) != first);
    CHECK(scene.movement().navigation.goal->order == scene.unit().unit->primary);
    scene.match().stop_orders(scene.actor);
    CHECK(scene.movement().navigation.goal == nullptr);
    CHECK(scene.unit().unit->primary == nullptr || scene.unit().unit->primary->kind != patrol_kind);
}

} // namespace

int main() {
    uint32_t failures{};
    const auto run = [&failures](const char* name, void (*test)()) {
        try {
            test();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << name << ": " << error.what() << '\n';
        }
    };
    run("unchanged patrol goal", unchanged_goal_keeps_native_navigation);
    run("real 1800 tick patrol", real_patrol_polling_preserves_one_goal_for_1800_ticks);
    run("changed goal fields", changed_goal_fields_restart_the_native_goal);
    run("pending and consumed terminal events",
        terminal_events_restart_even_after_the_sweep_consumes_them);
    run("foreign goal ownership", foreign_goal_and_foreign_order_are_not_retained);
    run("excluded drivers and layers", excluded_ground_states_use_the_previous_refresh);
    run("new leg and cancellation", new_leg_and_cancellation_keep_normal_order_lifecycle);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
