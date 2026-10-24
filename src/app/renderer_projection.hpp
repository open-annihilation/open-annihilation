// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <optional>

namespace oa::app::renderer_projection {

/// A presentation position in world pixels, with height positive upwards.
struct WorldPoint {
    double x{};
    double height{};
    double z{};
};

/// A projected position in viewport pixels and an unnormalised depth.
struct ScreenPoint {
    double x{};
    double y{};
    double depth{}; ///< height + z / 2; larger values lie towards the viewer
};

/// The classic oblique view; camera offsets are in unscaled map pixels.
struct ClassicProjection {
    double camera_x{};
    double camera_y{};
    double scale{1.0}; ///< viewport pixels per map pixel; finite and positive
    double origin_x{}; ///< viewport origin in pixels
    double origin_y{}; ///< viewport origin in pixels
};

/// Projects a world position through the classic oblique view.
///
/// @param projection the camera offsets, positive scale and viewport origin
/// @param point the world position in pixels
/// @return screen coordinates and depth, or nullopt for nonfinite input,
///     invalid scale or nonfinite output
[[nodiscard]] std::optional<ScreenPoint>
project_classic(const ClassicProjection& projection, const WorldPoint& point) noexcept;

/// Intersects the classic view ray with a plane of known height.
///
/// This is a plane intersection, not terrain picking; zero height selects
/// the ground plane. No depth-buffer normalisation is involved.
///
/// @param projection the camera offsets, positive scale and viewport origin
/// @param screen_x horizontal viewport coordinate in pixels
/// @param screen_y vertical viewport coordinate in pixels
/// @param height the plane's height in world pixels
/// @return the world position, or nullopt for nonfinite input, invalid scale
///     or nonfinite output
[[nodiscard]] std::optional<WorldPoint> unproject_classic_at_height(
    const ClassicProjection& projection, double screen_x, double screen_y, double height
) noexcept;

/// Lifts a painted terrain pixel onto geometry without moving its classic view.
///
/// The returned z coordinate is painted_y + height / 2, so projection at
/// unit scale and zero camera offset returns the painted pixel coordinates.
///
/// @param painted_x horizontal terrain texture coordinate in map pixels
/// @param painted_y vertical terrain texture coordinate in map pixels
/// @param height the terrain height in world pixels
/// @return the world position, or nullopt for nonfinite input or output
[[nodiscard]] std::optional<WorldPoint>
terrain_point_from_painted_map(double painted_x, double painted_y, double height) noexcept;

} // namespace oa::app::renderer_projection
