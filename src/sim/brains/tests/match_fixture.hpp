// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The computer-build fixture, copied for a brain seated on its computer player.
#pragma once

#include "oa/sim/ai.hpp"
#include "oa/sim/match_runtime.hpp"

#include <array>
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
    /// Yard of a building that claims its footprint, so a vehicle cannot path through it.
    std::array<uint8_t, 4> blocking_yard{0x2f, 0x2f, 0x2f, 0x2f};
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    sim::combat_state::WeaponRegistry weapons;
    std::vector<sim::spatial_state::Plot> plots;
    Services services;
    Scenario scenario;
    std::unique_ptr<sim::match_runtime::Match> match;

    /// Builds the computer-build match, with the local player and one computer.
    ///
    /// @param downloadable_type a type marked downloadable, or 0 for none
    /// @param builder_income whether the vehicle makes energy and metal
    /// @param rules the match rules
    /// @param movement_class a movement class for the vehicle, or 0 for none.
    ///     Non-zero raises the map onto dry land and gives the buildings a
    ///     blocking yard, so a ground move can be searched. Zero leaves the
    ///     match the seat, game and side tests build.
    explicit Fixture(
        uint32_t downloadable_type = 0,
        bool builder_income = true,
        const data::match_rules::MatchRules& rules = {},
        uint32_t movement_class = 0
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
        if (movement_class != 0) {
            map.sea_level = 20;
            for (auto& attribute : map.attributes)
                attribute.height = 40;
            for (auto& plot : plots)
                plot.low_height = plot.high_height = 40;
            fields[vehicle].movement_class = movement_class;
            fields[collector].yard_mask = blocking_yard;
            fields[relay].yard_mask = blocking_yard;
            fields[silo].yard_mask = blocking_yard;
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

    /// Places one unit and returns its slot.
    ///
    /// @param player the owner
    /// @param type the unit type
    /// @param cell_x the map cell across
    /// @param cell_z the map cell down
    /// @return the new slot
    sim::unit_spawn::Slot& spawn(uint8_t player, Type type, int32_t cell_x, int32_t cell_z) {
        const auto at = [](int32_t cell) { return static_cast<uint32_t>(cell * 16 + 8) << 16; };
        auto* slot = match->create({player, type, {at(cell_x), 0, at(cell_z)}, true, 1, 0});
        CHECK(slot && slot->unit);
        slot->unit->object_present = true;
        return *slot;
    }

    /// Ticks until `done` holds or the run ends.
    ///
    /// @param ticks how many ticks to try
    /// @param done returns true when the run can stop
    /// @return true when `done` held
    bool run(uint32_t ticks, const std::function<bool()>& done) {
        for (uint32_t i = 0; i < ticks; ++i) {
            ++match->state().game.tick;
            match->tick();
            if (done())
                return true;
        }
        return false;
    }

    /// Counts one player's units of one type.
    ///
    /// @param owner the player
    /// @param type the unit type
    /// @return how many of that type the player has
    size_t count(uint8_t owner, Type type) const {
        size_t n = 0;
        for (const auto& slot : match->world().slots)
            n += slot.unit && slot.record.type_index == type && slot.record.owner_index == owner;
        return n;
    }
};

} // namespace
