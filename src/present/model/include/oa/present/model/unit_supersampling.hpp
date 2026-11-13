// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Enhanced anti-aliasing of units: each unit's model, and its shadow, drawn
// at a whole number of samples along each axis of a game pixel into samples
// of its own and reduced into the frame, each frame pixel blending the
// average of the samples the unit covers with what lies under it by that
// coverage, so that the unit's edges are smoothed. The unit's cached image
// is kept at the same number of samples and built again exactly when the
// game's image is. Terrain, sprites, the fog and the interface are drawn as
// they are. At UnitSupersampling::off a unit draws exactly as without the
// setting. Presentation only: nothing the simulation reads changes.
#pragma once

#include "oa/present/model/model_draw.hpp"
#include "oa/present/model/rgb_bridge.hpp"

#include <cstdint>
#include <optional>

namespace oa::present::model {

/// Samples along each axis of a frame pixel a unit is drawn with; the
/// value is that number.
enum class UnitSupersampling : uint8_t {
    off = 1,
    x2 = 2,
    x4 = 4,
    x8 = 8,
    x16 = 16,
};

/// Returns the samples along each axis a level draws with.
///
/// @param level the level
/// @return 1 for off, else the level's number
[[nodiscard]] constexpr uint32_t supersampling_factor(UnitSupersampling level) noexcept {
    return static_cast<uint32_t>(level);
}

/// Returns the level that draws with a number of samples along each axis.
///
/// @param factor samples along each axis
/// @return the level; nothing for a number no level draws with
[[nodiscard]] std::optional<UnitSupersampling>
unit_supersampling_from_factor(uint32_t factor) noexcept;

/// Most samples one unit's draw may cover; a unit whose region would need
/// more at its level draws at the highest level that fits, and at
/// UnitSupersampling::off when none does.
inline constexpr uint64_t supersample_max_samples = 16 * 1024 * 1024;

/// The buffers supersampled unit draws use, kept from unit to unit and frame
/// to frame so that drawing allocates only when a unit needs more.
struct SupersampleScratch {
    SampledRegion region{}; ///< the samples a unit is drawn into
};

/// Returns the level a region of the bridge draws at under a chosen level.
///
/// @param region inclusive rectangle in the bridge's 8-bit pixels, already
///     clipped to the bridge's surface
/// @param level the level chosen
/// @return the highest level not above `level` whose samples over the region
///     stay within supersample_max_samples; off for an empty region
[[nodiscard]] UnitSupersampling
fitting_supersampling(const Rect32& region, UnitSupersampling level) noexcept;

/// What a unit's draw at a level of enhanced anti-aliasing builds and
/// decides (draw_unit_supersampled), worked out by plan_unit_supersampled,
/// so that draw_planned_unit can draw the unit, whole or in bands, without
/// building or changing anything but the bridge, the frame, the renderer's
/// composite and the scratch.
struct SupersampledUnitPlan {
    // Rect32 is packed (1-byte aligned): the rectangles come first, at
    // 4-byte-aligned places, since the drawing takes their fields by
    // reference.
    /// The region the bridge captures for the unit at UnitSupersampling::off,
    /// in its 8-bit pixels.
    alignas(4) Rect32 region{};
    /// The region the samples cover at the other levels.
    alignas(4) Rect32 covered{};
    /// The level it draws at (fitting_supersampling); off draws into the bridge.
    UnitSupersampling level{UnitSupersampling::off};
    /// What the draw's readying gave (prepare_linked_draw): a carried unit
    /// is not drawn.
    LinkedDraw linked{};
    /// The model the samples are drawn from: the unit's, with its finer draw state.
    ModelRef finer{};
    /// The model's draw (plan_model_draw), at the level's samples.
    ModelDrawPlan model{};
};

/// The caller's verified source for a model's complete shadow.
enum class ModelShadowSource : uint8_t { cached_image, verified_geometry };

/// Readies a unit and builds what its draw at a level builds, as
/// draw_unit_supersampled does, without drawing.
///
/// The draw states change as draw_unit_supersampled changes them, the finer
/// image built again when the game's was. A metadata plan belongs to a native
/// world frame; before a processor draw, plan again with pixels required.
///
/// @param[in,out] renderer drawing context
/// @param bridge the frame's model bridge, set up by bridge_begin; nothing
///     is captured
/// @param model the unit to draw
/// @param region the inclusive rectangle, in the bridge's 8-bit pixels, the
///     unit may draw in
/// @param movement_idle the movement object's idle flag, for mobile units
/// @param level the level
/// @param[out] plan what draw_planned_unit needs; its buffers are reused
/// @param image_plan whether cached pixels are needed; metadata is used only
///     at UnitSupersampling::off
/// @param shadow_source whether the caller has proved that the
///     current native renderer can cast this unit's complete shadow without
///     cached pixels; otherwise shadows continue to require pixels
void plan_unit_supersampled(
    ModelRenderer& renderer,
    const RgbBridge& bridge,
    const ModelRef& model,
    const Rect32& region,
    bool movement_idle,
    UnitSupersampling level,
    SupersampledUnitPlan& plan,
    ModelImagePlan image_plan = ModelImagePlan::pixels,
    ModelShadowSource shadow_source = ModelShadowSource::cached_image
);

/// Draws a unit planned by plan_unit_supersampled, as draw_unit_supersampled
/// draws it, through the whole bridge or one band of it.
///
/// Builds nothing and changes no draw state: a unit is planned once and
/// drawn by every band of a frame.
///
/// @param[in,out] renderer drawing context; its composite is used as scratch
/// @param[in,out] bridge the frame's model bridge
/// @param[in,out] band the band of the bridge to draw (bridge_split); null
///     for the whole bridge
/// @param[in,out] scratch the draw's buffers
/// @param model the unit plan_unit_supersampled planned
/// @param plan its plan
/// @param[in,out] bridge_holds_draws as draw_unit_supersampled takes it, for
///     the bridge or the band
void draw_planned_unit(
    ModelRenderer& renderer,
    RgbBridge& bridge,
    BridgeBand* band,
    SupersampleScratch& scratch,
    const ModelRef& model,
    const SupersampledUnitPlan& plan,
    bool& bridge_holds_draws
);

/// Draws a unit at a level of enhanced anti-aliasing.
///
/// At UnitSupersampling::off the unit draws into the bridge as it always
/// has: the region is captured (bridge_open) and draw_linked_model draws the
/// unit there, to be written back with the bridge's other draws; any finer
/// image the unit kept is let go. At the other levels the unit is readied as
/// draw_linked_model readies it (prepare_linked_draw), everything the bridge
/// holds is written back (bridge_end), and the unit, its shadow and its
/// carried units are drawn at the level (fitting_supersampling) into samples
/// over the part of the region the draw covers (bridge_open_sampled), from
/// its finer image (ModelState::finer), which is built again whenever the
/// game's image is, and reduced straight into the bridge's frame
/// (bridge_end_sampled). It plans the unit (plan_unit_supersampled) and
/// draws the plan (draw_planned_unit) through the whole bridge.
///
/// @param[in,out] renderer drawing context
/// @param[in,out] bridge the frame's model bridge
/// @param[in,out] scratch the draw's buffers
/// @param model the unit to draw
/// @param region the inclusive rectangle, in the bridge's 8-bit pixels, the
///     unit may draw in
/// @param movement_idle the movement object's idle flag, for mobile units
/// @param level the level
/// @param[in,out] bridge_holds_draws true while the bridge holds draws not
///     yet written back to the frame; the call keeps it true after drawing
///     into the bridge and makes it false after writing the bridge back
void draw_unit_supersampled(
    ModelRenderer& renderer,
    RgbBridge& bridge,
    SupersampleScratch& scratch,
    const ModelRef& model,
    const Rect32& region,
    bool movement_idle,
    UnitSupersampling level,
    bool& bridge_holds_draws
);

} // namespace oa::present::model
