// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The events a multiplayer match shares with the other players' machines and
// the entries that apply theirs, through recording MultiplayerHooks: the
// shots an interceptor's blast sets off (shot_intercepted), carry links
// (carry_link_changed, Match::apply_carry_link), finished units
// (unit_finished, Match::finish_unit), the StartBuilding script, a
// resurrected wreck (feature_changed), units handed to another player
// (unit_transferred, Match::transfer_unit) and a game ended from elsewhere
// (Match::end_local_game). With an entry null nothing is shared and the event
// still plays out here.
#include "match_tick_access.hpp"
#include "combat_fixture.hpp"
#include "oa/data/unit_definitions.hpp"
#include "oa/sim/weapon_execution/interceptor.hpp"
#include "oa/sim/weapon_execution/projectile_pool.hpp"
#include "oa/sim/weapon_execution/weapon_launch.hpp"
#include "oa/base/text.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include "oa/test/match_services.hpp"

namespace {

using namespace oa;
using sim::feature_runtime::FeatureChange;
using sim::match_runtime::DeathKind;
using sim::match_runtime::link_first_child;
using sim::match_runtime::link_parent;

// One call of a recorded hook.
struct Recorded {
    std::string kind;
    uint32_t first{};  // unit, change or shot weapon reference
    uint32_t second{}; // carrier, builder, new owner, cell column or interceptor weapon reference
    int32_t third{};   // piece, script function, death kind or cell row
    int32_t fourth{};  // mode, argument count or resurrecting unit
    std::array<uint32_t, 4> locals{};
    // carry: the unit already hung on the named carrier; intercepted: the
    // shot was already retired; transferred: the unit was still live and
    // owned by its old owner.
    bool state_at_call{};
};

// Installs every multiplayer entry, recording the ones a test reads.
struct Recorder {
    sim::match_runtime::Match* match{};
    std::vector<Recorded> calls;

    static Recorder& of(void* context) { return *static_cast<Recorder*>(context); }

    void install(sim::match_runtime::Match& target) {
        match = &target;
        auto& hooks = target.multiplayer;
        hooks = {};
        hooks.context = this;
        hooks.local_player_ticked = [](void*, oa::Player&) {};
        hooks.unit_created = [](void* context, uint16_t unit) {
            of(context).calls.push_back({"created", unit});
        };
        hooks.unit_flags_changed = [](void*, uint16_t, uint8_t) {};
        hooks.player_status_changed = [](void*) {};
        hooks.health_route = [](void*, uint16_t source) {
            return static_cast<sim::unit_health::RouteIdentity>(source);
        };
        hooks.health_shared =
            [](void*, sim::unit_health::RouteIdentity, const sim::unit_health::HealthEvent&) {};
        hooks.script_started = [](void* context,
                                  uint16_t unit,
                                  int16_t function,
                                  uint8_t count,
                                  const std::array<uint32_t, 4>& locals) {
            of(context).calls.push_back({"script", unit, 0, function, count, locals});
        };
        hooks.shot_fired = [](void*, const sim::match_runtime::ShotEvent&) {};
        hooks.unit_killed = [](void* context,
                               uint16_t unit,
                               const sim::match_runtime::KillOutcome& outcome) {
            of(context).calls.push_back({"killed", unit, 0, static_cast<int32_t>(outcome.kind)});
        };
        hooks.feature_hit_elsewhere = [](void*, uint8_t, int32_t, int32_t) { return false; };
        hooks.feature_changed =
            [](void* context, FeatureChange change, int32_t x, int32_t z, uint16_t unit) {
                of(context).calls.push_back(
                    {"feature", static_cast<uint32_t>(change), static_cast<uint32_t>(x), z, unit}
                );
            };
        hooks.shot_intercepted =
            [](void* context, const oa::Projectile& shot, const oa::Projectile& interceptor) {
                of(context).calls.push_back(
                    {"intercepted",
                     shot.def,
                     interceptor.def,
                     0,
                     0,
                     {},
                     (shot.flags & OA_PROJECTILE_FLAG_RETIRED) != 0}
                );
            };
        hooks.carry_link_changed =
            [](void* context, uint16_t unit, uint16_t carrier, int8_t piece, uint8_t mode) {
                auto& recorder = of(context);
                const auto& record = recorder.match->state().units[unit];
                recorder.calls.push_back(
                    {"carry", unit, carrier, piece, mode, {}, link_parent(record) == carrier}
                );
            };
        hooks.unit_finished = [](void* context, uint16_t unit, uint16_t builder) {
            of(context).calls.push_back({"finished", unit, builder});
        };
        hooks.unit_transferred = [](void* context, uint16_t unit, uint8_t new_owner) {
            auto& recorder = of(context);
            const auto& record = recorder.match->state().units[unit];
            const bool live = (record.flags & OA_UNIT_FLAG_LIVE) != 0 &&
                              (record.flags & OA_UNIT_FLAG_DEATH_PENDING) == 0;
            recorder.calls.push_back(
                {"transferred", unit, new_owner, record.owner_index, 0, {}, live}
            );
        };
    }

    std::vector<Recorded> take(std::string_view kind) {
        std::vector<Recorded> found;
        for (const auto& call : calls)
            if (call.kind == kind)
                found.push_back(call);
        return found;
    }

    std::vector<std::string> kinds() const {
        std::vector<std::string> found;
        for (const auto& call : calls)
            found.push_back(call.kind);
        return found;
    }
};

// Sets or clears the run flag (Game.session_flags bit 0) of a live
// multiplayer game.
void set_run_flag(sim::match_runtime::Match& match, bool on) {
    constexpr uint8_t run_flag = 1;
    auto& flags = match.state().game.session_flags;
    flags = static_cast<uint8_t>(on ? flags | run_flag : flags & ~run_flag);
}

// --- Shots an interceptor sets off ---------------------------------------

constexpr uint8_t interceptor_weapon = 2;
constexpr uint8_t missile_weapon = 3;

// An anti-missile rocket whose 96-wide blast sets other shots off, and the
// missile it stops.
constexpr std::string_view interceptor_tdf = R"([TESTAMD]
{
ID=2; lineofsight=1; interceptor=1; range=1000; reloadtime=1; weaponvelocity=100;
areaofeffect=96;
[DAMAGE] { default=10; }
}
[TESTMISSILE]
{
ID=3; lineofsight=1; range=1000; reloadtime=1; weaponvelocity=100; areaofeffect=32;
[DAMAGE] { default=10; }
}
)";

void arm_interceptors(combat_fixture::Fixture& f) {
    CHECK(sim::combat_state::install_weapon_text(f.weapons, interceptor_tdf) == 2);
    std::copy(
        f.weapons.records().begin(),
        f.weapons.records().end(),
        std::begin(f.match->state().game.weapon_defs)
    );
}

constexpr int32_t fx(int32_t whole) {
    return static_cast<int32_t>(static_cast<uint32_t>(whole) << 16);
}

// A live shot in the pool at a point, owned by a player, as a weapon
// constructor leaves it.
oa::Projectile&
launch(combat_fixture::Fixture& f, uint8_t weapon, int32_t x, int32_t z, uint8_t owner) {
    auto& world = f.match->state();
    auto* shot = sim::weapon_execution::allocate_projectile(world);
    CHECK(shot != nullptr);
    const FixedVec3 at{fx(x), fx(60), fx(z)};
    sim::weapon_execution::init_projectile_record(
        world, *shot, oa_ref_from_index(weapon), at, &at, world.game.tick, nullptr, 0
    );
    shot->owner_index = owner;
    shot->lifetime_tick = world.game.tick + 200;
    return *shot;
}

void clear_shots(combat_fixture::Fixture& f) {
    auto& world = f.match->state();
    for (int32_t i = 0; i < world.game.projectile_count; ++i)
        sim::weapon_execution::retire_projectile(world, world.projectiles[i]);
    sim::weapon_execution::compact_projectiles(world);
}

void interceptions_are_shared() {
    combat_fixture::Fixture f;
    arm_interceptors(f);
    Recorder recorder;
    recorder.install(*f.match);

    // Two missiles inside the interceptor's blast and one outside it: each
    // one inside is set off, then shared with the interceptor, in pool
    // order; the one outside flies on.
    auto& interceptor = launch(f, interceptor_weapon, 128, 128, 0);
    auto& near = launch(f, missile_weapon, 150, 128, 1);
    auto& far = launch(f, missile_weapon, 20, 20, 1);
    auto& beside = launch(f, missile_weapon, 128, 170, 1);
    f.match->detonate(interceptor, nullptr);
    const auto shared = recorder.take("intercepted");
    CHECK(shared.size() == 2);
    for (const auto& call : shared)
        CHECK(
            call.first == oa_ref_from_index(missile_weapon) &&
            call.second == oa_ref_from_index(interceptor_weapon) && call.state_at_call
        );
    CHECK((near.flags & OA_PROJECTILE_FLAG_RETIRED) && (beside.flags & OA_PROJECTILE_FLAG_RETIRED));
    CHECK(!(far.flags & OA_PROJECTILE_FLAG_RETIRED));
    clear_shots(f);

    // A mirrored player's interceptor damages nothing here and sets nothing
    // off: its own machine shares what its blast did.
    recorder.calls.clear();
    f.match->simulation().players[0].status = OA_PLAYER_STATUS_MIRRORED;
    auto& mirrored = launch(f, interceptor_weapon, 128, 128, 0);
    auto& spared = launch(f, missile_weapon, 150, 128, 1);
    f.match->detonate(mirrored, nullptr);
    CHECK(recorder.take("intercepted").empty());
    CHECK(!(spared.flags & OA_PROJECTILE_FLAG_RETIRED));
    f.match->simulation().players[0].status = OA_PLAYER_STATUS_LOCAL;
    clear_shots(f);

    // With the entry null the blast still sets the missile off.
    f.match->multiplayer.shot_intercepted = nullptr;
    auto& quiet = launch(f, interceptor_weapon, 128, 128, 0);
    auto& stopped = launch(f, missile_weapon, 150, 128, 1);
    f.match->detonate(quiet, nullptr);
    CHECK(stopped.flags & OA_PROJECTILE_FLAG_RETIRED);
    clear_shots(f);
    std::cout << "interceptions are shared passed\n";
}

// --- Carry links -------------------------------------------------------------

void carry_links_are_shared() {
    combat_fixture::Fixture f;
    Recorder recorder;
    recorder.install(*f.match);
    auto& transport = f.spawn(0, 40, 40);
    auto& cargo = f.spawn(0, 72, 40);
    const auto transport_id = transport.unit_index;
    const auto cargo_id = cargo.unit_index;

    // AttachUnit: shared once, before the link holds.
    f.match->script_attach_unit(transport_id, cargo_id, 0, 2);
    auto calls = recorder.take("carry");
    CHECK(calls.size() == 1);
    CHECK(calls[0].first == cargo_id && calls[0].second == transport_id);
    CHECK(calls[0].third == 0 && calls[0].fourth == 2 && !calls[0].state_at_call);
    CHECK(link_parent(cargo.record) == transport_id);

    // A refused link shares nothing: the carrier is carried, or the child is
    // a building.
    recorder.calls.clear();
    auto& other = f.spawn(0, 104, 40);
    f.match->set_carry_link(other.unit_index, cargo_id, 0, 2);
    other.record.flags |= OA_UNIT_FLAG_BUILDING;
    f.match->set_carry_link(other.unit_index, transport_id, 0, 2);
    other.record.flags &= ~OA_UNIT_FLAG_BUILDING;
    CHECK(recorder.take("carry").empty());
    CHECK(link_parent(other.record) == 0);

    // DropUnit: the unit is set down, shared before it lets go.
    f.match->script_drop_unit(transport_id, cargo_id);
    calls = recorder.take("carry");
    CHECK(calls.size() == 1);
    CHECK(calls[0].first == cargo_id && calls[0].second == 0);
    CHECK(calls[0].third == -1 && calls[0].fourth == 1);
    CHECK(link_parent(cargo.record) == 0);

    // A link another player's machine shared applies without being shared again.
    recorder.calls.clear();
    f.match->apply_carry_link(cargo_id, transport_id, 0, 2);
    CHECK(
        link_parent(cargo.record) == transport_id && link_first_child(transport.record) == cargo_id
    );
    f.match->apply_carry_link(cargo_id, 0, -1, 1);
    CHECK(link_parent(cargo.record) == 0);
    CHECK(recorder.take("carry").empty());

    // A dying carrier sets its cargo down (and kills it with it): the cargo's
    // link is shared, on whichever machine the carrier dies.
    f.match->set_carry_link(cargo_id, transport_id, 0, 2);
    recorder.calls.clear();
    f.match->kill_unit(transport_id, static_cast<uint8_t>(DeathKind::weapon));
    calls = recorder.take("carry");
    CHECK(calls.size() == 1 && calls[0].first == cargo_id && calls[0].second == 0);
    CHECK(link_parent(cargo.record) == 0);

    auto& mirrored_transport = f.spawn(1, 40, 120);
    auto& mirrored_cargo = f.spawn(1, 72, 120);
    f.match->apply_carry_link(mirrored_cargo.unit_index, mirrored_transport.unit_index, 0, 2);
    f.match->simulation().players[1].status = OA_PLAYER_STATUS_MIRRORED;
    recorder.calls.clear();
    constexpr uint8_t no_player = 10;
    f.match->apply_kill(mirrored_transport.unit_index, {DeathKind::weapon, 0, 0}, 0, no_player);
    calls = recorder.take("carry");
    CHECK(calls.size() == 1 && calls[0].first == mirrored_cargo.unit_index && calls[0].second == 0);

    // With the entry null a link still applies.
    f.match->simulation().players[1].status = OA_PLAYER_STATUS_LOCAL;
    f.match->multiplayer.carry_link_changed = nullptr;
    auto& carrier = f.spawn(0, 200, 200);
    auto& load = f.spawn(0, 232, 200);
    f.match->set_carry_link(load.unit_index, carrier.unit_index, 0, 2);
    CHECK(link_parent(load.record) == carrier.unit_index);
    std::cout << "carry links are shared passed\n";
}

// --- A match with builders, a factory and wrecks ------------------------------

constexpr int32_t map_cells = 32;
constexpr uint16_t builder_type = 1; // mobile builder; StartBuilding stores its argument
constexpr uint16_t factory_type = 2; // 2x2 structure that builds tanks
constexpr uint16_t tank_type = 3;    // mobile gun, TANK
constexpr uint8_t resurrect_kind = 37;
constexpr uint16_t wreck_feature = 0;
// The 2x2 wreck's origin plot and a plot of its footprint away from the origin.
constexpr int32_t wreck_x = 20, wreck_z = 20;
constexpr int32_t small_wreck_x = 26, small_wreck_z = 26;
constexpr uint16_t small_wreck_feature = 1;

using Services = oa::test::QuietServices;

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

sim::ground_orders::Point cell_point(int32_t x, int32_t z) {
    return {(x * 16 + 8) << 16, 0, (z * 16 + 8) << 16};
}

struct Fixture {
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain =
        std::vector<sim::visibility_state::TerrainCell>(map_cells * map_cells);
    std::array<sim::visibility_state::SightMask, 1> masks{};
    std::shared_ptr<formats::objects3d::Model> model =
        std::make_shared<formats::objects3d::Model>();
    std::shared_ptr<formats::cob::CobProgram> script = std::make_shared<formats::cob::CobProgram>();
    std::shared_ptr<formats::cob::CobProgram> builder_script =
        std::make_shared<formats::cob::CobProgram>();
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
    std::vector<FeatureDef> features = std::vector<FeatureDef>(2);
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    std::unique_ptr<sim::match_runtime::Match> match;

    Fixture() {
        map.attribute_width = map.attribute_height = map_cells;
        map.attributes.resize(map_cells * map_cells);
        masks[0].width = masks[0].height = 1;
        masks[0].pixels = {1};
        model->objects.resize(1);
        model->objects[0].name = "root";
        model->objects[0].vertices = {{0, 0, 0}, {0, 20 << 16, 0}};
        using namespace sim::script_vm;
        script->code = {opcode::return_};
        script->scripts = {{"Create", 0}};
        script->entry_points = {0};
        script->piece_names = {"root"};
        // StartBuilding keeps its argument in static 0.
        builder_script->code = {
            opcode::return_, opcode::push_local, 0, opcode::pop_static, 0, opcode::return_
        };
        builder_script->scripts = {{"Create", 0}, {"StartBuilding", 1}};
        builder_script->entry_points = {0, 1};
        builder_script->header.static_variable_count = 1;
        builder_script->piece_names = {"root"};
        // Activate opens the yard (YARD_OPEN) and enters the build stance.
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
        types[builder_type].simulation.flags |= OA_UNIT_DEF_FLAG_BUILDER;
        types[builder_type].simulation.abilities =
            OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_REPAIR |
            OA_UNIT_DEF_ABILITY_CAN_RECLAMATE | OA_UNIT_DEF_ABILITY_CAN_CAPTURE |
            OA_UNIT_DEF_ABILITY_CAN_RESURRECT;
        loaded[builder_type].script = builder_script;
        types[builder_type].cob = reinterpret_cast<uintptr_t>(builder_script.get());
        types[factory_type].simulation.flags |= OA_UNIT_DEF_FLAG_BUILDER;
        types[factory_type].simulation.maximum_health = 1000;
        types[factory_type].bm_code = 0;
        types[factory_type].footprint_x = types[factory_type].footprint_z = 2;
        loaded[factory_type].script = factory_script;
        types[factory_type].cob = reinterpret_cast<uintptr_t>(factory_script.get());
        fields[factory_type].yard_mask = factory_yard;
        types[tank_type].simulation.flags |= OA_UNIT_DEF_FLAG_HAS_WEAPONS;
        types[tank_type].simulation.abilities =
            OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_ATTACK;
        defs[tank_type].weapon1 = "TESTGUN";
        for (const auto mobile : {builder_type, tank_type})
            defs[mobile].can_move = true;
        defs[tank_type].can_attack = true;
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
        // A 2x2 tank wreck at (20, 20) and a 1x1 one at (26, 26).
        oa::base::text::copy_terminated(features[wreck_feature].name, "TANK_DEAD");
        features[wreck_feature].footprint_x = features[wreck_feature].footprint_z = 2;
        oa::base::text::copy_terminated(features[small_wreck_feature].name, "TANK_HEAP");
        features[small_wreck_feature].footprint_x = features[small_wreck_feature].footprint_z = 1;
        for (auto& def : features) {
            def.metal = 20.0F;
            def.flags = OA_FEATURE_FLAG_RECLAIMABLE;
            def.dead_feature = sim::feature_runtime::no_feature;
            def.burnt_feature = sim::feature_runtime::no_feature;
            def.reclamate_feature = sim::feature_runtime::no_feature;
        }
        for (int32_t z = wreck_z; z < wreck_z + 2; ++z)
            for (int32_t x = wreck_x; x < wreck_x + 2; ++x) {
                auto& cell = plots[static_cast<size_t>(z * map_cells + x)];
                const bool origin = x == wreck_x && z == wreck_z;
                cell.feature_word =
                    origin ? wreck_feature : sim::spatial_state::feature_continuation;
                cell.feature_back_x = static_cast<uint8_t>(x - wreck_x);
                cell.feature_back_z = static_cast<uint8_t>(z - wreck_z);
                cell.feature_footprint_x = cell.feature_footprint_z = 2;
                cell.blocking_feature = true;
            }
        plots[static_cast<size_t>(small_wreck_z * map_cells + small_wreck_x)].feature_word =
            small_wreck_feature;
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
        input.feature_defs = features;
        match = std::make_unique<sim::match_runtime::Match>(input, services);
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

    sim::unit_spawn::Slot* newest(uint16_t type) {
        sim::unit_spawn::Slot* found = nullptr;
        for (auto& slot : match->world().slots)
            if (slot.unit && slot.record.type_index == type)
                found = &slot;
        return found;
    }

    // The StartBuilding argument the unit's script last received.
    int32_t start_building_argument(const sim::unit_spawn::Slot& unit) {
        match->tick_scripts(1);
        auto* instance = match->instance(unit.unit_index);
        CHECK(instance && instance->script());
        const auto stored = instance->script()->vm().static_value(0);
        CHECK(stored.has_value());
        return *stored;
    }
};

// --- Finished units ----------------------------------------------------------

void factory_completion_is_shared() {
    Fixture f;
    Recorder recorder;
    recorder.install(*f.match);
    set_run_flag(*f.match, true);
    auto& factory = f.spawn(0, factory_type, 100, 100);
    const auto id = factory.unit_index;
    // A building created finished names itself as its builder, after its creation.
    CHECK((recorder.kinds() == std::vector<std::string>{"created", "finished"}));
    auto finished = recorder.take("finished");
    CHECK(finished[0].first == id && finished[0].second == id);
    recorder.calls.clear();

    // The factory's product: carried on the pad, built step by step, then
    // set down and finished, each shared once and in that order.
    auto& order = f.match->issue_building_build(id, tank_type, 1, false);
    CHECK(step(*f.match, factory, order) == 1);
    CHECK(step(*f.match, factory, order) == 2);
    f.match->tick_scripts(1);
    CHECK(step(*f.match, factory, order) == 1);
    CHECK(step(*f.match, factory, order) == 1);
    auto* frame = f.newest(tank_type);
    CHECK(frame && link_parent(frame->record) == id);
    auto carry = recorder.take("carry");
    CHECK(carry.size() == 1 && carry[0].first == frame->unit_index && carry[0].second == id);
    CHECK(carry[0].third == -1 && carry[0].fourth == 1 && !carry[0].state_at_call);
    CHECK(recorder.take("finished").empty());
    f.defs[tank_type].activate_when_built = true;
    f.match->multiplayer.unit_flags_changed = [](void* context, uint16_t unit, uint8_t) {
        Recorder::of(context).calls.push_back({"flags", unit});
    };
    recorder.calls.clear();
    uint32_t steps = 0;
    while (order.phase == 3 && steps++ < 200) {
        Fixture::fund(factory);
        (void)step(*f.match, factory, order);
    }
    CHECK(frame->record.build_remaining == 0.0F && link_parent(frame->record) == 0);
    std::vector<std::string> shared;
    for (const auto& call : recorder.calls)
        if (call.kind == "carry" || call.kind == "finished")
            shared.push_back(call.kind);
    CHECK((shared == std::vector<std::string>{"carry", "finished"}));

    // The product activates when built: it is set down first, then
    // activated, then finished.
    std::vector<std::string> activated;
    for (const auto& call : recorder.calls)
        if (call.kind == "carry" || call.kind == "flags" || call.kind == "finished")
            activated.push_back(call.kind);
    CHECK((activated == std::vector<std::string>{"carry", "flags", "finished"}));
    f.defs[tank_type].activate_when_built = false;
    f.match->multiplayer.unit_flags_changed = [](void*, uint16_t, uint8_t) {};
    finished = recorder.take("finished");
    CHECK(finished[0].first == frame->unit_index && finished[0].second == id);
    carry = recorder.take("carry");
    CHECK(
        carry[0].first == frame->unit_index && carry[0].second == 0 &&
        carry[0].state_at_call == false
    );

    // A mobile unit created finished shares its creation only.
    recorder.calls.clear();
    auto& tank = f.spawn(0, tank_type, 60, 60);
    CHECK((recorder.kinds() == std::vector<std::string>{"created"}));
    CHECK(recorder.calls[0].first == tank.unit_index);

    // Outside a live game nothing is shared.
    set_run_flag(*f.match, false);
    recorder.calls.clear();
    (void)f.spawn(0, factory_type, 160, 160);
    CHECK(recorder.calls.empty());
    std::cout << "factory completion is shared passed\n";
}

void mirrored_copy_is_finished() {
    Fixture f;
    Recorder recorder;
    f.defs[tank_type].activate_when_built = true;
    auto& factory = f.spawn(1, factory_type, 100, 100);
    auto& copy = f.spawn(1, tank_type, 104, 104, false);
    f.match->apply_carry_link(copy.unit_index, factory.unit_index, -1, 1);
    CHECK(copy.record.build_remaining != 0.0F);
    CHECK(!(copy.record.state_flags & OA_UNIT_STATE_ACTIVE));
    f.match->simulation().players[1].status = OA_PLAYER_STATUS_MIRRORED;
    recorder.install(*f.match);
    set_run_flag(*f.match, true);

    // The builder link finishes the copy and activates it; the copy stays on
    // its pad until its own machine's carry link sets it down, and nothing
    // is shared for it.
    copy.record.flags &= ~OA_UNIT_FLAG_CONSTRUCTION_DIRTY;
    f.match->finish_unit(copy.unit_index, factory.unit_index);
    CHECK(copy.record.build_remaining == 0.0F);
    CHECK(copy.record.flags & OA_UNIT_FLAG_CONSTRUCTION_DIRTY);
    CHECK(copy.record.state_flags & OA_UNIT_STATE_ACTIVE);
    CHECK(link_parent(copy.record) == factory.unit_index);
    CHECK(recorder.calls.empty());

    // A dead builder finishes nothing, and a builder slot outside the pool
    // is ignored; a unit slot outside it is taken as reserved slot 0, which
    // holds no unit, and nothing happens.
    auto& second = f.spawn(1, tank_type, 140, 140, false);
    recorder.calls.clear();
    f.match->finish_unit(second.unit_index, 0);
    f.match->finish_unit(second.unit_index, 0xffff);
    CHECK(second.record.build_remaining != 0.0F);
    f.match->finish_unit(0xffff, factory.unit_index);
    CHECK(recorder.calls.empty() && f.match->fault() == nullptr);
    // A building created finished elsewhere names itself: its copy is
    // finished the same way.
    f.match->finish_unit(second.unit_index, second.unit_index);
    CHECK(second.record.build_remaining == 0.0F && recorder.calls.empty());
    std::cout << "mirrored copy is finished passed\n";
}

// --- StartBuilding -----------------------------------------------------------

void start_building_is_shared() {
    Fixture f;
    Recorder recorder;
    recorder.install(*f.match);
    set_run_flag(*f.match, true);
    auto& builder = f.spawn(0, builder_type, 100, 100);
    const auto id = builder.unit_index;
    constexpr int16_t start_building_index = 1;
    const auto check_start = [&](bool below_zero) {
        const auto calls = recorder.take("script");
        CHECK(calls.size() == 1);
        CHECK(calls[0].first == id && calls[0].third == start_building_index);
        CHECK(calls[0].fourth == 1);
        // The heading goes out zero-extended from 16 bits; the builder's own
        // script took it as a signed word.
        const auto argument = f.start_building_argument(builder);
        CHECK(calls[0].locals[0] == static_cast<uint16_t>(argument));
        CHECK((argument < 0) == below_zero);
        CHECK(calls[0].locals[1] == 0 && calls[0].locals[2] == 0 && calls[0].locals[3] == 0);
        recorder.calls.clear();
    };

    // MobileBuild: phase 1 places the frame and starts building toward it;
    // the site east of the builder is a heading below zero as a signed word.
    auto& build = f.match->issue_mobile_build(id, factory_type, point(150, 101), false);
    CHECK(step(*f.match, builder, build) == 1);
    recorder.calls.clear();
    CHECK(step(*f.match, builder, build) == 1);
    check_start(true);
    f.match->stop_orders(id);

    // RepairUnit of a damaged unit within reach.
    auto& patient = f.spawn(0, tank_type, 60, 110);
    patient.record.health = 40;
    recorder.calls.clear();
    auto& repair = f.match->issue_repair(id, patient.unit_index, false);
    CHECK(step(*f.match, builder, repair) == 1);
    CHECK(step(*f.match, builder, repair) == 1);
    check_start(false);
    f.match->stop_orders(id);

    // ReclaimUnit of an enemy tank once the builder has arrived.
    auto& victim = f.spawn(1, tank_type, 110, 60);
    recorder.calls.clear();
    auto& reclaim = f.match->issue_reclaim(id, victim.unit_index, false);
    CHECK(step(*f.match, builder, reclaim) == 1);
    CHECK(step(*f.match, builder, reclaim, sim::ground_orders::arrived_event) == 1);
    CHECK(recorder.take("script").empty());
    CHECK(step(*f.match, builder, reclaim) == 1);
    check_start(true);
    f.match->stop_orders(id);

    // With the entry null the script still starts and the order goes on.
    f.match->multiplayer.script_started = nullptr;
    auto& quiet = f.match->issue_mobile_build(id, factory_type, point(40, 150), false);
    CHECK(step(*f.match, builder, quiet) == 1);
    CHECK(
        step(*f.match, builder, quiet) == 1 &&
        sim::match_runtime::MatchTickAccess::order_building(quiet)
    );
    f.match->stop_orders(id);
    std::cout << "start building is shared passed\n";
}

// --- Resurrection ------------------------------------------------------------

// Runs a resurrect order from its start to the step that raises the wreck.
void raise_wreck(Fixture& f, sim::unit_spawn::Slot& builder, sim::simulation_state::Order& order) {
    CHECK(step(*f.match, builder, order) == 1);
    CHECK(step(*f.match, builder, order) == 1);
    builder.record.build_flags |= 1; // the build stance the script would raise
    CHECK(step(*f.match, builder, order) == 1);
    CHECK(step(*f.match, builder, order) == 1);
    uint32_t steps = 0;
    while (step(*f.match, builder, order) == 2 && steps < 100)
        ++steps;
    CHECK(order.phase == 5);
    CHECK(step(*f.match, builder, order) == 1);
}

void resurrection_is_shared() {
    Fixture f;
    Recorder recorder;
    recorder.install(*f.match);
    set_run_flag(*f.match, true);
    auto& builder = f.spawn(0, builder_type, 300, 300);
    const auto id = builder.unit_index;

    // Aimed at a plot of the 2x2 wreck away from its origin: the wreck goes
    // once the unit is raised, named by its origin plot.
    auto& order =
        f.match->insert_ground_order(id, resurrect_kind, cell_point(wreck_x + 1, wreck_z + 1));

    // The raised unit is reported created by the resurrecting unit, right
    // after its own creation.
    struct Raised {
        struct Note {
            bool created_by{};
            uint16_t unit{};
            uint16_t type{};
            uint16_t builder{};
        };

        std::vector<Note> notes;
    } raised;

    sim::match_runtime::EventHooks hooks;
    hooks.context = &raised;
    hooks.unit_created = [](void* context, const oa::World& world, uint16_t unit) {
        static_cast<Raised*>(context)->notes.push_back(
            {false, unit, world.units[unit].type_index, 0}
        );
    };
    hooks.unit_created_by = [](void* context, const oa::World&, uint16_t unit, uint16_t builder) {
        static_cast<Raised*>(context)->notes.push_back({true, unit, 0, builder});
    };
    CHECK(f.match->add_event_listener(hooks) != 0);

    raise_wreck(f, builder, order);
    CHECK(raised.notes.size() == 2);
    CHECK(!raised.notes[0].created_by && raised.notes[0].type == tank_type);
    CHECK(raised.notes[0].unit != id);
    CHECK(
        raised.notes[1].created_by && raised.notes[1].unit == raised.notes[0].unit &&
        raised.notes[1].builder == id
    );
    auto shared = recorder.take("feature");
    CHECK(shared.size() == 1);
    CHECK(shared[0].first == static_cast<uint32_t>(FeatureChange::resurrected));
    CHECK(shared[0].second == static_cast<uint32_t>(wreck_x) && shared[0].third == wreck_z);
    CHECK(shared[0].fourth == id);
    const auto& origin =
        f.match->spatial().plots[static_cast<size_t>(wreck_z * map_cells + wreck_x)];
    CHECK(origin.feature_word == sim::spatial_state::no_feature);
    f.match->stop_orders(id);

    // A resurrecting unit simulated elsewhere shares nothing.
    recorder.calls.clear();
    auto& again =
        f.match->insert_ground_order(id, resurrect_kind, cell_point(small_wreck_x, small_wreck_z));
    CHECK(step(*f.match, builder, again) == 1);
    f.match->simulation().players[0].status = OA_PLAYER_STATUS_MIRRORED;
    again.phase = 1;
    while (again.phase < 5) {
        builder.record.build_flags |= 1;
        (void)step(*f.match, builder, again);
    }
    CHECK(step(*f.match, builder, again) == 1);
    CHECK(recorder.take("feature").empty());
    std::cout << "resurrection is shared passed\n";
}

// --- Units handed to another player ------------------------------------------

void gift_to_another_machine() {
    Fixture f;
    Recorder recorder;
    recorder.install(*f.match);
    set_run_flag(*f.match, true);
    auto& tank = f.spawn(0, tank_type, 100, 100);
    const auto id = tank.unit_index;
    tank.record.flags |= OA_UNIT_FLAG_SELECTED;
    f.match->simulation().players[1].status = OA_PLAYER_STATUS_MIRRORED;
    const auto losses = f.match->state().game.players[0].losses;
    recorder.calls.clear();

    // Handed over while still live and owned here, then dying as captured
    // with no attacker; no copy is created here.
    f.match->transfer_unit(id, 1);
    const auto handed = recorder.take("transferred");
    CHECK(handed.size() == 1 && handed[0].first == id && handed[0].second == 1);
    CHECK(handed[0].third == 0 && handed[0].state_at_call);
    CHECK(recorder.take("created").empty());
    CHECK(!(tank.record.flags & OA_UNIT_FLAG_SELECTED) && tank.record.capture_cooldown == 150);
    CHECK(tank.record.flags & OA_UNIT_FLAG_DEATH_PENDING);
    CHECK(tank.record.damage_kind == static_cast<uint8_t>(DeathKind::captured));
    f.match->kill_unit(id, tank.record.damage_kind);
    const auto killed = recorder.take("killed");
    CHECK(killed.size() == 1 && killed[0].first == id);
    CHECK(killed[0].third == static_cast<int32_t>(DeathKind::captured));
    CHECK(f.match->state().game.players[0].losses == losses);

    // Given to the player that owns it, or once dying, nothing happens.
    recorder.calls.clear();
    auto& kept = f.spawn(0, tank_type, 140, 100);
    recorder.calls.clear();
    f.match->transfer_unit(kept.unit_index, 0);
    kept.record.flags |= OA_UNIT_FLAG_DEATH_PENDING;
    f.match->transfer_unit(kept.unit_index, 1);
    kept.record.flags &= ~OA_UNIT_FLAG_DEATH_PENDING;
    // Nor to an absent player, or a player outside the table.
    f.match->simulation().players[2].present = false;
    f.match->transfer_unit(kept.unit_index, 2);
    f.match->transfer_unit(kept.unit_index, 10);
    CHECK(recorder.calls.empty());
    CHECK((kept.record.flags & OA_UNIT_FLAG_LIVE) && kept.record.owner_index == 0);

    // With the entry null the unit still dies.
    f.match->multiplayer.unit_transferred = nullptr;
    f.match->transfer_unit(kept.unit_index, 1);
    CHECK(kept.record.flags & OA_UNIT_FLAG_DEATH_PENDING);
    std::cout << "gift to another machine passed\n";
}

void gift_to_this_machine() {
    Fixture f;
    Recorder recorder;
    recorder.install(*f.match);
    set_run_flag(*f.match, true);

    // The unit's own state: a finished copy for the new owner where it
    // stands, the old unit dying as captured.
    auto& tank = f.spawn(0, tank_type, 100, 100);
    tank.record.health = 60;
    tank.record.bank = 3;
    tank.record.heading = 0x4000;
    tank.record.pitch = -5;
    tank.record.state_flags |= OA_UNIT_STATE_ACTIVE;
    tank.record.weapons[0].stockpile = 9;
    tank.record.weapons[1].stockpile = 8;
    recorder.calls.clear();
    f.match->transfer_unit(tank.unit_index, 1);
    CHECK(recorder.take("transferred").empty());
    auto created = recorder.take("created");
    CHECK(created.size() == 1);
    auto& copy = f.match->world().slots.at(created[0].first);
    CHECK(copy.record.owner_index == 1 && copy.record.type_index == tank_type);
    CHECK(copy.record.position.x == tank.record.position.x);
    CHECK(copy.record.health == 60 && copy.record.build_remaining == 0.0F);
    CHECK(copy.record.bank == 3 && copy.record.heading == 0x4000 && copy.record.pitch == -5);
    CHECK((copy.record.flags & (OA_UNIT_FLAG_MOVE_ORDER_MASK | OA_UNIT_FLAG_FIRE_ORDER_MASK)) == 0);
    CHECK(copy.record.state_flags & OA_UNIT_STATE_ACTIVE);
    // Stockpiles go over for slots whose weapon is enabled on the copy.
    CHECK(
        (copy.record.weapons[0].flags & OA_UNIT_WEAPON_ENABLED) &&
        copy.record.weapons[0].stockpile == 9
    );
    CHECK(
        !(copy.record.weapons[1].flags & OA_UNIT_WEAPON_ENABLED) &&
        copy.record.weapons[1].stockpile == 0
    );
    CHECK(tank.record.flags & OA_UNIT_FLAG_DEATH_PENDING);
    CHECK(tank.record.damage_kind == static_cast<uint8_t>(DeathKind::captured));

    // A unit another player's machine handed over: the copy takes the state
    // it carried, stockpiles only in slots whose weapon is enabled, and the
    // old copy is left for its own machine to kill.
    auto& given = f.spawn(1, tank_type, 150, 150);
    f.match->simulation().players[1].status = OA_PLAYER_STATUS_MIRRORED;
    recorder.calls.clear();
    sim::match_runtime::TransferredUnit carried;
    carried.build_remaining = 0.0F;
    carried.health = 77;
    carried.bank = -2;
    carried.heading = 0x9000;
    carried.pitch = 4;
    carried.stockpiles = {5, 6, 7};
    f.match->transfer_unit(given.unit_index, 0, &carried);
    created = recorder.take("created");
    CHECK(created.size() == 1);
    auto& received = f.match->world().slots.at(created[0].first);
    CHECK(received.record.owner_index == 0 && received.record.health == 77);
    CHECK(received.record.bank == -2 && received.record.heading == 0x9000);
    CHECK(received.record.pitch == 4);
    CHECK(received.record.weapons[0].flags & OA_UNIT_WEAPON_ENABLED);
    CHECK(received.record.weapons[0].stockpile == 5);
    CHECK(!(received.record.weapons[1].flags & OA_UNIT_WEAPON_ENABLED));
    CHECK(received.record.weapons[1].stockpile == 0 && received.record.weapons[2].stockpile == 0);
    CHECK(
        (given.record.flags & OA_UNIT_FLAG_LIVE) &&
        !(given.record.flags & OA_UNIT_FLAG_DEATH_PENDING)
    );
    CHECK(recorder.take("transferred").empty());

    // A unit simulated elsewhere given to a player simulated elsewhere is
    // left alone.
    auto& elsewhere = f.spawn(0, tank_type, 200, 200);
    f.match->simulation().players[0].status = OA_PLAYER_STATUS_MIRRORED;
    f.match->simulation().players[2].present = true;
    f.match->simulation().players[2].status = OA_PLAYER_STATUS_MIRRORED;
    recorder.calls.clear();
    f.match->transfer_unit(elsewhere.unit_index, 2);
    CHECK(recorder.calls.empty());
    CHECK((elsewhere.record.flags & OA_UNIT_FLAG_LIVE) && elsewhere.record.owner_index == 0);
    CHECK(!(elsewhere.record.flags & OA_UNIT_FLAG_DEATH_PENDING));
    std::cout << "gift to this machine passed\n";
}

// --- A game ended from elsewhere ---------------------------------------------

void game_ended_from_elsewhere() {
    // An ongoing game ends at once as a defeat, and stays one although its
    // victory test would now pass.
    {
        combat_fixture::Fixture f;
        (void)f.spawn(0, 40, 40);
        f.run(2);
        CHECK(f.match->outcome() == sim::scenario::Outcome::ongoing);
        f.match->end_local_game();
        const auto& state = f.match->outcome_state();
        CHECK(f.match->outcome() == sim::scenario::Outcome::defeat);
        CHECK(state.flags & sim::scenario::outcome_flag::finished);
        CHECK(!(state.flags & sim::scenario::outcome_flag::won));
        f.run(400);
        CHECK(f.match->outcome() == sim::scenario::Outcome::defeat);
        CHECK(!(f.match->outcome_state().flags & sim::scenario::outcome_flag::won));
    }
    // A game already won ends the same way: the victory becomes a defeat.
    {
        combat_fixture::Fixture f;
        (void)f.spawn(0, 40, 40);
        for (int32_t tick = 0; tick < 600 && f.match->outcome() == sim::scenario::Outcome::ongoing;
             ++tick)
            f.run(1);
        CHECK(f.match->outcome() == sim::scenario::Outcome::victory);
        const auto flags = f.match->outcome_state().flags;
        CHECK(flags & sim::scenario::outcome_flag::won);
        f.match->end_local_game();
        CHECK(f.match->outcome() == sim::scenario::Outcome::defeat);
        CHECK(
            f.match->outcome_state().flags ==
            ((flags | sim::scenario::outcome_flag::finished) & ~sim::scenario::outcome_flag::won)
        );
        f.run(10);
        CHECK(f.match->outcome() == sim::scenario::Outcome::defeat);
    }
    std::cout << "game ended from elsewhere passed\n";
}

} // namespace

int main() {
    try {
        interceptions_are_shared();
        carry_links_are_shared();
        factory_completion_is_shared();
        mirrored_copy_is_finished();
        start_building_is_shared();
        resurrection_is_shared();
        gift_to_another_machine();
        gift_to_this_machine();
        game_ended_from_elsewhere();
    } catch (const std::exception& error) {
        std::cerr << "shared events: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
