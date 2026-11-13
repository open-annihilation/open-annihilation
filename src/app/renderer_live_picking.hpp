// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "renderer_live_camera.hpp"
#include "renderer_live_scene.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>

namespace oa::app::renderer_live_picking {

/// Returns compact coordinates only for the exact current terrain prefix.
/// Missing coordinates leave picking on its original GPU vertex records.
/// @param scene the complete, successfully built scene
/// @param enabled the match permits the compact-coordinate path
/// @return matching owned coordinates, or an empty span
inline std::span<const renderer_live_scene::GroundCoordinate>
ground_coordinates(const renderer_live_scene::Scene& scene, bool enabled) noexcept {
    const auto count = scene.terrain_vertex_count;
    if (enabled && scene.ground_revision != 0 &&
        scene.ground_points_revision == scene.ground_revision &&
        scene.ground_points.size() == count && count <= scene.vertices.size())
        return scene.ground_points;
    return {};
}

/// Projects a complete block and preserves the ordinary sliver bounds.
/// Coordinate fields have the same exact bits on both source paths.
/// @param vertices the scene's ordinary GPU records
/// @param ground matching compact terrain coordinates, or none
/// @param projection the exact current camera rows
/// @param first the block's first vertex
/// @param end the vertex after the block, within vertices and projected
/// @param[out] projected the scene's screen coordinates and depths
/// @return conservative screen bounds, unbounded for a sliver
inline std::array<float, 4> project_block(
    std::span<const renderer_live_gpu::Vertex> vertices,
    std::span<const renderer_live_scene::GroundCoordinate> ground,
    const renderer_live_camera::Matrix& projection,
    std::size_t first,
    std::size_t end,
    std::span<std::array<float, 3>> projected
) {
    constexpr float none = std::numeric_limits<float>::infinity();
    std::array<float, 4> bounds{none, none, -none, -none};
    bool bounded = true;
    const auto& m = projection;
    const auto project = [&](const auto coordinates) {
        for (std::size_t index = first; index < end; ++index) {
            const auto& vertex = coordinates[index];
            auto& point = projected[index];
            point = {
                m[0] * vertex.x + m[1] * vertex.height + m[2] * vertex.z + m[3],
                m[4] * vertex.x + m[5] * vertex.height + m[6] * vertex.z + m[7],
                m[8] * vertex.x + m[9] * vertex.height + m[10] * vertex.z + m[11]
            };
            if (point[0] < bounds[0])
                bounds[0] = point[0];
            if (point[1] < bounds[1])
                bounds[1] = point[1];
            if (point[0] > bounds[2])
                bounds[2] = point[0];
            if (point[1] > bounds[3])
                bounds[3] = point[1];
            if (index % 3 == 2 && bounded)
                bounded = renderer_live_camera::triangle_hit_bounded(
                    {projected[index - 2], projected[index - 1], point}
                );
        }
    };
    if (!ground.empty() && end <= ground.size())
        project(ground);
    else
        project(vertices);
    return bounded ? bounds : std::array{-none, -none, none, none};
}

} // namespace oa::app::renderer_live_picking
