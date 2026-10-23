// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/ground_orders/ground_runtime.hpp"
#include "oa/sim/ground_orders/movement_map.hpp"
#include "oa/sim/ground_orders/search_trace.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << "failed: " #x << '\n';                                                    \
            std::abort();                                                                          \
        }                                                                                          \
    } while (false)
using namespace oa::sim::ground_orders;

struct Fixture : Host {
    unsigned sounds{}, weapons{}, cancels{};
    uint32_t random_value{};
    oa::sim::simulation_state::Unit* target{};
    bool attack_result{};

    void cancel_search(Navigation&) override { ++cancels; }

    void play_sound(oa::sim::simulation_state::Unit&, uint32_t category) override {
        sounds = sounds * 10 + category;
    }

    void wake_weapon(oa::sim::simulation_state::Unit&, uint32_t slot) override {
        weapons = weapons * 10 + slot + 1;
    }

    oa::sim::simulation_state::Unit* find_target(oa::sim::simulation_state::Unit&) override {
        return target;
    }

    bool issue_attack(oa::sim::simulation_state::Unit&, oa::sim::simulation_state::Unit&) override {
        return attack_result;
    }

    uint32_t random(uint32_t) override { return random_value; }
};

struct MapFixture : MovementMapSampler {
    unsigned samples{};
    bool open{};

    uint8_t classify_cell(int32_t x, int32_t z) override {
        ++samples;
        return open ? 3 : static_cast<uint8_t>((x + z) & 3);
    }
};

int main() {
    oa::sim::unit_spawn::LegacyWorld world(2);
    auto& state = world.unit(1);
    oa::sim::simulation_state::Order order;
    OrderState extra;
    oa::sim::unit_movement::Unit unit;
    oa::sim::unit_movement::Movement motion;
    Navigation nav;
    std::array<uint8_t, 3> weapons{2, 0x12, 2};
    Fixture host;
    UnitView view{state, unit, motion, nav, false, weapons};
    state.object_present = true;
    state.primary = &order;
    unit.footprint = {1, 1};
    unit.position = {8 * 65536, 0, 8 * 65536};
    extra.destination = {88 * 65536, 0, 8 * 65536};
    extra.command_flags = order_announce_flag;
    CHECK(move_ground(view, order, extra, 0, 10, host) == 1);
    CHECK(host.sounds == 5 && order.wait_events == 0xe0 && nav.count == 2 && nav.needs_search());
    CHECK(nav.points[0][0] == 8 && nav.points[1][0] == 88);
    CHECK(extra.goal && extra.goal->cell[0] == 5);
    CHECK(goal_cost(*extra.goal, 0, 0) == 86);
    CHECK(!goal_contains(*extra.goal, 0, 0));
    {
        oa::sim::unit_movement::Unit builder;
        builder.footprint = {2, 2};
        builder.cell = {0, 10};
        oa::sim::simulation_state::Order build_order;
        const Point site{0x200000, 0, 0x200000};
        const auto build_goal = make_build_goal(build_order, builder, site, 4, 4);
        CHECK(build_goal.shape == GoalShape::outline);
        CHECK(build_goal.left == -2 && build_goal.top == -2);
        CHECK(build_goal.right == 4 && build_goal.bottom == 4);
        CHECK(!goal_contains(build_goal, 1, 1)); // inside the site
        CHECK(goal_contains(build_goal, -2, 0)); // left border
        CHECK(goal_contains(build_goal, 3, 4));  // bottom border
        CHECK(!goal_contains(build_goal, 5, 4)); // beyond the corner
        int goal_cells = 0;
        for_each_goal_cell(build_goal, [&](int32_t, int32_t) { ++goal_cells; });
        CHECK(goal_cells == 7 * 2 + 5 * 2);
        CHECK(goal_cost(build_goal, 1, 1) == 3 * 16);  // interior: nearest edge
        CHECK(goal_cost(build_goal, -5, 0) == 3 * 16); // beside: straight distance
        CHECK(goal_cost(build_goal, -5, -4) == 3 * 16 + 2 * 6);
        const auto edge = goal_position(build_goal, builder);
        CHECK(edge[0] == static_cast<Fixed>((2 + 1 * 2) * 0x80000));
        CHECK(edge[2] == static_cast<Fixed>((2 + 4 * 2) * 0x80000));
        Point air{0x280000, 0, 0x180000};
        snap_to_footprint_centre(air, 1, 1);
        CHECK(air[0] == static_cast<Fixed>((1u + 2u * 2u) * 0x80000u));
        CHECK(air[2] == static_cast<Fixed>((1u + 1u * 2u) * 0x80000u));
        oa::sim::simulation_state::Order ring_order;
        oa::sim::unit_movement::Unit ring_unit;
        ring_unit.footprint = {1, 1};
        ring_unit.cell = {20, 0};
        ring_unit.position = {20 * 16 * 65536, 0, 0};
        const auto ring = make_ring_goal(ring_order, ring_unit, {8 * 65536, 0, 8 * 65536}, 160, 80);
        CHECK(ring.shape == GoalShape::ring && ring.cell[0] == 0 && ring.cell[1] == 0);
        CHECK(ring.radius_squared == 100 && ring.inner_radius_squared == 25);
        CHECK(!goal_contains(ring, 20, 0));
        CHECK(!goal_contains(ring, 4, 0));
        CHECK(goal_contains(ring, 6, 0));
        CHECK(goal_cost(ring, 20, 0) == 20 * 18 - 160);
        CHECK(goal_cost(ring, 1, 0) == 80 - 18);
        CHECK(goal_cost(ring, 6, 0) == 0);
        int ring_cells = 0;
        for_each_goal_cell(ring, [&](int32_t x, int32_t z) {
            CHECK(x == 0 && z == (160 + 80) / 32);
            ++ring_cells;
        });
        CHECK(ring_cells == 1);
        // Mean range (120 units) from the centre along the bearing toward the unit (+X).
        const auto ring_point = goal_position(ring, ring_unit);
        CHECK(ring_point[2] >= 0x80000 - 5 * 65536 && ring_point[2] <= 0x80000 + 5 * 65536);
        CHECK(ring_point[0] < 0x80000 - 100 * 65536 || ring_point[0] > 0x80000 + 100 * 65536);
    }
    {
        // Kinds the ground goals report.
        static_assert(static_cast<int>(GoalShape::circle) == 4);
        static_assert(static_cast<int>(GoalShape::ring) == 5);
        static_assert(static_cast<int>(GoalShape::outline) == 6);

        // goal_contains_unit reads the unit's cell (Unit.cell_x, Unit.cell_z); both squared
        // bounds are inclusive (25 and 100 cells^2 here).
        oa::sim::simulation_state::Order ring_order;
        oa::sim::unit_movement::Unit ring_unit;
        ring_unit.footprint = {1, 1};
        const auto ring = make_ring_goal(ring_order, ring_unit, {8 * 65536, 0, 8 * 65536}, 160, 80);
        for (const auto [x, inside] :
             {std::array{4, 0}, std::array{5, 1}, std::array{10, 1}, std::array{11, 0}}) {
            ring_unit.cell = {static_cast<int16_t>(x), 0};
            CHECK(goal_contains_unit(ring, ring_unit) == (inside != 0));
        }

        // Ring goal cells: the mean-range offset divides by 0x20 toward zero and is
        // added to the centre row as a 16-bit word.
        Goal wide{};
        wide.shape = GoalShape::ring;
        wide.cell = {3, 0x7fff};
        wide.tolerance = 40;
        std::array<int32_t, 2> cell{};
        for_each_goal_cell(wide, [&](int32_t x, int32_t z) { cell = {x, z}; });
        CHECK(cell[0] == 3 && cell[1] == -0x8000);
        wide.tolerance = -40;
        for_each_goal_cell(wide, [&](int32_t x, int32_t z) { cell = {x, z}; });
        CHECK(cell[1] == 0x7ffe);

        // Outline goal cells: rows (left..right at top, then bottom) before the side
        // columns of the inner rows.
        Goal border{};
        border.shape = GoalShape::outline;
        border.right = 2;
        border.bottom = 3;
        std::array<std::array<int32_t, 2>, 10> cells{};
        std::size_t count = 0;
        for_each_goal_cell(border, [&](int32_t x, int32_t z) {
            CHECK(count < cells.size());
            cells[count++] = {x, z};
        });
        constexpr std::array<std::array<int32_t, 2>, 10> expected_cells{
            {{0, 0}, {0, 3}, {1, 0}, {1, 3}, {2, 0}, {2, 3}, {0, 1}, {2, 1}, {0, 2}, {2, 2}}
        };
        CHECK(count == cells.size() && cells == expected_cells);

        // Outline goal position: the column mean divides toward zero and the bottom row is
        // taken as a 16-bit word; 16.16 results wrap.
        oa::sim::unit_movement::Unit builder;
        builder.footprint = {2, 2};
        border.left = -3;
        border.right = 0;
        border.bottom = 0x12345;
        const auto edge = goal_position(border, builder);
        CHECK(edge[0] == 0 && edge[1] == 0);
        CHECK(edge[2] == static_cast<Fixed>(0x34600000));
    }
    unit.type.maximum_turn = 1024;
    unit.type.maximum_speed = 65536;
    steer_ground(unit, motion, nav, 4096, 4096, 0, FastUnitSteering::as_3_1c);
    CHECK(motion.speed >= 0 && motion.turn == -1024);
    unit.cell = {5, 0};
    unit.position = {88 * 65536, 0, 8 * 65536};
    tick_navigation(view, 11, host);
    CHECK(nav.goal == nullptr && (order.raised_events & arrived_event));
    order.phase = 1;
    CHECK(move_ground(view, order, extra, arrived_event, 11, host) == 5 && host.sounds == 56);
    order = {};
    CHECK(standby(view, order, 0xffffffffu, host) == 1);
    CHECK(order.wake_tick == 0 && order.wait_events == 0x10001 && host.weapons == 13);
    CHECK(weapons[0] == 0x12 && weapons[2] == 0x12);
    order.phase = 1;
    host.random_value = 29;
    CHECK(standby(view, order, 100, host) == 2 && order.wake_tick == 159);
    state.flags = 0x200000;
    host.target = &state;
    host.attack_result = true;
    CHECK(standby(view, order, 100, host) == 5);
    auto& type = world.types()[1].simulation;
    state.type = &type;
    auto& slot = world.slot(1);
    slot.record.footprint_x = 1;
    slot.record.footprint_z = 1;
    slot.record.cell_x = 3;
    slot.record.cell_z = 2;
    slot.yaw = 100;
    oa::data::unit_definitions::UnitDefinition definition;
    definition.max_velocity_fixed = 123;
    definition.turn_rate = 8;
    definition.acceleration_fixed = 9;
    definition.brake_rate_fixed = 7;
    GroundRuntime runtime(slot, definition, 42);
    CHECK(
        runtime.movement.flags == 1 && runtime.navigation.flags == 8 &&
        runtime.movement.speed == 0 && runtime.movement_class == 42
    );
    CHECK(
        runtime.geometry.cell[0] == 3 && runtime.geometry.cell[1] == 2 &&
        runtime.geometry.heading == 100 && runtime.acceleration == 9
    );
    runtime.geometry.position[0] = -123;
    runtime.write_slot();
    CHECK(state.position[0] == uint32_t(-123));
    type.flags = 0x800;
    GroundRuntime aircraft(slot, definition);
    CHECK(aircraft.movement.flags == 1 && aircraft.geometry.type.flags == 0x800);
    Navigation empty;
    motion.speed = 4096;
    steer_ground(unit, motion, empty, 4096, 4096, 0, FastUnitSteering::as_3_1c);
    CHECK(motion.speed == 0 && motion.turn == 0);
    Navigation searched;
    searched.flags = search_pending_flag;
    searched.last_search_tick = 10;
    CHECK(!search_ready(searched, 69));
    CHECK(searched.last_search_tick == 10);
    CHECK(search_ready(searched, 70));
    CHECK(searched.last_search_tick == 70);
    searched.flags = 0;
    CHECK(!search_ready(searched, 200));
    MapFixture map_host;
    MovementMap map(8, 17, 1, 2, map_host);
    SearchController controller;
    Goal search_goal;
    CHECK(controller.begin({&state, &searched, &search_goal, &map}));
    CHECK(!controller.idle());
    // A second job, or one with a missing field, is refused.
    CHECK(!controller.begin({&state, &searched, &search_goal, &map}));
    CHECK(!SearchController{}.begin({&state, &searched, nullptr, &map}));
    Navigation other;
    controller.cancel(other);
    CHECK(!controller.idle());
    controller.cancel(searched);
    CHECK(controller.idle());
    // release_navigation drops only the job whose navigator is the one going away.
    CHECK(controller.begin({&state, &searched, &search_goal, &map}));
    release_navigation(controller, other);
    CHECK(!controller.idle());
    release_navigation(controller, searched);
    CHECK(
        controller.idle() && controller.active().navigation == nullptr &&
        controller.active().goal == nullptr && controller.active().movement_map == nullptr
    );
    CHECK(controller.begin({&state, &searched, &search_goal, &map}));
    controller.complete({}, 0);
    CHECK(
        controller.idle() && controller.active().navigation == &searched &&
        controller.active().goal == &search_goal && controller.active().movement_map == nullptr
    );
    map.refresh({uint32_t(2) | (uint32_t(3) << 16), uint32_t(1) | (uint32_t(2) << 16)});
    CHECK(
        map_host.samples == 15 && map.cell(1, 1) == 2 && map.cell(3, 5) == 0 && map.cell(0, 0) == 0
    );
    std::array changes{OccupancyChange{{uint32_t(6) | (uint32_t(15) << 16), 0}, 0, true}};
    uint32_t searcher_tick = 0;
    map.prepare_search({0, 0}, searcher_tick, changes, 100);
    CHECK(map.projection_tick() == 70 && map.cell(6, 15) == 1 && searcher_tick == 0);
    const auto before = map_host.samples;
    map.release_unit(changes[0].rectangle, 70);
    CHECK(map_host.samples == before);
    map.release_unit(changes[0].rectangle, 69);
    CHECK(map_host.samples > before);
    std::array<SearchMapCell, 16> cells{};
    cells[3].predecessor = 6;
    cells[2].predecessor = 6;
    cells[1].predecessor = 6;
    const auto trace = reconstruct_search_path(4, 4, {0, 0}, {3, 0}, 1, 1, cells);
    CHECK(
        trace.size() == 2 && trace[0][0] == 8 && trace[0][1] == 8 && trace[1][0] == 56 &&
        trace[1][1] == 8
    );

    // A mirrored navigator holds the shared route head and drives the same
    // ground steering.
    MirroredNavigation mirrored_route;
    oa::sim::unit_movement::Movement mirrored_motion;
    CHECK(!mirrored_route_present(mirrored_route));
    mirrored_route.count = 2;
    mirrored_route.points[0] = {8, 8};
    mirrored_route.points[1] = {88, 8};
    mirrored_motion.flags = oa::sim::unit_movement::collision_blocked;
    CHECK(
        mirrored_route_present(mirrored_route) &&
        (mirrored_motion.flags & oa::sim::unit_movement::collision_blocked) != 0
    );
    const auto mirrored_points = mirrored_steering_points(mirrored_route);
    CHECK(
        mirrored_points[1][0] == 88 * 65536 && mirrored_points[2][0] == 88 * 65536 &&
        mirrored_points[2][1] == 0
    );
    oa::sim::unit_movement::Unit mirrored_unit;
    mirrored_unit.type.maximum_turn = 1024;
    mirrored_unit.type.maximum_speed = 65536;
    mirrored_unit.position = {8 * 65536, 0, 8 * 65536};
    steer_ground(
        mirrored_unit, mirrored_motion, mirrored_route, 4096, 4096, 0, FastUnitSteering::as_3_1c
    );
    CHECK(mirrored_motion.turn == -1024);
    oa::sim::simulation_state::Order replaced;
    Goal first_goal;
    first_goal.order = &replaced;
    set_mirrored_goal(mirrored_route, &first_goal);
    set_mirrored_goal(mirrored_route, nullptr);
    CHECK((replaced.raised_events & goal_replaced_event) != 0 && mirrored_route.goal == nullptr);

    // Saved movement block round trip.
    runtime.movement.velocity = {1, -2, 3};
    runtime.previous_vector = {-4, 5, -6};
    runtime.movement.speed = 0x12345;
    runtime.movement.turn = -7;
    runtime.occupancy_changed_tick = 0xdeadbeefu;
    runtime.movement.flags = 0x1e;
    const auto saved = runtime.save_mobility();
    CHECK(saved[34] == 0x06 && saved[4] == 0xfe && saved[30] == 0xef);
    GroundRuntime restored(slot, definition);
    restored.movement.flags = 0xf1;
    restored.load_mobility(saved);
    CHECK(
        restored.movement.velocity == runtime.movement.velocity &&
        restored.previous_vector == runtime.previous_vector && restored.movement.speed == 0x12345 &&
        restored.movement.turn == -7 && restored.occupancy_changed_tick == 0xdeadbeefu &&
        restored.movement.flags == 0xf6
    );

    // A unit another player's machine simulates gets the mirrored navigator.
    auto& owner = world.simulation().players[0];
    owner.present = true;
    owner.status = mirrored_player_status;
    state.owner = &owner;
    type.flags = 0;
    GroundRuntime mirrored(slot, definition);
    CHECK(mirrored.mirrored_driver && mirrored.mirrored_navigation.count == 0);

    // In a match that applied mutators, a unit faster than any stock unit is
    // held to its route: it does not step past the route's end, comes on no
    // faster than its braking can stop it, and slows while it turns. A unit
    // no faster steers as before, and in any other match every unit steers
    // as 3.1c steers it.
    {
        constexpr Fixed pixel = 65536;
        constexpr Fixed racing_acceleration = 10404577; // 158.76 pixels a tick, a tick
        constexpr Fixed racing_braking = 21965455;      // 335.16
        constexpr uint16_t east = 0xc000;
        constexpr auto held = FastUnitSteering::held_to_route;
        constexpr auto as_3_1c = FastUnitSteering::as_3_1c;
        Navigation road;
        road.count = 2;
        road.flags = route_present_flag;
        road.points[0] = {6274, 5704};
        road.points[1] = {6296, 5704};
        oa::sim::unit_movement::Unit racer;
        racer.type.maximum_turn = 28800;
        racer.type.maximum_speed = 42 * pixel;
        racer.position = {6274 * pixel, 0, 5704 * pixel};
        racer.heading = east;
        oa::sim::unit_movement::Unit unheld = racer;
        oa::sim::unit_movement::Movement racing;
        steer_ground(racer, racing, road, racing_acceleration, racing_braking, 0, held);
        // 22 pixels short of the end, at 42 a tick, it steps onto the end.
        CHECK(racing.speed == 22 * pixel);
        CHECK(std::abs(racer.position[0] + racing.velocity[0] - 6296 * pixel) < pixel);
        CHECK(std::abs(racer.position[2] + racing.velocity[2] - 5704 * pixel) < pixel);
        // Steered as 3.1c steers it, it steps the whole 42, 20 past the end.
        oa::sim::unit_movement::Movement unheld_racing;
        steer_ground(unheld, unheld_racing, road, racing_acceleration, racing_braking, 0, as_3_1c);
        CHECK(unheld_racing.speed == 42 * pixel);
        CHECK(unheld.position[0] + unheld_racing.velocity[0] - 6296 * pixel > 19 * pixel);

        // At the fastest stock step, 3 pixels short, it still steps 6.
        oa::sim::unit_movement::Unit stock = racer;
        stock.type.maximum_speed = 6 * pixel;
        stock.position = {6293 * pixel, 0, 5704 * pixel};
        oa::sim::unit_movement::Movement stock_motion;
        steer_ground(stock, stock_motion, road, racing_acceleration, racing_braking, 0, held);
        CHECK(stock_motion.speed == 6 * pixel);

        // Braking 0.19 a tick, 300 pixels from the end, it starts at no more
        // than the speed that braking stops in 300 pixels, about 10.7; as
        // 3.1c steers it, at the whole 42.
        road.points[0] = {5996, 5704};
        oa::sim::unit_movement::Unit coaster = racer;
        coaster.position = {5996 * pixel, 0, 5704 * pixel};
        oa::sim::unit_movement::Unit unheld_coaster = coaster;
        oa::sim::unit_movement::Movement coasting;
        steer_ground(coaster, coasting, road, racing_acceleration, 12452, 0, held);
        CHECK(coasting.speed > 10 * pixel && coasting.speed < 11 * pixel);
        oa::sim::unit_movement::Movement unheld_coasting;
        steer_ground(unheld_coaster, unheld_coasting, road, racing_acceleration, 12452, 0, as_3_1c);
        CHECK(unheld_coasting.speed == 42 * pixel);

        // Turning 960 a tick, facing north with the end 40 pixels east, it
        // creeps while it turns, a little over a pixel a tick; as 3.1c steers
        // it, it sets off at the whole 42.
        road.points[0] = {6256, 5704};
        oa::sim::unit_movement::Unit turner = racer;
        turner.type.maximum_turn = 960;
        turner.position = {6256 * pixel, 0, 5704 * pixel};
        turner.heading = 0;
        oa::sim::unit_movement::Unit unheld_turner = turner;
        oa::sim::unit_movement::Movement turning;
        steer_ground(turner, turning, road, racing_acceleration, racing_braking, 0, held);
        CHECK(turning.turn == -960);
        CHECK(turning.speed > pixel && turning.speed < 2 * pixel);
        oa::sim::unit_movement::Movement unheld_turning;
        steer_ground(
            unheld_turner, unheld_turning, road, racing_acceleration, racing_braking, 0, as_3_1c
        );
        CHECK(unheld_turning.turn == -960 && unheld_turning.speed == 42 * pixel);
    }

    // An empty route raises the failure only when a goal is set and the unit
    // is outside it. A route, a unit already inside its goal, and a navigator
    // with no goal raise nothing.
    {
        oa::sim::simulation_state::Order failed_order;
        Goal goal;
        goal.order = &failed_order;
        goal.shape = GoalShape::circle;
        goal.cell = {10, 10};
        Navigation failed_nav;
        failed_nav.goal = &goal;
        failed_nav.flags = search_pending_flag;
        oa::sim::unit_movement::Unit geometry;
        geometry.cell = {0, 0};
        oa::sim::unit_movement::Movement failed_motion;
        std::array<uint8_t, 3> failed_weapons{};
        UnitView failed_view{state, geometry, failed_motion, failed_nav, false, failed_weapons};
        CHECK(accept_path(failed_view, {}) == true);
        CHECK((failed_order.raised_events & path_failed_event) != 0);
        CHECK((failed_nav.flags & route_present_flag) == 0);
        CHECK((failed_nav.flags & search_pending_flag) == 0);

        failed_order.raised_events = 0;
        geometry.cell = {10, 10};
        failed_nav.flags = search_pending_flag;
        CHECK(accept_path(failed_view, {}) == false);
        CHECK(failed_order.raised_events == 0);

        failed_nav.flags = search_pending_flag;
        const RoutePoint route[] = {{1, 2}, {3, 4}};
        CHECK(accept_path(failed_view, route) == false);
        CHECK(failed_order.raised_events == 0);
        CHECK(failed_nav.count == 2 && (failed_nav.flags & route_present_flag) != 0);

        failed_nav.goal = nullptr;
        failed_nav.flags = search_pending_flag;
        geometry.cell = {0, 0};
        CHECK(accept_path(failed_view, {}) == false);
        CHECK(failed_order.raised_events == 0);
    }
}
