// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// 3DO unit, feature, debris and projectile drawing. A unit draws from a
// cached 8-bit image of its model (key 1, optional depth plane) that holds
// the pieces flagged as cached; pieces that move every frame are drawn on
// top of a per-frame copy in the shared composite buffer, which also takes
// the build (nanoframe) effect, underwater clipping and carried units.
// Mobile units cast their image as a silhouette at ground level; buildings
// cast a sheared, run-length encoded silhouette cached with the image.
//
// Screen positions follow the game's battlefield placement: world x/z minus
// the camera, y lifting the point by half its height, plus the battlefield's
// screen origin (0x80, 0x20 on the game's 640x480 screen).

#include "oa/core/world.h"
#include "oa/data/limits.hpp"
#include "oa/sim/effect_particles.hpp"
#include "oa/sim/model_runtime/instance.hpp"
#include "oa/present/model/model_library.hpp"
#include "oa/present/surface.hpp"
#include "oa/present/model/sprite_placement.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace oa::present::model {

inline constexpr int32_t screen_left = 0x80;
inline constexpr int32_t screen_top = 0x20;
// Ground silhouettes are drawn this far right of the model.
inline constexpr int32_t shadow_offset_x = 5;
// Depth of a vertex is its height plus this base (plus the digger bias).
inline constexpr int32_t model_depth_base = 0x32;
inline constexpr int32_t digger_depth_bias = 0x4b;
inline constexpr int32_t shadow_depth_base = 0x19;
// Diggers are clipped below this depth.
inline constexpr uint8_t digger_clip_depth = 0x7d;
// Key and fill of every model image.
inline constexpr uint8_t image_key = 1;
// Side of the square composite buffer allocated at startup in 3.1c
// (data::limits::ModelComposite).
inline constexpr int32_t composite_side = 600;
// A root rotation word must move this far before the transforms are redone.
inline constexpr int32_t shift_threshold = 8;
// Shade row of an unlit vertex, and the light scale of the shaded builder.
inline constexpr int32_t unlit_shade = 0xf;
inline constexpr uint32_t shade_row_mask = 0x1f;
// Light direction of the shaded builder as the game initialises it
// (settable in percent). The vertex shade is the dot product (z, y then x
// products summed) scaled by default_light_scale (5.0) and truncated.
inline constexpr float default_light_x = -0.8F;
inline constexpr float default_light_y = 1.0F;
inline constexpr float default_light_z = 0.25F;
inline constexpr float default_light_scale = 5.0F;

// `pass` of the image builders: every visible piece, the pieces drawn per
// frame (not cached in the image), or the cached ones.
inline constexpr int32_t pass_all_pieces = -1;
inline constexpr int32_t pass_moving_pieces = 0;
inline constexpr int32_t pass_cached_pieces = 1;

// Remap value of remap_depth_bands: keep the pixel, or make it transparent.
inline constexpr int32_t remap_keep = -1;
inline constexpr int32_t remap_clear = -2;

// Game.graphics_flags bits read here.
inline constexpr uint16_t graphics_anti_alias = 0x2;
inline constexpr uint16_t graphics_shadows = 0x4;
inline constexpr uint16_t graphics_vehicle_shadows = 0x8;
inline constexpr uint16_t graphics_shading = 0x20;

// Unit.state_flags bit of a cloaked unit: the sight gate hides it from other
// players, and it draws translucent here.
inline constexpr uint8_t unit_state_cloaked = 0x04;
// Unit.flags bit set while a unit is carried with no attachment piece
// (attached at piece -1); such a unit stays out of its carrier's image.
inline constexpr uint32_t unit_flag_attached_without_piece = OA_UNIT_FLAG_ATTACHED_WITHOUT_PIECE;

// Width, height and hotspot of a model image.
struct ImageFrame {
    int32_t width{};
    int32_t height{};
    int32_t origin_x{};
    int32_t origin_y{};
};

// Screen extent of a model around its origin; right and bottom are
// exclusive-ish (they include the 2-pixel margin).
struct ModelBounds {
    int32_t left{};
    int32_t right{};
    int32_t top{};
    int32_t bottom{};
};

struct FinerModel;

/// A build effect drawn in place of the one a unit's build progress and
/// pulse give it (apply_build_effect): the image's depths from band_low up
/// to band_end take `band`, those below `below` and those from band_end up
/// `above`, each a colour, remap_keep or remap_clear; then the model is
/// outlined in `outline`.
struct BuildEffectBands {
    uint8_t band_low{};
    uint8_t band_end{};
    int32_t below{remap_keep};
    int32_t band{remap_keep};
    int32_t above{remap_keep};
    uint8_t outline{}; ///< palette entry
};

// Draw state kept with a model instance; the pieces themselves live in
// sim::model_runtime::Instance.
struct ModelState {
    UnitSpriteCache cache{}; // draw count and image presence
    bool transforms_dirty{true};
    present::SpriteBuffer image{}; // cached model image
    // The cached image was built while the unit was unfinished, so it holds
    // every visible piece, the ones drawn per frame among them.
    bool image_unfinished{};
    present::SpriteBuffer shadow{};            // building silhouette (row RLE)
    sim::model_runtime::RotationWords shift{}; // root rotation used by the transforms

    // Piece state seen at the last draw, used to replay the cache resets that
    // piece moves, turns and visibility changes cause.
    struct PieceSnapshot {
        formats::objects3d::FixedVector3 translation{};
        sim::model_runtime::RotationWords rotation{};
        uint16_t flags{};
    };

    std::vector<PieceSnapshot> pieces;
    /// Counts the builds of `image`, so that the image a finer draw keeps
    /// (FinerModel) is built again exactly when this one is.
    uint32_t image_builds{};
    /// The model drawn finer than the game's pixels (enhanced
    /// anti-aliasing); null until it first is.
    std::unique_ptr<FinerModel> finer{};
    /// The build effect the model's image is drawn with while its unit is
    /// unfinished, in place of the one its build progress and pulse give
    /// it: the building being placed (ui.build-preview). Empty draws the
    /// unit's own.
    std::optional<BuildEffectBands> build_bands{};
};

/// A model's draw state for draws finer than the game's pixels (enhanced
/// anti-aliasing): the image and building silhouette of its unit drawn at a
/// number of samples along each axis of a game pixel, built again whenever
/// the game's image is.
struct FinerModel {
    ModelState state{};      ///< holds the finer image and silhouette
    uint32_t samples{};      ///< samples along each axis they are drawn at; 0 before the first
    uint32_t image_builds{}; ///< the ModelState::image_builds of the game's image they follow
};

// One model as the drawing functions see it: the loaded model with texture
// bindings, the transformed pieces, the draw state and the unit that owns it.
struct ModelRef {
    sim::model_runtime::Instance* instance{};
    const PreparedModel* prepared{};
    ModelState* state{};
    const Unit* unit{};
    const UnitDef* def{};
};

// The drawing context: the composite buffer plus the game values drawing
// reads.
struct ModelRenderer {
    present::SpriteBuffer composite{};
    World* world{};
    uint16_t graphics_flags{}; // Game.graphics_flags
    uint32_t tick{};           // Game.tick
    /// Ticks the build effect's pulse lags behind `tick`
    /// (advance_build_pulse); 0, at zoom 1 and in, pulses as 3.1c does.
    uint32_t build_pulse_lag{};
    int32_t camera_x{}; // Game.camera_x, map pixels
    int32_t camera_y{}; // Game.camera_y
    int32_t origin_x{screen_left};
    int32_t origin_y{screen_top};
    uint8_t team_colors[10]{}; // each player's PlayerSetupInfo.color, by player index
    float light[3]{
        default_light_x, default_light_y, default_light_z
    }; // shaded-builder light direction
    float light_scale{default_light_scale}; // vertex shade scale
    void* user{};
    // Terrain height (pixels) under a world point.
    int32_t (*ground_height)(void* user, const FixedVec3& position){};
    // Model of a carried unit; a null instance when it has none. The carried
    // unit draws where the returned ModelRef's record stands, which may be a
    // copy of `unit` placed elsewhere (a draw between two ticks).
    ModelRef (*model_of)(void* user, const Unit& unit){};
    /// Samples along each axis of a game pixel models are drawn at: 1 draws
    /// the game's pixels; more draws every position, size and image that
    /// many times finer, origin_x and origin_y counted in samples too
    /// (enhanced anti-aliasing). Depths stay in game pixels.
    uint32_t samples{1};
    /// The composite's start size, and whether a model larger than it is cut
    /// to it rather than the composite growing: composite_side by
    /// composite_side, growing, as in 3.1c. init_composite_buffer reads it.
    /// The size is in game pixels: drawn at more samples, a model is cut at
    /// the size times the samples.
    data::limits::ModelComposite composite_limits{};
    /// A mobile unit's moving pieces are drawn over its image only once it is
    /// built, as a building's are (ui.interface-fixes nanoframe-raster);
    /// false draws a mobile unit's every frame, built or not, as 3.1c does.
    bool moving_pieces_once_built{};
    /// The table shadows blend through, laid out as the display's alpha
    /// table: null for the display's own, which draws them as the game
    /// does; else a ShadowTable's (shadow_fade.hpp), which draws them
    /// lighter as the view zooms out.
    const uint8_t* shadow_table{};
};

/// What one draw of a unit and its carried units draws with (prepare_linked_draw).
struct LinkedDraw {
    bool drawn{}; ///< false for a carried unit, which its carrier draws
    bool unlit{}; ///< animated textures show their first frame
};

/// Allocates the context's two-plane composite buffer at its start size.
///
/// The size is composite_limits' width by height, 600x600 in 3.1c. Without
/// clamp_oversize the buffer later grows to fit a larger model; with it the
/// buffer keeps its size, a model's per-frame copy is cut to the buffer's
/// width and height from its top left, and a larger image or silhouette that
/// would be built in the buffer is not drawn. A renderer that draws at more
/// samples a game pixel (ModelRenderer::samples) grows its buffer under
/// clamp_oversize to the size times the samples and cuts there, so that it
/// cuts a model where the game's pixels cut it. The composite's key byte is
/// image_key; every image copied in has key 1.
///
/// @param[in,out] renderer drawing context
/// @return false when the buffer cannot be allocated
bool init_composite_buffer(ModelRenderer& renderer);

/// Gives a renderer another's settings, keeping its own composite buffer.
///
/// Every field but the composite is copied, by assigning the renderer with
/// both composites set aside, so that no pixels are copied and a field
/// added later is copied too; a renderer without a composite gets one
/// (init_composite_buffer). A renderer of its own lets another thread draw
/// the same frame's models, since a draw writes its renderer's composite.
///
/// @param[in,out] from the renderer whose settings are copied; its
///     composite is set aside during the copy and put back
/// @param[in,out] to the renderer that takes them
void copy_renderer_settings(ModelRenderer& from, ModelRenderer& to);

/// Sets the light direction of the shaded builder from percentages.
///
/// @param[in,out] renderer drawing context
/// @param x light x in percent
/// @param y light y in percent
/// @param z light z in percent
void set_model_light(ModelRenderer& renderer, int32_t x, int32_t y, int32_t z) noexcept;

/// Allocates a model image without a depth plane, filled with image_key.
///
/// @param[out] image image to replace; empty when a size is negative or too large
/// @param width width in pixels
/// @param height height in rows
void allocate_image(present::SpriteBuffer& image, int32_t width, int32_t height);

/// Allocates a model image filled with image_key and a zeroed depth plane.
///
/// @param[out] image image to replace; empty when a size is negative or too large
/// @param width width in pixels
/// @param height height in rows
void allocate_depth_image(present::SpriteBuffer& image, int32_t width, int32_t height);

/// Measures the screen size and hotspot of a model's visible pieces, with a 2-pixel margin.
///
/// @param instance model instance with transformed pieces
/// @param offset 16.16 offset added to every vertex; null for none
/// @return the image size and hotspot
[[nodiscard]] ImageFrame measure_model_bounds(
    const sim::model_runtime::Instance& instance, const formats::objects3d::FixedVector3* offset
);

/// Grows screen bounds to cover a model's visible pieces, plus a 2-pixel margin.
///
/// @param[in,out] bounds bounds to grow; they always keep the origin
/// @param instance model instance with transformed pieces
/// @param x 16.16 x offset added to every vertex
/// @param y 16.16 y offset added to every vertex
/// @param z 16.16 z offset added to every vertex
void expand_model_bounds(
    ModelBounds& bounds,
    const sim::model_runtime::Instance& instance,
    int32_t x,
    int32_t y,
    int32_t z
);

/// Draws a model: its visible pieces flat when it has no cached image, else as draw_unit_model does.
///
/// @param[in,out] renderer drawing context
/// @param target surface to draw on; null for the locked display surface
/// @param model model to draw
/// @param camera_x camera x in 16.16 map pixels
/// @param camera_z camera z in 16.16 map pixels
/// @param first_frame true to show frame 0 of animated textures
void draw_model(
    ModelRenderer& renderer,
    Surface* target,
    const ModelRef& model,
    int32_t camera_x,
    int32_t camera_z,
    bool first_frame
);

/// A unit carried by a model being drawn, as a planned draw of its carrier
/// (plan_model_draw) finds it.
struct CarriedDraw {
    /// The carried unit's record, as the carrier's draw walks to it.
    const Unit* unit{};
    /// What the renderer's model_of handed back for it.
    ModelRef model{};
    /// Its image as the carrier's draw built it, with the build effect, when
    /// the carrier composes a depth image; empty when it has none.
    present::SpriteBuffer image{};
    /// Its image was built in the renderer's composite, so that the
    /// carrier's composite holds that build's leftovers when the image is
    /// composed into it: a draw builds it again, into scratch of its own.
    bool built_in_composite{};
};

/// What a draw of a model builds on the way and what it decides, worked out
/// once by plan_model_draw, so that draw_planned_model can draw the model
/// as draw_model does without building or changing anything but the
/// renderer's composite and its target. A frame can then be drawn band by
/// band from one plan per model.
struct ModelDrawPlan {
    bool from_image{}; ///< drawn from its cached image (draw_unit_model), else its pieces flat
    int32_t ground{};  ///< the terrain height under the unit, pixels
    /// The units it carries, in the order its draw walks them; their
    /// images when it composes a depth image.
    std::vector<CarriedDraw> carried;
};

/// Builds what draw_model builds of a model on the way, in the same order,
/// and notes what the draw decides, without drawing.
///
/// From a cached image the draw builds a building's silhouette when it has
/// none, and the images of the units it carries when it composes a depth
/// image; those builds change the draw states as draw_model changes them.
/// The renderer's model_of and ground_height are called as the draw calls
/// them.
///
/// @param[in,out] renderer drawing context; its composite is used as scratch
/// @param model model to plan the draw of
/// @param[out] plan what draw_planned_model needs; its buffers are reused
void plan_model_draw(ModelRenderer& renderer, const ModelRef& model, ModelDrawPlan& plan);

/// Draws a model as draw_model does, from what plan_model_draw built and noted.
///
/// Builds nothing, changes no draw state and calls none of the renderer's
/// hooks: only the target and the renderer's composite change, so a model
/// can be drawn from one plan many times, onto bands of one frame.
///
/// @param[in,out] renderer drawing context; its composite is used as scratch
/// @param target surface to draw on; null for the locked display surface
/// @param model the model plan_model_draw planned
/// @param camera_x camera x in 16.16 map pixels
/// @param camera_z camera z in 16.16 map pixels
/// @param first_frame true to show frame 0 of animated textures
/// @param plan the model's plan
void draw_planned_model(
    ModelRenderer& renderer,
    Surface* target,
    const ModelRef& model,
    int32_t camera_x,
    int32_t camera_z,
    bool first_frame,
    const ModelDrawPlan& plan
);

/// Draws one piece straight onto a surface without depth, positioned by its owner unit.
///
/// Coloured primitives are filled and textured quads mapped.
///
/// @param renderer drawing context
/// @param target surface to draw on; null for the locked display surface
/// @param unit unit whose position places the piece
/// @param camera_x camera x in 16.16 map pixels
/// @param camera_z camera z in 16.16 map pixels
/// @param object 3DO object of the piece
/// @param prepared prepared primitives of the object
/// @param piece transformed piece
/// @param owner player index whose team colour picks team textures
/// @param first_frame true to show frame 0 of animated textures
void draw_piece_flat(
    const ModelRenderer& renderer,
    Surface* target,
    const Unit& unit,
    int32_t camera_x,
    int32_t camera_z,
    const formats::objects3d::Object& object,
    const PreparedObject& prepared,
    const sim::model_runtime::PieceState& piece,
    uint8_t owner,
    bool first_frame
);

/// Measures a model and (re)builds its cached image.
///
/// Plain finished units get an image without a depth plane; attached,
/// z-buffered or unfinished units get one with a depth plane. Buildings with
/// shading on use the shaded builder. The state notes whether the unit was
/// unfinished, for draw_linked_model to drop the image once it is finished.
///
/// @param[in,out] renderer drawing context
/// @param model model whose state receives the image
/// @param attached true for a carried unit's image
/// @param pass pieces to draw: pass_all_pieces, pass_moving_pieces or pass_cached_pieces
/// @return false when the image cannot be allocated
bool prepare_model_image(
    ModelRenderer& renderer, const ModelRef& model, bool attached, int32_t pass
);

/// Copies the depth plane of a double-size image, taking every other byte of every other row.
///
/// @param source double-size image with a depth plane
/// @param[in,out] target image whose depth plane is filled; nothing happens
///     unless both have depth planes
void copy_sparse_depth(const Sprite& source, Sprite& target) noexcept;

/// Remaps every opaque pixel of a depth image by its depth band.
///
/// Depths below `threshold` - 4 take `below`, from there up to `threshold`
/// take `band`, and from `threshold` up take `above`. Each value is a colour,
/// remap_keep to leave the pixel or remap_clear to make it transparent.
///
/// @param[in,out] image image with a depth plane
/// @param threshold band top
/// @param above value above the band
/// @param below value below the band
/// @param band value inside the band
void remap_depth_bands(
    Sprite& image, uint8_t threshold, int32_t above, int32_t below, int32_t band
) noexcept;

/// Remaps every opaque pixel of a depth image by where its depth lies:
/// below `low` it takes `below`, from `low` up to `end` `band`, and from
/// `end` up `above`. Each value is a colour, remap_keep or remap_clear.
///
/// @param[in,out] image image with a depth plane
/// @param low the band's lowest depth
/// @param end the depth past the band's highest
/// @param above value above the band
/// @param below value below the band
/// @param band value inside the band
void remap_depth_range(
    Sprite& image, uint8_t low, uint8_t end, int32_t above, int32_t below, int32_t band
) noexcept;

/// The two colours the build effect pulses an unfinished unit with.
struct BuildPulseColours {
    uint8_t first{};  ///< the slower colour, which fills the bands early and late in the build
    uint8_t second{}; ///< the faster colour: the outline, and the band through mid-build
};

/// Returns the build effect's colours for a unit at a tick of its pulse.
///
/// As in 3.1c, each colour walks palette 0xa0..0xaf, bright green to
/// black, and back, one step for each step of its wave: the first wave
/// takes 33 steps every 30 ticks and the second 57, so the first colour
/// comes back to bright green 33 times every 960 ticks (about once a
/// second at 30 ticks a second) and the second 57 times. Each unit starts
/// its waves at its own place, by its id. The colours depend only on the
/// tick and the id, so every frame drawn during a tick shows the same ones,
/// however many frames a second are drawn.
///
/// @param pulse_tick the pulse's tick: Game.tick, less the build effect's lag
///        (ModelRenderer::build_pulse_lag)
/// @param unit_id the unit's id
/// @return the two colours
[[nodiscard]] BuildPulseColours build_pulse_colours(uint32_t pulse_tick, uint32_t unit_id) noexcept;

/// The build effect's own clock: how far its pulse lags behind the game's
/// tick in a view zoomed out (advance_build_pulse).
struct BuildPulseClock {
    bool seen{};             ///< a tick has been seen
    uint32_t last_tick{};    ///< the game's tick last seen
    uint32_t lag{};          ///< ticks the pulse lags behind the game's tick
    uint32_t lag_fraction{}; ///< the part of a tick of lag gathered so far, in 1/65536
};

/// The zoom at and above which the build effect pulses at 3.1c's own rate.
inline constexpr float build_pulse_full_rate_zoom = 1.0F;
/// The slowest the build effect pulses, as a part of 3.1c's rate: the rate
/// of a view zoomed out four times or more.
inline constexpr float build_pulse_least_rate = 0.25F;

/// Returns the share of 3.1c's pulse rate the build effect pulses at, at a zoom.
///
/// At zoom 1 and zoomed in, 1: the pulse is 3.1c's. Zoomed out, the pulse
/// slows with the units' size on screen, at the zoom's own share of the
/// rate (half at zoom 0.5), down to build_pulse_least_rate from zoom 0.25
/// on, so that a view of many small nanoframes does not flicker.
///
/// @param zoom battlefield zoom, screen pixels per map pixel
/// @return the share, from build_pulse_least_rate through 1
[[nodiscard]] float build_pulse_rate(float zoom) noexcept;

/// Moves the build effect's clock on to the game's tick and returns its lag.
///
/// The clock moves with the game's ticks, never with the frames drawn:
/// a frame drawn during the tick last seen leaves it as it is, so the
/// pulse is the same at every frame rate. Each tick since the tick last
/// seen adds to the lag the part of a tick the pulse falls behind at the
/// zoom (1 - build_pulse_rate). At zoom 1 and zoomed in the lag is 0, so
/// the pulse is 3.1c's, tick for tick; coming back to zoom 1 from a
/// zoomed-out view sets it back to 0 at once. A tick before the tick last
/// seen (a game loaded or started again) starts the clock again at 0.
///
/// @param[in,out] clock the build effect's clock
/// @param game_tick Game.tick
/// @param zoom battlefield zoom, screen pixels per map pixel
/// @return the lag, in ticks, for ModelRenderer::build_pulse_lag
uint32_t advance_build_pulse(BuildPulseClock& clock, uint32_t game_tick, float zoom) noexcept;

/// Applies the build (nanoframe) effect to an unfinished unit's depth image.
///
/// A pulsing band of nano colours (palette 0xa0..0xaf, build_pulse_colours)
/// sweeps up the model with the build progress, then the model is outlined;
/// a model whose draw state holds build bands (ModelState::build_bands) is
/// drawn with those instead.
///
/// @param renderer drawing context; its tick less its build_pulse_lag drives the pulse
/// @param[in,out] image model image with a depth plane
/// @param model model of the unit being built
/// @return false when the image has no depth plane or the unit is finished
bool apply_build_effect(const ModelRenderer& renderer, Sprite& image, const ModelRef& model);

/// Outlines every primitive of the visible pieces into a depth image.
///
/// @param[in,out] image model image with a depth plane
/// @param model model to outline
/// @param color outline colour
void outline_model(Sprite& image, const ModelRef& model, uint8_t color);

/// Draws a unit from its cached image.
///
/// Draws the ground shadow, then either the image and the moving pieces flat
/// (images without a depth plane) or a per-frame composite of the image, the
/// moving pieces, carried units, underwater clipping or tinting and digger
/// clipping. Cloaked units, and every unit under the debug overlay, draw
/// translucent. Nothing is drawn without a cached image.
///
/// @param[in,out] renderer drawing context
/// @param target surface to draw on; null for the locked display surface
/// @param model model to draw
/// @param camera_x camera x in 16.16 map pixels
/// @param camera_z camera z in 16.16 map pixels
/// @param first_frame true to show frame 0 of animated textures
void draw_unit_model(
    ModelRenderer& renderer,
    Surface* target,
    const ModelRef& model,
    int32_t camera_x,
    int32_t camera_z,
    bool first_frame
);

/// Builds a model image with flat texturing.
///
/// Anti-aliased buildings are drawn double size into the composite and then
/// downsampled through the alpha table (except for pass 0).
///
/// @param[in,out] renderer drawing context
/// @param[in,out] image image to draw into
/// @param model model to draw
/// @param owner player index whose team colour picks team textures
/// @param pass pieces to draw: pass_all_pieces, pass_moving_pieces or
///     pass_cached_pieces; an unfinished unit draws every visible piece
void build_model_image(
    ModelRenderer& renderer, Sprite& image, const ModelRef& model, uint8_t owner, int32_t pass
);

/// Builds a model image with per-vertex lighting from averaged face normals.
///
/// Pieces flagged shaded get a shade row per vertex from the light
/// direction; others use unlit_shade. Otherwise as build_model_image.
///
/// @param[in,out] renderer drawing context
/// @param[in,out] image image to draw into
/// @param model model to draw
/// @param owner player index whose team colour picks team textures
/// @param pass pieces to draw, as build_model_image takes them
void build_shaded_model_image(
    ModelRenderer& renderer, Sprite& image, const ModelRef& model, uint8_t owner, int32_t pass
);

/// Copies a model image into the composite buffer as a colour-0 silhouette.
///
/// @param[in,out] renderer drawing context whose composite receives the copy
/// @param image model image
/// @return the composite sprite (left as it was when the image does not fit)
Sprite& copy_silhouette(ModelRenderer& renderer, const Sprite& image);

/// Measures the screen size and hotspot of a model's sheared ground silhouette.
///
/// @param instance model instance with transformed pieces
/// @return the silhouette size and hotspot, with a 2-pixel margin
[[nodiscard]] ImageFrame measure_shadow_bounds(const sim::model_runtime::Instance& instance);

/// Fills the sheared silhouette of the cached visible pieces with colour 0.
///
/// @param[in,out] target sprite with a depth plane
/// @param model model whose pieces cast the silhouette
void draw_shadow_silhouette(Sprite& target, const ModelRef& model);

/// Builds a building's cached silhouette as a row-RLE sprite.
///
/// The sheared pieces are drawn in the composite, the model's own image
/// (offset by shadow_offset_x) is stamped out, and the result is encoded.
///
/// @param[in,out] renderer drawing context whose composite is used as scratch
/// @param model model whose state receives the silhouette
/// @param image the model's cached image
/// @return false when the silhouette does not fit or cannot be allocated
bool build_shadow_image(ModelRenderer& renderer, const ModelRef& model, const Sprite& image);

/// The ground shadow a model drawn from its cached image casts, as the game
/// draws it: the pixels it darkens and where they lie, in game pixels.
struct GroundShadow {
    /// The shadow's left column and top row, in game pixels from the
    /// camera's corner of the battlefield; the renderer's origin is not
    /// added.
    int32_t left{};
    int32_t top{};
    int32_t width{};
    int32_t height{};
    /// width by height bytes, rows top to bottom: 1 where the shadow darkens
    /// what is under it, 0 elsewhere.
    std::vector<uint8_t> coverage;
};

/// Works out the ground shadow draw_model casts for a model drawn from its
/// cached image at one sample a game pixel, without drawing it.
///
/// A building casts its sheared shadow image (build_shadow_image); any
/// other model with shadows on casts its image's silhouette, without the
/// part a digger has below the ground and, where the image has a depth
/// plane, without the part under the water. The shadow lies shadow_offset_x
/// right of the model, at the ground's height under it. The model's images
/// are read as its last plan or draw left them; nothing is built.
///
/// @param graphics_flags Game.graphics_flags as the draw reads them: shadows
///     and vehicle shadows
/// @param game the match's game record: its sea level
/// @param model the model
/// @param camera_x camera x in 16.16 map pixels
/// @param camera_z camera z in 16.16 map pixels
/// @param ground the terrain height under the unit, pixels (ModelDrawPlan::ground)
/// @param[out] shadow the shadow; its coverage keeps its memory from call to call
/// @return false where the draw casts none: shadows off, a type that casts
///     none, a vehicle without vehicle shadows, a building's image not built,
///     or a sunken feature's building shadow
bool ground_shadow(
    uint16_t graphics_flags,
    const Game& game,
    const ModelRef& model,
    int32_t camera_x,
    int32_t camera_z,
    int32_t ground,
    GroundShadow& shadow
);

/// Stores a new root shift when any rotation word moved by at least shift_threshold.
///
/// The transforms are marked dirty and the root piece's transform is reset;
/// a cached root piece also resets the image's draw count.
///
/// @param model model whose state is updated
/// @param rotation owner's rotation words
void set_model_shift(const ModelRef& model, sim::model_runtime::RotationWords rotation);

/// Updates the root shift from the owner's bank, heading and pitch and redoes the piece transforms when dirty.
///
/// @param model model to update
void update_model_transforms(const ModelRef& model);

/// Updates the transforms of a unit and its carried units, as draw_linked_model does first.
///
/// Only the transforms and the root shift change (update_model_transforms);
/// the image cache is left for the next draw_linked_model.
///
/// @param renderer drawing context; its model_of finds the carried units
/// @param model the carrying unit's model
void update_linked_transforms(const ModelRenderer& renderer, const ModelRef& model);

/// Replays the cache resets of the piece setters by comparing each piece with its state at the last draw.
///
/// model_runtime does not report piece changes, so each piece is compared
/// with its state at the last draw: a cached piece that moved, turned or
/// changed visibility resets the draw count, and a changed cache flag drops
/// the image.
///
/// @param model model whose state is updated
void note_piece_changes(const ModelRef& model);

/// Readies a unit for a draw, as draw_linked_model does before drawing.
///
/// Updates the transforms of the unit and its carried units
/// (update_linked_transforms), counts the draw in the image cache and builds
/// the cached image again when the cache asks for it, dropping an image
/// built while the unit was unfinished once it is finished. A carried unit
/// is left as it is: its carrier draws it.
///
/// @param[in,out] renderer drawing context
/// @param model model to draw
/// @param movement_idle the movement object's idle flag, for mobile units
/// @return whether the unit draws, and whether unlit
LinkedDraw prepare_linked_draw(ModelRenderer& renderer, const ModelRef& model, bool movement_idle);

/// Draws a unit and its carried units.
///
/// Updates the transforms of the unit and its carried units
/// (update_linked_transforms), refreshes the image cache and draws the model
/// at the renderer's camera. Carried units are drawn by their carrier, so a
/// carried unit draws nothing here.
///
/// Finishing a build drops the unit's cached image: an image built while the
/// unit was unfinished holds every piece, and once the unit is finished the
/// pieces its script turns and moves are drawn over the image each frame. So
/// such an image goes before the image cache is refreshed, and a building
/// builds its image again, of its cached pieces alone.
///
/// @param[in,out] renderer drawing context
/// @param target surface to draw on; null for the locked display surface
/// @param model model to draw
/// @param movement_idle the movement object's idle flag, for mobile units
void draw_linked_model(
    ModelRenderer& renderer, Surface* target, const ModelRef& model, bool movement_idle
);

/// Draws a debris piece flat when its world origin lies in a view rectangle.
///
/// @param renderer drawing context
/// @param target surface to draw on; null for the locked display surface
/// @param view screen rectangle the origin must lie in
/// @param object 3DO object of the piece
/// @param prepared prepared primitives of the object
/// @param points one 16.16 point per vertex, relative to the origin
/// @param origin 16.16 world origin of the piece
/// @param team_color frame index of team textures
/// @quirk Each vertex is lifted by half of (vertex y + origin y), with the
///     origin's world y rather than its computed screen y.
void draw_debris_piece(
    const ModelRenderer& renderer,
    Surface* target,
    const Rect32& view,
    const formats::objects3d::Object& object,
    const PreparedObject& prepared,
    const formats::objects3d::FixedVector3* points,
    const formats::objects3d::FixedVector3& origin,
    uint8_t team_color
);

/// Rotates a debris piece's object into a point buffer and draws it as draw_debris_piece does.
///
/// @param renderer drawing context
/// @param target surface to draw on; null for the locked display surface
/// @param view screen rectangle the origin must lie in
/// @param object 3DO object of the piece; its vertices are negated in x and z first
/// @param prepared prepared primitives of the object
/// @param rotation rotation words of the piece
/// @param origin 16.16 world origin of the piece
/// @param team_color frame index of team textures
/// @param[out] points scratch buffer; resized to the vertex count
void draw_rotated_debris(
    const ModelRenderer& renderer,
    Surface* target,
    const Rect32& view,
    const formats::objects3d::Object& object,
    const PreparedObject& prepared,
    sim::model_runtime::RotationWords rotation,
    const formats::objects3d::FixedVector3& origin,
    uint8_t team_color,
    std::vector<formats::objects3d::FixedVector3>& points
);

/// Draws one 3DO object flat at a camera-relative position.
///
/// @param renderer drawing context
/// @param target surface to draw on; null for the locked display surface
/// @param position 16.16 camera-relative position
/// @param points one 16.16 point per vertex on the loaded (negated x/z) axes
/// @param object 3DO object
/// @param prepared prepared primitives; animated textures show their running frame
/// @param rotation rotation words applied to every point
void draw_rotated_object(
    const ModelRenderer& renderer,
    Surface* target,
    const formats::objects3d::FixedVector3& position,
    const formats::objects3d::FixedVector3* points,
    const formats::objects3d::Object& object,
    const PreparedObject& prepared,
    sim::model_runtime::RotationWords rotation
);

/// Draws a projectile's object as draw_rotated_object does, its file vertices put on the loaded axes.
///
/// @param renderer drawing context
/// @param target surface to draw on; null for the locked display surface
/// @param position 16.16 camera-relative position
/// @param object 3DO object of the projectile
/// @param prepared prepared primitives of the object
/// @param rotation rotation words of the projectile
void draw_projectile_model(
    const ModelRenderer& renderer,
    Surface* target,
    const formats::objects3d::FixedVector3& position,
    const formats::objects3d::Object& object,
    const PreparedObject& prepared,
    sim::model_runtime::RotationWords rotation
);

/// Draws a shatter fragment as draw_rotated_object does.
///
/// The fragment's eight arena points are turned by `spin`, and its six faces
/// carry `source` (the loaded primitive it broke from) with the fragment's
/// flag word and colour. A team texture shows the owner's frame; an animated
/// one shows the running frame, where in 3.1c it stays on the frame it showed
/// when the fragment broke off.
///
/// @param renderer drawing context
/// @param target surface to draw on; null for the locked display surface
/// @param position 16.16 camera-relative position
/// @param fragment shatter fragment record
/// @param source prepared primitive the fragment broke from
/// @param spin rotation words of the fragment
void draw_shatter_fragment(
    const ModelRenderer& renderer,
    Surface* target,
    const formats::objects3d::FixedVector3& position,
    const sim::effect_particles::ShatterFragment& fragment,
    const PreparedPrimitive& source,
    sim::model_runtime::RotationWords spin
);

} // namespace oa::present::model
