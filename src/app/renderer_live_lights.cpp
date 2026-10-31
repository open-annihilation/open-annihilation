// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_live_lights.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace oa::app::renderer_live_lights {
namespace {
constexpr uint32_t max_dimension = 16384;
} // namespace

void pack(
    std::span<const renderer_effects::Source> sources,
    renderer_effects::Settings settings,
    double simulation_seconds,
    uint32_t width,
    uint32_t height,
    Table& table
) {
    table.lights.clear();
    table.tiles.clear();
    table.count = 0;
    table.tiles_across = 0;
    table.tiles_down = 0;
    if ((!settings.coloured_lighting && !settings.wreck_glow && !settings.heat_sizzle) ||
        sources.empty() || !std::isfinite(simulation_seconds) || width == 0 || height == 0 ||
        width > max_dimension || height > max_dimension)
        return;
    table.tiles_across = (width + tile_edge - 1) / tile_edge;
    table.tiles_down = (height + tile_edge - 1) / tile_edge;
    table.tiles.assign(std::size_t(table.tiles_across) * table.tiles_down * words_per_tile, 0);
    const float time = float(std::fmod(simulation_seconds, renderer_effects::animation_period));
    const auto strength = [](float value) { return std::clamp(value, 0.0F, 2.0F); };
    const auto unit = [](float value) { return std::clamp(value, 0.0F, 1.0F); };
    for (const auto& source :
         sources.first(std::min(sources.size(), renderer_effects::max_sources))) {
        if (!renderer_effects::valid_source(source))
            continue;
        const float radius = std::min(source.radius_pixels, renderer_effects::max_radius);
        const float radius_y = source.radius_y_pixels > 0
                                   ? std::min(source.radius_y_pixels, renderer_effects::max_radius)
                                   : radius;
        if (source.screen_x + radius < 0 || source.screen_y + radius_y < 0 ||
            source.screen_x - radius >= float(width) || source.screen_y - radius_y >= float(height))
            continue;
        const float light = settings.coloured_lighting ? strength(source.light) : 0;
        const float glow = settings.wreck_glow ? strength(source.glow) : 0;
        const bool distorts = settings.heat_sizzle && source.heat >= renderer_effects::minimum_heat;
        const float heat = distorts ? strength(source.heat) : 0;
        // A light that neither lights, glows nor distorts adds nothing.
        if (light == 0 && glow == 0 && heat == 0)
            continue;
        // Heat's phase at the frame's time, within a turn, so that the card's
        // sine takes the row's angle alone.
        const double phase = std::fmod(
            double(source.phase) - double(time) * renderer_effects::heat_temporal_frequency,
            2 * std::numbers::pi
        );
        table.lights.insert(
            table.lights.end(),
            {source.screen_x,
             source.screen_y,
             source.depth,
             source.depth_radius,
             radius,
             radius_y,
             float(phase),
             heat,
             unit(source.red),
             unit(source.green),
             unit(source.blue),
             light,
             glow,
             0,
             0,
             0}
        );
        // The tiles the light's reach crosses: its rectangle as compose()
        // bounds it.
        const auto left = uint32_t(std::max(0.0F, source.screen_x - radius));
        const auto top = uint32_t(std::max(0.0F, source.screen_y - radius_y));
        const auto right = uint32_t(std::min(float(width - 1), source.screen_x + radius));
        const auto bottom = uint32_t(std::min(float(height - 1), source.screen_y + radius_y));
        const uint32_t word = table.count / 32;
        const uint32_t bit = 1U << (table.count % 32);
        for (uint32_t row = top / tile_edge; row <= bottom / tile_edge; ++row)
            for (uint32_t column = left / tile_edge; column <= right / tile_edge; ++column)
                table.tiles
                    [(std::size_t(row) * table.tiles_across + column) * words_per_tile + word] |=
                    bit;
        ++table.count;
    }
    if (table.count == 0) {
        table.tiles.clear();
        table.tiles_across = 0;
        table.tiles_down = 0;
    }
}

std::array<float, 12> parameters(uint32_t width, uint32_t height) {
    return {
        renderer_effects::light_energy,
        renderer_effects::glow_energy,
        renderer_effects::glow_radius_fraction * renderer_effects::glow_radius_fraction,
        renderer_effects::silhouette_depth_fraction,
        float(width) - 1,
        float(height) - 1,
        renderer_effects::heat_spatial_frequency,
        renderer_effects::max_displacement,
        float(tile_edge),
        0,
        0,
        0
    };
}

} // namespace oa::app::renderer_live_lights
