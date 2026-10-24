// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "renderer_water.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace oa::app::renderer_water_surface {
/// A deterministic presentation sample, independent of simulation random state.
inline float hash(int x, int z) {
    uint32_t value = uint32_t(x) * 0x8da6b343U ^ uint32_t(z) * 0xd8163841U;
    value ^= value >> 13;
    value *= 0x85ebca6bU;
    value ^= value >> 16;
    return float(value & 65535U) / 65535.0F;
}

/// Smooth bounded noise without periodic parallel bands.
inline float noise(float x, float z) {
    const int ix = int(std::floor(x)), iz = int(std::floor(z));
    const float fx = x - ix, fz = z - iz;
    const float u = fx * fx * (3 - 2 * fx), v = fz * fz * (3 - 2 * fz);
    const float a = hash(ix, iz) * (1 - u) + hash(ix + 1, iz) * u;
    const float b = hash(ix, iz + 1) * (1 - u) + hash(ix + 1, iz + 1) * u;
    return a * (1 - v) + b * v;
}

/// Diffuse components travel at different speeds along one stable map flow.
inline float ripple_alpha(float x, float z, const renderer_water::Wind& wind, float zoom = 1) {
    const float medium = noise((x - wind.drift_x * 0.65F) / 34, (z - wind.drift_z * 0.65F) / 22);
    const float long_wave =
        noise((x + 37 - wind.drift_x * 0.28F) / 79, (z - 19 - wind.drift_z * 0.28F) / 53);
    const float broad = 0.65F * medium + 0.35F * long_wave;
    const float close = std::clamp((zoom - 0.5F) / 0.75F, 0.0F, 1.0F);
    const float fine_share = 0.4F * close * close * (3 - 2 * close);
    const float fine =
        noise((x + 11 - wind.drift_x * 1.35F) / 8, (z - 7 - wind.drift_z * 1.35F) / 11);
    const auto soft_patch = [](float field) {
        const float patch = std::clamp((field - 0.48F) / 0.34F, 0.0F, 1.0F);
        return patch * patch * (3 - 2 * patch);
    };
    const float patch = soft_patch(broad) * (1 - fine_share) + soft_patch(fine) * fine_share;
    return 0.14F * (0.12F + 0.88F * wind.strength) * patch;
}
} // namespace oa::app::renderer_water_surface
