// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Full tier's scene builder: the stages that turn the planner's
// battlefield draws (world_draws.hpp) into a card command list (card.hpp),
// which the Full branch of the presentation (runtime_full.cpp) runs on the
// card once per presented frame. This header holds what every stage
// shares, the view a frame is built for, and two of the stages: the sprite
// stage (runtime_full_sprites.cpp), the list's sprites, particle squares,
// lines and selection lines from the sprite pages, and the model stage
// (runtime_full_models.cpp), the list's units, 3D features, projectiles,
// debris and shatter fragments as meshes, with their shadows drawn into a
// transparent shadow target and composed once at half darkness, lighter as
// the view zooms out and not at all from a quarter out (the list's
// shadow_level, shadow_fade.hpp); a feature's shadow frame on the sprite
// stage fades the same way. The
// terrain stage is full_terrain.hpp and the fog passes are full_fog.hpp,
// both pure. The branch walks the list once and hands each draw to the
// stage of its kind, so that sprites and models keep the painter's order
// the planner laid down. Nothing here reads the Runtime, so a stage is
// tested on its own; the standard and Basic tiers never reach it.
#pragma once

#include "oa/app/card.hpp"
#include "oa/app/card/executor.hpp"
#include "oa/core/world.h"
#include "oa/formats/gaf.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/present/gpu_world/model_meshes.hpp"
#include "oa/present/gpu_world/sprite_pages.hpp"
#include "oa/present/model/model_draw.hpp"
#include "oa/present/model/model_library.hpp"
#include "oa/present/surface.h"
#include "oa/present/world_renderer.hpp"
#include "world_draws.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace oa::app::full {

// ---------------------------------------------------------------------------
// What every stage shares

/// A call only the Full tier makes failed: a page or target the executor
/// could not make or fill, or a frame it refused. The presentation drops
/// the tier for the run and presents the frame as Basic does.
class CardError : public std::runtime_error {
  public:

    using std::runtime_error::runtime_error;
};

/// A frame the executor refused before drawing anything of it: one the
/// stages built wrong, which no driver caused. The presentation drops the
/// tier for the run as for any CardError, but strikes nothing against the
/// driver.
class FrameRefusedError : public CardError {
  public:

    using CardError::CardError;
};

/// The view a frame's stages draw through: where the battlefield lies in
/// the target, the zoom the list was planned at, and what every vertex is
/// moved and scaled by.
struct SceneView {
    /// The battlefield's top-left corner in the target, in target pixels.
    float origin_x{};
    float origin_y{};
    /// Target pixels per layout pixel: 1 where the renderer's logical
    /// presentation maps layout pixels onto the display itself.
    float scale{1.0F};
    /// Screen pixels per map pixel the list was planned at: the zoom.
    float zoom{1.0F};
    /// How far past the camera's map pixel the view is drawn, in map
    /// pixels from 0 to 1: every vertex moves by its negative times the
    /// zoom, so that everything drawn moves together between map pixels.
    oa::present::world_renderer::ViewOffset offset{};
    /// The battlefield in layout pixels, which every batch's scissor clips to.
    int32_t width{};
    int32_t height{};
    /// The map pixel the battlefield's top-left corner shows: the camera,
    /// from which the fog's cells are counted, so that a sprite takes the
    /// fog's state at the point it is drawn at, as the fog lays its tiles
    /// over the picture by map column and row alone.
    int32_t camera_x{};
    int32_t camera_y{};
    /// The target the stages draw into; none for the frame's final target.
    card::TargetHandle target{};
};

/// Returns the target pixels one map pixel covers in a view.
///
/// @param view the view
/// @return the zoom times the view's scale
[[nodiscard]] float pixels_per_map_pixel(const SceneView& view) noexcept;

/// Returns the part of a scissor that lies over the map a view shows, where
/// a feature's draws are cut off, as the terrain ends there.
///
/// A map pixel's left edge lands `(pixel - camera_x - offset.x) * zoom *
/// scale` target pixels right of the battlefield's corner, and its top edge
/// likewise; the map's edges are rounded to the nearest pixel edge.
///
/// @param view the view: its camera, offset, zoom and scale
/// @param corner_x the battlefield's top-left corner in the scissor's target
/// @param corner_y its row
/// @param map_width map pixels across the map the view shows
/// @param map_height map pixels down
/// @param scissor the scissor, in the same target
/// @return the part of the scissor over the map; no width or no height when
///     the map lies wholly off it
[[nodiscard]] inline card::Rect shown_map_scissor(
    const SceneView& view,
    float corner_x,
    float corner_y,
    int32_t map_width,
    int32_t map_height,
    const card::Rect& scissor
) noexcept {
    const double pixel = static_cast<double>(view.zoom) * static_cast<double>(view.scale);
    const auto edge = [pixel](float corner, double from_camera) {
        return std::llround(static_cast<double>(corner) + from_camera * pixel);
    };
    const int64_t left = std::max<int64_t>(
        scissor.x, edge(corner_x, -static_cast<double>(view.camera_x) - view.offset.x)
    );
    const int64_t top = std::max<int64_t>(
        scissor.y, edge(corner_y, -static_cast<double>(view.camera_y) - view.offset.y)
    );
    const int64_t right = std::min<int64_t>(
        int64_t{scissor.x} + scissor.width,
        edge(corner_x, static_cast<double>(map_width) - view.camera_x - view.offset.x)
    );
    const int64_t bottom = std::min<int64_t>(
        int64_t{scissor.y} + scissor.height,
        edge(corner_y, static_cast<double>(map_height) - view.camera_y - view.offset.y)
    );
    return {
        static_cast<int32_t>(left),
        static_cast<int32_t>(top),
        static_cast<int32_t>(std::max<int64_t>(0, right - left)),
        static_cast<int32_t>(std::max<int64_t>(0, bottom - top))
    };
}

/// The viewer's sight, from which a stage tells the fog's state at a cell:
/// the sight grid's cells of fog_cell_pixels map pixels a side.
struct SightView {
    std::span<const uint8_t> coverage{};     ///< units of the viewer seeing each cell
    std::span<const uint16_t> player_bits{}; ///< the players that have mapped each cell
    int32_t width{};                         ///< cells across
    int32_t height{};                        ///< cells down
    uint16_t viewer_bit{};                   ///< the viewer's bit in player_bits
    bool line_of_sight{};                    ///< the line-of-sight rule is on
    bool mapping{true};                      ///< the mapping rule is on
    /// The dithered option: the fog dithers the cells out of sight instead
    /// of greying them, so a sprite under one is drawn in colour.
    bool dithered{};
};

/// What the fog shows of a cell.
enum class CellFog : uint8_t {
    seen,   ///< in sight: drawn in colour
    unseen, ///< out of sight, under the line-of-sight rule: drawn greyed
    /// Never mapped, under the mapping rule, and not out of sight: drawn
    /// in colour, under the black pass that covers never-mapped ground.
    unmapped,
};

/// Returns the fog's state at a map pixel, as the fog grid marks the
/// cell's corners: out of sight where the line-of-sight rule grays it,
/// else never mapped where the mapping rule blacks it out, else in sight.
/// A pixel off the sight grid, or off the map, is in sight, since the fog
/// marks nothing there.
///
/// @param sight the viewer's sight
/// @param map_x map pixel column
/// @param map_z map pixel row
/// @return the state
[[nodiscard]] CellFog cell_fog(const SightView& sight, int32_t map_x, int32_t map_z) noexcept;

/// Returns a flat colour through the display gamma, as the card takes it:
/// each channel from 0 to 1.
///
/// @param rgb the colour before the gamma
/// @param gamma the gamma's table; null for a gamma of 1
/// @return the colour, opaque
[[nodiscard]] card::Colour
flat_colour(const std::array<uint8_t, 3>& rgb, const std::array<uint8_t, 256>* gamma) noexcept;

/// Tells whether a kind of draw is the sprite stage's.
///
/// A projectile's lens is neither stage's: the Full tier draws none, since
/// it does not read back what it has drawn.
///
/// @param kind the kind
/// @return true for sprites, blended sprites, explosions' flashes, particle
///     squares, lines and selection lines
[[nodiscard]] bool sprite_kind(WorldDrawKind kind) noexcept;

/// Tells whether a kind of draw is the model stage's.
///
/// @param kind the kind
/// @return true for models, projectiles, debris pieces and shatter fragments
[[nodiscard]] bool model_kind(WorldDrawKind kind) noexcept;

// ---------------------------------------------------------------------------
// The sprite stage

/// What the sprite stage asks of its host: the card's page that holds a
/// sprite page's texels.
struct SpritePageHooks {
    void* context{};
    /// Returns the card's page holding the texels of a page of the sprite
    /// pages (SpritePages::pages), making it when the page is new or has a
    /// new size; the host uploads the texels the page changed after the
    /// stage ran. A handle of none, or a null hook, leaves the sprite
    /// undrawn and counted as refused.
    card::PageHandle (*card_page)(void* context, uint32_t page){};
};

/// What the sprite stage reads.
struct SpriteStageInputs {
    const WorldDrawList* list{}; ///< the frame's draws, in order
    SceneView view{};
    SightView sight{};
    /// The match's palette, 4 bytes a colour, for a selection line's colour.
    const oa::PaletteBytes* palette{};
    /// The display gamma's table, applied to every flat colour as the
    /// pages apply it to their texels; null for a gamma of 1.
    const std::array<uint8_t, 256>* gamma{};
};

/// What the sprite stage did.
struct SpriteStageResult {
    uint32_t sprites{}; ///< sprite draws emitted, greyed ones among them
    uint32_t greyed{};  ///< sprites drawn from their greyed cells
    uint32_t lit{};     ///< explosions' flashes drawn from their lit cells
    uint32_t refused{}; ///< sprites the pages or the host refused
    uint32_t squares{}; ///< particle squares emitted
    uint32_t lines{};   ///< lines and selection lines emitted
    uint32_t batches{}; ///< batches appended to the frame
    /// A frame placed on the pages for this draw was evicted before the
    /// draw ended, since the frame's distinct sprites exceed the pages'
    /// memory: the stage took its batches back and drew nothing.
    bool pages_overflowed{};
};

/// One frame's emission of the sprite stage, a draw at a time, so that the
/// branch can hand the list's draws to the stages in the list's order.
///
/// A sprite is its frame's cell on the pages (SpritePages::frame, keyed by
/// the frame the planner drew from), a quad `w * zoom` by `h * zoom` with
/// the hotspot scaled exactly, drawn by premultiplied alpha, which keeps
/// the key transparent: at a vertex alpha of one half where the planner
/// blends it through the alpha table, else opaque. Under a cell out of
/// sight it is drawn from its greyed cell when the pages hold a gray table;
/// under never-mapped ground it is drawn as under any other, since the
/// black pass over that ground goes over it. An explosion's flash is its
/// frame's lit cell (DrawMode::lit) drawn by the lighten blend, which
/// lights what is under it close to the light table's rows without
/// snapping to the palette, its vertex colour one half at
/// FlashStrength::reduced; it is drawn so under any fog, which goes over
/// it as over the ground. A page's texels are sampled
/// nearest at a whole-number zoom, linear below 1 and pixel-art above. A
/// particle's square is the planner's rectangle, solid; a line is a quad
/// `max(1, zoom)` layout pixels wide through the centres of its end
/// pixels, a selection line the same from map pixels at the zoom, each in
/// its colour through the gamma. Every other kind of draw is left to the
/// other stages. Consecutive draws that share their page and blend go in
/// one batch, every batch into the view's target under the battlefield's
/// scissor.
class SpriteFrame {
  public:

    /// Readies one frame's emission.
    ///
    /// @param inputs the list, the view, the sight, the palette and the gamma;
    ///        they outlive the frame's emission
    /// @param[in,out] pages the sprite pages, which place the frames drawn this frame
    /// @param hooks the host's pages for the sprite pages
    /// @param[in,out] frame the card's frame the batches are appended to
    SpriteFrame(
        const SpriteStageInputs& inputs,
        oa::present::gpu_world::SpritePages& pages,
        const SpritePageHooks& hooks,
        card::CardFrame& frame
    );
    SpriteFrame(const SpriteFrame&) = delete;
    SpriteFrame& operator=(const SpriteFrame&) = delete;
    /// Frees the frame's state; what was emitted stays in the card's frame
    /// as finish left it.
    ~SpriteFrame();

    /// Emits one draw of the list, where it is a kind the stage draws.
    ///
    /// @param draw the draw, one of the list's
    void emit(const WorldDraw& draw);

    /// Ends the frame's emission: where the pages evicted a frame placed
    /// this frame, which only a frame whose distinct sprites exceed the
    /// pages' memory does, the stage's batches are taken back, since a cell
    /// drawn from after its frame left it would show another sprite.
    ///
    /// @return what was emitted
    [[nodiscard]] SpriteStageResult finish();

  private:

    /// The frame's state, defined with the stage.
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

/// Emits the frame's sprites, particle squares, lines and selection lines
/// into the card's frame, in the list's order: a SpriteFrame over every
/// draw of the list.
///
/// @param inputs the list, the view, the sight, the palette and the gamma
/// @param[in,out] pages the sprite pages, which place the frames drawn this frame
/// @param hooks the host's pages for the sprite pages
/// @param[in,out] frame the card's frame the batches are appended to
/// @return what was emitted
SpriteStageResult emit_sprites(
    const SpriteStageInputs& inputs,
    oa::present::gpu_world::SpritePages& pages,
    const SpritePageHooks& hooks,
    card::CardFrame& frame
);

/// Returns how a sprite's page is sampled at a zoom: nearest at a
/// whole-number zoom, where the card gives the game's pixels exactly;
/// linear below 1; pixel-art above 1, read as nearest where the renderer
/// lacks the mode.
///
/// @param zoom screen pixels per map pixel
/// @return the sampling
[[nodiscard]] card::Sampling sprite_sampling(float zoom) noexcept;

/// Returns how wide a thin line is drawn: `max(1, zoom)` layout pixels, at
/// the view's scale, so that it stays about one screen pixel wide zoomed
/// out and grows with the zoom zoomed in.
///
/// @param view the view
/// @return the width in target pixels
[[nodiscard]] float line_width(const SceneView& view) noexcept;

/// Appends a line as a quad of one colour: the segment between the centres
/// of its first and last pixels, `width` pixels wide and drawn out half a
/// width past each end, so that one pixel wide along a row or a column it
/// covers exactly the pixels the game's line covers, and on a slant the
/// pixels nearest the segment. Positions are target pixels.
///
/// @param[in,out] frame the frame
/// @param x0 the first pixel's centre column
/// @param y0 the first pixel's centre row
/// @param x1 the last pixel's centre column
/// @param y1 the last pixel's centre row
/// @param width pixels across the line, above 0
/// @param colour the colour of every corner
void append_line_quad(
    card::CardFrame& frame,
    float x0,
    float y0,
    float x1,
    float y1,
    float width,
    const card::Colour& colour
);

// ---------------------------------------------------------------------------
// The model stage

/// Players a team colour is kept for, as ModelRenderer::team_colors has them.
inline constexpr std::size_t team_colour_players = 10;

/// What the model stage reads of a frame: the planner's list and the state
/// it points into, read and never written. The list, the World, the library
/// and the display must outlive the frame's emission.
struct ModelFrameInputs {
    const WorldDrawList* draws{};
    /// The match's World: the units a plan names, and Game's sea level,
    /// viewpoint player and debug overlay.
    const oa::World* world{};
    /// The prepared models and the texture library the frame draws from.
    const oa::present::model::ModelLibrary* library{};
    /// The models' display tables: the shade table gives the flat polygons
    /// of a lit building their colours. Null draws them unlit.
    const oa::present::model::ModelDisplay* display{};
    uint16_t graphics_flags{}; ///< Game.graphics_flags: shadows, vehicle shadows and shading
    /// The tick of the build effect's pulse, which the nanoframe's colours
    /// cycle by: the match's tick less ModelRenderer::build_pulse_lag.
    uint32_t build_pulse_tick{};
    /// An unfinished mobile unit is drawn as an unfinished building is,
    /// its nanoframe over every piece (ModelRenderer::moving_pieces_once_built);
    /// false draws its pieces whole with the running frames, as 3.1c does.
    bool moving_pieces_once_built{};
    /// Each player's colour, by player index (ModelRenderer::team_colors).
    std::array<uint8_t, team_colour_players> team_colors{};
    /// The shaded builder's light direction and the scale of its dot product.
    std::array<float, 3> light{
        oa::present::model::default_light_x,
        oa::present::model::default_light_y,
        oa::present::model::default_light_z
    };
    float light_scale{oa::present::model::default_light_scale};
    /// FX.GAF "shadow" frame 0, rendered, for the projectiles' shadows; null
    /// casts none.
    const oa::formats::gaf::RenderedFrame* projectile_shadow{};
};

/// What the model stage did since it was made, for checks and the frame
/// statistics.
struct ModelStageCounts {
    uint64_t units{};       ///< unit and 3D feature draws emitted
    uint64_t carried{};     ///< carried units drawn with their carriers
    uint64_t projectiles{}; ///< 3DO projectile draws, a missile's child counted with it
    uint64_t debris{};      ///< debris pieces drawn, those culled on their origin left out
    uint64_t fragments{};   ///< shatter fragments drawn
    uint64_t polygons{};    ///< polygons emitted as triangles
    uint64_t culled{};      ///< polygons left out as back-facing or flat
    uint64_t strip_quads{}; ///< textured quads drawn as strips across their rows
    /// How far the textured quad farthest from a parallelogram was from one,
    /// in map pixels: the corners' twist (the first and third corners'
    /// sum less the second and fourth's), four times what its two triangles
    /// move its texels at zoom 1.
    double widest_twist{};
    uint64_t shadows{};          ///< silhouettes and shadow sprites emitted
    uint64_t meshes_built{};     ///< meshes built from models
    uint64_t frames_placed{};    ///< texture frames placed on the pages
    uint64_t page_uploads{};     ///< page rectangles uploaded
    uint64_t target_creations{}; ///< shadow targets made
};

/// The model stage: the meshes, the texture pages and the shadow target
/// the card draws models from, and the batches it emits for a frame.
///
/// Per frame, emit_shadows first and then emit_draw for each draw of the
/// list in its order (emit_frame does both for a frame of models alone);
/// upload sends the page texels written since the last upload, and the
/// caller then executes the frame. The stage keeps the executor's handles
/// of what it made; close destroys them.
class ModelStage {
  public:

    /// The stage's state, defined with the stage.
    struct Impl;

    /// Makes a stage with empty pages, no meshes and no palette.
    ModelStage();
    ModelStage(const ModelStage&) = delete;
    ModelStage& operator=(const ModelStage&) = delete;
    /// Frees the stage's own memory; the executor's pages and targets are
    /// freed by close.
    ~ModelStage();

    /// Sets the palette and the display gamma the pages, the meshes and
    /// every flat colour are built from; a value that differs empties the
    /// pages and the meshes, which are built again on their next use.
    ///
    /// @param palette the match's palette
    /// @param gamma channel multiplier; 1 applies the palette unchanged
    void set_palette(const Palette& palette, float gamma);

    /// Destroys every page and target the stage made on an executor and
    /// forgets the meshes, as at a match's end or a device reset; the pages'
    /// memory limits go back to what they started with.
    ///
    /// @param[in,out] executor the executor the pages and targets were made on
    void close(card::Executor& executor) noexcept;

    /// Begins a frame: the texture frames it places or finds are held until
    /// the next one, never evicted to make room, and the pages grow where
    /// they alone fill them (gpu_world::SpritePages::begin_frame).
    void begin_frame() noexcept;

    /// Sets what both page sets ask before their memory limits grow.
    ///
    /// @param hooks the hooks; empty ones let the limits never grow
    void set_growth_hooks(const oa::present::gpu_world::GrowthHooks& hooks) noexcept;

    /// Emits the frame's shadows: every drawn unit's and 3D feature's
    /// silhouette and every projectile's shadow sprite into the shadow
    /// target, cleared first, then one resolve that composes the target over
    /// the battlefield at half darkness. Where the target cannot be made,
    /// each shadow is drawn straight into the battlefield at half darkness
    /// in list order. Below the list's full shadow level the darkness is
    /// that much of a half; at level 0 nothing is emitted, the target
    /// neither cleared nor composed. This function is the extension point a
    /// later "better shadows" option replaces.
    ///
    /// @param inputs the frame's list and state
    /// @param view the frame's view
    /// @param[in,out] executor the executor the target and pages are made on
    /// @param[in,out] frame the command list the batches are appended to
    void emit_shadows(
        const ModelFrameInputs& inputs,
        const SceneView& view,
        card::Executor& executor,
        card::CardFrame& frame
    );

    /// Emits one draw of the list: a unit or 3D feature, a projectile, a
    /// debris piece or a shatter fragment, as the polygons the processor
    /// draws for it; every other kind is left to the other stages.
    ///
    /// @param inputs the frame's list and state
    /// @param view the frame's view
    /// @param entry the draw, one of inputs.draws->draws
    /// @param[in,out] executor the executor the pages are made on
    /// @param[in,out] frame the command list the batches are appended to
    void emit_draw(
        const ModelFrameInputs& inputs,
        const SceneView& view,
        const WorldDraw& entry,
        card::Executor& executor,
        card::CardFrame& frame
    );

    /// Emits a frame of models alone: the shadows, then every draw of the
    /// list in its order.
    ///
    /// @param inputs the frame's list and state
    /// @param view the frame's view
    /// @param[in,out] executor the executor the pages and target are made on
    /// @param[in,out] frame the command list the batches are appended to
    void emit_frame(
        const ModelFrameInputs& inputs,
        const SceneView& view,
        card::Executor& executor,
        card::CardFrame& frame
    );

    /// Uploads the page texels written since the last upload, and frees
    /// the executor's pages of pages the sprite pages released.
    ///
    /// @param[in,out] executor the executor the pages were made on
    /// @return false, with error() set, when an upload failed
    [[nodiscard]] bool upload(card::Executor& executor);

    /// Returns what the stage has done.
    ///
    /// @return the counts since the stage was made
    [[nodiscard]] const ModelStageCounts& counts() const noexcept { return counts_; }

    /// Returns why the last executor call that failed did; the stage goes on
    /// without what it could not make.
    ///
    /// @return the executor's error, with what the stage was making; empty for none
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    /// Returns the pages the texture frames are placed on.
    ///
    /// @return the pages, in the palette's colours
    [[nodiscard]] const oa::present::gpu_world::SpritePages& pages() const noexcept {
        return pages_;
    }

    /// Returns the pages whose texels are doubled and clamped, which lit
    /// buildings draw their brightened polygons from.
    ///
    /// @return the bright pages
    [[nodiscard]] const oa::present::gpu_world::SpritePages& bright_pages() const noexcept {
        return bright_pages_;
    }

    /// Returns the bytes the meshes hold.
    ///
    /// @return the sum of mesh_bytes over the meshes built
    [[nodiscard]] std::size_t mesh_bytes() const noexcept;

    /// Returns the number of models whose mesh is built.
    ///
    /// @return the meshes held, refused models left out
    [[nodiscard]] std::size_t mesh_count() const noexcept;

    /// Reports whether the frame's shadows went to the shadow target, or
    /// straight into the battlefield because the target could not be made.
    ///
    /// @return true after an emit_shadows that composed the target
    [[nodiscard]] bool shadows_through_target() const noexcept { return shadows_through_target_; }

  private:

    std::unique_ptr<Impl> impl_;
    oa::present::gpu_world::SpritePages pages_;
    oa::present::gpu_world::SpritePages bright_pages_;
    ModelStageCounts counts_{};
    std::string error_{};
    bool shadows_through_target_{};
};

// ---------------------------------------------------------------------------
// Checks: the card's frame against the processor's raster

/// Most a channel of a pixel the card drew may differ from the processor's
/// for the two to count as the same colour: the card blends the gamma
/// colours in float and rounds its own way.
inline constexpr int model_raster_tolerance = 4;

/// How a frame of models the card drew compares with the processor's raster
/// of the same list over the same pixels. Pixels within one pixel of each
/// other may differ, since the card samples each pixel at its centre where
/// the processor's walk samples its corner: a sloped edge's pixels, a texel
/// of phase inside a polygon, and the colour of a pixel within one pixel of
/// either picture's edge, where the card reads a texel the processor's span
/// ends before.
struct ModelRasterComparison {
    std::size_t drawn{};       ///< pixels either picture draws
    std::size_t same{};        ///< pixels both draw within model_raster_tolerance a channel
    std::size_t phase{};       ///< pixels both draw, each colour found within a pixel in the other
    std::size_t edge_colour{}; ///< pixels both draw within a pixel of an edge, in other colours
    std::size_t far{};         ///< pixels both draw in colours the other has nowhere within a pixel
    std::size_t
        edge_coverage{}; ///< pixels one picture alone draws, the other drawing within a pixel
    std::size_t far_coverage{}; ///< pixels one picture alone draws, the other drawing nothing near
    std::size_t shadowed{};     ///< pixels the card darkened alone, compared by coverage only

    /// Returns the share of the drawn pixels one picture alone draws with
    /// the other drawing nothing near.
    ///
    /// @return far_coverage over drawn; 0 when nothing is drawn
    [[nodiscard]] double far_coverage_share() const noexcept;

    /// Returns the share of the pixels both draw whose colours are far apart.
    ///
    /// @return far over same + phase + edge_colour + far; 0 when nothing is
    ///     drawn by both
    [[nodiscard]] double far_share() const noexcept;
};

/// The bounds a frame of models is held to against the processor's raster:
/// the figures the installed game's unit models measured at zoom 1 on SDL's
/// software renderer, with room.
struct ModelRasterBounds {
    /// Most of the drawn pixels one picture alone may draw with the other
    /// drawing nothing within a pixel, and the pixels allowed on a small
    /// model whatever the share.
    double far_coverage_share{0.01};
    std::size_t far_coverage_pixels{4};
    /// Most of the pixels both draw that may take a colour the other has
    /// nowhere within a pixel.
    double far_share{0.25};
};

/// Compares a frame the card drew with the processor's raster of the same
/// list, pixel by pixel.
///
/// The card's picture is RGBA with alpha 0 where it drew nothing, 255 where
/// a model's body is and between where a shadow alone darkened the target;
/// the processor's is RGB24 with the display gamma applied, every pixel it
/// did not draw holding the key colour.
///
/// @param card_rgba the card's picture, 4 bytes a pixel, rows of `width`
/// @param processor_rgb the processor's picture, 3 bytes a pixel, rows of `width`
/// @param width columns of both
/// @param height rows of both
/// @param key the colour of the processor's undrawn pixels
/// @return the comparison
[[nodiscard]] ModelRasterComparison compare_model_rasters(
    const uint8_t* card_rgba,
    const uint8_t* processor_rgb,
    uint32_t width,
    uint32_t height,
    const std::array<uint8_t, 3>& key
);

/// Tells whether a comparison keeps within the bounds.
///
/// @param comparison the comparison
/// @param bounds the bounds
/// @return true when something was drawn, the far coverage is within its
///     share or its pixels, and the far colours within their share
[[nodiscard]] bool
within_bounds(const ModelRasterComparison& comparison, const ModelRasterBounds& bounds) noexcept;

} // namespace oa::app::full
