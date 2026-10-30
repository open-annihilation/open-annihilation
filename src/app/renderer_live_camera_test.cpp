// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Camera projection preserves the classic view and inverts picking at every allowed angle.
#include "renderer_live_camera.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {
using namespace oa::app::renderer_live_camera;
constexpr float tolerance = 0.001f;
constexpr float invalid = std::numeric_limits<float>::quiet_NaN();

/// Compare world positions with subpixel precision.
bool close(float left, float right) {
    return std::abs(left - right) < tolerance;
}

/// Keep the original projection exact, including the viewport translation.
void classic_projection() {
    Matrix matrix{};
    OA_CHECK(projection({}, {100, 0, 200, 320, 240, 1}, matrix));
    const Matrix expected{1, 0, 0, 220, 0, -0.5f, 1, 40, 0, 1, 0.5f, -100};
    OA_CHECK(matrix == expected);
    OA_CHECK(projection({}, {100, 40, 200, 320, 240, 2}, matrix));
    OA_CHECK(matrix[0] * 100 + matrix[3] == 320);
    OA_CHECK(matrix[5] * 40 + matrix[6] * 200 + matrix[7] == 240);
}

/// Recover map coordinates at different heights, zooms and camera limits.
void inverse_projection() {
    for (const float yaw : {-180.0f, -45.0f, 0.0f, 45.0f, 90.0f}) {
        for (const float pitch : {-50.0f, -20.0f, 0.0f, 20.0f}) {
            for (const float zoom : {0.5f, 1.0f, 2.0f}) {
                Matrix matrix{};
                OA_CHECK(projection({yaw, pitch, true}, {150, 50, 240, 320, 240, zoom}, matrix));
                for (const float height : {-20.0f, 0.0f, 100.0f, 255.0f}) {
                    constexpr float world_x = 410;
                    constexpr float world_z = 170;
                    const float screen_x =
                        matrix[0] * world_x + matrix[1] * height + matrix[2] * world_z + matrix[3];
                    const float screen_y =
                        matrix[4] * world_x + matrix[5] * height + matrix[6] * world_z + matrix[7];
                    float x{};
                    float z{};
                    OA_CHECK(world_at_height(matrix, screen_x, screen_y, height, x, z));
                    OA_CHECK(close(x, world_x));
                    OA_CHECK(close(z, world_z));
                    constexpr float dx = 12;
                    constexpr float dz = -18;
                    OA_CHECK(pan_delta(
                        matrix,
                        matrix[0] * dx + matrix[2] * dz,
                        matrix[4] * dx + matrix[6] * dz,
                        x,
                        z
                    ));
                    OA_CHECK(close(x, dx));
                    OA_CHECK(close(z, dz));
                }
            }
        }
    }
}

/// Preserve elevated pointer anchors through fractional zoom and its inverse.
void pointer_zoom() {
    for (const State state : {State{}, State{45, -20, false}, State{145, -50, true}}) {
        for (const float height : {0.0F, 255.0F}) {
            View view{150, 50, 240, 320, 240, 1};
            constexpr float pointer_x = 100;
            constexpr float pointer_y = 180;
            Matrix matrix{};
            OA_CHECK(projection(state, view, matrix));
            float anchor_x{}, anchor_z{};
            OA_CHECK(world_at_height(matrix, pointer_x, pointer_y, height, anchor_x, anchor_z));
            for (const float zoom : {1.05F, 4.0F, 0.25F, 1.0F}) {
                OA_CHECK(zoom_about(state, view, zoom, pointer_x, pointer_y, height));
                OA_CHECK(projection(state, view, matrix));
                OA_CHECK(close(matrix[0] * anchor_x + matrix[2] * anchor_z + matrix[3], pointer_x));
                OA_CHECK(close(
                    matrix[4] * anchor_x + matrix[5] * height + matrix[6] * anchor_z + matrix[7],
                    pointer_y
                ));
            }
            OA_CHECK(close(view.focus_x, 150) && close(view.focus_z, 240));
            const auto saved = view;
            OA_CHECK(!zoom_about(state, view, 0, pointer_x, pointer_y, height));
            OA_CHECK(!zoom_about(state, view, invalid, pointer_x, pointer_y, height));
            OA_CHECK(!zoom_about(state, view, 2, invalid, pointer_y, height));
            OA_CHECK(
                view.zoom == saved.zoom && view.focus_x == saved.focus_x &&
                view.focus_z == saved.focus_z
            );
        }
    }
}

/// Limit orbit, widen its range only when unlocked, and restore the classic view.
void camera_limits() {
    State state{};
    orbit(state, 90, 90);
    OA_CHECK(state.yaw_degrees == 45);
    OA_CHECK(state.pitch_degrees == 20);
    orbit(state, -90, -90);
    OA_CHECK(state.yaw_degrees == -45);
    OA_CHECK(state.pitch_degrees == -20);
    set_unlocked(state, true);
    orbit(state, 135, 100);
    OA_CHECK(state.yaw_degrees == 90);
    OA_CHECK(state.pitch_degrees > 20 && state.pitch_degrees < 22);
    set_unlocked(state, false);
    OA_CHECK(state.yaw_degrees == 45);
    OA_CHECK(state.pitch_degrees == 20);
    Matrix at_limit{};
    OA_CHECK(projection(state, {}, at_limit));
    OA_CHECK(projection({-45, -20, false}, {}, at_limit));
    OA_CHECK(!projection({45.01F, 20, false}, {}, at_limit));
    OA_CHECK(!projection({45, 20.01F, false}, {}, at_limit));
    OA_CHECK(!projection({-45.01F, -20, false}, {}, at_limit));
    OA_CHECK(!projection({-45, -20.01F, false}, {}, at_limit));
    orbit(state, invalid, 1);
    OA_CHECK(state.pitch_degrees == 20);
    reset(state);
    OA_CHECK(state.yaw_degrees == 0 && state.pitch_degrees == 0 && !state.unlocked);
}

/// Pick triangle interiors, edges and winding with interpolated depth.
void triangle_picking() {
    const std::array<std::array<float, 3>, 3> triangle{{{0, 0, 10}, {10, 0, 20}, {0, 10, 30}}};
    std::array<float, 3> weights{};
    float depth{};
    OA_CHECK(triangle_hit(triangle, 2, 3, weights, depth));
    OA_CHECK(close(weights[0], 0.5f) && close(weights[1], 0.2f) && close(weights[2], 0.3f));
    OA_CHECK(close(depth, 18));
    OA_CHECK(triangle_hit({triangle[2], triangle[1], triangle[0]}, 2, 3, weights, depth));
    OA_CHECK(close(depth, 18));
    OA_CHECK(triangle_hit(triangle, 0, 0, weights, depth));
    OA_CHECK(!triangle_hit(triangle, 6, 6, weights, depth));
    OA_CHECK(!triangle_hit(triangle, invalid, 0, weights, depth));
    OA_CHECK(!triangle_hit({}, 0, 0, weights, depth));
}

/// Evaluate triangle intersections directly to pin rounding of prepared coefficients.
bool triangle_hit_reference(
    const std::array<std::array<float, 3>, 3>& triangle,
    float screen_x,
    float screen_y,
    std::array<float, 3>& weights,
    float& depth
) noexcept {
    if (!std::isfinite(screen_x) || !std::isfinite(screen_y))
        return false;
    for (const auto& point : triangle)
        for (float coordinate : point)
            if (!std::isfinite(coordinate))
                return false;
    const auto& a = triangle[0];
    const auto& b = triangle[1];
    const auto& c = triangle[2];
    const double determinant = (double(b[1]) - c[1]) * (double(a[0]) - c[0]) +
                               (double(c[0]) - b[0]) * (double(a[1]) - c[1]);
    if (std::abs(determinant) < 1e-12f)
        return false;
    const double first = ((double(b[1]) - c[1]) * (double(screen_x) - c[0]) +
                          (double(c[0]) - b[0]) * (double(screen_y) - c[1])) /
                         determinant;
    const double second = ((double(c[1]) - a[1]) * (double(screen_x) - c[0]) +
                           (double(a[0]) - c[0]) * (double(screen_y) - c[1])) /
                          determinant;
    const double third = 1 - first - second;
    constexpr double edge_tolerance = 1e-6;
    if (first < -edge_tolerance || second < -edge_tolerance || third < -edge_tolerance)
        return false;
    const float hit_depth = static_cast<float>(first * a[2] + second * b[2] + third * c[2]);
    if (!std::isfinite(hit_depth))
        return false;
    weights = {static_cast<float>(first), static_cast<float>(second), static_cast<float>(third)};
    depth = hit_depth;
    return true;
}

/// Bounding rejection retains tolerant edges and the prepared query's exact hit results.
void bounded_triangle_picking() {
    using Triangle = std::array<std::array<float, 3>, 3>;
    uint32_t state = 12345;
    const auto random = [&] {
        state = state * 1664525U + 1013904223U;
        return float(state >> 8) / 16777216.0F;
    };
    for (int sample = 0; sample < 10000; ++sample) {
        const float scale = std::pow(10.0F, random() * 16 - 8);
        Triangle triangle{};
        for (auto& vertex : triangle)
            vertex = {(random() * 2 - 1) * scale, (random() * 2 - 1) * scale, random() * 100};
        if (sample % 4 == 0)
            triangle[2] = {
                std::lerp(triangle[0][0], triangle[1][0], 0.5F),
                std::lerp(triangle[0][1], triangle[1][1], 0.5F),
                10
            };
        PreparedTriangle prepared;
        (void)prepare_triangle(triangle, prepared);
        const auto compare = [&](float x, float y) {
            std::array<float, 3> expected_weights{11, 22, 33}, weights = expected_weights;
            float expected_depth = 44, depth = expected_depth;
            const bool expected =
                triangle_hit_prepared(prepared, x, y, expected_weights, expected_depth);
            OA_CHECK(triangle_hit(triangle, x, y, weights, depth) == expected);
            OA_CHECK(weights == expected_weights);
            OA_CHECK(depth == expected_depth);
        };
        for (int point = 0; point < 8; ++point)
            compare((random() * 4 - 2) * scale, (random() * 4 - 2) * scale);
        for (const auto& vertex : triangle) {
            compare(vertex[0], vertex[1]);
            compare(std::nextafter(vertex[0], std::numeric_limits<float>::infinity()), vertex[1]);
            compare(vertex[0], std::nextafter(vertex[1], -std::numeric_limits<float>::infinity()));
        }
        // Two weights can be slightly negative while remaining accepted.
        for (const float weight : {-0.000001F, -0.0000005F, 0.0000005F})
            compare(
                (1 - 2 * weight) * triangle[0][0] + weight * triangle[1][0] +
                    weight * triangle[2][0],
                (1 - 2 * weight) * triangle[0][1] + weight * triangle[1][1] +
                    weight * triangle[2][1]
            );
    }
}

/// Keep barycentric rounding exact across winding, edges, scale and invalid geometry.
void prepared_triangle_picking() {
    using Triangle = std::array<std::array<float, 3>, 3>;
    constexpr float infinity = std::numeric_limits<float>::infinity();
    constexpr float largest = std::numeric_limits<float>::max();
    const std::array triangles{
        Triangle{{{0, 0, 10}, {10, 0, 20}, {0, 10, 30}}},
        Triangle{{{91.125F, -17.25F, -300}, {3.5F, 87.625F, 0.125F}, {17, 1.25F, 850}}},
        Triangle{{{0, 0, 0}, {1e-7F, 0, 1}, {0, 1e-7F, 2}}},
        Triangle{{{0, 0, 0}, {1e-6F, 0, 1}, {0, 1e-6F, 2}}},
        Triangle{{{0, 0, largest}, {1, 0, -largest}, {0, 1, largest}}},
        Triangle{{{-largest, 0, 1}, {largest, 0, 2}, {0, largest, 3}}},
        Triangle{{{0, 0, 0}, {1, 1, 1}, {2, 2, 2}}},
        Triangle{{{invalid, 0, 0}, {1, 0, 1}, {0, 1, 2}}},
        Triangle{{{0, 0, 0}, {1, infinity, 1}, {0, 1, 2}}},
        Triangle{{{0, 0, 0}, {1, 0, 1}, {0, 1, invalid}}},
    };
    for (auto triangle : triangles) {
        for (int winding = 0; winding < 2; ++winding) {
            std::swap(triangle[0], triangle[2]);
            PreparedTriangle prepared;
            (void)prepare_triangle(triangle, prepared);
            const auto compare = [&](float x, float y) {
                std::array<float, 3> expected_weights{11, 22, 33};
                auto weights = expected_weights;
                float expected_depth = 44;
                float depth = expected_depth;
                const bool expected =
                    triangle_hit_reference(triangle, x, y, expected_weights, expected_depth);
                OA_CHECK(triangle_hit_prepared(prepared, x, y, weights, depth) == expected);
                OA_CHECK(weights == expected_weights);
                OA_CHECK(depth == expected_depth);
                weights = {11, 22, 33};
                depth = 44;
                OA_CHECK(triangle_hit(triangle, x, y, weights, depth) == expected);
                OA_CHECK(weights == expected_weights);
                OA_CHECK(depth == expected_depth);
            };
            for (int y = -20; y < 100; ++y)
                for (int x = -20; x < 100; ++x)
                    compare(x + 0.5F, y + 0.5F);
            for (const auto& point : triangle)
                compare(point[0], point[1]);
            for (float x : {-1e-6F, 0.0F, 1e-6F, largest, invalid, infinity})
                for (float y : {-1e-6F, 0.0F, 1e-6F, largest, invalid, infinity})
                    compare(x, y);
        }
    }
    PreparedTriangle prepared;
    OA_CHECK(prepare_triangle(triangles[0], prepared));
    OA_CHECK(!prepare_triangle({}, prepared));
    std::array<float, 3> weights{11, 22, 33};
    float depth = 44;
    OA_CHECK(!triangle_hit_prepared(prepared, 0, 0, weights, depth));
    OA_CHECK((weights == std::array<float, 3>{11, 22, 33}));
    OA_CHECK(depth == 44);
}

/// Narrowed rows keep every column whose centre the prepared query accepts.
void narrowed_rows() {
    using Triangle = std::array<std::array<float, 3>, 3>;
    uint32_t state = 54321;
    const auto random = [&] {
        state = state * 1664525U + 1013904223U;
        return float(state >> 8) / 16777216.0F;
    };
    constexpr float centre = 0.5F;
    std::size_t kept = 0;
    std::size_t left_out = 0;
    for (int sample = 0; sample < 4000; ++sample) {
        // Triangles from slivers a pixel across to ones wider than the rows.
        const float scale = std::pow(10.0F, random() * 3 - 0.5F);
        const float middle_x = random() * 64;
        const float middle_y = random() * 64;
        Triangle triangle{};
        for (auto& vertex : triangle)
            vertex = {
                middle_x + (random() * 2 - 1) * scale,
                middle_y + (random() * 2 - 1) * scale,
                random() * 100
            };
        if (sample % 5 == 0)
            triangle[2] = {
                std::lerp(triangle[0][0], triangle[1][0], 0.375F),
                std::lerp(triangle[0][1], triangle[1][1], 0.375F) + 1e-3F,
                10
            };
        if (sample % 7 == 0)
            triangle[1][1] = triangle[0][1];
        PreparedTriangle prepared;
        if (!prepare_triangle(triangle, prepared))
            continue;
        for (int y = -8; y < 72; ++y) {
            constexpr int row_first = -8;
            constexpr int row_last = 72;
            int first = row_first;
            int last = row_last;
            narrow_row(prepared, y + centre, centre, first, last);
            OA_CHECK(first >= row_first && last <= row_last);
            for (int x = row_first; x < row_last; ++x) {
                std::array<float, 3> weights{};
                float depth{};
                if (!triangle_hit_prepared(prepared, x + centre, y + centre, weights, depth))
                    continue;
                OA_CHECK(x >= first && x < last);
                ++kept;
            }
            left_out += static_cast<std::size_t>(row_last - row_first) -
                        static_cast<std::size_t>(std::max(0, last - first));
        }
    }
    // The narrowing leaves out most of the columns no triangle covers.
    OA_CHECK(kept > 0);
    OA_CHECK(left_out > kept);
    // An invalid triangle and an empty row change nothing.
    int first = 3;
    int last = 40;
    narrow_row(PreparedTriangle{}, 10.5F, centre, first, last);
    OA_CHECK(first == 3 && last == 40);
    first = last = 7;
    PreparedTriangle prepared;
    OA_CHECK(prepare_triangle(Triangle{{{0, 0, 0}, {64, 0, 0}, {0, 64, 0}}}, prepared));
    narrow_row(prepared, 10.5F, centre, first, last);
    OA_CHECK(first == 7 && last == 7);
    // A row wholly outside the triangle comes back empty.
    first = 0;
    last = 64;
    narrow_row(prepared, 80.5F, centre, first, last);
    OA_CHECK(first == last);
}

/// Include every visible terrain height while keeping the classic crop tight.
void terrain_bounds() {
    constexpr float width = 960;
    constexpr float height = 640;
    constexpr float margin = 32;
    Matrix matrix{};
    OA_CHECK(projection({}, {100, 0, 200, 0, 0, 1}, matrix));
    TerrainBounds bounds{};
    OA_CHECK(visible_terrain_bounds(matrix, width, height, margin, bounds));
    OA_CHECK(close(bounds.min_x, 100 - margin));
    OA_CHECK(close(bounds.min_y, 200 - margin));
    OA_CHECK(close(bounds.max_x, 100 + width + margin));
    OA_CHECK(close(bounds.max_y, 200 + height + margin));
    for (const bool unlocked : {false, true})
        for (const float yaw : {-180.0F, -90.0F, -45.0F, 0.0F, 45.0F, 90.0F, 180.0F})
            for (const float pitch : {-100.0F, -20.0F, 0.0F, 20.0F, 100.0F})
                for (const float zoom : {0.25F, 1.0F, 4.0F}) {
                    State state{0, 0, unlocked};
                    orbit(state, yaw, pitch);
                    OA_CHECK(
                        projection(state, {100, 120, 200, width / 2, height / 2, zoom}, matrix)
                    );
                    OA_CHECK(visible_terrain_bounds(matrix, width, height, margin, bounds));
                    for (const float world_height : {0.0F, 1.0F, 64.0F, 127.5F, 254.0F, 255.0F})
                        for (const float row : {0.0F, 0.25F, 0.5F, 0.75F, 1.0F})
                            for (const float column : {0.0F, 0.25F, 0.5F, 0.75F, 1.0F}) {
                                float x{}, z{};
                                OA_CHECK(world_at_height(
                                    matrix, column * width, row * height, world_height, x, z
                                ));
                                const float painted_y = z - world_height / 2;
                                OA_CHECK(x >= bounds.min_x && x <= bounds.max_x);
                                OA_CHECK(painted_y >= bounds.min_y && painted_y <= bounds.max_y);
                            }
                }
    const TerrainBounds saved = bounds;
    OA_CHECK(!visible_terrain_bounds({}, width, height, margin, bounds));
    OA_CHECK(!visible_terrain_bounds(matrix, 0, height, margin, bounds));
    OA_CHECK(!visible_terrain_bounds(matrix, width, -1, margin, bounds));
    OA_CHECK(!visible_terrain_bounds(matrix, width, height, -1, bounds));
    OA_CHECK(!visible_terrain_bounds(matrix, invalid, height, margin, bounds));
    OA_CHECK(!visible_terrain_bounds(matrix, width, invalid, margin, bounds));
    OA_CHECK(!visible_terrain_bounds(matrix, width, height, invalid, bounds));
    matrix[0] = invalid;
    OA_CHECK(!visible_terrain_bounds(matrix, width, height, margin, bounds));
    OA_CHECK(bounds.min_x == saved.min_x && bounds.min_y == saved.min_y);
    OA_CHECK(bounds.max_x == saved.max_x && bounds.max_y == saved.max_y);
}

/// Refuse invalid transforms without overwriting the caller's last usable result.
void invalid_input() {
    Matrix matrix{};
    OA_CHECK(projection({}, {}, matrix));
    const Matrix saved = matrix;
    OA_CHECK(!projection({invalid, 0, false}, {}, matrix));
    OA_CHECK(!projection({0, 90, true}, {}, matrix));
    OA_CHECK(!projection({}, {0, 0, 0, 0, 0, 0}, matrix));
    OA_CHECK(!projection({}, {0, 0, 0, 0, 0, invalid}, matrix));
    OA_CHECK(matrix == saved);
    float x = 11;
    float z = 22;
    OA_CHECK(!world_at_height(matrix, 0, 0, invalid, x, z));
    OA_CHECK(!world_at_height(matrix, invalid, 0, 0, x, z));
    OA_CHECK(!pan_delta({}, 1, 1, x, z));
    OA_CHECK(x == 11 && z == 22);
}
} // namespace

int main() {
    classic_projection();
    inverse_projection();
    pointer_zoom();
    camera_limits();
    invalid_input();
    triangle_picking();
    prepared_triangle_picking();
    bounded_triangle_picking();
    narrowed_rows();
    terrain_bounds();
    return oa::test::check_exit_status();
}
