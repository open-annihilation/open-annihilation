// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The match renderer's own state (Runtime::match_models): prepared models
// and textures, every unit slot's draw state, the 8-bit bridge onto the RGB
// frame, and what the presentation keeps to draw frames between ticks and
// the units of players this machine does not simulate on their playout.
// runtime_match_render.cpp draws from it; the director's frame loop and the
// debug grid reach it too.
#pragma once

#include "oa/app/match_model_draws.hpp"
#include "oa/app/runtime.hpp"
#include "oa/core/world.h"
#include "oa/formats/objects3d.hpp"
#include "oa/present/model/model_draw.hpp"
#include "oa/present/model/model_library.hpp"
#include "oa/present/model/rgb_bridge.hpp"
#include "oa/present/model/shadow_fade.hpp"
#include "oa/present/model/unit_supersampling.hpp"
#include "oa/present/unit_playout.hpp"
#include "oa/sim/match_runtime.hpp"
#include "presentation_interpolation.hpp"
#include "world_draws.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace oa::app {

/// What the presentation keeps to draw a frame between two ticks: every unit
/// slot's, the projectile pool's and the debris table's state at the last two
/// ticks it saw, and where the frame being drawn lies between them.
struct MatchPresentation {
    /// The part of a tick the frame being drawn lies past the tick before;
    /// whole_tick draws the current tick as it is.
    uint32_t fraction{whole_tick};
    uint32_t tick{}; ///< the tick the presentation saw last
    bool seen{};     ///< `tick` holds a tick seen
    /// The ticks from the tick before to `tick` (batch_ticks): more than one
    /// when a frame's clock step ran a batch of them; 1 when there is no tick
    /// before. Particles that step each tick show that many steps' way.
    uint32_t batch{1};
    /// Counts the draws; a unit's copies note the draw they were placed for.
    uint64_t draw{};
    /// The playout of the units of players this machine does not simulate,
    /// which the runtime feeds after each step: a unit of another machine's
    /// player is drawn where its owner's playout clock has it (mirrored_pose).
    /// Null draws every unit where its ticks place it.
    const oa::present::unit_playout::Playout* playout{};
    /// The moment of the playout the last frame drawn showed: the pointer
    /// picks a mirrored unit where that frame drew it.
    oa::present::unit_playout::FrameTime drawn_moment{};
    /// Set while a unit draws from its copies: a carried unit then draws from
    /// its own copies too (the renderer's model_of). Only units that carry or
    /// are carried set it, on the one thread that plans them
    /// (plan_unit_draws); no other unit reaches a carried unit's model.
    bool blending{};
    std::vector<UnitMotion> units; ///< by unit slot
    /// A slot's model instance did not fit its unit (instance_fits_unit) and
    /// the log has said so: it says so once a match.
    bool misfit_logged{};
    ShotFlight shots{};
    /// Each live projectile's pose for the frame being drawn, by pool index.
    std::vector<ShotPose> presented_shots;
    DebrisFall debris{};
};

/// One unit of a frame's draw list, planned before the list takes its draws
/// in their order (plan_unit_draws in runtime_match_render.cpp): what the
/// drawing thread finds of it first, in the order, then what its plan works
/// out on whichever drawing thread takes it, which the list takes at the
/// unit's place in the order.
struct UnitPlan {
    uint16_t unit_index{};
    /// The unit's model (unit_model); a null instance draws nothing.
    oa::present::model::ModelRef model{};
    /// Where its owner's playout has it (mirrored_pose); nothing for a unit
    /// drawn where its ticks place it.
    std::optional<oa::present::unit_playout::UnitPose> pose{};
    /// Its movement state stands still: its animated textures show their
    /// first frame.
    bool idle{};
    /// It is drawn from its copies (presented_unit): between two ticks once
    /// it or a unit it carries moved, and on every frame on its playout.
    bool from_copies{};
    /// It carries units or is carried, so that its plan reaches the models
    /// of other units: such units are planned one after another, in their
    /// order, on one thread.
    bool linked{};
    /// What it is drawn as: its own model, or its copies'.
    oa::present::model::ModelRef shown{};
    /// It adds a model draw to the list: false for a carried unit drawn from
    /// its copies, which its carrier draws.
    bool adds_draw{};
    ModelDraw draw{};
    /// A record of each unit it carries, which the live renderer draws from
    /// (ModelDraw), listed after its own.
    std::vector<ModelDraw> carried;
};

/// The frame's units to draw, planned before the draw list takes them
/// (plan_unit_draws), and the buffers the planning keeps from frame to frame.
struct UnitPlans {
    /// By the unit's place among the frame's units to draw.
    std::vector<UnitPlan> plans;
    /// The places of a run of units in the draw order, the units of the
    /// ground or those off it.
    std::vector<uint32_t> run;
    /// The run's places in the order they are planned: those that carry or
    /// are carried first, in the run's order, then the others.
    std::vector<uint32_t> order;
    /// By unit slot: the run whose carrier last found the slot's unit among
    /// those it carries.
    std::vector<uint32_t> carried_in_run;
    /// Counts the runs planned, for carried_in_run.
    uint32_t runs{};
    /// The vertex buffers of the last frame's model draws
    /// (ModelDraw::shown_vertices), which this frame's take over.
    std::vector<std::vector<std::vector<oa::formats::objects3d::FixedVector3>>> spare_vertices;
};

/// 3DO renderer state of one match: prepared models and textures, the draw
/// state of every unit slot, and the 8-bit bridge onto the RGB frame. Each 3D
/// feature keeps its own draw state in its MatchFeatureDraw.
struct MatchModels {
    const void* match{};
    oa::sim::match_runtime::Match* offline{};
    oa::present::model::ModelLibrary library;
    oa::present::model::ModelDisplay display;
    oa::present::model::ModelRenderer renderer;
    /// The faded alpha table the renderer's shadows blend through while the
    /// view is zoomed out (set_frame_shadows).
    oa::present::model::ShadowTable shadow_table;
    oa::present::model::RgbBridge bridge;
    // The buffers of units drawn finer than the frame (enhanced anti-aliasing).
    oa::present::model::SupersampleScratch supersample;
    // The frame's battlefield draws in order, worked out before any band
    // draws them (world_draws.hpp).
    WorldDrawList draws;
    // The bands the frame's bridge is split into, one for each drawing
    // thread; one band covers the whole frame on one thread.
    std::vector<oa::present::model::BridgeBand> bands;
    // The buffers of the bands after the first, which draws with the
    // renderer and supersampling buffers above; none on one thread. Their
    // renderers plan the frame's units on the drawing threads after the
    // first too (plan_unit_draws).
    std::vector<WorldBandScratch> band_scratch;
    // The frame's units, planned before the draws are worked out in order.
    UnitPlans unit_plans;
    // The first band's buffer of turned debris points.
    std::vector<oa::formats::objects3d::FixedVector3> debris_points;
    // The most bands a frame of the match was drawn in, which a frame run
    // reports.
    int32_t most_bands{};
    std::vector<UnitDrawState> units; // by unit slot
    // 3D features draw through one zeroed unit record marked as a z-buffered
    // building owned by the viewpoint player, kept here in place of its part
    // of Game.search_context_block.
    oa::Unit feature_unit{};

    // The building being placed, drawn at its site as a nanoframe
    // (ui.build-preview): its type's model, the draw state begun for it and
    // the unit record it draws through.
    struct BuildPreview {
        uint16_t type{}; ///< the type the instance was made for; 0 for none
        view_rules::BuildFacing facing{};
        oa::sim::model_runtime::Instance instance{};
        oa::present::model::ModelState state{};
        oa::Unit unit{};
    };

    BuildPreview build_preview{};
    // The build effect's clock, which slows the nanoframes' pulse in a view
    // zoomed out (advance_build_pulse).
    oa::present::model::BuildPulseClock build_pulse{};
    uint32_t animation_tick{};
    bool animation_started{};
    std::unordered_map<uint16_t, std::shared_ptr<const oa::formats::objects3d::Model>>
        weapon_models;
    std::vector<uint8_t> shadow_pixels;
    oa::Sprite projectile_shadow{}; // FX.GAF "shadow" frame 0
    MatchPresentation presentation{};
    DebugGridRandom debug_random{};
};

/// Returns the moment of the unit playout a frame drawn part of the way
/// through the current tick shows: that part of the way from the tick
/// before the presentation's batch to the tick it saw last
/// (oa::present::unit_playout::frame_time_between).
///
/// @param presentation the presentation, its tick noted
/// @param fraction the part of the way, 0 to whole_tick
/// @return the moment; at whole_tick, the tick the presentation saw last
[[nodiscard]] oa::present::unit_playout::FrameTime
playout_moment(const MatchPresentation& presentation, uint32_t fraction) noexcept;

/// Returns where and how a frame draws a unit of another machine's player
/// (in use with OA_PLAYER_STATUS_MIRRORED): where its owner's playout clock
/// has it at the frame's moment, whole ticks included, rather than where its
/// owner's newest record put it. A carried unit goes with its carrier: it is
/// moved as far as its carrier is drawn from the carrier's place; a unit
/// carried by one drawn where its ticks place it is drawn there too.
///
/// @param models the match's renderer state, its tick noted
/// @param world the match's World
/// @param slot unit slot
/// @param moment the moment the frame shows (playout_moment)
/// @return the pose; nothing for a unit drawn where its ticks place it: one
///     this machine simulates, one of a player in use but free or closed, one
///     carried by such a unit, an empty slot, or a presentation with no
///     playout or no tick noted
[[nodiscard]] std::optional<oa::present::unit_playout::UnitPose> mirrored_pose(
    const MatchModels& models,
    const oa::World& world,
    uint16_t slot,
    oa::present::unit_playout::FrameTime moment
) noexcept;

/// Returns where a unit shows in a frame drawn part of the way through the current tick.
///
/// @param models the match's renderer state, its tick noted
/// @param world the match's World
/// @param slot unit slot
/// @param fraction the part of the way, 0 to whole_tick
/// @return for a unit of another machine's player, its place on the playout
///     at that moment (mirrored_pose), at whole_tick too;
///     else its position that part of the way from the tick before when the
///     fraction is below whole_tick and the unit moved, else Unit.position;
///     a slot outside the pool gives the origin
[[nodiscard]] FixedVec3 shown_unit_position(
    const MatchModels& models, const oa::World& world, uint16_t slot, uint32_t fraction
);

/// Returns where a unit shows in the frame being drawn: shown_unit_position
/// at the presentation's fraction, which is whole_tick outside a draw.
///
/// @param models the match's renderer state, its tick noted
/// @param world the match's World
/// @param slot unit slot
/// @return its position as the frame shows it
[[nodiscard]] FixedVec3
shown_unit_position(const MatchModels& models, const oa::World& world, uint16_t slot);

/// Notes the tick the match is on in the presentation: every unit's pose,
/// the projectile pool and the debris table, as observe_unit, observe_shots
/// and observe_debris note them. Reads the match and writes nothing of it.
///
/// @param[in,out] models the match's renderer state
/// @param match the match
void observe_match_tick(MatchModels& models, oa::sim::match_runtime::Match& match);

} // namespace oa::app
