// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "group_order_static.hpp"

#include "oa/test/check.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {
namespace input = oa::sim::gameplay_input;
namespace ground = oa::sim::ground_orders;
namespace spatial = oa::sim::spatial_state;
namespace detail = oa::app::group_order_static_detail;

constexpr int32_t pixel = 65536;
constexpr uint32_t width = 32;
constexpr uint32_t height = 24;
constexpr uint8_t land_height = 60;
constexpr uint8_t sea_level = 30;
constexpr std::array supported_orders{
    input::UnitOrder::move_ground, input::UnitOrder::patrol, input::UnitOrder::repair_patrol
};

oa::FixedVec3 point(int32_t x, int32_t z, int32_t y = 17) {
    return {x * pixel, y * pixel, z * pixel};
}

bool same(const oa::FixedVec3& left, const oa::FixedVec3& right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

oa::Unit actor(int16_t footprint_x = 2, int16_t footprint_z = 2) {
    oa::Unit result{};
    result.type_index = 1;
    result.movement = 1;
    result.flags = OA_UNIT_FLAG_LIVE | 1;
    result.footprint_x = footprint_x;
    result.footprint_z = footprint_z;
    return result;
}

struct Scene {
    spatial::World world;
    std::array<spatial::Unit, 3> occupants{};
    spatial::CellClassifier movement{2, 2, 0, -10000, 6, 3, 8, 4, 500};

    Scene() {
        world.terrain_width = width;
        world.terrain_height = height;
        world.plots.resize(width * height);
        world.units = occupants;
        world.sea_level = sea_level;
        world.tick = 700;
        for (auto& plot : world.plots)
            plot.high_height = plot.low_height = land_height;
        occupants[1].id = 1;
        occupants[1].object_present = true;
        occupants[1].owner_object_present = true;
        occupants[1].owner_status = 1;
        occupants[1].object_tick = 0;
        occupants[2].id = 2;
        occupants[2].object_present = false;
        occupants[2].flags = OA_UNIT_FLAG_BUILDING | 1;
    }

    spatial::Plot& plot(int32_t x, int32_t z) {
        return world.plots.at(static_cast<std::size_t>(z) * width + x);
    }

    void block_region(const ground::Goal& goal, const oa::Unit& unit, bool blocked = true) {
        for (int32_t z = goal.cell[1] - 1; z <= goal.cell[1] + unit.footprint_z; ++z)
            for (int32_t x = goal.cell[0] - 1; x <= goal.cell[0] + unit.footprint_x; ++x)
                plot(x, z).blocking_feature = blocked;
    }

    void clear_footprint(int32_t x, int32_t z, const oa::Unit& unit) {
        for (int32_t dz = 0; dz < unit.footprint_z; ++dz)
            for (int32_t dx = 0; dx < unit.footprint_x; ++dx)
                plot(x + dx, z + dz).blocking_feature = false;
    }

    oa::FixedVec3 resolve(
        const oa::FixedVec3& formation,
        const oa::FixedVec3& click,
        const oa::Unit& unit,
        input::UnitOrder order = input::UnitOrder::move_ground,
        bool enabled = true
    ) const {
        return oa::app::static_group_order_point(
            formation, click, unit, order, movement, world, width * 16, height * 16, enabled
        );
    }
};

ground::Goal goal_for(
    oa::sim::simulation_state::Order& scratch,
    const oa::Unit& unit,
    input::UnitOrder order,
    const oa::FixedVec3& destination
) {
    const auto goal = detail::arrival_goal(scratch, unit, order, destination);
    OA_CHECK(goal.has_value());
    return goal.value_or(ground::Goal{});
}

void unusable_formations_use_the_exact_click() {
    const auto unit = actor();
    const auto formation = point(128, 96, -23);
    const auto click = oa::FixedVec3{304 * pixel + 123, 76543, 240 * pixel + 456};
    for (const auto order : supported_orders) {
        Scene scene;
        oa::sim::simulation_state::Order scratch{};
        const auto formation_goal = goal_for(scratch, unit, order, formation);
        scene.block_region(formation_goal, unit);
        OA_CHECK(detail::has_static_arrival(formation_goal, scene.movement, scene.world) == false);
        OA_CHECK(same(scene.resolve(formation, click, unit, order), click));

        // Deep water supplies no ground footprint even without a blocking feature.
        scene.block_region(formation_goal, unit, false);
        for (int32_t z = formation_goal.cell[1] - 1; z <= formation_goal.cell[1] + unit.footprint_z;
             ++z)
            for (int32_t x = formation_goal.cell[0] - 1;
                 x <= formation_goal.cell[0] + unit.footprint_x;
                 ++x)
                scene.plot(x, z).high_height = scene.plot(x, z).low_height = 0;
        OA_CHECK(same(scene.resolve(formation, click, unit, order), click));
    }
}

void legal_formations_and_two_unusable_regions_are_preserved() {
    const auto unit = actor();
    const auto formation = oa::FixedVec3{128 * pixel + 137, -76543, 96 * pixel + 919};
    const auto click = point(304, 240, 23);
    for (const auto order : supported_orders) {
        Scene scene;
        OA_CHECK(same(scene.resolve(formation, click, unit, order), formation));
        oa::sim::simulation_state::Order scratch{};
        const auto formation_goal = goal_for(scratch, unit, order, formation);
        const auto click_goal = goal_for(scratch, unit, order, click);
        scene.block_region(click_goal, unit);
        OA_CHECK(same(scene.resolve(formation, click, unit, order), formation));
        scene.block_region(formation_goal, unit);
        OA_CHECK(same(scene.resolve(formation, click, unit, order), formation));

        // An isolated legal endpoint stays legal; this policy does not search a route.
        scene.clear_footprint(formation_goal.cell[0], formation_goal.cell[1], unit);
        OA_CHECK(same(scene.resolve(formation, click, unit, order), formation));
    }
}

void repair_patrol_uses_the_whole_native_arrival_region() {
    Scene scene;
    const auto unit = actor();
    const auto formation = point(128, 96);
    const auto click = point(304, 240);
    oa::sim::simulation_state::Order scratch{};
    const auto repair = goal_for(scratch, unit, input::UnitOrder::repair_patrol, formation);
    scene.block_region(repair, unit);
    scene.clear_footprint(repair.cell[0] + 1, repair.cell[1], unit);
    OA_CHECK(
        !ground::static_flow_passable(scene.movement, repair.cell[0], repair.cell[1], scene.world)
    );
    OA_CHECK(ground::goal_contains(repair, repair.cell[0] + 1, repair.cell[1]));
    OA_CHECK(
        ground::static_flow_passable(
            scene.movement, repair.cell[0] + 1, repair.cell[1], scene.world
        )
    );
    OA_CHECK(
        same(scene.resolve(formation, click, unit, input::UnitOrder::repair_patrol), formation)
    );
    OA_CHECK(same(scene.resolve(formation, click, unit, input::UnitOrder::move_ground), click));
    OA_CHECK(same(scene.resolve(formation, click, unit, input::UnitOrder::patrol), click));

    // A diagonal clear footprint is outside the circular one-cell arrival region.
    scene.block_region(repair, unit);
    scene.clear_footprint(repair.cell[0] + 1, repair.cell[1] + 1, unit);
    OA_CHECK(!ground::goal_contains(repair, repair.cell[0] + 1, repair.cell[1] + 1));
    OA_CHECK(same(scene.resolve(formation, click, unit, input::UnitOrder::repair_patrol), click));
}

void canonical_footprints_override_movement_metadata() {
    const auto formation = point(128, 96);
    const auto click = point(304, 240);
    oa::sim::simulation_state::Order scratch{};
    {
        Scene scene;
        const auto unit = actor(3, 2);
        scene.movement.footprint_x = scene.movement.footprint_z = 1;
        const auto goal = goal_for(scratch, unit, input::UnitOrder::move_ground, formation);
        scene.plot(goal.cell[0] + 2, goal.cell[1] + 1).blocking_feature = true;
        OA_CHECK(
            ground::static_flow_passable(scene.movement, goal.cell[0], goal.cell[1], scene.world)
        );
        OA_CHECK(same(scene.resolve(formation, click, unit), click));
    }
    {
        Scene scene;
        const auto unit = actor(1, 1);
        scene.movement.footprint_x = scene.movement.footprint_z = 4;
        const auto goal = goal_for(scratch, unit, input::UnitOrder::move_ground, formation);
        scene.plot(goal.cell[0] + 1, goal.cell[1]).blocking_feature = true;
        OA_CHECK(
            !ground::static_flow_passable(scene.movement, goal.cell[0], goal.cell[1], scene.world)
        );
        OA_CHECK(same(scene.resolve(formation, click, unit), formation));
    }
}

void mobile_occupants_are_ignored_and_buildings_still_block() {
    Scene scene;
    const auto unit = actor();
    const auto formation = point(128, 96);
    const auto click = point(304, 240);
    oa::sim::simulation_state::Order scratch{};
    const auto formation_goal = goal_for(scratch, unit, input::UnitOrder::move_ground, formation);
    const auto click_goal = goal_for(scratch, unit, input::UnitOrder::move_ground, click);
    auto& occupied = scene.plot(formation_goal.cell[0], formation_goal.cell[1]);
    occupied.ground = 1;
    // This old mobile occupancy blocks the ordinary projected classifier.
    OA_CHECK(
        spatial::classify_movement_cell(
            scene.movement, formation_goal.cell[0], formation_goal.cell[1], scene.world
        ) == 0
    );
    OA_CHECK(same(scene.resolve(formation, click, unit), formation));
    occupied.ground = 2;
    OA_CHECK(same(scene.resolve(formation, click, unit), click));
    scene.plot(click_goal.cell[0], click_goal.cell[1]).ground = 1;
    OA_CHECK(same(scene.resolve(formation, click, unit), click));
    scene.plot(click_goal.cell[0], click_goal.cell[1]).ground = 2;
    OA_CHECK(same(scene.resolve(formation, click, unit), formation));
}

void water_depth_and_slope_metadata_are_respected() {
    const auto unit = actor(1, 1);
    const auto formation = point(128, 96);
    const auto click = point(304, 240);
    oa::sim::simulation_state::Order scratch{};
    const auto goal = goal_for(scratch, unit, input::UnitOrder::move_ground, formation);
    Scene scene;
    auto& plot = scene.plot(goal.cell[0], goal.cell[1]);
    plot.high_height = land_height + 7;
    OA_CHECK(same(scene.resolve(formation, click, unit), click));
    plot.high_height = land_height + 5;
    OA_CHECK(same(scene.resolve(formation, click, unit), formation));

    plot.high_height = plot.low_height = 20;
    scene.movement.max_water_depth = 8;
    OA_CHECK(same(scene.resolve(formation, click, unit), click));
    scene.movement.max_water_depth = 20;
    OA_CHECK(same(scene.resolve(formation, click, unit), formation));
    plot.high_height = 29;
    OA_CHECK(same(scene.resolve(formation, click, unit), click));
    plot.high_height = 27;
    OA_CHECK(same(scene.resolve(formation, click, unit), formation));

    // A water-only movement class can use the click but cannot arrive on land.
    const auto click_goal = goal_for(scratch, unit, input::UnitOrder::move_ground, click);
    auto& click_plot = scene.plot(click_goal.cell[0], click_goal.cell[1]);
    click_plot.high_height = click_plot.low_height = 20;
    plot.high_height = plot.low_height = land_height;
    scene.movement.min_water_depth = 2;
    OA_CHECK(same(scene.resolve(formation, click, unit), click));
}

void the_existing_bounds_policy_runs_before_static_validation() {
    Scene scene;
    const auto unit = actor();
    const auto outside = point(-8, 96, -23);
    const auto click = point(304, 240, 23);
    const auto bounded =
        oa::app::bounded_group_order_point(outside, click, unit, width * 16, height * 16, true);
    OA_CHECK(same(bounded, click));
    oa::sim::simulation_state::Order scratch{};
    const auto click_goal = goal_for(scratch, unit, input::UnitOrder::move_ground, click);
    scene.block_region(click_goal, unit);
    // Static validation cannot undo the established bounds fallback.
    OA_CHECK(same(scene.resolve(bounded, click, unit), click));

    const auto invalid_click = point(-24, 240);
    const auto unchanged = oa::app::bounded_group_order_point(
        outside, invalid_click, unit, width * 16, height * 16, true
    );
    OA_CHECK(same(unchanged, outside));
    OA_CHECK(same(scene.resolve(unchanged, invalid_click, unit), outside));
}

void padded_native_terrain_does_not_make_an_outside_click_playable() {
    Scene scene;
    const auto unit = actor();
    const auto formation = point(128, 96);
    const auto click = point(400, 240);
    constexpr int32_t playable_width = 384;
    constexpr int32_t playable_height = 320;
    oa::sim::simulation_state::Order scratch{};
    for (const auto order : supported_orders) {
        const auto formation_goal = goal_for(scratch, unit, order, formation);
        const auto click_goal = goal_for(scratch, unit, order, click);
        scene.block_region(formation_goal, unit);
        OA_CHECK(detail::has_static_arrival(click_goal, scene.movement, scene.world) == true);
        OA_CHECK(
            !oa::app::group_order_detail::axis_fits(click.x, unit.footprint_x, playable_width)
        );
        const auto bounded = oa::app::bounded_group_order_point(
            formation, click, unit, playable_width, playable_height, true
        );
        OA_CHECK(same(bounded, formation));
        OA_CHECK(same(
            oa::app::static_group_order_point(
                bounded,
                click,
                unit,
                order,
                scene.movement,
                scene.world,
                playable_width,
                playable_height,
                true
            ),
            formation
        ));
    }
}

void disabled_policy_ineligible_actors_and_other_orders_preserve_every_bit() {
    Scene scene;
    const auto unit = actor();
    const auto formation = oa::FixedVec3{128 * pixel + 17, -12345, 96 * pixel + 91};
    const auto click = point(304, 240);
    oa::sim::simulation_state::Order scratch{};
    scene.block_region(goal_for(scratch, unit, input::UnitOrder::move_ground, formation), unit);
    for (const auto order : supported_orders)
        OA_CHECK(same(scene.resolve(formation, click, unit, order, false), formation));
    const auto extreme = oa::FixedVec3{
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::min()
    };
    OA_CHECK(
        same(scene.resolve(extreme, click, unit, input::UnitOrder::move_ground, false), extreme)
    );
    for (uint32_t excluded = 0; excluded < 6; ++excluded) {
        auto ineligible = unit;
        switch (excluded) {
        case 0:
        case 1:
        case 2:
            ineligible.flags = (ineligible.flags & ~OA_UNIT_FLAG_OCCUPANCY_MASK) |
                               (excluded == 0 ? 0 : excluded + 1);
            break;
        case 3:
            ineligible.attach_parent = 1;
            break;
        case 4:
            ineligible.footprint_x = 0;
            break;
        default:
            ineligible.footprint_z = -1;
            break;
        }
        OA_CHECK(same(scene.resolve(formation, click, ineligible), formation));
    }
    for (const auto order :
         {input::UnitOrder::none,
          input::UnitOrder::vtol_move,
          input::UnitOrder::qmove,
          input::UnitOrder::attack_chase,
          input::UnitOrder::vtol_patrol,
          input::UnitOrder::vtol_repair_patrol,
          input::UnitOrder::qpatrol,
          input::UnitOrder::stop,
          input::UnitOrder::mobile_build})
        OA_CHECK(same(scene.resolve(formation, click, unit, order), formation));
}

template <class T>
std::vector<std::byte> representation(const T& value) {
    const auto bytes = std::as_bytes(std::span<const T>(&value, 1));
    return {bytes.begin(), bytes.end()};
}

template <class T>
std::vector<std::byte> representations(std::span<const T> values) {
    const auto bytes = std::as_bytes(values);
    return {bytes.begin(), bytes.end()};
}

void endpoint_checks_do_not_refresh_or_mutate_projection() {
    Scene scene;
    const auto unit = actor();
    const auto formation = point(128, 96);
    const auto click = point(304, 240);
    oa::sim::simulation_state::Order scratch{};
    const auto goal = goal_for(scratch, unit, input::UnitOrder::move_ground, formation);
    scene.plot(goal.cell[0], goal.cell[1]).ground = 1;
    scene.world.written_occupants[0] = 7;
    scene.world.written_occupant_count = 1;
    scene.world.written_occupants_lost = true;
    scene.world.buckets.resize(2);
    scene.world.buckets[0].head = 1;
    scene.world.fill_round = 19;
    scene.world.last_filled = 2;
    uint32_t fill_calls{};
    scene.world.fill_context = &fill_calls;
    scene.world.fill_unit = [](void* context, spatial::Unit&) {
        ++*static_cast<uint32_t*>(context);
    };
    const auto world_before = representation(scene.world);
    const auto plots_before = representations(std::span<const spatial::Plot>(scene.world.plots));
    const auto occupants_before = representation(scene.occupants);
    const auto buckets_before =
        representations(std::span<const spatial::Bucket>(scene.world.buckets));
    const auto actor_before = representation(unit);
    const auto movement_before = representation(scene.movement);
    for (const auto order : supported_orders) {
        OA_CHECK(same(scene.resolve(formation, click, unit, order), formation));
        OA_CHECK(same(scene.resolve(formation, click, unit, order, false), formation));
    }
    OA_CHECK(fill_calls == 0);
    OA_CHECK(representation(scene.world) == world_before);
    OA_CHECK(representations(std::span<const spatial::Plot>(scene.world.plots)) == plots_before);
    OA_CHECK(representation(scene.occupants) == occupants_before);
    OA_CHECK(
        representations(std::span<const spatial::Bucket>(scene.world.buckets)) == buckets_before
    );
    OA_CHECK(representation(unit) == actor_before);
    OA_CHECK(representation(scene.movement) == movement_before);
}
} // namespace

int main() {
    unusable_formations_use_the_exact_click();
    legal_formations_and_two_unusable_regions_are_preserved();
    repair_patrol_uses_the_whole_native_arrival_region();
    canonical_footprints_override_movement_metadata();
    mobile_occupants_are_ignored_and_buildings_still_block();
    water_depth_and_slope_metadata_are_respected();
    the_existing_bounds_policy_runs_before_static_validation();
    padded_native_terrain_does_not_make_an_outside_click_playable();
    disabled_policy_ineligible_actors_and_other_orders_preserve_every_bit();
    endpoint_checks_do_not_refresh_or_mutate_projection();
    return oa::test::check_exit_status();
}
