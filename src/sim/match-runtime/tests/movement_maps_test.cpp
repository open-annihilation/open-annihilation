// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Persistent movement-class maps (build, footprint refresh, object removal,
// search projection, a restored unit's occupancy tick), the player slot's
// single 30-tick deadline and the players' pass that ends a match's load.
#include "oa/sim/match_runtime.hpp"

#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/ground_orders/goals.hpp"
#include "oa/sim/match_runtime/rule_state.hpp"
#include "oa/sim/unit_movement/local_flow.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include "oa/test/match_services.hpp"

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(std::string(#x) + " at line " + std::to_string(__LINE__));    \
    } while (false)

using namespace oa;

namespace {
constexpr uint32_t map_cells = 32;
constexpr uint8_t ground_height = 40;
constexpr uint16_t tank = 1, kbot = 2, building = 3, type_count = 4;
constexpr sim::unit_spawn::AssetHandle tank_class = 1, kbot_class = 2;
// Blocking sprite features: a 2x2 rock and a 1x3 wall.
constexpr uint16_t rock = 0, wall_feature = 1;

struct Services : oa::test::QuietServices {
    uint32_t footprint_changes{}, object_removals{};

    void notify_object_footprint_removed(sim::spatial_state::Unit&, uint32_t) override {
        ++object_removals;
    }

    void notify_footprint_changed(std::array<int16_t, 2>, std::array<int16_t, 2>) override {
        ++footprint_changes;
    }
};

using Scenario = oa::test::EmptyScenario;

// A flat 32x32-cell land map with a two-cell tank class, a one-cell kbot
// class and a 3x3 building. `wall` marks blocking-feature cells present from
// the start; a tank makes `tank_energy` energy a settlement.
struct Fixture {
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain_values =
        std::vector<sim::visibility_state::TerrainCell>(map_cells * map_cells);
    std::array<sim::visibility_state::SightMask, 1> masks{};
    std::shared_ptr<formats::objects3d::Model> model =
        std::make_shared<formats::objects3d::Model>();
    std::shared_ptr<formats::cob::CobProgram> script = std::make_shared<formats::cob::CobProgram>();
    std::array<sim::unit_spawn::LoadedType, type_count> loaded;
    std::array<sim::unit_spawn::Type, type_count> types;
    std::array<data::unit_definitions::UnitDefinition, type_count> defs;
    std::array<data::unit_definitions::RuntimeDefinitionMetadata, type_count> metadata;
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    std::vector<sim::spatial_state::Plot> plots =
        std::vector<sim::spatial_state::Plot>(map_cells * map_cells);
    std::array<sim::match_runtime::RuntimeTypeFields, type_count> fields{};
    std::array<FeatureDef, 2> features{};
    std::vector<uint8_t> building_yard = std::vector<uint8_t>(9, 0x2f);
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    std::unique_ptr<sim::match_runtime::Match> match;

    explicit Fixture(
        const std::vector<std::array<int32_t, 2>>& wall = {},
        float tank_energy = 0,
        uint32_t version = 0,
        void (*prepare)(Fixture&) = nullptr,
        bool enabled = true
    ) {
        map.attribute_width = map.attribute_height = map_cells;
        map.sea_level = 20;
        map.attributes.resize(map_cells * map_cells);
        for (auto& attribute : map.attributes)
            attribute.height = ground_height;
        for (auto& plot : plots)
            plot.low_height = plot.high_height = ground_height;
        for (const auto& [x, z] : wall)
            plots[static_cast<std::size_t>(z) * map_cells + static_cast<std::size_t>(x)]
                .blocking_feature = true;
        masks[0].width = masks[0].height = 1;
        masks[0].pixels = {1};
        model->objects.resize(1);
        model->objects[0].name = "root";
        // A model 20 units high (UnitDef.model_height).
        model->objects[0].vertices = {{0, 0, 0}, {0, 20 << 16, 0}};
        script->code = {sim::script_vm::opcode::return_};
        script->scripts = {{"Create", 0}};
        script->entry_points = {0};
        script->piece_names = {"root"};
        const std::array<int16_t, type_count> footprint{0, 2, 1, 3};
        for (uint16_t i = 1; i < type_count; ++i) {
            types[i].simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
            types[i].simulation.maximum_health = 100;
            types[i].footprint_x = types[i].footprint_z = footprint[i];
            types[i].bm_code = i == building ? 0 : 1;
            types[i].model = reinterpret_cast<uintptr_t>(model.get());
            types[i].cob = reinterpret_cast<uintptr_t>(script.get());
            loaded[i].model = model;
            loaded[i].script = script;
            loaded[i].type = types[i];
            loaded[i].unit_name = "UNIT" + std::to_string(i);
            defs[i].sight_distance = 64;
            defs[i].acceleration_fixed = 65536;
            defs[i].brake_rate_fixed = 65536;
            defs[i].max_velocity_fixed = 2 * 65536;
            defs[i].turn_rate = 1024;
            metadata[i].footprint_x = metadata[i].footprint_z = footprint[i];
            fields[i].definition = &defs[i];
            fields[i].runtime_metadata = &metadata[i];
            fields[i].target_masks = &target_masks;
            fields[i].movement_class = sim::unit_spawn::AssetHandle{0};
        }
        fields[tank].movement_class = tank_class;
        defs[tank].energy_make = tank_energy;
        fields[kbot].movement_class = kbot_class;
        fields[building].yard_mask = building_yard;
        for (auto& feature : features) {
            feature.flags = OA_FEATURE_FLAG_SPRITE | OA_FEATURE_FLAG_BLOCKING;
            feature.dead_feature = sim::feature_runtime::no_feature;
        }
        features[rock].footprint_x = features[rock].footprint_z = 2;
        features[wall_feature].footprint_x = 1;
        features[wall_feature].footprint_z = 3;
        if (prepare)
            prepare(*this);
        sim::match_runtime::OfflineInputs input{
            map,
            loaded,
            types,
            fields,
            weapons,
            terrain_values,
            masks,
            12,
            12,
            8,
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
        input.rules.setup.enhanced_mode.enabled = enabled && version != 0;
        input.rules.setup.enhanced_mode.version = version;
        if (input.rules.setup.enhanced_mode.enabled)
            input.unit_player_mask = 3;
        match = std::make_unique<sim::match_runtime::Match>(input, services);
        if (const auto* fault = match->fault())
            throw std::runtime_error(fault);
        match->configure_strategic_environment({0, 0.5F, 0});
        for (uint8_t player = 0; player < 2; ++player) {
            match->simulation().players[player].present = true;
            match->simulation().players[player].status = 1;
            std::array<uint8_t, 10> allies{};
            allies[player] = 1;
            match->configure_player_alliances(player, allies);
            if (player == 0)
                match->configure_outcomes(0, allies, false);
        }
    }

    // A finished unit whose footprint origin is the given cell.
    sim::unit_spawn::Slot& spawn(uint16_t type, int32_t cell_x, int32_t cell_z) {
        const auto centre = [&](int32_t cell) {
            return static_cast<uint32_t>(cell * 16 + types[type].footprint_x * 8) << 16;
        };
        auto* slot = match->create({0, type, {centre(cell_x), 0, centre(cell_z)}, true, 1, 0});
        CHECK(slot && slot->unit);
        CHECK(slot->record.cell_x == cell_x && slot->record.cell_z == cell_z);
        return *slot;
    }

    // A feature placed or cleared through the feature runtime.
    void place(uint16_t feature, int32_t cell_x, int32_t cell_z) {
        (void)sim::feature_runtime::place_feature(
            match->state(),
            match->feature_host(),
            plot(cell_x, cell_z),
            feature,
            nullptr,
            nullptr,
            sim::feature_runtime::no_player
        );
        CHECK(match->state().plots[plot(cell_x, cell_z)].feature == feature);
    }

    void clear(int32_t cell_x, int32_t cell_z) {
        CHECK(
            sim::feature_runtime::clear_plot_feature(
                match->state(), match->feature_host(), plot(cell_x, cell_z), false
            )
        );
    }

    static std::size_t plot(int32_t cell_x, int32_t cell_z) {
        return static_cast<std::size_t>(cell_z) * map_cells + static_cast<std::size_t>(cell_x);
    }

    const sim::ground_orders::MovementMap& class_map(sim::unit_spawn::AssetHandle handle) const {
        const auto* found_map = match->movement_map(handle);
        CHECK(found_map != nullptr);
        return *found_map;
    }
};

// Expected class of every anchor in [first, last] of both axes around a
// square block of blocked cells: 0 where the class footprint overlaps the
// block, 1 where only its one-cell rim touches it, 3 beyond.
void check_block(
    const sim::ground_orders::MovementMap& map,
    int32_t origin,
    int32_t size,
    int32_t footprint,
    int32_t first,
    int32_t last
) {
    const auto overlaps = [&](int32_t anchor, int32_t reach) {
        return anchor + footprint + reach > origin && anchor - reach < origin + size;
    };
    for (auto z = first; z <= last; ++z)
        for (auto x = first; x <= last; ++x) {
            const uint8_t expected = overlaps(x, 0) && overlaps(z, 0)   ? 0
                                     : overlaps(x, 1) && overlaps(z, 1) ? 1
                                                                        : 3;
            const auto actual = map.cell(static_cast<uint32_t>(x), static_cast<uint32_t>(z));
            if (actual != expected)
                throw std::runtime_error(
                    "cell " + std::to_string(x) + "," + std::to_string(z) + " is " +
                    std::to_string(actual) + ", expected " + std::to_string(expected)
                );
        }
}

void check_clear(const sim::ground_orders::MovementMap& map, int32_t first, int32_t last) {
    for (auto z = first; z <= last; ++z)
        for (auto x = first; x <= last; ++x)
            CHECK(map.cell(static_cast<uint32_t>(x), static_cast<uint32_t>(z)) == 3);
}

// build_movement_maps builds a map for each class a type names and none for
// class-less types; a building registered mid-match walls its footprint in
// every class's map through the footprint refresh, and its death (no
// movement object) clears it again through the same refresh.
void building_walls_every_class_until_removed() {
    Fixture f;
    CHECK(f.match->movement_map(sim::unit_spawn::AssetHandle{0}) == nullptr);
    CHECK(f.class_map(tank_class).footprint_x() == 2 && f.class_map(kbot_class).footprint_x() == 1);
    CHECK(
        f.class_map(tank_class).width() == map_cells &&
        f.class_map(tank_class).height() == map_cells
    );
    check_clear(f.class_map(tank_class), 6, 15);
    check_clear(f.class_map(kbot_class), 6, 15);

    f.match->simulation().tick = 100;
    const auto changes = f.services.footprint_changes;
    auto& site = f.spawn(building, 10, 10);
    CHECK(f.services.footprint_changes == changes + 1);
    // The rectangle reaches the anchors just past the far edge: the tank's
    // rim at 13 touches column 12.
    check_block(f.class_map(tank_class), 10, 3, 2, 6, 15);
    check_block(f.class_map(kbot_class), 10, 3, 1, 6, 15);
    CHECK(f.class_map(tank_class).cell(13, 13) == 1 && f.class_map(tank_class).cell(9, 9) == 0);

    f.match->simulation().tick = 130;
    site.unit->record.damage_kind = static_cast<uint8_t>(sim::match_runtime::DeathKind::weapon);
    f.match->teardown_dead_unit(site);
    CHECK(f.match->spatial().plots[10 * map_cells + 10].ground == sim::spatial_state::no_unit);
    check_clear(f.class_map(tank_class), 6, 15);
    check_clear(f.class_map(kbot_class), 6, 15);
}

// Feature placement and removal refresh every class. The checked anchors
// stay below the top rows the map load voids.
void features_refresh_every_class() {
    Fixture f;
    const auto changes = f.services.footprint_changes;
    f.place(rock, 6, 6);
    CHECK(f.services.footprint_changes == changes + 1);
    CHECK(f.match->spatial().plots[Fixture::plot(7, 7)].blocking_feature);
    check_block(f.class_map(tank_class), 6, 2, 2, 3, 11);
    check_block(f.class_map(kbot_class), 6, 2, 1, 3, 11);
    f.clear(7, 7);
    CHECK(f.services.footprint_changes == changes + 2);
    CHECK(!f.match->spatial().plots[Fixture::plot(7, 7)].blocking_feature);
    check_clear(f.class_map(tank_class), 3, 11);
    check_clear(f.class_map(kbot_class), 3, 11);
}

struct PlotClassifier final : sim::ground_orders::MovementMapSampler {
    sim::spatial_state::CellClassifier movement{};
    const sim::spatial_state::World* world{};

    uint8_t classify_cell(int32_t x, int32_t z) override {
        return sim::spatial_state::classify_movement_cell(movement, x, z, *world);
    }

    uint8_t classify_plot(int32_t x, int32_t z) override {
        return sim::spatial_state::classify_movement_plot(movement, x, z, *world);
    }
};

std::vector<sim::ground_orders::RoutePoint> search(
    sim::simulation_state::Unit& unit,
    sim::ground_orders::MovementMap& map,
    const sim::ground_orders::SearchBegin& begin,
    sim::ground_orders::Goal& goal
) {
    static const std::vector<uint16_t> all_seen(64 * 64, 0xffff);
    sim::ground_orders::Navigation navigation;
    sim::ground_orders::SearchWorker worker;
    auto result = worker.begin({&unit, &navigation, &goal, &map}, begin, all_seen);
    for (int slice = 0; slice < 64 && result == sim::ground_orders::SearchAdvance::in_progress;
         ++slice)
        result = worker.advance_slice();
    CHECK(result == sim::ground_orders::SearchAdvance::succeeded);
    return worker.route();
}

// On a map that only changes through the refresh triggers, the job's
// persistent class map after the search projection holds what the whole-map
// build makes at job time with the searcher's own tick set to now, so
// the search returns the same route. A tank settled for more than 30 ticks
// is a wall; a kbot that moved within 30 ticks is not. A second search in the
// same tick gathers no other units and finds the same route.
void persistent_search_matches_the_job_build() {
    std::vector<std::array<int32_t, 2>> wall;
    for (int32_t z = 4; z <= 15; ++z)
        wall.push_back({8, z});
    Fixture f(wall);
    auto& searcher = f.spawn(kbot, 3, 11);
    f.match->simulation().tick = 5;
    auto& settled = f.spawn(tank, 13, 10);
    f.match->simulation().tick = 150;
    auto& site = f.spawn(building, 14, 15);
    (void)site;
    f.place(wall_feature, 12, 5);
    f.match->simulation().tick = 195;
    auto& walker = f.spawn(kbot, 16, 8);
    constexpr uint32_t now = 200;
    f.match->simulation().tick = now;

    auto* job_unit = searcher.unit;
    sim::ground_orders::SearchBegin begin;
    begin.current_tick = now;
    sim::ground_orders::MovementMap* persistent = nullptr;
    CHECK(f.match->prepare_search_job(searcher, begin, persistent));
    CHECK(persistent == f.match->movement_map(kbot_class));
    CHECK(begin.occupancy.size() == 2 && begin.unit_changed_tick != nullptr);
    CHECK(*begin.unit_changed_tick == 0);
    sim::ground_orders::Goal goal;
    goal.cell = {20, 11};
    const auto route = search(*job_unit, *persistent, begin, goal);
    CHECK(persistent->projection_tick() == now - 30 && *begin.unit_changed_tick == 0);
    // A second search of the class in the tick leaves the projection where
    // it is, so the job gathers no other units for it.
    sim::ground_orders::SearchBegin again;
    again.current_tick = now;
    sim::ground_orders::MovementMap* same = nullptr;
    CHECK(f.match->prepare_search_job(searcher, again, same) && same == persistent);
    CHECK(again.occupancy.empty() && again.unit_changed_tick == begin.unit_changed_tick);
    CHECK(search(*job_unit, *persistent, again, goal) == route);
    CHECK(persistent->cell(13, 10) == 0 && persistent->cell(14, 11) == 0);
    CHECK(persistent->cell(16, 8) == 3 && persistent->cell(3, 11) == 3);

    PlotClassifier classifier;
    classifier.world = &f.match->spatial();
    classifier.movement.footprint_x = classifier.movement.footprint_z = 1;
    classifier.movement.max_water_depth = f.metadata[kbot].max_water_depth;
    classifier.movement.min_water_depth = f.metadata[kbot].min_water_depth;
    classifier.movement.max_land_slope = f.metadata[kbot].max_slope;
    classifier.movement.bad_land_slope = f.metadata[kbot].bad_slope;
    classifier.movement.max_water_slope = f.metadata[kbot].max_water_slope;
    classifier.movement.bad_water_slope = f.metadata[kbot].bad_water_slope;
    classifier.movement.occupancy_before_tick = now - 30;
    sim::ground_orders::MovementMap job_map(map_cells, map_cells, 1, 1, classifier);
    auto& own_tick = f.match->spatial().units[searcher.unit_index].object_tick;
    const auto saved = own_tick;
    own_tick = now;
    job_map.rebuild();
    own_tick = saved;
    for (uint32_t z = 0; z < map_cells; ++z)
        for (uint32_t x = 0; x < map_cells; ++x)
            CHECK(persistent->cell(x, z) == job_map.cell(x, z));

    sim::ground_orders::SearchBegin job_begin;
    job_begin.current_tick = now;
    job_begin.world_width = job_begin.world_height = map_cells;
    CHECK(search(*job_unit, job_map, job_begin, goal) == route);
    CHECK(route.size() > 2);
    (void)settled;
    (void)walker;
}

// A restored unit's saved movement block gives the movement maps the tick it
// last changed cells, replacing the tick its placement stamped: a tank that
// stood still from tick 5, placed again by a load at tick 200, walls the next
// search's projection at tick 210, as one that stood there all along does.
void restored_mobility_keeps_the_occupancy_tick() {
    Fixture f;
    f.match->simulation().tick = 200;
    auto& parked = f.spawn(tank, 13, 10);
    auto& searcher = f.spawn(kbot, 3, 11);
    CHECK(f.match->spatial().units[parked.unit_index].object_tick == 200);
    auto* runtime = f.match->ground_runtime(parked.unit_index);
    CHECK(runtime != nullptr);
    auto block = runtime->save_mobility();
    // The occupancy tick is the block's little-endian word at 30.
    block[30] = 5;
    block[31] = block[32] = block[33] = 0;
    CHECK(f.match->restore_mobility(parked.unit_index, block));
    CHECK(runtime->occupancy_changed_tick == 5);
    CHECK(f.match->spatial().units[parked.unit_index].object_tick == 5);
    CHECK(!f.match->restore_mobility(f.spawn(building, 20, 20).unit_index, block));

    constexpr uint32_t now = 210;
    f.match->simulation().tick = now;
    sim::ground_orders::SearchBegin begin;
    begin.current_tick = now;
    sim::ground_orders::MovementMap* map = nullptr;
    CHECK(f.match->prepare_search_job(searcher, begin, map));
    CHECK(map == f.match->movement_map(kbot_class) && map->cell(13, 10) == 3);
    map->prepare_search(
        sim::ground_orders::occupancy_rectangle(searcher.record),
        *begin.unit_changed_tick,
        begin.occupancy,
        now
    );
    CHECK(map->projection_tick() == now - 30);
    CHECK(map->cell(13, 10) == 0 && map->cell(14, 11) == 0 && map->cell(12, 9) == 1);
}

namespace ground = sim::ground_orders;
namespace movement = sim::unit_movement;
namespace spatial = sim::spatial_state;
constexpr int32_t pixel = 65536;
constexpr uint32_t following_version = data::enhanced_mode::following_flow_version;
constexpr uint16_t east = 49152;
constexpr uint8_t ground_layer = 1;
constexpr uint8_t patrol_kind = 29;
constexpr uint32_t native_occupancy_history = 30;
constexpr uint32_t distributed_version = data::enhanced_mode::distributed_battle_flow_version;

void planning_inputs(Fixture& f) {
    f.defs[kbot].max_velocity_fixed = 0;
    f.metadata[tank].max_water_depth = 0;
    // A known water plot is an actual terrain obstacle for the land class.
    f.map.attributes[Fixture::plot(26, 8)].height = 0;
    f.plots[Fixture::plot(26, 8)].low_height = 0;
    f.plots[Fixture::plot(26, 8)].high_height = 0;
}

void cached_planning_excludes_only_movement_objects_in_eight() {
    for (uint32_t version = 0; version <= following_version + 1; ++version) {
        Fixture f({}, 0, version, planning_inputs);
        f.match->simulation().tick = 5;
        const auto aged = f.spawn(tank, 6, 8).unit_index;
        const auto immobile = f.spawn(kbot, 10, 8).unit_index;
        f.spawn(building, 18, 8);
        f.place(rock, 22, 8);
        f.match->simulation().tick = 195;
        f.spawn(tank, 14, 8);
        auto& searcher = f.spawn(tank, 4, 14);
        constexpr uint32_t now = 200;
        f.match->simulation().tick = now;
        ground::SearchBegin begin{};
        begin.current_tick = now;
        ground::MovementMap* map{};
        CHECK(f.match->prepare_search_job(searcher, begin, map));
        map->prepare_search(
            ground::occupancy_rectangle(searcher.record),
            *begin.unit_changed_tick,
            begin.occupancy,
            now
        );
        CHECK(map->projection_tick() == now - native_occupancy_history);
        CHECK(f.match->spatial().units[aged].object_present);
        CHECK(f.match->spatial().units[immobile].object_present);
        CHECK(f.match->ground_runtime(immobile)->geometry.type.maximum_speed == 0);
        const uint8_t occupied = version == following_version ? 3 : 0;
        if (map->cell(6, 8) != occupied)
            std::cerr << "Cached planning version=" << version
                      << ", aged=" << unsigned(map->cell(6, 8))
                      << ", expected=" << unsigned(occupied) << '\n';
        CHECK(map->cell(6, 8) == occupied);
        CHECK(map->cell(10, 8) == occupied);
        CHECK(map->cell(14, 8) == 3);
        CHECK(map->cell(18, 8) == 0);
        CHECK(map->cell(22, 8) == 0);
        CHECK(map->cell(26, 8) == 0);
        const auto& body = f.match->spatial().units[searcher.unit_index];
        for (const auto cell :
             {std::array<int16_t, 2>{6, 8}, {10, 8}, {14, 8}, {18, 8}, {22, 8}, {26, 8}}) {
            const auto physical = spatial::can_occupy(
                body, searcher.unit_index, cell, ground_layer, f.match->spatial()
            );
            CHECK(physical && !*physical);
        }
    }
    // A raw unsupported version above eight also retains the native cutoff;
    // the public profile resolver still refuses it. Disabled eight is Off.
    Fixture off({}, 0, following_version, planning_inputs, false);
    off.match->simulation().tick = 5;
    off.spawn(tank, 6, 8);
    auto& searcher = off.spawn(tank, 4, 14);
    ground::SearchBegin begin{};
    begin.current_tick = 200;
    ground::MovementMap* map{};
    CHECK(off.match->prepare_search_job(searcher, begin, map));
    map->prepare_search(
        ground::occupancy_rectangle(searcher.record),
        *begin.unit_changed_tick,
        begin.occupancy,
        begin.current_tick
    );
    CHECK(map->cell(6, 8) == 0);
}

struct PhysicalHost final : movement::Host {
    Fixture& fixture;

    explicit PhysicalHost(Fixture& f) : fixture(f) {}

    bool can_occupy(
        const movement::Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy
    ) override {
        const auto result = spatial::can_occupy(
            fixture.match->spatial().units[static_cast<uint16_t>(unit.id)],
            static_cast<uint16_t>(unit.id),
            cell,
            occupancy,
            fixture.match->spatial()
        );
        CHECK(result.has_value());
        return *result;
    }

    void remove_occupancy(movement::Unit&) override {}

    void insert_occupancy(movement::Unit&) override {}

    void update_spatial_membership(movement::Unit&) override {}
};

void place_gated_corridor(Fixture& f) {
    // Canonical blocking rocks make a closed two-plot corridor and its
    // north turnout. Tile the enclosing box away from native hidden rows.
    for (int32_t z = 2; z <= 12; z += 2)
        for (int32_t x = 0; x <= 24; x += 2) {
            const bool passage = z == 8 && x >= 2 && x <= 22;
            const bool turnout = x == 6 && (z == 4 || z == 6);
            if (!passage && !turnout)
                f.place(rock, x, z);
        }
}

void check_physical_bodies(Fixture& f, const std::array<uint16_t, 2>& ids) {
    std::array<uint16_t, map_cells * map_cells> claimed{};
    for (const auto id : ids) {
        const auto* body = f.match->ground_runtime(id);
        CHECK(body && body->geometry.footprint == (std::array<int16_t, 2>{2, 2}));
        for (int16_t z = 0; z < 2; ++z)
            for (int16_t x = 0; x < 2; ++x) {
                const auto px = int32_t(body->geometry.cell[0]) + x;
                const auto pz = int32_t(body->geometry.cell[1]) + z;
                CHECK(px >= 0 && pz >= 0 && px < int32_t(map_cells) && pz < int32_t(map_cells));
                const auto at = Fixture::plot(px, pz);
                CHECK(!claimed[at]);
                claimed[at] = id;
                const auto& plot = f.match->spatial().plots[at];
                CHECK(plot.ground == id && !plot.blocking_feature);
                CHECK(plot.low_height == ground_height && plot.high_height == ground_height);
            }
    }
}

bool same_goal_geometry(const ground::Goal& a, const ground::Goal& b) {
    return a.shape == b.shape && a.cell == b.cell && a.tolerance == b.tolerance &&
           a.radius_squared == b.radius_squared && a.inner_range == b.inner_range &&
           a.inner_radius_squared == b.inner_radius_squared && a.left == b.left &&
           a.right == b.right && a.top == b.top && a.bottom == b.bottom;
}

bool publish_native_movement_effects(
    Fixture& f,
    sim::unit_spawn::Slot& slot,
    ground::GroundRuntime& body,
    std::span<const ground::RoutePoint> route
) {
    std::array<uint8_t, 3> weapons{};
    const bool failed = ground::accept_path(
        {*slot.unit, body.geometry, body.movement, body.navigation, false, weapons}, route
    );
    // These are the movement effects of tick.cpp's native publisher. Its
    // RuntimeOrder notification latch and EventHooks are not modeled here;
    // this fixture has quiet services and inspects the real mission bits.
    const auto* table = sim::match_runtime::find_rule_state(f.match->rule_state(), "local-flow");
    CHECK(table && table->bytes && table->restore);
    std::vector<movement::LocalFlowHead> heads(f.match->state().unit_slot_count);
    CHECK(movement::read_local_flow_heads(heads, table->bytes(table->context)));
    const auto before = heads[slot.unit_index];
    heads[slot.unit_index].active = false;
    heads[slot.unit_index].count = 0;
    std::vector<uint8_t> bytes(heads.size() * movement::local_flow_head_bytes);
    CHECK(movement::write_local_flow_heads(heads, bytes));
    CHECK(table->restore(table->context, bytes));
    const auto* after = f.match->local_flow_head(slot.unit_index);
    CHECK(after && !after->active && after->count == 0);
    CHECK(
        after->points == before.points && after->goal_key == before.goal_key &&
        after->tick == before.tick && after->blocked_ticks == before.blocked_ticks &&
        after->mobile_blocked == before.mobile_blocked
    );
    return failed;
}

struct PatrolResult {
    bool native_plan_reaches{}, fresh_refusal{}, home_before_far{}, reached_far{}, rested{};
};

PatrolResult patrol_keeps_its_far_goal_after_the_gate_leaves(uint32_t version) {
    Fixture f({}, 0, version);
    place_gated_corridor(f);
    const auto gate = f.spawn(tank, 6, 8).unit_index;
    const auto mover = f.spawn(tank, 2, 8).unit_index;
    auto& body = *f.match->ground_runtime(mover);
    auto& slot = f.match->spawn_slot(mover);
    auto& patrol = f.match->issue_patrol(mover, {336 * pixel, 0, 144 * pixel}, false);
    CHECK(patrol.kind == patrol_kind);
    // Patrol first clones its Home leg, then installs its far Goal. Use its
    // ordinary early ticks, before the native sixty-tick search deadline.
    for (uint32_t tick = 0; tick < 8 && !body.navigation.goal; ++tick) {
        ++f.match->simulation().tick;
        f.match->tick();
    }
    CHECK(body.navigation.goal && body.navigation.goal->order == slot.unit->primary);
    const auto home_cell = body.geometry.cell;
    CHECK(home_cell == (std::array<int16_t, 2>{2, 8}));
    const auto* initial_head = f.match->local_flow_head(mover);
    std::cerr << "Patrol checkpoint: version=" << version
              << ", tick=" << uint32_t(f.match->simulation().tick)
              << ", head=" << (initial_head && initial_head->active)
              << ", count=" << (initial_head ? unsigned(initial_head->count) : 0)
              << ", P1=" << (initial_head ? initial_head->points[1][0] : 0) << ','
              << (initial_head ? initial_head->points[1][1] : 0)
              << ", pose=" << body.geometry.position[0] << ',' << body.geometry.position[2]
              << ", northwall="
              << bool(f.match->spatial().plots[Fixture::plot(3, 7)].blocking_feature)
              << ", westwall="
              << bool(f.match->spatial().plots[Fixture::plot(2, 8)].blocking_feature) << '\n';
    CHECK(initial_head && !initial_head->active && initial_head->count == 0);
    // The controlled checkpoint ages the gate and places the owned patrol
    // at its next cell boundary. No retry timestamp or event is suppressed.
    f.match->simulation().tick = 200;
    f.match->place_unit_at(mover, 88 * pixel - 1, ground_height * pixel, 144 * pixel, ground_layer);
    slot.record.heading = east;
    body.project_slot();
    body.movement.speed = f.defs[tank].max_velocity_fixed;
    body.movement.velocity = movement::aim_velocity(east, 0, body.movement.speed);
    body.movement.flags = ground_layer;
    const auto original_pose = body.geometry;
    const auto start_cell = body.geometry.cell;
    // This real step still refuses the aged mobile's actual footprint.
    ++f.match->simulation().tick;
    f.match->tick();
    check_physical_bodies(f, {gate, mover});
    CHECK(body.navigation.goal && body.navigation.goal->order == slot.unit->primary);
    const auto* far_goal = body.navigation.goal;
    const auto far = *far_goal;
    CHECK(far.cell == (std::array<int16_t, 2>{20, 8}));
    CHECK(body.geometry.cell == start_cell);
    CHECK((body.movement.flags & movement::collision_blocked) != 0);
    PhysicalHost host(f);
    auto copied_unit = original_pose;
    movement::Movement copied_movement{};
    copied_movement.velocity = body.movement.velocity;
    copied_movement.speed = f.defs[tank].max_velocity_fixed;
    copied_movement.flags = ground_layer;
    movement::LocalFlowCounters counters{};
    const auto sweep = movement::integrate_swept_local_flow(
        copied_unit, copied_movement, f.match->simulation().tick, true, host, counters
    );
    PatrolResult result;
    result.fresh_refusal = sweep.status == movement::GroundSweepStatus::blocked;
    CHECK(result.fresh_refusal && sweep.origin_queries > 0 && sweep.refused_origin_valid);
    CHECK(copied_unit.position == body.geometry.position && copied_unit.cell == body.geometry.cell);
    ground::SearchBegin begin{};
    begin.current_tick = f.match->simulation().tick;
    ground::MovementMap* map{};
    CHECK(f.match->prepare_search_job(slot, begin, map));
    ground::Navigation scratch{};
    ground::SearchWorker worker;
    const std::array<uint16_t, map_cells * map_cells> known = [] {
        std::array<uint16_t, map_cells * map_cells> result{};
        result.fill(0xffff);
        return result;
    }();
    auto status = worker.begin({slot.unit, &scratch, body.navigation.goal, map}, begin, known);
    for (uint32_t slice = 0; slice < 64 && status == ground::SearchAdvance::in_progress; ++slice)
        status = worker.advance_slice();
    CHECK(status != ground::SearchAdvance::in_progress);
    const auto route = worker.route();
    result.native_plan_reaches =
        !route.empty() &&
        ground::goal_contains(far, (route.back()[0] - 8) / 16, (route.back()[1] - 8) / 16);
    // Native searches may finish while Patrol waits in phase 2. Let that
    // phase arise normally before delivering this completed worker result;
    // phase 1 refreshes a Goal rather than consuming its completion event.
    uint32_t waited{};
    while (waited < 900 && slot.unit->primary && slot.unit->primary->kind == patrol_kind &&
           slot.unit->primary->phase != 2) {
        if (waited == 90)
            CHECK(!f.match->order_refused(
                f.match->issue_ground_move(gate, {112 * pixel, 0, 80 * pixel}, false)
            ));
        ++f.match->simulation().tick;
        f.match->tick();
        ++waited;
        check_physical_bodies(f, {gate, mover});
        CHECK(body.navigation.goal && same_goal_geometry(*body.navigation.goal, far));
    }
    CHECK(
        waited < 900 && slot.unit->primary && slot.unit->primary->kind == patrol_kind &&
        slot.unit->primary->phase == 2
    );
    CHECK(!ground::goal_contains_unit(far, body.geometry));
    // Publish the real worker result through the original movement effects,
    // equally in both modes. No failure event is forged or swallowed.
    CHECK(publish_native_movement_effects(f, slot, body, route) == route.empty());
    bool stopped{};
    for (uint32_t elapsed = waited; elapsed < 900; ++elapsed) {
        if (elapsed == 90)
            CHECK(!f.match->order_refused(
                f.match->issue_ground_move(gate, {112 * pixel, 0, 80 * pixel}, false)
            ));
        ++f.match->simulation().tick;
        f.match->tick();
        check_physical_bodies(f, {gate, mover});
        const bool contains_far = ground::goal_contains_unit(far, body.geometry);
        result.reached_far = result.reached_far || contains_far;
        if (!result.reached_far && body.navigation.goal &&
            !same_goal_geometry(*body.navigation.goal, far)) {
            CHECK(body.navigation.goal->cell == home_cell);
            result.home_before_far = true;
        }
        if (version == following_version && !result.reached_far) {
            CHECK(body.navigation.goal == far_goal);
            CHECK(body.navigation.goal->order == slot.unit->primary);
            CHECK(slot.unit->primary->kind == patrol_kind);
        }
        if (result.reached_far && !stopped &&
            (!body.navigation.goal || !same_goal_geometry(*body.navigation.goal, far))) {
            // Patrol may rotate only after actual far containment. An
            // ordinary Stop then permits natural deceleration at that end.
            f.match->stop_orders(mover);
            stopped = true;
        }
        if (stopped && body.movement.speed == 0 && !body.navigation.goal) {
            result.rested = true;
            break;
        }
    }
    std::cout << "Static planning Patrol: version=" << version << ", publication_wait=" << waited
              << ", plan_reaches=" << result.native_plan_reaches
              << ", fresh_refusal=" << result.fresh_refusal
              << ", home_before_far=" << result.home_before_far
              << ", reached_far=" << result.reached_far << ", rested=" << result.rested << '\n';
    return result;
}

void static_eight_planning_does_not_weaken_physical_traffic() {
    const auto old = patrol_keeps_its_far_goal_after_the_gate_leaves(distributed_version);
    const auto following = patrol_keeps_its_far_goal_after_the_gate_leaves(following_version);
    CHECK(!old.native_plan_reaches && old.fresh_refusal && old.home_before_far);
    CHECK(following.native_plan_reaches && following.fresh_refusal && !following.home_before_far);
    CHECK(following.reached_far && following.rested);
}

// The player slot update advances each slot's Player.next_economy_tick deadline
// once when it comes due; the local slot's outcome checks run under that same
// deadline and no other.
void outcome_checks_follow_the_slot_deadline() {
    Fixture f;
    f.spawn(tank, 4, 4);
    auto& local = f.match->state().game.players[0];
    auto& other = f.match->state().game.players[1];
    const auto run_to = [&](uint32_t tick) {
        while (f.match->simulation().tick < tick) {
            ++f.match->simulation().tick;
            f.match->tick();
        }
    };
    f.match->simulation().tick = 39;
    local.next_economy_tick = 45;
    other.next_economy_tick = 42;
    run_to(44);
    CHECK(f.match->outcome_state().countdown == -1);
    CHECK(local.next_economy_tick == 45 && other.next_economy_tick == 72);
    run_to(45);
    CHECK(f.match->outcome_state().countdown == 4 && local.next_economy_tick == 75);
    run_to(74);
    CHECK(f.match->outcome_state().countdown == 4 && other.next_economy_tick == 102);
    run_to(75);
    CHECK(f.match->outcome_state().countdown == 3 && local.next_economy_tick == 105);
}

// A match ends its load with one players' pass at the load's tick: the path
// search's round, and each slot's update, whose due deadline settles the
// economy and moves 30 ticks on. finish_load leaves the pass waiting, so the
// loaded match stands as it loaded; run_load_pass runs it once. A new game's
// stores then return to those its start set, where the same settlement in a
// first tick leaves them changed, and the first tick settles nothing.
void the_load_ends_with_one_player_pass() {
    // A tank making 25 energy a settlement, under a player whose start granted
    // room to store it, and an enemy tank, so the outcome checks go on.
    const auto set_stores = [](Fixture& f) {
        f.spawn(tank, 4, 4);
        CHECK(f.match->create(
            {1, tank, {uint32_t(24 * 16) << 16, 0, uint32_t(24 * 16) << 16}, true, 1, 0}
        ));
        auto& player = f.match->state().game.players[0];
        sim::unit_spawn::grant_start_storage(player, 1000, 1000);
        player.energy = 300;
        player.metal = 200;
        return &player;
    };
    Fixture plain({}, 25);
    auto* settled = set_stores(plain);
    ++plain.match->simulation().tick;
    plain.match->tick();
    CHECK(settled->next_economy_tick == 30);
    CHECK(settled->energy == 325 && settled->metal == 200);

    Fixture f({}, 25);
    auto* local = set_stores(f);
    const auto& jobs = f.match->path_search_jobs();
    CHECK(local->next_economy_tick == 0 && jobs.refresh_tick == 0);
    f.match->run_load_pass();
    CHECK(local->next_economy_tick == 0 && jobs.refresh_tick == 0);
    f.match->finish_load();
    CHECK(local->next_economy_tick == 0 && jobs.refresh_tick == 0);
    f.match->run_load_pass();
    CHECK(local->next_economy_tick == 30 && jobs.refresh_tick == 1);
    CHECK(local->energy == 300 && local->metal == 200 && local->energy_produced == 25);
    f.match->run_load_pass();
    f.match->finish_load();
    f.match->run_load_pass();
    CHECK(local->next_economy_tick == 30 && jobs.refresh_tick == 1);
    ++f.match->simulation().tick;
    f.match->tick();
    CHECK(local->next_economy_tick == 30 && local->energy == 300 && local->metal == 200);
    CHECK(jobs.refresh_tick == 2);
}

// A waiting load's pass runs first in the match's first tick, at the load's
// tick though the clock has moved on: a unit waiting for a path search is
// asked at tick 100, where the match loaded, and the clock is back at 101
// for the tick.
void the_first_tick_runs_a_waiting_load_pass() {
    Fixture f;
    auto& mover = f.spawn(tank, 4, 4);
    auto* runtime = f.match->ground_runtime(mover.unit_index);
    CHECK(runtime != nullptr);
    runtime->navigation.flags |= sim::ground_orders::search_pending_flag;
    f.match->simulation().tick = 100;
    f.match->finish_load();
    CHECK(runtime->navigation.last_search_tick == 0);
    f.match->simulation().tick = 101;
    f.match->tick();
    CHECK(runtime->navigation.last_search_tick == 100);
    CHECK(f.match->simulation().tick == 101 && f.match->path_search_jobs().refresh_tick == 2);
}
} // namespace

int main() {
    building_walls_every_class_until_removed();
    features_refresh_every_class();
    persistent_search_matches_the_job_build();
    cached_planning_excludes_only_movement_objects_in_eight();
    static_eight_planning_does_not_weaken_physical_traffic();
    restored_mobility_keeps_the_occupancy_tick();
    outcome_checks_follow_the_slot_deadline();
    the_load_ends_with_one_player_pass();
    the_first_tick_runs_a_waiting_load_pass();
    return 0;
}
