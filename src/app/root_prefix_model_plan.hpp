// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "deferred_model_geometry.hpp"
#include "root_prefix_geometry.hpp"
#include "world_draws.hpp"

#include "oa/present/model/unit_supersampling.hpp"

#include <utility>

namespace oa::app {

/// Publishes an admitted rendering recipe while retaining native geometry debt.
///
/// The caller has qualified the frame, current unit generation, type and local
/// pose. Only renderer-owned image/cache state changes: neither instance's raw
/// inputs, dirtiness, transformed vectors nor materialization revision changes.
/// A distinct shown instance owns its presentation state and records copy-image
/// debt without satisfying the original's native geometry or image obligations.
///
/// @param original the canonical model whose geometry can be read by simulation effects
/// @param shown the canonical model or its resolved exclusive presentation copy
/// @param[in,out] deferred the original's current-generation geometry and image obligations
/// @param recipe the immutable prefix and final-root rendering placement
/// @param ground terrain height beneath the shown unit, in world pixels
/// @param idle whether the unit's animated textures show their idle frame
/// @param[out] draw the admitted recipe draw, replacing its previous contents
/// @return true when publication succeeded; invalid references leave state unchanged
[[nodiscard]] inline bool publish_root_prefix_draw(
    const present::model::ModelRef& original,
    const present::model::ModelRef& shown,
    DeferredModelGeometry& deferred,
    RootPrefixRecipe recipe,
    int32_t ground,
    bool idle,
    ModelDraw& draw
) {
    if (!original.instance || !original.state || !original.unit ||
        deferred.instance != original.instance || !shown.instance || !shown.state ||
        !shown.unit || !shown.prepared || !recipe.geometry || !recipe.geometry->model)
        return false;
    const auto& model = shown.instance->model_handle();
    if (!model || recipe.geometry->model.get() != model.get() ||
        recipe.geometry->model.owner_before(model) ||
        model.owner_before(recipe.geometry->model) ||
        recipe.geometry->objects.size() != shown.instance->pieces().size())
        return false;
    const bool copied = shown.instance != original.instance;
    if (copied && shown.state == original.state)
        return false;
    present::model::note_piece_changes(original);
    if (copied)
        present::model::note_piece_changes(shown);
    deferred.observe_fresh(*original.instance);
    const bool needed = deferred.fresh_geometry || original.state->transforms_dirty ||
                        original.instance->transforms_dirty();
    deferred.freeze_current(*original.instance, needed, copied);
    original.state->transforms_dirty = false;
    shown.state->transforms_dirty = false;
    draw = {};
    draw.model = shown;
    draw.shown_position = shown.unit->position;
    draw.root_prefix = std::move(recipe);
    draw.plan.level = present::model::UnitSupersampling::off;
    draw.plan.covered = {0, 0, -1, -1};
    draw.plan.model.from_image = true;
    draw.plan.model.ground = ground;
    draw.plan.linked = {true, idle};
    return true;
}

/// Replaces one refused recipe with current native geometry and genuine pixels.
///
/// Runs before Scene workers or another ordinary consumer reads the draw.
/// Qualified recipe draws are unlinked; no unit/script query or carried-model
/// traversal is introduced. A copy rebuild satisfies only its own image debt.
///
/// @param[in,out] renderer the caller's ordinary model renderer
/// @param bridge the current native image bridge
/// @param[in,out] draw the refused recipe and its ordinary replacement
/// @param[in,out] deferred matching current-generation original obligations, or null
/// @return true when a recipe became an ordinary plan; false for an invalid or ordinary draw
[[nodiscard]] inline bool materialize_root_prefix_draw(
    present::model::ModelRenderer& renderer,
    const present::model::RgbBridge& bridge,
    ModelDraw& draw,
    DeferredModelGeometry* deferred = nullptr
) {
    const auto model = draw.model;
    if (!draw.root_prefix || !model.instance || !model.state || !model.unit ||
        !model.prepared || model.unit->attach_parent != 0 || model.unit->attach_first_child != 0)
        return false;
    const auto position = draw.shown_position.value_or(model.unit->position);
    const bool idle = draw.plan.linked.unlit;
    invalidate_model_images(*model.state);
    model.state->transforms_dirty = true;
    present::model::update_model_transforms(model);
    ModelDraw ordinary;
    ordinary.model = model;
    ordinary.shown_position = position;
    present::model::plan_unit_supersampled(
        renderer,
        bridge,
        model,
        {},
        idle,
        present::model::UnitSupersampling::off,
        ordinary.plan,
        present::model::ModelImagePlan::pixels
    );
    draw = std::move(ordinary);
    if (deferred) {
        if (model.instance == deferred->instance) {
            deferred->fresh_geometry = false;
            deferred->fresh_original_images = false;
            deferred->revision = model.instance->materialization_revision();
        } else
            deferred->fresh_copy_images = false;
    }
    return true;
}

} // namespace oa::app
