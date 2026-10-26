// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battlefield's draws of one frame, in the order they draw, so that the
// frame can be drawn in horizontal bands. The drawing thread works the list
// out first (Runtime::render_match_surface): everything drawing builds or
// changes on the way is done then, once, in the draws' order: the piece
// transforms, the units' cached images and silhouettes, the texture
// animations, the projectiles' models, the GAF frames decoded for the
// particles. Each band then draws the whole list with its own rows of the
// frame alone (draw_world_band), reading the list and the models and
// writing only its rows of the frame and of the model bridge, so that the
// bands give the frame drawn whole byte for byte, one after another or at
// the same time. A projectile's lens reads the frame around it, rows of
// other bands among them, so a frame with one is drawn as a single band. A
// large explosion file is drawn a frame at a time: each of
// its frames is rendered from the file's bytes as it is drawn, through a
// cache of rendered frames with a budget of bytes (GafFrameCache).
#pragma once

#include "oa/core/world.h"
#include "oa/formats/gaf.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/objects3d.hpp"
#include "oa/present/model/model_draw.hpp"
#include "oa/present/model/model_library.hpp"
#include "oa/present/model/rgb_bridge.hpp"
#include "oa/present/model/shadow_fade.hpp"
#include "oa/present/model/unit_supersampling.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/present/world_renderer.hpp"
#include "oa/sim/effect_particles.hpp"
#include "oa/sim/model_runtime/instance.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oa::app {

/// The frame a draw of the battlefield writes, and what it may write: the
/// RGB battlefield, the visible world rectangle every draw clips to, and the
/// rows of the band being drawn.
struct WorldTarget {
    uint8_t* rgb{};        ///< packed RGB rows of `width` pixels
    int32_t width{};       ///< pixels in a row
    int32_t height{};      ///< rows
    int32_t clip_x{};      ///< the visible world rectangle's left column
    int32_t clip_y{};      ///< its top row
    int32_t clip_width{};  ///< its columns
    int32_t clip_height{}; ///< its rows
    /// The first row the draw may change. A line or sprite over the band's
    /// edge works out its pixels as over the whole frame and writes only
    /// those in the band.
    int32_t first_row{};
    int32_t end_row{}; ///< the row after the last one the draw may change
    /// How far into its map pixel the frame's first column starts, in 16.16
    /// parts of the scene's step (oa/present/scene_grid.hpp's phase), which
    /// the sprites are laid on the scene grid with, as the terrain is.
    uint32_t phase_x{};
    uint32_t phase_y{}; ///< the same for the frame's first row
};

/// Writes one pixel of the battlefield frame, clipped to the visible world
/// rectangle, the frame and the band.
///
/// @param target the frame and what may be written
/// @param x frame column
/// @param y frame row
/// @param color RGB colour
void put_world_pixel(
    const WorldTarget& target, int x, int y, const std::array<uint8_t, 3>& color
) noexcept;

/// Draws a line on the battlefield frame, clipped to the visible world
/// rectangle first; only its pixels in the band are written.
///
/// Clipping before stepping keeps off-map endpoints (a build ghost under an
/// off-map cursor, say) from walking billions of steps or overflowing.
///
/// @param target the frame and what may be written
/// @param x0 first endpoint column
/// @param y0 first endpoint row
/// @param x1 second endpoint column
/// @param y1 second endpoint row
/// @param color RGB colour
void draw_world_line(
    const WorldTarget& target, int x0, int y0, int x1, int y1, const std::array<uint8_t, 3>& color
) noexcept;

/// Blits the covered pixels of a rendered GAF frame onto the battlefield frame, scaled nearest-pixel.
///
/// @param target the frame and what may be written
/// @param frame rendered frame
/// @param destination_x frame column of the frame's left edge
/// @param destination_y frame row of the frame's top edge
/// @param palette 4 bytes per colour
/// @param scale size factor; 0 or less draws at 1
/// @param shadow_level how much of the frame's colour each covered pixel
///     takes, in steps of 1 / oa::present::model::shadow_full_level
///     (oa::present::model::fade_shadow_channel): the frame's colour
///     alone at shadow_full_level, as every frame but a faded shadow draws
void blit_world_frame(
    const WorldTarget& target,
    const oa::formats::gaf::RenderedFrame& frame,
    int destination_x,
    int destination_y,
    const oa::PaletteBytes& palette,
    float scale,
    uint32_t shadow_level = oa::present::model::shadow_full_level
) noexcept;

/// Blits a rendered GAF frame onto the battlefield frame with its origin at a screen point.
///
/// @param target the frame and what may be written
/// @param frame rendered frame
/// @param screen frame point the GAF origin lands on
/// @param palette 4 bytes per colour
/// @param scale size factor; 0 or less draws at 1
/// @param shadow_level as blit_world_frame's
void blit_world_hotspot(
    const WorldTarget& target,
    const oa::formats::gaf::RenderedFrame& frame,
    const oa::present::world_renderer::ScreenPoint& screen,
    const oa::PaletteBytes& palette,
    float scale,
    uint32_t shadow_level = oa::present::model::shadow_full_level
) noexcept;

/// Blits a rendered GAF frame standing on the battlefield onto the scene,
/// its origin at a place on the map, laid on the scene grid.
///
/// The frame's pixel (i, j) lies on the map pixel `place - origin + (i, j)`
/// from the scene's first, and covers the scene pixels the grid gives that
/// map pixel at the scale (oa/present/scene_grid.hpp), the pixels the
/// terrain of that map pixel fills: what the frame shows stays over the
/// same ground at every scale and from every camera. At a scale of 1 the
/// frame is copied with its origin at the place.
///
/// @param target the frame and what may be written
/// @param frame rendered frame
/// @param place where the GAF origin lies, in map pixels from the scene's
///     first, raised by its height (place_world_point)
/// @param palette 4 bytes per colour
/// @param scale scene pixels per map pixel; 0 or less draws at 1
/// @param shadow_level as blit_world_frame's
void blit_world_sprite(
    const WorldTarget& target,
    const oa::formats::gaf::RenderedFrame& frame,
    const oa::present::world_renderer::ScreenPoint& place,
    const oa::PaletteBytes& palette,
    float scale,
    uint32_t shadow_level = oa::present::model::shadow_full_level
) noexcept;

/// Blends a rendered GAF frame's covered pixels into the battlefield frame
/// through the display's alpha table, its origin at a place on the map,
/// laid on the scene grid as blit_world_sprite lays it.
///
/// Each covered pixel takes table[source * 256 + destination], the
/// destination's palette index read back from the frame through the model
/// bridge's colour lookup, mixed with the frame's colour under it by a
/// shadow level. Nothing is drawn without an alpha table.
///
/// @param target the frame and what may be written
/// @param frame rendered frame
/// @param place where the GAF origin lies, in map pixels from the scene's first
/// @param scale scene pixels per map pixel; 0 or less draws at 1
/// @param display the models' display: its alpha table and palette
/// @param[in,out] bridge the frame's model bridge, whose colour lookup remembers nearest entries
/// @param[in,out] band the band being drawn, whose own colour memory is used
///     when it has one; null for the bridge's
/// @param shadow_level how much of the blend each covered pixel takes, as
///     blit_world_frame's: the blend alone at shadow_full_level
void blit_world_blended_hotspot(
    const WorldTarget& target,
    const oa::formats::gaf::RenderedFrame& frame,
    const oa::present::world_renderer::ScreenPoint& place,
    float scale,
    const oa::present::model::ModelDisplay& display,
    oa::present::model::RgbBridge& bridge,
    oa::present::model::BridgeBand* band,
    uint32_t shadow_level = oa::present::model::shadow_full_level
);

/// How strongly a frame's explosion flashes light what is under them.
enum class FlashStrength : uint8_t {
    /// Each pixel lights through the light table's row its value names, as
    /// 3.1c draws it: about twice as bright at the centre.
    full,
    /// Through half that row, rounded down: about one and a half times as
    /// bright at the centre.
    reduced,
};

/// Lights the pixels under a rendered explosion flash frame through the
/// light table, its origin at a place on the map, laid on the scene grid as
/// blit_world_sprite lays it.
///
/// Each covered pixel of the frame but its transparent index names a row of
/// the table: its value less oa::present::shade_ramp_base, halved at
/// FlashStrength::reduced. The pixel under it, its palette index read back
/// from the frame through the model bridge's colour lookup, becomes
/// table[row * 256 + under], so a flash drawn over another lights what that
/// one lit. A value whose row lies outside the table's
/// oa::present::ramp_table_rows draws nothing.
///
/// @param target the frame and what may be written
/// @param frame rendered flash frame
/// @param place where the frame's origin lies, in map pixels from the scene's first
/// @param scale scene pixels per map pixel; 0 or less draws at 1
/// @param light_table oa::present::ramp_table_rows rows of 256 palette
///     indices; null draws nothing
/// @param display the models' display: its palette
/// @param[in,out] bridge the frame's model bridge, whose colour lookup remembers nearest entries
/// @param[in,out] band the band being drawn, whose own colour memory is used
///     when it has one; null for the bridge's
/// @param strength how strongly the flash lights
void blit_world_lit_hotspot(
    const WorldTarget& target,
    const oa::formats::gaf::RenderedFrame& frame,
    const oa::present::world_renderer::ScreenPoint& place,
    float scale,
    const uint8_t* light_table,
    const oa::present::model::ModelDisplay& display,
    oa::present::model::RgbBridge& bridge,
    oa::present::model::BridgeBand* band,
    FlashStrength strength
);

/// Side of the square a projectile of render type 2 draws its lens over, in
/// map pixels; its centre is the square's middle.
inline constexpr int32_t projectile_lens_side = 22;
/// How strongly that lens magnifies: oa::present::build_lens_frame's scale.
inline constexpr int32_t projectile_lens_strength = 8;

/// How a point's height raises it on the battlefield frame.
enum class HeightLift : uint8_t {
    /// Half its height, rounded to the nearest pixel, halves away from zero.
    nearest,
    /// Half its whole map pixels of height, rounded down; at a scale, that
    /// times the scale, rounded. The game raises an explosion record's
    /// flash and sprite and a projectile's lens so, as it tests them
    /// against the battlefield (draw_explosions,
    /// projectile_lens_on_battlefield).
    down,
};

/// Projects a 16.16 world position onto the battlefield frame, raised by
/// its height as `lift` says.
///
/// x and z are signed, so a point left of or above the map lands left of or
/// above the map's edge on the frame. An odd height puts a point raised to
/// the nearest pixel one row above one raised down.
///
/// @param viewport battlefield viewport
/// @param position 16.16 x, height and z
/// @param lift how the height raises the point
/// @return frame point
[[nodiscard]] oa::present::world_renderer::ScreenPoint project_world_point(
    const oa::present::world_renderer::BattlefieldViewport& viewport,
    const std::array<uint32_t, 3>& position,
    HeightLift lift
) noexcept;

/// Returns where a 16.16 world position lies on the battlefield as the game
/// draws it unscaled: its whole map pixels from the viewport's camera,
/// raised by its height as `lift` says, from the viewport's corner, whatever
/// the viewport's scale. A sprite drawn there (blit_world_sprite) is laid on
/// a scene at any scale over the ground it stands on.
///
/// @param viewport battlefield viewport; its scale is not used
/// @param position 16.16 x, height and z
/// @param lift how the height raises the point
/// @return the place, in map pixels from the viewport's corner
[[nodiscard]] oa::present::world_renderer::ScreenPoint place_world_point(
    const oa::present::world_renderer::BattlefieldViewport& viewport,
    const std::array<uint32_t, 3>& position,
    HeightLift lift
) noexcept;

/// Tells whether a projectile's lens is drawn: whether the projectile's
/// position, placed on the game's screen as an explosion record's centre is
/// (draw_explosions), lies on the battlefield rectangle, its edges included.
///
/// The screen point is the position's whole map pixels less the camera's,
/// lifted by half its height rounded down, from the battlefield's corner
/// (battlefield_screen_x, battlefield_screen_y); the map pixels, the height
/// and the camera are each taken as signed 16-bit values. The lens is drawn
/// at that point (project_world_point with HeightLift::down).
///
/// @param view camera position and battlefield rectangle
/// @param position the projectile's 16.16 world position, x, height and z
/// @return true when its lens is drawn
[[nodiscard]] bool projectile_lens_on_battlefield(
    const oa::sim::effect_particles::ExplosionView& view, const std::array<uint32_t, 3>& position
) noexcept;

/// Draws a projectile's lens on the battlefield frame, its centre at a
/// screen point.
///
/// The lens covers a square of projectile_lens_side map pixels, placed as a
/// GAF frame of that side whose origin is its middle would be. Only the
/// five by five map pixels at its middle change: each takes the pixel the
/// lens table names (oa::present::build_lens_frame), one step nearer the
/// centre in each direction it lies off it, read from the frame as it
/// stood before the lens. The centre pixel grows to a block of three by
/// three, and the eight around it move out by one to frame it, those beside
/// it stretched to three. A lens drawn over another reads what that one
/// drew. At a scale each map pixel is a block of the frame, found as a GAF
/// frame's pixels are, and moves by its step times the scale, rounded.
///
/// The lens reads rows outside the band being drawn, so a frame with a lens
/// is drawn as one band (draw_world_bands).
///
/// @param target the frame and what may be written
/// @param screen frame point the lens's centre lands on
/// @param scale size factor; 0 or less draws at 1
void draw_world_lens(
    const WorldTarget& target, const oa::present::world_renderer::ScreenPoint& screen, float scale
);

/// What one draw of the battlefield is; WorldDraw::index names it in the
/// list of its kind.
enum class WorldDrawKind : uint8_t {
    commit,         ///< the bridge's draws go to the frame, when it holds any
    commit_always,  ///< the bridge's captured tiles go to the frame
    pixel_square,   ///< a square of one colour (WorldDrawList::squares)
    sprite,         ///< a GAF frame in the palette's colours (WorldDrawList::sprites)
    blended_sprite, ///< a GAF frame blended through the alpha table (sprites)
    /// An explosion's flash, lighting what is under it through the light
    /// table (sprites; blit_world_lit_hotspot).
    lit_sprite,
    line,           ///< a line of one colour (WorldDrawList::lines)
    selection_line, ///< a selection box's line through the bridge (selection_lines)
    model,          ///< a unit or a 3D feature (WorldDrawList::models)
    projectile,     ///< a projectile's 3DO object (WorldDrawList::projectiles)
    debris,         ///< a debris piece (WorldDrawList::debris)
    fragment,       ///< a shatter fragment (WorldDrawList::fragments)
    lens,           ///< a projectile's lens (WorldDrawList::lenses; draw_world_lens)
};

/// One draw of the battlefield.
struct WorldDraw {
    WorldDrawKind kind{};
    uint32_t index{}; ///< its place in its kind's list; unused for the commits
};

/// A particle's square, already clipped to the frame: the rows from `top` up
/// to `bottom` and the columns from `left` up to `right`.
struct SquareDraw {
    int32_t left{};
    int32_t top{};
    int32_t right{};
    int32_t bottom{};
    std::array<uint8_t, 3> color{};
};

/// A GAF frame drawn at a place on the battlefield.
struct SpriteDraw {
    /// The rendered frame. Its width, height and origin are the ones
    /// `render_normal` produced, so a layer that reaches past the frame
    /// record is drawn with that canvas, not with the record's box.
    const oa::formats::gaf::RenderedFrame* frame{};
    /// Where the frame's origin lies, in map pixels from the scene's first,
    /// raised by its height (place_world_point): the point the game's
    /// unscaled view draws it at, which the scene's scale lays on the scene
    /// grid (blit_world_sprite).
    oa::present::world_renderer::ScreenPoint place{};
    /// A feature's shadow frame, drawn as dark as the list's shadow_level.
    bool shadow{};
    /// A feature's frame, cut off where the map the view shows ends
    /// (WorldFrameDraw::shown_map), so that nothing of it lies on the black
    /// past the map's edges.
    bool on_map{};
    /// A low feature or a shadow lies on terrain in the spatial renderer;
    /// standing features keep their upright cutout.
    bool terrain_decal{};
    /// Canonical feature or interpolated effect origin, retained for spatial cameras.
    std::optional<oa::FixedVec3> world_position{};
    /// An explosion sprite may need clearance above the rendered ground at impact.
    bool surface_effect{};
    /// Presentation-only size and opacity for compact rapid-fire effects.
    float effect_scale{1};
    float effect_opacity{1};
    /// Filter only effect art; scenery keeps its original pixel sampling.
    bool soft_effect{};
};

/// A rectangle of scene pixels: the columns from `left` up to `right` and
/// the rows from `top` up to `bottom`.
struct SceneRect {
    int64_t left{};
    int64_t top{};
    int64_t right{};
    int64_t bottom{};
};

/// Returns the scene pixels a sprite's frame covers at a scale, laid on the
/// scene grid as blit_world_sprite lays it; unclipped.
///
/// @param sprite the sprite; its frame must be set
/// @param scale scene pixels per map pixel; 0 or less is 1
/// @param phase_x the scene's phase across (WorldTarget::phase_x)
/// @param phase_y the scene's phase down
/// @return the rectangle
[[nodiscard]] SceneRect sprite_scene_rect(
    const SpriteDraw& sprite, float scale, uint32_t phase_x = 0, uint32_t phase_y = 0
) noexcept;

/// A line of the frame (WorldDrawKind::line) in RGB, or of the bridge
/// (WorldDrawKind::selection_line) in a palette index, in its pixels.
struct LineDraw {
    int32_t x0{};
    int32_t y0{};
    int32_t x1{};
    int32_t y1{};
    std::array<uint8_t, 3> color{}; ///< the RGB of a frame's line
    uint8_t palette_index{};        ///< the palette index of a bridge's line
};

/// A spatial particle retains its interpolated world position for alternate cameras.
struct SpatialParticle {
    oa::FixedVec3 position{};
    std::array<uint8_t, 3> color{};
    bool emissive{};
};

/// A unit or a 3D feature, as plan_unit_supersampled planned it.
struct ModelDraw {
    oa::present::model::ModelRef model{};
    /// A 3D feature draws through a stand-in unit record
    /// (WorldDrawList::stand_ins), whose place is kept here: model.unit is
    /// set to it when the feature draws. -1 for a unit.
    int32_t stand_in{-1};
    oa::present::model::SupersampledUnitPlan plan{};
    /// A 3D feature, cut off where the map the view shows ends
    /// (WorldDrawList::shown_map_width): its plan's region lies within the
    /// map already, and the Full tier's card clips it there.
    bool on_map{};
    /// Presentation transforms captured before deferred drawing can rebuild them.
    std::optional<oa::FixedVec3> shown_position{};
    std::vector<std::vector<oa::formats::objects3d::FixedVector3>> shown_vertices{};
};

/// A projectile drawn as a 3DO object (render types 1, 3 and 6): its ground
/// shadow, its model and, for a missile with flight time left, its first
/// child.
struct ProjectileDraw {
    /// The bridge region captured, in its 8-bit pixels. Rect32 is packed, so
    /// each draw's region is aligned: the bridge takes its fields by reference.
    alignas(4) oa::Rect32 region{};
    bool shadow{};                                   ///< the shadow sprite is drawn
    int32_t x{};                                     ///< the shadow's column
    int32_t shadow_y{};                              ///< the shadow's row
    oa::formats::objects3d::FixedVector3 position{}; ///< 16.16, camera-relative
    const oa::formats::objects3d::Object* object{};
    const oa::present::model::PreparedObject* prepared{};
    oa::sim::model_runtime::RotationWords rotation{};
    const oa::formats::objects3d::Object* child{}; ///< null for none
    const oa::present::model::PreparedObject* child_prepared{};
    oa::sim::model_runtime::RotationWords child_rotation{};
};

/// A debris piece, turned by its spin and drawn at its origin.
struct DebrisDraw {
    alignas(4) oa::Rect32 region{}; ///< the bridge region captured, in its 8-bit pixels
    const oa::formats::objects3d::Object* object{};
    const oa::present::model::PreparedObject* prepared{};
    oa::sim::model_runtime::RotationWords spin{};
    oa::formats::objects3d::FixedVector3 origin{}; ///< 16.16 world origin
    uint8_t team{};                                ///< its unit's team colour
};

/// A shatter fragment, drawn into the bridge and written back at once.
struct FragmentDraw {
    alignas(4) oa::Rect32 region{}; ///< the bridge region captured, in its 8-bit pixels
    oa::formats::objects3d::FixedVector3 position{}; ///< 16.16, camera-relative
    const oa::sim::effect_particles::ShatterFragment* fragment{};
    const oa::present::model::PreparedPrimitive* primitive{};
    oa::sim::model_runtime::RotationWords spin{};
};

/// Bytes of pixels and coverage of decoded GAF frames a draw list keeps from
/// one frame's draws to the next (WorldDrawList::kept_decoded): 32 MiB, many
/// times what the frames of FX.GAF take.
inline constexpr std::size_t decoded_frames_kept_bytes = 32U * 1024U * 1024U;

/// The battlefield's draws of one frame, in order, and what they draw from.
struct WorldDrawList {
    /// How dark the frame's shadows are drawn, from 0 to
    /// oa::present::model::shadow_full_level, as the game draws them; at 0
    /// the list holds no shadow (set_frame_shadows).
    uint32_t shadow_level{oa::present::model::shadow_full_level};
    /// How strongly the frame's flashes (WorldDrawKind::lit_sprite) light
    /// what is under them. Like the shadow level, clear_world_draws leaves it.
    FlashStrength flash_strength{FlashStrength::full};
    /// The map the view shows, in map pixels across and down from its
    /// top-left corner, where the features' draws (SpriteDraw::on_map,
    /// ModelDraw::on_map) are cut off. Like the shadow level,
    /// clear_world_draws leaves it.
    int32_t shown_map_width{};
    int32_t shown_map_height{}; ///< map pixels down

    std::vector<WorldDraw> draws; ///< in the order they draw
    std::vector<SquareDraw> squares;
    std::vector<SpriteDraw> sprites;
    std::vector<LineDraw> lines;
    std::vector<ModelDraw> models;
    std::vector<SpatialParticle> spatial_particles;
    std::vector<oa::Unit> stand_ins; ///< 3D features' stand-in records
    std::vector<ProjectileDraw> projectiles;
    std::vector<DebrisDraw> debris;
    std::vector<FragmentDraw> fragments;
    /// The centres of the projectiles' lenses, as frame points.
    std::vector<oa::present::world_renderer::ScreenPoint> lenses;
    /// GAF frames decoded for the draws (particles, plasma and flames), each
    /// once, kept from one frame's draws to the next by the GAF frame's
    /// address; empty where the frame cannot be decoded. A map's elements
    /// stay where they are, so that the draws keep pointing at them.
    /// clear_world_draws lets them all go once they take more than
    /// decoded_frames_kept_bytes; the list itself goes with the match whose
    /// archives the addresses name.
    std::unordered_map<
        const oa::formats::gaf::Frame*,
        std::optional<oa::formats::gaf::RenderedFrame>>
        kept_decoded{};
    /// Bytes of pixels and coverage the frames of kept_decoded take.
    std::size_t kept_decoded_bytes{};
    /// The decoded frame of each GAF frame this frame's draws use, by the
    /// GAF frame's address; null for a frame that cannot be decoded.
    std::unordered_map<const oa::formats::gaf::Frame*, const oa::formats::gaf::RenderedFrame*>
        decoded_of;
    /// Frames of files drawn a frame at a time that this frame's draws use
    /// (ranged_frame), held so that the cache may let them go while the
    /// frame is drawn.
    std::vector<std::shared_ptr<const oa::formats::gaf::RenderedFrame>> held;
};

/// Empties a draw list for the next frame, keeping its buffers.
///
/// The shadow level, the flash strength and the shown map are left as they
/// are, and so are the decoded GAF frames (WorldDrawList::kept_decoded)
/// while they take no more than decoded_frames_kept_bytes.
///
/// @param[in,out] list the list
void clear_world_draws(WorldDrawList& list);

/// Sets how dark a frame's shadows are drawn, from the zoom the frame is
/// drawn at (oa::present::model::shadow_level), before the frame's draws
/// are worked out.
///
/// The list takes the level. At the game's own darkness the renderer's
/// shadows blend through the display's alpha table, as the game draws
/// them; lighter, through the table's faded rows for the models'
/// silhouettes and the projectiles' shadow sprite; at 0 the renderer's
/// shadow option is cleared for the frame, so that no unit or 3D feature
/// casts one, and the planner adds no shadow of a sprite feature or a
/// projectile (shadows_drawn).
///
/// @param[in,out] list the frame's list
/// @param[in,out] renderer the models' renderer, its graphics flags already the game's
/// @param[in,out] table the faded table, kept from frame to frame
/// @param display the models' display: its alpha table and palette
/// @param projectile_shadow the projectiles' shadow sprite; null or no data for none
/// @param zoom window pixels per map pixel
void set_frame_shadows(
    WorldDrawList& list,
    oa::present::model::ModelRenderer& renderer,
    oa::present::model::ShadowTable& table,
    const oa::present::model::ModelDisplay& display,
    const oa::Sprite* projectile_shadow,
    float zoom
);

/// Tells whether a list's frame draws any shadow.
///
/// @param list the list
/// @return false at shadow level 0
[[nodiscard]] inline bool shadows_drawn(const WorldDrawList& list) noexcept {
    return list.shadow_level != 0;
}

/// Returns a GAF frame decoded for a frame's draws, decoding it the first
/// time any frame's draws ask for it.
///
/// @param[in,out] list the frame's list, which keeps the decoded frame
/// @param frame the GAF frame
/// @return the decoded frame, or null when it cannot be decoded
const oa::formats::gaf::RenderedFrame*
decoded_frame(WorldDrawList& list, const oa::formats::gaf::Frame& frame);

/// Bytes of decoded pixels and coverage past which an explosion file is drawn
/// a frame at a time (draws_frame_by_frame): 16 MiB. The largest 3.1c
/// explosion file decodes to 2.5 MB.
inline constexpr uint64_t frame_by_frame_decoded_bytes = 16U * 1024U * 1024U;

/// Bytes of rendered frames a GafFrameCache keeps unless told otherwise:
/// 32 MiB, room for one frame of the largest a GAF holds, 4096 pixels square.
inline constexpr std::size_t gaf_frame_cache_bytes = 32U * 1024U * 1024U;

/// Tells whether a file is drawn a frame at a time, each frame rendered from
/// the file's bytes as it is drawn, rather than each sequence decoded whole
/// and kept: whether its pixels and coverage, decoded, take more than a
/// threshold.
///
/// @param checked the file, parsed with or without its pixels
/// @param threshold decoded bytes past which the file is drawn a frame at a time
/// @return true for a file over the threshold
[[nodiscard]] bool draws_frame_by_frame(
    const oa::formats::gaf::Archive& checked, uint64_t threshold = frame_by_frame_decoded_bytes
) noexcept;

/// Rendered frames of files drawn a frame at a time, kept from one frame's
/// draws to the next while together they fit a budget of bytes; to make
/// room, the frame drawn longest ago goes first. Frames are known by the
/// address of their GAF frame record, so the cache is cleared whenever the
/// records it was filled from go. It also remembers the frames that failed
/// to render, so that they are not read again.
class GafFrameCache {
  public:

    /// Creates an empty cache.
    ///
    /// @param budget most bytes of rendered pixels and coverage kept
    explicit GafFrameCache(std::size_t budget = gaf_frame_cache_bytes) noexcept;

    /// Returns a kept frame, which becomes the one drawn most recently.
    ///
    /// @param frame the GAF frame's record
    /// @return the rendered frame, or null when it is not kept
    [[nodiscard]] std::shared_ptr<const oa::formats::gaf::RenderedFrame>
    find(const oa::formats::gaf::Frame* frame);

    /// Keeps a rendered frame as the one drawn most recently, letting the
    /// frames drawn longest ago go until it fits the budget. A frame larger
    /// than the whole budget is not kept, and nothing goes for it.
    ///
    /// @param frame the GAF frame's record; a frame kept already is replaced
    /// @param rendered the frame rendered
    /// @return true when the frame is kept
    bool keep(
        const oa::formats::gaf::Frame* frame,
        std::shared_ptr<const oa::formats::gaf::RenderedFrame> rendered
    );

    /// Remembers that a frame failed to render.
    ///
    /// @param frame the GAF frame's record
    void mark_failed(const oa::formats::gaf::Frame* frame);

    /// Tells whether a frame failed to render.
    ///
    /// @param frame the GAF frame's record
    /// @return true once mark_failed named it, until the cache is cleared
    [[nodiscard]] bool failed(const oa::formats::gaf::Frame* frame) const;

    /// Lets every frame go and forgets the frames that failed.
    void clear() noexcept;

    /// Returns the bytes of rendered pixels and coverage kept.
    ///
    /// @return at most budget_bytes()
    [[nodiscard]] std::size_t kept_bytes() const noexcept { return kept_bytes_; }

    /// Returns how many frames are kept.
    ///
    /// @return the count
    [[nodiscard]] std::size_t kept_frames() const noexcept { return recent_.size(); }

    /// Returns the most bytes the cache keeps.
    ///
    /// @return the budget
    [[nodiscard]] std::size_t budget_bytes() const noexcept { return budget_bytes_; }

  private:

    /// A kept frame and the bytes it takes.
    struct Entry {
        const oa::formats::gaf::Frame* frame{};
        std::shared_ptr<const oa::formats::gaf::RenderedFrame> rendered;
        std::size_t bytes{};
    };

    std::size_t budget_bytes_{};
    std::size_t kept_bytes_{};
    std::list<Entry> recent_; ///< the frame drawn most recently first
    std::unordered_map<const oa::formats::gaf::Frame*, std::list<Entry>::iterator> entries_;
    std::unordered_set<const oa::formats::gaf::Frame*> failed_;
};

/// Returns a frame of a file drawn a frame at a time, rendered for a frame's
/// draws: the cache's copy, else the frame rendered from the file's bytes
/// (oa::formats::gaf::render_ranged) and kept in the cache.
///
/// The list holds the frame until it is cleared, so that the cache may let
/// it go while the frame is drawn. A frame larger than the cache's whole
/// budget is rendered for these draws alone and rendered again when drawn
/// again. A frame that fails to render is remembered by the cache and not
/// read again.
///
/// @param[in,out] list the frame's list, which holds the rendered frame
/// @param[in,out] cache the frames kept from one frame's draws to the next
/// @param frame the GAF frame, from a parse of the file without its pixels
/// @param reader where the file's bytes are read from
/// @param[out] failure receives the error of a frame that fails to render
///     for the first time; may be null
/// @return the rendered frame, or null when it cannot be rendered
const oa::formats::gaf::RenderedFrame* ranged_frame(
    WorldDrawList& list,
    GafFrameCache& cache,
    const oa::formats::gaf::Frame& frame,
    const oa::formats::gaf::ReadHooks& reader,
    std::optional<oa::formats::gaf::Error>* failure = nullptr
);

/// Adds a draw of a kind to the end of a list.
///
/// @param[in,out] list the list
/// @param kind its kind
/// @param index its place in its kind's list
void add_world_draw(WorldDrawList& list, WorldDrawKind kind, std::size_t index);

/// A rectangle of the battlefield frame: the columns from `left` up to
/// `right` and the rows from `top` up to `bottom`.
struct FrameArea {
    int32_t left{};
    int32_t top{};
    int32_t right{};
    int32_t bottom{};
};

/// What every band of one frame draws with.
struct WorldFrameDraw {
    /// The whole frame; each band narrows its rows to its own.
    WorldTarget target{};
    /// The frame pixels that show the map rather than the black past its
    /// edges, which a feature's frames (SpriteDraw::on_map) are drawn
    /// within (oa::present::world_renderer::shown_map_span); the whole
    /// frame unless set.
    FrameArea shown_map{
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::max()
    };
    const oa::PaletteBytes* palette{}; ///< the match's palette, 4 bytes a colour
    float scale{1.0F};                 ///< frame pixels per map pixel, as the GAF draws scale
    oa::present::model::RgbBridge* bridge{};
    /// The models' display: each band binds it on the thread drawing it.
    oa::present::model::ModelDisplay* display{};
    /// The match's light table, oa::present::ramp_table_rows rows of 256
    /// palette indices, that the flashes light through; null draws none.
    const uint8_t* light_table{};
    const oa::Sprite* projectile_shadow{}; ///< FX.GAF "shadow" frame 0; no data for none
    alignas(4) oa::Rect32 debris_view{};   ///< the rectangle debris origins are culled to
    /// Pixels across and down a frame line (a laser, lightning or a debug
    /// beam) is drawn thick, so that it stays visible once the frame is
    /// reduced to the screen; 1 draws it as the game always has.
    int32_t line_thickness{1};
    /// Pixels across and down a selection line is drawn thick in the model
    /// bridge, at one pixel per map pixel; 1 draws it as the game always has.
    int32_t bridge_line_thickness{1};
};

/// The buffers one band draws with, apart from the bridge band.
struct WorldBandScratch {
    oa::present::model::ModelRenderer
        renderer{}; ///< the frame's renderer settings, its own composite
    oa::present::model::SupersampleScratch supersample{};
    std::vector<oa::formats::objects3d::FixedVector3> debris_points;
};

/// Draws one band of a frame's battlefield draws.
///
/// Every draw of the list is drawn in order, each changing only the band's
/// rows of the frame and of the model bridge: the band's own bridge rows are
/// captured, drawn and written back, sprites, squares and lines write their
/// pixels in the band. Builds nothing and changes nothing but the band's
/// rows, the bridge's tiles and capture copy in them, the band and the
/// scratch. The models' display is bound on the calling thread while the
/// band draws.
///
/// @param list the frame's draws
/// @param frame what every band draws with
/// @param[in,out] band the band of the frame's bridge (bridge_split)
/// @param[in,out] renderer the renderer the band draws with: the frame's
///     settings, its composite the band's own
/// @param[in,out] supersample the band's buffers for units drawn finer
/// @param[in,out] debris_points the band's buffer for turned debris points
void draw_world_band(
    const WorldDrawList& list,
    const WorldFrameDraw& frame,
    oa::present::model::BridgeBand& band,
    oa::present::model::ModelRenderer& renderer,
    oa::present::model::SupersampleScratch& supersample,
    std::vector<oa::formats::objects3d::FixedVector3>& debris_points
);

} // namespace oa::app
