// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::app::renderer_model_shading {

inline constexpr uint8_t shade_rows = 32;
inline constexpr std::size_t maximum_corners = 16;

/// Splits a convex textured polygon into the integer rows of its indexed shading.
///
/// Each callback receives one clipped polygon and its palette-table row.
/// The interpolator retains texture coordinates and every other attribute.
///
/// @param polygon convex corners, in their draw order
/// @param shade returns a corner's interpolated shade row
/// @param mix interpolates two corners at a fraction from zero to one
/// @param emit receives the row and its borrowed polygon
/// @return false for too many corners, an invalid row, or clipping overflow
template <class Vertex, class Shade, class Mix, class Emit>
bool for_shade_bands(std::span<const Vertex> polygon, Shade shade, Mix mix, Emit emit) {
    if (polygon.size() < 3 || polygon.size() > maximum_corners)
        return false;
    float low = static_cast<float>(shade_rows - 1);
    float high = 0;
    for (const auto& vertex : polygon) {
        const float row = shade(vertex);
        if (!std::isfinite(row) || row < 0 || row > shade_rows - 1)
            return false;
        low = std::min(low, row);
        high = std::max(high, row);
    }
    const auto first = static_cast<uint8_t>(std::floor(low));
    const auto last = static_cast<uint8_t>(std::floor(high));
    if (first == last) {
        emit(first, polygon);
        return true;
    }
    for (uint8_t row = first; row <= last; ++row) {
        std::array<Vertex, maximum_corners> a{}, b{};
        std::copy(polygon.begin(), polygon.end(), a.begin());
        std::size_t count = polygon.size();
        for (int edge = 0; edge < 2 && count != 0; ++edge) {
            const float boundary = static_cast<float>(row + edge);
            std::size_t output = 0;
            const auto append = [&](const Vertex& vertex) {
                if (output == b.size())
                    return false;
                b[output++] = vertex;
                return true;
            };
            for (std::size_t index = 0; index < count; ++index) {
                const auto& from = a[index];
                const auto& to = a[(index + 1) % count];
                const float from_row = shade(from), to_row = shade(to);
                const bool from_inside = edge == 0 ? from_row >= boundary : from_row <= boundary;
                const bool to_inside = edge == 0 ? to_row >= boundary : to_row <= boundary;
                if (from_inside && !append(from))
                    return false;
                if (from_inside != to_inside &&
                    !append(mix(from, to, (boundary - from_row) / (to_row - from_row))))
                    return false;
            }
            a.swap(b);
            count = output;
        }
        if (count >= 3)
            emit(row, std::span<const Vertex>(a.data(), count));
    }
    return true;
}

} // namespace oa::app::renderer_model_shading
