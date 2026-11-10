// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "group_order_distribution.hpp"
#include "group_order_static.hpp"

#include "oa/test/check.hpp"

#include <algorithm>
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
constexpr int32_t cell_pixels = 16;
constexpr uint8_t land_height = 60;
constexpr uint8_t sea_level = 30;
constexpr uint32_t ground_layer = 1;
constexpr uint32_t default_width = 48;
constexpr uint32_t default_height = 40;
constexpr uint32_t default_cells = 1048576;
constexpr uint32_t default_origins = 262144;

/// Forms a command point without relying on floating-point quantization.
oa::FixedVec3 point(int32_t x, int32_t z, int32_t y = 17) {
    return {x * pixel, y * pixel, z * pixel};
}

/// Compares every destination bit, including its signed height.
bool same(const oa::FixedVec3& left, const oa::FixedVec3& right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

/// Stores every signed fixed-point source coordinate unchanged.
void source_position(oa::Unit& unit, const oa::FixedVec3& source) {
    unit.position = source;
}

/// Compares destination sets independently of actor assignment or issue order.
std::vector<std::array<int32_t, 3>> site_set(std::span<const input::SelectionOrder> orders) {
    std::vector<std::array<int32_t, 3>> result;
    for (const auto& order : orders)
        result.push_back({order.position.x, order.position.y, order.position.z});
    std::sort(result.begin(), result.end());
    return result;
}

/// Builds an eligible canonical ground actor with its own stable identifier.
oa::Unit actor(uint16_t id, int16_t footprint_x = 2, int16_t footprint_z = 2) {
    oa::Unit result{};
    result.id = id;
    result.type_index = 1;
    result.movement = 1;
    result.flags = OA_UNIT_FLAG_LIVE | ground_layer;
    result.footprint_x = footprint_x;
    result.footprint_z = footprint_z;
    return result;
}

/// Retains an object's actual bytes without copying or interpreting its padding.
template <class T>
std::vector<std::byte> representation(const T& value) {
    const auto bytes = std::as_bytes(std::span{&value, std::size_t{1}});
    return {bytes.begin(), bytes.end()};
}

/// Retains the complete backing storage of a canonical spatial table.
template <class T>
std::vector<std::byte> representations(std::span<const T> values) {
    const auto bytes = std::as_bytes(values);
    return {bytes.begin(), bytes.end()};
}

/// Holds passable land, native occupancy records and the resolved actor limits.
struct Scene {
    spatial::World world;
    std::array<spatial::Unit, 3> occupants{};
    spatial::CellClassifier movement{1, 1, 0, -10000, 6, 3, 8, 4, 500};
    bool amphibious_second_type{};

    /// Sizes the terrain independently of the planner's bounded scratch window.
    explicit Scene(uint32_t width = default_width, uint32_t height = default_height) {
        world.terrain_width = width;
        world.terrain_height = height;
        world.plots.resize(static_cast<std::size_t>(width) * height);
        world.units = occupants;
        world.sea_level = sea_level;
        world.tick = 700;
        for (auto& plot : world.plots)
            plot.high_height = plot.low_height = land_height;
        occupants[1].id = 1;
        occupants[1].object_present = true;
        occupants[1].owner_object_present = true;
        occupants[1].owner_status = 1;
        occupants[2].id = 2;
        occupants[2].object_present = false;
        occupants[2].flags = OA_UNIT_FLAG_BUILDING | ground_layer;
    }

    /// Accesses one actual terrain plot for a stated obstacle arrangement.
    spatial::Plot& plot(int32_t x, int32_t z) {
        return world.plots.at(static_cast<std::size_t>(z) * world.terrain_width + x);
    }

    /// Sets a rectangular static obstacle, using half-open plot coordinates.
    void block(int32_t left, int32_t top, int32_t right, int32_t bottom, bool blocked = true) {
        for (int32_t z = top; z < bottom; ++z)
            for (int32_t x = left; x < right; ++x)
                plot(x, z).blocking_feature = blocked;
    }

    /// Supplies the resolved limits while deliberately leaving footprints to the actor.
    static bool
    classifier(void* context, const oa::Unit& unit, spatial::CellClassifier& out) noexcept {
        const auto& scene = *static_cast<const Scene*>(context);
        out = scene.movement;
        if (scene.amphibious_second_type && unit.type_index == 2)
            out.max_water_depth = 100;
        return true;
    }

    /// Resolves a command without running the game loop or issuing its orders.
    oa::app::GroupDestinationReport resolve(
        std::span<input::SelectionOrder> orders,
        const oa::FixedVec3& click,
        oa::app::GroupDestinationLimits limits = {}
    ) {
        return oa::app::distribute_group_order_points(
            orders,
            click,
            world,
            static_cast<int32_t>(world.terrain_width) * cell_pixels,
            static_cast<int32_t>(world.terrain_height) * cell_pixels,
            {this, classifier},
            limits
        );
    }
};

/// Obtains native arrival geometry and removes its unused local order pointer.
ground::Goal goal_for(const input::SelectionOrder& order) {
    oa::sim::simulation_state::Order scratch{};
    const auto goal = detail::arrival_goal(scratch, *order.actor, order.order, order.position);
    OA_CHECK(goal.has_value());
    auto result = goal.value_or(ground::Goal{});
    result.order = nullptr;
    return result;
}

/// Bounds all possible native arrival footprints, including RepairPatrol's cross.
std::array<int32_t, 4> arrival_rectangle(const input::SelectionOrder& order) {
    const auto goal = goal_for(order);
    const int32_t radius = order.order == input::UnitOrder::repair_patrol ? 1 : 0;
    OA_CHECK(goal.radius_squared == radius);
    return {
        goal.cell[0] - radius,
        goal.cell[1] - radius,
        goal.cell[0] + order.actor->footprint_x + radius,
        goal.cell[1] + order.actor->footprint_z + radius
    };
}

/// Checks that two complete arrival rectangles share no occupied plot.
void check_disjoint(const input::SelectionOrder& left, const input::SelectionOrder& right) {
    const auto a = arrival_rectangle(left);
    const auto b = arrival_rectangle(right);
    OA_CHECK(a[2] <= b[0] || b[2] <= a[0] || a[3] <= b[1] || b[3] <= a[1]);
}

/// Checks the native coordinate round trip and an actual legal arrival footprint.
void check_candidate(
    const input::SelectionOrder& order, const oa::FixedVec3& click, const Scene& scene
) {
    const auto goal = goal_for(order);
    const auto& unit = *order.actor;
    OA_CHECK(
        order.position.x == (int32_t(goal.cell[0]) * cell_pixels + unit.footprint_x * 8) * pixel
    );
    OA_CHECK(
        order.position.z == (int32_t(goal.cell[1]) * cell_pixels + unit.footprint_z * 8) * pixel
    );
    OA_CHECK(order.position.y == click.y);
    auto movement = scene.movement;
    movement.footprint_x = unit.footprint_x;
    movement.footprint_z = unit.footprint_z;
    OA_CHECK(detail::has_static_arrival(goal, movement, scene.world) == true);
    OA_CHECK(
        oa::app::group_order_detail::axis_fits(
            order.position.x, unit.footprint_x, static_cast<int32_t>(scene.world.terrain_width) * 16
        )
    );
    OA_CHECK(
        oa::app::group_order_detail::axis_fits(
            order.position.z,
            unit.footprint_z,
            static_cast<int32_t>(scene.world.terrain_height) * 16
        )
    );
}

/// Reserves a later valid point before either earlier repaired actor can take it.
void later_valid_points_are_exact_and_cannot_be_stolen() {
    Scene scene;
    const std::array units{actor(9), actor(3), actor(17)};
    const auto click = point(128, 128, 41);
    const oa::FixedVec3 valid{click.x + 137, -76543, click.z + 911};
    std::array orders{
        input::SelectionOrder{&units[0], input::UnitOrder::patrol, point(-64, -64)},
        input::SelectionOrder{&units[1], input::UnitOrder::patrol, point(-96, -96)},
        input::SelectionOrder{&units[2], input::UnitOrder::patrol, valid}
    };
    const auto before = orders;
    const auto report = scene.resolve(orders, click);
    OA_CHECK(report.preserved == 1);
    OA_CHECK(report.repaired == 2);
    OA_CHECK(report.assigned == 2);
    OA_CHECK(!report.abandoned && !report.exhausted);
    OA_CHECK(same(orders[2].position, valid));
    for (std::size_t i = 0; i < orders.size(); ++i) {
        OA_CHECK(orders[i].actor == before[i].actor);
        OA_CHECK(orders[i].order == before[i].order);
        if (i < 2)
            check_candidate(orders[i], click, scene);
        for (std::size_t j = 0; j < i; ++j)
            check_disjoint(orders[i], orders[j]);
    }

    auto reordered = std::array{before[2], before[1], before[0]};
    const auto repeat = scene.resolve(reordered, click);
    OA_CHECK(repeat.assigned == 2);
    for (const auto& order : orders) {
        const auto same_actor =
            std::find_if(reordered.begin(), reordered.end(), [&](const auto& item) {
                return item.actor->id == order.actor->id;
            });
        OA_CHECK(same_actor != reordered.end());
        if (same_actor != reordered.end())
            OA_CHECK(same(order.position, same_actor->position));
    }
}

/// Keeps every valid XYZ, including duplicate valid aliases, without inventing repairs.
void valid_points_and_existing_aliases_remain_unchanged() {
    Scene scene;
    const std::array units{actor(1), actor(2), actor(3)};
    const oa::FixedVec3 first{128 * pixel + 137, -456789, 128 * pixel + 991};
    const oa::FixedVec3 second{192 * pixel + 99, 123456, 240 * pixel + 17};
    std::array orders{
        input::SelectionOrder{&units[0], input::UnitOrder::move_ground, first},
        input::SelectionOrder{&units[1], input::UnitOrder::move_ground, first},
        input::SelectionOrder{&units[2], input::UnitOrder::patrol, second}
    };
    const auto before = orders;
    const auto report = scene.resolve(orders, point(400, 400));
    OA_CHECK(report.preserved == 3 && report.repaired == 0 && report.assigned == 0);
    for (std::size_t i = 0; i < orders.size(); ++i)
        OA_CHECK(same(orders[i].position, before[i].position));
}

/// Uses full mixed footprints and native RepairPatrol arrival rectangles.
void mixed_footprints_and_repair_patrol_do_not_overlap() {
    Scene scene;
    const std::array units{actor(4, 1, 1), actor(2, 3, 2), actor(9, 2, 3), actor(7, 2, 2)};
    const auto click = point(320, 288, -31);
    std::array orders{
        input::SelectionOrder{&units[0], input::UnitOrder::patrol, point(-64, -64)},
        input::SelectionOrder{&units[1], input::UnitOrder::move_ground, point(-96, -96)},
        input::SelectionOrder{&units[2], input::UnitOrder::repair_patrol, point(-128, -128)},
        input::SelectionOrder{&units[3], input::UnitOrder::repair_patrol, point(-160, -160)}
    };
    const auto report = scene.resolve(orders, click);
    OA_CHECK(report.repaired == orders.size() && report.assigned == orders.size());
    OA_CHECK(!report.abandoned && !report.exhausted);
    for (std::size_t i = 0; i < orders.size(); ++i) {
        check_candidate(orders[i], click, scene);
        for (std::size_t j = 0; j < i; ++j)
            check_disjoint(orders[i], orders[j]);
    }
}

/// Reserves every possible footprint of a later valid RepairPatrol destination.
void valid_repair_patrol_arrivals_are_reserved_before_repairs() {
    Scene scene;
    const std::array units{actor(1), actor(2), actor(3)};
    const auto click = point(320, 288, 47);
    const oa::FixedVec3 valid{click.x + 231, -54321, click.z + 991};
    std::array orders{
        input::SelectionOrder{&units[0], input::UnitOrder::patrol, point(-64, -64)},
        input::SelectionOrder{&units[1], input::UnitOrder::patrol, point(-96, -96)},
        input::SelectionOrder{&units[2], input::UnitOrder::repair_patrol, valid}
    };
    const auto report = scene.resolve(orders, click);
    OA_CHECK(report.preserved == 1 && report.assigned == 2);
    OA_CHECK(same(orders[2].position, valid));
    for (std::size_t i = 0; i < 2; ++i) {
        check_candidate(orders[i], click, scene);
        check_disjoint(orders[i], orders[2]);
    }
    check_disjoint(orders[0], orders[1]);
}

/// Keeps equal-footprint water classes from sharing a permissive static component.
void water_limits_remain_part_of_the_component_key() {
    Scene scene;
    scene.amphibious_second_type = true;
    for (auto& plot : scene.world.plots)
        plot.high_height = plot.low_height = 0;
    for (int32_t z = 4; z < 7; ++z)
        for (int32_t x = 4; x < 7; ++x)
            scene.plot(x, z).high_height = scene.plot(x, z).low_height = land_height;
    constexpr std::size_t preserved = 9;
    std::array<oa::Unit, preserved + 2> units{};
    std::array<input::SelectionOrder, preserved + 2> orders{};
    for (std::size_t i = 0; i < units.size(); ++i)
        units[i] = actor(static_cast<uint16_t>(i + 1), 1, 1);
    units[0].type_index = 2;
    orders[0] = {&units[0], input::UnitOrder::patrol, point(-64, -64)};
    orders[1] = {&units[1], input::UnitOrder::patrol, point(-96, -96)};
    for (std::size_t i = 0; i < preserved; ++i)
        orders[i + 2] = {
            &units[i + 2],
            input::UnitOrder::patrol,
            point(
                72 + static_cast<int32_t>(i % 3) * 16,
                72 + static_cast<int32_t>(i / 3) * 16,
                -static_cast<int32_t>(i) - 1
            )
        };
    const auto before = orders;
    const auto click = point(88, 88);
    const auto report = scene.resolve(orders, click);
    OA_CHECK(report.preserved == preserved && report.repaired == 2);
    OA_CHECK(report.classes == 2);
    // Every land plot belongs to a preserved valid arrival. Only the
    // amphibious actor can take an unreserved destination outside that island.
    OA_CHECK(report.assigned == 1);
    OA_CHECK(same(orders[1].position, click));
    const auto amphibious = goal_for(orders[0]);
    OA_CHECK(scene.plot(amphibious.cell[0], amphibious.cell[1]).low_height == 0);
    auto movement = scene.movement;
    movement.max_water_depth = 100;
    OA_CHECK(detail::has_static_arrival(amphibious, movement, scene.world) == true);
    for (std::size_t i = 2; i < orders.size(); ++i) {
        OA_CHECK(same(orders[i].position, before[i].position));
        check_disjoint(orders[0], orders[i]);
    }
}

/// Gives an earlier actor the final free body even when its class is interleaved.
void stable_actor_priority_crosses_movement_classes() {
    Scene scene;
    scene.amphibious_second_type = true;
    scene.block(0, 0, default_width, default_height);
    scene.block(5, 5, 7, 6, false);
    std::array units{actor(3, 1, 1), actor(1, 1, 1), actor(2, 1, 1)};
    units[2].type_index = 2;
    const auto click = point(88, 88, -17);
    std::array orders{
        input::SelectionOrder{&units[0], input::UnitOrder::patrol, point(-64, -64)},
        input::SelectionOrder{&units[1], input::UnitOrder::patrol, point(-96, -96)},
        input::SelectionOrder{&units[2], input::UnitOrder::patrol, point(-128, -128)}
    };
    const auto report = scene.resolve(orders, click);
    OA_CHECK(report.classes == 2 && report.repaired == 3 && report.assigned == 2);
    OA_CHECK(same(orders[1].position, click));
    OA_CHECK(same(orders[2].position, point(104, 88, -17)));
    OA_CHECK(same(orders[0].position, click));
    check_disjoint(orders[1], orders[2]);
    for (std::size_t i = 0; i < orders.size(); ++i)
        OA_CHECK(orders[i].actor == &units[i]);
}

/// Keeps new points in the click's static island instead of jumping a barrier.
void a_small_click_island_exhausts_without_using_another_island() {
    Scene scene;
    scene.block(0, 0, default_width, default_height);
    scene.block(4, 4, 12, 12, false);
    scene.block(20, 4, 36, 20, false);
    const auto click = point(112, 112);
    constexpr std::size_t repaired = 20;
    std::array<oa::Unit, repaired + 1> units{};
    std::array<input::SelectionOrder, repaired + 1> orders{};
    for (std::size_t i = 0; i < repaired; ++i) {
        units[i] = actor(static_cast<uint16_t>(i + 1));
        orders[i] = {&units[i], input::UnitOrder::patrol, point(-64, -64)};
    }
    units.back() = actor(static_cast<uint16_t>(units.size()));
    const auto valid_island_point = point(400, 160, -7);
    orders.back() = {&units.back(), input::UnitOrder::patrol, valid_island_point};
    const auto report = scene.resolve(orders, click);
    OA_CHECK(report.preserved == 1 && report.repaired == repaired);
    OA_CHECK(report.assigned > 0 && report.assigned <= 16);
    OA_CHECK(report.assigned < report.repaired);
    OA_CHECK(same(orders.back().position, valid_island_point));
    for (std::size_t i = 0; i < repaired; ++i) {
        const auto rectangle = arrival_rectangle(orders[i]);
        OA_CHECK(rectangle[0] >= 4 && rectangle[1] >= 4);
        OA_CHECK(rectangle[2] <= 12 && rectangle[3] <= 12);
    }
}

/// Keeps candidates within the playable bounds even beside a map corner.
void repaired_edge_points_fit_their_complete_footprints() {
    Scene scene;
    const std::array units{actor(1, 2, 2), actor(2, 3, 2), actor(3, 2, 3)};
    const auto click = point(32, 32);
    std::array orders{
        input::SelectionOrder{&units[0], input::UnitOrder::patrol, point(-64, -64)},
        input::SelectionOrder{&units[1], input::UnitOrder::patrol, point(-96, -96)},
        input::SelectionOrder{&units[2], input::UnitOrder::patrol, point(-128, -128)}
    };
    const auto report = scene.resolve(orders, click);
    OA_CHECK(report.assigned == orders.size());
    for (std::size_t i = 0; i < orders.size(); ++i) {
        check_candidate(orders[i], click, scene);
        for (std::size_t j = 0; j < i; ++j)
            check_disjoint(orders[i], orders[j]);
    }
}

/// Leaves raw points alone when neither their native arrival nor the click is usable.
void unusable_clicks_keep_the_existing_static_fallback() {
    Scene scene;
    const std::array units{actor(1), actor(2)};
    const auto click = point(320, 288);
    const auto formation = point(128, 128, -5);
    std::array orders{
        input::SelectionOrder{&units[0], input::UnitOrder::patrol, formation},
        input::SelectionOrder{&units[1], input::UnitOrder::patrol, formation}
    };
    scene.block(0, 0, default_width, default_height);
    const auto report = scene.resolve(orders, click);
    OA_CHECK(report.assigned == 0);
    for (const auto& order : orders)
        OA_CHECK(same(order.position, formation));
}

/// Leaves unsupported large footprints at the established click fallback.
void unsupported_footprints_are_not_given_unvalidated_points() {
    Scene scene;
    const std::array units{actor(1, 9, 2), actor(2, 2, 9)};
    const auto click = point(320, 288, -4);
    std::array orders{
        input::SelectionOrder{&units[0], input::UnitOrder::patrol, point(-64, -64)},
        input::SelectionOrder{&units[1], input::UnitOrder::patrol, point(-96, -96)}
    };
    const auto report = scene.resolve(orders, click);
    OA_CHECK(report.repaired == 2 && report.assigned == 0);
    OA_CHECK(report.unsupported == 2);
    for (const auto& order : orders)
        OA_CHECK(same(order.position, click));
}

/// Reports work exhaustion and preserves the earlier fallback without partial changes.
void validation_work_caps_leave_the_complete_legacy_result() {
    const std::array limits{
        oa::app::GroupDestinationLimits{8, 0, default_cells},
        oa::app::GroupDestinationLimits{8, default_origins, 1}
    };
    for (const auto limit : limits) {
        Scene scene;
        const std::array units{actor(1), actor(2), actor(3)};
        const auto click = point(320, 288, -4);
        const auto valid = point(128, 128, 41);
        std::array orders{
            input::SelectionOrder{&units[0], input::UnitOrder::patrol, point(-64, -64)},
            input::SelectionOrder{&units[1], input::UnitOrder::patrol, point(-96, -96)},
            input::SelectionOrder{&units[2], input::UnitOrder::patrol, valid}
        };
        const auto report = scene.resolve(orders, click, limit);
        OA_CHECK(report.exhausted && report.abandoned && report.assigned == 0);
        OA_CHECK(report.origins <= limit.origins && report.cells <= limit.cells);
        OA_CHECK(same(orders[0].position, click) && same(orders[1].position, click));
        OA_CHECK(same(orders[2].position, valid));
    }
}

/// Reports a class cap instead of mixing incompatible footprint components.
void movement_class_caps_are_explicit() {
    Scene scene;
    const std::array units{actor(1, 2, 2), actor(2, 3, 2)};
    const auto click = point(320, 288);
    std::array orders{
        input::SelectionOrder{&units[0], input::UnitOrder::patrol, point(-64, -64)},
        input::SelectionOrder{&units[1], input::UnitOrder::patrol, point(-96, -96)}
    };
    const auto report = scene.resolve(orders, click, {1, default_origins, default_cells});
    OA_CHECK(report.classes <= 1);
    OA_CHECK(report.assigned < report.repaired);
    OA_CHECK(report.exhausted);
    if (report.assigned == 0)
        for (const auto& order : orders)
            OA_CHECK(same(order.position, click));
}

/// Clamps externally supplied class limits to the fixed eight-key storage.
void large_limits_do_not_expand_the_class_storage() {
    Scene scene;
    const auto click = point(320, 288);
    std::array<oa::Unit, 9> units{};
    std::array<input::SelectionOrder, 9> orders{};
    for (std::size_t i = 0; i < units.size(); ++i) {
        const auto x = static_cast<int16_t>(std::min<std::size_t>(i + 1, 8));
        units[i] = actor(static_cast<uint16_t>(i + 1), x, i == 8 ? 2 : 1);
        orders[i] = {&units[i], input::UnitOrder::patrol, point(-64, -64)};
    }
    const auto report = scene.resolve(
        orders,
        click,
        {std::numeric_limits<uint32_t>::max(),
         std::numeric_limits<uint32_t>::max(),
         std::numeric_limits<uint32_t>::max()}
    );
    OA_CHECK(report.classes <= 8);
    OA_CHECK(report.assigned < report.repaired);
    OA_CHECK(report.exhausted);
    OA_CHECK(report.origins <= default_origins && report.cells <= default_cells);
}

/// Reserves a valid footprint that reaches the window from an outside centre.
void reservations_include_rectangles_with_centres_outside_the_window() {
    constexpr uint32_t width = 400;
    constexpr uint32_t height = 12;
    constexpr std::size_t repaired = 4000;
    Scene scene(width, height);
    std::vector<oa::Unit> units(repaired + 1);
    std::vector<input::SelectionOrder> orders(repaired + 1);
    const auto click = point(3200, 80);
    for (std::size_t i = 0; i < repaired; ++i) {
        units[i] = actor(static_cast<uint16_t>(i + 1), 1, 1);
        orders[i] = {&units[i], input::UnitOrder::patrol, point(-64, -64)};
    }
    units.back() = actor(static_cast<uint16_t>(units.size()), 8, 8);
    // The 256-cell window starts at column72; this centre is at column70,
    // while its footprint [66,74) reaches two columns into the window.
    const auto valid = point(1120, 80, -19);
    orders.back() = {&units.back(), input::UnitOrder::patrol, valid};
    const auto report = scene.resolve(orders, click);
    OA_CHECK(report.preserved == 1 && report.repaired == repaired);
    // A one-cell native footprint can reach columns72..327 and rows0..10.
    // The valid footprint reserves two of those columns in eight rows.
    constexpr uint32_t available = 256 * 11 - 2 * 8;
    OA_CHECK(report.assigned == available);
    OA_CHECK(same(orders.back().position, valid));
    bool reached_left_edge = false;
    for (std::size_t i = 0; i < repaired; ++i) {
        check_disjoint(orders[i], orders.back());
        reached_left_edge |= goal_for(orders[i]).cell[0] == 72;
    }
    OA_CHECK(reached_left_edge);
}

/// Refuses to refresh spatial projections or alter any canonical backing storage.
void planning_does_not_mutate_the_world_or_actor_records() {
    Scene scene;
    const std::array units{actor(1), actor(2), actor(3)};
    uint32_t fill_calls = 0;
    scene.world.fill_context = &fill_calls;
    scene.world.fill_unit = [](void* context, spatial::Unit&) {
        ++*static_cast<uint32_t*>(context);
    };
    scene.world.fill_round = 73;
    scene.world.last_filled = 2;
    scene.plot(8, 8).ground = 1;
    const auto world_before = representation(scene.world);
    const auto plots_before = representations(std::span<const spatial::Plot>(scene.world.plots));
    const auto occupants_before = representation(scene.occupants);
    const auto actors_before = representation(units);
    const auto movement_before = representation(scene.movement);
    std::array orders{
        input::SelectionOrder{&units[0], input::UnitOrder::patrol, point(-64, -64)},
        input::SelectionOrder{&units[1], input::UnitOrder::patrol, point(-96, -96)},
        input::SelectionOrder{&units[2], input::UnitOrder::patrol, point(256, 256)}
    };
    const auto report = scene.resolve(orders, point(128, 128));
    OA_CHECK(report.assigned == 2);
    OA_CHECK(fill_calls == 0);
    OA_CHECK(representation(scene.world) == world_before);
    OA_CHECK(representations(std::span<const spatial::Plot>(scene.world.plots)) == plots_before);
    OA_CHECK(representation(scene.occupants) == occupants_before);
    OA_CHECK(representation(units) == actors_before);
    OA_CHECK(representation(scene.movement) == movement_before);
}

/// Checks explicit lateral assignments to the same five cardinal native sites.
void check_five_source_mapping(
    const std::array<oa::FixedVec3, 5>& sources,
    const std::array<std::array<int32_t, 2>, 5>& expected,
    int32_t direction_x,
    int32_t direction_z
) {
    Scene scene;
    const auto click = point(328, 296, -31);
    std::array<oa::Unit, 5> units{};
    for (std::size_t index = 0; index < units.size(); ++index) {
        units[index] = actor(static_cast<uint16_t>(index + 1), 1, 1);
        source_position(units[index], sources[index]);
    }
    const auto actors_before = representation(units);
    constexpr std::array<std::size_t, 5> issue_order{4, 1, 3, 0, 2};
    std::array<input::SelectionOrder, 5> orders{};
    for (std::size_t index = 0; index < orders.size(); ++index)
        orders[index] = {&units[issue_order[index]], input::UnitOrder::patrol, point(-64, -64)};
    const auto before = orders;
    const auto report = scene.resolve(orders, click);
    OA_CHECK(report.preserved == 0 && report.repaired == 5 && report.assigned == 5);
    OA_CHECK(!report.abandoned && !report.exhausted);
    OA_CHECK(report.forward_x == direction_x && report.forward_z == direction_z);
    OA_CHECK(report.sort_comparisons > 0 && report.rematched <= report.assigned);
    std::vector<std::array<int32_t, 3>> cardinal_sites;
    for (const auto& pixels : std::array<std::array<int32_t, 2>, 5>{
             {{328, 296}, {312, 296}, {344, 296}, {328, 280}, {328, 312}}
         }) {
        const auto at = point(pixels[0], pixels[1], -31);
        cardinal_sites.push_back({at.x, at.y, at.z});
    }
    std::sort(cardinal_sites.begin(), cardinal_sites.end());
    OA_CHECK(site_set(orders) == cardinal_sites);
    for (std::size_t index = 0; index < orders.size(); ++index) {
        const auto& order = orders[index];
        const auto& pixels = expected[order.actor->id - 1];
        OA_CHECK(same(order.position, point(pixels[0], pixels[1], -31)));
        OA_CHECK(order.actor == before[index].actor && order.order == before[index].order);
        check_candidate(order, click, scene);
        for (std::size_t previous = 0; previous < index; ++previous)
            check_disjoint(order, orders[previous]);
    }
    auto permuted = before;
    std::reverse(permuted.begin(), permuted.end());
    const auto repeated = scene.resolve(permuted, click);
    OA_CHECK(site_set(permuted) == cardinal_sites);
    OA_CHECK(repeated.forward_x == report.forward_x && repeated.forward_z == report.forward_z);
    OA_CHECK(repeated.origins == report.origins && repeated.cells == report.cells);
    OA_CHECK(repeated.reservation_cells == report.reservation_cells);
    for (const auto& order : permuted) {
        const auto& pixels = expected[order.actor->id - 1];
        OA_CHECK(same(order.position, point(pixels[0], pixels[1], -31)));
    }
    OA_CHECK(representation(units) == actors_before);
}

/// Keeps lateral order when reversed IDs approach from either horizontal side.
void lateral_rank_follows_horizontal_sources_in_both_directions() {
    constexpr std::array<std::array<int32_t, 2>, 5> expected{
        {{328, 312}, {344, 296}, {328, 296}, {312, 296}, {328, 280}}
    };
    for (const int32_t x : {120, 520}) {
        const std::array sources{
            point(x, 328), point(x, 312), point(x, 296), point(x, 280), point(x, 264)
        };
        check_five_source_mapping(sources, expected, 328 - x, 0);
    }
}

/// Uses the vertical approach's perpendicular axis rather than actor IDs.
void lateral_rank_follows_vertical_sources_in_both_directions() {
    constexpr std::array<std::array<int32_t, 2>, 5> expected{
        {{344, 296}, {328, 280}, {328, 296}, {328, 312}, {312, 296}}
    };
    for (const int32_t z : {96, 496}) {
        const std::array sources{
            point(360, z), point(344, z), point(328, z), point(312, z), point(296, z)
        };
        check_five_source_mapping(sources, expected, 0, 296 - z);
    }
}

/// Uses both integer direction components for a diagonal approach.
void lateral_rank_uses_the_full_diagonal_projection() {
    constexpr std::array<std::array<int32_t, 2>, 5> expected{
        {{328, 312}, {312, 296}, {328, 296}, {344, 296}, {328, 280}}
    };
    const std::array sources{
        point(96, 128), point(112, 112), point(128, 96), point(144, 80), point(160, 64)
    };
    check_five_source_mapping(sources, expected, 200, 200);
    const std::array signed_sources{
        point(-32, 32), point(-16, 16), point(0, 0), point(16, -16), point(32, -32)
    };
    check_five_source_mapping(signed_sources, expected, 328, 296);
}

/// Retains subpixel source ordering in both perpendicular and forward projections.
void spatial_sort_keeps_full_fixed_point_source_bits() {
    constexpr std::array<std::array<int32_t, 2>, 5> expected{
        {{328, 312}, {344, 296}, {328, 296}, {312, 296}, {328, 280}}
    };
    for (const bool lateral : {true, false}) {
        std::array<oa::FixedVec3, 5> sources{};
        for (std::size_t index = 0; index < sources.size(); ++index) {
            sources[index] = point(120, 296);
            const auto fraction = static_cast<int32_t>(sources.size() - index - 1);
            if (lateral)
                sources[index].z += fraction;
            else
                sources[index].x += fraction;
        }
        check_five_source_mapping(sources, expected, 208, 0);
    }
}

/// Breaks coincident-source ties by ID and uses positive X for a zero direction.
void coincident_sources_and_zero_direction_have_stable_ties() {
    constexpr std::array<std::array<int32_t, 2>, 5> expected{
        {{328, 280}, {312, 296}, {328, 296}, {344, 296}, {328, 312}}
    };
    std::array<oa::FixedVec3, 5> sources{};
    sources.fill(point(120, 296));
    check_five_source_mapping(sources, expected, 208, 0);
    sources.fill(point(328, 296));
    check_five_source_mapping(sources, expected, 1, 0);
}

/// Includes a preserved actor's signed canonical position in the command direction.
void direction_uses_all_command_actors_before_rematching() {
    Scene scene;
    const auto click = point(328, 296);
    std::array units{actor(1, 1, 1), actor(2, 1, 1), actor(3, 1, 1)};
    source_position(units[0], point(120, 280));
    source_position(units[1], point(120, 312));
    source_position(units[2], point(624, -4));
    const oa::FixedVec3 valid{640 * pixel + 73, -777, 496 * pixel + 91};
    std::array orders{
        input::SelectionOrder{&units[0], input::UnitOrder::patrol, point(-64, -64)},
        input::SelectionOrder{&units[1], input::UnitOrder::patrol, point(-96, -96)},
        input::SelectionOrder{&units[2], input::UnitOrder::patrol, valid}
    };
    const auto report = scene.resolve(orders, click);
    OA_CHECK(report.preserved == 1 && report.repaired == 2 && report.assigned == 2);
    OA_CHECK(report.forward_x == 40 && report.forward_z == 100);
    OA_CHECK(same(orders[0].position, click));
    OA_CHECK(same(orders[1].position, point(312, 296)));
    OA_CHECK(same(orders[2].position, valid));
    check_disjoint(orders[0], orders[1]);
}

/// Permutes only admitted IDs, keeping later actors at their capacity fallback.
void spatial_sort_retains_the_global_id_admitted_subset() {
    Scene scene;
    scene.block(0, 0, default_width, default_height);
    scene.block(5, 5, 8, 6, false);
    const auto click = point(88, 88, -17);
    std::array units{actor(3, 1, 1), actor(1, 1, 1), actor(2, 1, 1), actor(4, 1, 1)};
    source_position(units[0], point(-40, 88));
    source_position(units[1], point(-40, 104));
    source_position(units[2], point(-40, 72));
    source_position(units[3], point(-40, 88));
    std::array orders{
        input::SelectionOrder{&units[0], input::UnitOrder::patrol, point(-64, -64)},
        input::SelectionOrder{&units[1], input::UnitOrder::patrol, point(-96, -96)},
        input::SelectionOrder{&units[2], input::UnitOrder::patrol, point(-128, -128)},
        input::SelectionOrder{&units[3], input::UnitOrder::patrol, click}
    };
    const auto report = scene.resolve(orders, click);
    OA_CHECK(report.preserved == 1 && report.repaired == 3 && report.assigned == 2);
    OA_CHECK(report.exhausted);
    OA_CHECK(report.forward_x == 128 && report.forward_z == 0);
    OA_CHECK(same(orders[0].position, click));
    OA_CHECK(same(orders[1].position, point(120, 88, -17)));
    OA_CHECK(same(orders[2].position, point(104, 88, -17)));
    OA_CHECK(same(orders[3].position, click));
    check_disjoint(orders[1], orders[2]);
    check_disjoint(orders[1], orders[3]);
    check_disjoint(orders[2], orders[3]);
}

/// Retains each complete key's site set while matching its actors geometrically.
void spatial_rematching_stays_within_complete_movement_and_order_keys() {
    Scene scene;
    scene.amphibious_second_type = true;
    const auto click = point(328, 296, -41);
    std::array<oa::Unit, 7> units{};
    for (std::size_t index = 0; index < units.size(); ++index) {
        units[index] = actor(static_cast<uint16_t>(index + 1), 1, 1);
        source_position(units[index], point(120, index < 3 ? 312 : 280));
    }
    units[1].type_index = units[4].type_index = 2;
    source_position(units.back(), point(120, 296));
    const auto valid = point(640, 496, -19);
    std::array<input::SelectionOrder, 7> orders{};
    for (std::size_t index = 0; index < 6; ++index)
        orders[index] = {
            &units[index],
            index % 3 == 2 ? input::UnitOrder::move_ground : input::UnitOrder::patrol,
            point(-64, -64)
        };
    orders.back() = {&units.back(), input::UnitOrder::patrol, valid};
    const auto before = orders;
    const auto first = scene.resolve(orders, click);
    OA_CHECK(first.preserved == 1 && first.repaired == 6 && first.assigned == 6);
    OA_CHECK(first.classes == 3 && first.forward_x == 208 && first.forward_z == 0);
    OA_CHECK(same(orders.back().position, valid));
    auto changed_units = units;
    for (std::size_t index = 0; index < 6; ++index)
        source_position(changed_units[index], point(120, index < 3 ? 280 : 312));
    auto permuted = before;
    for (auto& order : permuted)
        order.actor = &changed_units[order.actor->id - 1];
    std::reverse(permuted.begin(), permuted.end());
    const auto second = scene.resolve(permuted, click);
    OA_CHECK(second.preserved == first.preserved && second.assigned == first.assigned);
    OA_CHECK(second.classes == first.classes && second.origins == first.origins);
    OA_CHECK(second.cells == first.cells && second.reservation_cells == first.reservation_cells);
    OA_CHECK(second.forward_x == first.forward_x && second.forward_z == first.forward_z);
    OA_CHECK(site_set(permuted) == site_set(orders));
    std::array<oa::FixedVec3, 7> changed_destinations{};
    for (const auto& order : permuted)
        changed_destinations[order.actor->id - 1] = order.position;
    for (std::size_t index = 0; index < 3; ++index) {
        OA_CHECK(same(changed_destinations[index], orders[index + 3].position));
        OA_CHECK(same(changed_destinations[index + 3], orders[index].position));
    }
    OA_CHECK(same(changed_destinations.back(), valid));
    for (std::size_t index = 0; index < orders.size(); ++index) {
        if (index < 6)
            check_candidate(orders[index], click, scene);
        for (std::size_t previous = 0; previous < index; ++previous)
            check_disjoint(orders[index], orders[previous]);
    }
}
} // namespace

int main() {
    later_valid_points_are_exact_and_cannot_be_stolen();
    valid_points_and_existing_aliases_remain_unchanged();
    mixed_footprints_and_repair_patrol_do_not_overlap();
    valid_repair_patrol_arrivals_are_reserved_before_repairs();
    water_limits_remain_part_of_the_component_key();
    stable_actor_priority_crosses_movement_classes();
    a_small_click_island_exhausts_without_using_another_island();
    repaired_edge_points_fit_their_complete_footprints();
    unusable_clicks_keep_the_existing_static_fallback();
    unsupported_footprints_are_not_given_unvalidated_points();
    validation_work_caps_leave_the_complete_legacy_result();
    movement_class_caps_are_explicit();
    large_limits_do_not_expand_the_class_storage();
    reservations_include_rectangles_with_centres_outside_the_window();
    planning_does_not_mutate_the_world_or_actor_records();
    lateral_rank_follows_horizontal_sources_in_both_directions();
    lateral_rank_follows_vertical_sources_in_both_directions();
    lateral_rank_uses_the_full_diagonal_projection();
    spatial_sort_keeps_full_fixed_point_source_bits();
    coincident_sources_and_zero_direction_have_stable_ties();
    direction_uses_all_command_actors_before_rematching();
    spatial_sort_retains_the_global_id_admitted_subset();
    spatial_rematching_stays_within_complete_movement_and_order_keys();
    return oa::test::check_exit_status();
}
