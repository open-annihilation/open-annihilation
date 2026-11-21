// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Enhanced ground group orders replace an out-of-bounds formation target with
// the original valid click; historical formation points and native patrols stay intact.
#include "group_order_bounds.hpp"

#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/sim/ground_orders/goals.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/script_vm.hpp"
#include "oa/test/check.hpp"
#include "oa/test/match_services.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
namespace input = oa::sim::gameplay_input;
namespace runtime = oa::sim::match_runtime;
namespace ground = oa::sim::ground_orders;

constexpr int32_t pixel = 65536;
constexpr int32_t map_width = 256;
constexpr int32_t map_height = 192;
constexpr uint8_t ground_layer = 1;
constexpr uint8_t land_height = 60;
constexpr uint8_t sea_level = 30;
constexpr uint8_t patrol_kind = 29;
constexpr uint16_t west = 16384;
constexpr uint32_t battle_version = oa::data::enhanced_mode::battle_flow_version;

oa::FixedVec3 point(int32_t x, int32_t z, int32_t y = 17) {
    return {x * pixel, y * pixel, z * pixel};
}

bool same(const oa::FixedVec3& left, const oa::FixedVec3& right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

oa::Unit actor(uint8_t footprint_x, uint8_t footprint_z) {
    oa::Unit unit{};
    unit.type_index = 1;
    unit.movement = 1;
    unit.flags = OA_UNIT_FLAG_LIVE | ground_layer;
    unit.footprint_x = footprint_x;
    unit.footprint_z = footprint_z;
    return unit;
}

oa::FixedVec3 bounded(
    const oa::FixedVec3& formation,
    const oa::FixedVec3& click,
    const oa::Unit& unit,
    bool enabled = true
) {
    return oa::app::bounded_group_order_point(
        formation, click, unit, map_width, map_height, enabled
    );
}

void fixed_footprint_boundaries_keep_valid_formation_points() {
    for (const auto footprint : {std::array<uint8_t, 2>{1, 1}, {2, 2}, {4, 4}, {4, 2}, {2, 4}}) {
        const auto unit = actor(footprint[0], footprint[1]);
        const auto click = point(128, 96, 23);
        const int32_t lower_x = 8 * (footprint[0] - 1) * pixel;
        const int32_t lower_z = 8 * (footprint[1] - 1) * pixel;
        const int32_t upper_x = map_width * pixel - lower_x;
        const int32_t upper_z = map_height * pixel - lower_z;
        for (const auto formation :
             {oa::FixedVec3{lower_x, -12345, lower_z},
              oa::FixedVec3{upper_x - 1, 12345, upper_z - 1}})
            OA_CHECK(same(bounded(formation, click, unit), formation));
        for (const auto formation :
             {oa::FixedVec3{lower_x - 1, -12345, click.z},
              oa::FixedVec3{upper_x, 12345, click.z},
              oa::FixedVec3{click.x, -12345, lower_z - 1},
              oa::FixedVec3{click.x, 12345, upper_z}})
            OA_CHECK(same(bounded(formation, click, unit), click));
    }
    const auto unit = actor(2, 2);
    const auto click = point(128, 96);
    const auto last_valid = oa::FixedVec3{10232 * pixel - 1, 12345, 96 * pixel};
    const auto first_invalid = oa::FixedVec3{10232 * pixel, -12345, 96 * pixel};
    OA_CHECK(same(
        oa::app::bounded_group_order_point(last_valid, click, unit, 10240, map_height, true),
        last_valid
    ));
    OA_CHECK(same(
        oa::app::bounded_group_order_point(first_invalid, click, unit, 10240, map_height, true),
        click
    ));
    // A partial last plot supplies no additional complete footprint cell.
    const auto partial_plot = point(248, 96);
    OA_CHECK(same(
        oa::app::bounded_group_order_point(partial_plot, click, unit, 271, map_height, true), click
    ));
}

void disabled_profiles_and_ineligible_actors_keep_every_point_bit() {
    const auto click = point(128, 96, 23);
    const auto unit = actor(2, 2);
    const std::array<oa::FixedVec3, 3> formations{
        point(-8, 128, -23),
        point(248, 128, 51),
        oa::FixedVec3{
            std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int32_t>::max(),
            std::numeric_limits<int32_t>::min()
        }
    };
    for (uint32_t version = 0; version <= battle_version; ++version)
        for (const auto& formation : formations) {
            const auto result = bounded(formation, click, unit, version == battle_version);
            OA_CHECK(same(result, version == battle_version ? click : formation));
        }
    for (uint32_t excluded = 0; excluded < 5; ++excluded) {
        auto ineligible = unit;
        switch (excluded) {
        case 0:
            ineligible.flags = (ineligible.flags & ~OA_UNIT_FLAG_OCCUPANCY_MASK) | 2;
            break;
        case 1:
            ineligible.flags &= ~OA_UNIT_FLAG_OCCUPANCY_MASK;
            break;
        case 2:
            ineligible.flags = (ineligible.flags & ~OA_UNIT_FLAG_OCCUPANCY_MASK) | 3;
            break;
        case 3:
            ineligible.attach_parent = 1;
            break;
        default:
            ineligible.footprint_x = 0;
            break;
        }
        OA_CHECK(same(bounded(formations[0], click, ineligible), formations[0]));
    }
}

void an_invalid_click_never_rewrites_an_invalid_formation() {
    const auto unit = actor(2, 2);
    const auto formation = point(-8, 128, -23);
    for (const auto click : {point(-8, 128), point(0, 128), point(248, 128), point(128, 184)})
        OA_CHECK(same(bounded(formation, click, unit), formation));
    for (const int32_t dimension : {0, -16, 16})
        OA_CHECK(same(
            oa::app::bounded_group_order_point(
                formation, point(128, 96), unit, dimension, map_height, true
            ),
            formation
        ));
}

struct Scene {
    oa::formats::tnt::Map map;
    std::array<oa::sim::visibility_state::TerrainCell, 256> terrain{};
    std::array<oa::sim::spatial_state::Plot, 256> plots{};
    std::array<oa::sim::visibility_state::SightMask, 1> sight_masks{};
    std::array<oa::sim::visibility_state::AltitudeCell, 64> altitude{};
    std::array<oa::sim::visibility_state::AltitudeSightPattern, 6> altitude_patterns{};
    std::shared_ptr<oa::formats::objects3d::Model> model =
        std::make_shared<oa::formats::objects3d::Model>();
    std::shared_ptr<oa::formats::cob::CobProgram> script =
        std::make_shared<oa::formats::cob::CobProgram>();
    std::array<oa::sim::unit_spawn::LoadedType, 2> loaded{};
    std::array<oa::sim::unit_spawn::Type, 2> types{};
    oa::data::unit_definitions::UnitDefinition definition{};
    oa::data::unit_definitions::RuntimeDefinitionMetadata metadata{};
    oa::data::unit_definitions::UnitTargetCategoryMasks target_masks{};
    std::array<runtime::RuntimeTypeFields, 2> fields{};
    oa::sim::combat_state::WeaponRegistry weapons;
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    oa::test::QuietServices services;
    oa::test::EmptyScenario scenario;
    std::unique_ptr<runtime::Match> game;
    std::array<uint16_t, 2> actors{};

    explicit Scene(uint32_t version) {
        map.attribute_width = map.attribute_height = 16;
        map.attributes.resize(256);
        map.sea_level = sea_level;
        for (size_t index = 0; index < map.attributes.size(); ++index) {
            map.attributes[index].height = land_height;
            plots[index].low_height = plots[index].high_height = land_height;
        }
        sight_masks[0].width = sight_masks[0].height = 1;
        sight_masks[0].pixels = {1};
        model->objects.resize(1);
        model->objects[0].name = "root";
        model->objects[0].vertices = {{0, 0, 0}, {0, 20 * pixel, 0}};
        script->code = {oa::sim::script_vm::opcode::return_};
        script->scripts = {{"Create", 0}};
        script->entry_points = {0};
        script->piece_names = {"root"};
        types[1].simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
        types[1].simulation.maximum_health = 1000;
        types[1].footprint_x = types[1].footprint_z = 2;
        types[1].bm_code = 1;
        types[1].model = reinterpret_cast<uintptr_t>(model.get());
        types[1].cob = reinterpret_cast<uintptr_t>(script.get());
        loaded[1].model = model;
        loaded[1].script = script;
        loaded[1].type = types[1];
        loaded[1].unit_name = "PATROLLER";
        definition.can_move = definition.can_patrol = true;
        definition.acceleration_fixed = definition.brake_rate_fixed = pixel;
        definition.max_velocity_fixed = 2 * pixel;
        definition.turn_rate = 1024;
        metadata.footprint_x = metadata.footprint_z = 2;
        metadata.min_water_depth = -10000;
        metadata.max_water_depth = 0;
        metadata.max_slope = metadata.bad_slope = 255;
        metadata.max_water_slope = metadata.bad_water_slope = 255;
        fields[1].definition = &definition;
        fields[1].runtime_metadata = &metadata;
        fields[1].target_masks = &target_masks;
        fields[1].movement_class = 1;
        fields[1].yard_mask = yard;
        runtime::OfflineInputs inputs{
            map,
            loaded,
            types,
            fields,
            weapons,
            terrain,
            sight_masks,
            8,
            8,
            4,
            2,
            0,
            sea_level,
            1,
            &scenario,
            {},
            plots,
            oa::sim::visibility_state::AltitudeSightData{8, 8, altitude, altitude_patterns}
        };
        oa::data::enhanced_mode::apply_alias(
            inputs.rules.setup.enhanced_mode, static_cast<uint32_t>(version)
        );
        inputs.unit_player_mask = 3;
        game = std::make_unique<runtime::Match>(inputs, services);
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
        for (size_t index = 0; index < actors.size(); ++index) {
            const auto x = index == 0 ? 48 : 96;
            auto* slot = match().create(
                {0,
                 1,
                 {static_cast<uint32_t>(x * pixel), land_height * pixel, 128 * pixel},
                 true,
                 ground_layer,
                 0}
            );
            if (!slot || !slot->unit)
                throw std::runtime_error("cannot create the native group patroller");
            actors[index] = slot->unit_index;
            match().place_unit_at(
                slot->unit_index, x * pixel, land_height * pixel, 128 * pixel, ground_layer
            );
            slot->record.heading = west;
            slot->record.flags = (slot->record.flags &
                                  ~(OA_UNIT_FLAG_MOVE_ORDER_MASK | OA_UNIT_FLAG_FIRE_ORDER_MASK)) |
                                 (1u << OA_UNIT_FLAG_MOVE_ORDER_SHIFT) |
                                 (2u << OA_UNIT_FLAG_FIRE_ORDER_SHIFT) | OA_UNIT_FLAG_SELECTED;
        }
    }

    runtime::Match& match() { return *game; }

    void run(uint32_t ticks) {
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            ++match().simulation().tick;
            match().tick();
            if (match().fault() != nullptr)
                throw std::runtime_error(match().fault());
        }
    }

    void check_footprints() {
        for (const auto id : actors) {
            const auto& slot = match().spawn_slot(id);
            const auto* native = match().ground_runtime(id);
            OA_CHECK(native != nullptr);
            if (!native)
                return;
            OA_CHECK(
                native->geometry.cell ==
                (std::array<int16_t, 2>{slot.record.cell_x, slot.record.cell_z})
            );
            for (int16_t z = 0; z < 2; ++z)
                for (int16_t x = 0; x < 2; ++x) {
                    const auto px = native->geometry.cell[0] + x;
                    const auto pz = native->geometry.cell[1] + z;
                    OA_CHECK(px >= 0 && pz >= 0 && px < 16 && pz < 16);
                    if (px < 0 || pz < 0 || px >= 16 || pz >= 16)
                        continue;
                    OA_CHECK(match().spatial().plots[size_t(pz) * 16 + size_t(px)].ground == id);
                }
        }
    }
};

void actual_group_formation_keeps_valid_offsets_and_runs_native_patrols() {
    for (const auto version : {4u, battle_version}) {
        Scene scene(version);
        auto& match = scene.match();
        const auto centre = input::selection_centre(match.state(), nullptr);
        OA_CHECK(centre.units == 2 && centre.sum_x == 144 && centre.sum_z == 256);
        const auto click = point(16, 128, land_height);
        std::array<oa::FixedVec3, 2> sent{};
        for (size_t index = 0; index < scene.actors.size(); ++index) {
            const auto id = scene.actors[index];
            const auto& unit = match.spawn_slot(id).record;
            const auto formation = input::group_order_point(centre, unit, click);
            OA_CHECK(formation.x == (index == 0 ? -8 : 40) * pixel);
            sent[index] = oa::app::bounded_group_order_point(
                formation, click, unit, 256, 256, version == battle_version
            );
            OA_CHECK(
                sent[index].x == (index == 0 && version == battle_version ? 16
                                  : index == 0                            ? -8
                                                                          : 40) *
                                     pixel
            );
            OA_CHECK(sent[index].y == click.y && sent[index].z == click.z);
            OA_CHECK(
                match.issue_patrol(id, {sent[index].x, sent[index].y, sent[index].z}, false).kind ==
                patrol_kind
            );
        }
        scene.run(2);
        const auto* first = match.ground_runtime(scene.actors[0]);
        const auto* second = match.ground_runtime(scene.actors[1]);
        OA_CHECK(first && second && first->navigation.goal && second->navigation.goal);
        if (!first || !second || !first->navigation.goal || !second->navigation.goal)
            return;
        OA_CHECK(first->navigation.goal->cell[0] == (version == battle_version ? 0 : -1));
        OA_CHECK(second->navigation.goal->cell[0] == 2);
        bool reached_click_cell{}, returned_to_first_start{};
        for (uint32_t tick = 0; tick < 300; ++tick) {
            scene.run(1);
            scene.check_footprints();
            if (first->geometry.cell[0] == 0)
                reached_click_cell = true;
            if (version == battle_version && reached_click_cell && first->navigation.goal &&
                first->navigation.goal->cell[0] == 2)
                returned_to_first_start = true;
        }
        if (version == battle_version)
            OA_CHECK(reached_click_cell && returned_to_first_start);
        for (const auto id : scene.actors) {
            OA_CHECK(
                match.issue_patrol(id, {208 * pixel, land_height * pixel, 128 * pixel}, false)
                    .kind == patrol_kind
            );
        }
        scene.run(2);
        for (const auto id : scene.actors) {
            const auto* state = match.ground_runtime(id);
            OA_CHECK(state && state->navigation.goal);
            if (!state || !state->navigation.goal)
                return;
            OA_CHECK(state->navigation.goal->cell == (std::array<int16_t, 2>{12, 7}));
            match.stop_orders(id);
            OA_CHECK(state->navigation.goal == nullptr);
        }
    }
}

} // namespace

int main() {
    fixed_footprint_boundaries_keep_valid_formation_points();
    disabled_profiles_and_ineligible_actors_keep_every_point_bit();
    an_invalid_click_never_rewrites_an_invalid_formation();
    try {
        actual_group_formation_keeps_valid_offsets_and_runs_native_patrols();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "actual group patrol: %s\n", error.what());
        OA_CHECK(false);
    }
    return oa::test::check_exit_status();
}
