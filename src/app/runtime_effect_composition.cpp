// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/runtime.hpp"
#include "renderer_effect_sources.hpp"
#include "renderer_effects.hpp"
#include "renderer_water.hpp"
#include "renderer_water_effects.hpp"
#include "oa/present/world_renderer/world_fog.hpp"
#include "oa/sim/visibility_state.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace oa::app {
namespace {
constexpr float minimum_light_strength = 1.0F / 110.0F;
constexpr float minimum_glow_strength = 1.0F / 150.0F;
constexpr float minimum_heat_strength = 0.25F;

/// Select a source's light independently from its heat distortion.
float light_intensity(
    const ui::engine_settings::EngineSettings& settings, renderer_effect_sources::Kind kind
) {
    switch (kind) {
    case renderer_effect_sources::Kind::weapon:
        return std::min(settings.weapon_lighting, 200U) / 100.0F;
    case renderer_effect_sources::Kind::nanolathe:
        return std::min(settings.nanolathe_lighting, 200U) / 100.0F;
    case renderer_effect_sources::Kind::wreck:
        return std::min(settings.wreck_glow, 200U) / 100.0F;
    }
    return 0;
}
} // namespace

bool Runtime::match_effects_enabled() {
    return match_effects_in_view(false);
}

bool Runtime::match_effects_distort() {
    return match_effects_in_view(true);
}

bool Runtime::match_effects_in_view(bool distortion_only) {
    if (!match_)
        return false;
    const auto& settings = engine_settings();
    const auto place = match_view_place();
    const float zoom = match_zoom();
    for (const auto& source : match_effect_sources()) {
        const auto detail = renderer_effects::detail_strength(source.radius * zoom, zoom);
        const float light_scale = light_intensity(settings, source.kind);
        const float heat_scale = std::min(settings.heat_distortion, 200U) / 100.0F;
        const bool lights =
            std::clamp(source.intensity, 0.0F, 1.0F) * detail[0] * light_scale >=
                minimum_light_strength ||
            std::clamp(source.glow, 0.0F, 1.0F) * detail[0] * light_scale >= minimum_glow_strength;
        const bool distorts =
            std::clamp(source.heat, 0.0F, 1.0F) * detail[1] * heat_scale >= minimum_heat_strength;
        if (distorts || (lights && !distortion_only)) {
            float x = (source.x - static_cast<float>(place[0])) * zoom;
            float y = (source.z - source.height * 0.5F - static_cast<float>(place[1])) * zoom;
            if (live_renderer_active()) {
                const auto point = live_project_world(source.x, source.height, source.z);
                x = static_cast<float>(point.x - match_layout_.left);
                y = static_cast<float>(point.y - match_layout_.top);
            }
            // The camera's vertical projection stretches a world sphere by sqrt(1.25).
            constexpr float vertical_projection_scale = 1.11803399F;
            const float radius =
                source.radius * zoom * (live_renderer_active() ? vertical_projection_scale : 1.0F);
            if (x + radius >= 0 && y + radius >= 0 &&
                x - radius < match_layout_.battlefield_width() &&
                y - radius < match_layout_.battlefield_height())
                return true;
        }
    }
    return false;
}

void Runtime::compose_match_effects(renderer::Surface& surface, bool live) {
    compose_match_effect_pixels(
        surface.rgb, surface.width, surface.height, surface.width * 3, 3, 0, 0, 1, 1, live
    );
}

void Runtime::compose_match_effect_pixels(
    std::span<uint8_t> pixels,
    uint32_t width,
    uint32_t height,
    uint32_t pitch,
    uint32_t channels,
    float origin_x,
    float origin_y,
    float scale_x,
    float scale_y,
    bool live,
    float effect_row_offset
) {
    if (!match_ || !selected_tnt_ || !match_effects_enabled() || width == 0 || height == 0 ||
        width > 8192 || height > 8192 || !std::isfinite(scale_x) || !std::isfinite(scale_y) ||
        scale_x <= 0 || scale_y <= 0)
        return;
    const float zoom = match_zoom();
    if (!std::isfinite(zoom) || zoom <= 0)
        return;
    const auto place = match_view_place();
    const auto& settings = engine_settings();
    // Underwater shimmer's strength, which a lava sea leaves at none.
    const float shimmer = std::min(water_levels().shimmer, 2.0F);
    std::vector<renderer_effects::Source> projected;
    projected.reserve(match_effect_sources().size());
    constexpr float heat_spatial_frequency = 0.55F;
    float depth_scale = 1;
    if (live) {
        const float zero = live_project_depth(0, 0, 0);
        const float dx = live_project_depth(1, 0, 0) - zero;
        const float dy = live_project_depth(0, 1, 0) - zero;
        const float dz = live_project_depth(0, 0, 1) - zero;
        depth_scale = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    for (const auto& source : match_effect_sources()) {
        const auto detail = renderer_effects::detail_strength(source.radius * zoom, zoom);
        const float light_scale = light_intensity(settings, source.kind);
        const float heat_scale = std::min(settings.heat_distortion, 200U) / 100.0F;
        const float light = std::clamp(source.intensity, 0.0F, 1.0F) * detail[0] * light_scale;
        const float glow = std::clamp(source.glow, 0.0F, 1.0F) * detail[0] * light_scale;
        const auto water = renderer_water_effects::source(
            source.red,
            source.green,
            source.blue,
            source.x,
            source.z,
            float(selected_tnt_->sea_level) - source.height,
            match_->simulation().tick / 30.0,
            shimmer * std::clamp((zoom - 0.25F) / 0.25F, 0.0F, 1.0F)
        );
        const float heat = std::clamp(source.heat, 0.0F, 1.0F) * detail[1] * heat_scale * water[3];
        if (light < minimum_light_strength && glow < minimum_glow_strength &&
            heat < minimum_heat_strength)
            continue;
        float x = (source.x - static_cast<float>(place[0])) * zoom;
        float y = (source.z - source.height * 0.5F - static_cast<float>(place[1])) * zoom;
        float depth{};
        if (live) {
            const auto point = live_project_world(source.x, source.height, source.z);
            x = static_cast<float>(point.x - match_layout_.left);
            y = static_cast<float>(point.y - match_layout_.top);
            depth = live_project_depth(source.x, source.height, source.z);
        }
        const float radius = source.radius * zoom * std::max(scale_x, scale_y);
        const float radius_y = radius * (live ? depth_scale : 1.0F);
        x = (x - origin_x) * scale_x;
        y = (y - origin_y) * scale_y;
        if (x + radius < 0 || y + radius_y < 0 || x - radius >= width || y - radius_y >= height)
            continue;
        projected.push_back(
            {x,
             y,
             depth,
             radius,
             source.radius * depth_scale,
             water[0],
             water[1],
             water[2],
             light,
             glow,
             heat,
             source.x * 0.13F + source.z * 0.17F + effect_row_offset * heat_spatial_frequency,
             radius_y}
        );
    }
    if (projected.empty())
        return;
    // Only pixels reached by an effect need visibility sampling.
    std::vector<uint32_t> row_left(height, width), row_right(height, 0);
    std::vector<std::array<uint32_t, 4>> regions;
    std::size_t region_pixels = 0;
    for (const auto& source : projected) {
        constexpr float maximum_radius = 256;
        constexpr float heat_sample_margin = 4;
        const float radius = std::min(source.radius_pixels, maximum_radius) + heat_sample_margin;
        const float radius_y =
            std::min(source.radius_y_pixels, maximum_radius) + heat_sample_margin;
        if (!std::isfinite(source.screen_x) || !std::isfinite(source.screen_y) ||
            !std::isfinite(radius) || radius <= 0 || !std::isfinite(radius_y) || radius_y <= 0)
            continue;
        const auto left = static_cast<uint32_t>(
            std::clamp(std::floor(source.screen_x - radius), 0.0F, float(width))
        );
        const auto right = static_cast<uint32_t>(
            std::clamp(std::ceil(source.screen_x + radius + 1), 0.0F, float(width))
        );
        const auto top = static_cast<uint32_t>(
            std::clamp(std::floor(source.screen_y - radius_y), 0.0F, float(height))
        );
        const auto bottom = static_cast<uint32_t>(
            std::clamp(std::ceil(source.screen_y + radius_y + 1), 0.0F, float(height))
        );
        regions.push_back({left, top, right, bottom});
        region_pixels += static_cast<std::size_t>(right - left) * (bottom - top);
        for (auto y = top; y < bottom; ++y) {
            row_left[y] = std::min(row_left[y], left);
            row_right[y] = std::max(row_right[y], right);
        }
    }
    std::vector<uint8_t> visibility;
    std::vector<float> depths;
    if (live) {
        std::size_t span_pixels = 0;
        for (uint32_t y = 0; y < height; ++y)
            if (row_right[y] > row_left[y])
                span_pixels += row_right[y] - row_left[y];
        std::vector<uint8_t> requested_pixels;
        // A row interval can bridge distant lights. Keep the simple row path
        // when their rectangles overlap or leave little unused space.
        constexpr std::size_t sparse_region_margin = 4;
        if (regions.size() > 1 &&
            span_pixels > region_pixels + region_pixels / sparse_region_margin) {
            requested_pixels.resize(static_cast<std::size_t>(width) * height);
            for (const auto& region : regions)
                for (auto y = region[1]; y < region[3]; ++y) {
                    const auto offset = static_cast<std::size_t>(y) * width;
                    std::fill(
                        requested_pixels.begin() + offset + region[0],
                        requested_pixels.begin() + offset + region[2],
                        1
                    );
                }
        }
        live_scene_visibility(visibility, depths, false, row_left, row_right, requested_pixels);
        if (visibility.size() != static_cast<std::size_t>(width) * height)
            return;
    } else {
        const auto& sight = match_->sight();
        const auto coverage = match_->player_coverage(match_view_player());
        // The view player's mapped bit, and those of the players whose ground
        // it is shown under allied vision.
        const auto own = static_cast<uint16_t>(1U << (match_view_player() & 0x1fU));
        const present::world_renderer::FogOptions fog_rules{
            match_line_of_sight_on(), match_mapping_on(), allied_mapped_bits()
        };
        const auto bit = static_cast<uint16_t>(own | fog_rules.shared_mapped);
        constexpr float cell = present::world_renderer::fog_cell_pixels;
        const bool mapping = fog_rules.mapping;
        const auto visible = [&](int column, int row) {
            if (column < 0 || row < 0 || column >= sight.width || row >= sight.height)
                return false;
            const auto index = static_cast<std::size_t>(row) * sight.width + column;
            if (mapping &&
                (index >= sight.player_bits.size() || (sight.player_bits[index] & bit) == 0))
                return false;
            return present::world_renderer::cell_shown_in_sight(
                index < coverage.size() ? coverage[index] : uint16_t{0},
                index < sight.player_bits.size() ? sight.player_bits[index] : uint16_t{0},
                own,
                fog_rules
            );
        };
        visibility.resize(static_cast<std::size_t>(width) * height);
        // The compose reads only pixels inside the lights' rectangles. Each
        // pixel column's map position and cell are worked out once, and a
        // cell's sight and its eight neighbours' once along a row.
        uint32_t reach_left = width;
        uint32_t reach_right = 0;
        for (uint32_t y = 0; y < height; ++y)
            if (row_left[y] < row_right[y]) {
                reach_left = std::min(reach_left, row_left[y]);
                reach_right = std::max(reach_right, row_right[y]);
            }
        std::vector<float> map_xs;
        std::vector<int> columns;
        for (uint32_t x = reach_left; x < reach_right; ++x) {
            const float map_x =
                static_cast<float>(place[0]) + (origin_x + (x + 0.5F) / scale_x) / zoom;
            map_xs.push_back(map_x);
            columns.push_back(static_cast<int>(std::floor(map_x / cell)));
        }
        // Bit (dz + 1) * 3 + dx + 1 holds whether the cell dx, dz from a
        // pixel's own cell is in sight; bit 4 is the pixel's own cell.
        constexpr uint16_t own_cell = 1U << 4;
        constexpr uint16_t every_cell = 0x1ff;
        std::vector<std::array<uint32_t, 2>> spans;
        for (uint32_t y = 0; y < height; ++y) {
            if (row_left[y] >= row_right[y])
                continue;
            const float map_y =
                static_cast<float>(place[1]) + (origin_y + (y + 0.5F) / scale_y) / zoom;
            const int row = static_cast<int>(std::floor(map_y / cell));
            spans.clear();
            for (const auto& region : regions)
                if (region[1] <= y && y < region[3] && region[0] < region[2])
                    spans.push_back({region[0], region[2]});
            std::sort(spans.begin(), spans.end());
            bool sight_known = false;
            int sight_column = 0;
            uint16_t in_sight = 0;
            uint32_t filled = 0;
            for (const auto& span : spans) {
                for (uint32_t x = std::max(span[0], filled); x < span[1]; ++x) {
                    const int column = columns[x - reach_left];
                    if (!sight_known || column != sight_column) {
                        in_sight = 0;
                        for (int dz = -1; dz <= 1; ++dz)
                            for (int dx = -1; dx <= 1; ++dx)
                                if (visible(column + dx, row + dz))
                                    in_sight |=
                                        static_cast<uint16_t>(1U << ((dz + 1) * 3 + dx + 1));
                        sight_known = true;
                        sight_column = column;
                    }
                    if ((in_sight & own_cell) == 0)
                        continue;
                    uint8_t& pixel_visibility = visibility[static_cast<std::size_t>(y) * width + x];
                    if (in_sight == every_cell) {
                        pixel_visibility = 255;
                        continue;
                    }
                    const float map_x = map_xs[x - reach_left];
                    // Fade toward unseen neighbours without adding light to their pixels.
                    float attenuation = 1;
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dx = -1; dx <= 1; ++dx)
                            if ((in_sight & (1U << ((dz + 1) * 3 + dx + 1))) == 0) {
                                const float distance_x = std::max(
                                    {(column + dx) * cell - map_x,
                                     map_x - (column + dx + 1) * cell,
                                     0.0F}
                                );
                                const float distance_y = std::max(
                                    {(row + dz) * cell - map_y, map_y - (row + dz + 1) * cell, 0.0F}
                                );
                                attenuation = std::min(
                                    attenuation,
                                    std::clamp(
                                        std::hypot(distance_x, distance_y) / (cell * 0.5F),
                                        0.0F,
                                        1.0F
                                    )
                                );
                            }
                    pixel_visibility = static_cast<uint8_t>(attenuation * 255);
                }
                filled = std::max(filled, span[1]);
            }
        }
    }
    renderer_effects::compose(
        {pixels,
         static_cast<int>(width),
         static_cast<int>(height),
         static_cast<int>(pitch),
         static_cast<int>(channels),
         visibility,
         depths},
        projected,
        {true, true, settings.heat_distortion != 0},
        static_cast<double>(match_->simulation().tick) / 30.0,
        draw_pool_.get()
    );
}

} // namespace oa::app
