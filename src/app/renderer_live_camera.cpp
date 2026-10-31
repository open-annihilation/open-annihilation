// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "renderer_live_camera.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>

namespace oa::app::renderer_live_camera {
namespace {
constexpr float restricted_yaw = 45;
constexpr float restricted_pitch = 20;
constexpr float classic_elevation = 63.4349488f;
constexpr float minimum_elevation = 10;
constexpr float maximum_elevation = 85;
constexpr float half_turn = 180;
constexpr float full_turn = 360;
constexpr float height_projection = 0.5f;
constexpr float radians_per_degree = std::numbers::pi_v<float> / half_turn;
constexpr float minimum_determinant = 1e-12f;

/// Clamp angles and replace unusable state with the classic orientation.
void constrain(State& state) noexcept {
    if (!std::isfinite(state.yaw_degrees))
        state.yaw_degrees = 0;
    if (!std::isfinite(state.pitch_degrees))
        state.pitch_degrees = 0;
    state.yaw_degrees = std::remainder(state.yaw_degrees, full_turn);
    if (state.unlocked) {
        state.pitch_degrees = std::clamp(
            state.pitch_degrees,
            minimum_elevation - classic_elevation,
            maximum_elevation - classic_elevation
        );
    } else {
        state.yaw_degrees = std::clamp(state.yaw_degrees, -restricted_yaw, restricted_yaw);
        state.pitch_degrees = std::clamp(state.pitch_degrees, -restricted_pitch, restricted_pitch);
    }
}

/// Reject transforms containing nonfinite coefficients.
bool finite_matrix(const Matrix& matrix) noexcept {
    return std::all_of(matrix.begin(), matrix.end(), [](float v) { return std::isfinite(v); });
}
} // namespace

bool prepare_triangle(
    const std::array<std::array<float, 3>, 3>& triangle, PreparedTriangle& prepared
) noexcept {
    prepared = {};
    for (const auto& point : triangle)
        for (float coordinate : point)
            if (!std::isfinite(coordinate))
                return false;
    const auto& a = triangle[0];
    const auto& b = triangle[1];
    const auto& c = triangle[2];
    prepared.first_x = double(b[1]) - c[1];
    prepared.first_y = double(c[0]) - b[0];
    prepared.second_x = double(c[1]) - a[1];
    prepared.second_y = double(a[0]) - c[0];
    prepared.determinant =
        prepared.first_x * (double(a[0]) - c[0]) + prepared.first_y * (double(a[1]) - c[1]);
    if (std::abs(prepared.determinant) < minimum_determinant)
        return false;
    prepared.origin_x = c[0];
    prepared.origin_y = c[1];
    prepared.depths = {a[2], b[2], c[2]};
    prepared.valid = true;
    return true;
}

bool triangle_hit_bounded(const std::array<std::array<float, 3>, 3>& triangle) noexcept {
    const auto& a = triangle[0];
    const auto& b = triangle[1];
    const auto& c = triangle[2];
    // Slivers can magnify cancellation in the original barycentric query.
    const double first = (double(b[1]) - c[1]) * (double(a[0]) - c[0]);
    const double second = (double(c[0]) - b[0]) * (double(a[1]) - c[1]);
    constexpr double minimum_relative_area = 1e-9;
    return std::abs(first + second) > (std::abs(first) + std::abs(second)) * minimum_relative_area;
}

bool triangle_hit(
    const std::array<std::array<float, 3>, 3>& triangle,
    float screen_x,
    float screen_y,
    std::array<float, 3>& weights,
    float& depth
) noexcept {
    const auto& a = triangle[0];
    const auto& b = triangle[1];
    const auto& c = triangle[2];
    const double left = std::min({a[0], b[0], c[0]});
    const double right = std::max({a[0], b[0], c[0]});
    // Accepted barycentric weights may extend beyond the vertices. Allow twice
    // their maximum aggregate edge tolerance, including arithmetic roundoff.
    const double margin_x = (right - left) * triangle_bounds_tolerance;
    bool outside = screen_x < left - margin_x || screen_x > right + margin_x;
    if (!outside) {
        const double top = std::min({a[1], b[1], c[1]});
        const double bottom = std::max({a[1], b[1], c[1]});
        const double margin_y = (bottom - top) * triangle_bounds_tolerance;
        outside = screen_y < top - margin_y || screen_y > bottom + margin_y;
    }
    // Keep the original query for ill-conditioned triangles instead of
    // changing hits.
    if (outside && triangle_hit_bounded(triangle))
        return false;
    PreparedTriangle prepared;
    return prepare_triangle(triangle, prepared) &&
           triangle_hit_prepared(prepared, screen_x, screen_y, weights, depth);
}

void orbit(State& state, float yaw_delta_degrees, float pitch_delta_degrees) noexcept {
    if (!std::isfinite(yaw_delta_degrees) || !std::isfinite(pitch_delta_degrees))
        return;
    state.yaw_degrees += std::remainder(yaw_delta_degrees, full_turn);
    state.pitch_degrees += pitch_delta_degrees;
    constrain(state);
}

void set_unlocked(State& state, bool unlocked) noexcept {
    state.unlocked = unlocked;
    constrain(state);
}

void reset(State& state) noexcept {
    state = {};
}

bool projection(const State& state, const View& view, Matrix& matrix) noexcept {
    if (!std::isfinite(state.yaw_degrees) || !std::isfinite(state.pitch_degrees) ||
        !std::isfinite(view.focus_x) || !std::isfinite(view.focus_y) ||
        !std::isfinite(view.focus_z) || !std::isfinite(view.screen_x) ||
        !std::isfinite(view.screen_y) || !std::isfinite(view.zoom) || view.zoom <= 0)
        return false;
    State bounded = state;
    constrain(bounded);
    if (bounded.pitch_degrees != state.pitch_degrees || bounded.yaw_degrees != state.yaw_degrees)
        return false;
    const float yaw = state.yaw_degrees * radians_per_degree;
    const float pitch = state.pitch_degrees * radians_per_degree;
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float ground = cp + height_projection * sp;
    const float height = sp - height_projection * cp;
    const float depth_ground = height_projection * cp - sp;
    Matrix result{
        view.zoom * cy,
        0,
        -view.zoom * sy,
        0,
        view.zoom * ground * sy,
        view.zoom * height,
        view.zoom * ground * cy,
        0,
        depth_ground * sy,
        ground,
        depth_ground * cy,
        0
    };
    result[3] = view.screen_x - result[0] * view.focus_x - result[2] * view.focus_z;
    result[7] = view.screen_y - result[4] * view.focus_x - result[5] * view.focus_y -
                result[6] * view.focus_z;
    result[11] = -result[8] * view.focus_x - result[9] * view.focus_y - result[10] * view.focus_z;
    if (!finite_matrix(result))
        return false;
    matrix = result;
    return true;
}

bool zoom_about(
    const State& state, View& view, float zoom, float screen_x, float screen_y, float world_height
) noexcept {
    Matrix before{}, after{};
    View next = view;
    next.zoom = zoom;
    float before_x{}, before_z{}, after_x{}, after_z{};
    if (!projection(state, view, before) || !projection(state, next, after) ||
        !world_at_height(before, screen_x, screen_y, world_height, before_x, before_z) ||
        !world_at_height(after, screen_x, screen_y, world_height, after_x, after_z))
        return false;
    next.focus_x += before_x - after_x;
    next.focus_z += before_z - after_z;
    if (!projection(state, next, after))
        return false;
    view = next;
    return true;
}

bool pan_delta(
    const Matrix& matrix, float screen_dx, float screen_dy, float& world_dx, float& world_dz
) noexcept {
    if (!finite_matrix(matrix) || !std::isfinite(screen_dx) || !std::isfinite(screen_dy))
        return false;
    const float determinant = matrix[0] * matrix[6] - matrix[2] * matrix[4];
    if (!std::isfinite(determinant) || std::abs(determinant) < minimum_determinant)
        return false;
    const float x = (screen_dx * matrix[6] - matrix[2] * screen_dy) / determinant;
    const float z = (matrix[0] * screen_dy - screen_dx * matrix[4]) / determinant;
    if (!std::isfinite(x) || !std::isfinite(z))
        return false;
    world_dx = x;
    world_dz = z;
    return true;
}

bool world_at_height(
    const Matrix& matrix,
    float screen_x,
    float screen_y,
    float world_height,
    float& world_x,
    float& world_z
) noexcept {
    if (!std::isfinite(world_height))
        return false;
    return pan_delta(
        matrix,
        screen_x - matrix[1] * world_height - matrix[3],
        screen_y - matrix[5] * world_height - matrix[7],
        world_x,
        world_z
    );
}

bool visible_terrain_bounds(
    const Matrix& matrix,
    float viewport_width,
    float viewport_height,
    float margin,
    TerrainBounds& bounds
) noexcept {
    if (!std::isfinite(viewport_width) || !std::isfinite(viewport_height) ||
        !std::isfinite(margin) || viewport_width <= 0 || viewport_height <= 0 || margin < 0)
        return false;
    constexpr float maximum_terrain_height = 255;
    constexpr float infinity = std::numeric_limits<float>::infinity();
    TerrainBounds result{infinity, infinity, -infinity, -infinity};
    for (const float height : {0.0F, maximum_terrain_height})
        for (const float screen_y : {0.0F, viewport_height})
            for (const float screen_x : {0.0F, viewport_width}) {
                float world_x{}, world_z{};
                if (!world_at_height(matrix, screen_x, screen_y, height, world_x, world_z))
                    return false;
                const float painted_y = world_z - height_projection * height;
                result.min_x = std::min(result.min_x, world_x);
                result.min_y = std::min(result.min_y, painted_y);
                result.max_x = std::max(result.max_x, world_x);
                result.max_y = std::max(result.max_y, painted_y);
            }
    result.min_x = std::nextafter(result.min_x - margin, -infinity);
    result.min_y = std::nextafter(result.min_y - margin, -infinity);
    result.max_x = std::nextafter(result.max_x + margin, infinity);
    result.max_y = std::nextafter(result.max_y + margin, infinity);
    if (!std::isfinite(result.min_x) || !std::isfinite(result.min_y) ||
        !std::isfinite(result.max_x) || !std::isfinite(result.max_y))
        return false;
    bounds = result;
    return true;
}

} // namespace oa::app::renderer_live_camera
