// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/base/game_math.hpp"
#include "oa/sim/model_runtime/instance.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <vector>

namespace oa::app {

using RootPrefixPoint = formats::objects3d::FixedVector3;
using RootPrefixBounds = sim::model_runtime::PieceBox;

/// Fraction bits in the signed plane-rotation coefficients.
inline constexpr unsigned root_prefix_rotation_bits = 30;
/// One in the signed plane-rotation coefficient format.
inline constexpr int32_t root_prefix_rotation_scale = int32_t{1} << root_prefix_rotation_bits;
/// Fixed-coordinate units in one world pixel.
inline constexpr int32_t root_prefix_fixed_scale = int32_t{1} << 16;
/// Largest admitted magnitude before the final root turn, in 16.16 units.
inline constexpr int32_t root_prefix_coordinate_limit = 512 * root_prefix_fixed_scale;
/// Largest admitted magnitude after root placement, in 16.16 units.
inline constexpr int32_t root_prefix_local_limit = 2048 * root_prefix_fixed_scale;
/// Largest number of pieces in one owned prefix packet.
inline constexpr size_t root_prefix_piece_limit = 64;
/// Largest number of source points in one owned prefix packet.
inline constexpr size_t root_prefix_point_limit = 2048;

/// Signed sine and cosine for a final root XZ turn, with 30 fraction bits.
struct RootPrefixRotation {
    int32_t sine{};
    int32_t cosine{root_prefix_rotation_scale};
};

/// Owns one exact zero-XZ prefix after removing its old root placement.
///
/// Points and piece ranges use the source instance's COB-bound piece order.
/// Publishing through a const shared handle keeps an earlier frame's points
/// alive when a transform-cache entry is replaced. The model handle owns the
/// immutable topology; this packet carries no unit or simulation state.
struct RootPrefixGeometry {
    std::shared_ptr<const formats::objects3d::Model> model{};
    std::vector<uint32_t> objects{};
    std::vector<uint32_t> piece_vertices{};
    std::vector<RootPrefixPoint> points{};
    std::vector<RootPrefixBounds> piece_bounds{};

    /// Returns the packet and owned buffer bytes, excluding the shared model.
    /// @return allocated payload bytes, or size_t's maximum on overflow
    [[nodiscard]] size_t storage_bytes() const noexcept {
        size_t bytes = sizeof(*this);
        const auto append = [&](size_t count, size_t width) {
            if (count > (std::numeric_limits<size_t>::max() - bytes) / width)
                return false;
            bytes += count * width;
            return true;
        };
        if (!append(objects.capacity(), sizeof(objects[0])) ||
            !append(piece_vertices.capacity(), sizeof(piece_vertices[0])) ||
            !append(points.capacity(), sizeof(points[0])) ||
            !append(piece_bounds.capacity(), sizeof(piece_bounds[0])))
            return std::numeric_limits<size_t>::max();
        return bytes;
    }
};

/// Places one owned prefix with the declared Q30 root-rotation policy.
struct RootPrefixRecipe {
    std::shared_ptr<const RootPrefixGeometry> geometry{};
    RootPrefixRotation rotation{};
    RootPrefixPoint placement{}; ///< signed 16.16 root object offset plus script translation
};

namespace root_prefix_detail {

/// Keeps a signed coordinate's low 32 bits without signed overflow.
/// @param value the coordinate before wrapping
/// @return the signed coordinate represented by its low 32 bits
[[nodiscard]] inline int32_t low32(int64_t value) noexcept {
    return std::bit_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(value)));
}

/// Rounds a signed Q30 value to nearest integer, with even ties.
/// @param value a product or sum with 30 fraction bits
/// @return the rounded signed integer, before coordinate wrapping
[[nodiscard]] inline int64_t round_rotation(int64_t value) noexcept {
    const bool negative = value < 0;
    const uint64_t bits = std::bit_cast<uint64_t>(value);
    const uint64_t magnitude = negative ? uint64_t{0} - bits : bits;
    constexpr uint64_t remainder_mask = uint64_t{root_prefix_rotation_scale} - 1;
    constexpr uint64_t half = uint64_t{root_prefix_rotation_scale} / 2;
    const auto remainder = magnitude & remainder_mask;
    uint64_t rounded = magnitude >> root_prefix_rotation_bits;
    if (remainder > half || (remainder == half && (rounded & 1U) != 0))
        ++rounded;
    return negative ? -static_cast<int64_t>(rounded) : static_cast<int64_t>(rounded);
}

/// Checks that both coefficients fit the signed Q30 unit interval.
/// @param rotation the coefficients to check
/// @return true when both magnitudes are at most one
[[nodiscard]] inline bool valid_rotation(RootPrefixRotation rotation) noexcept {
    return rotation.sine >= -root_prefix_rotation_scale &&
           rotation.sine <= root_prefix_rotation_scale &&
           rotation.cosine >= -root_prefix_rotation_scale &&
           rotation.cosine <= root_prefix_rotation_scale;
}

/// Checks the admitted magnitude of every raw prefix coordinate.
/// @param point a signed 16.16 point before root placement
/// @return true when each coordinate is within the ordinary-unit domain
[[nodiscard]] inline bool bounded_point(RootPrefixPoint point) noexcept {
    return point.x >= -root_prefix_coordinate_limit && point.x <= root_prefix_coordinate_limit &&
           point.y >= -root_prefix_coordinate_limit && point.y <= root_prefix_coordinate_limit &&
           point.z >= -root_prefix_coordinate_limit && point.z <= root_prefix_coordinate_limit;
}

/// Compares two signed 16.16 vectors component by component.
/// @param first the first vector
/// @param second the second vector
/// @return true when every coordinate matches
[[nodiscard]] inline bool same_point(RootPrefixPoint first, RootPrefixPoint second) noexcept {
    return first.x == second.x && first.y == second.y && first.z == second.z;
}

} // namespace root_prefix_detail

/// Quantizes the game's final-root coefficients to signed Q30 with even ties.
///
/// For admitted prefix points, coefficient quantization contributes less than
/// 1/16 of a fixed-coordinate unit before integer rounding. Together with the
/// native coefficient/product rounding, each final XZ coordinate differs from
/// the native final turn by at most one fixed-coordinate unit. This is a
/// presentation policy; canonical model transforms retain their native path.
///
/// @param angle the signed final root XZ word, 65536 per full turn
/// @return coefficients under nearest-even rounding, or nothing in another mode
[[nodiscard]] inline std::optional<RootPrefixRotation>
prepare_root_prefix_rotation(int16_t angle) noexcept {
    if (std::fegetround() != FE_TONEAREST)
        return std::nullopt;
    const auto prepared = base::game_math::prepare_integer_rotation(angle);
    const RootPrefixRotation result{
        static_cast<int32_t>(std::llrint(prepared.sine * root_prefix_rotation_scale)),
        static_cast<int32_t>(std::llrint(prepared.cosine * root_prefix_rotation_scale))
    };
    return root_prefix_detail::valid_rotation(result) ? std::optional{result} : std::nullopt;
}

/// Rotates an unplaced fixed point with signed products and nearest-even Q30 rounding.
///
/// Each coordinate keeps its low 32 bits after rounding. Y is unchanged.
/// This integer operation is independent of the caller's floating-point mode.
///
/// @param point the unplaced signed 16.16 point
/// @param rotation coefficients prepared by prepare_root_prefix_rotation
/// @return the rotated point, or zero for coefficients outside the Q30 unit interval
[[nodiscard]] inline RootPrefixPoint
rotate_root_prefix_point(RootPrefixPoint point, RootPrefixRotation rotation) noexcept {
    if (!root_prefix_detail::valid_rotation(rotation))
        return {};
    const int64_t x = int64_t{rotation.cosine} * point.x - int64_t{rotation.sine} * point.z;
    const int64_t z = int64_t{rotation.sine} * point.x + int64_t{rotation.cosine} * point.z;
    return {
        root_prefix_detail::low32(root_prefix_detail::round_rotation(x)),
        point.y,
        root_prefix_detail::low32(root_prefix_detail::round_rotation(z))
    };
}

/// Rotates one unplaced point and adds root placement with low 32 bits kept.
/// @param point the unplaced signed 16.16 point
/// @param rotation coefficients prepared by prepare_root_prefix_rotation
/// @param placement the signed 16.16 root object offset plus script translation
/// @return the placed point, or zero for coefficients outside the Q30 unit interval
[[nodiscard]] inline RootPrefixPoint place_root_prefix_point(
    RootPrefixPoint point, RootPrefixRotation rotation, RootPrefixPoint placement
) noexcept {
    if (!root_prefix_detail::valid_rotation(rotation))
        return {};
    point = rotate_root_prefix_point(point, rotation);
    point.x = root_prefix_detail::low32(int64_t{point.x} + placement.x);
    point.y = root_prefix_detail::low32(int64_t{point.y} + placement.y);
    point.z = root_prefix_detail::low32(int64_t{point.z} + placement.z);
    return point;
}

/// Returns the root's wrapped object offset plus its current script translation.
/// @param instance the instance whose root placement is needed
/// @return the signed 16.16 placement, or nothing without the model's ordinary root
[[nodiscard]] inline std::optional<RootPrefixPoint>
root_prefix_placement(const sim::model_runtime::Instance& instance) noexcept {
    const auto& model = instance.model_handle();
    const auto pieces = instance.pieces();
    if (!model || model->objects.empty() || instance.root_piece() >= pieces.size())
        return std::nullopt;
    const auto& root = pieces[instance.root_piece()];
    if (root.object_index != 0 || root.parent != sim::model_runtime::kNoPiece)
        return std::nullopt;
    const auto& offset = model->objects[0].offset_from_parent;
    return RootPrefixPoint{
        root_prefix_detail::low32(-int64_t{offset.x} + root.translation.x),
        root_prefix_detail::low32(int64_t{offset.y} + root.translation.y),
        root_prefix_detail::low32(-int64_t{offset.z} + root.translation.z)
    };
}

/// Resolves one piece's source point through an owned Q30 recipe.
/// @param recipe the owned prefix and final root placement
/// @param piece the source instance's piece index
/// @param vertex the source object's vertex index
/// @return the placed signed 16.16 point, or zero for an invalid range
[[nodiscard]] inline RootPrefixPoint
root_prefix_point(const RootPrefixRecipe& recipe, uint32_t piece, uint32_t vertex) noexcept {
    if (!recipe.geometry || piece >= recipe.geometry->objects.size() ||
        recipe.geometry->piece_vertices.size() != recipe.geometry->objects.size() + 1)
        return {};
    const auto first = recipe.geometry->piece_vertices[piece];
    const auto last = recipe.geometry->piece_vertices[piece + 1];
    if (first > last || last > recipe.geometry->points.size() || vertex >= last - first ||
        !root_prefix_detail::valid_rotation(recipe.rotation))
        return {};
    return place_root_prefix_point(
        recipe.geometry->points[first + vertex], recipe.rotation, recipe.placement
    );
}

/// Bounds one placed piece from its cached box without reading its points.
///
/// The four XZ corners enclose the rounded linear turn. A possible signed
/// wrap or a coordinate beyond the local ordinary-unit domain refuses the box.
///
/// @param recipe the owned prefix and final root placement
/// @param piece the source instance's piece index
/// @return an inclusive signed 16.16 box, or nothing for empty or unsupported input
[[nodiscard]] inline std::optional<RootPrefixBounds>
root_prefix_piece_bounds(const RootPrefixRecipe& recipe, uint32_t piece) noexcept {
    if (!recipe.geometry || piece >= recipe.geometry->objects.size() ||
        recipe.geometry->piece_bounds.size() != recipe.geometry->objects.size() ||
        recipe.geometry->piece_vertices.size() != recipe.geometry->objects.size() + 1 ||
        recipe.geometry->piece_vertices[piece] >= recipe.geometry->piece_vertices[piece + 1] ||
        recipe.geometry->piece_vertices[piece + 1] > recipe.geometry->points.size() ||
        !root_prefix_detail::valid_rotation(recipe.rotation))
        return std::nullopt;
    const auto& raw = recipe.geometry->piece_bounds[piece];
    if (!root_prefix_detail::bounded_point(raw.low) ||
        !root_prefix_detail::bounded_point(raw.high) || raw.low.x > raw.high.x ||
        raw.low.y > raw.high.y || raw.low.z > raw.high.z)
        return std::nullopt;
    int64_t low_x = std::numeric_limits<int64_t>::max(), low_z = low_x;
    int64_t high_x = std::numeric_limits<int64_t>::min(), high_z = high_x;
    for (const auto x : {raw.low.x, raw.high.x})
        for (const auto z : {raw.low.z, raw.high.z}) {
            const auto point = rotate_root_prefix_point({x, 0, z}, recipe.rotation);
            const int64_t placed_x = int64_t{point.x} + recipe.placement.x;
            const int64_t placed_z = int64_t{point.z} + recipe.placement.z;
            low_x = std::min(low_x, placed_x);
            high_x = std::max(high_x, placed_x);
            low_z = std::min(low_z, placed_z);
            high_z = std::max(high_z, placed_z);
        }
    const int64_t low_y = int64_t{raw.low.y} + recipe.placement.y;
    const int64_t high_y = int64_t{raw.high.y} + recipe.placement.y;
    if (low_x < -root_prefix_local_limit || low_y < -root_prefix_local_limit ||
        low_z < -root_prefix_local_limit || high_x > root_prefix_local_limit ||
        high_y > root_prefix_local_limit || high_z > root_prefix_local_limit)
        return std::nullopt;
    return RootPrefixBounds{
        {static_cast<int32_t>(low_x), static_cast<int32_t>(low_y), static_cast<int32_t>(low_z)},
        {static_cast<int32_t>(high_x), static_cast<int32_t>(high_y), static_cast<int32_t>(high_z)}
    };
}

/// Copies a completed zero-XZ prefix into bounded immutable rendering storage.
///
/// The caller proves the source's completed rebuild used a zero combined root
/// XZ turn. Its root placement must match source_placement, and its buffers
/// must remain unchanged for this call. No canonical record or instance is
/// changed. The packet removes source placement with unsigned wrapping once;
/// every retained coordinate must then lie within 512 world pixels of zero.
///
/// @param source the completed exact zero-XZ prefix
/// @param source_placement the old signed 16.16 root object offset plus script translation
/// @param byte_limit maximum packet and owned vector payload bytes
/// @return immutable owned geometry, or null for unsupported input or allocation failure
[[nodiscard]] inline std::shared_ptr<const RootPrefixGeometry> make_root_prefix_geometry(
    const sim::model_runtime::Instance& source, RootPrefixPoint source_placement, size_t byte_limit
) noexcept {
    const auto& model = source.model_handle();
    const auto pieces = source.pieces();
    const auto placement = root_prefix_placement(source);
    if (!model || pieces.empty() || pieces.size() > root_prefix_piece_limit ||
        pieces.size() != model->objects.size() || source.transforms_dirty() ||
        source.materialization_revision() == 0 || !placement ||
        !root_prefix_detail::same_point(*placement, source_placement) ||
        model->objects[0].next_sibling != sim::model_runtime::kNoPiece)
        return {};
    try {
        if (sim::model_runtime::model_hierarchy_error(model) != nullptr)
            return {};
        std::array<uint32_t, root_prefix_piece_limit> piece_for_object{};
        piece_for_object.fill(sim::model_runtime::kNoPiece);
        size_t point_count{};
        for (size_t piece = 0; piece < pieces.size(); ++piece) {
            const auto& state = pieces[piece];
            if (state.object_index >= model->objects.size() ||
                piece_for_object[state.object_index] != sim::model_runtime::kNoPiece)
                return {};
            piece_for_object[state.object_index] = static_cast<uint32_t>(piece);
            const auto vertices = model->objects[state.object_index].vertices.size();
            if (vertices > root_prefix_point_limit - point_count ||
                state.transformed_vertices.size() != vertices || state.transform_marker != 0 ||
                !root_prefix_detail::same_point(state.translation, state.built_translation) ||
                state.rotation.xy != state.built_rotation.xy ||
                state.rotation.xz != state.built_rotation.xz ||
                state.rotation.yz != state.built_rotation.yz)
                return {};
            point_count += vertices;
        }
        if (point_count == 0 || source.root_piece() != piece_for_object[0])
            return {};
        const auto linked = [&](uint32_t object) {
            return object == sim::model_runtime::kNoPiece ? sim::model_runtime::kNoPiece
                                                          : piece_for_object[object];
        };
        for (const auto& state : pieces) {
            const auto& object = model->objects[state.object_index];
            if (state.parent != linked(object.parent) ||
                state.first_child != linked(object.first_child) ||
                state.next_sibling != linked(object.next_sibling))
                return {};
        }
        const size_t required = sizeof(RootPrefixGeometry) +
                                pieces.size() * (sizeof(uint32_t) * 2 + sizeof(RootPrefixBounds)) +
                                sizeof(uint32_t) + point_count * sizeof(RootPrefixPoint);
        if (required > byte_limit)
            return {};
        auto geometry = std::make_shared<RootPrefixGeometry>();
        geometry->model = model;
        geometry->objects.resize(pieces.size());
        geometry->piece_vertices.resize(pieces.size() + 1);
        geometry->points.resize(point_count);
        geometry->piece_bounds.resize(pieces.size());
        if (geometry->storage_bytes() > byte_limit)
            return {};
        uint32_t at{};
        for (size_t piece = 0; piece < pieces.size(); ++piece) {
            geometry->objects[piece] = pieces[piece].object_index;
            geometry->piece_vertices[piece] = at;
            bool nonempty{};
            auto& box = geometry->piece_bounds[piece];
            for (const auto& placed : pieces[piece].transformed_vertices) {
                const RootPrefixPoint point{
                    root_prefix_detail::low32(int64_t{placed.x} - source_placement.x),
                    root_prefix_detail::low32(int64_t{placed.y} - source_placement.y),
                    root_prefix_detail::low32(int64_t{placed.z} - source_placement.z)
                };
                if (!root_prefix_detail::bounded_point(point))
                    return {};
                geometry->points[at++] = point;
                if (!nonempty)
                    box = {point, point};
                else {
                    box.low = {
                        std::min(box.low.x, point.x),
                        std::min(box.low.y, point.y),
                        std::min(box.low.z, point.z)
                    };
                    box.high = {
                        std::max(box.high.x, point.x),
                        std::max(box.high.y, point.y),
                        std::max(box.high.z, point.z)
                    };
                }
                nonempty = true;
            }
        }
        geometry->piece_vertices.back() = at;
        return geometry;
    } catch (const std::bad_alloc&) {
        return {};
    }
}

} // namespace oa::app
