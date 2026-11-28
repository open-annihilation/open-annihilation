// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// With shared routes, a patrol's regular step leaves a moving ground unit its
// goal and route while the goal comes out the same and no search of its own
// runs, and asks for a fresh route at once; a unit that has stood still takes
// the goal afresh. Off and a set without shared routes take the goal afresh
// at every step, as 3.1c does.
#include "combat_fixture.hpp"
#include "support/match_tick_access.hpp"

#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/ground_orders/goals.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"

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
namespace enhanced = oa::data::enhanced_mode;
using FeatureSet = enhanced::FeatureSet;

constexpr int32_t pixel = 65536;
constexpr uint32_t map_width = 384, map_height = 24;
constexpr uint8_t land_height = 60;
constexpr uint8_t sea_level = 30;
constexpr uint8_t ground_layer = 1;
constexpr uint8_t patrol_kind = 29;
constexpr uint32_t next_phase = 1;
constexpr uint32_t patrol_wait = 15;
constexpr uint32_t replan_ticks = 30;
constexpr uint16_t east = 49152;
constexpr int32_t maximum_speed = 117964;
constexpr int32_t acceleration = 6553;
constexpr uint16_t maximum_turn = 1120;
constexpr uint32_t step_tick = 120;

/// Off, capacity alone, local traffic without shared routes, and Revision 1.
const std::array<FeatureSet, 4> feature_sets{
    FeatureSet{},
    enhanced::close_features(FeatureSet{}.with(enhanced::Feature::capacity)),
    enhanced::close_features(FeatureSet{}.with(enhanced::Feature::local_flow)),
    enhanced::revision_features(1),
};

/// Tells whether a set keeps a patrol's route.
///
/// @param features the set
/// @return true when it holds routes_fine_regions
bool keeps_routes(const FeatureSet& features) {
    return features.has(enhanced::Feature::routes_fine_regions);
}

/// One ground unit on a flat strip of land, patrolling east.
struct Scene {
    Fixture fixture;
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    uint16_t actor{};
    ground::Point destination{};

    /// Builds the strip, with `wall` a ridge of water across it at columns
    /// 200 to 203 whose one gap, rows 11 and 12, only the ordinary search
    /// finds: the shared routes' coarse nodes lie on even rows.
    ///
    /// @param features the match's set
    /// @param wall whether the water ridge crosses the strip
    explicit Scene(const FeatureSet& features, bool wall = false) {
        fixture.match.reset();
        fixture.map.attribute_width = map_width;
        fixture.map.attribute_height = map_height;
        fixture.map.sea_level = sea_level;
        fixture.map.attributes.resize(size_t(map_width) * map_height);
        fixture.terrain_values.resize(size_t(map_width) * map_height);
        fixture.collision_plots.resize(size_t(map_width) * map_height);
        fixture.altitude_cells.resize(size_t(map_width / 2) * (map_height / 2));
        for (uint32_t z = 0; z < map_height; ++z)
            for (uint32_t x = 0; x < map_width; ++x) {
                const bool water = wall && x >= 200 && x <= 203 && z != 11 && z != 12;
                const auto height = water ? uint8_t{0} : land_height;
                fixture.map.attributes[size_t(z) * map_width + x].height = height;
                auto& plot = fixture.collision_plots[size_t(z) * map_width + x];
                plot.low_height = plot.high_height = height;
            }
        fixture.types[1].footprint_x = fixture.types[1].footprint_z = 2;
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
            static_cast<int32_t>(map_width / 2),
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
                static_cast<int32_t>(map_width / 2),
                map_height / 2,
                fixture.altitude_cells,
                fixture.altitude_patterns
            }
        };
        enhanced::store_choice(
            inputs.rules.setup.enhanced_mode,
            features.empty() ? enhanced::Choice{} : enhanced::Choice{1, features}
        );
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
        // The players know the whole strip, the ridge included.
        auto& sight = match().sight_mutable().player_bits;
        std::fill(sight.begin(), sight.end(), uint16_t{3});
        auto& unit = fixture.spawn(0, 64, 128);
        actor = unit.unit_index;
        match().place_unit_at(actor, 64 * pixel, land_height * pixel, 128 * pixel, ground_layer);
        unit.record.heading = east;
        destination = {6000 * pixel, 0, 128 * pixel};
        CHECK(match().fault() == nullptr);
        CHECK(match().enhanced_features() == features);
    }

    runtime::Match& match() { return *fixture.match; }

    oa::sim::unit_spawn::Slot& unit() { return match().spawn_slot(actor); }

    ground::GroundRuntime& movement() {
        auto* state = match().ground_runtime(actor);
        CHECK(state != nullptr);
        return *state;
    }

    void run(uint32_t ticks = 1) { fixture.run(ticks); }

    /// Orders the patrol and runs until its first step has given the unit a
    /// goal and, unless `searching`, until no route search of its own runs.
    ///
    /// @param searching whether a running search of the unit may be left running
    void begin(bool searching = false) {
        CHECK(match().issue_patrol(actor, destination, false).kind == patrol_kind);
        for (uint32_t tick = 0; tick < 8 && movement().navigation.goal == nullptr; ++tick)
            run();
        for (uint32_t tick = 0; !searching && tick < 240 && search_running(); ++tick)
            run();
        CHECK(movement().navigation.goal != nullptr);
        CHECK(unit().unit->primary != nullptr && unit().unit->primary->kind == patrol_kind);
        CHECK(searching || !search_running());
    }

    /// Tells whether the unit's own route search is running.
    ///
    /// @return true when the match's search works for the unit's navigator
    bool search_running() {
        return runtime::MatchTickAccess::active_path_search(match()).navigation ==
               &movement().navigation;
    }
};

/// The parts of a navigator's route a patrol step keeps or replaces.
struct RouteState {
    std::array<ground::RoutePoint, ground::navigation_capacity> points{};
    uint32_t count{}, flags{};
    bool operator==(const RouteState&) const = default;
};

RouteState route_state(const ground::Navigation& navigation) {
    return {navigation.points, navigation.count, navigation.flags};
}

/// Gives the unit a four-point route, a pending search last allowed at tick 7,
/// and a last motion `still_for` ticks before the step.
void seed_navigation(Scene& scene, uint32_t still_for) {
    scene.match().simulation().tick = step_tick;
    auto& navigation = scene.movement().navigation;
    navigation.points = {};
    navigation.points[0] = {64, 128};
    navigation.points[1] = {128, 128};
    navigation.points[2] = {256, 128};
    navigation.points[3] = {512, 128};
    navigation.count = 4;
    navigation.flags = ground::route_present_flag | ground::search_pending_flag;
    navigation.last_search_tick = 7;
    scene.movement().movement.last_motion_tick = step_tick - still_for;
}

/// Runs the patrol's goal step (phase 1) with the given waking events.
uint32_t patrol_step(Scene& scene, uint32_t events = 0) {
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

/// Checks that a step replaced the goal and set the unit on a straight leg
/// with its search clock cleared, as 3.1c's step does.
void check_replaced(Scene& scene, const ground::Goal* previous) {
    const auto& navigation = scene.movement().navigation;
    CHECK(navigation.goal != nullptr && navigation.goal != previous);
    CHECK(navigation.last_search_tick == 0);
    CHECK(navigation.count == 2);
    CHECK((navigation.flags & ground::route_changed_flag) != 0);
}

/// A moving unit keeps its goal and route with shared routes and searches
/// again at once; every other set takes the goal afresh.
void an_unchanged_step_keeps_a_moving_unit_route() {
    for (const auto& features : feature_sets) {
        Scene scene(features);
        scene.begin();
        seed_navigation(scene, 1);
        const auto* goal = scene.movement().navigation.goal;
        const auto expected = route_state(scene.movement().navigation);
        const auto position = scene.movement().geometry.position;
        const auto random = scene.match().random_state();
        CHECK(patrol_step(scene) == next_phase);
        CHECK(scene.movement().geometry.position == position);
        CHECK(scene.match().random_state() == random);
        CHECK(scene.unit().unit->primary->wake_tick == step_tick + patrol_wait);
        if (keeps_routes(features)) {
            // The same route, still unchanged for the network, and a search
            // pending whose clock restarted.
            CHECK(scene.movement().navigation.goal == goal);
            CHECK(route_state(scene.movement().navigation) == expected);
            CHECK(scene.movement().navigation.last_search_tick == 0);
        } else {
            check_replaced(scene, goal);
        }
    }
}

/// A unit that has not moved for 30 ticks takes the goal afresh.
void a_unit_that_stood_still_plans_again() {
    const auto revision_1 = enhanced::revision_features(1);
    for (const uint32_t still_for : {replan_ticks - 1, replan_ticks, replan_ticks * 4}) {
        Scene scene(revision_1);
        scene.begin();
        seed_navigation(scene, still_for);
        const auto* goal = scene.movement().navigation.goal;
        CHECK(patrol_step(scene) == next_phase);
        if (still_for < replan_ticks)
            CHECK(scene.movement().navigation.goal == goal);
        else
            check_replaced(scene, goal);
    }
}

/// A changed goal, a goal event, or a route too short to follow takes the goal
/// afresh.
void changed_goals_and_events_take_the_goal_afresh() {
    const auto revision_1 = enhanced::revision_features(1);
    using Change = void (*)(ground::Goal&);
    const std::array<Change, 4> changes{
        [](ground::Goal& goal) { ++goal.cell[0]; },
        [](ground::Goal& goal) { ++goal.cell[1]; },
        [](ground::Goal& goal) { ++goal.tolerance; },
        [](ground::Goal& goal) { ++goal.radius_squared; },
    };
    for (const auto change : changes) {
        Scene scene(revision_1);
        scene.begin();
        seed_navigation(scene, 1);
        auto* previous = scene.movement().navigation.goal;
        change(*previous);
        CHECK(patrol_step(scene) == next_phase);
        check_replaced(scene, previous);
    }
    for (const uint32_t event : {ground::arrived_event, ground::path_failed_event}) {
        Scene scene(revision_1);
        scene.begin();
        seed_navigation(scene, 1);
        const auto* previous = scene.movement().navigation.goal;
        scene.unit().unit->primary->raised_events |= event;
        CHECK(patrol_step(scene) == next_phase);
        check_replaced(scene, previous);
    }
    Scene scene(revision_1);
    scene.begin();
    seed_navigation(scene, 1);
    scene.movement().navigation.count = 1;
    const auto* previous = scene.movement().navigation.goal;
    CHECK(patrol_step(scene) == next_phase);
    check_replaced(scene, previous);
}

/// A step while the unit's own route search runs, here through a gap the
/// shared routes cannot use, cancels it and takes the goal afresh, as without
/// shared routes.
void a_running_search_is_restarted() {
    Scene scene(enhanced::revision_features(1), true);
    scene.begin(true);
    // A small search credit spreads the search over many ticks.
    scene.match().path_search_jobs().tick_credit = 8;
    ground::SearchRecord running{};
    for (uint32_t tick = 0; tick < 240 && running.unit != scene.unit().unit; ++tick) {
        scene.run();
        running = runtime::MatchTickAccess::active_path_search(scene.match());
    }
    CHECK(running.unit == scene.unit().unit);
    CHECK(running.navigation == &scene.movement().navigation);
    const auto* previous = scene.movement().navigation.goal;
    scene.movement().movement.last_motion_tick = scene.match().simulation().tick;
    CHECK(patrol_step(scene) == next_phase);
    CHECK(scene.movement().navigation.goal != previous);
    CHECK(runtime::MatchTickAccess::active_path_search(scene.match()).unit == nullptr);
}

/// A patrol played tick by tick keeps one goal while its unit marches with
/// shared routes, and takes a new one at every step without them; the unit
/// gets as far either way.
void a_marching_patrol_keeps_one_goal() {
    constexpr uint32_t ticks = 1800;
    Scene plain(FeatureSet{});
    Scene kept(enhanced::revision_features(1));
    for (auto* scene : {&plain, &kept})
        scene->begin();
    const ground::Goal* plain_goal = plain.movement().navigation.goal;
    const ground::Goal* kept_goal = kept.movement().navigation.goal;
    uint32_t plain_steps = 0, kept_steps = 0;
    for (uint32_t tick = 0; tick < ticks; ++tick) {
        plain.run();
        kept.run();
        const auto* a = plain.movement().navigation.goal;
        const auto* b = kept.movement().navigation.goal;
        if (a && a != plain_goal) {
            ++plain_steps;
            plain_goal = a;
        }
        if (b && b != kept_goal) {
            ++kept_steps;
            kept_goal = b;
        }
        CHECK(kept.match().fault() == nullptr);
    }
    CHECK(plain_steps >= 20);
    CHECK(kept_steps <= 3);
    // Both march about as far east.
    const int64_t start = 64 * pixel;
    const auto plain_x = plain.movement().geometry.position[0] - start;
    const auto kept_x = kept.movement().geometry.position[0] - start;
    CHECK(plain_x > 1000 * pixel && kept_x * 10 >= plain_x * 9);
    std::cout << "patrol over " << ticks << " ticks: goals taken afresh " << plain_steps
              << " without shared routes, " << kept_steps << " with\n";
}

} // namespace

int main() {
    try {
        an_unchanged_step_keeps_a_moving_unit_route();
        a_unit_that_stood_still_plans_again();
        changed_goals_and_events_take_the_goal_afresh();
        a_running_search_is_restarted();
        a_marching_patrol_keeps_one_goal();
        std::cout << "Patrol route keep: steps keep moving units' routes with shared routes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
