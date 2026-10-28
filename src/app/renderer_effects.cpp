// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_effects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace oa::app::renderer_effects {
namespace {
constexpr size_t max_sources = 128;
constexpr int max_dimension = 16384;
constexpr float max_radius = 256;
constexpr float light_energy = 110;
constexpr float glow_energy = 150;
constexpr float max_displacement = 2;
constexpr float glow_radius_fraction = 0.35f;
constexpr float silhouette_depth_fraction = 0.08f;
/// Rows of each band of the surface the lights are composed in.
constexpr uint32_t band_rows = 32;

/// Reject malformed input before any pixel indexing or float-to-integer conversion.
bool valid_source(const Source& source) {
    for (const float value :
         {source.screen_x,
          source.screen_y,
          source.depth,
          source.radius_pixels,
          source.depth_radius,
          source.red,
          source.green,
          source.blue,
          source.light,
          source.glow,
          source.heat,
          source.phase,
          source.radius_y_pixels}) {
        if (!std::isfinite(value))
            return false;
    }
    return source.radius_pixels > 0 && source.depth_radius >= 0 && source.radius_y_pixels >= 0;
}

/// Keep normalized colours bounded even for an overbright source.
float unit(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

/// Permit the user-selected double-strength effect while bounding malformed energy.
float strength(float value) {
    return std::clamp(value, 0.0F, 2.0F);
}
} // namespace

std::array<float, 2> detail_strength(float radius_pixels, float zoom) noexcept {
    if (!std::isfinite(radius_pixels) || !std::isfinite(zoom) || radius_pixels <= 0 || zoom <= 0)
        return {};
    const auto smooth = [](float value) {
        const float t = std::clamp(value, 0.0F, 1.0F);
        return t * t * (3.0F - 2.0F * t);
    };
    constexpr float invisible_radius = 1;
    constexpr float full_detail_radius = 8;
    constexpr float heat_start_zoom = 0.35F;
    constexpr float heat_full_zoom = 1;
    const float light =
        smooth((radius_pixels - invisible_radius) / (full_detail_radius - invisible_radius));
    return {light, light * smooth((zoom - heat_start_zoom) / (heat_full_zoom - heat_start_zoom))};
}

void compose(
    Surface surface,
    std::span<const Source> sources,
    Settings settings,
    double simulation_seconds,
    platform::job_pool::Pool* pool
) {
    if ((!settings.coloured_lighting && !settings.wreck_glow && !settings.heat_sizzle) ||
        sources.empty() || !std::isfinite(simulation_seconds))
        return;
    if (surface.width <= 0 || surface.height <= 0 || surface.width > max_dimension ||
        surface.height > max_dimension || (surface.channels != 3 && surface.channels != 4) ||
        surface.stride_bytes < surface.width * surface.channels)
        return;
    const auto count = size_t(surface.width) * size_t(surface.height);
    const auto bytes = size_t(surface.stride_bytes) * size_t(surface.height);
    if (surface.pixels.size() < bytes ||
        (!surface.visibility.empty() && surface.visibility.size() < count) ||
        (!surface.depth.empty() && surface.depth.size() < count))
        return;
    std::vector<uint8_t> undistorted;
    if (settings.heat_sizzle &&
        std::any_of(sources.begin(), sources.end(), [](const Source& source) {
            return source.heat >= 0.25F;
        }))
        undistorted.assign(surface.pixels.begin(), surface.pixels.begin() + bytes);
    const double animation_period = 120;
    const float time = float(std::fmod(simulation_seconds, animation_period));
    // Each visibility byte as the share of the light it lets through.
    std::array<float, 256> visibility_share{};
    for (size_t value = 0; value < visibility_share.size(); ++value)
        visibility_share[value] = float(value) / 255.0f;

    // Where each light reaches, and each of its columns' distance from its
    // centre in its radii.
    struct Reach {
        const Source* source{};
        float radius{};
        float radius_y{};
        int left{};
        int top{};
        int right{};
        int bottom{};
        std::array<float, 3> colour{};
        bool heat_enabled{};
        float glow_strength{}; ///< zero for a light without a glow, which adds nothing to it
        size_t first_column{}; ///< the light's first entry in column_dx
    };

    std::vector<Reach> reaches;
    std::vector<float> column_dx;
    for (const auto& source : sources.first(std::min(sources.size(), max_sources))) {
        if (!valid_source(source))
            continue;
        const float radius = std::min(source.radius_pixels, max_radius);
        const float radius_y =
            source.radius_y_pixels > 0 ? std::min(source.radius_y_pixels, max_radius) : radius;
        if (source.screen_x + radius < 0 || source.screen_y + radius_y < 0 ||
            source.screen_x - radius >= float(surface.width) ||
            source.screen_y - radius_y >= float(surface.height))
            continue;
        Reach reach{&source, radius, radius_y};
        reach.left = int(std::max(0.0f, source.screen_x - radius));
        reach.top = int(std::max(0.0f, source.screen_y - radius_y));
        reach.right = int(std::min(float(surface.width - 1), source.screen_x + radius));
        reach.bottom = int(std::min(float(surface.height - 1), source.screen_y + radius_y));
        reach.colour = {unit(source.red), unit(source.green), unit(source.blue)};
        reach.heat_enabled = settings.heat_sizzle && source.heat >= 0.25F;
        reach.glow_strength = strength(source.glow);
        reach.first_column = column_dx.size();
        for (int x = reach.left; x <= reach.right; ++x)
            column_dx.push_back((float(x) - source.screen_x) / radius);
        reaches.push_back(reach);
    }
    // Each band of rows takes every light in turn, so each pixel takes the
    // lights in the same order whatever the bands and threads; a light reads
    // and writes the rows of its own band alone.
    const auto bands = platform::job_pool::bands_of_rows(uint32_t(surface.height), band_rows);
    platform::job_pool::run_bands(pool, bands, [&](uint32_t band) {
        const int band_top = int(band * band_rows);
        const int band_bottom = std::min(surface.height, band_top + int(band_rows)) - 1;
        for (const auto& reach : reaches) {
            const Source& source = *reach.source;
            const float radius = reach.radius;
            const float radius_y = reach.radius_y;
            const int left = reach.left;
            const int right = reach.right;
            const auto& colour = reach.colour;
            const bool heat_enabled = reach.heat_enabled;
            const float glow_strength = reach.glow_strength;
            for (int y = std::max(reach.top, band_top); y <= std::min(reach.bottom, band_bottom);
                 ++y) {
                const float dy = (float(y) - source.screen_y) / radius_y;
                // Nothing a radius or more above or below the centre is reached,
                // nor anything two pixels or more beyond the ellipse's width on
                // the row: its reach is below zero by far more than rounding.
                if (dy * dy >= 1)
                    continue;
                const float half_width = radius * std::sqrt(1 - dy * dy);
                const int first = std::max(left, int(std::floor(source.screen_x - half_width)) - 1);
                const int last = std::min(right, int(std::ceil(source.screen_x + half_width)) + 1);
                constexpr float spatial_frequency = 0.55f;
                constexpr float temporal_frequency = 11;
                const float displacement = heat_enabled
                                               ? std::sin(
                                                     float(y) * spatial_frequency -
                                                     time * temporal_frequency + source.phase
                                                 ) * max_displacement *
                                                     strength(source.heat)
                                               : 0;
                for (int x = first; x <= last; ++x) {
                    const auto index = size_t(y) * size_t(surface.width) + size_t(x);
                    const float visibility = surface.visibility.empty()
                                                 ? 1
                                                 : visibility_share[surface.visibility[index]];
                    if (visibility == 0)
                        continue;
                    const float dx = column_dx[reach.first_column + size_t(x - left)];
                    const float distance_squared = dx * dx + dy * dy;
                    float falloff = 1 - distance_squared;
                    float depth_delta = 0;
                    if (!surface.depth.empty()) {
                        if (!std::isfinite(surface.depth[index]) || source.depth_radius <= 0)
                            continue;
                        depth_delta = surface.depth[index] - source.depth;
                        const float dz = depth_delta / source.depth_radius;
                        falloff -= dz * dz;
                    }
                    if (falloff <= 0)
                        continue;
                    const float attenuation = falloff * falloff * visibility;
                    const auto offset = size_t(y) * size_t(surface.stride_bytes) +
                                        size_t(x) * size_t(surface.channels);
                    const bool foreground =
                        depth_delta > source.depth_radius * silhouette_depth_fraction;
                    if (heat_enabled && !foreground) {
                        // Preserve fractional movement so weak heat shimmers instead of jumping
                        // between complete pixels. Both taps must stay on the same visible surface.
                        const float sample_x = std::clamp(
                            float(x) + displacement * attenuation, 0.0F, float(surface.width - 1)
                        );
                        const int sample_left = int(sample_x);
                        const int sample_right = std::min(sample_left + 1, surface.width - 1);
                        const float fraction = sample_x - float(sample_left);
                        const auto same_surface = [&](int sample_column) {
                            const auto sample_index =
                                size_t(y) * size_t(surface.width) + size_t(sample_column);
                            const bool same_visibility =
                                surface.visibility.empty() ||
                                surface.visibility[sample_index] == surface.visibility[index];
                            const bool same_depth =
                                surface.depth.empty() ||
                                (std::isfinite(surface.depth[sample_index]) &&
                                 std::abs(surface.depth[sample_index] - surface.depth[index]) <=
                                     source.depth_radius * silhouette_depth_fraction);
                            return same_visibility && same_depth;
                        };
                        if (same_surface(sample_left) &&
                            (fraction == 0 || same_surface(sample_right))) {
                            const auto left_offset = size_t(y) * size_t(surface.stride_bytes) +
                                                     size_t(sample_left) * size_t(surface.channels);
                            const auto right_offset =
                                size_t(y) * size_t(surface.stride_bytes) +
                                size_t(sample_right) * size_t(surface.channels);
                            for (int channel = 0; channel < 3; ++channel) {
                                const auto address = offset + size_t(channel);
                                const float left_sample =
                                    float(undistorted[left_offset + size_t(channel)]);
                                const float right_sample =
                                    float(undistorted[right_offset + size_t(channel)]);
                                const float difference = left_sample +
                                                         (right_sample - left_sample) * fraction -
                                                         float(undistorted[address]);
                                surface.pixels[address] = uint8_t(
                                    std::clamp(
                                        std::round(float(surface.pixels[address]) + difference),
                                        0.0F,
                                        255.0F
                                    )
                                );
                            }
                        }
                    }
                    float energy = settings.coloured_lighting
                                       ? strength(source.light) * light_energy * attenuation
                                       : 0;
                    if (settings.wreck_glow && !foreground && glow_strength > 0) {
                        const float core = std::max(
                            0.0f,
                            1 - distance_squared / (glow_radius_fraction * glow_radius_fraction)
                        );
                        energy += glow_strength * glow_energy * core * core * visibility;
                    }
                    // The light's whole steps, whatever the channel held: its
                    // sum with the channel could otherwise round up a step.
                    for (int channel = 0; channel < 3; ++channel) {
                        const auto address = offset + size_t(channel);
                        const float base = float(surface.pixels[address]);
                        surface.pixels[address] =
                            uint8_t(std::min(255.0f, base + std::floor(energy * colour[channel])));
                    }
                }
            }
        }
    });
}
} // namespace oa::app::renderer_effects
