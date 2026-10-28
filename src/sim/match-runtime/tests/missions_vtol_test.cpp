// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "match_tick_access.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/match_runtime/construction_orders.hpp"
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "oa/test/match_services.hpp"
using namespace oa;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (false)

// Aircraft movement missions: the issue paths pick the VTOL kinds for CAN_FLY
// units, and the first Match::tick runs each handler's take-off phase.
namespace {
constexpr uint8_t vtol_follow = 49, vtol_move = 55, vtol_patrol = 56, vtol_pickup = 57,
                  vtol_unload = 65;
constexpr uint16_t aircraft_type = 1, ground_type = 2, pad_type = 3;

struct Services : oa::test::QuietServices {
    std::vector<std::pair<uint16_t, uint32_t>> speech;

    void command_sound(sim::unit_spawn::Slot& slot, uint32_t category) override {
        speech.emplace_back(slot.unit_index, category);
    }

    std::vector<uint32_t> spoken_by(uint16_t unit) const {
        std::vector<uint32_t> out;
        for (const auto& [who, category] : speech)
            if (who == unit)
                out.push_back(category);
        return out;
    }
};

using Scenario = oa::test::EmptyScenario;

std::array<uint32_t, 3> at(uint32_t x, uint32_t z) {
    return {x << 16, 0, z << 16};
}

sim::ground_orders::Point point(int32_t x, int32_t z) {
    return {x << 16, 0, z << 16};
}
} // namespace

int main() {
    formats::tnt::Map map;
    map.attribute_width = map.attribute_height = 32;
    map.attributes.resize(32 * 32);
    std::vector<sim::visibility_state::TerrainCell> terrain_values(32 * 32);
    sim::visibility_state::SightMask mask;
    mask.width = mask.height = 1;
    mask.transparent = 0;
    mask.pixels = {1};
    const std::array masks{mask};
    auto model = std::make_shared<formats::objects3d::Model>();
    model->objects.resize(1);
    model->objects[0].name = "root";
    auto script = std::make_shared<formats::cob::CobProgram>();
    using namespace sim::script_vm;
    script->code = {opcode::return_};
    script->scripts = {{"Create", 0}};
    script->entry_points = {0};
    script->piece_names = {"root"};
    constexpr std::size_t type_count = 4;
    std::array<sim::unit_spawn::LoadedType, type_count> loaded;
    std::array<sim::unit_spawn::Type, type_count> types;
    std::array<data::unit_definitions::UnitDefinition, type_count> defs;
    std::array<sim::match_runtime::RuntimeTypeFields, type_count> fields{};
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    data::unit_definitions::RuntimeDefinitionMetadata metadata;
    const std::array<uint8_t, 1> yard{4};
    for (std::size_t i = 1; i < type_count; ++i) {
        types[i].simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
        types[i].simulation.maximum_health = 100;
        types[i].footprint_x = types[i].footprint_z = 1;
        types[i].bm_code = 1;
        types[i].model = reinterpret_cast<uintptr_t>(model.get());
        types[i].cob = reinterpret_cast<uintptr_t>(script.get());
        loaded[i].model = model;
        loaded[i].script = script;
        defs[i].sight_distance = 160;
        defs[i].acceleration_fixed = 65536;
        defs[i].brake_rate_fixed = 65536;
        defs[i].max_velocity_fixed = 2 * 65536;
        defs[i].turn_rate = 1024;
        defs[i].energy_storage = defs[i].metal_storage = 1000.0F;
        fields[i].definition = &defs[i];
        fields[i].yard_mask = yard;
        fields[i].runtime_metadata = &metadata;
        fields[i].target_masks = &target_masks;
        fields[i].movement_class = 0;
    }
    types[aircraft_type].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_FLY;
    defs[aircraft_type].can_fly = true;
    defs[aircraft_type].cruise_altitude = 60;
    defs[aircraft_type].transport_size = 2;
    // The ground type's model stands 10 units high.
    auto ground_model = std::make_shared<formats::objects3d::Model>(*model);
    ground_model->objects[0].vertices = {{0, 0, 0}, {0, 10 << 16, 0}};
    loaded[ground_type].model = ground_model;
    types[ground_type].model = reinterpret_cast<uintptr_t>(ground_model.get());
    types[pad_type].simulation.flags |= OA_UNIT_DEF_FLAG_BUILDER | OA_UNIT_DEF_FLAG_IS_AIRBASE;
    types[pad_type].bm_code = 0;
    fields[pad_type].movement_class.reset();
    for (std::size_t i = 1; i < type_count; ++i)
        loaded[i].type = types[i];
    std::vector<sim::spatial_state::Plot> collision_plots(32 * 32);
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    sim::match_runtime::OfflineInputs input{
        map, loaded, types, fields,    weapons, terrain_values,  masks, 16, 16, 16,   2,
        0,   30,     1,     &scenario, {},      collision_plots, {},    0,  0,  0.0F, {}
    };
    sim::match_runtime::Match match(input, services);
    match.configure_strategic_environment({0, 0.5F, 0});
    match.simulation().players[0].present = true;
    match.simulation().players[0].status = 1;
    std::array<uint8_t, 10> allies{};
    allies[0] = 1;
    match.configure_player_alliances(0, allies);
    match.configure_outcomes(0, allies, false);

    auto* mover = match.create({0, aircraft_type, at(40, 40), true, 1, 0});
    auto* patroller = match.create({0, aircraft_type, at(120, 40), true, 1, 0});
    auto* damaged = match.create({0, aircraft_type, at(200, 200), true, 1, 0});
    auto* pad = match.create({0, pad_type, at(232, 200), true, 1, 0});
    auto* guard = match.create({0, aircraft_type, at(40, 200), true, 1, 0});
    auto* guarded = match.create({0, ground_type, at(72, 200), true, 1, 0});
    auto* transport = match.create({0, aircraft_type, at(200, 120), true, 1, 0});
    auto* cargo = match.create({0, ground_type, at(232, 120), true, 1, 0});
    CHECK(mover && patroller && damaged && pad && guard && guarded && transport && cargo);
    CHECK(mover->movement_object && transport->movement_object);
    pad->record.state_flags |= 1;
    damaged->unit->health = 10;

    uint32_t tick = 1;
    const auto run = [&](uint32_t ticks) {
        for (uint32_t i = 0; i < ticks; ++i) {
            match.simulation().tick = tick++;
            match.tick();
        }
    };
    const auto head = [&](sim::unit_spawn::Slot* slot) { return slot->unit->primary; };

    auto& move = match.issue_ground_move(mover->unit_index, point(40, 40), false);
    CHECK(move.kind == vtol_move);
    auto& patrol = match.issue_patrol(patroller->unit_index, point(120, 200), false);
    CHECK(patrol.kind == vtol_patrol);
    auto& hurt_patrol = match.issue_patrol(damaged->unit_index, point(40, 120), false);
    CHECK(hurt_patrol.kind == vtol_patrol);
    auto& follow = match.issue_guard(guard->unit_index, guarded->unit_index, false);
    CHECK(follow.kind == vtol_follow);
    auto& pickup = match.issue_load(transport->unit_index, cargo->unit_index, false);
    CHECK(pickup.kind == vtol_pickup);
    auto& unload = match.issue_unload(mover->unit_index, point(200, 40), true);
    CHECK(unload.kind == vtol_unload);

    // Phase 0 of every order takes off; a landed unit waits on a climb goal
    // over itself before phase 1.
    run(1);
    CHECK(head(mover) == &move && move.phase == 1);
    CHECK(
        move.wait_events == sim::ground_orders::arrived_event +
                                sim::ground_orders::path_failed_event +
                                sim::ground_orders::goal_replaced_event
    );
    // VTOL_Patrol phase 0 clones the patrol leg back to the start and speaks.
    CHECK(
        head(patroller) == &patrol && patrol.phase == 1 && patrol.next &&
        patrol.next->kind == vtol_patrol
    );
    CHECK(services.spoken_by(patroller->unit_index) == std::vector<uint32_t>{5});
    CHECK(services.spoken_by(guard->unit_index) == std::vector<uint32_t>{5});

    // A ground unit keeps the ground missions.
    auto& ground_guard = match.issue_guard(guarded->unit_index, guard->unit_index, false);
    CHECK(ground_guard.kind == sim::match_runtime::follow_ground_kind);

    // One layer switch (set_movement_layer) serves take-off, landing and mirrored units.
    // Any layer but 1 activates the unit; exactly 1 stops it, keeps its turn
    // rate and the bits above the layer, eases its filtered vector once for
    // the attitude and deactivates it. The compare is unmasked, so a layer
    // of 5 lands the unit through its low bits without stopping it.
    auto* lander = match.create({0, aircraft_type, at(120, 120), true, 1, 0});
    CHECK(lander && lander->movement_object);
    const auto lander_index = lander->unit_index;
    auto* landing = match.ground_runtime(lander_index);
    CHECK(landing && (landing->movement.flags & 3) == 1);
    match.set_unit_state_flags(lander_index, 1, false);
    match.set_movement_layer(lander_index, 2);
    CHECK((landing->movement.flags & 3) == 2 && (lander->record.state_flags & 1) != 0);
    using Vector = std::array<sim::unit_movement::Fixed, 3>;
    landing->movement.velocity = {5, -6, 7};
    landing->movement.speed = 0x30000;
    landing->movement.turn = 900;
    landing->movement.flags = 0xf2;
    landing->previous_vector = {0x10000, 0, -0x20000};
    lander->record.heading = 0;
    match.set_movement_layer(lander_index, 1);
    CHECK(landing->movement.velocity == (Vector{0, 0, 0}) && landing->movement.speed == 0);
    CHECK(landing->movement.turn == 900 && landing->movement.flags == 0xf1);
    CHECK(landing->previous_vector == (Vector{0xf333, 0, -0x1e666}));
    CHECK(lander->record.bank < 0 && (lander->record.state_flags & 1) == 0);
    landing->movement.velocity = {5, -6, 7};
    landing->movement.speed = 0x30000;
    match.set_movement_layer(lander_index, 1);
    CHECK(landing->movement.speed == 0x30000 && landing->movement.velocity[2] == 7);
    CHECK(landing->previous_vector[0] == 0xf333);
    match.set_movement_layer(lander_index, 2);
    CHECK(landing->movement.speed == 0x30000 && landing->movement.turn == 900);
    CHECK(landing->movement.flags == 0xf2);
    match.set_unit_state_flags(lander_index, 1, false);
    match.set_movement_layer(lander_index, 5);
    CHECK(landing->movement.speed == 0x30000 && landing->movement.velocity[0] == 5);
    CHECK(landing->movement.flags == 0xf1 && (lander->record.state_flags & 1) != 0);

    // The driver's per-tick slot (the movement tick calls it unconditionally) runs for
    // a carried aircraft too: the owner's driver marks a layer it has not
    // sent yet for sending.
    landing->movement.flags = 0x02;
    match.set_carry_link(lander_index, pad->unit_index, -1, 0);
    CHECK(lander->record.attach_parent != 0);
    auto* driver = match.air_driver(lander_index);
    CHECK(driver && driver->local && driver->goal == nullptr);
    driver->flags =
        static_cast<uint8_t>(sim::air::layer_ground << sim::air::driver_sent_layer_shift);
    run(1);
    CHECK((driver->flags & sim::air::driver_resend) != 0);

    // A landing that finds no free pad says cannot comply, or the voice the
    // match's display rules name.
    const auto landing_voice = [&](sim::match_runtime::Match& played, Services& heard) {
        auto* flier = played.create({0, aircraft_type, at(40, 120), true, 1, 0});
        auto* base = played.create({0, pad_type, at(72, 120), true, 1, 0});
        CHECK(flier && base);
        auto& land = played.issue_order(
            flier->unit_index,
            sim::ground_orders::vtol_landing_kind,
            false,
            base->unit_index,
            nullptr,
            0,
            0
        );
        CHECK(land.kind == sim::ground_orders::vtol_landing_kind);
        land.phase = 3;
        heard.speech.clear();
        sim::match_runtime::MatchTickAccess host(played);
        CHECK(host.dispatch_mission(played.state(), flier->record, land, 0) == 0);
        const auto spoken = heard.spoken_by(flier->unit_index);
        CHECK(spoken.size() == 1);
        return spoken.front();
    };
    CHECK(landing_voice(match, services) == 7);
    input.display.landing_fail_voice = 6;
    Services arrived_services;
    sim::match_runtime::Match arrived(input, arrived_services);
    arrived.configure_strategic_environment({0, 0.5F, 0});
    arrived.simulation().players[0].present = true;
    arrived.simulation().players[0].status = 1;
    arrived.configure_player_alliances(0, allies);
    CHECK(landing_voice(arrived, arrived_services) == 6);

    // A repair pad pays for the aircraft it repairs, as in 3.1c: the landed
    // aircraft's SelfRepair asks the pad for each step, and the energy is
    // requested in the pad's economy block, never the aircraft's. The bottom
    // bar shows a unit's own block, so the cost shows on the pad.
    defs[pad_type].worker_time = 60;
    defs[aircraft_type].build_cost_energy = 200;
    defs[aircraft_type].build_time = 100;
    auto* repaired = match.create({0, aircraft_type, at(40, 232), true, 1, 0});
    auto* repair_pad = match.create({0, pad_type, at(72, 232), true, 1, 0});
    CHECK(repaired && repair_pad);
    repaired->record.health = 40;
    repaired->record.state_flags |= OA_UNIT_STATE_ACTIVE;
    repair_pad->record.state_flags |= OA_UNIT_STATE_ACTIVE;
    repaired->record.economy.energy = {};
    repair_pad->record.economy.energy = {};
    auto& mend = match.insert_ground_order(
        repaired->unit_index,
        sim::match_runtime::self_repair_kind,
        std::nullopt,
        0,
        repair_pad->unit_index
    );
    sim::match_runtime::MatchTickAccess mender(match);
    CHECK(mender.dispatch_mission(match.state(), repaired->record, mend, 0) == 1);
    ++mend.phase;
    CHECK(mender.dispatch_mission(match.state(), repaired->record, mend, 0) == 2);
    CHECK(repaired->record.health == 41);
    // Under 3.1c's repair rate a step pays 1 energy for 1 hit point.
    CHECK(repair_pad->record.economy.energy.requested == 1.0F);
    CHECK(repair_pad->record.economy.energy.accepted == 1.0F);
    CHECK(repaired->record.economy.energy.requested == 0.0F);
    CHECK(repaired->record.economy.energy.accepted == 0.0F);

    std::cout << "vtol missions ok\n";
    return 0;
}
