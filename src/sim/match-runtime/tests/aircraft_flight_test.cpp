// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Aircraft flight orders over the air driver: the take-off climb,
// VTOL_LandIfCan, the idle VTOL_Standby and the off-map bucket test the
// flight step and goals share.
#include "oa/data/enhanced_mode.hpp"
#include "match_tick_access.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include "oa/test/match_services.hpp"

using namespace oa;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #x);   \
    } while (false)

namespace {
/// The rules the cases play: Off, capacity alone, and Revision 1, whose
/// searches use the exact indexes.
const std::array<oa::data::enhanced_mode::Choice, 3> played_choices{{
    {},
    {1, oa::data::enhanced_mode::FeatureSet{}.with(oa::data::enhanced_mode::Feature::capacity)},
    {1, oa::data::enhanced_mode::revision_features(1)},
}};
constexpr int32_t map_cells = 32;
// ARMCA.FBI: Canfly=1, cruisealt=70, FootprintX/Z=2, DefaultMissionType=VTOL_standby.
constexpr uint16_t copter_type = 1;
constexpr int16_t copter_cruise = 70;
// ARMATLAS.FBI: Canfly=1, cruisealt=90, FootprintX/Z=3, DefaultMissionType=VTOL_standby.
constexpr uint16_t transport_type = 2;
constexpr int16_t transport_cruise = 90;
constexpr uint16_t cargo_type = 3;
constexpr size_t type_count = 4;

constexpr uint32_t timer_event = 0x01;
constexpr uint32_t arrived_event = 0x20;
constexpr uint32_t goal_events = 0xe0;
constexpr uint32_t weapon_event = 0x10000;

using Services = oa::test::QuietServices;

using Scenario = oa::test::EmptyScenario;

sim::ground_orders::Point point(int32_t x, int32_t y, int32_t z) {
    return {x << 16, y << 16, z << 16};
}

struct Fixture {
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain_values;
    std::array<sim::visibility_state::SightMask, 1> masks{};
    std::shared_ptr<formats::objects3d::Model> model;
    std::shared_ptr<formats::cob::CobProgram> script;
    std::array<sim::unit_spawn::LoadedType, type_count> loaded{};
    std::array<sim::unit_spawn::Type, type_count> types{};
    std::array<data::unit_definitions::UnitDefinition, type_count> defs{};
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    std::array<data::unit_definitions::RuntimeDefinitionMetadata, type_count> metadata{};
    std::vector<sim::spatial_state::Plot> collision_plots;
    std::array<sim::match_runtime::RuntimeTypeFields, type_count> fields{};
    std::array<uint8_t, 9> yard{4, 4, 4, 4, 4, 4, 4, 4, 4};
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    std::unique_ptr<sim::match_runtime::Match> match;

    explicit Fixture(const oa::data::enhanced_mode::Choice& choice = {}) {
        map.attribute_width = map.attribute_height = map_cells;
        map.attributes.resize(map_cells * map_cells);
        terrain_values.resize(map_cells * map_cells);
        collision_plots.resize(map_cells * map_cells);
        // Sight reaches eight 32-unit cells around a unit: the whole map from its middle.
        masks[0].width = masks[0].height = 17;
        masks[0].offset_x = masks[0].offset_z = 8;
        masks[0].pixels.assign(17 * 17, 1);
        model = std::make_shared<formats::objects3d::Model>();
        model->objects.resize(1);
        model->objects[0].name = "root";
        script = std::make_shared<formats::cob::CobProgram>();
        script->code = {sim::script_vm::opcode::return_};
        script->scripts = {{"Create", 0}};
        script->entry_points = {0};
        script->piece_names = {"root"};
        const std::array<int16_t, type_count> footprints{0, 2, 3, 2};
        for (size_t i = 1; i < type_count; ++i) {
            auto& type = types[i];
            type.simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
            type.simulation.maximum_health = 100;
            type.footprint_x = type.footprint_z = footprints[i];
            type.bm_code = 1;
            type.model = reinterpret_cast<uintptr_t>(model.get());
            type.cob = reinterpret_cast<uintptr_t>(script.get());
            loaded[i].model = model;
            loaded[i].script = script;
            auto& def = defs[i];
            def.sight_distance = 400;
            def.acceleration_fixed = 0x10000;
            def.brake_rate_fixed = 0x10000;
            def.max_velocity_fixed = 4 * 0x10000;
            def.turn_rate = 1024;
            metadata[i].footprint_x = metadata[i].footprint_z = static_cast<uint8_t>(footprints[i]);
            fields[i].definition = &def;
            fields[i].yard_mask =
                std::span(yard).first(static_cast<size_t>(footprints[i] * footprints[i]));
            fields[i].runtime_metadata = &metadata[i];
            fields[i].target_masks = &target_masks;
            fields[i].movement_class = 0;
        }
        types[copter_type].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_FLY;
        defs[copter_type].can_fly = true;
        defs[copter_type].cruise_altitude = copter_cruise;
        types[transport_type].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_FLY;
        defs[transport_type].can_fly = true;
        defs[transport_type].cruise_altitude = transport_cruise;
        defs[transport_type].transport_size = 3;
        for (size_t i = 1; i < type_count; ++i)
            loaded[i].type = types[i];
        sim::match_runtime::OfflineInputs input{
            map, loaded, types, fields,    weapons, terrain_values,  masks, 16, 16, 16,   2,
            0,   30,     1,     &scenario, {},      collision_plots, {},    0,  0,  0.0F, {}
        };
        oa::data::enhanced_mode::store_choice(input.rules.setup.enhanced_mode, choice);
        input.unit_player_mask = 1;
        match = std::make_unique<sim::match_runtime::Match>(input, services);
        CHECK(match->fault() == nullptr);
        for (auto type : {copter_type, transport_type})
            match->state().unit_defs[type].default_mission_type =
                sim::ground_orders::vtol_standby_kind;
        match->configure_strategic_environment({0, 0.5f, 0});
        match->simulation().players[0].present = true;
        match->simulation().players[0].status = 1;
        std::array<uint8_t, 10> allies{};
        allies[0] = 1;
        match->configure_player_alliances(0, allies);
        match->configure_outcomes(0, allies, false);
    }

    sim::unit_spawn::Slot& spawn(uint16_t type, int32_t x, int32_t z) {
        auto* slot = match->create(
            {0,
             type,
             {static_cast<uint32_t>(x) << 16, 0, static_cast<uint32_t>(z) << 16},
             true,
             1,
             0}
        );
        CHECK(slot && slot->unit->object_present);
        return *slot;
    }

    // An aircraft already flying at `height` over its spot.
    sim::unit_spawn::Slot& spawn_airborne(uint16_t type, int32_t x, int32_t z, int32_t height) {
        auto& slot = spawn(type, x, z);
        match->set_movement_layer(slot.unit_index, sim::air::layer_air);
        slot.record.position.y = height << 16;
        // With no goal the driver holds its last point; keep that point here.
        match->air_driver(slot.unit_index)->position = slot.record.position;
        run(1);
        CHECK((slot.record.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == sim::air::layer_air);
        return slot;
    }

    void run(uint32_t ticks) {
        for (uint32_t i = 0; i < ticks; ++i) {
            ++match->state().game.tick;
            match->tick();
        }
    }

    uint8_t layer(const sim::unit_spawn::Slot& slot) {
        return match->ground_runtime(slot.unit_index)->movement.flags & 3;
    }

    sim::simulation_state::Order* head(const sim::unit_spawn::Slot& slot) {
        return slot.unit->primary;
    }

    const sim::air::AirGoal* goal(const sim::unit_spawn::Slot& slot) {
        const auto* driver = match->air_driver(slot.unit_index);
        return driver != nullptr ? driver->goal : nullptr;
    }

    // Runs the order once and applies the queue's phase rule for results 0 and 1.
    uint32_t
    step(sim::unit_spawn::Slot& slot, sim::simulation_state::Order& order, uint32_t events) {
        sim::match_runtime::MatchTickAccess host(*match);
        order.wait_events = 0;
        const auto result = host.dispatch_mission(match->state(), slot.record, order, events);
        if (result == 0)
            order.phase = 0;
        else if (result == 1)
            ++order.phase;
        return result;
    }
};

// A landed aircraft rises straight up to half its cruise altitude before its
// order turns it toward the destination.
void take_off_climbs_first() {
    Fixture f;
    auto& copter = f.spawn(copter_type, 256, 256);
    CHECK(f.layer(copter) == sim::air::layer_ground);
    auto& move = f.match->issue_ground_move(copter.unit_index, point(400, 0, 256), false);
    CHECK(move.kind == sim::ground_orders::vtol_move_kind);
    f.run(1);
    CHECK(f.layer(copter) == sim::air::layer_air);
    const auto* climb = f.goal(copter);
    CHECK(climb && (climb->flags & sim::air::goal_fixed_altitude) != 0);
    CHECK(climb->altitude == copter_cruise / 2);
    CHECK(climb->point.x == 256 << 16 && climb->point.z == 256 << 16);
    CHECK(climb->point.y == (copter_cruise / 2) << 16);
    CHECK(move.phase == 1 && (move.wait_events & goal_events) == goal_events);
    // Level with its start the goal is not reached: the order holds its phase
    // until the climb is within one unit of the goal height.
    f.run(1);
    CHECK(move.phase == 1 && f.goal(copter) == climb);
    uint32_t ticks = 0;
    while (f.goal(copter) == climb && ticks++ < 600) {
        CHECK(move.phase == 1);
        f.run(1);
    }
    CHECK(f.goal(copter) != climb);
    CHECK(std::abs(copter.record.position.y - ((copter_cruise / 2) << 16)) < 0x10001);
    f.run(1);
    CHECK(move.phase == 2);
}

// Airborne already, the take-off leaves the goal and the wait alone.
void take_off_in_the_air_sets_no_goal() {
    Fixture f;
    auto& copter = f.spawn_airborne(copter_type, 256, 256, 60);
    f.match->stop_orders(copter.unit_index);
    auto& move = f.match->insert_ground_order(
        copter.unit_index, sim::ground_orders::vtol_move_kind, point(400, 0, 256)
    );
    CHECK(f.step(copter, move, 0) == 1);
    CHECK(f.goal(copter) == nullptr && move.wait_events == 0);
}

void land_if_can_gives_way_to_a_queued_order() {
    Fixture f;
    auto& copter = f.spawn(copter_type, 256, 256);
    (void)f.match->insert_ground_order(
        copter.unit_index, sim::ground_orders::vtol_move_kind, point(400, 0, 256)
    );
    auto& land =
        f.match->insert_ground_order(copter.unit_index, sim::ground_orders::vtol_land_if_can_kind);
    CHECK(land.next != nullptr);
    CHECK(f.step(copter, land, 0) == 5);
    CHECK(f.layer(copter) == sim::air::layer_ground);
}

// Clear ground below: EndTransport, a descent goal at a fixed altitude of
// 0 above the land, a wait on it, deactivation, and the ground layer on arrival.
void land_if_can_sets_down_where_it_hovers() {
    Fixture f;
    auto& copter = f.spawn_airborne(copter_type, 256, 256, 60);
    f.match->stop_orders(copter.unit_index);
    auto& land =
        f.match->insert_ground_order(copter.unit_index, sim::ground_orders::vtol_land_if_can_kind);
    CHECK(f.step(copter, land, 0) == 1);
    CHECK(f.goal(copter) == nullptr);
    copter.record.state_flags |= 1;
    CHECK(f.step(copter, land, 0) == 1);
    const auto* descent = f.goal(copter);
    CHECK(descent && (descent->flags & sim::air::goal_fixed_altitude) != 0);
    CHECK((descent->flags & sim::air::goal_arrival_radius) == 0);
    CHECK(descent->altitude == 0 && descent->point.y == 0);
    CHECK(descent->point.x == 256 << 16 && descent->point.z == 256 << 16);
    CHECK(land.wait_events == goal_events);
    CHECK((copter.record.state_flags & 1) == 0);
    CHECK(f.layer(copter) == sim::air::layer_air);
    CHECK(f.step(copter, land, 0) == 8);
    CHECK(f.layer(copter) == sim::air::layer_air);
    land.phase = 2;
    CHECK(f.step(copter, land, arrived_event) == 5);
    CHECK(f.layer(copter) == sim::air::layer_ground);
}

// Nowhere to land: the aircraft flies 160 units out from the destination
// along the stored heading, arriving within 64, and each goal event turns
// the heading a third of a turn back.
void land_if_can_circles_when_blocked() {
    Fixture f;
    auto& copter = f.spawn_airborne(copter_type, 256, 256, 60);
    f.match->stop_orders(copter.unit_index);
    for (auto& plot : f.match->spatial().plots)
        plot.flags |= sim::spatial_state::plot_claimed;
    // Heading 0 points the circle's leg along -Z.
    auto& land = f.match->insert_ground_order(
        copter.unit_index, sim::ground_orders::vtol_land_if_can_kind, point(256, 7, 256), 0
    );
    land.phase = 1;
    CHECK(f.step(copter, land, 0) == 2 && land.phase == 1);
    const auto* leg = f.goal(copter);
    CHECK(leg && (leg->flags & sim::air::goal_arrival_radius) != 0 && leg->arrival_radius == 0x40);
    CHECK((leg->flags & sim::air::goal_fixed_altitude) == 0);
    CHECK(
        leg->point.x == 256 << 16 && leg->point.z == (256 - 160) << 16 && leg->point.y == 7 << 16
    );
    CHECK((land.wait_events & goal_events) == goal_events);
    CHECK((copter.record.state_flags & 1) != 0);
    f.match->stop_orders(copter.unit_index);
    auto& turned = f.match->insert_ground_order(
        copter.unit_index, sim::ground_orders::vtol_land_if_can_kind, point(256, 7, 256), 0x5555
    );
    turned.phase = 1;
    CHECK(f.step(copter, turned, arrived_event) == 2);
    CHECK(f.goal(copter)->point.x == 256 << 16 && f.goal(copter)->point.z == (256 - 160) << 16);
    // Phase 0 keeps a destination whose X and Z are zero but whose height is not.
    f.match->stop_orders(copter.unit_index);
    auto& corner = f.match->insert_ground_order(
        copter.unit_index, sim::ground_orders::vtol_land_if_can_kind, point(0, 7, 0)
    );
    CHECK(f.step(copter, corner, 0) == 1);
    CHECK(f.step(copter, corner, 0) == 2);
    CHECK(f.goal(copter)->point.y == 7 << 16);
}

// An idle aircraft in the air runs its DefaultMissionType, VTOL_Standby, and
// only that queues the landing.
void idle_aircraft_stands_by() {
    Fixture f;
    auto& copter = f.spawn_airborne(copter_type, 256, 256, 60);
    auto* standby = f.head(copter);
    CHECK(standby && standby->kind == sim::ground_orders::vtol_standby_kind && standby->phase == 0);
    // Phase 0 waits a tick and on the weapon event.
    f.run(1);
    CHECK(f.head(copter) == standby && standby->phase == 1);
    CHECK((standby->wait_events & (weapon_event | timer_event)) == (weapon_event | timer_event));
    // With nothing to attack and no cargo, phase 2 queues the landing.
    f.run(1);
    CHECK(f.head(copter) && f.head(copter)->kind == sim::ground_orders::vtol_land_if_can_kind);
}

// A landed aircraft's standby re-arms the weapon wake with its long wait.
void grounded_standby_waits_on_weapons() {
    Fixture f;
    auto& copter = f.spawn(copter_type, 256, 256);
    f.run(1);
    auto* standby = f.head(copter);
    CHECK(standby && standby->kind == sim::ground_orders::vtol_standby_kind);
    standby->phase = 2;
    const auto now = f.match->state().game.tick;
    CHECK(f.step(copter, *standby, 0) == 2 && standby->phase == 1);
    CHECK((standby->wait_events & weapon_event) != 0);
    CHECK(standby->wake_tick >= now + 0x1e && standby->wake_tick < now + 0x3c);
}

// A transport holding cargo circles 8 to 39 units round its anchor at its
// full cruise altitude instead of landing.
void loaded_transport_loiters() {
    Fixture f;
    auto& cargo = f.spawn(cargo_type, 300, 300);
    auto& transport = f.spawn_airborne(transport_type, 256, 256, 90);
    // Phase 0 anchors the standby where the transport is.
    f.run(1);
    auto* standby = f.head(transport);
    CHECK(standby && standby->kind == sim::ground_orders::vtol_standby_kind && standby->phase == 1);
    f.match->set_carry_link(cargo.unit_index, transport.unit_index, -1, 0);
    standby->phase = 2;
    const auto now = f.match->state().game.tick;
    CHECK(f.step(transport, *standby, 0) == 2 && standby->phase == 1);
    CHECK(f.head(transport) == standby);
    const auto* loiter = f.goal(transport);
    CHECK(loiter && (loiter->flags & sim::air::goal_fixed_altitude) != 0);
    CHECK(loiter->altitude == transport_cruise && loiter->point.y == transport_cruise << 16);
    const double dx = (loiter->point.x - (256 << 16)) / 65536.0;
    const double dz = (loiter->point.z - (256 << 16)) / 65536.0;
    const double radius = std::sqrt(dx * dx + dz * dz);
    CHECK(radius > 7.5 && radius < 40.5);
    CHECK(standby->wake_tick >= now + 0x1e && standby->wake_tick < now + 0x2d);
    CHECK((standby->wait_events & weapon_event) == 0);
}

// The off-map test the flight step and goals share is the footprint's bucket
// (as registration sets it): a 2x2 footprint hanging over the first column is off the map
// though the unit's origin is over it, while a unit past the scroll extent
// (the map less 0x80 pixels down) is still on it.
void off_map_is_the_footprint_bucket() {
    Fixture f;
    auto& west = f.spawn(copter_type, 6, 256);
    auto& south = f.spawn(copter_type, 256, 400);
    auto& middle = f.spawn(copter_type, 256, 256);
    CHECK(west.record.position.x > 0 && west.record.cell_x < 0);
    CHECK(
        (south.record.position.z >> 16) >=
        static_cast<int32_t>(f.match->state().game.map_pixel_height)
    );
    sim::match_runtime::MatchTickAccess host(*f.match);
    const auto air = host.air_host();
    CHECK(air.outside_map(air.context, &west.record));
    CHECK(!air.outside_map(air.context, &south.record));
    CHECK(!air.outside_map(air.context, &middle.record));
}

// Arrived copies and mirrored goals must lose a dead target even after the
// current goal has changed. Accessing a flying driver may populate either.
void dead_targets_leave_arrived_and_mirrored_air_goals() {
    for (const auto& choice : played_choices) {
        Fixture f(choice);
        auto& target = f.spawn(cargo_type, 160, 128);
        auto& aircraft = f.spawn(copter_type, 256, 128);
        auto* driver = f.match->air_driver(aircraft.unit_index);
        CHECK(driver != nullptr);
        driver->arrived =
            sim::air::air_goal_over_unit(nullptr, &aircraft.record, &target.record, -1);
        driver->local = false;
        const auto mirrored =
            sim::air::air_goal_over_unit(nullptr, &aircraft.record, &target.record, -1);
        f.match->set_mirrored_air_goal(aircraft.unit_index, &mirrored);
        CHECK(driver->arrived.target == &target.record);
        CHECK(driver->goal != nullptr && driver->goal->target == &target.record);

        const sim::match_runtime::KillOutcome outcome{sim::match_runtime::DeathKind::dismissed};
        f.match->apply_kill(target.unit_index, outcome, 0, OA_PLAYER_COUNT);
        CHECK(target.record.type_index == 0);
        CHECK(driver->arrived.target == nullptr);
        CHECK(driver->goal != nullptr && driver->goal->target == nullptr);
        CHECK(f.match->fault() == nullptr);
    }
}
} // namespace

int main() {
    const std::pair<const char*, void (*)()> cases[] = {
        {"take_off_climbs_first", take_off_climbs_first},
        {"take_off_in_the_air_sets_no_goal", take_off_in_the_air_sets_no_goal},
        {"land_if_can_gives_way_to_a_queued_order", land_if_can_gives_way_to_a_queued_order},
        {"land_if_can_sets_down_where_it_hovers", land_if_can_sets_down_where_it_hovers},
        {"land_if_can_circles_when_blocked", land_if_can_circles_when_blocked},
        {"idle_aircraft_stands_by", idle_aircraft_stands_by},
        {"grounded_standby_waits_on_weapons", grounded_standby_waits_on_weapons},
        {"loaded_transport_loiters", loaded_transport_loiters},
        {"off_map_is_the_footprint_bucket", off_map_is_the_footprint_bucket},
        {"dead_targets_leave_arrived_and_mirrored_air_goals",
         dead_targets_leave_arrived_and_mirrored_air_goals},
    };
    int failures = 0;
    for (const auto& [name, run] : cases) {
        try {
            run();
        } catch (const std::exception& error) {
            std::cerr << "aircraft flight " << name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    if (failures != 0)
        return 1;
    std::cout << "aircraft flight ok\n";
    return 0;
}
