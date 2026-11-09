// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Enhanced ground group orders replace an out-of-bounds formation target with
// the original valid click; historical formation points and native patrols stay intact.
#include "group_order_bounds.hpp"

#include "combat_fixture.hpp"
#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/sim/ground_orders/goals.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <memory>

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
    combat_fixture::Fixture fixture;
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    std::array<uint16_t, 2> actors{};

    explicit Scene(uint32_t version) {
        fixture.match.reset();
        fixture.map.sea_level = sea_level;
        for (size_t index = 0; index < fixture.map.attributes.size(); ++index) {
            fixture.map.attributes[index].height = land_height;
            fixture.collision_plots[index].low_height = land_height;
            fixture.collision_plots[index].high_height = land_height;
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
        runtime::OfflineInputs inputs{
            fixture.map,
            fixture.loaded,
            fixture.types,
            fixture.fields,
            fixture.weapons,
            fixture.terrain_values,
            fixture.masks,
            8,
            8,
            4,
            2,
            0,
            sea_level,
            1,
            &fixture.scenario,
            {},
            fixture.collision_plots,
            oa::sim::visibility_state::AltitudeSightData{
                8, 8, fixture.altitude_cells, fixture.altitude_patterns
            }
        };
        inputs.rules.setup.enhanced_mode.enabled = version != 0;
        inputs.rules.setup.enhanced_mode.version = version;
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
        for (size_t index = 0; index < actors.size(); ++index) {
            const auto x = index == 0 ? 48 : 96;
            auto& slot = fixture.spawn(0, x, 128);
            actors[index] = slot.unit_index;
            match().place_unit_at(
                slot.unit_index, x * pixel, land_height * pixel, 128 * pixel, ground_layer
            );
            slot.record.heading = west;
            slot.record.flags |= OA_UNIT_FLAG_SELECTED;
        }
    }

    runtime::Match& match() { return *fixture.match; }

    void check_footprints() {
        for (const auto id : actors) {
            const auto& slot = match().spawn_slot(id);
            const auto* native = match().ground_runtime(id);
            CHECK(native != nullptr);
            CHECK(
                native->geometry.cell ==
                (std::array<int16_t, 2>{slot.record.cell_x, slot.record.cell_z})
            );
            for (int16_t z = 0; z < 2; ++z)
                for (int16_t x = 0; x < 2; ++x) {
                    const auto px = native->geometry.cell[0] + x;
                    const auto pz = native->geometry.cell[1] + z;
                    CHECK(px >= 0 && pz >= 0 && px < 16 && pz < 16);
                    CHECK(match().spatial().plots[size_t(pz) * 16 + size_t(px)].ground == id);
                }
        }
    }
};

void actual_group_formation_keeps_valid_offsets_and_runs_native_patrols() {
    for (const auto version : {4u, battle_version}) {
        Scene scene(version);
        auto& match = scene.match();
        const auto centre = input::selection_centre(match.state(), nullptr);
        CHECK(centre.units == 2 && centre.sum_x == 144 && centre.sum_z == 256);
        const auto click = point(16, 128, land_height);
        std::array<oa::FixedVec3, 2> sent{};
        for (size_t index = 0; index < scene.actors.size(); ++index) {
            const auto id = scene.actors[index];
            const auto& unit = match.spawn_slot(id).record;
            const auto formation = input::group_order_point(centre, unit, click);
            CHECK(formation.x == (index == 0 ? -8 : 40) * pixel);
            sent[index] = oa::app::bounded_group_order_point(
                formation, click, unit, 256, 256, version == battle_version
            );
            CHECK(
                sent[index].x == (index == 0 && version == battle_version ? 16
                                  : index == 0                            ? -8
                                                                          : 40) *
                                     pixel
            );
            CHECK(sent[index].y == click.y && sent[index].z == click.z);
            CHECK(
                match.issue_patrol(id, {sent[index].x, sent[index].y, sent[index].z}, false).kind ==
                patrol_kind
            );
        }
        scene.fixture.run(2);
        const auto* first = match.ground_runtime(scene.actors[0]);
        const auto* second = match.ground_runtime(scene.actors[1]);
        CHECK(first && second && first->navigation.goal && second->navigation.goal);
        CHECK(first->navigation.goal->cell[0] == (version == battle_version ? 0 : -1));
        CHECK(second->navigation.goal->cell[0] == 2);
        bool reached_click_cell{}, returned_to_first_start{};
        for (uint32_t tick = 0; tick < 300; ++tick) {
            scene.fixture.run(1);
            scene.check_footprints();
            if (first->geometry.cell[0] == 0)
                reached_click_cell = true;
            if (version == battle_version && reached_click_cell && first->navigation.goal &&
                first->navigation.goal->cell[0] == 2)
                returned_to_first_start = true;
        }
        if (version == battle_version)
            CHECK(reached_click_cell && returned_to_first_start);
        for (const auto id : scene.actors) {
            CHECK(
                match.issue_patrol(id, {208 * pixel, land_height * pixel, 128 * pixel}, false)
                    .kind == patrol_kind
            );
        }
        scene.fixture.run(2);
        for (const auto id : scene.actors) {
            const auto* state = match.ground_runtime(id);
            CHECK(state && state->navigation.goal);
            CHECK(state->navigation.goal->cell == (std::array<int16_t, 2>{12, 7}));
            match.stop_orders(id);
            CHECK(state->navigation.goal == nullptr);
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
