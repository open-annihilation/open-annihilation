// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "renderer_live_camera.hpp"
#include "renderer_live_gpu.hpp"
#include "oa/formats/tnt.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace oa::app::renderer_live_frame {

/// Current camera geometry, without changing the view or opening GPU resources.
struct Prepared {
    renderer_live_camera::View view{};
    renderer_live_camera::Matrix projection{};
    std::array<int32_t, 4> crop{}; // left, top, right, bottom
    bool projected{};
};

/// Prepares the current view and its painted terrain crop in one arithmetic path.
/// @param map the selected terrain
/// @param camera current orientation
/// @param place the current exact view place
/// @param zoom current screen pixels per map pixel
/// @param width current battlefield columns
/// @param height current battlefield rows
/// @param turned whether this frame uses the rotated view
/// @param pivot_held whether the preceding view held its pivot height
/// @param pivot_height the preceding view's pivot height
/// @return the current view, projection and crop; projected is false for invalid geometry
[[nodiscard]] inline Prepared prepare(
    const oa::formats::tnt::Map& map,
    const renderer_live_camera::State& camera,
    const std::array<double, 2>& place,
    float zoom,
    float width,
    float height,
    bool turned,
    bool pivot_held,
    float pivot_height
) noexcept {
    constexpr int terrain_cell_edge = 16;
    constexpr float height_lift = 0.5F;
    Prepared result;
    const float focus_x = static_cast<float>(place[0]) + width / (2 * zoom);
    const float focus_y = static_cast<float>(place[1]) + height / (2 * zoom);
    float elevation{};
    if (map.attribute_width && map.attribute_height && !map.attributes.empty()) {
        constexpr double least_coordinate = std::numeric_limits<int>::min();
        constexpr double greatest_coordinate = std::numeric_limits<int>::max();
        if (!std::isfinite(focus_x) || !std::isfinite(focus_y) ||
            double{focus_x} < least_coordinate || double{focus_x} > greatest_coordinate ||
            double{focus_y} < least_coordinate || double{focus_y} > greatest_coordinate)
            return result;
        const auto column = std::clamp(
            static_cast<int>(focus_x) / terrain_cell_edge,
            0,
            static_cast<int>(map.attribute_width) - 1
        );
        const auto row = std::clamp(
            static_cast<int>(focus_y) / terrain_cell_edge,
            0,
            static_cast<int>(map.attribute_height) - 1
        );
        const auto index = static_cast<std::size_t>(row) * map.attribute_width + column;
        if (index >= map.attributes.size())
            return result;
        elevation = map.attributes[index].height;
    }
    // A held orbit pivot keeps its height while the view crosses terrain cells.
    if (turned && pivot_held)
        elevation = pivot_height;
    result.view = {
        focus_x, elevation, focus_y + elevation * height_lift, width * 0.5F, height * 0.5F, zoom
    };
    result.projected = renderer_live_camera::projection(camera, result.view, result.projection);
    if (!result.projected || !turned)
        return result;
    const int32_t map_width = static_cast<int32_t>(map.attribute_width) * terrain_cell_edge;
    const int32_t map_height = static_cast<int32_t>(map.attribute_height) * terrain_cell_edge;
    result.crop = {0, 0, map_width, map_height};
    constexpr float terrain_margin = 2 * terrain_cell_edge;
    renderer_live_camera::TerrainBounds visible{};
    if (renderer_live_camera::visible_terrain_bounds(
            result.projection, width, height, terrain_margin, visible
        )) {
        result.crop[0] =
            static_cast<int32_t>(std::clamp(std::floor(visible.min_x), 0.0F, float(map_width)));
        result.crop[1] =
            static_cast<int32_t>(std::clamp(std::floor(visible.min_y), 0.0F, float(map_height)));
        result.crop[2] =
            static_cast<int32_t>(std::clamp(std::ceil(visible.max_x), 0.0F, float(map_width)));
        result.crop[3] =
            static_cast<int32_t>(std::clamp(std::ceil(visible.max_y), 0.0F, float(map_height)));
    }
    return result;
}

/// Returns the active rounded shadow-map extent of a painted terrain crop.
/// @param left first painted column
/// @param top first painted row
/// @param right crop's exclusive right edge
/// @param bottom crop's exclusive bottom edge
/// @return the active map, including its rounded edge texels
[[nodiscard]] inline renderer_live_gpu::ShadowMap
shadow_map_for_crop(int32_t left, int32_t top, int32_t right, int32_t bottom) noexcept {
    constexpr uint32_t widest = renderer_live_gpu::largest_edge;
    constexpr float coarsest = 8;
    renderer_live_gpu::ShadowMap map;
    map.left = static_cast<float>(left);
    map.top = static_cast<float>(top);
    const auto across = static_cast<float>(std::max(1, right - left));
    const auto down = static_cast<float>(std::max(1, bottom - top));
    while (map.scale < coarsest && (across / map.scale > widest || down / map.scale > widest))
        map.scale *= 2;
    map.width = std::min(widest, static_cast<uint32_t>(std::ceil(across / map.scale)));
    map.height = std::min(widest, static_cast<uint32_t>(std::ceil(down / map.scale)));
    return map;
}

} // namespace oa::app::renderer_live_frame
