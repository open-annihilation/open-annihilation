// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "model_shadow_metadata.hpp"
#include "oa/base/float_precision.hpp"
#include "oa/base/game_math.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <vector>

namespace oa::app::preplanning_visibility {

using FixedBounds = oa::sim::model_runtime::PieceBox;
using Rotation = oa::sim::model_runtime::RotationWords;

/// One immutable object's reset vertices, before any pose or ancestor transform.
struct ObjectBounds {
    FixedBounds box{};
    bool nonempty{};
    uint64_t l1_radius{}; ///< source maximum L1 length, in fixed-coordinate LSBs
    bool radius_ready{};
};

/// Raw per-object bounds. These are not a static bound of an animated model.
struct Geometry {
    std::shared_ptr<const oa::formats::objects3d::Model> model;
    std::vector<ObjectBounds> objects;
};

/// Reusable topology validation storage; no transformed vertices are retained.
struct Scratch {
    std::vector<uint32_t> piece_for_object;
    std::vector<uint64_t> far_radius_path; ///< reused integer ancestor contributions
    /// Reused only by completed-geometry validation, never by raw-pose bounds.
    std::vector<uint8_t> completed_topology_seen;
    std::vector<std::array<uint32_t, 2>> completed_pending;
};

namespace detail {

/// Matches the native reset/offset negation, including INT32_MIN.
/// @param value the signed 16.16 model coordinate
/// @return the negated coordinate with low 32 bits kept; INT32_MIN remains unchanged
inline int32_t negate(int32_t value) noexcept {
    return std::bit_cast<int32_t>(0U - std::bit_cast<uint32_t>(value));
}

/// Returns a fixed coordinate's magnitude without signed negation overflow.
/// @param value a signed 16.16 model coordinate
/// @return its nonnegative magnitude, including 2147483648 for INT32_MIN
inline uint64_t magnitude(int32_t value) noexcept {
    return value < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(value))
                     : static_cast<uint64_t>(value);
}

/// Grows an inclusive signed fixed-point box.
/// @param[in,out] into the enclosing box, in 16.16 model coordinates
/// @param part the inclusive box to include, in the same coordinates
inline void include(FixedBounds& into, const FixedBounds& part) noexcept {
    into.low.x = std::min(into.low.x, part.low.x);
    into.low.y = std::min(into.low.y, part.low.y);
    into.low.z = std::min(into.low.z, part.low.z);
    into.high.x = std::max(into.high.x, part.high.x);
    into.high.y = std::max(into.high.y, part.high.y);
    into.high.z = std::max(into.high.z, part.high.z);
}

/// Applies one native rounded plane rotation, refusing a wrapped interval.
/// @param[in,out] low_a the first axis's inclusive lower 16.16 coordinate
/// @param[in,out] high_a the first axis's inclusive upper 16.16 coordinate, at least low_a
/// @param[in,out] low_b the second axis's inclusive lower 16.16 coordinate
/// @param[in,out] high_b the second axis's inclusive upper 16.16 coordinate, at least low_b
/// @param angle the signed angle word, 65536 per turn
/// @return true with enclosing rotated intervals; false leaves all coordinates unchanged
inline bool rotate_pair(
    int32_t& low_a, int32_t& high_a, int32_t& low_b, int32_t& high_b, int16_t angle
) noexcept {
    if (angle == 0 || (low_a == 0 && high_a == 0 && low_b == 0 && high_b == 0))
        return true;
    double least_a = std::numeric_limits<double>::infinity();
    double least_b = least_a, greatest_a = -least_a, greatest_b = -least_a;
    for (const int32_t a : {low_a, high_a})
        for (const int32_t b : {low_b, high_b}) {
            const auto point = oa::base::game_math::rotate_pair(a, b, angle);
            if (!std::isfinite(point.first) || !std::isfinite(point.second))
                return false;
            least_a = std::min(least_a, point.first);
            greatest_a = std::max(greatest_a, point.first);
            least_b = std::min(least_b, point.second);
            greatest_b = std::max(greatest_b, point.second);
        }
    constexpr double least = std::numeric_limits<int32_t>::min();
    constexpr double greatest = std::numeric_limits<int32_t>::max();
    if (least_a < least || least_b < least || greatest_a > greatest || greatest_b > greatest)
        return false;
    // Native rotation uses llrint before taking low32. Monotone rounding
    // encloses every interior pair; the range check makes low32 unnecessary.
    low_a = static_cast<int32_t>(std::llrint(least_a));
    high_a = static_cast<int32_t>(std::llrint(greatest_a));
    low_b = static_cast<int32_t>(std::llrint(least_b));
    high_b = static_cast<int32_t>(std::llrint(greatest_b));
    return true;
}

/// Applies the native XY, YZ, XZ pair order, reboxing after every rounded pair.
/// @param[in,out] box the inclusive 16.16 model box; earlier planes may change it on failure
/// @param words the signed rotation words, 65536 per turn on each plane
/// @return true for a bounded rotation; false when any plane is nonfinite or may wrap
inline bool rotate(FixedBounds& box, Rotation words) noexcept {
    return rotate_pair(box.low.x, box.high.x, box.low.y, box.high.y, words.xy) &&
           rotate_pair(box.low.y, box.high.y, box.low.z, box.high.z, words.yz) &&
           rotate_pair(box.low.x, box.high.x, box.low.z, box.high.z, words.xz);
}

/// Adds one offset/translation exactly where no native low32 wrap can occur.
/// @param[in,out] low the inclusive lower 16.16 model coordinate
/// @param[in,out] high the inclusive upper 16.16 model coordinate, at least low
/// @param offset the parent offset on this axis, in 16.16 model coordinates
/// @param shift the local translation on this axis, in 16.16 model coordinates
/// @return true with translated endpoints; false leaves both endpoints unchanged
inline bool translate(int32_t& low, int32_t& high, int32_t offset, int32_t shift) noexcept {
    const int64_t first = int64_t{low} + offset + shift;
    const int64_t last = int64_t{high} + offset + shift;
    if (first < std::numeric_limits<int32_t>::min() || last > std::numeric_limits<int32_t>::max())
        return false;
    low = static_cast<int32_t>(first);
    high = static_cast<int32_t>(last);
    return true;
}

/// Matches the native signed angle-word addition without relying on narrowing.
/// @param a the first signed angle word, 65536 per turn
/// @param b the second signed angle word, 65536 per turn
/// @return the signed sum with low 16 bits kept
inline int16_t add_angle(int16_t a, int16_t b) noexcept {
    return std::bit_cast<int16_t>(
        static_cast<uint16_t>(std::bit_cast<uint16_t>(a) + std::bit_cast<uint16_t>(b))
    );
}

} // namespace detail

/// Scans immutable raw vertices once, applying the exact native reset signs.
/// Allocation or invalid model hierarchy leaves an unusable geometry record.
/// @param model the immutable model and hierarchy, with raw vertices in 16.16 coordinates
/// @return per-object reset bounds; a record without a model on hierarchy or allocation failure
[[nodiscard]] inline Geometry
prepare_geometry(std::shared_ptr<const oa::formats::objects3d::Model> model) {
    try {
        if (oa::sim::model_runtime::model_hierarchy_error(model) != nullptr)
            return {};
        Geometry result;
        result.model = std::move(model);
        result.objects.resize(result.model->objects.size());
        for (std::size_t index = 0; index < result.objects.size(); ++index) {
            auto& target = result.objects[index];
            for (const auto& vertex : result.model->objects[index].vertices) {
                const oa::formats::objects3d::FixedVector3 reset{
                    detail::negate(vertex.x), vertex.y, detail::negate(vertex.z)
                };
                target.l1_radius = std::max(
                    target.l1_radius,
                    detail::magnitude(reset.x) + detail::magnitude(reset.y) +
                        detail::magnitude(reset.z)
                );
                const FixedBounds point{reset, reset};
                if (target.nonempty)
                    detail::include(target.box, point);
                else {
                    target.box = point;
                    target.nonempty = true;
                }
            }
            target.radius_ready = true;
        }
        return result;
    } catch (const std::bad_alloc&) {
        return {};
    }
}

namespace detail {

/// Transforms raw boxes after this call has validated the exact piece mapping.
/// @param geometry the immutable bounds belonging to instance's shared model
/// @param instance the unchanged raw pose whose topology was just validated
/// @param root the exact effective root rotation words
/// @return the ordinary exact interval bound, or no value on arithmetic refusal
[[nodiscard]] inline std::optional<FixedBounds> bound_with_validated_topology(
    const Geometry& geometry, const oa::sim::model_runtime::Instance& instance, Rotation root
) {
    namespace runtime = oa::sim::model_runtime;
    const auto pieces = instance.pieces();
    std::optional<FixedBounds> result;
    for (std::size_t index = 0; index < pieces.size(); ++index) {
        const auto& visible = pieces[index];
        const auto& raw = geometry.objects[visible.object_index];
        if ((visible.flags & static_cast<uint16_t>(runtime::PieceFlag::visible)) == 0 ||
            !raw.nonempty)
            continue;
        auto box = raw.box;
        auto top = static_cast<uint32_t>(index);
        for (std::size_t depth = 0; pieces[top].parent != runtime::kNoPiece; ++depth) {
            if (depth >= pieces.size() || pieces[top].parent >= pieces.size())
                return std::nullopt;
            top = pieces[top].parent;
        }
        // Native rebuild deliberately leaves root-level sibling trees reset.
        if (top == instance.root_piece())
            for (auto current = static_cast<uint32_t>(index); current != runtime::kNoPiece;
                 current = pieces[current].parent) {
                const auto& piece = pieces[current];
                const auto& object = geometry.model->objects[piece.object_index];
                auto turn = piece.rotation;
                if (piece.parent == runtime::kNoPiece) {
                    turn.xy = detail::add_angle(turn.xy, root.xy);
                    turn.xz = detail::add_angle(turn.xz, root.xz);
                    turn.yz = detail::add_angle(turn.yz, root.yz);
                }
                if (!detail::rotate(box, turn) ||
                    !detail::translate(
                        box.low.x,
                        box.high.x,
                        detail::negate(object.offset_from_parent.x),
                        piece.translation.x
                    ) ||
                    !detail::translate(
                        box.low.y, box.high.y, object.offset_from_parent.y, piece.translation.y
                    ) ||
                    !detail::translate(
                        box.low.z,
                        box.high.z,
                        detail::negate(object.offset_from_parent.z),
                        piece.translation.z
                    ))
                    return std::nullopt;
            }
        if (result)
            detail::include(*result, box);
        else
            result = box;
    }
    return result;
}

} // namespace detail

/// Encloses the exact resolved local pose without reading transformed buffers.
///
/// The pose must already have the current frame's local translations,
/// rotations and visibility flags. In particular, endpoint boxes are not
/// an interpolation bound: use the renderer's existing pose blending first.
/// Root is the target ModelState's effective thresholded shift. Neither
/// canonical nor presentation state is changed. Unknown, empty, invalid,
/// differently owned or wrapping input returns no bound.
///
/// @param geometry the immutable reset bounds prepared from the instance's shared model
/// @param instance the resolved current-frame local pose and visible-piece flags
/// @param root the effective thresholded root rotation words, 65536 per turn
/// @param[in,out] scratch reusable piece-index storage; may change even when no bound is returned
/// @return an inclusive 16.16 model-space bound, or no value when containment is unproved
[[nodiscard]] inline std::optional<FixedBounds> bound_instance(
    const Geometry& geometry,
    const oa::sim::model_runtime::Instance& instance,
    Rotation root,
    Scratch& scratch
) {
    namespace runtime = oa::sim::model_runtime;
    const auto& handle = instance.model_handle();
    if (!geometry.model || !handle || geometry.model.get() != handle.get() ||
        geometry.model.owner_before(handle) || handle.owner_before(geometry.model) ||
        geometry.objects.size() != geometry.model->objects.size() ||
        !oa::base::float_precision::rounds_to_double() || std::fegetround() != FE_TONEAREST ||
        !oa::base::float_precision::float_control_matches(
            oa::base::float_precision::program_float_control().saved
        ))
        return std::nullopt;
    const auto pieces = instance.pieces();
    if (pieces.size() != geometry.objects.size() || pieces.empty())
        return std::nullopt;
    try {
        scratch.piece_for_object.assign(pieces.size(), runtime::kNoPiece);
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < pieces.size(); ++index) {
        const auto object = pieces[index].object_index;
        if (object >= pieces.size() || scratch.piece_for_object[object] != runtime::kNoPiece)
            return std::nullopt;
        scratch.piece_for_object[object] = static_cast<uint32_t>(index);
    }
    const auto piece_link = [&](uint32_t object) {
        return object == runtime::kNoPiece || object >= pieces.size()
                   ? runtime::kNoPiece
                   : scratch.piece_for_object[object];
    };
    if (instance.root_piece() != scratch.piece_for_object[0])
        return std::nullopt;
    for (const auto& piece : pieces) {
        const auto& object = geometry.model->objects[piece.object_index];
        if ((object.parent != runtime::kNoPiece && object.parent >= piece.object_index) ||
            (object.parent != runtime::kNoPiece && object.parent >= pieces.size()) ||
            (object.first_child != runtime::kNoPiece && object.first_child >= pieces.size()) ||
            (object.next_sibling != runtime::kNoPiece && object.next_sibling >= pieces.size()) ||
            piece.parent != piece_link(object.parent) ||
            piece.first_child != piece_link(object.first_child) ||
            piece.next_sibling != piece_link(object.next_sibling))
            return std::nullopt;
    }
    return detail::bound_with_validated_topology(geometry, instance, root);
}

/// Encloses far geometry with integer path radii, without trigonometric work.
///
/// A source L1 radius bounds its Euclidean radius. Each plane uses coefficients
/// rounded to 64-bit significands, four products and two sums rounded to 53 bits,
/// then nearest integer coordinates. Below 2^31, coefficient norm error is less
/// than 2^-32 coordinate LSBs and product/sum norm error less than 2^-18.
/// Integer rounding adds at most sqrt(1/2), so two LSBs per plane suffice.
/// Six per ancestor cover all three planes, including zero turns.
/// Separate offset/translation L1 lengths safely cover the native offset signs.
/// Every path contribution is nonnegative; the final strict radius cap also
/// proves every earlier plane and translation stays inside signed coordinates.
/// Root sibling trees and unsupported domains retain the exact ordinary proof.
///
/// @param geometry immutable prepared object bounds and source radii
/// @param instance current raw local poses and visible-piece flags
/// @param[in,out] scratch reusable bounded topology and integer path storage
/// @return a rotation-independent 16.16 containing cube, or no proved bound
[[nodiscard]] inline std::optional<FixedBounds> bound_far_instance(
    const Geometry& geometry, const oa::sim::model_runtime::Instance& instance, Scratch& scratch
) {
    namespace runtime = oa::sim::model_runtime;
    constexpr size_t maximum_far_pieces = 256;
    const auto& handle = instance.model_handle();
    if (!geometry.model || !handle || geometry.model.get() != handle.get() ||
        geometry.model.owner_before(handle) || handle.owner_before(geometry.model) ||
        geometry.objects.size() != geometry.model->objects.size() ||
        !oa::base::float_precision::rounds_to_double() || std::fegetround() != FE_TONEAREST ||
        !oa::base::float_precision::float_control_matches(
            oa::base::float_precision::program_float_control().saved
        ))
        return std::nullopt;
    const auto pieces = instance.pieces();
    if (pieces.size() != geometry.objects.size() || pieces.empty() ||
        pieces.size() > maximum_far_pieces)
        return std::nullopt;
    try {
        scratch.piece_for_object.assign(pieces.size(), runtime::kNoPiece);
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < pieces.size(); ++index) {
        const auto object = pieces[index].object_index;
        if (object >= pieces.size() || scratch.piece_for_object[object] != runtime::kNoPiece)
            return std::nullopt;
        scratch.piece_for_object[object] = static_cast<uint32_t>(index);
    }
    const auto piece_link = [&](uint32_t object) {
        return object == runtime::kNoPiece || object >= pieces.size()
                   ? runtime::kNoPiece
                   : scratch.piece_for_object[object];
    };
    if (instance.root_piece() != scratch.piece_for_object[0])
        return std::nullopt;
    constexpr uint64_t coordinate_limit =
        static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    constexpr uint64_t ancestor_rounding_pad = 6;
    if (geometry.model->objects[0].next_sibling != runtime::kNoPiece)
        return std::nullopt;
    try {
        scratch.far_radius_path.resize(pieces.size());
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
    uint64_t radius{};
    bool nonempty{};
    for (size_t object_index = 0; object_index < pieces.size(); ++object_index) {
        const auto& object = geometry.model->objects[object_index];
        const auto& piece = pieces[scratch.piece_for_object[object_index]];
        const auto& raw = geometry.objects[object_index];
        if ((object.parent != runtime::kNoPiece && object.parent >= object_index) ||
            (object.parent != runtime::kNoPiece && object.parent >= pieces.size()) ||
            (object.first_child != runtime::kNoPiece && object.first_child >= pieces.size()) ||
            (object.next_sibling != runtime::kNoPiece && object.next_sibling >= pieces.size()) ||
            piece.parent != piece_link(object.parent) ||
            piece.first_child != piece_link(object.first_child) ||
            piece.next_sibling != piece_link(object.next_sibling))
            return std::nullopt;
        if (!raw.radius_ready || (object_index != 0 && object.parent == runtime::kNoPiece))
            return std::nullopt;
        const uint64_t parent =
            object.parent == runtime::kNoPiece ? 0 : scratch.far_radius_path[object.parent];
        const uint64_t translation = detail::magnitude(object.offset_from_parent.x) +
                                     detail::magnitude(object.offset_from_parent.y) +
                                     detail::magnitude(object.offset_from_parent.z) +
                                     detail::magnitude(piece.translation.x) +
                                     detail::magnitude(piece.translation.y) +
                                     detail::magnitude(piece.translation.z);
        const uint64_t added = translation + ancestor_rounding_pad;
        if (added >= coordinate_limit || parent >= coordinate_limit - added)
            return std::nullopt;
        const uint64_t path = parent + added;
        scratch.far_radius_path[object_index] = path;
        if ((piece.flags & static_cast<uint16_t>(runtime::PieceFlag::visible)) == 0 ||
            !raw.nonempty)
            continue;
        if (raw.l1_radius >= coordinate_limit - path)
            return std::nullopt;
        radius = std::max(radius, raw.l1_radius + path);
        nonempty = true;
    }
    if (!nonempty)
        return std::nullopt;
    const auto r = static_cast<int32_t>(radius);
    return FixedBounds{{-r, -r, -r}, {r, r, r}};
}

/// Bounds completed native geometry without repeating its hierarchy rotations.
///
/// The caller must have completed the ordinary update and model plan for this
/// exact instance, under its effective root, without subsequently changing its
/// model, local pose, links or transformed buffers. The instance's private
/// built-root words are not exposed; clean flags alone do not prove this
/// caller obligation. Invalid topology, shape or current-pose evidence keeps
/// the ordinary geometry path. Neither the instance nor its model is changed.
///
/// @param instance the exact instance whose ordinary transforms just completed
/// @param[in,out] scratch reusable topology storage; may change on a refused bound
/// @return the exact visible-vertex inclusive 16.16 model box, or no value for uncertain input
[[nodiscard]] inline std::optional<FixedBounds>
bound_completed_instance(const oa::sim::model_runtime::Instance& instance, Scratch& scratch) {
    namespace runtime = oa::sim::model_runtime;
    const auto& handle = instance.model_handle();
    const auto pieces = instance.pieces();
    if (!handle || pieces.empty() || pieces.size() != handle->objects.size() ||
        pieces.size() > std::numeric_limits<uint32_t>::max() || instance.transforms_dirty() ||
        !oa::base::float_precision::rounds_to_double() || std::fegetround() != FE_TONEAREST ||
        !oa::base::float_precision::float_control_matches(
            oa::base::float_precision::program_float_control().saved
        ))
        return std::nullopt;
    try {
        scratch.piece_for_object.assign(pieces.size(), runtime::kNoPiece);
        scratch.completed_topology_seen.assign(pieces.size(), 0);
        scratch.completed_pending.clear();
        scratch.completed_pending.reserve(pieces.size());
        scratch.completed_pending.push_back({0, runtime::kNoPiece});
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
    // Validate the same immutable preorder topology accepted by make_instance,
    // using retained storage rather than allocating a separate hierarchy check.
    for (std::size_t object = 0; object < handle->objects.size(); ++object) {
        const auto& source = handle->objects[object];
        for (const uint32_t link : {source.parent, source.first_child, source.next_sibling})
            if (link != runtime::kNoPiece && link >= pieces.size())
                return std::nullopt;
        if (source.parent != runtime::kNoPiece && source.parent >= object)
            return std::nullopt;
    }
    while (!scratch.completed_pending.empty()) {
        const auto [object, parent] = scratch.completed_pending.back();
        scratch.completed_pending.pop_back();
        if (scratch.completed_topology_seen[object] != 0)
            return std::nullopt;
        scratch.completed_topology_seen[object] = 1;
        const auto& source = handle->objects[object];
        if (source.parent != parent)
            return std::nullopt;
        // Even malformed graphs may only use the already reserved stack.
        const std::size_t links = (source.next_sibling != runtime::kNoPiece ? 1U : 0U) +
                                  (source.first_child != runtime::kNoPiece ? 1U : 0U);
        if (links > pieces.size() - scratch.completed_pending.size())
            return std::nullopt;
        if (source.next_sibling != runtime::kNoPiece)
            scratch.completed_pending.push_back({source.next_sibling, parent});
        if (source.first_child != runtime::kNoPiece)
            scratch.completed_pending.push_back({source.first_child, object});
    }
    if (std::find(
            scratch.completed_topology_seen.begin(), scratch.completed_topology_seen.end(), 0
        ) != scratch.completed_topology_seen.end())
        return std::nullopt;
    for (std::size_t index = 0; index < pieces.size(); ++index) {
        const auto object = pieces[index].object_index;
        if (object >= pieces.size() || scratch.piece_for_object[object] != runtime::kNoPiece)
            return std::nullopt;
        scratch.piece_for_object[object] = static_cast<uint32_t>(index);
    }
    if (instance.root_piece() != scratch.piece_for_object[0])
        return std::nullopt;
    const auto piece_link = [&](uint32_t object) {
        return object == runtime::kNoPiece ? runtime::kNoPiece : scratch.piece_for_object[object];
    };
    std::optional<FixedBounds> result;
    for (const auto& piece : pieces) {
        const auto& source = handle->objects[piece.object_index];
        if (piece.parent != piece_link(source.parent) ||
            piece.first_child != piece_link(source.first_child) ||
            piece.next_sibling != piece_link(source.next_sibling) || piece.transform_marker != 0 ||
            piece.transformed_vertices.size() != source.vertices.size() ||
            piece.translation.x != piece.built_translation.x ||
            piece.translation.y != piece.built_translation.y ||
            piece.translation.z != piece.built_translation.z ||
            piece.rotation.xy != piece.built_rotation.xy ||
            piece.rotation.xz != piece.built_rotation.xz ||
            piece.rotation.yz != piece.built_rotation.yz)
            return std::nullopt;
        if ((piece.flags & static_cast<uint16_t>(runtime::PieceFlag::visible)) == 0)
            continue;
        for (const auto& point : piece.transformed_vertices) {
            const FixedBounds bounds{point, point};
            if (result)
                detail::include(*result, bounds);
            else
                result = bounds;
        }
    }
    return result;
}

/// Current-frame projection and rounded shadow-map extent, prepared together.
struct FrameView {
    std::array<float, 12> projection{};
    std::array<float, 4> shadow_bounds{}; // left, top, right, bottom, including edge texels
    uint32_t width{}, height{};
    float shadow_scale{1};
    uint64_t frame{};
    /// Compares the current-frame proof inputs member by member.
    /// @return true when every member compares equal
    bool operator==(const FrameView&) const = default;
};

/// Caller facts required before a geometric rejection may be considered.
/// The caller still owes all canonical update, cache and effect side effects.
struct Eligibility {
    bool enhanced{}, local{}, rig_resolved{}, ground_resolved{}, gpu_type_supported{};
    /// Existing cast-depth/instance limits and ordinary native geometry are proved.
    bool budget_safe{}, native_geometry_resolved{}, fog_seen{};
    /// Ordinary model/adaptive detail, excluding global strategic dots.
    bool ordinary_model_path{};
    bool selected{}, debug{}, probe{}, reflection{}, complex{}, snapshot{}, vertex_override{};
    uint64_t frame{};
    uintptr_t owner_token{};
    uint32_t generation{}, rig_generation{};
    int32_t sea_level{}, ground_height{};
};

namespace detail {

/// Current camera inverse, valid only for one unchanged rejection attempt.
struct RejectionCamera {
    std::array<float, 3> across{}, down{};
};

/// Checks the policy, owner and camera facts shared by one or two raw bounds.
/// @param shown the exact resolved model and its 16.16 world origin
/// @param view the current screen projection and rounded shadow-map extent
/// @param gate the required current-frame policy, ownership and rig evidence
/// @return the existing camera inverse, or no value when any shared check refuses
[[nodiscard]] inline std::optional<RejectionCamera> prepare_rejection(
    const oa::present::model::ModelRef& shown, const FrameView& view, const Eligibility& gate
) {
    if (!gate.enhanced || !gate.local || !gate.rig_resolved || !gate.ground_resolved ||
        !gate.gpu_type_supported || !gate.budget_safe || !gate.native_geometry_resolved ||
        !gate.fog_seen || !gate.ordinary_model_path || gate.selected || gate.debug || gate.probe ||
        gate.reflection || gate.complex || gate.snapshot || gate.vertex_override ||
        gate.frame == 0 || gate.frame != view.frame || gate.generation == 0 ||
        gate.generation != gate.rig_generation || view.width == 0 || view.height == 0 ||
        !std::isfinite(view.shadow_scale) || !(view.shadow_scale > 0) ||
        !std::all_of(
            view.projection.begin(),
            view.projection.end(),
            [](float value) { return std::isfinite(value); }
        ) ||
        !std::all_of(
            view.shadow_bounds.begin(),
            view.shadow_bounds.end(),
            [](float value) { return std::isfinite(value); }
        ) ||
        !(view.shadow_bounds[0] < view.shadow_bounds[2]) ||
        !(view.shadow_bounds[1] < view.shadow_bounds[3]))
        return std::nullopt;
    ModelDraw candidate;
    candidate.model = shown;
    if (!geometry_shadow_metadata_candidate(candidate, gate.sea_level) ||
        (shown.unit->state_flags & oa::present::model::unit_state_cloaked) != 0 ||
        (shown.unit->flags & OA_UNIT_FLAG_SELECTED) != 0 || gate.owner_token == 0 ||
        shown.instance->owner_token() != gate.owner_token)
        return std::nullopt;
    const auto& p = view.projection;
    // Keep the accepted Scene's marker_axes camera domain, including its
    // depth row, determinant threshold and finite inverse columns.
    const std::array a{p[0], p[1], p[2]}, b{p[4], p[5], p[6]}, c{p[8], p[9], p[10]};
    std::array camera_across{
        b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0]
    };
    std::array camera_down{
        c[1] * a[2] - c[2] * a[1], c[2] * a[0] - c[0] * a[2], c[0] * a[1] - c[1] * a[0]
    };
    const float determinant =
        a[0] * camera_across[0] + a[1] * camera_across[1] + a[2] * camera_across[2];
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12F)
        return std::nullopt;
    for (std::size_t axis = 0; axis < camera_across.size(); ++axis) {
        camera_across[axis] /= determinant;
        camera_down[axis] /= determinant;
        if (!std::isfinite(camera_across[axis]) || !std::isfinite(camera_down[axis]))
            return std::nullopt;
    }
    return RejectionCamera{camera_across, camera_down};
}

/// Applies the unchanged body, reservation, marker and shadow rejection arithmetic.
/// @param shown the unchanged model already accepted by prepare_rejection
/// @param view the same attempt's current projection and shadow extent
/// @param gate the same attempt's accepted policy, owner and ground facts
/// @param fixed an enclosing inclusive 16.16 model-space bound
/// @param camera the camera inverse prepared for this exact attempt
/// @return true only when every contribution is proved outside
[[nodiscard]] inline bool rejection_proof_with_fixed_bound(
    const oa::present::model::ModelRef& shown,
    const FrameView& view,
    const Eligibility& gate,
    const FixedBounds& fixed,
    const RejectionCamera& camera
) {
    const auto& p = view.projection;
    const auto& camera_across = camera.across;
    const auto& camera_down = camera.down;
    constexpr float scale = 65536;
    const auto& position = shown.unit->position;
    const std::array origin{position.x / scale, position.y / scale, position.z / scale};
    const std::array low{
        origin[0] + fixed.low.x / scale,
        origin[1] + fixed.low.y / scale,
        origin[2] - fixed.high.z / scale
    };
    const std::array high{
        origin[0] + fixed.high.x / scale,
        origin[1] + fixed.high.y / scale,
        origin[2] - fixed.low.z / scale
    };
    // Keep the existing ground-shadow/GPU coordinate domain. An interval
    // expanded beyond supported coordinates conservatively stays ordinary.
    constexpr float coordinate_limit = 32768;
    for (std::size_t axis = 0; axis < low.size(); ++axis)
        if (!std::isfinite(low[axis]) || !std::isfinite(high[axis]) ||
            low[axis] < -coordinate_limit || high[axis] > coordinate_limit)
            return false;
    // The old detail classifier must also reserve exactly zero surface
    // geometry. Bound its translation-first center expression and its
    // extent separately, rather than inferring that budget result from
    // the model pass's differently ordered projection below.
    bool reservation_outside = false;
    for (std::size_t row = 0; row < 2; ++row) {
        const auto r = row * 4;
        float first = p[r + 3], last = first, extent = 0, margin = 0;
        for (std::size_t axis = 0; axis < low.size(); ++axis) {
            const float coefficient = p[r + axis];
            first += coefficient * (coefficient < 0 ? high[axis] : low[axis]);
            last += coefficient * (coefficient < 0 ? low[axis] : high[axis]);
            extent += std::abs(coefficient) * (high[axis] - low[axis]);
            margin += std::abs(coefficient);
        }
        const float after = last + extent * 0.5F + margin;
        const float before = first - extent * 0.5F - margin;
        if (!std::isfinite(first) || !std::isfinite(last) || !std::isfinite(extent) ||
            !std::isfinite(margin) || !std::isfinite(after) || !std::isfinite(before))
            return false;
        const float edge = static_cast<float>(row == 0 ? view.width : view.height);
        reservation_outside |= after < 0 || before > edge;
    }
    if (!reservation_outside)
        return false;
    bool body_outside = false;
    for (std::size_t row = 0; row < 2; ++row) {
        const auto r = row * 4;
        std::array<float, 3> least{}, greatest{};
        for (std::size_t axis = 0; axis < least.size(); ++axis) {
            least[axis] = p[r + axis] < 0 ? high[axis] : low[axis];
            greatest[axis] = p[r + axis] < 0 ? low[axis] : high[axis];
        }
        const float first = p[r] * least[0] + p[r + 1] * least[1] + p[r + 2] * least[2] + p[r + 3];
        const float last =
            p[r] * greatest[0] + p[r + 1] * greatest[1] + p[r + 2] * greatest[2] + p[r + 3];
        const float margin = 1.0F * (std::abs(p[r]) + std::abs(p[r + 1]) + std::abs(p[r + 2]));
        const float before = first - margin, after = last + margin;
        if (!std::isfinite(first) || !std::isfinite(last) || !std::isfinite(before) ||
            !std::isfinite(after))
            return false;
        const float edge = static_cast<float>(row == 0 ? view.width : view.height);
        body_outside |= after < 0 || before > edge;
    }
    if (!body_outside)
        return false;
    // An adaptive/budget marker uses the exact body's bbox midpoint as
    // its anchor. That midpoint lies inside this enclosing world box.
    // Bound every allowed radius and its upward lift, then prove the
    // marker's own early screen rejection, independently of its budget.
    constexpr float maximum_marker_radius = 64.0F * 0.5F;
    const float maximum_lift =
        maximum_marker_radius * (std::abs(camera_across[1]) + std::abs(camera_down[1])) + 0.25F;
    auto marker_low = low, marker_high = high;
    marker_low[1] = low[1] + 0.25F;
    marker_high[1] = high[1] + maximum_lift;
    if (!std::isfinite(maximum_lift) || !std::isfinite(marker_low[1]) ||
        !std::isfinite(marker_high[1]))
        return false;
    bool marker_outside = false;
    for (std::size_t row = 0; row < 2; ++row) {
        const auto r = row * 4;
        std::array<float, 3> least{}, greatest{};
        for (std::size_t axis = 0; axis < least.size(); ++axis) {
            least[axis] = p[r + axis] < 0 ? marker_high[axis] : marker_low[axis];
            greatest[axis] = p[r + axis] < 0 ? marker_low[axis] : marker_high[axis];
        }
        const float first = p[r] * least[0] + p[r + 1] * least[1] + p[r + 2] * least[2] + p[r + 3];
        const float last =
            p[r] * greatest[0] + p[r + 1] * greatest[1] + p[r + 2] * greatest[2] + p[r + 3];
        const float before = first - maximum_marker_radius;
        const float after = last + maximum_marker_radius;
        if (!std::isfinite(first) || !std::isfinite(last) || !std::isfinite(before) ||
            !std::isfinite(after))
            return false;
        const float edge = static_cast<float>(row == 0 ? view.width : view.height);
        marker_outside |= after < 0 || before > edge;
    }
    if (!marker_outside)
        return false;
    std::array<float, 3> least{}, greatest{};
    for (std::size_t axis = 0; axis < least.size(); ++axis) {
        least[axis] = std::floor(low[axis] - origin[axis]);
        greatest[axis] = std::floor(high[axis] - origin[axis]);
    }
    const float across = origin[0] + 0.0F;
    const float down = origin[2] + -static_cast<float>(gate.ground_height) * 0.5F;
    const float left = across + (oa::present::model::shadow_offset_x + least[0]);
    const float right = across + (oa::present::model::shadow_offset_x + greatest[0]);
    const float top = down + (least[2] - std::floor(greatest[1] * 0.5F));
    const float bottom = down + (greatest[2] - std::floor(least[1] * 0.5F));
    const float margin = 2 * view.shadow_scale + 1;
    if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) ||
        !std::isfinite(bottom) || !std::isfinite(margin))
        return false;
    const std::array padded{
        view.shadow_bounds[0] - margin,
        view.shadow_bounds[1] - margin,
        view.shadow_bounds[2] + margin,
        view.shadow_bounds[3] + margin
    };
    if (!std::all_of(padded.begin(), padded.end(), [](float value) {
            return std::isfinite(value);
        }))
        return false;
    return right < padded[0] || left > padded[2] || bottom < padded[1] || top > padded[3];
}

/// Shares the existing rejection arithmetic without choosing a geometry source.
/// @param shown the exact resolved model and its 16.16 world origin
/// @param view the current screen projection and rounded shadow-map extent
/// @param gate the required current-frame policy, ownership and rig evidence
/// @param bounds a deferred provider of an inclusive 16.16 model-space box
/// @return true only when all policy, budget, body, marker and shadow checks prove rejection
template <typename Bounds>
[[nodiscard]] inline bool rejection_proof_with_bounds(
    const oa::present::model::ModelRef& shown,
    const FrameView& view,
    const Eligibility& gate,
    Bounds&& bounds
) {
    const auto camera = prepare_rejection(shown, view, gate);
    if (!camera)
        return false;
    const auto bound = bounds();
    return bound && rejection_proof_with_fixed_bound(shown, view, gate, *bound, *camera);
}

} // namespace detail

/// Proves the ordinary ground body, possible marker and shadow lie outside this frame.
///
/// This is only a geometry proof, not an integrated replacement for planning.
/// The shown model must hold the exact resolved frame pose and effective root
/// shift; missing identity, policy or arithmetic evidence keeps the old path.
/// The ordinary detail classifier must also reserve no surface geometry.
///
/// @param geometry the immutable reset bounds of the shown instance's shared model
/// @param shown the current resolved ordinary ground model and its 16.16 world position
/// @param effective_root the shown state's thresholded rotation words, 65536 per turn
/// @param view the same frame's projection to screen pixels and shadow bounds in world pixels
/// @param gate the proved caller facts, including nonzero frame and matching rig generations
/// @param[in,out] scratch reusable piece-index storage; may change on a refused proof
/// @return true only for a complete outside proof; false for visible or uncertain input
[[nodiscard]] inline bool rejection_proof(
    const Geometry& geometry,
    const oa::present::model::ModelRef& shown,
    Rotation effective_root,
    const FrameView& view,
    const Eligibility& gate,
    Scratch& scratch
) {
    return detail::rejection_proof_with_bounds(shown, view, gate, [&] {
        return bound_instance(geometry, *shown.instance, effective_root, scratch);
    });
}

/// Proves distant current-pose geometry outside without any plane rotations.
/// A false result retains the existing exact raw-pose proof; it is not visibility.
/// @param geometry immutable prepared bounds and source radii
/// @param shown exact resolved current pose and world origin
/// @param view current screen projection and rounded shadow-map extent
/// @param gate required current policy, owner and generation evidence
/// @param[in,out] scratch reusable topology and path-radius storage
/// @return true only when the unchanged full body/marker/shadow predicate rejects
[[nodiscard]] inline bool far_rejection_proof(
    const Geometry& geometry,
    const oa::present::model::ModelRef& shown,
    const FrameView& view,
    const Eligibility& gate,
    Scratch& scratch
) {
    return detail::rejection_proof_with_bounds(shown, view, gate, [&] {
        return bound_far_instance(geometry, *shown.instance, scratch);
    });
}

/// Tries the far cube, then the exact raw-pose bound within one unchanged attempt.
///
/// Policy and camera checks are shared only by these immediately adjacent tests.
/// A successful far bound also proves the piece mapping for the exact fallback.
/// Every refused far bound uses the ordinary validated exact entry point instead,
/// including allocation failures and inputs outside the far proof's piece limit.
/// No topology, pose, ownership or camera evidence survives this function call.
///
/// @param geometry immutable prepared bounds and source radii
/// @param shown exact resolved current pose and world origin
/// @param effective_root the shown state's current effective root rotation words
/// @param view current screen projection and rounded shadow-map extent
/// @param gate required current policy, owner and generation evidence
/// @param[in,out] scratch reusable topology and path-radius storage
/// @param try_far whether the caller already permits the far proof
/// @return the same result as the ordinary far-first, exact-fallback sequence
[[nodiscard]] inline bool raw_rejection_proof(
    const Geometry& geometry,
    const oa::present::model::ModelRef& shown,
    Rotation effective_root,
    const FrameView& view,
    const Eligibility& gate,
    Scratch& scratch,
    bool try_far = true
) {
    const auto camera = detail::prepare_rejection(shown, view, gate);
    if (!camera)
        return false;
    std::optional<FixedBounds> far_bound;
    if (try_far) {
        far_bound = bound_far_instance(geometry, *shown.instance, scratch);
        if (far_bound &&
            detail::rejection_proof_with_fixed_bound(shown, view, gate, *far_bound, *camera))
            return true;
    }
    const auto exact =
        far_bound ? detail::bound_with_validated_topology(geometry, *shown.instance, effective_root)
                  : bound_instance(geometry, *shown.instance, effective_root, scratch);
    return exact && detail::rejection_proof_with_fixed_bound(shown, view, gate, *exact, *camera);
}

/// Proves a completed ordinary whole-tick model contributes no frame geometry.
///
/// The caller must just have completed its normal original-instance update
/// and model planning, preserving every ordinary state/cache/effect side
/// effect. This function reads the resulting native vertices without raw-pose
/// interval rotations. Its proof does not authorize resetting the completed
/// plan or model state; a caller may retain their references and list position
/// while disabling only the already proved invisible geometry.
///
/// @param completed the exact current linked Off plan, without vertex or position overrides
/// @param view the same frame's screen projection and rounded shadow-map bounds
/// @param gate the proved policy, owner, generation and actual completed-plan ground height
/// @param[in,out] scratch reusable topology storage; may change on a refused proof
/// @return true only when all completed-geometry and shared rejection checks succeed
[[nodiscard]] inline bool completed_rejection_proof(
    const ModelDraw& completed, const FrameView& view, const Eligibility& gate, Scratch& scratch
) {
    const auto& shown = completed.model;
    if (completed.preplan_omitted || !completed.plan.linked.drawn ||
        completed.plan.level != oa::present::model::UnitSupersampling::off ||
        !completed.plan.model.from_image || !completed.plan.model.carried.empty() ||
        completed.plan.model.ground != gate.ground_height || !completed.shown_vertices.empty() ||
        !completed.shown_position || shown.state == nullptr || shown.state->transforms_dirty ||
        shown.state->finer || shown.state->build_bands ||
        !geometry_shadow_metadata_candidate(completed, gate.sea_level))
        return false;
    const auto& position = *completed.shown_position;
    if (position.x != shown.unit->position.x || position.y != shown.unit->position.y ||
        position.z != shown.unit->position.z)
        return false;
    return detail::rejection_proof_with_bounds(shown, view, gate, [&] {
        return bound_completed_instance(*shown.instance, scratch);
    });
}

} // namespace oa::app::preplanning_visibility
