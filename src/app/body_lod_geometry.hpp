// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "oa/formats/objects3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace oa::app::body_lod {

/// Chooses the number of supporting source vertices retained by a piece.
enum class Quality : uint8_t { low, medium };

/// A closed hull's triangle and the source polygon supplying its material.
struct Triangle {
    std::array<uint16_t, 3> vertices{};
    uint32_t primitive{};

    /// Compares source vertex indices and the material polygon.
    /// @param other the other triangle
    /// @return true for identical source indices and material
    [[nodiscard]] bool operator==(const Triangle& other) const = default;
};

/// A piece's source-index triangles, reusable with every presented pose.
struct PieceMesh {
    std::vector<Triangle> triangles{};
    bool reduced{};
    bool valid{true}; ///< false for malformed or oversized source data
};

namespace detail {

using Point = std::array<double, 3>;
inline constexpr std::size_t maximum_source_vertices = std::size_t{1} << 16;
inline constexpr std::size_t maximum_source_triangles = std::size_t{1} << 16;
inline constexpr std::size_t maximum_support_vertices = 8;
inline constexpr double fixed_scale = formats::objects3d::kThreeDoUnitsPerWorldUnit;
inline constexpr double maximum_piece_extent = 8192;
inline constexpr double minimum_aspect = 1.0 / 1024;
inline constexpr double plane_tolerance = 1e-10;

/// Returns a source vertex in model units without applying a pose.
/// @param value source coordinates in signed 16.16 model units
/// @return the same coordinates in model units
[[nodiscard]] inline Point point(const formats::objects3d::FixedVector3& value) noexcept {
    return {value.x / fixed_scale, value.y / fixed_scale, value.z / fixed_scale};
}

/// Subtracts the second vector from the first.
/// @param first the first vector
/// @param second the vector to subtract
/// @return their difference
[[nodiscard]] inline Point subtract(const Point& first, const Point& second) noexcept {
    return {first[0] - second[0], first[1] - second[1], first[2] - second[2]};
}

/// Returns the two vectors' cross product.
/// @param first the first vector
/// @param second the second vector
/// @return their cross product
[[nodiscard]] inline Point cross(const Point& first, const Point& second) noexcept {
    return {
        first[1] * second[2] - first[2] * second[1],
        first[2] * second[0] - first[0] * second[2],
        first[0] * second[1] - first[1] * second[0]
    };
}

/// Returns the two vectors' dot product.
/// @param first the first vector
/// @param second the second vector
/// @return their dot product
[[nodiscard]] inline double dot(const Point& first, const Point& second) noexcept {
    return first[0] * second[0] + first[1] * second[1] + first[2] * second[2];
}

/// Returns a triangle's unnormalised normal.
/// @param first the first corner
/// @param second the second corner
/// @param third the third corner
/// @return the cross product of its two edges
[[nodiscard]] inline Point normal(const Point& first, const Point& second, const Point& third) {
    return cross(subtract(second, first), subtract(third, first));
}

/// Returns the source polygon nearest a hull face's direction and place.
/// @param source the immutable source object
/// @param primitives valid drawable source polygon indices
/// @param direction the hull face's outward unit normal
/// @param center the hull face's centre in model units
/// @param model_center the referenced source geometry's centre
/// @param extent the largest containing dimension in model units
/// @return the source polygon supplying the face's material
[[nodiscard]] inline uint32_t material_polygon(
    const formats::objects3d::Object& source,
    std::span<const uint32_t> primitives,
    const Point& direction,
    const Point& center,
    const Point& model_center,
    double extent
) {
    uint32_t selected = primitives.front();
    double best = -std::numeric_limits<double>::infinity();
    for (const auto index : primitives) {
        const auto& polygon = source.primitives[index];
        if (polygon.vertex_indices.size() < 3 ||
            static_cast<int64_t>(index) == source.selection_primitive)
            continue;
        Point location{};
        for (const auto vertex : polygon.vertex_indices) {
            const auto coordinate = point(source.vertices[vertex]);
            for (std::size_t axis = 0; axis < location.size(); ++axis)
                location[axis] += coordinate[axis] / polygon.vertex_indices.size();
        }
        const auto first = point(source.vertices[polygon.vertex_indices.front()]);
        Point facing{};
        for (std::size_t corner = 1; corner + 1 < polygon.vertex_indices.size(); ++corner) {
            facing = normal(
                first,
                point(source.vertices[polygon.vertex_indices[corner]]),
                point(source.vertices[polygon.vertex_indices[corner + 1]])
            );
            if (dot(facing, facing) > 0)
                break;
        }
        const double magnitude = std::sqrt(dot(facing, facing));
        if (!(magnitude > 0))
            continue;
        if (dot(facing, subtract(location, model_center)) < 0)
            for (auto& axis : facing)
                axis = -axis;
        const auto gap = subtract(location, center);
        constexpr double position_weight = 0.125;
        const double score = dot(direction, facing) / magnitude -
                             position_weight * dot(gap, gap) / (extent * extent);
        if (score > best) {
            best = score;
            selected = index;
        }
    }
    return selected;
}

/// Orders a supporting plane's boundary, excluding collinear interior points.
/// @param source the immutable source object
/// @param support at most eight selected source vertex indices
/// @param mask the selected points lying on this supporting plane
/// @param normal the plane's outward normal
/// @return boundary indices in outward winding order
[[nodiscard]] inline std::vector<uint16_t> boundary(
    const formats::objects3d::Object& source,
    std::span<const uint16_t> support,
    uint16_t mask,
    const Point& normal
) {
    std::size_t dropped = 0;
    for (std::size_t axis = 1; axis < normal.size(); ++axis)
        if (std::abs(normal[axis]) > std::abs(normal[dropped]))
            dropped = axis;
    const std::size_t across = (dropped + 1) % normal.size();
    const std::size_t down = (dropped + 2) % normal.size();
    std::vector<uint16_t> vertices;
    for (std::size_t index = 0; index < support.size(); ++index)
        if ((mask & (uint16_t{1} << index)) != 0)
            vertices.push_back(support[index]);
    const auto coordinate = [&](uint16_t vertex) { return point(source.vertices[vertex]); };
    std::sort(vertices.begin(), vertices.end(), [&](uint16_t first, uint16_t second) {
        const auto a = coordinate(first), b = coordinate(second);
        return a[across] != b[across] ? a[across] < b[across]
                                      : (a[down] != b[down] ? a[down] < b[down] : first < second);
    });
    const auto turn = [&](uint16_t first, uint16_t second, uint16_t third) {
        const auto a = coordinate(first), b = coordinate(second), c = coordinate(third);
        return (b[across] - a[across]) * (c[down] - a[down]) -
               (b[down] - a[down]) * (c[across] - a[across]);
    };
    std::vector<uint16_t> hull;
    for (const auto vertex : vertices) {
        while (hull.size() >= 2 && turn(hull[hull.size() - 2], hull.back(), vertex) <= 0)
            hull.pop_back();
        hull.push_back(vertex);
    }
    const auto lower = hull.size();
    for (auto at = vertices.rbegin() + 1; at != vertices.rend(); ++at) {
        while (hull.size() > lower && turn(hull[hull.size() - 2], hull.back(), *at) <= 0)
            hull.pop_back();
        hull.push_back(*at);
    }
    if (!hull.empty())
        hull.pop_back();
    if (hull.size() >= 3 &&
        dot(normal, detail::normal(coordinate(hull[0]), coordinate(hull[1]), coordinate(hull[2]))) <
            0)
        std::reverse(hull.begin(), hull.end());
    return hull;
}

} // namespace detail

/// Builds a bounded, closed coarse hull using only drawable source vertices.
///
/// Referencing the original vertices keeps every accepted triangle attached to
/// the piece's ordinary posed geometry. Thin, planar and unreduced pieces keep
/// their original triangle fans. Selection and unreferenced geometry add none.
/// Materials name nearby original polygons; the hull is a visual approximation.
///
/// @param source one immutable model object
/// @param drawable_primitives source polygon indices accepted by the renderer
/// @param quality four or eight supporting directions
/// @return reduced closed hull or original fans; valid is false for bad input
[[nodiscard]] inline PieceMesh make_piece(
    const formats::objects3d::Object& source,
    std::span<const uint32_t> drawable_primitives,
    Quality quality
) {
    using namespace detail;
    PieceMesh original;
    if (source.vertices.size() > maximum_source_vertices ||
        drawable_primitives.size() > maximum_source_triangles) {
        original.valid = false;
        return original;
    }
    std::vector<uint16_t> referenced;
    std::vector<bool> used(source.vertices.size());
    std::vector<uint32_t> primitives;
    for (const auto index : drawable_primitives) {
        if (index >= source.primitives.size())
            return {{}, false, false};
        if (static_cast<int64_t>(index) == source.selection_primitive)
            continue;
        const auto& indices = source.primitives[index].vertex_indices;
        if (indices.size() < 3)
            continue;
        if (indices.size() - 2 > maximum_source_triangles - original.triangles.size())
            return {{}, false, false};
        for (const auto vertex : indices)
            if (vertex >= source.vertices.size())
                return {{}, false, false};
        bool material = false;
        for (std::size_t fan = 1; fan + 1 < indices.size(); ++fan) {
            const std::array vertices{indices[0], indices[fan], indices[fan + 1]};
            original.triangles.push_back({vertices, index});
            const auto area = normal(
                point(source.vertices[vertices[0]]),
                point(source.vertices[vertices[1]]),
                point(source.vertices[vertices[2]])
            );
            if (!(dot(area, area) > 0))
                continue;
            material = true;
            for (const auto vertex : vertices)
                if (!used[vertex]) {
                    used[vertex] = true;
                    referenced.push_back(vertex);
                }
        }
        if (material)
            primitives.push_back(index);
    }
    if (referenced.size() < 4)
        return original;
    std::sort(referenced.begin(), referenced.end());
    const auto first = point(source.vertices[referenced.front()]);
    Point low = first, high = first, center{};
    for (const auto vertex : referenced) {
        const auto value = point(source.vertices[vertex]);
        for (std::size_t axis = 0; axis < value.size(); ++axis) {
            low[axis] = std::min(low[axis], value[axis]);
            high[axis] = std::max(high[axis], value[axis]);
            center[axis] += value[axis] / referenced.size();
        }
    }
    const auto dimensions = subtract(high, low);
    const double extent = *std::max_element(dimensions.begin(), dimensions.end());
    if (!(extent > 0) || extent > maximum_piece_extent ||
        *std::min_element(dimensions.begin(), dimensions.end()) <= extent * minimum_aspect)
        return original;
    constexpr std::array<Point, maximum_support_vertices> directions{
        Point{1, 1, 1},
        Point{1, -1, -1},
        Point{-1, 1, -1},
        Point{-1, -1, 1},
        Point{-1, -1, -1},
        Point{-1, 1, 1},
        Point{1, -1, 1},
        Point{1, 1, -1}
    };
    constexpr std::size_t low_directions = 4;
    const auto count = quality == Quality::low ? low_directions : directions.size();
    std::vector<uint16_t> support;
    for (std::size_t direction = 0; direction < count; ++direction) {
        auto selected = referenced.front();
        double best = -std::numeric_limits<double>::infinity();
        for (const auto vertex : referenced) {
            const double distance = dot(point(source.vertices[vertex]), directions[direction]);
            if (distance > best) {
                best = distance;
                selected = vertex;
            }
        }
        const auto value = point(source.vertices[selected]);
        if (std::none_of(support.begin(), support.end(), [&](uint16_t vertex) {
                return point(source.vertices[vertex]) == value;
            }))
            support.push_back(selected);
    }
    if (support.size() < 4)
        return original;
    PieceMesh reduced;
    std::vector<uint16_t> planes;
    const double tolerance = plane_tolerance * extent;
    for (std::size_t a = 0; a + 2 < support.size(); ++a)
        for (std::size_t b = a + 1; b + 1 < support.size(); ++b)
            for (std::size_t c = b + 1; c < support.size(); ++c) {
                const auto origin = point(source.vertices[support[a]]);
                auto facing = normal(
                    origin, point(source.vertices[support[b]]), point(source.vertices[support[c]])
                );
                const double length = std::sqrt(dot(facing, facing));
                if (!(length > 0))
                    continue;
                for (auto& value : facing)
                    value /= length;
                bool positive = false, negative = false;
                uint16_t mask{};
                for (std::size_t vertex = 0; vertex < support.size(); ++vertex) {
                    const double distance =
                        dot(facing, subtract(point(source.vertices[support[vertex]]), origin));
                    positive |= distance > tolerance;
                    negative |= distance < -tolerance;
                    if (std::abs(distance) <= tolerance)
                        mask |= uint16_t{1} << vertex;
                }
                if (positive == negative ||
                    std::find(planes.begin(), planes.end(), mask) != planes.end())
                    continue;
                if (positive)
                    for (auto& value : facing)
                        value = -value;
                planes.push_back(mask);
                const auto face = boundary(source, support, mask, facing);
                if (face.size() < 3)
                    return original;
                Point location{};
                for (const auto vertex : face) {
                    const auto value = point(source.vertices[vertex]);
                    for (std::size_t axis = 0; axis < location.size(); ++axis)
                        location[axis] += value[axis] / face.size();
                }
                const auto material =
                    material_polygon(source, primitives, facing, location, center, extent);
                for (std::size_t fan = 1; fan + 1 < face.size(); ++fan)
                    reduced.triangles.push_back({{face[0], face[fan], face[fan + 1]}, material});
            }
    if (reduced.triangles.size() < 4 || reduced.triangles.size() >= original.triangles.size())
        return original;
    // Every undirected hull edge must have two opposite directed uses.
    for (const auto& triangle : reduced.triangles)
        for (std::size_t edge = 0; edge < triangle.vertices.size(); ++edge) {
            const auto from = triangle.vertices[edge];
            const auto to = triangle.vertices[(edge + 1) % triangle.vertices.size()];
            std::size_t forward{}, reverse{};
            for (const auto& other : reduced.triangles)
                for (std::size_t at = 0; at < other.vertices.size(); ++at) {
                    const auto first_vertex = other.vertices[at];
                    const auto second_vertex = other.vertices[(at + 1) % other.vertices.size()];
                    forward += first_vertex == from && second_vertex == to;
                    reverse += first_vertex == to && second_vertex == from;
                }
            if (forward != 1 || reverse != 1)
                return original;
        }
    reduced.reduced = true;
    return reduced;
}

} // namespace oa::app::body_lod
