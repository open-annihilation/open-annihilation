// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A computer player handed its AI profile and side build lists as the session
// starts builds through the full match tick: the knowledge record's strength
// triples (the strategic refresh, then the knowledge walk's one-in-30 roll)
// score the builder's list and the construction task places the pick.
#include "oa/sim/ai.hpp"
#include "oa/sim/match_runtime.hpp"

#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
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
constexpr int32_t map_cells = 64;
constexpr uint8_t computer = 1;
// The computer players are set up at the session start and the knowledge walk rolls one in 30 each
// 30-tick refresh; a 900-tick run holds 30 refresh rolls.
constexpr uint32_t run_ticks = 1200;
constexpr float starting_resources = 900.0F;
// The build scoring grades the income in Player.energy_produced and
// metal_produced: under 50 energy or 3 metal
// a second both needs saturate and only the metal and energy strengths score.
// After the medium computer's 0.7 credit (energy and metal) the builder's
// yield clears 200 energy and 5 metal, leaving the whole score to the base
// priority while the stores stay full.
constexpr float builder_energy_make = 300.0F;
constexpr float builder_metal_make = 10.0F;

enum Type : uint16_t { vehicle = 1, collector, relay, silo, missile, type_count };

// The types' build lists (UnitDef.build_ids), from type 0 on: a count, then that many
// ids. The vehicle builds the collector and the relay; the silo's list offers the missile
// its weapon stockpiles.
constexpr uint16_t build_lists[] = {0, 2, collector, relay, 0, 0, 1, missile, 0};
// The same types with the missile on the vehicle's list alone.
constexpr uint16_t builder_round_lists[] = {0, 1, missile, 0, 0, 0, 0};

// A leading line before any plan applies, as the plan flag starts set; it
// weights the collector to a tenth. The medium plan halves the relay's weight.
constexpr const char* profile = R"(// test profile
weight ARMSOLAR 0.1
plan medium
weight ARMRELAY 0.5
limit ARMRELAY 0
)";

using Services = oa::test::QuietServices;

using Scenario = oa::test::EmptyScenario;

// A flat 64x64-cell map with an ARM construction vehicle, a solar collector
// and a relay it can build; player 0 is local, player 1 the computer. The
// vehicle supplies enough ongoing income for the priority strength to enter
// the build scoring's weighted score after the first economy settlement, and
// so does the silo, an armed building whose build list offers a missile: a type
// named as a weapon round, with no health of its own.
struct Fixture {
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain_values;
    std::array<sim::visibility_state::SightMask, 1> masks{};
    std::shared_ptr<formats::objects3d::Model> model =
        std::make_shared<formats::objects3d::Model>();
    std::shared_ptr<formats::cob::CobProgram> script = std::make_shared<formats::cob::CobProgram>();
    std::array<sim::unit_spawn::LoadedType, type_count> loaded;
    std::array<sim::unit_spawn::Type, type_count> types;
    std::array<data::unit_definitions::UnitDefinition, type_count> defs;
    std::array<data::unit_definitions::RuntimeDefinitionMetadata, type_count> metadata;
    std::array<sim::match_runtime::RuntimeTypeFields, type_count> fields{};
    std::vector<uint8_t> yard_cells = std::vector<uint8_t>(4, 4);
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    sim::combat_state::WeaponRegistry weapons;
    std::vector<sim::spatial_state::Plot> plots;
    Services services;
    Scenario scenario;
    std::unique_ptr<sim::match_runtime::Match> match;

    // An armed vehicle carries the silo's weapon, as a mobile missile defence does.
    explicit Fixture(
        uint32_t downloadable_type = 0,
        bool builder_income = true,
        const data::match_rules::MatchRules& rules = {},
        bool armed_vehicle = false
    ) {
        map.attribute_width = map.attribute_height = map_cells;
        map.attributes.resize(map_cells * map_cells);
        plots.resize(map_cells * map_cells);
        terrain_values.resize(map_cells * map_cells);
        masks[0].width = masks[0].height = 15;
        masks[0].offset_x = masks[0].offset_z = 7;
        masks[0].pixels.assign(15 * 15, 1);
        model->objects.resize(1);
        auto& root = model->objects[0];
        root.name = "root";
        constexpr int32_t half = 8 << 16, top = 12 << 16;
        root.vertices = {
            {-half, 0, -half}, {half, 0, -half}, {half, 0, half}, {-half, 0, half}, {0, top, 0}
        };
        root.primitives.resize(1);
        root.primitives[0].vertex_indices = {0, 1, 2, 3};
        root.selection_primitive = 0;
        script->code = {sim::script_vm::opcode::return_};
        script->scripts = {{"Create", 0}};
        script->entry_points = {0};
        script->piece_names = {"root"};
        const char* names[type_count] = {
            "", "ARMCV", "ARMSOLAR", "ARMRELAY", "ARMSILO", "ARMMAKENUKE"
        };
        for (size_t i = 1; i < type_count; ++i) {
            loaded[i].model = model;
            loaded[i].script = script;
            loaded[i].unit_name = names[i];
            types[i].simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
            types[i].simulation.maximum_health = 100;
            types[i].footprint_x = types[i].footprint_z = 2;
            types[i].bm_code = 0;
            types[i].model = reinterpret_cast<uintptr_t>(model.get());
            types[i].cob = reinterpret_cast<uintptr_t>(script.get());
            defs[i].unit_name = names[i];
            defs[i].side = "ARM";
            defs[i].categories = {"ARM", "LEVEL1"};
            defs[i].sight_distance = 200;
            defs[i].build_time = 60;
            defs[i].build_cost_energy = 50;
            defs[i].build_cost_metal = 10;
            metadata[i].footprint_x = metadata[i].footprint_z = 2;
            fields[i].definition = &defs[i];
            fields[i].runtime_metadata = &metadata[i];
            fields[i].target_masks = &target_masks;
            fields[i].yard_mask = yard_cells;
        }
        types[vehicle].simulation.flags |= OA_UNIT_DEF_FLAG_BUILDER;
        types[vehicle].bm_code = 1;
        defs[vehicle].builder = true;
        defs[vehicle].can_move = true;
        defs[vehicle].worker_time = 300;
        defs[vehicle].build_distance = 96;
        defs[vehicle].acceleration_fixed = 65536 / 4;
        defs[vehicle].brake_rate_fixed = 65536 / 4;
        defs[vehicle].max_velocity_fixed = 2 * 65536;
        defs[vehicle].turn_rate = 800;
        defs[vehicle].energy_storage = 1000.0F;
        defs[vehicle].metal_storage = 1000.0F;
        if (builder_income) {
            defs[vehicle].energy_make = builder_energy_make;
            defs[vehicle].metal_make = builder_metal_make;
        }
        fields[vehicle].movement_class = 0;
        if (armed_vehicle) {
            types[vehicle].simulation.flags |= OA_UNIT_DEF_FLAG_HAS_WEAPONS;
            defs[vehicle].weapon1 = "NUKE";
        }
        defs[collector].energy_make = 20.0F;
        types[silo].simulation.flags |= OA_UNIT_DEF_FLAG_BUILDER | OA_UNIT_DEF_FLAG_HAS_WEAPONS;
        defs[silo].builder = true;
        defs[silo].weapon1 = "NUKE";
        defs[silo].energy_storage = 1000.0F;
        defs[silo].metal_storage = 1000.0F;
        defs[silo].energy_make = builder_energy_make;
        defs[silo].metal_make = builder_metal_make;
        // The missile is a mobile type with no MaxDamage, as a build list's rounds are.
        types[missile].simulation.maximum_health = 0;
        types[missile].bm_code = 1;
        (void)sim::combat_state::install_weapon_text(
            weapons,
            "[NUKE]{id=1; reloadtime=0.4; energypershot=120; metalpershot=60; stockpile=1;}"
        );
        if (downloadable_type != 0) {
            types[downloadable_type].simulation.flags |= OA_UNIT_DEF_FLAG_DOWNLOADABLE;
            defs[downloadable_type].downloadable = true;
        }
        for (size_t i = 1; i < type_count; ++i)
            loaded[i].type = types[i];

        sim::match_runtime::OfflineInputs input{
            map,  loaded, types, fields, weapons,   terrain_values,       masks, 32, 32, 40,
            2,    0,      30,    1,      &scenario, [] { return 1000u; }, plots, {}, 0,  0,
            0.0F, {}
        };
        input.rules = rules;
        match = std::make_unique<sim::match_runtime::Match>(input, services);
        match->set_difficulty(OA_DIFFICULTY_MEDIUM);
        for (uint8_t p = 0; p < 2; ++p) {
            match->simulation().players[p].present = true;
            match->simulation().players[p].status = p == computer ? 2 : 1;
            std::array<uint8_t, 10> allies{};
            allies[p] = 1;
            match->configure_player_alliances(p, allies);
            auto& player = match->state().game.players[p];
            player.energy = player.metal = starting_resources;
        }
        std::array<uint8_t, 10> local_allies{};
        local_allies[0] = 1;
        match->configure_outcomes(0, local_allies, false);
        match->state().unit_defs[vehicle].abilities =
            OA_UNIT_DEF_ABILITY_CAN_MOVE | OA_UNIT_DEF_ABILITY_CAN_STOP;
    }

    sim::unit_spawn::Slot& spawn(uint8_t player, Type type, int32_t cell_x, int32_t cell_z) {
        const auto at = [](int32_t cell) { return static_cast<uint32_t>(cell * 16 + 8) << 16; };
        auto* slot = match->create({player, type, {at(cell_x), 0, at(cell_z)}, true, 1, 0});
        CHECK(slot && slot->unit);
        slot->unit->object_present = true;
        return *slot;
    }

    // Ticks until `done` holds or the run ends; false when it never held.
    bool run(uint32_t ticks, const std::function<bool()>& done) {
        for (uint32_t i = 0; i < ticks; ++i) {
            ++match->state().game.tick;
            match->tick();
            if (done())
                return true;
        }
        return false;
    }

    size_t count(uint8_t owner, Type type) const {
        size_t n = 0;
        for (const auto& slot : match->world().slots)
            n += slot.unit && slot.record.type_index == type && slot.record.owner_index == owner;
        return n;
    }
};

void computer_builds_from_its_profile_and_lists() {
    Fixture f;
    f.spawn(0, vehicle, 8, 8);
    f.spawn(computer, vehicle, 40, 40);
    CHECK(sim::ai::configure_match_computer_players(*f.match, profile, build_lists, false));
    const auto* players = sim::ai::match_computer_players(*f.match);
    CHECK(players->downloadables_restricted == 0);
    uint32_t ticks = 0;
    const bool built = f.run(run_ticks, [&] {
        ++ticks;
        return f.count(computer, collector) != 0;
    });
    // The strategic refresh fills one triple per type when the record is built; the
    // collector's base priority is (1 + its 20 EnergyMake) times 4 while the
    // player owns none.
    const auto& strengths = f.match->strategic_state(computer).strengths;
    CHECK(strengths.size() == type_count);
    CHECK(strengths[collector][0] == 84);
    // Its 10 metal and 50 energy cost leave both of its other strengths at 0.
    CHECK(strengths[collector][1] == 0 && strengths[collector][2] == 0);
    const auto* k =
        sim::ai::computer_player_knowledge(sim::ai::match_computer_players(*f.match), computer);
    CHECK(k != nullptr);
    // The leading weight line applied before the plans; the medium plan
    // weighted and capped the relay.
    CHECK(k->weight_percent[collector] == 10);
    CHECK(k->weight_percent[relay] == 50 && k->limits[relay] == 0);
    CHECK(sim::ai::match_computer_players(*f.match)->types[vehicle].build_count == 2);
    CHECK(built);
    const auto& income = f.match->state().game.players[computer];
    CHECK(oa::player_energy_produced(&income) == 210.0F);
    CHECK(oa::player_metal_produced(&income) == 7.0F);
    // The builder joins its squad at the first sort, 30 ticks in.
    CHECK(ticks > 30);
    CHECK(f.count(computer, relay) == 0);
    CHECK(f.count(0, collector) == 0);
    // Past the 900-tick mark the refreshes keep running without failing.
    f.run(run_ticks - ticks, [] { return false; });
}

// With no income the build scoring adds 100 to both needs: the metal share takes the
// whole score and the collector's metal strength is 0, so it is never picked.
void computer_without_income_skips_the_collector() {
    Fixture f(0, false);
    f.spawn(computer, vehicle, 40, 40);
    CHECK(sim::ai::configure_match_computer_players(*f.match, profile, build_lists, false));
    CHECK(!f.run(run_ticks, [&] { return f.count(computer, collector) != 0; }));
}

// The build scoring scores no DOWNLOADABLE type while the session object's kind is 1:
// with only downloadable picks the campaign computer places nothing, the same
// list in a skirmish builds.
void campaign_computer_skips_downloadable_types() {
    for (const bool campaign : {true, false}) {
        Fixture f(collector);
        f.spawn(computer, vehicle, 40, 40);
        CHECK(sim::ai::configure_match_computer_players(*f.match, "", build_lists, campaign));
        CHECK(
            sim::ai::match_computer_players(*f.match)->downloadables_restricted ==
            (campaign ? 1 : 0)
        );
        // The relay is capped to none so the collector is the only pick.
        f.run(1, [] { return false; });
        sim::ai::computer_apply_limit(
            sim::ai::match_computer_players(*f.match), computer, "ARMRELAY", 0
        );
        const bool built = f.run(run_ticks, [&] { return f.count(computer, collector) != 0; });
        CHECK(built == !campaign);
    }
}

// A computer player's silo whose build list offers a weapon round, a type whose name holds
// MAKENUKE or MAKEANTI, stockpiles it for its first weapon (BuildWeapon), as the build
// buttons do: it never builds a unit of the missile's type, which has no health to die by.
// The silo is armed, so ai.squad5-factory-tick gives it the structures task.
void computer_silo_stockpiles_its_rounds() {
    data::match_rules::MatchRules rules{};
    rules.ai.squad5_factory_tick.enabled = true;
    Fixture f(0, true, rules);
    auto& launcher = f.spawn(computer, silo, 40, 40);
    CHECK(sim::ai::configure_match_computer_players(*f.match, "", build_lists, false));
    bool missile_built = false;
    const bool stockpiled = f.run(run_ticks, [&] {
        missile_built = missile_built || f.count(computer, missile) != 0 ||
                        f.match->queued_build_count(launcher.unit_index, missile) != 0;
        return launcher.record.weapons[0].stockpile != 0;
    });
    CHECK(!missile_built);
    CHECK(stockpiled);
    CHECK(f.match->fault() == nullptr);
}

// A computer player's mobile builder whose build list offers a weapon round stockpiles it
// for its first weapon, as the silo does: the construction task places no site for it, so
// no unit of the missile's type is ever queued or built.
void computer_builder_stockpiles_its_rounds() {
    Fixture f(0, true, {}, true);
    auto& defence = f.spawn(computer, vehicle, 40, 40);
    CHECK(sim::ai::configure_match_computer_players(*f.match, "", builder_round_lists, false));
    bool missile_built = false;
    const bool stockpiled = f.run(run_ticks, [&] {
        missile_built = missile_built || f.count(computer, missile) != 0 ||
                        f.match->queued_build_count(defence.unit_index, missile) != 0;
        return defence.record.weapons[0].stockpile != 0;
    });
    CHECK(!missile_built);
    CHECK(stockpiled);
    CHECK(f.match->fault() == nullptr);
}

// The ai.* rules at their 3.1c values (every parameter's baseline, the hacks turned on)
// play the same game as no rules; at the values a mod sets they still build, through the
// match's own host (its secondary order queues among the factory tick's queries).
void computer_rules_through_the_match() {
    namespace rules = data::match_rules;
    const auto play = [](const rules::MatchRules& given) {
        Fixture f(0, true, given);
        f.spawn(0, vehicle, 8, 8);
        f.spawn(computer, vehicle, 40, 40);
        CHECK(sim::ai::configure_match_computer_players(*f.match, profile, build_lists, false));
        f.run(run_ticks, [] { return false; });
        std::vector<int64_t> state;
        for (const auto& slot : f.match->world().slots) {
            if (!slot.unit)
                continue;
            state.push_back(slot.record.type_index);
            state.push_back(slot.record.position.x);
            state.push_back(slot.record.position.z);
            state.push_back(slot.record.squad);
        }
        state.push_back(static_cast<int64_t>(f.count(computer, collector)));
        return state;
    };
    rules::MatchRules baseline_on{};
    baseline_on.ai.difficulty_names.enabled = true;
    baseline_on.ai.factory_tick_filter.enabled = true;
    baseline_on.ai.builder_withhold_threshold.enabled = true;
    baseline_on.ai.attack_wave_size.enabled = true;
    baseline_on.ai.patrol_group_size.enabled = true;
    baseline_on.ai.squad_assignment.enabled = true;
    baseline_on.ai.nearest_enemy_filter.enabled = true;
    const auto base = play({});
    CHECK(base.back() != 0);
    CHECK(play(baseline_on) == base);

    rules::MatchRules modded{};
    auto& ai = modded.ai;
    ai.difficulty_names.enabled = true;
    ai.difficulty_names.names = {
        rules::AiDifficultyNamesNames::hard,
        rules::AiDifficultyNamesNames::medium,
        rules::AiDifficultyNamesNames::easy
    };
    ai.squad5_factory_tick.enabled = true;
    ai.factory_tick_filter = {true, true, rules::AiFactoryTickFilterPowerToggle::energy_use_32};
    ai.builder_withhold_threshold = {true, 10};
    ai.commander_keeps_orders_when_damaged.enabled = true;
    ai.attack_wave_size = {true, 10};
    ai.patrol_group_size = {true, 15};
    ai.patrol_null_enemy_skip.enabled = true;
    ai.squad_assignment = {true, rules::AiSquadAssignmentRules::role_squads};
    ai.nearest_enemy_filter = {true, rules::AiNearestEnemyFilterRules::skip_submerged};
    const auto modded_game = play(modded);
    CHECK(modded_game.back() != 0);
    CHECK(play(modded) == modded_game);
}
} // namespace

int main() {
    try {
        computer_builds_from_its_profile_and_lists();
        computer_without_income_skips_the_collector();
        campaign_computer_skips_downloadable_types();
        computer_silo_stockpiles_its_rounds();
        computer_builder_stockpiles_its_rounds();
        computer_rules_through_the_match();
    } catch (const std::exception& error) {
        std::cerr << "computer build test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "computer build tests passed\n";
    return 0;
}
