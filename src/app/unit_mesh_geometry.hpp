// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "oa/formats/objects3d.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace oa::app::unit_detail_transition {

/// The containing dimensions of a type's drawable rest-pose mesh, in map units.
struct Geometry {
    std::array<double, 3> dimensions{};
};

/// Derives dimensions from drawable mesh polygons rather than collision bounds.
///
/// Piece offsets follow the rest-pose hierarchy and signed 32-bit wrapping.
/// Selection polygons, nonvisible source primitives and unreferenced vertices
/// supply no size. Authored geometry is a stable rest-pose proxy, rather than
/// a measurement of today's animation or visible raster pixels.
///
/// @param model loaded immutable 3DO geometry
/// @return containing x, height and z extents; zero for an invalid or empty mesh
[[nodiscard]] inline Geometry derive_geometry(const formats::objects3d::Model& model) {
    constexpr auto no_object = formats::objects3d::kNoObject;

    struct Placement {
        std::array<int32_t, 3> offset{};
        bool rooted{};
    };

    const auto add = [](int32_t first, int32_t second) {
        return std::bit_cast<int32_t>(
            std::bit_cast<uint32_t>(first) + std::bit_cast<uint32_t>(second)
        );
    };
    const auto negate = [](int32_t value) {
        return std::bit_cast<int32_t>(uint32_t{} - std::bit_cast<uint32_t>(value));
    };
    std::vector<Placement> placements(model.objects.size());
    std::array least{
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::max()
    };
    std::array greatest{
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::min()
    };
    bool bounded = false;
    for (std::size_t index = 0; index < model.objects.size(); ++index) {
        const auto& object = model.objects[index];
        auto& placement = placements[index];
        const std::array local{
            object.offset_from_parent.x, object.offset_from_parent.y, object.offset_from_parent.z
        };
        if (index == 0) {
            if (object.parent != no_object)
                return {};
            placement = {local, true};
        } else if (object.parent != no_object) {
            if (object.parent >= index)
                return {};
            placement.rooted = placements[object.parent].rooted;
            if (placement.rooted)
                for (std::size_t axis = 0; axis < local.size(); ++axis)
                    placement.offset[axis] =
                        add(placements[object.parent].offset[axis], local[axis]);
        }
        const auto point = [&](uint16_t vertex) {
            const auto& source = object.vertices[vertex];
            std::array result{source.x, source.y, source.z};
            // Root-level siblings remain untransformed in the model runtime.
            if (placement.rooted)
                for (std::size_t axis = 0; axis < result.size(); ++axis)
                    result[axis] = add(result[axis], placement.offset[axis]);
            result[0] = negate(result[0]);
            result[2] = negate(result[2]);
            return result;
        };
        for (std::size_t face = 0; face < object.primitives.size(); ++face) {
            const auto& polygon = object.primitives[face];
            if (static_cast<int64_t>(face) == object.selection_primitive ||
                !polygon.source_visible() || polygon.vertex_indices.size() < 3)
                continue;
            if (std::any_of(
                    polygon.vertex_indices.begin(),
                    polygon.vertex_indices.end(),
                    [&](uint16_t vertex) { return vertex >= object.vertices.size(); }
                ))
                return {};
            const auto first = point(polygon.vertex_indices.front());
            bool area = false;
            for (std::size_t corner = 1; corner + 1 < polygon.vertex_indices.size(); ++corner) {
                const auto second = point(polygon.vertex_indices[corner]);
                const auto third = point(polygon.vertex_indices[corner + 1]);
                std::array<double, 3> across{}, down{};
                for (std::size_t axis = 0; axis < across.size(); ++axis) {
                    across[axis] = static_cast<double>(second[axis]) - first[axis];
                    down[axis] = static_cast<double>(third[axis]) - first[axis];
                }
                area |= across[1] * down[2] != across[2] * down[1] ||
                        across[2] * down[0] != across[0] * down[2] ||
                        across[0] * down[1] != across[1] * down[0];
            }
            if (!area)
                continue;
            for (const auto vertex : polygon.vertex_indices) {
                const auto coordinates = point(vertex);
                for (std::size_t axis = 0; axis < coordinates.size(); ++axis) {
                    least[axis] = std::min(least[axis], coordinates[axis]);
                    greatest[axis] = std::max(greatest[axis], coordinates[axis]);
                }
                bounded = true;
            }
        }
    }
    Geometry result;
    if (bounded) {
        constexpr double fixed_scale = 65536.0;
        for (std::size_t axis = 0; axis < result.dimensions.size(); ++axis)
            result.dimensions[axis] =
                (static_cast<int64_t>(greatest[axis]) - least[axis]) / fixed_scale;
    }
    return result;
}

/// Retains mesh dimensions by loaded type without keeping old model assets alive.
class GeometryCache {
  public:

    /// Supplies dimensions once per immutable model, in the type array's order.
    /// @param types loaded types, each with a shared model handle
    /// @return cached dimensions; valid until this cache is prepared again
    template <class Types>
    [[nodiscard]] std::span<const Geometry> prepare(const Types& types) {
        entries_.resize(types.size());
        geometry_.resize(types.size());
        for (std::size_t index = 0; index < types.size(); ++index) {
            const auto& model = types[index].model;
            auto& entry = entries_[index];
            if (entry.checked && entry.model == model.get() && !entry.owner.owner_before(model) &&
                !model.owner_before(entry.owner))
                continue;
            geometry_[index] = model ? derive_geometry(*model) : Geometry{};
            entry.owner = model;
            entry.model = model.get();
            entry.checked = true;
        }
        return geometry_;
    }

  private:

    struct Entry {
        std::weak_ptr<const formats::objects3d::Model> owner{};
        const formats::objects3d::Model* model{};
        bool checked{};
    };

    std::vector<Entry> entries_{};
    std::vector<Geometry> geometry_{};
};

} // namespace oa::app::unit_detail_transition
