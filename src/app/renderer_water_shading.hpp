// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "oa/app/card.hpp"
#include "renderer_water_appearance.hpp"
#include <algorithm>
#include <array>
#include <span>
#include <vector>
#include <cmath>

namespace oa::app::renderer_water_shading {
/// Shade a submerged point in world coordinates; above-water points remain exact.
inline void apply(
    card::Vertex& vertex,
    float x,
    float z,
    float depth,
    double seconds,
    float intensity,
    float scale
) {
    if (depth <= 0 || intensity <= 0)
        return;
    namespace appearance = renderer_water_appearance;
    const float strength = std::min(intensity, 2.0F);
    const float immersion = std::clamp(depth / appearance::immersion_distance, 0.0F, 1.0F);
    const float depth_fraction = std::clamp(depth / appearance::tint_distance, 0.0F, 1.0F);
    const float weight = std::min(
        appearance::tint_max,
        (appearance::tint_base + appearance::tint_depth * depth_fraction) * strength * immersion
    );
    const float transmission =
        1 - std::min(
                appearance::absorption_max,
                (appearance::absorption_base + appearance::absorption_depth * depth_fraction) *
                    strength * immersion
            );
    const float phase = float(seconds);
    const float a = std::sin(x * 0.11F + z * 0.07F + phase * 1.1F);
    const float b = std::sin(x * -0.065F + z * 0.115F - phase * 0.8F);
    const float caustic = std::pow(std::max(0.0F, 1 - std::abs(a + b)), 3);
    const float fade =
        1 -
        std::clamp(
            (depth - appearance::caustic_fade_start) / appearance::caustic_fade_range, 0.0F, 1.0F
        );
    const float light = (1 + (appearance::caustic_gain * caustic + appearance::caustic_bias) *
                                 strength * fade * immersion) *
                        transmission;
    vertex.colour.red = std::clamp(
        vertex.colour.red * (1 - weight * (1 - appearance::tint_red)) * light, 0.0F, 1.0F
    );
    vertex.colour.green = std::clamp(
        vertex.colour.green * (1 - weight * (1 - appearance::tint_green)) * light, 0.0F, 1.0F
    );
    vertex.colour.blue = std::clamp(
        vertex.colour.blue * (1 - weight * (1 - appearance::tint_blue)) * light, 0.0F, 1.0F
    );
    // A submerged contact highlight ends at the physical waterline.
    const float rim = std::max(
                          0.0F,
                          std::min(
                              depth / appearance::rim_peak_depth,
                              (appearance::rim_width - depth) /
                                  (appearance::rim_width - appearance::rim_peak_depth)
                          )
                      ) *
                      appearance::rim_strength * strength;
    vertex.colour.red = std::min(1.0F, vertex.colour.red + rim * appearance::rim_red);
    vertex.colour.green = std::min(1.0F, vertex.colour.green + rim * appearance::rim_green);
    vertex.colour.blue = std::min(1.0F, vertex.colour.blue + rim * appearance::rim_blue);
    vertex.x += std::sin(z * 0.1F + phase * 1.6F) * 0.6F * strength * fade * immersion * scale;
}

/// Split triangles at the waterline and bounded depth bands before vertex shading.
/// Interpolated texture coordinates and dry colours remain those of the source face.
template <class Depth, class Shade>
inline void split(
    std::span<const card::Vertex> vertices,
    std::span<const card::Index> indices,
    std::vector<card::Vertex>& output,
    std::vector<card::Index>& triangles,
    Depth depth_of,
    Shade shade
) {
    struct Point {
        card::Vertex vertex;
        float depth;
    };

    constexpr std::array<float, 7> boundaries{
        0,
        renderer_water_appearance::rim_peak_depth,
        renderer_water_appearance::rim_width,
        renderer_water_appearance::immersion_distance,
        renderer_water_appearance::caustic_fade_start,
        renderer_water_appearance::tint_distance,
        renderer_water_appearance::caustic_fade_start +
            renderer_water_appearance::caustic_fade_range
    };
    output.clear();
    triangles.clear();
    const auto interpolate = [](const Point& a, const Point& b, float depth) {
        const float t = (depth - a.depth) / (b.depth - a.depth);
        Point point{a.vertex, depth};
        const auto mix = [t](float x, float y) { return x + (y - x) * t; };
        point.vertex.x = mix(a.vertex.x, b.vertex.x);
        point.vertex.y = mix(a.vertex.y, b.vertex.y);
        point.vertex.u = mix(a.vertex.u, b.vertex.u);
        point.vertex.v = mix(a.vertex.v, b.vertex.v);
        point.vertex.colour = {
            mix(a.vertex.colour.red, b.vertex.colour.red),
            mix(a.vertex.colour.green, b.vertex.colour.green),
            mix(a.vertex.colour.blue, b.vertex.colour.blue),
            mix(a.vertex.colour.alpha, b.vertex.colour.alpha)
        };
        return point;
    };
    const auto emit = [&](std::span<const Point> points) {
        if (points.size() < 3)
            return;
        const auto base = static_cast<card::Index>(output.size());
        for (const auto& point : points) {
            auto vertex = point.vertex;
            if (point.depth > 0)
                shade(vertex, point.depth);
            output.push_back(vertex);
        }
        for (card::Index k = 1; k + 1 < points.size(); ++k)
            triangles.insert(triangles.end(), {base, base + k, base + k + 1});
    };
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        std::array<Point, 8> remaining{}, below{}, above{};
        std::size_t count = 3;
        for (std::size_t k = 0; k < 3; ++k) {
            const auto& vertex = vertices[indices[i + k]];
            remaining[k] = {vertex, depth_of(vertex)};
        }
        for (float boundary : boundaries) {
            bool all_below = true, all_above = true;
            for (std::size_t k = 0; k < count; ++k) {
                all_below = all_below && remaining[k].depth <= boundary;
                all_above = all_above && remaining[k].depth >= boundary;
            }
            if (all_below) {
                emit(std::span(remaining.data(), count));
                count = 0;
                break;
            }
            if (all_above)
                continue;
            std::size_t dry_count = 0, wet_count = 0;
            for (std::size_t k = 0; k < count; ++k) {
                const auto& a = remaining[k];
                const auto& b = remaining[(k + 1) % count];
                if (a.depth <= boundary)
                    below[dry_count++] = a;
                if (a.depth >= boundary)
                    above[wet_count++] = a;
                if ((a.depth < boundary && b.depth > boundary) ||
                    (a.depth > boundary && b.depth < boundary)) {
                    const auto point = interpolate(a, b, boundary);
                    below[dry_count++] = point;
                    above[wet_count++] = point;
                }
            }
            emit(std::span(below.data(), dry_count));
            remaining = above;
            count = wet_count;
            if (count < 3)
                break;
        }
        emit(std::span(remaining.data(), count));
    }
}
} // namespace oa::app::renderer_water_shading
