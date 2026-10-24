// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Classic projection pins screen coordinates, round trips known-height planes,
// preserves painted terrain across heights, orders points along the view ray,
// and refuses nonfinite coordinates, invalid scales and arithmetic overflow.
#include "renderer_projection.hpp"

#include "oa/test/check.hpp"

#include <array>
#include <cmath>
#include <limits>

namespace {
namespace rp = oa::app::renderer_projection;

/// Absolute tolerance for the small coordinate fixtures.
constexpr double coordinate_tolerance = 1e-9;

/// Checks two coordinates within the fixtures' floating point tolerance.
bool close(double actual, double expected) {
    return std::abs(actual - expected) <= coordinate_tolerance;
}

/// Pins the camera translation, scale and viewport origin against known pixels.
void test_classic_coordinates() {
    const rp::ClassicProjection projection{100.0, 200.0, 2.0, 16.0, 24.0};
    const auto screen = rp::project_classic(projection, {120.0, 40.0, 250.0});
    OA_CHECK(screen.has_value());
    if (!screen)
        return;
    OA_CHECK(screen->x == 56.0);
    OA_CHECK(screen->y == 84.0);
    OA_CHECK(screen->depth == 165.0);
    const auto ground = rp::unproject_classic_at_height(projection, 56.0, 84.0, 0.0);
    OA_CHECK(ground.has_value());
    if (!ground)
        return;
    OA_CHECK(ground->x == 120.0);
    OA_CHECK(ground->height == 0.0);
    OA_CHECK(ground->z == 230.0);
}

/// Checks plane intersections over positive, negative and fractional heights.
void test_round_trip() {
    constexpr std::array heights{-80.0, 0.0, 0.25, 32.0, 255.0};
    constexpr std::array scales{0.25, 1.0, 1.5, 4.0};
    for (double height : heights) {
        for (double scale : scales) {
            const rp::ClassicProjection projection{-27.5, 81.25, scale, 14.0, -9.0};
            const rp::WorldPoint point{121.25, height, -36.5};
            const auto screen = rp::project_classic(projection, point);
            OA_CHECK(screen.has_value());
            if (!screen)
                continue;
            const auto restored =
                rp::unproject_classic_at_height(projection, screen->x, screen->y, height);
            OA_CHECK(restored.has_value());
            if (!restored)
                continue;
            OA_CHECK(close(restored->x, point.x));
            OA_CHECK(close(restored->height, point.height));
            OA_CHECK(close(restored->z, point.z));
        }
    }
}

/// Checks that lifting painted pixels leaves their classic image in place.
void test_painted_terrain() {
    constexpr std::array heights{-20.0, 0.0, 8.0, 64.0, 255.0};
    for (double height : heights) {
        const auto point = rp::terrain_point_from_painted_map(37.0, 92.0, height);
        OA_CHECK(point.has_value());
        if (!point)
            continue;
        const auto screen = rp::project_classic({}, *point);
        OA_CHECK(screen.has_value());
        if (!screen)
            continue;
        OA_CHECK(screen->x == 37.0);
        OA_CHECK(screen->y == 92.0);
        OA_CHECK(point->height == height);
    }
}

/// Checks depth increases towards the viewer without moving the screen point.
void test_view_ray_depth() {
    const auto far = rp::project_classic({}, {70.0, 10.0, 40.0});
    const auto middle = rp::project_classic({}, {70.0, 30.0, 50.0});
    const auto near = rp::project_classic({}, {70.0, 50.0, 60.0});
    OA_CHECK(far && middle && near);
    if (!far || !middle || !near)
        return;
    OA_CHECK(far->x == middle->x && middle->x == near->x);
    OA_CHECK(far->y == middle->y && middle->y == near->y);
    OA_CHECK(far->depth < middle->depth && middle->depth < near->depth);
}

/// Refuses nonfinite values in every coordinate and camera parameter.
void test_invalid_input() {
    constexpr double infinity = std::numeric_limits<double>::infinity();
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr std::array invalid_values{infinity, -infinity, nan};
    constexpr std::array camera_members{
        &rp::ClassicProjection::camera_x,
        &rp::ClassicProjection::camera_y,
        &rp::ClassicProjection::scale,
        &rp::ClassicProjection::origin_x,
        &rp::ClassicProjection::origin_y
    };
    for (double invalid : invalid_values) {
        for (auto member : camera_members) {
            rp::ClassicProjection projection;
            projection.*member = invalid;
            OA_CHECK(!rp::project_classic(projection, {}));
            OA_CHECK(!rp::unproject_classic_at_height(projection, 0.0, 0.0, 0.0));
        }
        OA_CHECK(!rp::project_classic({}, {invalid, 0.0, 0.0}));
        OA_CHECK(!rp::project_classic({}, {0.0, invalid, 0.0}));
        OA_CHECK(!rp::project_classic({}, {0.0, 0.0, invalid}));
        OA_CHECK(!rp::unproject_classic_at_height({}, invalid, 0.0, 0.0));
        OA_CHECK(!rp::unproject_classic_at_height({}, 0.0, invalid, 0.0));
        OA_CHECK(!rp::unproject_classic_at_height({}, 0.0, 0.0, invalid));
        OA_CHECK(!rp::terrain_point_from_painted_map(invalid, 0.0, 0.0));
        OA_CHECK(!rp::terrain_point_from_painted_map(0.0, invalid, 0.0));
        OA_CHECK(!rp::terrain_point_from_painted_map(0.0, 0.0, invalid));
    }
    constexpr std::array invalid_scales{0.0, -1.0};
    for (double scale : invalid_scales) {
        const rp::ClassicProjection projection{0.0, 0.0, scale};
        OA_CHECK(!rp::project_classic(projection, {}));
        OA_CHECK(!rp::unproject_classic_at_height(projection, 0.0, 0.0, 0.0));
    }
}

/// Refuses finite inputs whose projection or inverse cannot remain finite.
void test_overflow() {
    constexpr double largest = std::numeric_limits<double>::max();
    OA_CHECK(!rp::project_classic({0.0, 0.0, 2.0}, {largest, 0.0, 0.0}));
    OA_CHECK(!rp::project_classic({}, {0.0, largest, largest}));
    OA_CHECK(!rp::unproject_classic_at_height({0.0, 0.0, 0.25}, largest, 0.0, 0.0));
    OA_CHECK(!rp::terrain_point_from_painted_map(0.0, largest, largest));
}
} // namespace

/// Runs the classic projection checks and reports any failures.
int main() {
    test_classic_coordinates();
    test_round_trip();
    test_painted_terrain();
    test_view_ray_depth();
    test_invalid_input();
    test_overflow();
    return oa::test::check_exit_status();
}
