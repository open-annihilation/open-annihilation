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

/// Keeps the last logical draw pose while an invisible model's vertices wait.
/// Admitted local ground scripts run before draws. Their later-tick effects
/// refresh geometry before reading it, superseding this recipe by revision.
/// Other direct geometry readers must resolve it before consuming vectors.
struct DeferredModelGeometry {
    std::vector<oa::sim::model_runtime::TransformPose> poses;
    oa::sim::model_runtime::Instance* instance{};
    uint32_t generation{};
    uint64_t revision{};
    oa::sim::model_runtime::RotationWords root{};
    bool pending{};

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

    /// Prepares one slot's bounded storage on the calling thread.
    /// @param slot canonical unit slot, already present in units
    /// @param generation the model instance's current generation
    /// @param[in,out] instance the current canonical model
    void prepare(size_t slot, uint32_t generation, oa::sim::model_runtime::Instance& instance) {
        auto& kept = units[slot];
        if (kept.instance != &instance || kept.generation != generation) {
            kept.pending = false;
            kept.instance = &instance;
            kept.generation = generation;
        }
        kept.observe(instance);
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
