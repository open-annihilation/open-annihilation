// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace oa::app::renderer_aircraft_shadow {
/// Altitude, in map pixels above the ground or water under an aircraft,
/// from which its shadow takes the wider soft edge; a fighter's and a
/// bomber's cruise heights are above it.
inline constexpr float wide_edge_altitude = 96;
/// The blur radius of a shadow's soft edge below wide_edge_altitude, in
/// pixels of the game's picture; the edge fades out over twice as many.
inline constexpr uint32_t narrow_edge_radius = 1;
/// The blur radius from wide_edge_altitude up, in pixels of the game's picture.
inline constexpr uint32_t wide_edge_radius = 2;

/// Returns the blur radius of an aircraft shadow's soft edge.
///
/// Only the edge follows the height: the shadow keeps the game's place,
/// size and darkness at every altitude, as on the other tiers.
///
/// @param altitude height above the ground or water under the aircraft, in map pixels
/// @return narrow_edge_radius near the ground, wide_edge_radius from
///     wide_edge_altitude up
inline uint32_t edge_radius(float altitude) noexcept {
    return std::isfinite(altitude) && altitude >= wide_edge_altitude ? wide_edge_radius
                                                                     : narrow_edge_radius;
}

/// Gives a binary silhouette a soft edge, padded by twice the radius.
///
/// Two separable box blurs of the silhouette, at twice their strength, lie
/// under the silhouette itself, so that every pixel the silhouette covers
/// keeps its full alpha and the edge fades from it to nothing past it.
/// Each pass uses a running sum, so the cost is bounded by image area
/// rather than radius.
///
/// @param binary the silhouette, a row after another, nonzero where it covers
/// @param width the silhouette's width, at most 2048 pixels
/// @param height the silhouette's height, at most 2048 pixels
/// @param radius the blur radius, in pixels; one above wide_edge_radius is taken as it
/// @return the alpha, 0 to 255, of width plus four radii by height plus
///     four radii pixels, or none for a silhouette of no size, too large or
///     shorter than its size
inline std::vector<uint8_t>
soften(std::span<const uint8_t> binary, uint32_t width, uint32_t height, uint32_t radius) {
    radius = std::min(radius, wide_edge_radius);
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
    const std::vector<uint8_t> solid = alpha;
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
    for (std::size_t i = 0; i < alpha.size(); ++i)
        alpha[i] = std::max(solid[i], static_cast<uint8_t>(std::min(255, alpha[i] * 2)));
    return alpha;
}
} // namespace oa::app::renderer_aircraft_shadow
