// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Full tier's fog of war on the graphics card: the fog grid the
// processor builds each frame (world_fog.hpp), whose 32-pixel tiles sit half
// a sight cell off the terrain's tiles so that each tile's corners lie on
// four cell centres, drawn as passes of quads whose corner alphas are 1 at
// a fogged corner and 0 elsewhere, interpolated across each tile, in place
// of the FOG.GAF mask shapes:
//
// - the greyed pass over cells that are mapped but out of sight, which
//   draws the terrain under each such tile again from the greyed terrain
//   pages (the atlas built with a palette of the gray table's entries),
//   blended over the colour terrain by the corner alphas, so a tile whose
//   four corners are out of sight shows the gray table's colours exactly
//   and a tile at an edge ramps from colour to grey;
// - its masked form, the same picture in a few quads whatever the fog: the
//   fog mask, a texel for each sight cell at the grid's tile corners, read
//   LINEAR so that it gives the corner alphas interpolated across each
//   tile, darkens the colour terrain by the alpha and scales the greyed
//   terrain, drawn whole in a target of its own, by it, and the greyed
//   picture is then added over the colour;
// - its dithered form, a solid quad of the dither colour at half the alpha,
//   the even tone the dither averages to, drawn over the objects;
// - the black pass over cells never mapped, a solid quad per tile.
//
// The Classic fog (Fog of War's Classic) draws what the processor draws
// instead: each partial tile's FOG.GAF shape, its opaque texels as the
// rectangles of ClassicShapes, so that the edges step from cell to cell
// as the game drew them. The greyed pass draws the greyed terrain under
// the gray tiles' rectangles at alpha 1, the dithered form clears every
// other map pixel under them to the dither colour through a page of the
// checkerboard, and the black pass fills the black tiles' rectangles with
// their palette colours; tiles wholly out of sight or never mapped are
// drawn as the smooth passes draw them, which is already the processor's
// picture.
//
// Beside the fog, the alphas of the quads that stand in for the two
// painters that shade the world beneath them, the kill board's shade and
// light levels among them (level_alpha).
//
// Pure: no SDL and no Runtime. The presentation chooses the terrain level,
// the targets and the colours, and runs the frame.
#pragma once

#include "oa/app/card.hpp"
#include "oa/present/gpu_world/terrain_atlas.hpp"
#include "oa/present/world_renderer/world_fog.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace oa::app::full_fog {

/// Where the fog grid lands in a target: the map pixel under the view's
/// top-left corner, which the grid was built for, the target pixel that map
/// pixel lands on, and the target pixels a map pixel spans.
struct FogPlacement {
    int32_t camera_x{}; ///< map pixel at the view's left edge
    int32_t camera_z{}; ///< map pixel at the view's top edge
    float origin_x{};   ///< target pixel the camera's map pixel lands on
    float origin_y{};
    float scale{1.0F}; ///< target pixels per map pixel, above 0
    /// Every quad's edges on whole target pixels, each edge where its map
    /// pixel lands rounded to the nearest, so that the quads of a zoom
    /// whose tiles span a fraction of a pixel meet on every renderer.
    bool whole_pixels{};
};

/// One level of the greyed terrain pages the greyed pass draws from, and
/// the share of the pass it carries: the terrain's own levels and shares at
/// the zoom, so that a tile wholly out of sight is filtered as the colour
/// terrain under it is.
struct GreyedLevel {
    uint8_t level{}; ///< the atlas level, below tile_level_count
    card::Sampling sampling{card::Sampling::nearest};
    float share{1.0F}; ///< the level's share of the greyed picture; the shares sum to 1
};

/// Most levels a greyed pass draws: the two that bracket a zoom below 1.
inline constexpr uint32_t most_greyed_levels = 2;

/// Corners of a fog tile, in the order the alphas are given: top-left,
/// top-right, bottom-left, bottom-right.
inline constexpr uint32_t fog_tile_corners = 4;

/// Rows of the display's shade and light tables, each scaling a colour.
inline constexpr int32_t table_rows = 32;
/// What one row of the shade table scales a colour by: row r scales by
/// r times this, so row 15 is about 1, unlit.
inline constexpr float shade_row_step = 0.06875F;
/// What one row of the light table adds to a colour's scale: row r scales
/// by 1 + r times this.
inline constexpr float light_row_step = 1.0F / 30.0F;

/// How a painter's shade or light level is drawn on the card: a quad of
/// the colour at the alpha, blended as the blend says, over the world.
struct LevelQuad {
    card::Colour colour{}; ///< black or white, at the alpha
    card::Blend blend{card::Blend::alpha};
};

/// Returns the quad that stands in for a shade or light level of the
/// display's tables over the world (the levels of shade_rect_level): a
/// negative level selects shade row 0x20 + level, clamped at 0, which
/// scales the colour by the row times shade_row_step, so the quad is black
/// by alpha at one less the scale, and nothing at a scale of 1 or more; a
/// level of 0 or more selects light row `level`, clamped at table_rows - 1,
/// which scales by 1 + row times light_row_step, so the quad is white added
/// at one less the scale's reciprocal, which lifts a colour toward white
/// and saturates as the table does.
///
/// @param level the level, shade below 0 and light from 0
/// @return the quad; alpha 0 where the level changes nothing
[[nodiscard]] LevelQuad level_quad(int32_t level) noexcept;

/// Returns the alpha at a fog tile's four corners from a corner mask: 1
/// where the corner's bit is set, 0 elsewhere.
///
/// @param mask the tile's corner mask (fog_corner_top_left and the rest)
/// @return the alphas, top-left, top-right, bottom-left, bottom-right
[[nodiscard]] std::array<float, fog_tile_corners> corner_alphas(uint8_t mask) noexcept;

/// Returns the alpha a level's pass draws a corner at, so that the levels
/// drawn one over another, each over what the earlier ones left, give the
/// corner's alpha of their weighted picture over the colour terrain:
/// alpha * share / (1 - alpha * later), where `later` is the sum of the
/// shares of the levels drawn after this one, and 0 where those cover the
/// corner wholly. Exact at the corners; across a tile the card interpolates
/// each pass's alphas on its own.
///
/// @param alpha the corner's alpha, 0 to 1
/// @param share the level's share of the greyed picture, 0 to 1
/// @param later the shares of the levels drawn after it, summing with `share` to at most 1
/// @return the pass's alpha at the corner, 0 to 1
[[nodiscard]] float pass_alpha(float alpha, float share, float later) noexcept;

/// Appends the greyed pass over the cells mapped but out of sight: for each
/// fog tile with any corner out of sight, the terrain under it from the
/// greyed pages, blended by card::Blend::alpha with the corner alphas
/// interpolated across the tile. A fog tile covers a quarter of each of four
/// terrain tiles, so it is drawn as four quads, each the quarter of a
/// terrain tile with the alphas the tile's corners interpolate to at its
/// corners; a terrain tile whose four fog tiles are wholly out of sight is
/// drawn once, whole, at alpha 1. A fog tile wholly never mapped, which the
/// black pass covers, adds nothing. Each level of `levels` is one pass over
/// the same quads, in order, at the alphas pass_alpha gives; one batch for
/// each run of quads on one page. Quads past the map's edge are left out,
/// as the terrain's are.
///
/// @param[in,out] frame the frame
/// @param grid the fog grid, built for the placement's camera
/// @param atlas the map's atlas, whose grid and slots the greyed pages share
/// @param greyed_pages the executor's greyed page for each page of the atlas, in order
/// @param levels the levels drawn, in order, with their shares
/// @param placement where the grid lands in the target
/// @param target the target the quads are drawn into; none for the window
/// @param scissor the pixels of the target the draw may write; null for all
/// @return quads appended, every level counted
uint32_t append_unseen_terrain(
    card::CardFrame& frame,
    const oa::present::world_renderer::FogGrid& grid,
    const oa::present::gpu_world::TerrainAtlas& atlas,
    std::span<const card::PageHandle> greyed_pages,
    std::span<const GreyedLevel> levels,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
);

/// The fog mask of a grid: a texel for each sight cell at a corner of the
/// grid's tiles, (width + 1) by (height + 1), every byte 255 where the cell
/// is out of sight and 0 where it is in sight, so that a LINEAR read between
/// the texels' centres gives the alpha a greyed tile's corners interpolate
/// to there; then a column and a row more, repeating the last ones, so that
/// a read at the far texels' centres takes nothing from past them.
struct UnseenMask {
    uint32_t width{};                ///< texels across: the grid's tiles and 2; 0 for an empty grid
    uint32_t height{};               ///< texels down: the grid's tiles and 2
    std::vector<card::Texel> texels; ///< row by row, width times height
    bool any_unseen{};               ///< a cell is out of sight, so the mask draws something
};

/// Builds a grid's fog mask, each cell out of sight where any tile it is a
/// corner of says so (FogTile::unseen): the tiles that reach the map agree
/// on each cell, and a tile wholly past the map's edge, which nothing draws,
/// marks none.
///
/// @param grid the fog grid
/// @param[out] mask the mask; empty for an empty grid
void build_unseen_mask(const oa::present::world_renderer::FogGrid& grid, UnseenMask& mask);

/// Appends the fog mask over the grid's tiles: one quad, white at alpha 1,
/// from half a tile before the first tile's top-left corner to half a tile
/// past the last tile's bottom-right corner, textured LINEAR by the mask's
/// cells from the first texel's edge to the last cell's far edge, so that
/// each cell's centre lands on its tile corner and each point of a tile
/// reads the alpha its corners interpolate to there. The texture spans
/// whole texels, which a renderer that reads a quad's texture as a
/// rectangle of whole texels, as SDL's software renderer does, reads as
/// given. Drawn by card::Blend::darken it scales what is under it by one
/// less the alpha; by card::Blend::modulate, by the alpha.
///
/// @param[in,out] frame the frame
/// @param grid the fog grid, built for the placement's camera
/// @param mask the grid's mask (build_unseen_mask)
/// @param page the page holding the mask from its top-left texel
/// @param page_width texels across the page, at least the mask's
/// @param page_height texels down the page, at least the mask's
/// @param blend how the mask is drawn
/// @param placement where the grid lands in the target
/// @param target the target the quad is drawn into; none for the window
/// @param scissor the pixels of the target the draw may write; null for all
/// @return quads appended: 1, or 0 for an empty grid, a mask not the
///         grid's or a page smaller than the mask
uint32_t append_unseen_mask(
    card::CardFrame& frame,
    const oa::present::world_renderer::FogGrid& grid,
    const UnseenMask& mask,
    card::PageHandle page,
    uint32_t page_width,
    uint32_t page_height,
    card::Blend blend,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
);

/// Appends the dithered form of the greyed pass: for each fog tile with any
/// corner out of sight and not wholly never mapped, a solid quad of the
/// dither colour, blended by card::Blend::alpha at half the corner alphas,
/// the even tone the dither's every-other pixel averages to. Tiles wholly
/// out of sight that follow one another in a row are one quad. Unlike the
/// greyed pass it goes over the objects, as the black pass does, since the
/// processor dithers them with the ground.
///
/// @param[in,out] frame the frame
/// @param grid the fog grid, built for the placement's camera
/// @param dither the dither colour, its alpha ignored
/// @param placement where the grid lands in the target
/// @param target the target the quads are drawn into; none for the window
/// @param scissor the pixels of the target the draw may write; null for all
/// @return quads appended
uint32_t append_unseen_dither(
    card::CardFrame& frame,
    const oa::present::world_renderer::FogGrid& grid,
    const card::Colour& dither,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
);

/// Appends the black pass over the cells never mapped: for each fog tile
/// with any corner never mapped, a solid quad of the colour, blended by
/// card::Blend::alpha at the corner alphas. Tiles wholly never mapped that
/// follow one another in a row are one quad.
///
/// @param[in,out] frame the frame
/// @param grid the fog grid, built for the placement's camera
/// @param colour the colour of never-mapped ground, its alpha ignored
/// @param placement where the grid lands in the target
/// @param target the target the quads are drawn into; none for the window
/// @param scissor the pixels of the target the draw may write; null for all
/// @return quads appended
uint32_t append_unmapped(
    card::CardFrame& frame,
    const oa::present::world_renderer::FogGrid& grid,
    const card::Colour& colour,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
);

/// One rectangle of a FOG.GAF tile's opaque texels of one palette index,
/// within one quarter of the tile, so that it lies over one terrain tile.
struct ShapeBox {
    uint8_t x{};      ///< the left texel within the tile, 0 to 31
    uint8_t y{};      ///< the top texel
    uint8_t width{};  ///< texels across, 1 to 16
    uint8_t height{}; ///< texels down, 1 to 16
    uint8_t index{};  ///< the palette index of its texels
};

/// The Classic fog's shapes: the opaque texels of each FOG.GAF tile of
/// both banks, every variant and every partial corner mask, as rectangles.
/// Each row of a quarter's texels is cut into runs of one palette index,
/// and a run is one rectangle with the runs under it that match it.
struct ClassicShapes {
    std::vector<ShapeBox> boxes; ///< every tile's rectangles, in FogTileSet's order
    /// Where each tile's rectangles begin in `boxes`, in FogTileSet's
    /// order, and after the last tile's, the end; empty until built.
    std::vector<uint32_t> first;

    /// Returns whether the shapes are built.
    [[nodiscard]] bool built() const noexcept {
        return first.size() == 2 * oa::present::world_renderer::FogTileSet::tiles_per_bank + 1;
    }

    /// Returns a tile's rectangles.
    ///
    /// @param bank black or gray tiles
    /// @param variant tile-set variant, 0..3
    /// @param mask partial corner mask, 1..14
    /// @return the tile's rectangles; none before the shapes are built
    [[nodiscard]] std::span<const ShapeBox>
    at(oa::present::world_renderer::FogTileSet::Bank bank,
       int32_t variant,
       uint8_t mask) const noexcept;
};

/// Builds the Classic fog's shapes from the FOG.GAF tile art.
///
/// @param tiles the tile art; shapes of art not loaded are left unbuilt
/// @param[out] shapes the shapes
void build_classic_shapes(
    const oa::present::world_renderer::FogTileSet& tiles, ClassicShapes& shapes
);

/// Texels a side of the Classic dither's page: one fog tile's.
inline constexpr uint32_t dither_page_edge = 32;

/// Fills the Classic dither's page, dither_page_edge texels a side: opaque
/// white at each texel whose column and row sum to an even number, the map
/// pixels the dither clears, and clear between. A fog tile's top-left map
/// pixel is always even across and down, so the page laid over any tile
/// clears the pixels the processor clears.
///
/// @param[out] texels the page's texels, row by row
void build_dither_texels(std::vector<card::Texel>& texels);

/// Appends the Classic greyed pass over the cells mapped but out of sight:
/// a tile wholly out of sight as append_unseen_terrain draws it, at alpha
/// 1, and a tile partly out of sight as the greyed terrain under the
/// rectangles of its gray FOG.GAF shape (the shape of its variant,
/// (row + column + phase) & 3, and corner mask), at alpha 1, so that its
/// edge steps as the game's does. Each level of `levels` is one pass over
/// the same quads, in order, at the alphas pass_alpha gives. Quads past
/// the map's edge are left out, as the terrain's are.
///
/// @param[in,out] frame the frame
/// @param grid the fog grid, built for the placement's camera
/// @param shapes the Classic fog's shapes, built
/// @param atlas the map's atlas, whose grid and slots the greyed pages share
/// @param greyed_pages the executor's greyed page for each page of the atlas, in order
/// @param levels the levels drawn, in order, with their shares
/// @param placement where the grid lands in the target
/// @param target the target the quads are drawn into; none for the window
/// @param scissor the pixels of the target the draw may write; null for all
/// @return quads appended, every level counted
uint32_t append_unseen_terrain_classic(
    card::CardFrame& frame,
    const oa::present::world_renderer::FogGrid& grid,
    const ClassicShapes& shapes,
    const oa::present::gpu_world::TerrainAtlas& atlas,
    std::span<const card::PageHandle> greyed_pages,
    std::span<const GreyedLevel> levels,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
);

/// A point of the greyed pass in a target: where it lands, and how much of
/// the greyed picture the pass draws there.
struct FogPoint {
    float x{};     ///< target pixel column
    float y{};     ///< target pixel row
    float alpha{}; ///< from 0, the colour, to 1, the grey
};

/// Most points one piece of a greyed cover holds: a triangle cut by the
/// four sides of a rectangle.
inline constexpr std::size_t most_cover_points = 7;

/// One piece of a greyed cover: a convex polygon, its points in order round
/// it, across which the alpha is linear.
struct CoverPiece {
    std::array<FogPoint, most_cover_points> points{};
    uint32_t count{}; ///< the points used, 3 or more
};

/// How much of a rectangle the greyed pass covers.
enum class Cover : uint8_t {
    none,  ///< nothing of it: what lies there is drawn in colour
    part,  ///< the pieces greyed_cover collected
    whole, ///< all of it at alpha 1: what lies there is drawn greyed
};

/// Collects the greyed pass over a rectangle of a target, so that a
/// feature's frame drawn there takes the fog pixel by pixel as the ground
/// under it does, where the processor greys every pixel of the battlefield
/// under its gray FOG.GAF tiles, the ground and the features alike: the
/// pieces of each fog tile with a corner out of sight that the rectangle
/// cuts, at the alphas the pass draws the terrain there at.
///
/// With `shapes` null, the smooth pass: each quarter of such a tile is the
/// two triangles of append_unseen_terrain's quad, its diagonal from the
/// top-left to the bottom-right corner and its corners at the corner
/// alphas interpolated to them, so that the alpha at each pixel is the one
/// the terrain's quad draws there, each triangle cut by the rectangle. With
/// `shapes`, the Classic pass: a tile wholly out of sight, and each
/// rectangle of a partly out-of-sight tile's gray FOG.GAF shape, at alpha
/// 1, cut by the rectangle. A tile wholly never mapped adds nothing, since
/// the black pass goes over it as over the ground. Edges land where the
/// terrain's do (FogPlacement::whole_pixels). Pieces past the map's edges
/// are kept, where the terrain's are left out: a feature's frame is cut off
/// there by its scissor.
///
/// @param grid the fog grid, built for the placement's camera
/// @param shapes the Classic fog's shapes, built; null for the smooth pass
/// @param placement where the grid lands in the target
/// @param left the rectangle's left edge, in target pixels
/// @param top its top edge
/// @param right its right edge
/// @param bottom its bottom edge
/// @param[out] pieces the pieces for Cover::part; emptied otherwise
/// @return how much of the rectangle the pass covers: Cover::whole only
///         where every tile it reaches is wholly out of sight at alpha 1,
///         and Cover::none for an empty rectangle, an empty grid, or
///         Classic shapes not built
Cover greyed_cover(
    const oa::present::world_renderer::FogGrid& grid,
    const ClassicShapes* shapes,
    const FogPlacement& placement,
    float left,
    float top,
    float right,
    float bottom,
    std::vector<CoverPiece>& pieces
);

/// Appends the Classic dithered form of the greyed pass: for each fog tile
/// with any corner out of sight and not wholly never mapped, the dither's
/// page drawn NEAREST in the dither colour over the whole tile where every
/// corner is out of sight, and over the rectangles of its gray FOG.GAF
/// shape elsewhere, so that every other map pixel under the gray tiles
/// takes the dither colour, as the processor clears them. It goes over the
/// objects, as the smooth form does.
///
/// @param[in,out] frame the frame
/// @param grid the fog grid, built for the placement's camera
/// @param shapes the Classic fog's shapes, built
/// @param page the dither's page (build_dither_texels), dither_page_edge texels a side
/// @param dither the dither colour, its alpha ignored
/// @param placement where the grid lands in the target
/// @param target the target the quads are drawn into; none for the window
/// @param scissor the pixels of the target the draw may write; null for all
/// @return quads appended
uint32_t append_unseen_dither_classic(
    card::CardFrame& frame,
    const oa::present::world_renderer::FogGrid& grid,
    const ClassicShapes& shapes,
    card::PageHandle page,
    const card::Colour& dither,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
);

/// Appends the Classic black pass over the cells never mapped: tiles
/// wholly never mapped as append_unmapped draws them, solid quads of the
/// colour, and for each tile partly never mapped the rectangles of its
/// black FOG.GAF shape, each a solid quad of its texels' palette colour.
///
/// @param[in,out] frame the frame
/// @param grid the fog grid, built for the placement's camera
/// @param shapes the Classic fog's shapes, built
/// @param colour the colour of a tile wholly never mapped, its alpha ignored
/// @param palette the colour of each palette index, through the display's gamma
/// @param placement where the grid lands in the target
/// @param target the target the quads are drawn into; none for the window
/// @param scissor the pixels of the target the draw may write; null for all
/// @return quads appended
uint32_t append_unmapped_classic(
    card::CardFrame& frame,
    const oa::present::world_renderer::FogGrid& grid,
    const ClassicShapes& shapes,
    const card::Colour& colour,
    std::span<const card::Colour, OA_PALETTE_COLORS> palette,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
);

} // namespace oa::app::full_fog
