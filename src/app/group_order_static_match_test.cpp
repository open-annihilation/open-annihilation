// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Static group destinations use the same arrival geometry as actual ground
// missions and keep an unusable formation from stranding a native order.
#include "group_order_static.hpp"

#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/script_vm.hpp"
#include "oa/test/check.hpp"
#include "oa/test/match_services.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
namespace input = oa::sim::gameplay_input;
namespace runtime = oa::sim::match_runtime;
namespace ground = oa::sim::ground_orders;
namespace spatial = oa::sim::spatial_state;

constexpr int32_t pixel = 65536;
constexpr uint32_t map_cells = 24;
constexpr int32_t map_pixels = static_cast<int32_t>(map_cells) * 16;
constexpr uint32_t map_plots = map_cells * map_cells;
constexpr uint32_t sight_cells = map_cells / 2;
constexpr uint8_t land_height = 60;
constexpr uint8_t sea_level = 30;
constexpr uint8_t ground_layer = 1;
constexpr uint8_t repair_patrol_kind = 34;
constexpr uint16_t initial_heading = 16384;
constexpr uint32_t battle_version = oa::data::enhanced_mode::battle_flow_version;
constexpr uint32_t arrival_limit_ticks = 200;
constexpr int32_t start_x = 64;
constexpr int32_t lane_z = 192;
constexpr std::array orders = {
    input::UnitOrder::move_ground, input::UnitOrder::patrol, input::UnitOrder::repair_patrol
};

/// Converts whole map pixels to the native signed fixed-point destination.
oa::FixedVec3 point(int32_t x, int32_t z) {
    return {x * pixel, land_height * pixel, z * pixel};
}

/// Compares every destination bit, including height.
bool same_point(const oa::FixedVec3& left, const oa::FixedVec3& right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

/// Compares all arrival geometry while leaving each Goal's order pointer local.
void check_geometry(const ground::Goal& actual, const ground::Goal& expected) {
    OA_CHECK(actual.shape == expected.shape);
    OA_CHECK(actual.cell == expected.cell);
    OA_CHECK(actual.tolerance == expected.tolerance);
    OA_CHECK(actual.radius_squared == expected.radius_squared);
    OA_CHECK(actual.inner_range == expected.inner_range);
    OA_CHECK(actual.inner_radius_squared == expected.inner_radius_squared);
    OA_CHECK(actual.left == expected.left && actual.right == expected.right);
    OA_CHECK(actual.top == expected.top && actual.bottom == expected.bottom);
}

/// Holds one native mobile on flat land with an optional impassable water patch.
struct Scene {
    oa::formats::tnt::Map map;
    std::array<oa::sim::visibility_state::TerrainCell, map_plots> terrain{};
    std::array<spatial::Plot, map_plots> plots{};
    std::array<oa::sim::visibility_state::SightMask, 1> sight_masks{};
    std::array<oa::sim::visibility_state::AltitudeCell, sight_cells * sight_cells> altitude{};
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
    std::vector<uint8_t> yard;
    oa::test::QuietServices services;
    oa::test::EmptyScenario scenario;
    std::unique_ptr<runtime::Match> game;
    uint16_t actor{};

    /// Builds the match before issuing any helper-selected destination.
    Scene(uint32_t version, int16_t footprint_x, int16_t footprint_z, bool blocked_formation)
        : yard(static_cast<size_t>(footprint_x) * static_cast<size_t>(footprint_z), 4) {
        map.attribute_width = map.attribute_height = map_cells;
        map.attributes.resize(map_plots);
        map.sea_level = sea_level;
        for (uint32_t z = 0; z < map_cells; ++z)
            for (uint32_t x = 0; x < map_cells; ++x) {
                const size_t index = size_t(z) * map_cells + x;
                const bool water = blocked_formation && x >= 16 && x <= 20 && z >= 6 && z <= 18;
                const uint8_t height = water ? 0 : land_height;
                map.attributes[index].height = height;
                plots[index].low_height = plots[index].high_height = height;
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
        types[1].footprint_x = footprint_x;
        types[1].footprint_z = footprint_z;
        types[1].bm_code = 1;
        types[1].model = reinterpret_cast<uintptr_t>(model.get());
        types[1].cob = reinterpret_cast<uintptr_t>(script.get());
        loaded[1].model = model;
        loaded[1].script = script;
        loaded[1].type = types[1];
        loaded[1].unit_name = "GROUNDORDER";
        definition.can_move = definition.can_patrol = true;
        definition.acceleration_fixed = definition.brake_rate_fixed = pixel;
        definition.max_velocity_fixed = 2 * pixel;
        definition.turn_rate = 1024;
        metadata.footprint_x = footprint_x;
        metadata.footprint_z = footprint_z;
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
            sight_cells,
            sight_cells,
            4,
            2,
            0,
            sea_level,
            1,
            &scenario,
            {},
            plots,
            oa::sim::visibility_state::AltitudeSightData{
                sight_cells, sight_cells, altitude, altitude_patterns
            }
        };
        inputs.rules.setup.enhanced_mode.enabled = version != 0;
        inputs.rules.setup.enhanced_mode.version = version;
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
        auto* slot = match().create(
            {0, 1, {start_x * pixel, land_height * pixel, lane_z * pixel}, true, ground_layer, 0}
        );
        if (slot == nullptr || slot->unit == nullptr)
            throw std::runtime_error("cannot create the native static-goal actor");
        actor = slot->unit_index;
        match().place_unit_at(
            actor, start_x * pixel, land_height * pixel, lane_z * pixel, ground_layer
        );
        slot->record.heading = initial_heading;
        if (match().fault() != nullptr)
            throw std::runtime_error(match().fault());
    }

    /// Returns the actual match used by every command and movement tick.
    runtime::Match& match() { return *game; }

    /// Returns the canonical actor whose footprint quantizes each native Goal.
    const oa::Unit& unit() { return match().spawn_slot(actor).record; }

    /// Resolves the same water and slope fields as the actor's movement class.
    spatial::CellClassifier classifier() const {
        return {
            metadata.footprint_x,
            metadata.footprint_z,
            metadata.max_water_depth,
            metadata.min_water_depth,
            metadata.max_slope,
            metadata.bad_slope,
            metadata.max_water_slope,
            metadata.bad_water_slope,
            0
        };
    }

    /// Advances full unit, mission and movement ticks without replacing routes.
    void run() {
        ++match().simulation().tick;
        match().tick();
        if (match().fault() != nullptr)
            throw std::runtime_error(match().fault());
    }

    /// Issues the matching native command and returns its first installed Goal.
    ground::Goal first_goal(input::UnitOrder order, const oa::FixedVec3& destination) {
        const ground::Point at{destination.x, destination.y, destination.z};
        switch (order) {
        case input::UnitOrder::move_ground:
            (void)match().issue_ground_move(actor, at, false);
            break;
        case input::UnitOrder::patrol:
            (void)match().issue_patrol(actor, at, false);
            break;
        case input::UnitOrder::repair_patrol:
            (void)match().issue_order(actor, repair_patrol_kind, false, 0, &at, 0, 0);
            break;
        default:
            throw std::runtime_error("unsupported native static-goal order");
        }
        for (uint32_t tick = 0; tick < 4; ++tick) {
            run();
            const auto* native = match().ground_runtime(actor);
            if (native != nullptr && native->navigation.goal != nullptr) {
                OA_CHECK(native->navigation.goal->order == match().spawn_slot(actor).unit->primary);
                return *native->navigation.goal;
            }
        }
        throw std::runtime_error("native command did not create its first Goal");
    }

    /// Exhaustively checks actual arrival cells against native static passability.
    bool has_legal_arrival(const ground::Goal& goal) {
        const auto movement = classifier();
        for (uint32_t z = 0; z < map_cells; ++z)
            for (uint32_t x = 0; x < map_cells; ++x)
                if (ground::goal_contains(goal, static_cast<int32_t>(x), static_cast<int32_t>(z)) &&
                    ground::static_flow_passable(movement, x, z, match().spatial()))
                    return true;
        return false;
    }
};

/// Matches every helper geometry field against the first real native mission Goal.
void arrival_geometry_matches_all_native_ground_commands() {
    constexpr std::array<std::array<int16_t, 2>, 4> footprints = {{{1, 1}, {2, 2}, {4, 2}, {2, 4}}};
    const auto destination =
        oa::FixedVec3{240 * pixel + pixel / 2, land_height * pixel, 176 * pixel + pixel / 4};
    for (uint32_t version = 0; version <= battle_version; ++version)
        for (const auto footprint : footprints)
            for (const auto order : orders) {
                Scene scene(version, footprint[0], footprint[1], false);
                oa::sim::simulation_state::Order scratch{};
                const auto expected = oa::app::group_order_static_detail::arrival_goal(
                    scratch, scene.unit(), order, destination
                );
                OA_CHECK(expected.has_value());
                if (!expected)
                    continue;
                const auto actual = scene.first_goal(order, destination);
                check_geometry(actual, *expected);
                const int32_t tolerance = order == input::UnitOrder::move_ground ? 4
                                          : order == input::UnitOrder::patrol    ? 0
                                                                                 : 16;
                OA_CHECK(actual.tolerance == tolerance);
                OA_CHECK(scene.has_legal_arrival(actual));
            }
}

/// Preserves historical unusable formation destinations for every earlier preset.
void earlier_presets_keep_the_original_native_formation_goal() {
    const auto formation = point(288, lane_z);
    const auto click = point(176, lane_z);
    for (uint32_t version = 0; version < battle_version; ++version)
        for (const auto order : orders) {
            Scene scene(version, 2, 2, true);
            const auto selected = oa::app::static_group_order_point(
                formation,
                click,
                scene.unit(),
                order,
                scene.classifier(),
                scene.match().spatial(),
                map_pixels,
                map_pixels,
                false
            );
            OA_CHECK(same_point(selected, formation));
            const auto native = scene.first_goal(order, selected);
            oa::sim::simulation_state::Order scratch{};
            const auto expected = oa::app::group_order_static_detail::arrival_goal(
                scratch, scene.unit(), order, formation
            );
            OA_CHECK(expected.has_value());
            if (expected)
                check_geometry(native, *expected);
            OA_CHECK(!scene.has_legal_arrival(native));
        }
}

/// Sends an unusable formation to its legal click and reaches it through native movement.
void corrected_group_destinations_reach_the_original_click() {
    const auto formation = point(288, lane_z);
    const auto click = point(176, lane_z);
    for (const auto order : orders) {
        Scene scene(battle_version, 2, 2, true);
        const auto selected = oa::app::static_group_order_point(
            formation,
            click,
            scene.unit(),
            order,
            scene.classifier(),
            scene.match().spatial(),
            map_pixels,
            map_pixels,
            true
        );
        OA_CHECK(same_point(selected, click));
        const auto native = scene.first_goal(order, selected);
        OA_CHECK(scene.has_legal_arrival(native));
        oa::sim::simulation_state::Order scratch{};
        const auto expected =
            oa::app::group_order_static_detail::arrival_goal(scratch, scene.unit(), order, click);
        OA_CHECK(expected.has_value());
        if (expected)
            check_geometry(native, *expected);
        const auto* movement = scene.match().ground_runtime(scene.actor);
        if (movement == nullptr)
            throw std::runtime_error("static-goal actor has no ground runtime");
        const int32_t initial_cost =
            ground::goal_cost(native, movement->geometry.cell[0], movement->geometry.cell[1]);
        int32_t lowest_cost = initial_cost;
        bool arrived =
            ground::goal_contains(native, movement->geometry.cell[0], movement->geometry.cell[1]);
        for (uint32_t tick = 0; tick < arrival_limit_ticks && !arrived; ++tick) {
            scene.run();
            const auto& cell = movement->geometry.cell;
            lowest_cost = std::min(lowest_cost, ground::goal_cost(native, cell[0], cell[1]));
            arrived = ground::goal_contains(native, cell[0], cell[1]);
            OA_CHECK(
                ground::static_flow_passable(
                    scene.classifier(), cell[0], cell[1], scene.match().spatial()
                )
            );
        }
        OA_CHECK(lowest_cost < initial_cost);
        OA_CHECK(arrived);
    }
}

/// Runs every native case even when another case reports an exception.
template <typename Function>
void run_case(const char* name, Function function) {
    try {
        function();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s: %s\n", name, error.what());
        OA_CHECK(false);
    }
}
} // namespace

/// Runs native arrival geometry, compatibility and movement checks.
int main() {
    run_case("native arrival geometry", arrival_geometry_matches_all_native_ground_commands);
    run_case("earlier formation goals", earlier_presets_keep_the_original_native_formation_goal);
    run_case("corrected native movement", corrected_group_destinations_reach_the_original_click);
    return oa::test::check_exit_status();
}
