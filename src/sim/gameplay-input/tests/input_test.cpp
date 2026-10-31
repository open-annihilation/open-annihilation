// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/gameplay_input/input.hpp"
#include "oa/test/check.hpp"
#include <array>
#include <bit>
#include <limits>
#include <span>

int main() {
    using namespace oa::sim::gameplay_input;
    const std::array<ScreenPoint, 4> clockwise{{{0, 0}, {0, 10}, {10, 10}, {10, 0}}};
    OA_CHECK(inside_clockwise(clockwise, {5, 5}));
    OA_CHECK(!inside_clockwise(clockwise, {0, 5}));
    OA_CHECK(!inside_clockwise(clockwise, {12, 5}));
    OA_CHECK(!inside_clockwise(std::span(clockwise).first(2), {5, 5}));
    // The edge test multiplies in 32 bits: the first edge's 0x10000 * 0x8000
    // wraps negative, so the centre of this square is rejected.
    const std::array<ScreenPoint, 4> wide{{{0, 0}, {0, 0x10000}, {0x10000, 0x10000}, {0x10000, 0}}};
    OA_CHECK(!inside_clockwise(wide, {0x8000, 0x8000}));
    const std::array<ScreenPoint, 4> half{{{0, 0}, {0, 0x8000}, {0x8000, 0x8000}, {0x8000, 0}}};
    OA_CHECK(inside_clockwise(half, {0x4000, 0x4000}));
    const oa::formats::objects3d::FixedVector3 local{2 * 65536, 4 * 65536, -3 * 65536},
        world{100 * 65536, 20 * 65536, 200 * 65536};
    const auto p = project(local, world, {50, 75});
    OA_CHECK(p.x == 180 && p.y == 148);
    oa::formats::objects3d::Model model;
    model.objects.resize(1);
    model.objects[0].vertices = {
        {-10 * 65536, 0, -10 * 65536},
        {-10 * 65536, 0, 10 * 65536},
        {10 * 65536, 0, 10 * 65536},
        {10 * 65536, 0, -10 * 65536}
    };
    const PickUnit unit{7, {100 * 65536, 0, 200 * 65536}, {}, &model};
    OA_CHECK(hits_root_bounds(unit, {50, 75}, {178, 157}));
    OA_CHECK(!hits_root_bounds(unit, {50, 75}, {200, 157}));
    // A unit without a model has no box to hit.
    const PickUnit modelless{8, {100 * 65536, 0, 200 * 65536}, {}, nullptr};
    OA_CHECK(!hits_root_bounds(modelless, {50, 75}, {178, 157}));
    oa::formats::tnt::Map map;
    map.attribute_width = 4;
    map.attribute_height = 8;
    map.sea_level = 0;
    map.attributes.resize(32);
    for (auto& attribute : map.attributes)
        attribute.height = 20;
    const oa::sim::unit_movement::Terrain terrain(map);
    const auto ground = terrain_intersection(terrain, 32, 40, 64, 128);
    OA_CHECK(ground.x == 32 * 65536 && ground.y == 20 * 65536 && ground.z == 50 * 65536);
    // A map with no size has no terrain to find.
    const auto nowhere = terrain_intersection(terrain, 32, 40, 0, 128);
    OA_CHECK(nowhere.x == 0 && nowhere.y == 0 && nowhere.z == 0);
    // A drag box's corner over the map is the terrain point there, whether
    // or not the view may show past the map's edges.
    for (const bool past_edges : {false, true}) {
        const auto on_map = drag_box_corner(terrain, 32, 40, 64, 128, past_edges);
        OA_CHECK(on_map.x == ground.x && on_map.y == ground.y && on_map.z == ground.z);
    }
    // Past the edges a view kept on the map holds the corner on the map's
    // ground, as terrain_intersection does.
    const auto held = drag_box_corner(terrain, -40, -24, 64, 128, false);
    const auto edge = terrain_intersection(terrain, -40, -24, 64, 128);
    OA_CHECK(held.x == edge.x && held.y == edge.y && held.z == edge.z);
    OA_CHECK(held.x == 0);
    // A view that shows past them takes the point itself at height 0, on
    // every side.
    const auto left = drag_box_corner(terrain, -40, 60, 64, 128, true);
    OA_CHECK(left.x == -40 * 65536 && left.y == 0 && left.z == 60 * 65536);
    const auto above = drag_box_corner(terrain, 10, -24, 64, 128, true);
    OA_CHECK(above.x == 10 * 65536 && above.y == 0 && above.z == -24 * 65536);
    const auto right = drag_box_corner(terrain, 64, 60, 64, 128, true);
    OA_CHECK(right.x == 64 * 65536 && right.y == 0 && right.z == 60 * 65536);
    const auto below = drag_box_corner(terrain, 10, 300, 64, 128, true);
    OA_CHECK(below.x == 10 * 65536 && below.y == 0 && below.z == 300 * 65536);
    // A map with no size still has no terrain to find.
    const auto sizeless = drag_box_corner(terrain, -40, 60, 0, 128, true);
    OA_CHECK(sizeless.x == 0 && sizeless.y == 0 && sizeless.z == 0);

    // Footprints against a shown map of 6368 by 6272 map pixels, a 400-cell
    // square map's less its hidden edges. A 4-cell building at height 42 is
    // drawn 21 rows up: over rows 388 to 391 it lies within the map, over
    // rows 390 to 393, as over a vent in row 393, it hangs past its bottom.
    OA_CHECK(footprint_within_shown_map(88, 388, 4, 4, 42, 6368, 6272));
    OA_CHECK(!footprint_within_shown_map(88, 390, 4, 4, 42, 6368, 6272));
    // Its last row ends on the shown map's last row, or one row past it.
    OA_CHECK(footprint_within_shown_map(88, 388, 4, 4, 0, 6368, 6272));
    OA_CHECK(!footprint_within_shown_map(88, 389, 4, 4, 0, 6368, 6272));
    // Raised by half its height, rounded down, as the hidden edges are
    // measured: on ground 32 high, a footprint whose last row, 392, starts
    // at the shown height is drawn within the map.
    OA_CHECK(footprint_within_shown_map(88, 389, 4, 4, 32, 6368, 6272));
    OA_CHECK(footprint_within_shown_map(88, 389, 4, 4, 33, 6368, 6272));
    OA_CHECK(!footprint_within_shown_map(88, 389, 4, 4, 31, 6368, 6272));
    // Across, heights do not count: its last column ends on the shown
    // width, or one column past it; left of the map it is past too.
    OA_CHECK(footprint_within_shown_map(394, 100, 4, 4, 200, 6368, 6272));
    OA_CHECK(!footprint_within_shown_map(395, 100, 4, 4, 0, 6368, 6272));
    OA_CHECK(footprint_within_shown_map(0, 100, 4, 4, 0, 6368, 6272));
    OA_CHECK(!footprint_within_shown_map(-1, 100, 4, 4, 0, 6368, 6272));
    // Raised above the map's top edge it is past it.
    OA_CHECK(footprint_within_shown_map(10, 1, 4, 4, 32, 6368, 6272));
    OA_CHECK(!footprint_within_shown_map(10, 1, 4, 4, 40, 6368, 6272));
    // No footprint wraps round into the map, and with no shown map known
    // every footprint lies within it.
    OA_CHECK(!footprint_within_shown_map(0, 0x7fffffff, 4, 4, 0, 6368, 6272));
    OA_CHECK(!footprint_within_shown_map(0x7fffffff, 100, 4, 4, 0, 6368, 6272));
    OA_CHECK(footprint_within_shown_map(-1, 900, 4, 4, 0, 0, 0));
    OA_CHECK(command_binds_cursor_unit(0));
    OA_CHECK(command_binds_cursor_unit(1));
    OA_CHECK(command_binds_cursor_unit(2));
    OA_CHECK(command_binds_cursor_unit(3));
    OA_CHECK(!command_binds_cursor_unit(unload_command));
    OA_CHECK(!command_binds_cursor_unit(stop_command));
    OA_CHECK(!command_binds_cursor_unit(build_command));
    OA_CHECK(command_binds_cursor_unit(0xff));
    return oa::test::check_exit_status();
}
