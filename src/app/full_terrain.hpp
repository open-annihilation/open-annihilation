// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Full tier's terrain on the graphics card: the rule that chooses, for a
// zoom, which levels of the terrain atlas are drawn, how each is sampled and
// blended and whether a target stands between the pages and the window;
// and the quads of the terrain tiles a view shows, appended to a card
// command list (card.hpp) from the atlas's pages (terrain_atlas.hpp), and
// the same terrain drawn again where Water ripples move its art. Pure: no
// SDL and no Runtime. runtime_full.cpp uploads the pages, builds each
// frame with these and runs it.
#pragma once

#include "oa/app/card.hpp"
#include "oa/present/gpu_world/terrain_atlas.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace oa::app::full_terrain {

/// One draw of the terrain from one level of the atlas.
struct TerrainPass {
    uint8_t level{}; ///< the atlas level drawn: 0 or 1
    card::Sampling sampling{card::Sampling::nearest};
    card::Blend blend{card::Blend::none};
    float alpha{1.0F}; ///< the vertex alpha of the quads, which the blend weighs the pass by
};

/// Most passes one zoom draws: the two levels that bracket a zoom below 1.
inline constexpr uint32_t most_terrain_passes = 2;

/// How the Full tier draws the terrain at a zoom, with no supersampling.
///
/// Zoomed out, below zoom 1, the two atlas levels that bracket the zoom are
/// blended: with t = log2(1 / zoom), level ceil(t) LINEAR, then the level
/// above it LINEAR over it at alpha 1 - (t - (ceil(t) - 1)); at a whole t
/// that level alone. So at zoom 0.5 level 1 alone is drawn, each of its
/// texels one window pixel, which is today's box filter where the camera
/// lies on an even map pixel; between 0.5 and 0.25 level 2 is blended under
/// level 1; and at 0.25 and below, which only the Full tier's zoom floor
/// (kMinFullBattlefieldZoom) reaches, level 2, the last tile level, is
/// drawn alone, reduced. At zoom 1 and at
/// every whole-number zoom above it level 0 is drawn NEAREST: 3.1c's pixels
/// exactly. At another zoom above 1 level 0 is drawn by the pixel-art
/// sampling mode where the renderer has it; elsewhere, SDL's software
/// renderer among them, it is drawn NEAREST into a target at the next whole
/// number and the target drawn LINEAR to the window by zoom over that
/// number, which is the Basic tier's sharp-bilinear.
struct TerrainDrawPlan {
    std::array<TerrainPass, most_terrain_passes> passes{}; ///< in the order drawn
    uint32_t pass_count{};
    /// The passes are drawn into a target at target_zoom, which is then
    /// drawn LINEAR to the window.
    bool through_target{};
    uint32_t target_zoom{1}; ///< window pixels per map pixel in the target; ceil(zoom)
};

/// The level the zoomed-out blend draws first between zoom 0.5 and 1, under
/// level 0, and alone at 0.5.
inline constexpr uint8_t far_level = 1;

/// Returns how the terrain is drawn at a zoom (TerrainDrawPlan).
///
/// @param zoom window pixels per map pixel, above 0
/// @param pixel_art_sampling the renderer draws card::Sampling::pixel_art
///        as its own mode, not as NEAREST
/// @return the plan
[[nodiscard]] TerrainDrawPlan plan_terrain_draw(float zoom, bool pixel_art_sampling) noexcept;

/// Where a view of the map lands in a target and at what scale.
struct TerrainView {
    int32_t camera_x{}; ///< map pixel at the view's left edge; below 0 left of the map
    int32_t camera_y{}; ///< map pixel at the view's top edge; below 0 above the map
    float origin_x{};   ///< target pixel the view's left edge lands on
    float origin_y{};   ///< target pixel the view's top edge lands on
    float scale{1.0F};  ///< target pixels per map pixel, above 0
    uint32_t width{};   ///< target pixels the view spans across
    uint32_t height{};  ///< target pixels the view spans down
    /// Target pixels before the view's origin, across and down, that its
    /// tiles cover too: a picture moved by a fraction of a pixel shows that
    /// much more of the map at its edge.
    uint32_t margin{};
    /// Every tile's edges on whole target pixels, each edge where its map
    /// pixel lands rounded to the nearest, so that the tiles of a zoom
    /// whose tiles span a fraction of a pixel meet on every renderer.
    bool whole_pixels{};
};

/// The tiles of the grid a view shows: columns first_column to end_column
/// less one, rows first_row to end_row less one; empty when the view lies
/// wholly before or past the map.
struct TileRange {
    uint32_t first_column{};
    uint32_t end_column{};
    uint32_t first_row{};
    uint32_t end_row{};
};

/// Returns the tiles a view shows: from the tile under the camera, or
/// under the map pixel its margin reaches before the camera, to the tile
/// under the view's far edge, within the grid; a camera before the map
/// starts at the grid's first tile.
///
/// @param atlas the map's atlas, whose grid is the map's tiles
/// @param view the view
/// @return the range; empty wholly before or past the map's edges
[[nodiscard]] TileRange
visible_tiles(const oa::present::gpu_world::TerrainAtlas& atlas, const TerrainView& view) noexcept;

/// Appends the quads of a view's tiles at a level to a frame, one batch for
/// each page the tiles read, in page order.
///
/// Each tile is a quad of tile_edge times the view's scale pixels a side at
/// origin + (tile - camera) * scale, textured by the tile's texels at the
/// level, its gutter ring left out, so that a LINEAR draw reads the gutter
/// and never a neighbour, every corner in the pass's colour (white at its
/// alpha). The tiles of one pass never overlap, so the quads are grouped by
/// page whatever their order in the grid: a view costs the card as many
/// calls as the atlas has pages, never more. The vertices are appended in
/// grid order, row by row, and each page's batch names one run of the
/// frame's indices. Tiles past the map's edge are not drawn; what the
/// clear left there, black, is what today's fill paints past the terrain.
///
/// @param[in,out] frame the frame
/// @param atlas the map's atlas
/// @param pages the executor's page for each page of the atlas, in order
/// @param view the view
/// @param pass the level, sampling, blend and alpha
/// @param target the target the quads are drawn into; none for the window
/// @param scissor the pixels of the target the draw may write; null for all
/// @return quads appended
uint32_t append_terrain_tiles(
    card::CardFrame& frame,
    const oa::present::gpu_world::TerrainAtlas& atlas,
    std::span<const card::PageHandle> pages,
    const TerrainView& view,
    const TerrainPass& pass,
    card::TargetHandle target,
    const card::Rect* scissor
);

/// The first atlas level a mosaic holds: the zoomed-out levels, 1 and 2,
/// are each drawn from the whole map's picture at that level rather than
/// tile by tile.
inline constexpr uint8_t first_mosaic_level = 1;
/// Levels that have a mosaic: from first_mosaic_level to the last tile level.
inline constexpr uint32_t mosaic_level_count =
    oa::present::gpu_world::tile_level_count - first_mosaic_level;

/// One chunk of a level's mosaic: a texture holding a part of the whole
/// map's picture at the level, and the part of it the chunk draws. Where a
/// chunk meets another, its texture holds one texel of its neighbour's
/// beyond the part it draws, so that a LINEAR draw reads across the seam as
/// across any texel; at the map's edges it holds nothing beyond, and the
/// card's clamped addressing repeats the edge texel there, as a tile's
/// gutter does.
struct MosaicChunk {
    uint32_t x{};            ///< the texture's first texel across, in texels of the level's picture
    uint32_t y{};            ///< the texture's first texel down
    uint32_t width{};        ///< texels across the texture
    uint32_t height{};       ///< texels down the texture
    uint32_t drawn_x{};      ///< the first texel the chunk draws across, in the picture's texels
    uint32_t drawn_y{};      ///< the first texel the chunk draws down
    uint32_t drawn_width{};  ///< texels across the part the chunk draws
    uint32_t drawn_height{}; ///< texels down the part the chunk draws
};

/// A level's mosaic: the whole map's picture at the level, cut into chunks
/// no wider or taller than a limit, row by row.
struct MosaicLevel {
    uint8_t level{};   ///< the atlas level, first_mosaic_level or past it
    uint32_t width{};  ///< texels across the whole picture
    uint32_t height{}; ///< texels down the whole picture
    std::vector<MosaicChunk> chunks;
};

/// Plans a level's mosaic for a grid of tiles: the picture is the grid's
/// tiles at the level, tile_edge texels each halved once per level, and it
/// is cut into as few chunks as fit `chunk_limit` texels a side, each
/// chunk's drawn part one less than the limit on a side that meets another.
///
/// @param grid_width tile columns
/// @param grid_height tile rows
/// @param level the atlas level, from first_mosaic_level to the last tile level
/// @param chunk_limit the most texels a chunk's texture may span a side, at least 3
/// @return the plan; no chunks for an empty grid, a level outside the
///         mosaic levels or a limit below 3
[[nodiscard]] MosaicLevel
plan_mosaic_level(uint32_t grid_width, uint32_t grid_height, uint8_t level, uint32_t chunk_limit);

/// Appends a pass of a view's terrain from a level's mosaic: for each chunk
/// whose drawn part the view shows, one quad of that part, clipped to the
/// view, textured by the chunk's texels at the pass's sampling, blend and
/// alpha, one batch a chunk. The view's tiles cost the card as many quads
/// as it shows chunks, whatever the zoom. Ground past the map's edges is
/// not drawn, as append_terrain_tiles leaves it.
///
/// @param[in,out] frame the frame
/// @param mosaic the level's mosaic, its level the pass's
/// @param pages the executor's page for each chunk, in order
/// @param view the view
/// @param pass the level, sampling, blend and alpha
/// @param target the target the quads are drawn into; none for the window
/// @param scissor the pixels of the target the draw may write; null for all
/// @return quads appended; 0 where the pass's level is not the mosaic's or
///         a chunk has no page
uint32_t append_terrain_mosaic(
    card::CardFrame& frame,
    const MosaicLevel& mosaic,
    std::span<const card::PageHandle> pages,
    const TerrainView& view,
    const TerrainPass& pass,
    card::TargetHandle target,
    const card::Rect* scissor
);

/// How far the map's art moves at each point of a grid of map pixels, as
/// Water ripples move the water's art (renderer_water_classic::art_motion):
/// the points `spacing` map pixels apart from first_x, first_y, `columns`
/// across and `rows` down. Between the points the motion is the bilinear
/// blend of the four around; past the grid, and where an offset is zero,
/// the art stays where the terrain's passes drew it.
struct ArtMotion {
    uint32_t spacing{}; ///< map pixels between points: 4, 8, 16 or 32, a divisor of tile_edge
    int32_t first_x{};  ///< the first column's map pixel, a multiple of spacing
    int32_t first_y{};  ///< the first row's map pixel, a multiple of spacing
    uint32_t columns{};
    uint32_t rows{};
    /// Map pixels across and down that the art at each point moves by, row by row.
    std::vector<std::array<float, 2>> offsets;

    /// Returns true when no point moves the art.
    [[nodiscard]] bool still() const noexcept {
        for (const auto& offset : offsets)
            if (offset[0] != 0.0F || offset[1] != 0.0F)
                return false;
        return true;
    }

    /// Returns how far the art at a map point moves: the blend of the grid's
    /// four points around it; zero past the grid.
    [[nodiscard]] std::array<float, 2> at(double x, double y) const noexcept {
        if (spacing == 0 || columns == 0 || rows == 0 ||
            offsets.size() < static_cast<std::size_t>(columns) * rows)
            return {};
        const double across = (x - first_x) / spacing;
        const double down = (y - first_y) / spacing;
        if (!(across >= 0.0) || !(down >= 0.0) || across > columns - 1.0 || down > rows - 1.0)
            return {};
        const uint32_t column = std::min(static_cast<uint32_t>(across), columns - 1U);
        const uint32_t row = std::min(static_cast<uint32_t>(down), rows - 1U);
        const uint32_t next_column = std::min(column + 1U, columns - 1U);
        const uint32_t next_row = std::min(row + 1U, rows - 1U);
        const auto tx = static_cast<float>(across - column);
        const auto ty = static_cast<float>(down - row);
        const auto point = [&](uint32_t c, uint32_t r) -> const std::array<float, 2>& {
            return offsets[static_cast<std::size_t>(r) * columns + c];
        };
        const auto& a = point(column, row);
        const auto& b = point(next_column, row);
        const auto& c = point(column, next_row);
        const auto& d = point(next_column, next_row);
        std::array<float, 2> blend{};
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const float upper = a[axis] + (b[axis] - a[axis]) * tx;
            const float lower = c[axis] + (d[axis] - c[axis]) * tx;
            blend[axis] = upper + (lower - upper) * ty;
        }
        return blend;
    }
};

/// Draws a view's terrain at a pass's level again where `motion` moves it,
/// over the pass's tiles: each tile, within the tiles the view shows and
/// one more on every side, cut into squares of motion.spacing map pixels,
/// and each square whose art moves drawn with its corners moved by the
/// motion at them and its texels those of where it lay, so that the art
/// itself moves. A square that does not move is left out: the tile under
/// it already shows it. The squares meet wherever their corners do, since
/// a corner's motion depends on its place alone, and a tile's corners land
/// where append_terrain_tiles lands them, so that the art meets the
/// unmoved terrain around it. One batch a page, in page order, at the
/// pass's sampling, blend and alpha.
///
/// @param[in,out] frame the frame
/// @param atlas the map's atlas
/// @param pages the executor's page for each page of the atlas, in order
/// @param view the view the pass drew
/// @param pass the pass the terrain was drawn by
/// @param motion how far the art moves
/// @param target the target the squares are drawn into; none for the window
/// @param scissor the pixels of the target the draw may write; null for all
/// @return squares appended
uint32_t append_moved_terrain_tiles(
    card::CardFrame& frame,
    const oa::present::gpu_world::TerrainAtlas& atlas,
    std::span<const card::PageHandle> pages,
    const TerrainView& view,
    const TerrainPass& pass,
    const ArtMotion& motion,
    card::TargetHandle target,
    const card::Rect* scissor
);

/// Draws a view's terrain from a level's mosaic again where `motion` moves
/// it, as append_moved_terrain_tiles draws the tiles: each chunk's drawn
/// part the view shows, a grid spacing past the view on every side, cut
/// into squares on the grid's lines and the chunk's edges, and each square
/// that moves drawn moved. One batch a chunk.
///
/// @param[in,out] frame the frame
/// @param mosaic the level's mosaic, its level the pass's
/// @param pages the executor's page for each chunk, in order
/// @param view the view the pass drew
/// @param pass the pass the terrain was drawn by
/// @param motion how far the art moves
/// @param target the target the squares are drawn into; none for the window
/// @param scissor the pixels of the target the draw may write; null for all
/// @return squares appended; 0 where the pass's level is not the mosaic's
uint32_t append_moved_terrain_mosaic(
    card::CardFrame& frame,
    const MosaicLevel& mosaic,
    std::span<const card::PageHandle> pages,
    const TerrainView& view,
    const TerrainPass& pass,
    const ArtMotion& motion,
    card::TargetHandle target,
    const card::Rect* scissor
);

} // namespace oa::app::full_terrain
