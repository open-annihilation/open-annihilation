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
};

/// Raw per-object bounds. These are not a static bound of an animated model.
struct Geometry {
    std::shared_ptr<const oa::formats::objects3d::Model> model;
    std::vector<ObjectBounds> objects;
};

/// Reusable topology validation storage; no transformed vertices are retained.
struct Scratch {
    std::vector<uint32_t> piece_for_object;
};

namespace detail {

/// Matches the native reset/offset negation, including INT32_MIN.
/// @param value the signed 16.16 model coordinate
/// @return the negated coordinate with low 32 bits kept; INT32_MIN remains unchanged
inline int32_t negate(int32_t value) noexcept {
    return std::bit_cast<int32_t>(0U - std::bit_cast<uint32_t>(value));
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
                const FixedBounds point{reset, reset};
                if (target.nonempty)
                    detail::include(target.box, point);
                else {
                    target.box = point;
                    target.nonempty = true;
                }
            }
        }
        return result;
    } catch (const std::bad_alloc&) {
        return {};
    }
}

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
        return false;
    ModelDraw candidate;
    candidate.model = shown;
    if (!geometry_shadow_metadata_candidate(candidate, gate.sea_level) ||
        (shown.unit->state_flags & oa::present::model::unit_state_cloaked) != 0 ||
        (shown.unit->flags & OA_UNIT_FLAG_SELECTED) != 0 || gate.owner_token == 0 ||
        shown.instance->owner_token() != gate.owner_token)
        return false;
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
        return false;
    for (std::size_t axis = 0; axis < camera_across.size(); ++axis) {
        camera_across[axis] /= determinant;
        camera_down[axis] /= determinant;
        if (!std::isfinite(camera_across[axis]) || !std::isfinite(camera_down[axis]))
            return false;
    }
    const auto fixed = bound_instance(geometry, *shown.instance, effective_root, scratch);
    if (!fixed)
        return false;
    constexpr float scale = 65536;
    const auto& position = shown.unit->position;
    const std::array origin{position.x / scale, position.y / scale, position.z / scale};
    const std::array low{
        origin[0] + fixed->low.x / scale,
        origin[1] + fixed->low.y / scale,
        origin[2] - fixed->high.z / scale
    };
    const std::array high{
        origin[0] + fixed->high.x / scale,
        origin[1] + fixed->high.y / scale,
        origin[2] - fixed->low.z / scale
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
        const float edge = row == 0 ? view.width : view.height;
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
        const float edge = row == 0 ? view.width : view.height;
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
        const float edge = row == 0 ? view.width : view.height;
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

} // namespace oa::app::preplanning_visibility
