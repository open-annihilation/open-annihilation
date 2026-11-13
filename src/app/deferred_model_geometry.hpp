// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "oa/present/model/model_draw.hpp"
#include "oa/present/model/sprite_placement.hpp"
#include "oa/sim/model_runtime/instance.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace oa::app {

/// Tracks current geometry and image obligations of invisible model draws.
/// Admitted local ground scripts refresh geometry before effect reads; newer
/// materializations satisfy geometry without validating an older cached image.
/// Historical pose storage remains available to explicit retained-pose callers.
struct DeferredModelGeometry {
    std::vector<oa::sim::model_runtime::TransformPose> poses;
    oa::sim::model_runtime::Instance* instance{};
    uint32_t generation{};
    uint64_t revision{};
    oa::sim::model_runtime::RotationWords root{};
    bool pending{};
    /// Current-pose geometry and image obligations of the version-10 draw path.
    bool fresh_geometry{};
    bool fresh_original_images{};
    bool fresh_copy_images{};

    /// Accepts newer effect geometry without validating frozen presentation images.
    /// @param current the canonical model instance
    void observe_fresh(oa::sim::model_runtime::Instance& current) noexcept {
        if (instance != &current || revision != current.materialization_revision())
            fresh_geometry = false;
    }

    /// Freezes expensive presentation work after a complete invisibility proof.
    /// @param current the canonical model, whose raw inputs and vectors are unchanged
    /// @param geometry_needed whether an ordinary draw would update its vectors
    /// @param copied whether the omitted draw uses interpolated copies
    void freeze_current(
        oa::sim::model_runtime::Instance& current, bool geometry_needed, bool copied
    ) noexcept {
        observe_fresh(current);
        fresh_geometry |= geometry_needed;
        fresh_original_images = true;
        fresh_copy_images |= copied;
        revision = current.materialization_revision();
    }

    /// Discards a recipe superseded by a simulation effect's geometry write.
    /// @param current the current canonical model instance
    void observe(oa::sim::model_runtime::Instance& current) noexcept {
        if (instance != &current || revision != current.materialization_revision())
            pending = false;
    }

    /// Restores the last logical draw's geometry before a previous-region read.
    /// @param[in,out] current the canonical instance, keeping current raw inputs
    /// @return true when no recipe remains or its geometry was restored
    [[nodiscard]] bool resolve(oa::sim::model_runtime::Instance& current) noexcept {
        observe(current);
        if (!pending)
            return true;
        if (!current.rebuild_retained_transforms(poses, root, true))
            return false;
        pending = false;
        revision = current.materialization_revision();
        return true;
    }

    /// Records current raw inputs without claiming that their vectors are built.
    /// @param[in,out] current the canonical instance, whose raw poses stay unchanged
    /// @param shift the current thresholded root rotation
    /// @return true when admitted storage holds the complete recipe
    [[nodiscard]] bool retain(
        oa::sim::model_runtime::Instance& current, oa::sim::model_runtime::RotationWords shift
    ) noexcept {
        if (instance != &current || poses.size() != current.pieces().size())
            return false;
        const auto pieces = current.pieces();
        for (size_t i = 0; i < pieces.size(); ++i) {
            poses[i] = {pieces[i].translation, pieces[i].rotation};
            pieces[i].transform_marker = 0;
        }
        root = shift;
        revision = current.materialization_revision();
        pending = true;
        return true;
    }
};

/// Bounds recipe storage across the match; workers never allocate recipes.
struct DeferredModelGeometries {
    static constexpr size_t maximum_bytes = size_t{32} * 1024 * 1024;
    static constexpr size_t maximum_pieces = 256;
    std::vector<DeferredModelGeometry> units;
    size_t bytes{};
    /// Allocation work granted to cold ingress snapshot buffers during this match.
    size_t snapshot_warm_bytes{};
    static constexpr size_t snapshot_warm_limit = size_t{8} * 1024 * 1024;
    static constexpr size_t snapshot_warm_frame_limit = size_t{256} * 1024;

    /// Preallocates a cold snapshot buffer without advancing any pose history.
    /// Visible work never waits for this optional bounded allocation budget.
    /// @param[in,out] state a draw state whose snapshot buffer may be cold
    /// @param pieces current canonical piece count
    /// @param[in,out] frame_room bytes left in this frame's optional warm budget
    /// @return true when a bounded capacity was prepared
    [[nodiscard]] bool
    prewarm_snapshots(oa::present::model::ModelState& state, size_t pieces, size_t& frame_room) {
        using Snapshot = oa::present::model::ModelState::PieceSnapshot;
        if (!state.pieces.empty() || state.pieces.capacity() != 0 || pieces == 0 ||
            pieces > maximum_pieces || pieces > frame_room / sizeof(Snapshot) ||
            pieces > (snapshot_warm_limit - snapshot_warm_bytes) / sizeof(Snapshot))
            return false;
        try {
            std::vector<Snapshot> next;
            next.reserve(pieces);
            const size_t used = next.capacity() * sizeof(Snapshot);
            if (used > frame_room || used > snapshot_warm_limit - snapshot_warm_bytes)
                return false;
            state.pieces = std::move(next);
            snapshot_warm_bytes += used;
            frame_room -= used;
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    /// Prepares one slot's bounded storage on the calling thread.
    /// @param slot canonical unit slot, already present in units
    /// @param generation the model instance's current generation
    /// @param[in,out] instance the current canonical model
    /// @param retained_storage whether a historical-pose caller needs recipe storage
    void prepare(
        size_t slot,
        uint32_t generation,
        oa::sim::model_runtime::Instance& instance,
        bool retained_storage = true
    ) {
        auto& kept = units[slot];
        if (kept.instance != &instance || kept.generation != generation) {
            kept.pending = false;
            kept.fresh_geometry = false;
            kept.fresh_original_images = false;
            kept.fresh_copy_images = false;
            kept.instance = &instance;
            kept.generation = generation;
        }
        kept.observe(instance);
        kept.observe_fresh(instance);
        if (!retained_storage)
            return;
        const size_t count = instance.pieces().size();
        if (count == kept.poses.size() || count == 0 || count > maximum_pieces)
            return;
        const size_t previous = kept.poses.capacity() * sizeof(kept.poses[0]);
        const size_t available = maximum_bytes - (bytes - previous);
        if (count > available / sizeof(kept.poses[0]))
            return;
        try {
            std::vector<oa::sim::model_runtime::TransformPose> next(count);
            const size_t used = next.capacity() * sizeof(next[0]);
            if (used > available)
                return;
            kept.poses = std::move(next);
            bytes = bytes - previous + used;
        } catch (const std::bad_alloc&) {
            // Missing storage keeps ordinary geometry planning.
        }
    }
};

/// Invalidates an offscreen image history before a real consumer needs pixels.
///
/// Small buffers retain their capacity for ingress. Oversized buffers are freed;
/// visible rebuilding still uses the ordinary bounded allocation/fallback path.
/// No visible frame waits for a background image or consumes stale pixels.
///
/// @param[in,out] state the renderer-owned cache; root/dirty/build revision stay current
inline void invalidate_model_images(oa::present::model::ModelState& state) {
    constexpr size_t maximum_retained_buffer_bytes = size_t{64} * 1024;
    const auto clear = [](oa::present::SpriteBuffer& image) {
        image.sprite = {};
        if (image.pixels.capacity() > maximum_retained_buffer_bytes)
            std::vector<uint8_t>{}.swap(image.pixels);
        else
            image.pixels.clear();
    };
    state.cache = {};
    clear(state.image);
    clear(state.shadow);
    state.deferred_image.reset();
    state.deferred_image_depth = false;
    state.image_unfinished = false;
    state.finer.reset();
    state.pieces.clear();
}

/// Predicts the metadata-only cache step that can run without vertex reads.
/// @param model a completed unlinked model with current pose/cache invalidations noted
/// @param idle whether moving texture frames use their first frame
/// @return the ordinary cache outcome, or nothing when geometry/pixels are needed
[[nodiscard]] inline std::optional<oa::present::model::UnitSpriteDraw>
deferred_model_cache_step(const oa::present::model::ModelRef& model, bool idle) noexcept {
    namespace draw = oa::present::model;
    if (!model.instance || !model.state || !model.unit || model.state->image_unfinished ||
        model.state->finer || model.state->build_bands || model.unit->build_remaining != 0 ||
        model.unit->attach_parent != 0 || model.unit->attach_first_child != 0)
        return std::nullopt;
    auto& state = *model.state;
    const bool image = state.image.sprite.data != nullptr || state.deferred_image.has_value();
    if (!image)
        return std::nullopt;
    auto cache = state.cache;
    cache.has_image = true;
    cache.image_has_mask = state.image.sprite.data != nullptr ? state.image.sprite.aux != nullptr
                                                              : state.deferred_image_depth;
    const auto step = draw::prepare_unit_sprite(*model.unit, cache, idle);
    return step.rebuild ? std::nullopt : std::optional{step};
}

} // namespace oa::app
