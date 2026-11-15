// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace oa::app::full::polygon_shapes {

/// Counts the words of an exact polygon-shape key without exceeding a limit.
///
/// Polygon supplies an unsigned corner_count. The count word precedes one
/// corner-count word per polygon and three coordinate words per corner.
/// @param polygons the ordered polygons
/// @param limit the greatest permitted word count
/// @return the complete word count, or none when it cannot fit
template <class Polygon>
[[nodiscard]] constexpr std::optional<size_t>
required_words(std::span<const Polygon> polygons, size_t limit) noexcept {
    static_assert(std::is_unsigned_v<decltype(Polygon::corner_count)>);
    if (limit == 0 || polygons.size() > limit - 1)
        return {};
    size_t required = 1 + polygons.size();
    for (const auto& polygon : polygons) {
        if (polygon.corner_count > (limit - required) / 3)
            return {};
        required += 3 * static_cast<size_t>(polygon.corner_count);
    }
    return required;
}

/// Writes every exact normalized shape word into reusable, independently owned storage.
///
/// Corner supplies x, y and depth; Polygon supplies first_corner and corner_count.
/// Their ranges must be valid as for the ordinary polygon walk. Storage is sized
/// once before writing; allocation failures propagate without a retry.
/// @param corners the placed corners
/// @param polygons the ordered polygons
/// @param shape the key scratch, replaced completely
/// @param start the digest's initial value
/// @param multiplier the multiplier applied after each value
/// @param fold_shift the final nonempty digest's upper-bit fold, below 64
/// @return the digest of the written key
template <class Corner, class Polygon>
[[nodiscard]] uint64_t write(
    std::span<const Corner> corners,
    std::span<const Polygon> polygons,
    std::vector<int64_t>& shape,
    uint64_t start,
    uint64_t multiplier,
    unsigned fold_shift
) {
    const auto words = required_words(polygons, shape.max_size());
    if (!words)
        throw std::length_error("The polygon shape exceeds its word limit");
    shape.resize(*words);
    size_t at = 0;
    uint64_t digest = start;
    const auto fold = [&](int64_t value) {
        digest = (digest ^ static_cast<uint64_t>(value)) * multiplier;
        shape[at++] = value;
    };
    fold(static_cast<int64_t>(polygons.size()));
    if (polygons.empty())
        return digest;
    const auto& origin = corners[polygons.front().first_corner];
    for (const auto& polygon : polygons) {
        fold(polygon.corner_count);
        for (const auto& corner : corners.subspan(polygon.first_corner, polygon.corner_count)) {
            fold(int64_t{corner.x} - origin.x);
            fold(int64_t{corner.y} - origin.y);
            fold(corner.depth);
        }
    }
    return digest ^ (digest >> fold_shift);
}

} // namespace oa::app::full::polygon_shapes
