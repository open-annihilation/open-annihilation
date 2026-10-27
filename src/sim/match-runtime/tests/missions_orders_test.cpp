// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The order and mission handlers of the ground mission block (command, attack,
// guard, construction, repair, reclaim and capture orders), dispatched
// through the match as the order sweep runs them.
#include "match_tick_access.hpp"
#include "oa/sim/ground_orders/goals.hpp"
#include "oa/sim/match_runtime/command.hpp"
#include "oa/sim/match_runtime/construction_orders.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "oa/test/match_services.hpp"

using namespace oa;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #x);   \
    } while (false)

namespace {
// Unit.flags bits of the standing move and fire orders.
constexpr uint32_t standing_order_bits =
    OA_UNIT_FLAG_MOVE_ORDER_MASK | OA_UNIT_FLAG_FIRE_ORDER_MASK;
constexpr int32_t map_cells = 32;
constexpr uint16_t builder_type = 1; // mobile builder with a gun
constexpr uint16_t factory_type = 2; // 2x2 structure that builds tanks
constexpr uint16_t tank_type = 3;    // mobile gun
constexpr uint8_t get_built_kind = 19;
constexpr uint8_t move_ground_kind = 26;
constexpr uint8_t park_kind = 28;
constexpr uint8_t qmove_kind = 30;
constexpr uint8_t qpatrol_kind = 31;

struct Services : oa::test::QuietServices {
    std::vector<uint32_t> speech;
    std::vector<std::string> captions; // one per speech; empty for none
    std::vector<std::string> taken;    // the captions of the last take()

    void command_sound(sim::unit_spawn::Slot&, uint32_t category) override {
        speech.push_back(category);
        captions.emplace_back();
    }

    /// The match's speech hook: a speech with its order's caption.
    static void
    speak(void* context, sim::unit_spawn::Slot&, uint32_t category, const char* caption) {
        auto& services = *static_cast<Services*>(context);
        services.speech.push_back(category);
        services.captions.emplace_back(caption);
    }

    std::vector<uint32_t> take() {
        auto spoken = speech;
        speech.clear();
        taken = std::move(captions);
        captions.clear();
        return spoken;
    }
};

using Scenario = oa::test::EmptyScenario;

// Dispatches the order once and applies the sweep's phase rule for results 0 and 1.
uint32_t step(
    sim::match_runtime::Match& match,
    sim::unit_spawn::Slot& unit,
    sim::simulation_state::Order& order,
    uint32_t events = 0
) {
    sim::match_runtime::MatchTickAccess host(match);
    order.wait_events = 0;
    const auto result = host.dispatch_mission(match.state(), unit.record, order, events);
    if (result == 0)
        order.phase = 0;
    else if (result == 1)
        ++order.phase;
    return result;
}

std::array<uint32_t, 3> at(int32_t x, int32_t z) {
    return {static_cast<uint32_t>(x) << 16, 0, static_cast<uint32_t>(z) << 16};
}

sim::ground_orders::Point point(int32_t x, int32_t z) {
    return {x << 16, 0, z << 16};
}

struct Fixture {
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain =
        std::vector<sim::visibility_state::TerrainCell>(map_cells * map_cells);
    std::array<sim::visibility_state::SightMask, 1> masks{};
    std::shared_ptr<formats::objects3d::Model> model =
        std::make_shared<formats::objects3d::Model>();
    std::shared_ptr<formats::cob::CobProgram> script = std::make_shared<formats::cob::CobProgram>();
    std::shared_ptr<formats::cob::CobProgram> factory_script =
        std::make_shared<formats::cob::CobProgram>();
    static constexpr size_t type_count = 4;
    std::array<sim::unit_spawn::LoadedType, type_count> loaded;
    std::array<sim::unit_spawn::Type, type_count> types;
    std::array<data::unit_definitions::UnitDefinition, type_count> defs;
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    data::unit_definitions::RuntimeDefinitionMetadata metadata;
    std::array<sim::match_runtime::RuntimeTypeFields, type_count> fields{};
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    // Open-yard cells, free for a product once the yard opens.
    std::array<uint8_t, 4> factory_yard{0x35, 0x35, 0x35, 0x35};
    std::vector<sim::spatial_state::Plot> plots =
        std::vector<sim::spatial_state::Plot>(map_cells * map_cells);
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    std::unique_ptr<sim::match_runtime::Match> match;

    /// Builds the map, the three types and a match over them.
    ///
    /// @param tank_maximum_health the tank's maximum health (MaxDamage); 0
    ///        makes a type with none
    explicit Fixture(uint32_t tank_maximum_health = 100) {
        map.attribute_width = map.attribute_height = map_cells;
        map.attributes.resize(map_cells * map_cells);
        masks[0].width = masks[0].height = 1;
        masks[0].pixels = {1};
        model->objects.resize(1);
        model->objects[0].name = "root";
        // Model top (UnitDef.model_height) 20 units above the root.
        model->objects[0].vertices = {{0, 0, 0}, {0, 20 << 16, 0}};
        script->code = {sim::script_vm::opcode::return_};
        script->scripts = {{"Create", 0}};
        script->entry_points = {0};
        script->piece_names = {"root"};
        // Activate opens the yard (YARD_OPEN) and enters the build stance.
        using namespace sim::script_vm;
        factory_script->code = {
            opcode::return_,
            opcode::push_constant,
            18,
            opcode::push_constant,
            1,
            opcode::set_unit_value,
            opcode::push_constant,
            5,
            opcode::push_constant,
            1,
            opcode::set_unit_value,
            opcode::return_
        };
        factory_script->scripts = {{"Create", 0}, {"Activate", 1}};
        factory_script->entry_points = {0, 1};
        factory_script->piece_names = {"root"};
        for (size_t i = 1; i < type_count; ++i) {
            loaded[i].model = model;
            loaded[i].script = script;
            types[i].simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
            types[i].simulation.maximum_health = 100;
            types[i].footprint_x = types[i].footprint_z = 1;
            types[i].bm_code = 1;
            types[i].model = reinterpret_cast<uintptr_t>(model.get());
            types[i].cob = reinterpret_cast<uintptr_t>(script.get());
            defs[i].sight_distance = 200;
            defs[i].acceleration_fixed = 65536;
            defs[i].brake_rate_fixed = 65536;
            defs[i].max_velocity_fixed = 2 * 65536;
            defs[i].turn_rate = 1024;
            defs[i].worker_time = 60;
            defs[i].build_distance = 64;
            defs[i].build_time = 100;
            defs[i].build_cost_energy = 200;
            defs[i].build_cost_metal = 50;
            defs[i].energy_storage = 1000.0F;
            defs[i].metal_storage = 1000.0F;
            fields[i].definition = &defs[i];
            fields[i].yard_mask = yard;
            fields[i].runtime_metadata = &metadata;
            fields[i].target_masks = &target_masks;
        }
        types[builder_type].simulation.flags |=
            OA_UNIT_DEF_FLAG_BUILDER | OA_UNIT_DEF_FLAG_HAS_WEAPONS;
        types[builder_type].simulation.abilities =
            OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_REPAIR |
            OA_UNIT_DEF_ABILITY_CAN_RECLAMATE | OA_UNIT_DEF_ABILITY_CAN_CAPTURE |
            OA_UNIT_DEF_ABILITY_CAN_PATROL | OA_UNIT_DEF_ABILITY_CAN_ATTACK |
            OA_UNIT_DEF_ABILITY_CAN_GUARD | OA_UNIT_DEF_ABILITY_ON_OFFABLE |
            OA_UNIT_DEF_ABILITY_CAN_CLOAK | (2u << OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_SHIFT);
        types[factory_type].simulation.flags |= OA_UNIT_DEF_FLAG_BUILDER;
        types[factory_type].simulation.maximum_health = 1000;
        types[factory_type].bm_code = 0;
        types[factory_type].footprint_x = types[factory_type].footprint_z = 2;
        loaded[factory_type].script = factory_script;
        types[factory_type].cob = reinterpret_cast<uintptr_t>(factory_script.get());
        fields[factory_type].yard_mask = factory_yard;
        types[tank_type].simulation.flags |= OA_UNIT_DEF_FLAG_HAS_WEAPONS;
        types[tank_type].simulation.maximum_health = tank_maximum_health;
        types[tank_type].simulation.abilities = OA_UNIT_DEF_ABILITY_CAN_MOVE |
                                                OA_UNIT_DEF_ABILITY_CAN_ATTACK |
                                                OA_UNIT_DEF_ABILITY_CAN_PATROL;
        defs[builder_type].weapon1 = "TESTGUN";
        defs[tank_type].weapon1 = "TESTGUN";
        for (const auto mobile : {builder_type, tank_type})
            defs[mobile].can_move = defs[mobile].can_attack = defs[mobile].can_patrol = true;
        for (size_t i = 1; i < type_count; ++i) {
            loaded[i].type = types[i];
            if (i != factory_type)
                fields[i].movement_class = 0;
        }
        loaded[tank_type].unit_name = "TANK";
        (void)sim::combat_state::install_weapon_text(
            weapons,
            "[TESTGUN]{id=1; reloadtime=0.1; range=400; lineofsight=1; weaponvelocity=100; "
            "turret=1; [DAMAGE]{default=10;}}"
        );
        sim::match_runtime::OfflineInputs input{
            map,
            loaded,
            types,
            fields,
            weapons,
            terrain,
            masks,
            16,
            16,
            12,
            2,
            0,
            30,
            1,
            &scenario,
            {},
            plots,
            {}
        };
        match = std::make_unique<sim::match_runtime::Match>(input, services);
        match->set_speech_hooks({&services, &Services::speak});
        match->configure_strategic_environment({0, 0.5F, 0});
        for (uint8_t p = 0; p < 2; ++p) {
            match->simulation().players[p].present = true;
            match->simulation().players[p].status = p == 0 ? 1 : 2;
            std::array<uint8_t, 10> allies{};
            allies[p] = 1;
            match->configure_player_alliances(p, allies);
            auto& player = match->state().game.players[p];
            player.energy = player.energy_storage = 1000.0F;
            player.metal = player.metal_storage = 1000.0F;
        }
        match->state().game.tick = 100;
    }

    uint32_t tick() const { return match->state().game.tick; }

    sim::unit_spawn::Slot&
    spawn(uint8_t player, uint16_t type, int32_t x, int32_t z, bool finished = true) {
        auto* slot = match->create({player, type, at(x, z), finished, 1, 0});
        CHECK(slot && slot->unit);
        return *slot;
    }

    // The unit's economy block pays for any request.
    static void fund(sim::unit_spawn::Slot& unit) {
        auto& economy = unit.record.economy;
        economy.energy.requested = economy.energy.accepted = economy.energy.gate = 0.0F;
        economy.metal.requested = economy.metal.accepted = economy.metal.gate = 0.0F;
    }

    sim::simulation_state::Order&
    order(uint16_t unit, uint8_t kind, uint16_t target = 0, int32_t parameter_1 = 0) {
        return match->insert_ground_order(unit, kind, std::nullopt, parameter_1, target);
    }

    sim::simulation_state::Order&
    place_order(uint16_t unit, uint8_t kind, sim::ground_orders::Point where) {
        return match->insert_ground_order(unit, kind, where);
    }

    sim::simulation_state::Order* head(uint16_t unit) { return match->orders(unit).primary; }
};

bool aims_at(
    const sim::unit_spawn::Slot& unit, uint32_t slot, const sim::unit_spawn::Slot& target
) {
    const auto& weapon = unit.record.weapons[slot];
    return weapon.target_b == OA_UNIT_TARGET_IS_UNIT &&
           weapon.target_a == static_cast<int16_t>(target.unit_index);
}

void stop_and_state_orders() {
    Fixture f;
    auto& unit = f.spawn(0, builder_type, 100, 100);
    const auto id = unit.unit_index;

    // Stop drops every weapon target; a ground unit queues nothing.
    auto& target = f.spawn(1, tank_type, 110, 100);
    unit.record.weapons[0].target_a = static_cast<int16_t>(target.unit_index);
    unit.record.weapons[0].target_b = OA_UNIT_TARGET_IS_UNIT;
    // The STOP command replaces the unit's orders and is announced.
    (void)f.match->issue_patrol(id, point(40, 40), false);
    auto& stop = f.match->issue_stop(id);
    CHECK(f.head(id) == &stop && stop.next == nullptr);
    CHECK(step(*f.match, unit, stop) == 5);
    CHECK(f.services.take() == std::vector<uint32_t>{5});
    CHECK(!aims_at(unit, 0, target));
    CHECK(f.head(id) == &stop);
    f.match->stop_orders(id);
    // An airborne aircraft also queues VTOL_LandIfCan where it is.
    f.match->state().unit_defs[builder_type].flags |= OA_UNIT_DEF_FLAG_CAN_FLY;
    unit.record.flags = (unit.record.flags & ~OA_UNIT_FLAG_OCCUPANCY_MASK) | 2u;
    auto& landing = f.order(id, sim::match_runtime::stop_kind);
    CHECK(step(*f.match, unit, landing) == 5);
    CHECK(f.head(id) && f.head(id)->kind == sim::ground_orders::vtol_land_if_can_kind);
    f.match->stop_orders(id);
    f.match->state().unit_defs[builder_type].flags &= ~OA_UNIT_DEF_FLAG_CAN_FLY;
    unit.record.flags = (unit.record.flags & ~OA_UNIT_FLAG_OCCUPANCY_MASK) | 1u;

    // MakeSelectable clears the unselectable bit and sets bit 0x20.
    unit.record.flags =
        (unit.record.flags | OA_UNIT_FLAG_NOT_SELECTABLE) & ~OA_UNIT_FLAG_SELECTABLE;
    CHECK(step(*f.match, unit, f.order(id, sim::match_runtime::make_selectable_kind)) == 5);
    CHECK(
        !(unit.record.flags & OA_UNIT_FLAG_NOT_SELECTABLE) &&
        (unit.record.flags & OA_UNIT_FLAG_SELECTABLE)
    );
    f.match->stop_orders(id);

    // Activate and Deactivate switch an on/off type.
    unit.record.state_flags &= static_cast<uint8_t>(~OA_UNIT_STATE_ACTIVE);
    CHECK(
        step(
            *f.match, unit, f.match->issue_state_order(id, sim::match_runtime::activate_kind, 0)
        ) == 5
    );
    CHECK(unit.record.state_flags & OA_UNIT_STATE_ACTIVE);
    f.match->stop_orders(id);
    CHECK(
        step(
            *f.match, unit, f.match->issue_state_order(id, sim::match_runtime::deactivate_kind, 0)
        ) == 5
    );
    CHECK(!(unit.record.state_flags & OA_UNIT_STATE_ACTIVE));
    f.match->stop_orders(id);
    // A type that is not on/off stays as it is.
    auto& tank = f.spawn(0, tank_type, 60, 60);
    const auto tank_state = tank.record.state_flags;
    CHECK(
        step(
            *f.match,
            tank,
            f.match->issue_state_order(tank.unit_index, sim::match_runtime::activate_kind, 0)
        ) == 5
    );
    CHECK(tank.record.state_flags == tank_state);
    f.match->stop_orders(tank.unit_index);

    // Cloak_On and Cloak_Off keep the cloak running for a cloakable type only.
    CHECK(step(*f.match, unit, f.match->issue_cloak(id, true)) == 5);
    CHECK(unit.record.flags & OA_UNIT_FLAG_CLOAK_RUNNING);
    f.match->stop_orders(id);
    CHECK(step(*f.match, unit, f.match->issue_cloak(id, false)) == 5);
    CHECK(!(unit.record.flags & OA_UNIT_FLAG_CLOAK_RUNNING));
    f.match->stop_orders(id);
    CHECK(step(*f.match, tank, f.match->issue_cloak(tank.unit_index, true)) == 5);
    CHECK(!(tank.record.flags & OA_UNIT_FLAG_CLOAK_RUNNING));
    f.match->stop_orders(tank.unit_index);

    // Standing_MoveOrder writes flag bits 18..19 from the order's first parameter.
    CHECK(
        step(
            *f.match,
            unit,
            f.match->issue_state_order(id, sim::match_runtime::standing_move_order_kind, 6)
        ) == 5
    );
    CHECK(
        (unit.record.flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) == (2u << OA_UNIT_FLAG_MOVE_ORDER_SHIFT)
    );
    f.match->stop_orders(id);

    // Standing_FireOrder writes bits 20..21; hold fire drops the targets of the
    // weapons that pick their own (slot flag 0x10).
    unit.record.weapons[0].target_a = static_cast<int16_t>(target.unit_index);
    unit.record.weapons[0].target_b = OA_UNIT_TARGET_IS_UNIT;
    unit.record.weapons[0].flags |= OA_UNIT_WEAPON_RETALIATE;
    CHECK(
        step(
            *f.match,
            unit,
            f.match->issue_state_order(id, sim::match_runtime::standing_fire_order_kind, 0)
        ) == 5
    );
    CHECK((unit.record.flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) == 0);
    CHECK(!aims_at(unit, 0, target));
    f.match->stop_orders(id);
    unit.record.weapons[0].target_a = static_cast<int16_t>(target.unit_index);
    unit.record.weapons[0].target_b = OA_UNIT_TARGET_IS_UNIT;
    CHECK(
        step(
            *f.match,
            unit,
            f.match->issue_state_order(id, sim::match_runtime::standing_fire_order_kind, 2)
        ) == 5
    );
    CHECK(
        (unit.record.flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) == (2u << OA_UNIT_FLAG_FIRE_ORDER_SHIFT)
    );
    CHECK(aims_at(unit, 0, target));
    f.match->stop_orders(id);

    // QMove waits a minute and passes the order on.
    auto& queued = f.order(id, qmove_kind);
    CHECK(
        step(*f.match, unit, queued) == 6 && queued.wake_tick == f.tick() + 0x3c &&
        (queued.wait_events & 1)
    );
    f.match->stop_orders(id);

    // WaitForAttack: no target finishes; with one it waits on 0x18, then ends.
    CHECK(step(*f.match, unit, f.order(id, sim::match_runtime::wait_for_attack_kind)) == 5);
    f.match->stop_orders(id);
    auto& wait = f.order(id, sim::match_runtime::wait_for_attack_kind, target.unit_index);
    CHECK(step(*f.match, unit, wait) == 1 && wait.wait_events == 0x18);
    CHECK(step(*f.match, unit, wait) == 5);
    wait.phase = 2;
    CHECK(step(*f.match, unit, wait) == 7);
    f.match->stop_orders(id);

    // BeCarried: a free unit finishes; a carried one frees its weapons, then
    // waits ten ticks at a time.
    CHECK(step(*f.match, unit, f.order(id, sim::match_runtime::be_carried_kind)) == 5);
    f.match->stop_orders(id);
    unit.record.attach_parent = oa::oa_unit_ref_from_slot(tank.unit_index);
    auto& carried = f.order(id, sim::match_runtime::be_carried_kind);
    CHECK(step(*f.match, unit, carried) == 1);
    CHECK(step(*f.match, unit, carried) == 2 && carried.wake_tick == f.tick() + 10);
    CHECK(carried.phase == 1);
    unit.record.attach_parent = 0;
    f.match->stop_orders(id);
}

void self_destruct_counts_down() {
    Fixture f;
    auto& unit = f.spawn(0, builder_type, 100, 100);
    const auto id = unit.unit_index;
    // A countdown of two speaks count2, count1 and count0 a second apart.
    const std::array<uint16_t, 1> selected{id};
    f.match->toggle_self_destruct(selected);
    auto* countdown = f.match->orders(id).secondary;
    CHECK(countdown && countdown->kind == sim::match_runtime::self_destruct_kind);
    CHECK(f.match->self_destruct_remaining(id) == 2);
    CHECK(step(*f.match, unit, *countdown) == 1);
    CHECK(f.services.take() == std::vector<uint32_t>{0x14});
    CHECK(countdown->wake_tick == f.tick() + 0x1e && countdown->wait_events == 3);
    CHECK(f.match->self_destruct_remaining(id) == 2);
    CHECK(step(*f.match, unit, *countdown) == 1);
    CHECK(f.services.take() == std::vector<uint32_t>{0x15});
    CHECK(step(*f.match, unit, *countdown) == 1);
    CHECK(f.services.take() == std::vector<uint32_t>{0x16});
    CHECK(countdown->wake_tick < f.tick() + 0xf);
    CHECK(f.match->self_destruct_remaining(id) == 0);
    CHECK(step(*f.match, unit, *countdown) == 5);
    CHECK(unit.record.flags & OA_UNIT_FLAG_DEATH_PENDING);

    // A second Ctrl+D cancels the countdown: tearing the order down wakes it
    // with the cancel event and it says so.
    Fixture g;
    auto& other = g.spawn(0, builder_type, 100, 100);
    const std::array<uint16_t, 1> again{other.unit_index};
    g.match->toggle_self_destruct(again);
    auto* pending = g.match->orders(other.unit_index).secondary;
    CHECK(step(*g.match, other, *pending) == 1);
    g.services.take();
    pending->wait_events = 3;
    g.match->toggle_self_destruct(again);
    CHECK(g.match->orders(other.unit_index).secondary == nullptr);
    CHECK(g.services.take() == std::vector<uint32_t>{0x17});
    CHECK(!(other.record.flags & OA_UNIT_FLAG_DEATH_PENDING));

    // A type without a countdown blows up on the first dispatch.
    auto& tank = g.spawn(0, tank_type, 60, 60);
    const std::array<uint16_t, 1> tank_only{tank.unit_index};
    g.match->toggle_self_destruct(tank_only);
    CHECK(step(*g.match, tank, *g.match->orders(tank.unit_index).secondary) == 5);
    CHECK(tank.record.flags & OA_UNIT_FLAG_DEATH_PENDING);
}

void attack_orders() {
    Fixture f;
    auto& unit = f.spawn(0, builder_type, 100, 100);
    const auto id = unit.unit_index;
    auto& enemy = f.spawn(1, tank_type, 140, 100);

    // Attack_NoMove: phase 1 frees weapon 0 and aims it; phase 2 gives up.
    auto& hold = f.order(id, sim::match_runtime::attack_no_move_kind, enemy.unit_index);
    CHECK(step(*f.match, unit, hold) == 1);
    CHECK(step(*f.match, unit, hold) == 1 && hold.wait_events == 0x11808);
    CHECK(aims_at(unit, 0, enemy));
    CHECK(step(*f.match, unit, hold) == 9);
    CHECK(step(*f.match, unit, hold, 0x800) == 5);
    f.match->stop_orders(id);
    CHECK(step(*f.match, unit, f.order(id, sim::match_runtime::attack_no_move_kind)) == 5);
    f.match->stop_orders(id);

    // Attack_Chase: phase 0 stores the start and picks the first enabled
    // weapon; phase 1 fires on a target in reach and waits on 0x13808.
    auto& chase = f.order(id, sim::match_runtime::attack_chase_kind, enemy.unit_index);
    CHECK(step(*f.match, unit, chase) == 1);
    CHECK(step(*f.match, unit, chase) == 2 && chase.wait_events == 0x13808 && chase.phase == 1);
    CHECK(aims_at(unit, 0, enemy));
    // A weapon out of reach sends it to the approach: circle at range first.
    CHECK(step(*f.match, unit, chase, 0x1000) == 1 && chase.phase == 2);
    CHECK(step(*f.match, unit, chase) == 1 && chase.phase == 3);
    // Phase 3 re-checks every second, waiting on the goal and range events.
    CHECK(
        step(*f.match, unit, chase) == 2 && chase.wait_events == 0x148e9 &&
        chase.wake_tick == f.tick() + 0x1e
    );
    CHECK(step(*f.match, unit, chase, 0x20) == 4 && chase.phase == 1);
    CHECK(step(*f.match, unit, chase, 0x800) == 5);
    f.match->stop_orders(id);

    // AttackSpecial becomes the attack the resolver picks, on the third weapon.
    auto& special = f.match->issue_attack_special(id, point(140, 100), false, enemy.unit_index);
    CHECK(step(*f.match, unit, special) == 2);
    CHECK(special.kind == sim::match_runtime::attack_chase_kind);
    // Only bits 0x600 of the old packed descriptor survive the change.
    CHECK(special.preserve_flags == 0x80 && special.flags == 0);
    f.match->stop_orders(id);

    // Suppress: phase 0 takes the range, phase 1 aims both guns at the point,
    // phase 2 moves within the shrinking range around it.
    auto* ordered = f.match->issue_attack_ground(id, point(160, 100), false);
    CHECK(ordered && ordered->kind == sim::match_runtime::suppress_kind);
    auto& suppress = *ordered;
    CHECK(step(*f.match, unit, suppress) == 1);
    CHECK(step(*f.match, unit, suppress) == 1 && suppress.wait_events == 0x1c00);
    CHECK(unit.record.weapons[0].target_a == 160 && unit.record.weapons[0].target_b == 100);
    CHECK(unit.record.weapons[1].target_a == 160 && unit.record.weapons[1].target_b == 100);
    CHECK(step(*f.match, unit, suppress, 0x400) == 6 && suppress.phase == 1);
    suppress.phase = 2;
    CHECK(
        step(*f.match, unit, suppress) == 4 && suppress.phase == 1 && suppress.wait_events == 0xe0
    );
    CHECK(step(*f.match, unit, suppress, 0x800) == 5);
    f.match->stop_orders(id);

    // Command 9 gives a type with the repair bit RepairPatrol.
    CHECK(
        f.match->issue_patrol(id, point(40, 40), false).kind ==
        sim::match_runtime::repair_patrol_kind
    );
    f.match->stop_orders(id);

    // Patrol, which a type without it gets: phase 0 queues the loop back,
    // phase 1 walks to the point and waits 15 ticks or the goal, phase 2
    // rotates on arrival.
    auto& tank = f.spawn(0, tank_type, 60, 100);
    const auto tank_id = tank.unit_index;
    auto& patrol = f.match->issue_patrol(tank_id, point(40, 40), false);
    CHECK(patrol.kind == sim::match_runtime::patrol_kind);
    CHECK(step(*f.match, tank, patrol) == 1 && patrol.wake_tick == f.tick() + 1);
    std::vector<uint8_t> kinds;
    f.match->visit_primary_queue(tank_id, [&](const auto& queued) {
        kinds.push_back(queued.kind);
    });
    CHECK(
        (kinds ==
         std::vector<uint8_t>{sim::match_runtime::patrol_kind, sim::match_runtime::patrol_kind})
    );
    CHECK(
        step(*f.match, tank, patrol) == 1 && patrol.wait_events == 0xe1 &&
        patrol.wake_tick == f.tick() + 0xf
    );
    CHECK(step(*f.match, tank, patrol, 0x20) == 6 && patrol.phase == 1);
    f.match->stop_orders(tank_id);
}

void guard_and_teleport() {
    Fixture f;
    auto& unit = f.spawn(0, builder_type, 100, 100);
    const auto id = unit.unit_index;
    auto& guarded = f.spawn(0, tank_type, 140, 100);

    // Follow_Ground: phase 0 keeps four footprints of room around the
    // guarded unit; phase 1 follows it, waiting a second or for 0x18.
    auto& guard = f.match->issue_guard(id, guarded.unit_index, false);
    CHECK(guard.kind == sim::match_runtime::follow_ground_kind);
    CHECK(step(*f.match, unit, guard) == 1);
    CHECK(
        step(*f.match, unit, guard) == 2 && guard.wait_events == 0x19 &&
        guard.wake_tick == f.tick() + 0x1e
    );
    // A damaged guarded unit gets repaired by a builder.
    guarded.record.health = 40;
    CHECK(step(*f.match, unit, guard) == 3 && guard.wait_events == 0);
    CHECK(f.head(id) && f.head(id)->kind == sim::match_runtime::repair_unit_kind);
    f.match->stop_orders(id);
    guarded.record.health = 100;
    // Its attacker gets attacked when the guarded unit is hit.
    auto& enemy = f.spawn(1, tank_type, 150, 110);
    guarded.record.last_attacker_id = enemy.unit_index;
    auto& strike = f.match->issue_guard(id, guarded.unit_index, false);
    strike.phase = 1;
    CHECK(step(*f.match, unit, strike, 0x10) == 3);
    CHECK(f.head(id) && f.head(id)->kind == sim::match_runtime::attack_chase_kind);
    f.match->stop_orders(id);
    // An attached guard, or a flying guarded unit, cannot guard.
    unit.record.attach_parent = oa::oa_unit_ref_from_slot(guarded.unit_index);
    CHECK(step(*f.match, unit, f.match->issue_guard(id, guarded.unit_index, false)) == 7);
    unit.record.attach_parent = 0;
    f.match->stop_orders(id);

    // Teleport moves every other unit inside the teleporter's bounds by the
    // offset to the order point.
    auto& gate = f.spawn(0, factory_type, 200, 200);
    auto& passenger = f.spawn(0, tank_type, 202, 198);
    auto& outside = f.spawn(0, tank_type, 240, 240);
    CHECK(
        step(
            *f.match,
            gate,
            f.place_order(gate.unit_index, sim::match_runtime::teleport_kind, point(300, 300))
        ) == 5
    );
    CHECK(passenger.record.position.x == (302 << 16) && passenger.record.position.z == (298 << 16));
    CHECK(outside.record.position.x == (240 << 16));
    CHECK(gate.record.position.x == (200 << 16));
}

void construction_orders() {
    Fixture f;
    auto& builder = f.spawn(0, builder_type, 100, 100);
    const auto id = builder.unit_index;

    // MobileBuild: phase 0 snaps the site to the footprint grid and heads for
    // its border.
    auto& build = f.match->issue_mobile_build(id, factory_type, point(150, 101), false);
    CHECK(step(*f.match, builder, build) == 1 && build.wait_events == 0xe0);
    // Phase 1 places the frame, speaks 9 and hands it a GetBuilt order.
    CHECK(step(*f.match, builder, build) == 1);
    CHECK(f.services.take() == std::vector<uint32_t>{9});
    CHECK(f.services.taken == std::vector<std::string>{"Starting construction"});
    sim::unit_spawn::Slot* frame = nullptr;
    for (auto& slot : f.match->world().slots)
        if (slot.unit && slot.record.type_index == factory_type)
            frame = &slot;
    CHECK(frame && frame->record.build_remaining != 0.0F);
    // (150, 101) snaps to the 2x2 site on cells 8..9 by 5..6.
    CHECK(frame->record.position.x == (144 << 16) && frame->record.position.z == (96 << 16));
    CHECK(f.head(frame->unit_index) && f.head(frame->unit_index)->kind == get_built_kind);
    CHECK(build.flags & sim::match_runtime::MatchTickAccess::building_flag);
    // Phase 2 waits for the build stance; phase 3 nanolathes each tick.
    CHECK(step(*f.match, builder, build) == 1);
    Fixture::fund(builder);
    const auto before = frame->record.build_remaining;
    CHECK(step(*f.match, builder, build) == 2 && build.wait_events == 0xb);
    CHECK(frame->record.build_remaining < before);
    CHECK(builder.record.decloak_until_tick == f.tick() + 300);
    uint32_t steps = 0;
    while (build.phase == 3 && steps++ < 200) {
        Fixture::fund(builder);
        (void)step(*f.match, builder, build);
    }
    CHECK(frame->record.build_remaining == 0.0F && build.phase == 4);
    CHECK(step(*f.match, builder, build) == 5);
    CHECK(f.services.take() == std::vector<uint32_t>{8});
    CHECK(f.services.taken == std::vector<std::string>{"Building complete"});

    // GetBuilt of the finished structure ends there: it has no movement
    // object to park.
    auto& built = *f.head(frame->unit_index);
    CHECK(step(*f.match, *frame, built) == 5);
    CHECK(f.head(frame->unit_index) == &built && !built.next);
    f.match->stop_orders(frame->unit_index);

    // A loss of the frame ends MobileBuild with category 7; a cancel quietly.
    auto& lost = f.match->issue_mobile_build(id, factory_type, point(60, 60), false);
    CHECK(step(*f.match, builder, lost, 8) == 8);
    CHECK(f.services.take() == std::vector<uint32_t>{7});
    CHECK(f.services.taken == std::vector<std::string>{"Construction terminated"});
    CHECK(step(*f.match, builder, lost, 2) == 5);
    f.match->stop_orders(id);

    // HelpBuild joins an unfinished frame and works it; a finished one ends it.
    auto& other = f.spawn(0, factory_type, 60, 140, false);
    auto& help = f.match->issue_help_build(id, other.unit_index, false);
    CHECK(help.kind == sim::match_runtime::help_build_kind);
    CHECK(step(*f.match, builder, help) == 1 && help.wait_events == 0xe8);
    CHECK(
        step(*f.match, builder, help) == 1 &&
        (help.flags & sim::match_runtime::MatchTickAccess::building_flag)
    );
    CHECK(step(*f.match, builder, help) == 1);
    Fixture::fund(builder);
    const auto help_before = other.record.build_remaining;
    CHECK(step(*f.match, builder, help) == 2 && other.record.build_remaining < help_before);
    other.record.build_remaining = 0.0F;
    help.phase = 1;
    CHECK(step(*f.match, builder, help) == 5);
    f.match->stop_orders(id);
}

// A queued MobileBuild can be moved onto another site until its frame exists.
// The one the builder is already walking to starts that walk again; a later
// order only changes its site. A move order, and an index past the queue, are
// left alone.
void queued_build_can_be_moved() {
    Fixture f;
    auto& builder = f.spawn(0, builder_type, 100, 100);
    const auto id = builder.unit_index;
    auto& first = f.match->issue_mobile_build(id, factory_type, point(150, 101), false);
    (void)f.match->issue_mobile_build(id, factory_type, point(200, 160), true);

    using Record = sim::match_runtime::Match::OrderRecordView;
    const auto queue = [&] {
        std::array<Record, 8> records{};
        const auto count = f.match->queue_records(id, false, records.data(), records.size());
        return std::pair{count, records};
    };
    const auto snapped_of = [](sim::ground_orders::Point site) {
        sim::match_runtime::snap_build_position(site, 2, 2);
        return site;
    };

    const auto later = snapped_of(point(220, 180));
    CHECK(f.match->relocate_queued_build(id, 1, point(220, 180)));
    {
        const auto [count, records] = queue();
        CHECK(count == 2);
        CHECK(records[1].kind == sim::match_runtime::mobile_build_kind);
        CHECK(records[1].phase == 0 && !records[1].build_started);
        CHECK(records[1].point[0] == later[0] && records[1].point[2] == later[2]);
        CHECK(records[0].point[0] == (150 << 16) && records[0].point[2] == (101 << 16));
    }
    CHECK(!f.match->relocate_queued_build(id, 2, point(40, 40)));

    // Phase 0 snaps the head and heads for its border.
    CHECK(step(*f.match, builder, first) == 1 && first.phase == 1);
    const auto* goal = f.match->ground_runtime(id)->navigation.goal;
    CHECK(goal != nullptr);
    const auto old_left = goal->left;
    const auto old_top = goal->top;
    first.raised_events = sim::ground_orders::path_failed_event;

    const auto head_site = snapped_of(point(70, 90));
    CHECK(f.match->relocate_queued_build(id, 0, point(70, 90)));
    CHECK(first.phase == 0);
    CHECK(f.match->ground_runtime(id)->navigation.goal == nullptr);
    CHECK((first.raised_events & sim::ground_orders::goal_event_mask) == 0);
    {
        const auto [count, records] = queue();
        CHECK(count == 2);
        CHECK(records[0].point[0] == head_site[0] && records[0].point[2] == head_site[2]);
        CHECK(records[1].point[0] == later[0] && records[1].point[2] == later[2]);
    }

    // The approach starts again at the new site, and the frame is placed there.
    CHECK(step(*f.match, builder, first) == 1 && first.phase == 1);
    const auto* again = f.match->ground_runtime(id)->navigation.goal;
    CHECK(again != nullptr && (again->left != old_left || again->top != old_top));
    CHECK(step(*f.match, builder, first) == 1);
    sim::unit_spawn::Slot* frame = nullptr;
    for (auto& slot : f.match->world().slots)
        if (slot.unit && slot.record.type_index == factory_type)
            frame = &slot;
    CHECK(frame != nullptr);
    CHECK(frame->record.position.x == head_site[0] && frame->record.position.z == head_site[2]);

    const auto built_at = queue().second[0].point;
    CHECK(!f.match->relocate_queued_build(id, 0, point(30, 30)));
    CHECK(queue().second[0].point[0] == built_at[0] && queue().second[0].point[2] == built_at[2]);

    const auto tail = snapped_of(point(40, 200));
    CHECK(f.match->relocate_queued_build(id, 1, point(40, 200)));
    CHECK(queue().second[1].point[0] == tail[0] && queue().second[1].point[2] == tail[2]);
    CHECK(queue().second[0].point[0] == built_at[0]);

    f.match->stop_orders(id);
    auto& move = f.match->issue_ground_move(id, point(80, 80), false);
    CHECK(!f.match->relocate_queued_build(id, 0, point(90, 90)));
    CHECK(move.kind == move_ground_kind);
    CHECK(queue().second[0].point[0] == (80 << 16) && queue().second[0].point[2] == (80 << 16));
}

void factory_orders() {
    Fixture f;
    auto& factory = f.spawn(0, factory_type, 100, 100);
    const auto id = factory.unit_index;
    factory.record.flags |=
        (1u << OA_UNIT_FLAG_MOVE_ORDER_SHIFT) | (2u << OA_UNIT_FLAG_FIRE_ORDER_SHIFT);

    // BuildingBuild: phase 0 switches the factory on, phase 1 waits for the
    // build stance, phase 2 places the frame on the pad.
    auto& order = f.match->issue_building_build(id, tank_type, 2, false);
    // queued_build_count sums the second parameters of the build orders (descriptor bit
    // 0x100) of the type.
    CHECK(f.match->queued_build_count(id, tank_type) == 2);
    CHECK(f.match->queued_build_count(id, factory_type) == 0);
    CHECK(
        step(*f.match, factory, order) == 1 && (factory.record.state_flags & OA_UNIT_STATE_ACTIVE)
    );
    // Until Activate has run, the factory waits on a script value or a cancel.
    CHECK(step(*f.match, factory, order) == 2 && order.wait_events == 6);
    f.match->tick_scripts(1);
    CHECK(step(*f.match, factory, order) == 1);
    CHECK(step(*f.match, factory, order) == 1);
    CHECK(f.services.take() == std::vector<uint32_t>{9});
    CHECK(f.services.taken == std::vector<std::string>{"Starting construction"});
    sim::unit_spawn::Slot* frame = nullptr;
    for (auto& slot : f.match->world().slots)
        if (slot.unit && slot.record.type_index == tank_type)
            frame = &slot;
    CHECK(frame && frame->record.build_remaining != 0.0F);
    CHECK(sim::match_runtime::link_parent(frame->record) == id);
    CHECK(
        (frame->record.flags & standing_order_bits) == (factory.record.flags & standing_order_bits)
    );
    CHECK(f.head(frame->unit_index)->kind == get_built_kind);
    // Phase 3 builds a step a tick.
    Fixture::fund(factory);
    const auto before = frame->record.build_remaining;
    CHECK(step(*f.match, factory, order) == 2 && order.wait_events == 0xb);
    CHECK(frame->record.build_remaining < before);
    uint32_t steps = 0;
    while (order.phase == 3 && steps++ < 200) {
        Fixture::fund(factory);
        (void)step(*f.match, factory, order);
    }
    CHECK(order.phase == 4);
    // Phase 4 counts the product off and starts over.
    CHECK(step(*f.match, factory, order) == 0);
    // A factory's finished product is announced with the category's own text.
    CHECK(f.services.take() == std::vector<uint32_t>{8});
    CHECK(f.services.taken == std::vector<std::string>{""});
    CHECK(f.match->queued_build_count(id, tank_type) == 1);

    // With no moves queued on the factory, a finished tank's GetBuilt parks it.
    f.match->stop_orders(id);
    auto& parked = f.spawn(0, tank_type, 60, 100);
    auto& arrival = f.order(parked.unit_index, get_built_kind, id);
    CHECK(step(*f.match, parked, arrival) == 5);
    CHECK(arrival.next && arrival.next->kind == park_kind);
    f.match->stop_orders(parked.unit_index);

    // A move or patrol given to the factory, which has no movement object,
    // queues QMove or QPatrol there, and the finished tank's GetBuilt takes
    // them on as its own move and patrol.
    CHECK(f.match->takes_move_order(id) == false);
    f.match->state().unit_defs[factory_type].abilities |=
        OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_PATROL;
    CHECK(f.match->takes_move_order(id));
    CHECK(f.match->issue_ground_move(id, point(20, 20), false).kind == qmove_kind);
    CHECK(f.match->issue_patrol(id, point(40, 20), true).kind == qpatrol_kind);
    auto& got = *f.head(frame->unit_index);
    CHECK(step(*f.match, *frame, got) == 5);
    std::vector<uint8_t> kinds;
    f.match->visit_primary_queue(frame->unit_index, [&](const auto& queued) {
        kinds.push_back(queued.kind);
    });
    CHECK(std::find(kinds.begin(), kinds.end(), move_ground_kind) != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), sim::match_runtime::patrol_kind) != kinds.end());
    CHECK(std::find(kinds.begin(), kinds.end(), park_kind) == kinds.end());
    CHECK(
        (frame->record.flags & standing_order_bits) == (factory.record.flags & standing_order_bits)
    );
    // The factory passes its QMove on each minute.
    auto& rally = *f.head(id);
    CHECK(step(*f.match, factory, rally) == 6 && rally.wake_tick == f.tick() + 0x3c);
    f.match->stop_orders(id);

    // An unfinished frame waits for its builders, then decays each 11 ticks
    // of the timer.
    auto& idle = f.spawn(0, tank_type, 60, 60, false);
    idle.record.build_remaining = 0.5F;
    auto& waiting = f.order(idle.unit_index, get_built_kind);
    CHECK(step(*f.match, idle, waiting) == 1 && waiting.wake_tick == f.tick() + 300);
    CHECK(waiting.wait_events == 0x8001);
    CHECK(step(*f.match, idle, waiting) == 1 && waiting.wake_tick == f.tick() + 0x1e);
    Fixture::fund(idle);
    CHECK(step(*f.match, idle, waiting, 1) == 2 && waiting.wake_tick == f.tick() + 0xb);
    CHECK(idle.record.build_remaining > 0.5F);

    // A cancel refunds trunc((1 - unbuilt share) * metal cost) and destroys
    // the frame; after two float steps of 0.02 that truncates to 1.
    f.match->place_unit_at(frame->unit_index, 200 << 16, 0, 200 << 16, 1);
    auto& again = f.match->issue_building_build(id, tank_type, 1, false);
    again.phase = 2;
    CHECK(step(*f.match, factory, again) == 1);
    sim::unit_spawn::Slot* second = nullptr;
    for (auto& slot : f.match->world().slots)
        if (slot.unit && slot.record.type_index == tank_type && slot.record.build_remaining == 1.0F)
            second = &slot;
    CHECK(second);
    for (int work = 0; work < 2; ++work) {
        Fixture::fund(factory);
        CHECK(step(*f.match, factory, again) == 2);
    }
    const auto metal = factory.record.economy.metal.produced;
    const auto refund =
        static_cast<int32_t>((1.0 - static_cast<double>(second->record.build_remaining)) * 50.0);
    CHECK(refund == 1);
    CHECK(sim::match_runtime::link_parent(second->record) == id);
    CHECK(step(*f.match, factory, again, 2) == 5);
    CHECK(factory.record.economy.metal.produced == metal + static_cast<float>(refund));
    // The builder link finishes the frame and takes it off the pad before
    // the 30000 damage.
    CHECK(
        second->record.build_remaining == 0.0F && !sim::match_runtime::link_parent(second->record)
    );
    CHECK(second->record.flags & OA_UNIT_FLAG_DEATH_PENDING);
    CHECK(!(factory.record.state_flags & OA_UNIT_STATE_ACTIVE));
}

// A factory a constructor builds, run through the match's own tick: it has no
// movement object, so its GetBuilt parks nothing, and the product it is then
// given is built and finished.
// change_queued_count: a positive change goes to the last queued order when it builds
// the same type, otherwise to a new queued order; a negative one comes off the
// latest order for the type, and each order it empties is removed.
void factory_queue_counts() {
    Fixture f;
    auto& factory = f.spawn(0, factory_type, 100, 100);
    const auto id = factory.unit_index;
    using Entry = std::pair<int32_t, int32_t>; // type, count
    const auto queue = [&] {
        std::vector<Entry> entries;
        std::array<sim::match_runtime::Match::OrderRecordView, 8> records{};
        const auto count = f.match->queue_records(id, false, records.data(), records.size());
        for (std::size_t i = 0; i < count; ++i) {
            CHECK(records[i].kind == sim::match_runtime::building_build_kind);
            CHECK(
                records[i].target == 0 &&
                (records[i].command_flags & (sim::match_runtime::command_unqueued |
                                             sim::match_runtime::command_has_point)) == 0
            );
            entries.emplace_back(records[i].parameter_1, records[i].parameter_2);
        }
        return entries;
    };
    f.match->queue_factory_build(id, tank_type, 2);
    f.match->queue_factory_build(id, builder_type, 1);
    f.match->queue_factory_build(id, tank_type, 3);
    f.match->queue_factory_build(id, tank_type, 1);
    CHECK((queue() == std::vector<Entry>{{tank_type, 2}, {builder_type, 1}, {tank_type, 4}}));
    // -5 empties the last tank order (4) and takes the remaining 1 from the first.
    f.match->queue_factory_build(id, tank_type, -5);
    CHECK((queue() == std::vector<Entry>{{tank_type, 1}, {builder_type, 1}}));
    // A count equal to the change is emptied too, and nothing further matches.
    f.match->queue_factory_build(id, tank_type, -1);
    CHECK((queue() == std::vector<Entry>{{builder_type, 1}}));
    f.match->queue_factory_build(id, tank_type, -3);
    CHECK((queue() == std::vector<Entry>{{builder_type, 1}}));
}

// issue_or_cancel_order: a queued order removes the first queued order of its kind whose
// target matches and whose point lies within 16 pixels (inclusive) on x and on
// z, instead of being issued; an unqueued one is always issued.
void queued_order_cancels_its_match() {
    Fixture f;
    auto& tank = f.spawn(0, tank_type, 100, 100);
    auto& other = f.spawn(1, tank_type, 200, 100);
    auto& third = f.spawn(1, tank_type, 240, 100);
    const auto id = tank.unit_index;
    f.match->stop_orders(id);
    const auto kinds = [&] {
        std::vector<uint8_t> found;
        std::array<sim::match_runtime::Match::OrderRecordView, 8> records{};
        const auto count = f.match->queue_records(id, false, records.data(), records.size());
        for (std::size_t i = 0; i < count; ++i)
            found.push_back(records[i].kind);
        return found;
    };
    const auto a = point(150, 100);
    const auto edge = point(166, 84);
    const auto beyond = point(150, 117);
    f.match->issue_or_cancel_order(id, qmove_kind, true, 0, &a, 0, 0);
    CHECK(kinds() == std::vector<uint8_t>{qmove_kind});
    f.match->issue_or_cancel_order(id, qmove_kind, true, 0, &edge, 0, 0);
    CHECK(kinds().empty());
    f.match->issue_or_cancel_order(id, qmove_kind, true, 0, &a, 0, 0);
    f.match->issue_or_cancel_order(id, qmove_kind, true, 0, &beyond, 0, 0);
    CHECK((kinds() == std::vector<uint8_t>{qmove_kind, qmove_kind}));
    // A null point matches any point.
    f.match->issue_or_cancel_order(id, qmove_kind, true, 0, nullptr, 0, 0);
    CHECK(kinds() == std::vector<uint8_t>{qmove_kind});
    f.match->stop_orders(id);
    const auto chase = sim::match_runtime::attack_chase_kind;
    f.match->issue_or_cancel_order(id, chase, true, other.unit_index, nullptr, 0, 0);
    CHECK(kinds() == std::vector<uint8_t>{chase});
    f.match->issue_or_cancel_order(id, chase, true, third.unit_index, nullptr, 0, 0);
    CHECK((kinds() == std::vector<uint8_t>{chase, chase}));
    f.match->issue_or_cancel_order(id, chase, true, other.unit_index, nullptr, 0, 0);
    CHECK(kinds() == std::vector<uint8_t>{chase});
    f.match->issue_or_cancel_order(id, chase, false, other.unit_index, nullptr, 0, 0);
    CHECK(kinds() == std::vector<uint8_t>{chase});
    f.match->stop_orders(id);
    // The match on its own, as the pointer orders use it: true when it took
    // one off, and a target of 0 matches any.
    f.match->issue_or_cancel_order(id, qmove_kind, true, 0, &a, 0, 0);
    f.match->issue_or_cancel_order(id, chase, true, other.unit_index, &a, 0, 0);
    CHECK(!f.match->cancel_queued_order(id, qmove_kind, 0, &beyond));
    CHECK(!f.match->cancel_queued_order(id, chase, third.unit_index, &a));
    CHECK(f.match->cancel_queued_order(id, chase, 0, &edge));
    CHECK(kinds() == std::vector<uint8_t>{qmove_kind});
    CHECK(f.match->cancel_queued_order(id, qmove_kind, 0, &edge) && kinds().empty());
    CHECK(!f.match->cancel_queued_order(id, qmove_kind, 0, nullptr));
    // A click a mod's click snap moved matches only an order whose whole
    // pixels lie from 8 before the click's to 7 after them.
    f.match->issue_or_cancel_order(id, qmove_kind, true, 0, &a, 0, 0);
    const auto far = point(159, 100);
    const auto near = point(142, 100);
    CHECK(!f.match->cancel_queued_order(id, qmove_kind, 0, &far, true));
    CHECK(!f.match->cancel_queued_order(id, qmove_kind, 0, &near, true));
    const auto inside = point(158, 93);
    CHECK(f.match->cancel_queued_order(id, qmove_kind, 0, &inside, true) && kinds().empty());
    f.match->issue_or_cancel_order(id, qmove_kind, true, 0, &a, 0, 0);
    const auto other_edge = point(143, 108);
    CHECK(f.match->cancel_queued_order(id, qmove_kind, 0, &other_edge, true) && kinds().empty());
}

// A short move ends once, then a tank whose type stands by is reported idle
// once. A tank with no default mission, a nanoframe waiting for its builder
// and a finished factory are never reported idle.
void idle_and_ended_orders_are_reported() {
    struct Report {
        bool idle{};
        uint16_t unit{};
        uint8_t kind{};
        bool done{};
    };

    Fixture f;
    std::vector<Report> reports;
    sim::match_runtime::EventHooks hooks;
    hooks.context = &reports;
    hooks.order_ended =
        [](void* context, const oa::World&, uint16_t unit, uint8_t kind, bool done) {
            static_cast<std::vector<Report>*>(context)->push_back({false, unit, kind, done});
        };
    hooks.unit_idle = [](void* context, const oa::World&, uint16_t unit) {
        static_cast<std::vector<Report>*>(context)->push_back({true, unit, 0, false});
    };
    CHECK(f.match->add_event_listener(hooks) != 0);

    std::array<uint8_t, 10> allies{};
    allies[0] = 1;
    f.match->configure_outcomes(0, allies, true);
    uint32_t clock = f.tick();
    const auto advance = [&] {
        f.match->simulation().tick = clock++;
        f.match->tick();
    };
    const auto count_of = [&](uint16_t unit, bool idle) {
        int count = 0;
        for (const auto& report : reports)
            if (report.unit == unit && report.idle == idle)
                ++count;
        return count;
    };

    f.match->state().unit_defs[tank_type].default_mission_type = sim::ground_orders::standby_kind;
    auto& mover = f.spawn(0, tank_type, 100, 100);
    const auto move_kind =
        f.match->issue_ground_move(mover.unit_index, point(108, 100), false).kind;
    bool arrived = false;
    for (uint32_t passed = 0; passed < 1200 && !arrived; ++passed) {
        advance();
        arrived = count_of(mover.unit_index, false) == 1 && count_of(mover.unit_index, true) == 1;
    }
    CHECK(arrived);
    CHECK(count_of(mover.unit_index, false) == 1);
    CHECK(count_of(mover.unit_index, true) == 1);
    int mover_reports = 0;
    bool ended_first = false;
    for (const auto& report : reports) {
        if (report.unit != mover.unit_index)
            continue;
        if (mover_reports == 0) {
            CHECK(!report.idle && report.kind == move_kind && report.done);
            ended_first = true;
        } else {
            CHECK(report.idle);
        }
        ++mover_reports;
    }
    CHECK(ended_first && mover_reports == 2);
    CHECK(
        f.head(mover.unit_index) &&
        f.head(mover.unit_index)->kind == sim::ground_orders::standby_kind
    );
    const auto after_arrival = reports.size();
    for (uint32_t passed = 0; passed < 60; ++passed)
        advance();
    CHECK(reports.size() == after_arrival);

    // An unfinished tank waiting on GetBuilt, and one with an empty queue,
    // are not idle. The factory has no movement object, so its default
    // mission is never queued.
    auto& waiting = f.spawn(0, tank_type, 40, 40, false);
    CHECK(waiting.record.build_remaining != 0.0F);
    (void)f.order(waiting.unit_index, get_built_kind);
    auto& bare = f.spawn(0, tank_type, 48, 48, false);
    CHECK(bare.record.build_remaining != 0.0F);
    f.match->state().unit_defs[factory_type].default_mission_type =
        sim::ground_orders::standby_kind;
    auto& factory = f.spawn(0, factory_type, 200, 200);
    CHECK(!f.match->ground_runtime(factory.unit_index));
    for (uint32_t passed = 0; passed < 60; ++passed)
        advance();
    CHECK(count_of(waiting.unit_index, true) == 0);
    CHECK(count_of(bare.unit_index, true) == 0);
    CHECK(count_of(factory.unit_index, true) == 0);
    CHECK(reports.size() == after_arrival);
    CHECK(f.head(waiting.unit_index) && f.head(waiting.unit_index)->kind == get_built_kind);

    f.match->state().unit_defs[tank_type].default_mission_type = 0;
    auto& plain = f.spawn(0, tank_type, 80, 80);
    const auto plain_kind = f.match->issue_ground_move(plain.unit_index, point(88, 80), false).kind;
    bool plain_arrived = false;
    for (uint32_t passed = 0; passed < 1200 && !plain_arrived; ++passed) {
        advance();
        plain_arrived = count_of(plain.unit_index, false) == 1;
    }
    CHECK(plain_arrived);
    CHECK(count_of(plain.unit_index, false) == 1);
    CHECK(count_of(plain.unit_index, true) == 0);
    for (const auto& report : reports)
        if (report.unit == plain.unit_index)
            CHECK(!report.idle && report.kind == plain_kind && report.done);
    const auto after_plain = reports.size();
    for (uint32_t passed = 0; passed < 60; ++passed)
        advance();
    CHECK(reports.size() == after_plain);
    CHECK(count_of(plain.unit_index, true) == 0);
    CHECK(count_of(waiting.unit_index, true) == 0);
    CHECK(count_of(bare.unit_index, true) == 0);
    CHECK(count_of(factory.unit_index, true) == 0);
    std::cout << "idle and ended orders are reported passed\n";
}

void built_factory_produces() {
    struct Carry {
        uint16_t unit{};
        uint16_t carrier{};
        bool loaded{};
    };

    std::vector<Carry> carries;
    Fixture f;
    sim::match_runtime::EventHooks hooks;
    hooks.context = &carries;
    hooks.unit_carried =
        [](void* context, const oa::World&, uint16_t unit, uint16_t carrier, bool loaded) {
            static_cast<std::vector<Carry>*>(context)->push_back({unit, carrier, loaded});
        };
    CHECK(f.match->add_event_listener(hooks) != 0);

    auto& builder = f.spawn(0, builder_type, 100, 100);
    std::array<uint8_t, 10> allies{};
    allies[0] = 1;
    f.match->configure_outcomes(0, allies, true);
    uint32_t clock = f.tick();
    const auto run_until = [&](uint32_t limit, auto done) {
        for (uint32_t i = 0; i < limit && !done(); ++i) {
            f.match->simulation().tick = clock++;
            f.match->tick();
        }
        return done();
    };
    const auto find = [&](uint16_t type) -> sim::unit_spawn::Slot* {
        for (auto& slot : f.match->world().slots)
            if (slot.unit && slot.record.type_index == type)
                return &slot;
        return nullptr;
    };
    (void)f.match->issue_mobile_build(builder.unit_index, factory_type, point(150, 101), false);
    CHECK(run_until(600, [&] {
        const auto* factory = find(factory_type);
        return factory && factory->record.build_remaining == 0.0F;
    }));
    auto& factory = *find(factory_type);
    (void)run_until(30, [&] { return !factory.unit->primary; });
    CHECK(!factory.unit->primary);
    CHECK(!f.match->ground_runtime(factory.unit_index));
    f.match->queue_factory_build(factory.unit_index, tank_type, 1);
    CHECK(
        f.head(factory.unit_index) &&
        f.head(factory.unit_index)->kind == sim::match_runtime::building_build_kind
    );
    CHECK(run_until(600, [&] {
        const auto* tank = find(tank_type);
        return tank && tank->record.build_remaining == 0.0F && !f.head(factory.unit_index);
    }));
    const auto* tank = find(tank_type);
    CHECK(tank != nullptr);
    std::vector<Carry> pad;
    for (const auto& carry : carries)
        if (carry.unit == tank->unit_index)
            pad.push_back(carry);
    CHECK(pad.size() == 2);
    CHECK(pad[0].loaded && pad[0].carrier == factory.unit_index);
    CHECK(!pad[1].loaded && pad[1].carrier == factory.unit_index);
    std::cout << "built factory produces passed\n";
}

// The factory's frame names the builder that placed it, and the tank names
// the factory, each created-by report following that unit's created report.
void new_units_name_their_builder() {
    struct Note {
        bool created_by{};
        uint16_t unit{};
        uint16_t type{};
        uint16_t builder{};
    };

    struct Log {
        std::vector<Note> notes;
    } log;

    Fixture f;
    sim::match_runtime::EventHooks hooks;
    hooks.context = &log;
    hooks.unit_created = [](void* context, const oa::World& world, uint16_t unit) {
        static_cast<Log*>(context)->notes.push_back({false, unit, world.units[unit].type_index, 0});
    };
    hooks.unit_created_by = [](void* context, const oa::World&, uint16_t unit, uint16_t builder) {
        static_cast<Log*>(context)->notes.push_back({true, unit, 0, builder});
    };
    CHECK(f.match->add_event_listener(hooks) != 0);

    auto& builder = f.spawn(0, builder_type, 100, 100);
    std::array<uint8_t, 10> allies{};
    allies[0] = 1;
    f.match->configure_outcomes(0, allies, true);
    uint32_t clock = f.tick();
    const auto run_until = [&](uint32_t limit, auto done) {
        for (uint32_t i = 0; i < limit && !done(); ++i) {
            f.match->simulation().tick = clock++;
            f.match->tick();
        }
        return done();
    };
    const auto find = [&](uint16_t type) -> sim::unit_spawn::Slot* {
        for (auto& slot : f.match->world().slots)
            if (slot.unit && slot.record.type_index == type)
                return &slot;
        return nullptr;
    };
    (void)f.match->issue_mobile_build(builder.unit_index, factory_type, point(150, 101), false);
    CHECK(run_until(600, [&] {
        const auto* factory = find(factory_type);
        return factory && factory->record.build_remaining == 0.0F;
    }));
    auto& factory = *find(factory_type);
    (void)run_until(30, [&] { return !factory.unit->primary; });
    CHECK(!factory.unit->primary);
    f.match->queue_factory_build(factory.unit_index, tank_type, 1);
    CHECK(run_until(600, [&] {
        const auto* tank = find(tank_type);
        return tank && tank->record.build_remaining == 0.0F && !f.head(factory.unit_index);
    }));

    CHECK(log.notes.size() == 6);
    int builders = 0;
    int factories = 0;
    int tanks = 0;
    for (size_t i = 0; i < log.notes.size(); i += 2) {
        const auto& created = log.notes[i];
        const auto& by = log.notes[i + 1];
        CHECK(!created.created_by && by.created_by && created.unit == by.unit);
        if (created.type == builder_type) {
            ++builders;
            CHECK(by.builder == 0 && by.unit == builder.unit_index);
        } else if (created.type == factory_type) {
            ++factories;
            CHECK(by.builder == builder.unit_index);
        } else if (created.type == tank_type) {
            ++tanks;
            CHECK(by.builder == factory.unit_index);
        } else {
            CHECK(false);
        }
    }
    CHECK(builders == 1 && factories == 1 && tanks == 1);
    std::cout << "new units name their builder passed\n";
}

// A move given to a factory waits there as QMove; the unit the factory then
// builds, once off the pad, has that move as its only order and the factory's
// standing orders (GetBuilt; the builder link only detaches it), and walks
// there.
void factory_move_hands_off() {
    Fixture f;
    std::array<uint8_t, 10> allies{};
    allies[0] = 1;
    f.match->configure_outcomes(0, allies, true);
    auto& factory = f.spawn(0, factory_type, 100, 100);
    const auto id = factory.unit_index;
    f.match->state().unit_defs[factory_type].abilities |= OA_UNIT_DEF_ABILITY_CAN_MOVE;
    // Return fire and roam, neither of them a new tank's own standing orders.
    constexpr uint32_t standing = OA_UNIT_FLAG_FIRE_ORDER_MASK | OA_UNIT_FLAG_MOVE_ORDER_MASK;
    const uint32_t return_fire_roam =
        (1u << OA_UNIT_FLAG_FIRE_ORDER_SHIFT) | (2u << OA_UNIT_FLAG_MOVE_ORDER_SHIFT);
    const auto& own = f.spawn(0, tank_type, 40, 40);
    for (const auto mask : {OA_UNIT_FLAG_FIRE_ORDER_MASK, OA_UNIT_FLAG_MOVE_ORDER_MASK})
        CHECK((own.record.flags & mask) != (return_fire_roam & mask));
    factory.record.flags = (factory.record.flags & ~standing) | return_fire_roam;
    const auto rally = point(200, 100);
    CHECK(f.match->issue_ground_move(id, rally, false).kind == qmove_kind);
    f.match->queue_factory_build(id, tank_type, 1);
    uint32_t clock = f.tick();
    sim::unit_spawn::Slot* tank = nullptr;
    for (uint32_t i = 0; i < 900 && !(tank && f.head(tank->unit_index) &&
                                      f.head(tank->unit_index)->kind != get_built_kind);
         ++i) {
        f.match->simulation().tick = clock++;
        f.match->tick();
        for (auto& slot : f.match->world().slots)
            if (slot.unit && slot.unit_index != own.unit_index &&
                slot.record.type_index == tank_type && slot.record.build_remaining == 0.0F)
                tank = &slot;
    }
    CHECK(tank && !tank->record.attach_parent);
    std::vector<sim::match_runtime::Match::QueuedCommandView> queue;
    f.match->visit_primary_queue(tank->unit_index, [&](const auto& view) {
        queue.push_back(view);
    });
    CHECK(queue.size() == 1 && queue[0].kind == move_ground_kind && queue[0].destination == rally);
    CHECK((tank->record.flags & standing) == return_fire_roam);
    // The factory keeps its QMove for the next unit.
    CHECK(f.head(id) && f.head(id)->kind == qmove_kind);
    const auto distance = [&] {
        const auto dx = static_cast<int64_t>(tank->record.position.x >> 16) - 200;
        const auto dz = static_cast<int64_t>(tank->record.position.z >> 16) - 100;
        return dx * dx + dz * dz;
    };
    const auto start = distance();
    for (uint32_t i = 0; i < 60; ++i) {
        f.match->simulation().tick = clock++;
        f.match->tick();
    }
    CHECK(distance() < start);
}

void repair_orders() {
    Fixture f;
    auto& builder = f.spawn(0, builder_type, 100, 100);
    const auto id = builder.unit_index;
    auto& patient = f.spawn(0, tank_type, 104, 100);
    patient.record.health = 40;
    builder.record.state_flags |= OA_UNIT_STATE_ACTIVE;

    // RepairUnit: phase 0 announces, phase 1 starts in build reach, phase 3
    // heals a step a tick and holds the cloak off.
    auto& repair = f.match->issue_repair(id, patient.unit_index, false);
    CHECK(repair.kind == sim::match_runtime::repair_unit_kind);
    CHECK(step(*f.match, builder, repair) == 1);
    CHECK(f.services.take() == std::vector<uint32_t>{5});
    CHECK(
        step(*f.match, builder, repair) == 1 &&
        (repair.flags & sim::match_runtime::MatchTickAccess::building_flag)
    );
    CHECK(step(*f.match, builder, repair) == 1);
    Fixture::fund(builder);
    CHECK(step(*f.match, builder, repair) == 2 && repair.wait_events == 9);
    CHECK(patient.record.health == 41 && builder.record.decloak_until_tick == f.tick() + 0x96);
    // A moving patient stops the work.
    patient.record.flags |= 4u;
    CHECK(
        step(*f.match, builder, repair) == 0 &&
        !(repair.flags & sim::match_runtime::MatchTickAccess::building_flag) &&
        repair.wake_tick == f.tick() + 0xf
    );
    patient.record.flags &= ~4u;
    patient.record.health = 100;
    repair.phase = 3;
    CHECK(step(*f.match, builder, repair) == 1);
    CHECK(step(*f.match, builder, repair) == 5);
    CHECK(f.services.take() == std::vector<uint32_t>{10});
    f.match->stop_orders(id);
    // Out of build reach it walks to the patient first.
    auto& far = f.spawn(0, tank_type, 200, 200);
    far.record.health = 40;
    auto& walk = f.match->issue_repair(id, far.unit_index, false);
    walk.phase = 1;
    CHECK(step(*f.match, builder, walk) == 2 && (walk.wait_events & 0xe9) == 0xe9);
    CHECK(walk.wake_tick >= f.tick() + 0x1e && walk.wake_tick < f.tick() + 0x3c);
    f.match->stop_orders(id);
    // An airborne patient ends it with category 7.
    far.record.flags = (far.record.flags & ~OA_UNIT_FLAG_OCCUPANCY_MASK) | 2u;
    CHECK(step(*f.match, builder, f.match->issue_repair(id, far.unit_index, false)) == 5);
    CHECK(f.services.take() == std::vector<uint32_t>{7});
    far.record.flags = (far.record.flags & ~OA_UNIT_FLAG_OCCUPANCY_MASK) | 1u;
    f.match->stop_orders(id);

    // RepairUnitNoMove repairs from where the unit stands.
    patient.record.health = 40;
    auto& stay = f.order(id, sim::match_runtime::repair_unit_no_move_kind, patient.unit_index);
    CHECK(step(*f.match, builder, stay) == 1);
    Fixture::fund(builder);
    CHECK(
        step(*f.match, builder, stay) == 2 && patient.record.health == 41 && stay.wait_events == 9
    );
    patient.record.health = 100;
    CHECK(step(*f.match, builder, stay) == 1);
    CHECK(step(*f.match, builder, stay) == 5);
    CHECK(f.services.take() == std::vector<uint32_t>{10});
    f.match->stop_orders(id);
    auto& tank = f.spawn(0, tank_type, 60, 60);
    CHECK(
        step(
            *f.match,
            tank,
            f.order(
                tank.unit_index, sim::match_runtime::repair_unit_no_move_kind, patient.unit_index
            )
        ) == 7
    );
    f.match->stop_orders(tank.unit_index);

    // SelfRepair: the unit stands on the builder in its target while it
    // repairs it.
    tank.record.health = 40;
    tank.record.state_flags |= OA_UNIT_STATE_ACTIVE;
    auto& mend = f.order(tank.unit_index, sim::match_runtime::self_repair_kind, id);
    CHECK(step(*f.match, tank, mend) == 1);
    Fixture::fund(builder);
    CHECK(step(*f.match, tank, mend) == 2 && tank.record.health == 41 && mend.wait_events == 9);
    CHECK(tank.record.decloak_until_tick == f.tick() + 0x96);
    tank.record.health = 100;
    CHECK(step(*f.match, tank, mend) == 1);
    CHECK(step(*f.match, tank, mend) == 5);
    CHECK(f.services.take() == std::vector<uint32_t>{10});
    f.match->stop_orders(tank.unit_index);
    CHECK(
        step(*f.match, tank, f.order(tank.unit_index, sim::match_runtime::self_repair_kind)) == 8
    );
    CHECK(f.services.take() == std::vector<uint32_t>{7});
    f.match->stop_orders(tank.unit_index);
}

void repairs_charge_the_repairer() {
    Fixture f;
    // Players 0 and 1 are allies.
    std::array<uint8_t, 10> allies{};
    allies[0] = allies[1] = 1;
    f.match->configure_player_alliances(0, allies);
    f.match->configure_player_alliances(1, allies);
    auto& builder = f.spawn(0, builder_type, 100, 100);
    const auto id = builder.unit_index;
    auto& own = f.spawn(0, tank_type, 104, 100);
    auto& ally = f.spawn(1, tank_type, 100, 104);
    builder.record.state_flags |= OA_UNIT_STATE_ACTIVE;
    own.record.health = ally.record.health = 40;
    Fixture::fund(builder);
    Fixture::fund(own);
    Fixture::fund(ally);

    // As in 3.1c the builder pays for its own unit's repair: under 3.1c's
    // repair rate a step requests 1 energy in the builder's economy block
    // and none in the patient's.
    auto& mend_own = f.order(id, sim::match_runtime::repair_unit_no_move_kind, own.unit_index);
    CHECK(step(*f.match, builder, mend_own) == 1);
    CHECK(step(*f.match, builder, mend_own) == 2 && own.record.health == 41);
    CHECK(builder.record.economy.energy.requested == 1.0F);
    CHECK(builder.record.economy.energy.accepted == 1.0F);
    CHECK(own.record.economy.energy.requested == 0.0F);
    f.match->stop_orders(id);

    // And for an ally's unit: the builder's owner pays, not the ally.
    auto& mend = f.match->issue_repair(id, ally.unit_index, false);
    CHECK(step(*f.match, builder, mend) == 1);
    CHECK(step(*f.match, builder, mend) == 1);
    CHECK(step(*f.match, builder, mend) == 1);
    CHECK(step(*f.match, builder, mend) == 2 && ally.record.health == 41);
    CHECK(builder.record.economy.energy.requested == 2.0F);
    CHECK(builder.record.economy.energy.accepted == 2.0F);
    CHECK(ally.record.economy.energy.requested == 0.0F);
    f.match->update_player_economy(0);
    f.match->update_player_economy(1);
    const auto& players = f.match->state().game.players;
    CHECK(players[0].energy_requested == 2.0F && players[0].energy == 998.0F);
    CHECK(players[1].energy_requested == 0.0F && players[1].energy == 1000.0F);
    // The bottom bar draws each unit's own block: the builder shows the two
    // energy, the ally's unit none.
    CHECK(builder.record.economy.energy.last_requested == 2.0F);
    CHECK(ally.record.economy.energy.last_requested == 0.0F);

    // Short of energy: with the store empty a step still goes ahead, but the
    // next settlement cannot cover it and leaves the builder's block short
    // (its gate above 0). The builder's gate then refuses the following
    // step: it is requested, not afforded, and nothing is healed.
    f.match->state().game.players[0].energy = 0.0F;
    CHECK(step(*f.match, builder, mend) == 2 && ally.record.health == 42);
    f.match->update_player_economy(0);
    CHECK(builder.record.economy.energy.gate > 0.0F);
    CHECK(step(*f.match, builder, mend) == 2 && ally.record.health == 42);
    CHECK(builder.record.economy.energy.requested == 1.0F);
    CHECK(builder.record.economy.energy.accepted == 0.0F);
    CHECK(ally.record.economy.energy.requested == 0.0F);
    // With energy again the builder's gate opens; a shortfall in the
    // patient's own block does not stop the repair, since it does not pay.
    f.match->state().game.players[0].energy = 1000.0F;
    f.match->update_player_economy(0);
    CHECK(builder.record.economy.energy.gate == 0.0F);
    ally.record.economy.energy.gate = 1.0F;
    CHECK(step(*f.match, builder, mend) == 2 && ally.record.health == 43);
    CHECK(builder.record.economy.energy.accepted == 1.0F);
    CHECK(ally.record.economy.energy.requested == 0.0F);
    f.match->stop_orders(id);
}

void reclaim_and_capture() {
    Fixture f;
    auto& builder = f.spawn(0, builder_type, 100, 100);
    const auto id = builder.unit_index;
    auto& enemy = f.spawn(1, tank_type, 104, 100);

    // ReclaimUnit: phase 1 heads for the unit and takes the bite for 15
    // ticks: (5 / 5) * 60 * 100 * 15 / (50 * 300) = 6.
    auto& reclaim = f.match->issue_reclaim(id, enemy.unit_index, false);
    CHECK(step(*f.match, builder, reclaim) == 1);
    CHECK(step(*f.match, builder, reclaim) == 2 && reclaim.wake_tick == f.tick() + 0xf);
    CHECK((reclaim.wait_events & 0x100e9) == 0x100e9);
    reclaim.phase = 2;
    CHECK(
        step(*f.match, builder, reclaim) == 1 &&
        (reclaim.flags & sim::match_runtime::MatchTickAccess::building_flag)
    );
    CHECK(step(*f.match, builder, reclaim) == 1);
    CHECK(step(*f.match, builder, reclaim) == 1);
    CHECK((f.services.take() == std::vector<uint32_t>{5, 0x0b}));
    // Phase 5 sprays two ticks at a time; the ninth step, with 16 ticks
    // sprayed, bites.
    for (int spray = 0; spray < 9; ++spray)
        CHECK(step(*f.match, builder, reclaim) == 2 && reclaim.wake_tick == f.tick() + 2);
    CHECK(enemy.record.health == 94);
    CHECK(builder.record.decloak_until_tick == f.tick() + 900);
    // The bite is an attack: its owner hears category 2.
    CHECK(f.services.take() == std::vector<uint32_t>{2});
    // Out of reach it stops building and starts over.
    const auto near_x = builder.record.position.x;
    builder.record.position.x = 300 << 16;
    CHECK(step(*f.match, builder, reclaim) == 0 && reclaim.wake_tick == f.tick() + 0xf);
    builder.record.position.x = near_x;
    f.match->stop_orders(id);

    // Capture: phase 0 works out the spraying time from the target's costs,
    // health and veterancy: min(200 * 30 * 0.0005 + 50 * 30 / 140 + 150,
    // 1800) = 163; (100 + 100) * 163 / 200 = 163; (0 + 10) * 163 * 10 / 100 = 163.
    auto& prize = f.spawn(1, tank_type, 104, 104);
    auto& capture = f.match->issue_capture(id, prize.unit_index, false);
    CHECK(step(*f.match, builder, capture) == 1 && capture.wait_events == 0x100e8);
    CHECK(step(*f.match, builder, capture) == 1);
    CHECK(step(*f.match, builder, capture) == 1);
    CHECK(step(*f.match, builder, capture) == 1);
    CHECK((f.services.take() == std::vector<uint32_t>{5, 0x0b}));
    uint32_t sprays = 0;
    while (step(*f.match, builder, capture) == 2 && sprays < 200)
        ++sprays;
    CHECK(sprays == 82 && capture.phase == 5);
    CHECK(step(*f.match, builder, capture) == 5);
    // The transfer destroys the captured unit (its owner hears category 2), then
    // the capturer speaks category 0x10.
    CHECK((f.services.take() == std::vector<uint32_t>{2, 0x10}));
    bool captured = false;
    for (auto& slot : f.match->world().slots)
        if (slot.unit && slot.record.type_index == tank_type && slot.record.owner_index == 0)
            captured = true;
    CHECK(captured);
    f.match->stop_orders(id);

    // A unit that can capture cannot be captured or reclaimed.
    auto& rival = f.spawn(1, builder_type, 108, 100);
    CHECK(step(*f.match, builder, f.match->issue_capture(id, rival.unit_index, false)) == 8);
    CHECK(f.services.take() == std::vector<uint32_t>{7});
    f.match->stop_orders(id);
    CHECK(step(*f.match, builder, f.match->issue_reclaim(id, rival.unit_index, false)) == 8);
    CHECK((f.services.take() == std::vector<uint32_t>{7, 7}));
    f.match->stop_orders(id);
}

// Capture's spraying time scales with (health + maximum health) / (2 * maximum
// health). A tank with 100 health of 100 takes 82 sprays, as above; one whose
// type has no maximum health (MaxDamage 0), made with no health, takes the
// quotient as 0 and changes sides at the first capture step, with the usual
// speech and no fault.
void capture_without_maximum_health_is_at_once() {
    for (const uint32_t maximum : {100u, 0u}) {
        Fixture f(maximum);
        auto& builder = f.spawn(0, builder_type, 100, 100);
        const auto id = builder.unit_index;
        auto& prize = f.spawn(1, tank_type, 104, 104);
        CHECK(prize.record.health == static_cast<int16_t>(maximum));
        auto& capture = f.match->issue_capture(id, prize.unit_index, false);
        for (int phase = 0; phase < 4; ++phase)
            CHECK(step(*f.match, builder, capture) == 1);
        CHECK((f.services.take() == std::vector<uint32_t>{5, 0x0b}));
        uint32_t sprays = 0;
        while (step(*f.match, builder, capture) == 2 && sprays < 200)
            ++sprays;
        CHECK(sprays == (maximum != 0 ? 82u : 0u) && capture.phase == 5);
        CHECK(step(*f.match, builder, capture) == 5);
        CHECK((f.services.take() == std::vector<uint32_t>{2, 0x10}));
        bool captured = false;
        for (auto& slot : f.match->world().slots)
            if (slot.unit && slot.record.type_index == tank_type && slot.record.owner_index == 0)
                captured = true;
        CHECK(captured);
        CHECK(f.match->fault() == nullptr);
    }
}
} // namespace

// A factory that dies with a unit on its build pad takes the unit with it.
// The dying factory's orders go first, so a half-built frame its
// BuildingBuild still works on is cancelled by that order (kind 9), which
// lets it go. A unit hanging from the pad with no order working on it, as
// the factory attaches what it builds, dies as cargo (kind 6), the way cargo
// dies with its transport.
void factory_death_kills_the_unit_on_its_pad() {
    constexpr auto weapon = static_cast<uint8_t>(sim::match_runtime::DeathKind::weapon);
    constexpr auto cargo = static_cast<uint8_t>(sim::match_runtime::DeathKind::cargo);
    constexpr auto cancelled = static_cast<uint8_t>(sim::match_runtime::DeathKind::cancelled);
    for (const bool building : {true, false}) {
        Fixture f;
        std::array<uint8_t, 10> allies{};
        allies[0] = 1;
        f.match->configure_outcomes(0, allies, false);
        auto& factory = f.spawn(0, factory_type, 100, 100);
        const auto id = factory.unit_index;
        sim::unit_spawn::Slot* frame = nullptr;
        if (building) {
            auto& order = f.match->issue_building_build(id, tank_type, 1, false);
            CHECK(step(*f.match, factory, order) == 1);
            CHECK(step(*f.match, factory, order) == 2);
            f.match->tick_scripts(1);
            CHECK(step(*f.match, factory, order) == 1);
            CHECK(step(*f.match, factory, order) == 1);
            for (auto& slot : f.match->world().slots)
                if (slot.unit && slot.record.type_index == tank_type)
                    frame = &slot;
            CHECK(frame && frame->record.build_remaining != 0.0F);
        } else {
            frame = &f.spawn(0, tank_type, 100, 100);
            f.match->set_carry_link(frame->unit_index, id, -1, 1);
        }
        CHECK(sim::match_runtime::link_parent(frame->record) == id);
        f.match->apply_damage_event(factory, nullptr, 30000, weapon, 0);
        uint8_t dealt = 0;
        for (int tick = 0; tick < 120 && frame->record.type_index != 0; ++tick) {
            ++f.match->state().game.tick;
            f.match->tick();
            if (dealt == 0)
                dealt = frame->record.damage_kind;
        }
        CHECK(frame->record.type_index == 0 && dealt == (building ? cancelled : cargo));
    }
}

// A frame nobody builds decays until nothing of it is left, and the step that
// takes the last of it destroys it as a cancelled build: its metal comes back,
// it leaves no wreck and counts as nobody's kill.
void abandoned_frame_decays_away() {
    Fixture f;
    constexpr auto cancelled = static_cast<uint8_t>(sim::match_runtime::DeathKind::cancelled);
    auto& frame = f.spawn(0, tank_type, 60, 60, false);
    // One decay step takes 11 * build time / energy cost = 0.055 of the
    // build, so two leave nothing of a frame 0.1 built.
    frame.record.build_remaining = 0.9F;
    frame.record.health = 10;
    const auto metal = frame.record.economy.metal.produced;
    auto& waiting = f.order(frame.unit_index, get_built_kind);
    CHECK(step(*f.match, frame, waiting) == 1);
    CHECK(step(*f.match, frame, waiting) == 1);
    CHECK(step(*f.match, frame, waiting, 1) == 2);
    CHECK(frame.record.build_remaining > 0.9F && frame.record.build_remaining < 1.0F);
    CHECK(!(frame.record.flags & OA_UNIT_FLAG_DEATH_PENDING));
    CHECK(step(*f.match, frame, waiting, 1) == 2);
    CHECK(frame.record.build_remaining == 1.0F && frame.record.health < 1);
    CHECK(frame.record.flags & OA_UNIT_FLAG_DEATH_PENDING);
    CHECK(frame.record.damage_kind == cancelled);
    // Each step gives back the metal of the share it takes: 0.1 of 50 in all.
    const auto refunded = frame.record.economy.metal.produced - metal;
    CHECK(refunded > 4.99F && refunded < 5.01F);

    // The sweep then tears the frame down.
    struct Deaths {
        std::vector<sim::match_runtime::KillOutcome> outcomes;
    } deaths;

    sim::match_runtime::EventHooks hooks{};
    hooks.context = &deaths;
    hooks.unit_died = [](void* context,
                         const oa::World&,
                         uint16_t,
                         const sim::match_runtime::KillOutcome& outcome,
                         bool) { static_cast<Deaths*>(context)->outcomes.push_back(outcome); };
    f.match->event_hooks = hooks;
    std::array<uint8_t, 10> allies{};
    allies[0] = 1;
    f.match->configure_outcomes(0, allies, false);
    const auto losses = f.match->state().game.players[0].losses;
    ++f.match->state().game.tick;
    f.match->tick();
    CHECK(frame.record.type_index == 0 && !(frame.record.flags & OA_UNIT_FLAG_LIVE));
    CHECK(deaths.outcomes.size() == 1);
    CHECK(static_cast<uint8_t>(deaths.outcomes[0].kind) == cancelled);
    CHECK(deaths.outcomes[0].killed_percent == 0 && deaths.outcomes[0].wreck_level == 0);
    CHECK(f.match->state().game.players[0].losses == losses);
}

// An unfinished building waits 300 ticks, then 30, before it loses build
// progress, and then loses a step every 11 ticks. One step of this fixture is
// 11 / 200 of the build. A builder ordered back onto it, still waiting to
// turn into its build stance, does not stop the loss. The tick that applies
// build progress does: the loss stays off for 30 ticks after the last such
// tick, then starts again.
void unfinished_building_waits_and_a_builder_stops_the_decay() {
    constexpr uint32_t opening_wait = 300;
    constexpr uint32_t progress_grace = 30;
    constexpr uint32_t decay_period = 11;
    constexpr int build_ticks = 15;
    constexpr float half = 0.5F;

    Fixture f;
    std::array<uint8_t, 10> allies{};
    allies[0] = 1;
    f.match->configure_outcomes(0, allies, false);

    // The builder is the earlier slot, so its work lands before the frame's
    // own order runs on the same tick.
    auto& builder = f.spawn(0, builder_type, 40, 40);
    auto& frame = f.spawn(0, factory_type, 80, 80, false);
    CHECK(builder.unit_index < frame.unit_index);
    frame.record.build_remaining = half;
    frame.record.health = 500;
    auto& waiting = f.order(frame.unit_index, get_built_kind);

    const auto advance = [&] {
        ++f.match->state().game.tick;
        f.match->tick();
    };
    const auto rose_one_step = [](float before, float after) {
        const auto rose = after - before;
        return rose > 0.054F && rose < 0.056F;
    };

    advance();
    CHECK(waiting.phase == 1);
    CHECK(waiting.wake_tick == f.tick() + opening_wait);
    CHECK(frame.record.build_remaining == half);
    const auto opening_due = waiting.wake_tick;
    const auto first_decay = opening_due + progress_grace;
    while (f.tick() < first_decay) {
        advance();
        if (f.tick() == opening_due) {
            CHECK(waiting.phase == 2);
            CHECK(waiting.wake_tick == f.tick() + progress_grace);
        }
        if (f.tick() < first_decay)
            CHECK(frame.record.build_remaining == half);
    }
    CHECK(f.tick() == first_decay);
    CHECK(rose_one_step(half, frame.record.build_remaining));
    CHECK(waiting.phase == 2);
    CHECK(waiting.wake_tick == f.tick() + decay_period);
    CHECK(!(frame.record.flags & OA_UNIT_FLAG_DEATH_PENDING));

    // Ordered, and still turning: the loss keeps its 11-tick pace.
    auto& help = f.match->issue_help_build(builder.unit_index, frame.unit_index, false);
    help.phase = 2;
    help.wait_events = 0;
    help.wake_tick = 0xffffffffu;
    builder.record.build_flags = static_cast<uint8_t>(
        builder.record.build_flags & static_cast<uint8_t>(~OA_UNIT_BUILD_IN_BUILD_STANCE)
    );
    const auto while_turning = frame.record.build_remaining;
    for (uint32_t waited = 1; waited < decay_period; ++waited) {
        advance();
        CHECK(frame.record.build_remaining == while_turning);
        CHECK(help.phase == 2);
    }
    advance();
    CHECK(rose_one_step(while_turning, frame.record.build_remaining));
    CHECK(waiting.wake_tick == f.tick() + decay_period);

    // The stance is reached, so the next ticks apply build progress.
    builder.record.build_flags =
        static_cast<uint8_t>(builder.record.build_flags | OA_UNIT_BUILD_IN_BUILD_STANCE);
    help.wait_events = 0;
    help.wake_tick = 0xffffffffu;
    float previous = frame.record.build_remaining;
    for (int built = 0; built < build_ticks; ++built) {
        advance();
        CHECK(help.phase == 3);
        CHECK(frame.record.build_remaining < previous);
        CHECK(waiting.wake_tick == f.tick() + progress_grace);
        CHECK(waiting.phase == 2);
        previous = frame.record.build_remaining;
    }

    f.match->stop_orders(builder.unit_index);
    const auto held = frame.record.build_remaining;
    const auto resumes = f.tick() + progress_grace;
    while (f.tick() < resumes) {
        advance();
        if (f.tick() < resumes)
            CHECK(frame.record.build_remaining == held);
    }
    CHECK(f.tick() == resumes);
    CHECK(rose_one_step(held, frame.record.build_remaining));
    CHECK(waiting.wake_tick == f.tick() + decay_period);
}

int main() {
    try {
        stop_and_state_orders();
        self_destruct_counts_down();
        attack_orders();
        guard_and_teleport();
        construction_orders();
        queued_build_can_be_moved();
        factory_orders();
        abandoned_frame_decays_away();
        unfinished_building_waits_and_a_builder_stops_the_decay();
        factory_queue_counts();
        queued_order_cancels_its_match();
        built_factory_produces();
        idle_and_ended_orders_are_reported();
        new_units_name_their_builder();
        factory_move_hands_off();
        repair_orders();
        repairs_charge_the_repairer();
        reclaim_and_capture();
        capture_without_maximum_health_is_at_once();
        factory_death_kills_the_unit_on_its_pad();
    } catch (const std::exception& error) {
        std::cerr << "orders: " << error.what() << '\n';
        return 1;
    }
    std::cout << "orders passed\n";
    return 0;
}
