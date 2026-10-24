// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace oa::app::renderer_aircraft_shadow {
/// Bounded altitude-dependent footprint, density and silhouette blur.
struct Style {
    float spread{1};
    float opacity{1};
    uint32_t radius{};
};

/// Keep landed aircraft tight and dark; soften and fade high aircraft continuously.
inline Style style(float altitude) noexcept {
    const float height = std::isfinite(altitude) ? std::clamp(altitude / 192.0F, 0.0F, 1.0F) : 0;
    const float amount = height * height * (3 - 2 * height);
    return {1 + amount * 0.45F, 1 - amount * 0.7F, static_cast<uint32_t>(std::lround(amount * 8))};
}

/// Pad a binary silhouette by twice the radius, then apply two separable box blurs.
/// Each pass uses a running sum, so the cost is bounded by image area rather than radius.
inline std::vector<uint8_t>
soften(std::span<const uint8_t> binary, uint32_t width, uint32_t height, uint32_t radius) {
    radius = std::min(radius, 8U);
    if (width == 0 || height == 0 || width > 2048 || height > 2048 ||
        binary.size() < static_cast<std::size_t>(width) * height)
        return {};
    const uint32_t padding = radius * 2;
    const uint32_t columns = width + padding * 2;
    const uint32_t rows = height + padding * 2;
    std::vector<uint8_t> alpha(static_cast<std::size_t>(columns) * rows);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            alpha[(y + padding) * columns + x + padding] = binary[y * width + x] ? 255 : 0;
    if (radius == 0)
        return alpha;
    std::vector<uint8_t> scratch(alpha.size());
    const int32_t reach = static_cast<int32_t>(radius);
    const uint32_t divisor = radius * 2 + 1;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        for (uint32_t y = 0; y < rows; ++y) {
            uint32_t sum{};
            for (uint32_t x = 0; x <= radius && x < columns; ++x)
                sum += alpha[y * columns + x];
            for (int32_t x = 0; x < static_cast<int32_t>(columns); ++x) {
                scratch[y * columns + x] = static_cast<uint8_t>((sum + divisor / 2) / divisor);
                if (x - reach >= 0)
                    sum -= alpha[y * columns + x - reach];
                if (x + reach + 1 < static_cast<int32_t>(columns))
                    sum += alpha[y * columns + x + reach + 1];
            }
        }
        for (uint32_t x = 0; x < columns; ++x) {
            uint32_t sum{};
            for (uint32_t y = 0; y <= radius && y < rows; ++y)
                sum += scratch[y * columns + x];
            for (int32_t y = 0; y < static_cast<int32_t>(rows); ++y) {
                alpha[y * columns + x] = static_cast<uint8_t>((sum + divisor / 2) / divisor);
                if (y - reach >= 0)
                    sum -= scratch[(y - reach) * columns + x];
                if (y + reach + 1 < static_cast<int32_t>(rows))
                    sum += scratch[(y + reach + 1) * columns + x];
            }
        }
    }
    return alpha;
}
} // namespace oa::app::renderer_aircraft_shadow
