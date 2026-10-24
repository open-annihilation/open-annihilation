// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "renderer_water_shading.hpp"

namespace oa::app::renderer_water_effects {
/// Attenuate a light source at its true depth; the fourth result scales air heat distortion.
inline std::array<float, 4> source(
    float red,
    float green,
    float blue,
    float x,
    float z,
    float depth,
    double seconds,
    float intensity
) {
    card::Vertex vertex{};
    vertex.colour = {red, green, blue, 1};
    renderer_water_shading::apply(vertex, x, z, depth, seconds, intensity, 0);
    const float immersion =
        std::clamp(depth / renderer_water_appearance::immersion_distance, 0.0F, 1.0F);
    return {
        vertex.colour.red,
        vertex.colour.green,
        vertex.colour.blue,
        1 - std::clamp(intensity * immersion, 0.0F, 1.0F)
    };
}
} // namespace oa::app::renderer_water_effects
