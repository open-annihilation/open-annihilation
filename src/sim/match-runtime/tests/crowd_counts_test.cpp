// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The crowds a player's shared routes go round, with Revision 1: each
// player's own live ground units at a coarse node. Aircraft have movement
// state too, but stand in no ground route, so they are not counted, whether
// flying or landed. A match with Enhanced Mode Off counts no crowds.
#include "combat_fixture.hpp"
#include "support/match_tick_access.hpp"

#include "oa/data/enhanced_mode.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>

namespace {
using namespace combat_fixture;
namespace runtime = oa::sim::match_runtime;
namespace enhanced = oa::data::enhanced_mode;
using FeatureSet = enhanced::FeatureSet;

constexpr int32_t pixel = 65536;
constexpr uint32_t map_width = 64, map_height = 64;
constexpr uint8_t land_height = 60;
constexpr uint8_t sea_level = 30;
constexpr uint8_t ground_layer = 1, air_layer = 2;

/// Three units of one type for player 0 and one for player 1 on flat land,
/// the type able to fly or not.
struct Scene {
    Fixture fixture;
    std::array<uint8_t, 4> yard{4, 4, 4, 4};
    std::array<uint16_t, 4> units{};

    /// Builds the land and places the units: player 0's three in the cell
    /// (20, 20), at coarse node (10, 10), and player 1's in the cell (40, 40).
    ///
    /// @param features the match's set
    /// @param flying whether the units' type can fly
    /// @param layer the occupancy layer the units are placed on
    Scene(const FeatureSet& features, bool flying, uint8_t layer) {
        fixture.match.reset();
        fixture.map.attribute_width = map_width;
        fixture.map.attribute_height = map_height;
        fixture.map.sea_level = sea_level;
        fixture.map.attributes.resize(size_t(map_width) * map_height);
        fixture.terrain_values.resize(size_t(map_width) * map_height);
        fixture.collision_plots.resize(size_t(map_width) * map_height);
        fixture.altitude_cells.resize(size_t(map_width / 2) * (map_height / 2));
        for (uint32_t cell = 0; cell < map_width * map_height; ++cell) {
            fixture.map.attributes[cell].height = land_height;
            fixture.collision_plots[cell].low_height = land_height;
            fixture.collision_plots[cell].high_height = land_height;
        }
        if (flying)
            fixture.types[1].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_FLY;
        fixture.loaded[1].type = fixture.types[1];
        fixture.fields[1].yard_mask = yard;
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
        for (uint8_t owner = 0; owner < 2; ++owner) {
            match().simulation().players[owner].present = true;
            match().simulation().players[owner].status = 1;
        }
        for (size_t i = 0; i < units.size(); ++i) {
            const uint8_t owner = i < 3 ? 0 : 1;
            const uint32_t at = owner == 0 ? 20 * 16 + 8 : 40 * 16 + 8;
            auto& unit = fixture.spawn(owner, at, at);
            units[i] = unit.unit_index;
            match().place_unit_at(
                units[i], int32_t(at) * pixel, land_height * pixel, int32_t(at) * pixel, layer
            );
        }
        CHECK(match().fault() == nullptr);
        CHECK(match().enhanced_features() == features);
    }

    runtime::Match& match() { return *fixture.match; }

    /// Returns a player's units counted at a coarse node.
    uint32_t crowd(uint8_t player, uint32_t column, uint32_t row) {
        return runtime::MatchTickAccess::crowd_units(match(), player, column, row);
    }
};

/// Counts ground units at their node, each player in its own grid.
void ground_units_are_counted() {
    Scene scene(enhanced::revision_features(1), false, ground_layer);
    for (const auto unit : scene.units)
        CHECK(scene.match().ground_runtime(unit) != nullptr);
    CHECK(scene.match().state().units[scene.units[0]].cell_x == 20);
    CHECK(scene.match().state().units[scene.units[0]].cell_z == 20);
    // The node of cell (20, 20) is ((20 + 1) / 2, (20 + 1) / 2).
    CHECK(scene.crowd(0, 10, 10) == 3);
    CHECK(scene.crowd(1, 10, 10) == 0);
    CHECK(scene.crowd(1, 20, 20) == 1);
    CHECK(scene.crowd(0, 20, 20) == 0);
    CHECK(scene.crowd(0, 11, 10) == 0);
}

/// Leaves aircraft out, flying or landed, though each has movement state.
void aircraft_are_not_counted() {
    for (const auto layer : {air_layer, ground_layer}) {
        Scene scene(enhanced::revision_features(1), true, layer);
        for (const auto unit : scene.units)
            CHECK(scene.match().ground_runtime(unit) != nullptr);
        CHECK(scene.match().state().units[scene.units[0]].cell_x == 20);
        CHECK(scene.crowd(0, 10, 10) == 0);
        CHECK(scene.crowd(1, 20, 20) == 0);
    }
}

/// Counts nothing with Enhanced Mode Off.
void off_counts_no_crowds() {
    Scene scene(FeatureSet{}, false, ground_layer);
    CHECK(scene.crowd(0, 10, 10) == 0);
}

} // namespace

int main() {
    try {
        ground_units_are_counted();
        aircraft_are_not_counted();
        off_counts_no_crowds();
        std::cout << "Crowd counts: ground units counted, aircraft not\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
