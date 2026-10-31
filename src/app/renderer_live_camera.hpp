// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

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

/// A point's weights and depth on a prepared triangle, and whether it hits it.
struct PreparedHit {
    std::array<float, 3> weights{}; ///< barycentric weights, meaningful on a hit
    float depth{};                  ///< interpolated depth, meaningful on a hit
    bool hit{};                     ///< whether triangle_hit_prepared accepts the point
};

/// Weigh a finite point on a valid prepared triangle without branching on
/// the result. triangle_hit_prepared is this after its own checks, so the
/// two give the same weights, depth and answer for every such point; a
/// caller testing many points works them all out before testing any, so
/// that one point's divisions need not wait for the last point's answer.
/// Inline for that reason.
/// @param triangle coefficients for which prepare_triangle succeeded
/// @param screen_x finite pointer column in pixels
/// @param screen_y finite pointer row in pixels
/// @return the weights and depth, with whether the point hits
[[nodiscard]] inline PreparedHit
weigh_prepared(const PreparedTriangle& triangle, float screen_x, float screen_y) noexcept {
    const double first = (triangle.first_x * (double(screen_x) - triangle.origin_x) +
                          triangle.first_y * (double(screen_y) - triangle.origin_y)) /
                         triangle.determinant;
    const double second = (triangle.second_x * (double(screen_x) - triangle.origin_x) +
                           triangle.second_y * (double(screen_y) - triangle.origin_y)) /
                          triangle.determinant;
    const double third = 1 - first - second;
    constexpr double edge_tolerance = 1e-6;
    const float hit_depth = static_cast<float>(
        first * triangle.depths[0] + second * triangle.depths[1] + third * triangle.depths[2]
    );
    // Each test once, joined without short cuts.
    const bool hit = !(first < -edge_tolerance) & !(second < -edge_tolerance) &
                     !(third < -edge_tolerance) & std::isfinite(hit_depth);
    return {
        {static_cast<float>(first), static_cast<float>(second), static_cast<float>(third)},
        hit_depth,
        hit
    };
}

/// Intersect a point using prepared coefficients, preserving triangle_hit rounding.
/// @param triangle coefficients returned by prepare_triangle
/// @param screen_x pointer column in pixels
/// @param screen_y pointer row in pixels
/// @param[out] weights barycentric weights, unchanged on a miss
/// @param[out] depth interpolated depth, unchanged on a miss
/// @return whether the point hits the prepared triangle
[[nodiscard]] inline bool triangle_hit_prepared(
    const PreparedTriangle& triangle,
    float screen_x,
    float screen_y,
    std::array<float, 3>& weights,
    float& depth
) noexcept {
    if (!triangle.valid || !std::isfinite(screen_x) || !std::isfinite(screen_y))
        return false;
    const auto point = weigh_prepared(triangle, screen_x, screen_y);
    if (!point.hit)
        return false;
    weights = point.weights;
    depth = point.depth;
    return true;
}

/// The most columns of a row narrow_row leaves whole: too few to repay narrowing.
inline constexpr int most_columns_left_whole = 4;

/// What narrow_row works out from a prepared triangle alone, once for all
/// the rows a caller narrows.
struct RowNarrowing {
    /// Each weight's numerator's change a row, before the determinant's sign.
    std::array<double, 3> row_terms{};
    /// One over each weight's numerator's change a column, the sign taken out.
    std::array<double, 3> inverse_slopes{};
    /// Whether each weight bounds a row's columns, and from which side.
    std::array<int8_t, 3> sides{}; ///< 1 from the left, -1 from the right, 0 not at all
    double sign{};                 ///< the determinant's sign
    double size{};                 ///< the determinant's magnitude
    double limit{};                ///< the least numerator a column can keep
    float origin_x{};
    float origin_y{};
    bool valid{};
};

/// Work out the row-independent terms of narrow_row for a prepared triangle.
/// @param triangle coefficients returned by prepare_triangle
/// @return the terms; invalid for an invalid triangle
[[nodiscard]] inline RowNarrowing prepare_narrowing(const PreparedTriangle& triangle) noexcept {
    RowNarrowing narrowing;
    if (!triangle.valid)
        return narrowing;
    // Each weight's numerator along the row is slope * (x - origin_x) + offset,
    // here with the determinant's sign taken out, and the weight is at least
    // its tolerance where the numerator is at least -limit. The third weight
    // is one less the other two. The limit is twice the hit's tolerance and
    // each bound has half a column to spare: far more than the rounding of
    // either test, so no column the hit accepts is left out.
    narrowing.sign = triangle.determinant < 0 ? -1.0 : 1.0;
    narrowing.size = std::abs(triangle.determinant);
    const std::array<double, 3> slopes{
        triangle.first_x * narrowing.sign,
        triangle.second_x * narrowing.sign,
        -(triangle.first_x + triangle.second_x) * narrowing.sign
    };
    narrowing.row_terms = {
        triangle.first_y, triangle.second_y, triangle.first_y + triangle.second_y
    };
    constexpr double limit_tolerance = 2e-6;
    // A weight that changes by less than this share of the determinant a
    // column leaves the row whole: its bound would rest on rounding.
    constexpr double minimum_slope = 1e-9;
    narrowing.limit = narrowing.size * limit_tolerance;
    for (std::size_t edge = 0; edge < slopes.size(); ++edge) {
        if (std::abs(slopes[edge]) <= narrowing.size * minimum_slope)
            continue;
        narrowing.inverse_slopes[edge] = 1 / slopes[edge];
        narrowing.sides[edge] = slopes[edge] > 0 ? 1 : -1;
    }
    narrowing.origin_x = triangle.origin_x;
    narrowing.origin_y = triangle.origin_y;
    narrowing.valid = true;
    return narrowing;
}

/// Narrow a row's pixel columns to those whose centres a prepared triangle can hit.
/// Every column in [first, last) whose centre triangle_hit_prepared accepts stays
/// in the range, with at most a column more on either side; the range may come
/// back empty, and a range of a few columns is left whole. Columns keep their
/// order, so a caller's results do not change. Inline, as the lights'
/// visibility narrows every row of every triangle it draws.
/// @param narrowing the triangle's terms from prepare_narrowing
/// @param screen_y the row's centre in pixels
/// @param centre where a column's centre lies past its left edge, in pixels
/// @param[in,out] first the row's first column, raised to the triangle's
/// @param[in,out] last one past the row's last column, lowered to the triangle's
inline void narrow_row(
    const RowNarrowing& narrowing, float screen_y, float centre, int& first, int& last
) noexcept {
    if (!narrowing.valid || !std::isfinite(screen_y) || !std::isfinite(centre) ||
        last - first <= most_columns_left_whole)
        return;
    const double row = double(screen_y) - narrowing.origin_y;
    const std::array<double, 3> offsets{
        narrowing.row_terms[0] * row * narrowing.sign,
        narrowing.row_terms[1] * row * narrowing.sign,
        narrowing.size - narrowing.row_terms[2] * row * narrowing.sign
    };
    constexpr double spare = 0.5;
    double low = -std::numeric_limits<double>::infinity();
    double high = std::numeric_limits<double>::infinity();
    for (std::size_t edge = 0; edge < offsets.size(); ++edge) {
        if (narrowing.sides[edge] == 0)
            continue;
        const double bound = (-narrowing.limit - offsets[edge]) * narrowing.inverse_slopes[edge];
        if (!std::isfinite(bound))
            continue;
        if (narrowing.sides[edge] > 0)
            low = std::max(low, bound - spare);
        else
            high = std::min(high, bound + spare);
    }
    // Back to columns: those whose centres lie within [low, high].
    const double left = std::max(double(first), std::ceil(low + narrowing.origin_x - centre));
    const double right = std::min(double(last), std::floor(high + narrowing.origin_x - centre) + 1);
    if (!(left < right)) {
        last = first;
        return;
    }
    first = static_cast<int>(left);
    last = static_cast<int>(right);
}

/// Narrow a row's pixel columns to those whose centres a prepared triangle can
/// hit, as narrow_row with the triangle's prepare_narrowing terms.
/// @param triangle coefficients returned by prepare_triangle
/// @param screen_y the row's centre in pixels
/// @param centre where a column's centre lies past its left edge, in pixels
/// @param[in,out] first the row's first column, raised to the triangle's
/// @param[in,out] last one past the row's last column, lowered to the triangle's
inline void narrow_row(
    const PreparedTriangle& triangle, float screen_y, float centre, int& first, int& last
) noexcept {
    narrow_row(prepare_narrowing(triangle), screen_y, centre, first, last);
}

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
