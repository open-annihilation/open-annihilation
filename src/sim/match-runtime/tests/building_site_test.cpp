// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/match_runtime.hpp"
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include "oa/test/match_services.hpp"
using namespace oa;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (false)

using Services = oa::test::StrictServices;

using Scenario = oa::test::EmptyScenario;

int main() {
    formats::tnt::Map map;
    // 16 cells wide and 32 tall, so the sites stay clear of the hidden edges
    // the map load voids: columns 14-15 and the bottom band.
    map.attribute_width = 16;
    map.attribute_height = 32;
    map.sea_level = 50;
    map.attributes.resize(512);
    std::vector<sim::visibility_state::TerrainCell> terrain_values(512);
    sim::visibility_state::SightMask mask;
    mask.width = mask.height = 1;
    mask.transparent = 0;
    mask.pixels = {1};
    const std::array masks{mask};
    auto model = std::make_shared<formats::objects3d::Model>();
    model->objects.resize(1);
    model->objects[0].name = "root";
    std::array<sim::unit_spawn::LoadedType, 4> loaded;
    loaded[1].model = model;
    loaded[2].model = model;
    loaded[3].model = model;
    std::array<sim::unit_spawn::Type, 4> types;
    types[1].footprint_x = types[1].footprint_z = 3;
    types[1].bm_code = 0;
    loaded[1].type = types[1];
    types[2].footprint_x = types[2].footprint_z = 2;
    types[2].bm_code = 1;
    loaded[2].type = types[2];
    types[3].footprint_x = types[3].footprint_z = 5;
    types[3].bm_code = 0;
    loaded[3].type = types[3];
    data::unit_definitions::UnitDefinition def;
    def.waterline = 0;
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    data::unit_definitions::RuntimeDefinitionMetadata metadata;
    metadata.footprint_x = metadata.footprint_z = 3;
    metadata.max_slope = 10;
    metadata.max_water_depth = 0;
    metadata.min_water_depth = -10000;
    std::vector<uint8_t> mex_yard(9, 0x2f);
    std::vector<uint8_t> geo_yard(9, 0x8f);
    std::vector<uint8_t> tide_yard(9, 0x37);
    metadata.yard_cells = mex_yard;
    std::vector<sim::spatial_state::Plot> collision_plots(512);
    for (auto& plot : collision_plots) {
        plot.low_height = 80;
        plot.high_height = 80;
    }
    data::unit_definitions::RuntimeDefinitionMetadata kbot_meta;
    kbot_meta.footprint_x = kbot_meta.footprint_z = 2;
    kbot_meta.max_slope = 17;
    kbot_meta.max_water_depth = 12;
    kbot_meta.min_water_depth = -10000;
    // ARMSOLAR: 5x5 'o' yard, MaxSlope=10, MaxWaterDepth=0.
    data::unit_definitions::RuntimeDefinitionMetadata solar_meta = metadata;
    solar_meta.footprint_x = solar_meta.footprint_z = 5;
    std::vector<uint8_t> solar_yard(25, 0x2f);
    solar_meta.yard_cells = solar_yard;
    std::array<sim::match_runtime::RuntimeTypeFields, 4> fields{};
    fields[1].definition = &def;
    fields[1].yard_mask = metadata.yard_cells;
    fields[1].runtime_metadata = &metadata;
    fields[1].target_masks = &target_masks;
    fields[2].runtime_metadata = &kbot_meta;
    fields[2].target_masks = &target_masks;
    fields[3].definition = &def;
    fields[3].yard_mask = solar_meta.yard_cells;
    fields[3].runtime_metadata = &solar_meta;
    fields[3].target_masks = &target_masks;
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    sim::match_runtime::OfflineInputs input{
        map,
        loaded,
        types,
        fields,
        weapons,
        terrain_values,
        masks,
        8,
        16,
        2,
        2,
        0,
        30,
        1,
        &scenario,
        {},
        collision_plots,
        {}
    };
    sim::match_runtime::Match match(input, services);
    // The launch rebuilds the sight grids.
    match.reset_sight_buffers(true);

    const auto set_site = [&](int32_t x0, int32_t z0, int32_t size, uint8_t low, uint8_t high) {
        for (int32_t row = 0; row < size; ++row)
            for (int32_t column = 0; column < size; ++column) {
                auto& plot = match.spatial().plots
                                 [static_cast<std::size_t>(z0 + row) * 16 +
                                  static_cast<std::size_t>(x0 + column)];
                plot.low_height = low;
                plot.high_height = high;
            }
    };

    // ARMMEX on flat metal-free ground: the site test has no metal test and the
    // building stands on the lowest level plot.
    CHECK(match.building_site(1, 2, 2, 0) == 80);
    CHECK(match.building_site_clear(1, 2, 2, 0));
    // On a metal patch: the plot metal is only summed, never tested.
    for (int32_t row = 0; row < 3; ++row)
        for (int32_t column = 0; column < 3; ++column) {
            auto& plot =
                match.spatial().plots
                    [static_cast<std::size_t>(2 + row) * 16 + static_cast<std::size_t>(2 + column)];
            plot.metal = 86;
            plot.metal_feature = true;
        }
    CHECK(match.building_site(1, 2, 2, 0) == 80);
    for (int32_t row = 0; row < 3; ++row)
        for (int32_t column = 0; column < 3; ++column) {
            auto& plot =
                match.spatial().plots
                    [static_cast<std::size_t>(2 + row) * 16 + static_cast<std::size_t>(2 + column)];
            plot.metal = 0;
            plot.metal_feature = false;
        }
    // A slope of exactly MaxSlope stands, at the lowest plot; one more refuses.
    match.spatial().plots[3 * 16 + 3].low_height = 70;
    CHECK(match.building_site(1, 2, 2, 0) == 70);
    match.spatial().plots[3 * 16 + 3].low_height = 69;
    CHECK(!match.building_site(1, 2, 2, 0));
    // footprint_height still gives the refused site its lowest level plot.
    CHECK(match.footprint_height(1, 2, 2) == 69);
    match.spatial().plots[3 * 16 + 3].low_height = 80;
    // MaxWaterDepth=0: ground at sea level stands, a plot below it refuses.
    set_site(2, 2, 3, 50, 50);
    CHECK(match.building_site(1, 2, 2, 0) == 50);
    match.spatial().plots[4 * 16 + 4].low_height = 49;
    CHECK(!match.building_site(1, 2, 2, 0));
    set_site(2, 2, 3, 80, 80);
    // ARMSOLAR on water refuses; on land it stands.
    CHECK(match.building_site(3, 8, 8, 0) == 80);
    set_site(8, 8, 5, 20, 20);
    CHECK(!match.building_site(3, 8, 8, 0));
    CHECK(match.footprint_height(3, 8, 8) == 20);
    set_site(8, 8, 5, 80, 80);
    // An indestructible feature under a plain 'o' cell does not refuse (bit 6
    // is only in 'f' cells); a blocking one does.
    match.spatial().plots[8 * 16 + 8].indestructible_feature = true;
    CHECK(match.building_site(3, 8, 8, 0));
    match.spatial().plots[8 * 16 + 8].blocking_feature = true;
    CHECK(!match.building_site(3, 8, 8, 0));
    match.spatial().plots[8 * 16 + 8].indestructible_feature = false;
    match.spatial().plots[8 * 16 + 8].blocking_feature = false;

    CHECK(match.building_site_clear(1, 2, 2, 0));
    CHECK(!match.building_site_clear(1, 0, 2, 0));
    CHECK(!match.building_site_clear(1, 2, 0, 0));
    CHECK(!match.building_site_clear(1, 13, 2, 0));
    // Cells left of and above the map refuse too; 3.1c gives the same answers
    // on these plots for every site in this test.
    CHECK(!match.building_site_clear(1, -1, 2, 0));
    CHECK(!match.building_site_clear(1, 2, -1, 0));
    // A claimed plot alone refuses the simulation's own test.
    match.spatial().plots[2 * 16 + 3].flags |= 2;
    CHECK(!match.building_site_clear(1, 2, 2, 0));
    match.spatial().plots[2 * 16 + 3].flags &= static_cast<uint8_t>(~2u);
    CHECK(match.building_site_clear(1, 2, 2, 0));

    match.spatial().plots[2 * 16 + 3].high_height = 100;
    CHECK(!match.building_site_clear(1, 2, 2, 0));
    match.spatial().plots[2 * 16 + 3].high_height = 80;

    match.spatial().plots[2 * 16 + 2].low_height = 40;
    match.spatial().plots[2 * 16 + 2].high_height = 40;
    CHECK(!match.building_site_clear(1, 2, 2, 0));
    match.spatial().plots[2 * 16 + 2].low_height = 80;
    match.spatial().plots[2 * 16 + 2].high_height = 80;

    match.spatial().plots[2 * 16 + 2].blocking_feature = true;
    CHECK(!match.building_site_clear(1, 2, 2, 0));
    match.spatial().plots[2 * 16 + 2].blocking_feature = false;

    match.spatial().plots[2 * 16 + 2].ground = 1;
    CHECK(!match.building_site_clear(1, 2, 2, 0));
    CHECK(match.building_site_clear(1, 2, 2, 1));
    match.spatial().plots[2 * 16 + 2].ground = 0;

    metadata.yard_cells = geo_yard;
    fields[1].yard_mask = metadata.yard_cells;
    CHECK(!match.building_site_clear(1, 2, 2, 0));
    match.spatial().plots[3 * 16 + 3].geo_feature = true;
    CHECK(match.building_site_clear(1, 2, 2, 0));
    match.spatial().plots[3 * 16 + 3].geo_feature = false;
    match.spatial().plots[3 * 16 + 3].metal = 86;
    match.spatial().plots[3 * 16 + 3].metal_feature = true;
    CHECK(!match.building_site_clear(1, 2, 2, 0));
    match.spatial().plots[3 * 16 + 3].metal = 0;
    match.spatial().plots[3 * 16 + 3].metal_feature = false;

    metadata.yard_cells = tide_yard;
    fields[1].yard_mask = metadata.yard_cells;
    metadata.min_water_depth = 20;
    metadata.max_water_depth = 10000;
    CHECK(!match.building_site_clear(1, 2, 2, 0));
    for (int32_t row = 0; row < 3; ++row)
        for (int32_t column = 0; column < 3; ++column) {
            auto& plot =
                match.spatial().plots
                    [static_cast<std::size_t>(2 + row) * 16 + static_cast<std::size_t>(2 + column)];
            plot.low_height = 20;
            plot.high_height = 20;
        }
    CHECK(match.building_site(1, 2, 2, 0) == 50);
    // A yard without level cells floats at sea level less its waterline,
    // wrapped to a byte: waterline 60 over sea level 50 stands at 246, so the
    // 'w' cells' plots (20) stay below it.
    def.waterline = 60;
    metadata.min_water_depth = -10000;
    CHECK(match.building_site(1, 2, 2, 0) == 246);
    // A building a mission places, or the unit-name cheat creates, snaps to
    // the footprint's centre over the site, (3 + 2 * 2) * 8 = 56, and the 'w'
    // yard floats its waterline below the sea, as a shipyard (WaterLine=22)
    // does, whatever height the point had. A mobile unit keeps its point.
    const auto fixed = [](int32_t whole) {
        return static_cast<int32_t>(static_cast<uint32_t>(whole) << 16);
    };
    def.waterline = 22;
    FixedVec3 floating{fixed(50) + 0x4000, fixed(90), fixed(61)};
    match.snap_to_build_grid(1, floating);
    CHECK(floating.x == fixed(56) && floating.y == fixed(28) && floating.z == fixed(56));
    FixedVec3 moving{fixed(50), fixed(90), fixed(61)};
    match.snap_to_build_grid(2, moving);
    CHECK(moving.x == fixed(50) && moving.y == fixed(90) && moving.z == fixed(61));
    def.waterline = 0;
    metadata.min_water_depth = 20;

    // Factory pad (ARMLAB 'c' cells): the lab sets plot.flags bit 2 on every
    // yard cell. The building-site test with a mobile product's default mask
    // 0x2f rejects those cells; site_clear_for's bm_code==1 walk does not.
    for (int32_t row = 0; row < 2; ++row)
        for (int32_t column = 0; column < 2; ++column) {
            auto& plot =
                match.spatial().plots
                    [static_cast<std::size_t>(8 + row) * 16 + static_cast<std::size_t>(8 + column)];
            plot.low_height = 80;
            plot.high_height = 80;
            plot.flags = 2;
            plot.ground = 0;
            plot.blocking_feature = false;
        }
    CHECK(!match.building_site_clear(2, 8, 8, 0));
    CHECK(match.site_clear_for(2, 8, 8, 0, 1));
    match.spatial().plots[8 * 16 + 8].ground = 1;
    CHECK(!match.site_clear_for(2, 8, 8, 0, 1));
    CHECK(match.site_clear_for(2, 8, 8, 1, 1));
    CHECK(match.site_clear_for(2, 8, 8, 0, 0));
    match.spatial().plots[8 * 16 + 8].ground = 0;
    for (int32_t row = 0; row < 2; ++row)
        for (int32_t column = 0; column < 2; ++column)
            match.spatial()
                .plots
                    [static_cast<std::size_t>(8 + row) * 16 + static_cast<std::size_t>(8 + column)]
                .flags = 0;

    // The build ghost's test for the placing player (it passes the
    // local player): without the mapping rule the launch mapped every point.
    metadata.yard_cells = mex_yard;
    fields[1].yard_mask = metadata.yard_cells;
    metadata.min_water_depth = -10000;
    set_site(2, 2, 3, 80, 80);
    // On land the building stands on its lowest level plot.
    FixedVec3 standing{fixed(50), 0, fixed(61)};
    match.snap_to_build_grid(1, standing);
    CHECK(standing.x == fixed(56) && standing.y == fixed(80) && standing.z == fixed(56));
    CHECK(match.point_mapped({56u << 16, 0, 56u << 16}));
    CHECK(match.building_site(1, 2, 2, 0, 0) == 80);

    // Mapping and line of sight on.
    auto gated_input = input;
    gated_input.visibility_flags = 3;
    sim::match_runtime::Match gated(gated_input, services);
    gated.reset_sight_buffers(true);
    auto& sight = gated.sight_mutable();
    // The 3x3 site at cell (2,2) is centred on (56,56); at the corner's
    // terrain height 0 that is sight cell (1,1).
    const std::size_t centre = 1 * 8 + 1;
    CHECK(gated.building_site_clear(1, 2, 2, 0));
    CHECK(!gated.building_site(1, 2, 2, 0, 0));
    sight.player_bits[centre] = 1;
    CHECK(gated.building_site(1, 2, 2, 0, 0) == 80);
    // Mapped but out of sight: units and claimed plots do not refuse the
    // placing player, features do; the simulation's own test sees them.
    gated.spatial().plots[3 * 16 + 3].ground = 5;
    gated.spatial().plots[2 * 16 + 4].flags = 2;
    CHECK(gated.building_site(1, 2, 2, 0, 0) == 80);
    CHECK(!gated.building_site_clear(1, 2, 2, 0));
    sight.coverage[centre] = 1;
    CHECK(!gated.building_site(1, 2, 2, 0, 0));
    gated.spatial().plots[3 * 16 + 3].ground = 0;
    CHECK(!gated.building_site(1, 2, 2, 0, 0));
    gated.spatial().plots[2 * 16 + 4].flags = 0;
    CHECK(gated.building_site(1, 2, 2, 0, 0) == 80);
    sight.coverage[centre] = 0;
    gated.spatial().plots[4 * 16 + 4].blocking_feature = true;
    CHECK(!gated.building_site(1, 2, 2, 0, 0));
    gated.spatial().plots[4 * 16 + 4].blocking_feature = false;
    // The centre is raised by the corner cell's own terrain height (TNT
    // attribute, MapPlot.height), not the plots' extremes: at height 64 it samples
    // sight cell (1,0), which is unmapped.
    map.attributes[2 * 16 + 2].height = 64;
    CHECK(!gated.building_site(1, 2, 2, 0, 0));
    sight.player_bits[1] = 1;
    CHECK(gated.building_site(1, 2, 2, 0, 0) == 80);
    map.attributes[2 * 16 + 2].height = 0;
    // Mapping and line of sight off: every in-range point is visible.
    auto open_input = input;
    open_input.visibility_flags = 0;
    sim::match_runtime::Match open(open_input, services);
    open.reset_sight_buffers(true);
    CHECK(open.point_visible(0, {56u << 16, 0, 56u << 16}));
    CHECK(!open.point_visible(0, {600u << 16, 0, 56u << 16}));
    open.spatial().plots[3 * 16 + 3].ground = 5;
    CHECK(!open.building_site(1, 2, 2, 0, 0));
    return 0;
}
