// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>

namespace oa::app::renderer_live_camera {

/// Keep presentation-only camera angles outside the saved game records.
struct State {
    float yaw_degrees{};
    float pitch_degrees{}; ///< elevation change from the classic view
    bool unlocked{};
};

/// Place a world focus at a viewport point, in pixels.
struct View {
    float focus_x{};
    float focus_y{};
    float focus_z{};
    float screen_x{};
    float screen_y{};
    float zoom{1};
};

/// Store row-major affine screen x, screen y and depth over world x, height, z, 1.
/// Larger depth is closer to the viewer.
using Matrix = std::array<float, 12>;

/// Hold constant barycentric coefficients for repeated screen-point queries.
struct PreparedTriangle {
    double first_x{};
    double first_y{};
    double second_x{};
    double second_y{};
    double determinant{};
    float origin_x{};
    float origin_y{};
    std::array<float, 3> depths{};
    bool valid{};
};

/// Prepare a projected triangle once for repeated pixel intersections.
/// @param triangle three screen x, screen y, depth vertices
/// @param[out] prepared coefficients; invalid on failure
/// @return whether the triangle is finite and nondegenerate
[[nodiscard]] bool prepare_triangle(
    const std::array<std::array<float, 3>, 3>& triangle, PreparedTriangle& prepared
) noexcept;

/// Intersect a point using prepared coefficients, preserving triangle_hit rounding.
/// @param triangle coefficients returned by prepare_triangle
/// @param screen_x pointer column in pixels
/// @param screen_y pointer row in pixels
/// @param[out] weights barycentric weights, unchanged on a miss
/// @param[out] depth interpolated depth, unchanged on a miss
/// @return whether the point hits the prepared triangle
[[nodiscard]] bool triangle_hit_prepared(
    const PreparedTriangle& triangle,
    float screen_x,
    float screen_y,
    std::array<float, 3>& weights,
    float& depth
) noexcept;

/// Intersect a screen point with a projected triangle and interpolate its depth.
/// @param triangle three screen x, screen y, depth vertices
/// @param screen_x pointer column in pixels
/// @param screen_y pointer row in pixels
/// @param[out] weights barycentric weights, unchanged on a miss
/// @param[out] depth interpolated depth, unchanged on a miss
/// @return whether the point hits a finite, nondegenerate triangle
[[nodiscard]] bool triangle_hit(
    const std::array<std::array<float, 3>, 3>& triangle,
    float screen_x,
    float screen_y,
    std::array<float, 3>& weights,
    float& depth
) noexcept;

/// Orbit within the current camera limits; ignore nonfinite deltas.
/// @param[in,out] state camera angles and unlock setting
/// @param yaw_delta_degrees horizontal rotation in degrees
/// @param pitch_delta_degrees elevation change in degrees
void orbit(State& state, float yaw_delta_degrees, float pitch_delta_degrees) noexcept;

/// Set camera freedom and immediately clamp angles to the new limits.
/// @param[in,out] state camera angles and unlock setting
/// @param unlocked whether to allow the wider range
void set_unlocked(State& state, bool unlocked) noexcept;

/// Restore classic orientation and restricted motion.
/// @param[out] state camera angles and unlock setting
void reset(State& state) noexcept;

/// Build a camera transform; classic orientation preserves x and z-height/2 exactly.
/// @param state camera angles and unlock setting
/// @param view focus, viewport centre and positive pixel scale
/// @param[out] matrix transform, unchanged on failure
/// @return false for nonfinite inputs, invalid angles or nonpositive zoom
[[nodiscard]] bool projection(const State& state, const View& view, Matrix& matrix) noexcept;

/// Change scale while keeping the world point at the pointer on the same screen pixel.
/// @param state camera orientation
/// @param[in,out] view current view, unchanged on invalid input
/// @param zoom positive new pixel scale
/// @param screen_x pointer column relative to the viewport
/// @param screen_y pointer row relative to the viewport
/// @param world_height terrain height at the pointer
/// @return whether the view was updated
[[nodiscard]] bool zoom_about(
    const State& state, View& view, float zoom, float screen_x, float screen_y, float world_height
) noexcept;

/// Intersect a screen ray with a horizontal world plane.
/// @param matrix camera transform
/// @param screen_x horizontal viewport coordinate in pixels
/// @param screen_y vertical viewport coordinate in pixels
/// @param world_height plane height in world pixels
/// @param[out] world_x horizontal world position, unchanged on failure
/// @param[out] world_z vertical map position, unchanged on failure
/// @return false for nonfinite input or singular projection
[[nodiscard]] bool world_at_height(
    const Matrix& matrix,
    float screen_x,
    float screen_y,
    float world_height,
    float& world_x,
    float& world_z
) noexcept;

/// Bound visible terrain in painted map pixels, where y is world z minus half height.
struct TerrainBounds {
    float min_x{};
    float min_y{};
    float max_x{};
    float max_y{};
};

/// Bound the viewport's terrain rays over all terrain heights from 0 through 255.
/// The affine projection reaches its extrema at viewport corners and height endpoints.
/// @param matrix camera transform, with viewport coordinates starting at zero
/// @param viewport_width positive viewport width in screen pixels
/// @param viewport_height positive viewport height in screen pixels
/// @param margin nonnegative expansion on each side in painted map pixels
/// @param[out] bounds conservative painted map bounds, unchanged on failure
/// @return false for nonfinite input or result, invalid dimensions or a singular projection
[[nodiscard]] bool visible_terrain_bounds(
    const Matrix& matrix,
    float viewport_width,
    float viewport_height,
    float margin,
    TerrainBounds& bounds
) noexcept;

/// Convert a screen displacement to a displacement along the world ground plane.
/// @param matrix camera transform
/// @param screen_dx horizontal screen displacement in pixels
/// @param screen_dy vertical screen displacement in pixels
/// @param[out] world_dx horizontal world displacement, unchanged on failure
/// @param[out] world_dz vertical map displacement, unchanged on failure
/// @return false for nonfinite input or singular projection
[[nodiscard]] bool pan_delta(
    const Matrix& matrix, float screen_dx, float screen_dy, float& world_dx, float& world_dz
) noexcept;

} // namespace oa::app::renderer_live_camera
