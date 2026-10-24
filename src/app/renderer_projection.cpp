// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "renderer_projection.hpp"

#include <cmath>

namespace oa::app::renderer_projection {
namespace {

/// The classic view's screen displacement per world pixel of height.
constexpr double height_projection = 0.5;

/// Checks that every camera value is finite and the scale is positive.
bool valid_projection(const ClassicProjection& projection) noexcept {
    return std::isfinite(projection.camera_x) && std::isfinite(projection.camera_y) &&
           std::isfinite(projection.scale) && projection.scale > 0.0 &&
           std::isfinite(projection.origin_x) && std::isfinite(projection.origin_y);
}

/// Checks that all three world coordinates are finite.
bool finite_point(const WorldPoint& point) noexcept {
    return std::isfinite(point.x) && std::isfinite(point.height) && std::isfinite(point.z);
}

} // namespace

std::optional<ScreenPoint>
project_classic(const ClassicProjection& projection, const WorldPoint& point) noexcept {
    if (!valid_projection(projection) || !finite_point(point))
        return std::nullopt;
    const ScreenPoint screen{
        (point.x - projection.camera_x) * projection.scale + projection.origin_x,
        (point.z - point.height * height_projection - projection.camera_y) * projection.scale +
            projection.origin_y,
        point.height + point.z * height_projection
    };
    if (!std::isfinite(screen.x) || !std::isfinite(screen.y) || !std::isfinite(screen.depth))
        return std::nullopt;
    return screen;
}

std::optional<WorldPoint> unproject_classic_at_height(
    const ClassicProjection& projection, double screen_x, double screen_y, double height
) noexcept {
    if (!valid_projection(projection) || !std::isfinite(screen_x) || !std::isfinite(screen_y) ||
        !std::isfinite(height))
        return std::nullopt;
    const WorldPoint point{
        (screen_x - projection.origin_x) / projection.scale + projection.camera_x,
        height,
        (screen_y - projection.origin_y) / projection.scale + projection.camera_y +
            height * height_projection
    };
    if (!finite_point(point))
        return std::nullopt;
    return point;
}

std::optional<WorldPoint>
terrain_point_from_painted_map(double painted_x, double painted_y, double height) noexcept {
    if (!std::isfinite(painted_x) || !std::isfinite(painted_y) || !std::isfinite(height))
        return std::nullopt;
    const WorldPoint point{painted_x, height, painted_y + height * height_projection};
    if (!finite_point(point))
        return std::nullopt;
    return point;
}

} // namespace oa::app::renderer_projection
