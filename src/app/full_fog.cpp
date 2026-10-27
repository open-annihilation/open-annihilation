// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "full_fog.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <utility>
#include <vector>

namespace oa::app::full_fog {

namespace gw = oa::present::gpu_world;
namespace wr = oa::present::world_renderer;

namespace {

/// Map pixels a fog tile spans: a sight cell.
constexpr int32_t tile_pixels = wr::fog_cell_pixels;
/// Map pixels a quarter of a fog tile spans: the part of one terrain tile
/// under it.
constexpr int32_t quarter_pixels = tile_pixels / 2;
/// Half way across a fog tile, as a fraction of it: where a terrain tile's
/// edge crosses it.
constexpr float half_tile = 0.5F;
/// Map pixels across half a fog tile: how far the fog mask's quad reaches
/// past the grid's tiles, where the centres of its edge texels lie.
constexpr int32_t half_tile_pixels = tile_pixels / 2;
/// The alpha the dithered pass draws a corner out of sight at.
constexpr float dither_alpha = 0.5F;
/// Indices a quad adds.
constexpr uint32_t quad_indices = 6;

static_assert(static_cast<uint32_t>(tile_pixels) == gw::tile_edge);

/// The corner bits of a fog tile in the order the alphas are given.
constexpr std::array<uint8_t, fog_tile_corners> corner_bits{
    wr::fog_corner_top_left,
    wr::fog_corner_top_right,
    wr::fog_corner_bottom_left,
    wr::fog_corner_bottom_right,
};

/// Which fog layer a pass reads.
enum class Layer : uint8_t { unseen, unmapped };

/// Returns the corners of a fog tile the greyed passes draw: those out of
/// sight, unless every corner was never mapped, since the black pass covers
/// the whole tile then.
///
/// @param tile the tile
/// @return the corner mask; 0 for a tile the passes leave alone
[[nodiscard]] uint8_t greyed_mask(const wr::FogTile& tile) noexcept {
    return tile.unmapped == wr::fog_mask_full ? 0 : tile.unseen;
}

/// Returns a tile's mask on a layer: the greyed corners, or the corners
/// never mapped.
///
/// @param tile the tile
/// @param layer the layer
/// @return the corner mask
[[nodiscard]] uint8_t mask_of(const wr::FogTile& tile, Layer layer) noexcept {
    return layer == Layer::unseen ? greyed_mask(tile) : tile.unmapped;
}

/// Tells whether a fog tile of the grid is drawn greyed at every corner.
///
/// @param grid the grid
/// @param column the tile's column; one off the grid is not
/// @param row the tile's row
/// @return true when every corner is out of sight and the black pass does
///     not cover the tile
[[nodiscard]] bool wholly_greyed(const wr::FogGrid& grid, int32_t column, int32_t row) noexcept {
    if (column < 0 || row < 0 || column >= grid.width || row >= grid.height)
        return false;
    return greyed_mask(grid.at(column, row)) == wr::fog_mask_full;
}

/// Appends a quad whose corners have their own colours, as card::append_quad
/// lays its vertices and indices out: top-left, top-right, bottom-right,
/// bottom-left, the diagonal from the top-left to the bottom-right corner.
///
/// @param[in,out] frame the frame
/// @param x the left edge, in pixels of the target
/// @param y the top edge
/// @param width pixels across
/// @param height pixels down
/// @param u0 the texture coordinate of the left edge
/// @param v0 the texture coordinate of the top edge
/// @param u1 the texture coordinate of the right edge
/// @param v1 the texture coordinate of the bottom edge
/// @param colours the corners' colours, top-left, top-right, bottom-left, bottom-right
void append_corner_quad(
    card::CardFrame& frame,
    float x,
    float y,
    float width,
    float height,
    float u0,
    float v0,
    float u1,
    float v1,
    const std::array<card::Colour, fog_tile_corners>& colours
) {
    const auto first = static_cast<card::Index>(frame.vertices.size());
    frame.vertices.push_back({x, y, colours[0], u0, v0});
    frame.vertices.push_back({x + width, y, colours[1], u1, v0});
    frame.vertices.push_back({x + width, y + height, colours[3], u1, v1});
    frame.vertices.push_back({x, y + height, colours[2], u0, v1});
    frame.indices.push_back(first);
    frame.indices.push_back(first + 1);
    frame.indices.push_back(first + 2);
    frame.indices.push_back(first);
    frame.indices.push_back(first + 2);
    frame.indices.push_back(first + 3);
}

/// Opens a draw batch of alpha-blended quads, or keeps the open one when it
/// shares the page.
///
/// @param[in,out] frame the frame
/// @param[in,out] batch the open batch; null for none
/// @param[in,out] batch_page the page the open batch draws
/// @param page the page the next quad draws; none for untextured
/// @param level the page's level
/// @param sampling how the page is read
/// @param target the target
/// @param scissor the scissor; null for none
void open_batch(
    card::CardFrame& frame,
    card::Batch*& batch,
    card::PageHandle& batch_page,
    card::PageHandle page,
    uint8_t level,
    card::Sampling sampling,
    card::TargetHandle target,
    const card::Rect* scissor
) {
    if (batch != nullptr && batch_page == page)
        return;
    frame.batches.emplace_back();
    batch = &frame.batches.back();
    batch_page = page;
    batch->operation = card::Operation::draw;
    batch->target = target;
    batch->page = page;
    batch->level = page == card::PageHandle{} ? 0 : level;
    batch->blend = card::Blend::alpha;
    batch->sampling = sampling;
    batch->scissored = scissor != nullptr;
    if (scissor != nullptr)
        batch->scissor = *scissor;
    batch->first_index = static_cast<card::Index>(frame.indices.size());
    batch->index_count = 0;
}

/// Returns the target pixel a map pixel lands on along one axis.
///
/// @param origin the target pixel the camera's map pixel lands on
/// @param map the map pixel
/// @param camera the camera's map pixel
/// @param scale target pixels per map pixel
/// @return the target pixel
[[nodiscard]] float placed(float origin, int32_t map, int32_t camera, float scale) noexcept {
    return static_cast<float>(
        static_cast<double>(origin) + static_cast<double>(map - camera) * scale
    );
}

/// Returns where a quad's edge and its span land along one axis.
///
/// @param placement the placement
/// @param horizontal across, else down
/// @param map the map pixel of the quad's first edge
/// @param span the map pixels the quad spans
/// @return the target pixel of the first edge, and the target pixels to the
///         other: on whole pixels for FogPlacement::whole_pixels
[[nodiscard]] std::pair<float, float>
placed_span(const FogPlacement& placement, bool horizontal, int32_t map, int32_t span) noexcept {
    const float origin = horizontal ? placement.origin_x : placement.origin_y;
    const int32_t camera = horizontal ? placement.camera_x : placement.camera_z;
    const float start = placed(origin, map, camera, placement.scale);
    if (!placement.whole_pixels)
        return {start, static_cast<float>(static_cast<double>(span) * placement.scale)};
    const float first = std::round(start);
    return {first, std::round(placed(origin, map + span, camera, placement.scale)) - first};
}

/// One piece of the greyed pass: a terrain tile, or a quarter of one under
/// a fog tile, with the alphas at its corners.
struct GreyedPiece {
    int32_t map_x{};     ///< the piece's left edge, in map pixels
    int32_t map_z{};     ///< the piece's top edge
    int32_t span{};      ///< map pixels a side: a tile or a quarter
    uint16_t slot{};     ///< the atlas slot of the terrain tile it lies in
    uint8_t quarter_x{}; ///< the quarter of the tile it is: 0 for the left, 1 for the right
    uint8_t quarter_z{}; ///< 0 for the top, 1 for the bottom
    bool whole{};        ///< the whole tile
    std::array<float, fog_tile_corners> alphas{};
};

/// Returns the alpha a tile's corners interpolate to at a point of the tile.
///
/// @param corners the tile's corner alphas
/// @param u how far across the tile, 0 to 1
/// @param v how far down the tile, 0 to 1
/// @return the alpha
[[nodiscard]] float
alpha_at(const std::array<float, fog_tile_corners>& corners, float u, float v) noexcept {
    return corners[0] * (1.0F - u) * (1.0F - v) + corners[1] * u * (1.0F - v) +
           corners[2] * (1.0F - u) * v + corners[3] * u * v;
}

/// Collects the pieces of the greyed pass over a grid: a whole terrain tile
/// where its four fog tiles are wholly out of sight, else the quarters of
/// terrain tiles under each fog tile drawn greyed.
///
/// @param grid the fog grid
/// @param atlas the map's atlas
/// @param placement where the grid lands
/// @param[out] pieces the pieces, in the grid's row order
void collect_greyed_pieces(
    const wr::FogGrid& grid,
    const gw::TerrainAtlas& atlas,
    const FogPlacement& placement,
    std::vector<GreyedPiece>& pieces
) {
    pieces.clear();
    const auto grid_width = static_cast<int32_t>(atlas.grid_width);
    const auto grid_height = static_cast<int32_t>(atlas.grid_height);
    for (int32_t row = 0; row < grid.height; ++row) {
        const int32_t tile_z = placement.camera_z + grid.offset_z + row * tile_pixels;
        for (int32_t column = 0; column < grid.width; ++column) {
            const uint8_t mask = greyed_mask(grid.at(column, row));
            if (mask == 0)
                continue;
            const int32_t tile_x = placement.camera_x + grid.offset_x + column * tile_pixels;
            const auto corners = corner_alphas(mask);
            for (uint8_t quarter_z = 0; quarter_z < 2; ++quarter_z)
                for (uint8_t quarter_x = 0; quarter_x < 2; ++quarter_x) {
                    const int32_t map_x = tile_x + quarter_x * quarter_pixels;
                    const int32_t map_z = tile_z + quarter_z * quarter_pixels;
                    if (map_x < 0 || map_z < 0)
                        continue;
                    const int32_t terrain_column = map_x / tile_pixels;
                    const int32_t terrain_row = map_z / tile_pixels;
                    if (terrain_column >= grid_width || terrain_row >= grid_height)
                        continue;
                    GreyedPiece piece;
                    piece.slot = atlas.grid
                                     [static_cast<std::size_t>(terrain_row) * atlas.grid_width +
                                      static_cast<std::size_t>(terrain_column)];
                    // The fog tile at the terrain tile's top-left corner, and
                    // the three after it: all wholly out of sight, the tile
                    // is drawn once, from the fog tile at its top-left.
                    const int32_t first_column = column + quarter_x - 1;
                    const int32_t first_row = row + quarter_z - 1;
                    const bool uniform = wholly_greyed(grid, first_column, first_row) &&
                                         wholly_greyed(grid, first_column + 1, first_row) &&
                                         wholly_greyed(grid, first_column, first_row + 1) &&
                                         wholly_greyed(grid, first_column + 1, first_row + 1);
                    if (uniform) {
                        if (quarter_x != 1 || quarter_z != 1)
                            continue;
                        piece.map_x = terrain_column * tile_pixels;
                        piece.map_z = terrain_row * tile_pixels;
                        piece.span = tile_pixels;
                        piece.whole = true;
                        piece.alphas.fill(1.0F);
                        pieces.push_back(piece);
                        continue;
                    }
                    piece.map_x = map_x;
                    piece.map_z = map_z;
                    piece.span = quarter_pixels;
                    // Which quarter of the terrain tile the piece is, from
                    // where it lies within the tile.
                    piece.quarter_x = static_cast<uint8_t>((map_x % tile_pixels) / quarter_pixels);
                    piece.quarter_z = static_cast<uint8_t>((map_z % tile_pixels) / quarter_pixels);
                    const float u0 = quarter_x == 0 ? 0.0F : half_tile;
                    const float v0 = quarter_z == 0 ? 0.0F : half_tile;
                    piece.alphas = {
                        alpha_at(corners, u0, v0),
                        alpha_at(corners, u0 + half_tile, v0),
                        alpha_at(corners, u0, v0 + half_tile),
                        alpha_at(corners, u0 + half_tile, v0 + half_tile),
                    };
                    pieces.push_back(piece);
                }
        }
    }
}

/// Appends a pass of solid quads over one layer of the grid: a quad for
/// each tile with any corner marked on the layer, at the corner alphas
/// times a factor, tiles wholly marked that follow one another in a row
/// merged into one.
///
/// @param[in,out] frame the frame
/// @param grid the fog grid
/// @param layer the layer read
/// @param colour the quads' colour
/// @param factor what the corner alphas are scaled by
/// @param placement where the grid lands
/// @param target the target
/// @param scissor the scissor; null for none
/// @return quads appended
uint32_t append_solid_pass(
    card::CardFrame& frame,
    const wr::FogGrid& grid,
    Layer layer,
    const card::Colour& colour,
    float factor,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
) {
    if (grid.tiles.empty())
        return 0;
    uint32_t quads = 0;
    card::Batch* batch = nullptr;
    card::PageHandle batch_page{};
    const auto coloured = [&](float alpha) {
        return card::Colour{colour.red, colour.green, colour.blue, alpha * factor};
    };
    for (int32_t row = 0; row < grid.height; ++row) {
        const int32_t tile_z = placement.camera_z + grid.offset_z + row * tile_pixels;
        const auto [y, tile_high] = placed_span(placement, false, tile_z, tile_pixels);
        for (int32_t column = 0; column < grid.width;) {
            const uint8_t mask = mask_of(grid.at(column, row), layer);
            if (mask == 0) {
                ++column;
                continue;
            }
            const int32_t tile_x = placement.camera_x + grid.offset_x + column * tile_pixels;
            const auto [x, tile_wide] = placed_span(placement, true, tile_x, tile_pixels);
            open_batch(frame, batch, batch_page, {}, 0, card::Sampling::nearest, target, scissor);
            if (mask == wr::fog_mask_full) {
                int32_t end = column + 1;
                while (end < grid.width && mask_of(grid.at(end, row), layer) == wr::fog_mask_full)
                    ++end;
                const auto run =
                    placement.whole_pixels
                        ? placed_span(placement, true, tile_x, (end - column) * tile_pixels).second
                        : placed(
                              placement.origin_x,
                              tile_x + (end - column) * tile_pixels,
                              placement.camera_x,
                              placement.scale
                          ) - x;
                card::append_quad(
                    frame, x, y, run, tile_high, 0.0F, 0.0F, 0.0F, 0.0F, coloured(1.0F)
                );
                column = end;
            } else {
                const auto alphas = corner_alphas(mask);
                append_corner_quad(
                    frame,
                    x,
                    y,
                    tile_wide,
                    tile_high,
                    0.0F,
                    0.0F,
                    0.0F,
                    0.0F,
                    {coloured(alphas[0]),
                     coloured(alphas[1]),
                     coloured(alphas[2]),
                     coloured(alphas[3])}
                );
                ++column;
            }
            batch->index_count += quad_indices;
            ++quads;
        }
    }
    return quads;
}

/// Returns where the map pixels [map, map + span) land along one axis: the
/// target pixel of the first edge and of the far one, each where its own
/// map pixel lands, so that rectangles that meet share an edge exactly.
///
/// @param placement the placement
/// @param horizontal across, else down
/// @param map the first map pixel
/// @param span the map pixels spanned
/// @return the first edge and the far edge, in target pixels: on whole
///         pixels for FogPlacement::whole_pixels
[[nodiscard]] std::pair<float, float>
placed_edges(const FogPlacement& placement, bool horizontal, int32_t map, int32_t span) noexcept {
    const float origin = horizontal ? placement.origin_x : placement.origin_y;
    const int32_t camera = horizontal ? placement.camera_x : placement.camera_z;
    const float first = placed(origin, map, camera, placement.scale);
    const float last = placed(origin, map + span, camera, placement.scale);
    if (!placement.whole_pixels)
        return {first, last};
    return {std::round(first), std::round(last)};
}

/// The variant of the FOG.GAF tile set a grid's tile is drawn from, as the
/// processor chooses it.
///
/// @param grid the grid
/// @param column the tile's column
/// @param row the tile's row
/// @return the variant, 0..3
[[nodiscard]] int32_t tile_variant(const wr::FogGrid& grid, int32_t column, int32_t row) noexcept {
    return (row + column + grid.variant_phase) & (wr::fog_tile_variants - 1);
}

/// One piece of the Classic greyed pass: a rectangle of map pixels over
/// one terrain tile.
struct ClassicPiece {
    int32_t map_x{};  ///< the piece's left edge, in map pixels
    int32_t map_z{};  ///< the piece's top edge
    int32_t width{};  ///< map pixels across
    int32_t height{}; ///< map pixels down
    int32_t in_x{};   ///< where its left edge lies within the terrain tile, 0..31
    int32_t in_z{};   ///< where its top edge lies within the terrain tile
    uint16_t slot{};  ///< the atlas slot of the terrain tile it lies in
};

/// Collects the pieces of the Classic greyed pass over a grid: a whole
/// terrain tile where its four fog tiles are wholly out of sight, the
/// quarters of terrain tiles under each other fog tile wholly out of
/// sight, and the rectangles of the gray shape of each fog tile partly out
/// of sight.
///
/// @param grid the fog grid
/// @param shapes the Classic fog's shapes
/// @param atlas the map's atlas
/// @param placement where the grid lands
/// @param[out] pieces the pieces, in the grid's row order
void collect_classic_pieces(
    const wr::FogGrid& grid,
    const ClassicShapes& shapes,
    const gw::TerrainAtlas& atlas,
    const FogPlacement& placement,
    std::vector<ClassicPiece>& pieces
) {
    pieces.clear();
    const auto grid_width = static_cast<int32_t>(atlas.grid_width);
    const auto grid_height = static_cast<int32_t>(atlas.grid_height);
    // Adds the rectangle of map pixels at (map_x, map_z) where it lies on
    // the map; it lies over one terrain tile.
    const auto add = [&](int32_t map_x, int32_t map_z, int32_t width, int32_t height) {
        if (map_x < 0 || map_z < 0)
            return;
        const int32_t terrain_column = map_x / tile_pixels;
        const int32_t terrain_row = map_z / tile_pixels;
        if (terrain_column >= grid_width || terrain_row >= grid_height)
            return;
        ClassicPiece piece;
        piece.map_x = map_x;
        piece.map_z = map_z;
        piece.width = width;
        piece.height = height;
        piece.in_x = map_x % tile_pixels;
        piece.in_z = map_z % tile_pixels;
        piece.slot = atlas.grid
                         [static_cast<std::size_t>(terrain_row) * atlas.grid_width +
                          static_cast<std::size_t>(terrain_column)];
        pieces.push_back(piece);
    };
    for (int32_t row = 0; row < grid.height; ++row) {
        const int32_t tile_z = placement.camera_z + grid.offset_z + row * tile_pixels;
        for (int32_t column = 0; column < grid.width; ++column) {
            const uint8_t mask = greyed_mask(grid.at(column, row));
            if (mask == 0)
                continue;
            const int32_t tile_x = placement.camera_x + grid.offset_x + column * tile_pixels;
            if (mask != wr::fog_mask_full) {
                for (const ShapeBox& box :
                     shapes.at(wr::FogTileSet::gray, tile_variant(grid, column, row), mask))
                    add(tile_x + box.x, tile_z + box.y, box.width, box.height);
                continue;
            }
            for (int32_t quarter_z = 0; quarter_z < 2; ++quarter_z)
                for (int32_t quarter_x = 0; quarter_x < 2; ++quarter_x) {
                    // A terrain tile whose four fog tiles are wholly out of
                    // sight is drawn once, from the fog tile at its top-left.
                    const int32_t first_column = column + quarter_x - 1;
                    const int32_t first_row = row + quarter_z - 1;
                    const bool uniform = wholly_greyed(grid, first_column, first_row) &&
                                         wholly_greyed(grid, first_column + 1, first_row) &&
                                         wholly_greyed(grid, first_column, first_row + 1) &&
                                         wholly_greyed(grid, first_column + 1, first_row + 1);
                    const int32_t map_x = tile_x + quarter_x * quarter_pixels;
                    const int32_t map_z = tile_z + quarter_z * quarter_pixels;
                    if (!uniform) {
                        add(map_x, map_z, quarter_pixels, quarter_pixels);
                        continue;
                    }
                    if (quarter_x == 1 && quarter_z == 1 && map_x >= 0 && map_z >= 0)
                        add(map_x - map_x % tile_pixels,
                            map_z - map_z % tile_pixels,
                            tile_pixels,
                            tile_pixels);
                }
        }
    }
}

/// Appends one rectangle of a fog tile as a quad of the colour, untextured
/// or textured by the same texels of a page laid over the tile.
///
/// @param[in,out] frame the frame
/// @param placement where the grid lands
/// @param map_x the rectangle's left edge, in map pixels
/// @param map_z its top edge
/// @param width map pixels across
/// @param height map pixels down
/// @param texel_x its left edge within the tile, in the page's texels; ignored untextured
/// @param texel_y its top edge within the tile
/// @param colour the quad's colour
void append_box_quad(
    card::CardFrame& frame,
    const FogPlacement& placement,
    int32_t map_x,
    int32_t map_z,
    int32_t width,
    int32_t height,
    int32_t texel_x,
    int32_t texel_y,
    const card::Colour& colour
) {
    const auto [left, right] = placed_edges(placement, true, map_x, width);
    const auto [top, bottom] = placed_edges(placement, false, map_z, height);
    constexpr auto page_edge = static_cast<float>(dither_page_edge);
    card::append_quad(
        frame,
        left,
        top,
        right - left,
        bottom - top,
        static_cast<float>(texel_x) / page_edge,
        static_cast<float>(texel_y) / page_edge,
        static_cast<float>(texel_x + width) / page_edge,
        static_cast<float>(texel_y + height) / page_edge,
        colour
    );
}

/// Target pixels within which two points of a piece are one: the points a
/// cut makes on an edge a corner already lies on.
constexpr float same_point = 1.0e-4F;

/// Cuts a convex polygon of the greyed pass by a rectangle, a side at a
/// time, each point the cut makes lying on its edge with the alpha the edge
/// interpolates to there, so that the alpha stays the polygon's linear one.
/// A point that falls on the one before it is dropped, so that a corner
/// lying on a side is kept once.
///
/// @param piece the polygon, cut in place; fewer than 3 points where
///     nothing of it lies in the rectangle
/// @param left the rectangle's left edge
/// @param top its top edge
/// @param right its right edge
/// @param bottom its bottom edge
void cut_piece(CoverPiece& piece, float left, float top, float right, float bottom) noexcept {
    // Each side: whether it bounds the columns, else the rows, the edge,
    // and whether the inside lies at or past it.
    struct Side {
        bool across{};
        float edge{};
        bool past{};
    };

    const std::array<Side, 4> sides{{
        {true, left, true},
        {true, right, false},
        {false, top, true},
        {false, bottom, false},
    }};
    // A polygon cut by one side gains at most one point.
    std::array<FogPoint, most_cover_points + 4> points{};
    uint32_t count = std::min<uint32_t>(piece.count, most_cover_points);
    std::copy_n(piece.points.begin(), count, points.begin());
    for (const Side& side : sides) {
        if (count < 3)
            break;
        const auto along = [&](const FogPoint& point) { return side.across ? point.x : point.y; };
        const auto inside = [&](const FogPoint& point) {
            return side.past ? along(point) >= side.edge : along(point) <= side.edge;
        };
        std::array<FogPoint, most_cover_points + 4> kept{};
        uint32_t kept_count = 0;
        const auto keep = [&](const FogPoint& point) {
            if (kept_count != 0 && std::abs(kept[kept_count - 1].x - point.x) < same_point &&
                std::abs(kept[kept_count - 1].y - point.y) < same_point)
                return;
            if (kept_count < kept.size())
                kept[kept_count++] = point;
        };
        for (uint32_t index = 0; index < count; ++index) {
            const FogPoint& from = points[index];
            const FogPoint& to = points[(index + 1) % count];
            const bool from_in = inside(from);
            if (from_in)
                keep(from);
            if (from_in != inside(to)) {
                const float share = (side.edge - along(from)) / (along(to) - along(from));
                keep({
                    from.x + (to.x - from.x) * share,
                    from.y + (to.y - from.y) * share,
                    from.alpha + (to.alpha - from.alpha) * share,
                });
            }
        }
        if (kept_count > 1 && std::abs(kept[kept_count - 1].x - kept[0].x) < same_point &&
            std::abs(kept[kept_count - 1].y - kept[0].y) < same_point)
            --kept_count;
        count = kept_count;
        points = kept;
    }
    // A triangle cut by four sides keeps at most seven points, a rectangle
    // four.
    piece.count = count >= 3 ? std::min<uint32_t>(count, most_cover_points) : 0;
    std::copy_n(points.begin(), piece.count, piece.points.begin());
}

} // namespace

std::array<float, fog_tile_corners> corner_alphas(uint8_t mask) noexcept {
    std::array<float, fog_tile_corners> alphas{};
    for (std::size_t corner = 0; corner < fog_tile_corners; ++corner)
        alphas[corner] = (mask & corner_bits[corner]) != 0 ? 1.0F : 0.0F;
    return alphas;
}

LevelQuad level_quad(int32_t level) noexcept {
    LevelQuad quad;
    if (level < 0) {
        const int32_t row = std::max<int32_t>(0, table_rows + level);
        const float scale = static_cast<float>(row) * shade_row_step;
        quad.colour = {0.0F, 0.0F, 0.0F, std::clamp(1.0F - scale, 0.0F, 1.0F)};
        quad.blend = card::Blend::alpha;
        return quad;
    }
    const int32_t row = std::min<int32_t>(level, table_rows - 1);
    const float scale = 1.0F + static_cast<float>(row) * light_row_step;
    quad.colour = {1.0F, 1.0F, 1.0F, std::clamp(1.0F - 1.0F / scale, 0.0F, 1.0F)};
    quad.blend = card::Blend::additive;
    return quad;
}

float pass_alpha(float alpha, float share, float later) noexcept {
    // Nothing is left for a pass whose later levels cover the corner wholly.
    const float left = 1.0F - alpha * later;
    if (!(left > 0.0F))
        return 0.0F;
    return std::clamp(alpha * share / left, 0.0F, 1.0F);
}

uint32_t append_unseen_terrain(
    card::CardFrame& frame,
    const wr::FogGrid& grid,
    const gw::TerrainAtlas& atlas,
    std::span<const card::PageHandle> greyed_pages,
    std::span<const GreyedLevel> levels,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
) {
    if (grid.tiles.empty() || levels.empty() || levels.size() > most_greyed_levels ||
        greyed_pages.size() < atlas.pages.size() || atlas.grid_width == 0 || atlas.grid_height == 0)
        return 0;
    for (const auto& level : levels)
        if (level.level >= gw::tile_level_count)
            return 0;
    // The pieces are gathered into one list a thread keeps from frame to
    // frame, which holds the room the busiest frame needed.
    thread_local std::vector<GreyedPiece> pieces;
    collect_greyed_pieces(grid, atlas, placement, pieces);
    if (pieces.empty())
        return 0;
    uint32_t quads = 0;
    for (std::size_t pass = 0; pass < levels.size(); ++pass) {
        const auto& drawn = levels[pass];
        float later = 0.0F;
        for (std::size_t after = pass + 1; after < levels.size(); ++after)
            later += levels[after].share;
        card::Batch* batch = nullptr;
        card::PageHandle batch_page{};
        for (const auto& piece : pieces) {
            const auto rect = gw::tile_rect(atlas, piece.slot, drawn.level);
            if (!rect || rect->page >= atlas.pages.size())
                continue;
            const auto& level = atlas.pages[rect->page].levels[drawn.level];
            const float level_width = static_cast<float>(level.width);
            const float level_height = static_cast<float>(level.height);
            // The texels of the piece: the tile's, or the quarter's of them.
            const uint32_t edge = piece.whole ? rect->edge : rect->edge / 2;
            const uint32_t texel_x = rect->x + (piece.whole ? 0 : piece.quarter_x * edge);
            const uint32_t texel_y = rect->y + (piece.whole ? 0 : piece.quarter_z * edge);
            open_batch(
                frame,
                batch,
                batch_page,
                greyed_pages[rect->page],
                drawn.level,
                drawn.sampling,
                target,
                scissor
            );
            std::array<card::Colour, fog_tile_corners> colours{};
            for (std::size_t corner = 0; corner < fog_tile_corners; ++corner)
                colours[corner] = {
                    1.0F, 1.0F, 1.0F, pass_alpha(piece.alphas[corner], drawn.share, later)
                };
            const auto [left, wide] = placed_span(placement, true, piece.map_x, piece.span);
            const auto [top, high] = placed_span(placement, false, piece.map_z, piece.span);
            append_corner_quad(
                frame,
                left,
                top,
                wide,
                high,
                static_cast<float>(texel_x) / level_width,
                static_cast<float>(texel_y) / level_height,
                static_cast<float>(texel_x + edge) / level_width,
                static_cast<float>(texel_y + edge) / level_height,
                colours
            );
            batch->index_count += quad_indices;
            ++quads;
        }
    }
    return quads;
}

void build_unseen_mask(const wr::FogGrid& grid, UnseenMask& mask) {
    mask.texels.clear();
    mask.any_unseen = false;
    if (grid.tiles.empty() || grid.width <= 0 || grid.height <= 0) {
        mask.width = 0;
        mask.height = 0;
        return;
    }
    // A cell for each tile corner, and the column and row that repeat the
    // last.
    const int32_t cells_across = grid.width + 1;
    const int32_t cells_down = grid.height + 1;
    mask.width = static_cast<uint32_t>(cells_across + 1);
    mask.height = static_cast<uint32_t>(cells_down + 1);
    mask.texels.assign(std::size_t{mask.width} * mask.height, card::Texel{});
    const auto cell = [&](int32_t x, int32_t z) -> card::Texel& {
        return mask.texels[static_cast<std::size_t>(z) * mask.width + static_cast<std::size_t>(x)];
    };
    // A cell is out of sight where any tile it is a corner of says so: a
    // tile that straddles the map's edge carries the corners of the cells
    // within onto those past it, and a tile wholly past the edge, which
    // nothing draws, carries none (build_fog_grid).
    constexpr card::Texel unseen{255, 255, 255, 255};
    for (int32_t row = 0; row < grid.height; ++row)
        for (int32_t column = 0; column < grid.width; ++column) {
            const uint8_t corners = grid.at(column, row).unseen;
            for (std::size_t corner = 0; corner < fog_tile_corners; ++corner)
                if ((corners & corner_bits[corner]) != 0) {
                    cell(
                        column + static_cast<int32_t>(corner % 2),
                        row + static_cast<int32_t>(corner / 2)
                    ) = unseen;
                    mask.any_unseen = true;
                }
        }
    for (int32_t z = 0; z < cells_down; ++z)
        cell(cells_across, z) = cell(cells_across - 1, z);
    for (int32_t x = 0; x <= cells_across; ++x)
        cell(x, cells_down) = cell(x, cells_down - 1);
}

uint32_t append_unseen_mask(
    card::CardFrame& frame,
    const wr::FogGrid& grid,
    const UnseenMask& mask,
    card::PageHandle page,
    uint32_t page_width,
    uint32_t page_height,
    card::Blend blend,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
) {
    if (grid.tiles.empty() || mask.width != static_cast<uint32_t>(grid.width) + 2U ||
        mask.height != static_cast<uint32_t>(grid.height) + 2U || page_width < mask.width ||
        page_height < mask.height)
        return 0;
    // Half a tile before the first tile's corner, so that the quad's
    // texels are whole ones and each cell's centre lands on its corner.
    const int32_t map_x = placement.camera_x + grid.offset_x - half_tile_pixels;
    const int32_t map_z = placement.camera_z + grid.offset_z - half_tile_pixels;
    const auto [x, wide] = placed_span(placement, true, map_x, (grid.width + 1) * tile_pixels);
    const auto [y, high] = placed_span(placement, false, map_z, (grid.height + 1) * tile_pixels);
    card::Batch* batch = nullptr;
    card::PageHandle batch_page{};
    open_batch(frame, batch, batch_page, page, 0, card::Sampling::linear, target, scissor);
    batch->blend = blend;
    card::append_quad(
        frame,
        x,
        y,
        wide,
        high,
        0.0F,
        0.0F,
        static_cast<float>(grid.width + 1) / static_cast<float>(page_width),
        static_cast<float>(grid.height + 1) / static_cast<float>(page_height),
        card::Colour{}
    );
    batch->index_count += quad_indices;
    return 1;
}

uint32_t append_unseen_dither(
    card::CardFrame& frame,
    const wr::FogGrid& grid,
    const card::Colour& dither,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
) {
    return append_solid_pass(
        frame, grid, Layer::unseen, dither, dither_alpha, placement, target, scissor
    );
}

uint32_t append_unmapped(
    card::CardFrame& frame,
    const wr::FogGrid& grid,
    const card::Colour& colour,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
) {
    return append_solid_pass(
        frame, grid, Layer::unmapped, colour, 1.0F, placement, target, scissor
    );
}

std::span<const ShapeBox>
ClassicShapes::at(wr::FogTileSet::Bank bank, int32_t variant, uint8_t mask) const noexcept {
    if (!built() || variant < 0 || variant >= wr::fog_tile_variants || mask == 0 ||
        mask >= wr::fog_mask_full)
        return {};
    const std::size_t tile = static_cast<std::size_t>(bank) * wr::FogTileSet::tiles_per_bank +
                             static_cast<std::size_t>(variant) * (wr::fog_mask_full - 1) +
                             (mask - 1U);
    return std::span<const ShapeBox>(boxes).subspan(first[tile], first[tile + 1] - first[tile]);
}

void build_classic_shapes(const wr::FogTileSet& tiles, ClassicShapes& shapes) {
    shapes.boxes.clear();
    shapes.first.clear();
    if (!tiles.loaded())
        return;

    /// A run of one row's texels of one palette index within a quarter.
    struct Run {
        uint8_t begin{};
        uint8_t end{};
        uint8_t index{};
    };

    std::vector<Run> above;
    std::vector<Run> runs;
    // The open rectangle each run of the row above grows, by its place there.
    std::vector<std::size_t> open_above;
    std::vector<std::size_t> open;
    for (const auto bank : {wr::FogTileSet::black, wr::FogTileSet::gray})
        for (int32_t variant = 0; variant < wr::fog_tile_variants; ++variant)
            for (uint8_t mask = 1; mask < wr::fog_mask_full; ++mask) {
                shapes.first.push_back(static_cast<uint32_t>(shapes.boxes.size()));
                const wr::FogTileArt& art = tiles.at(bank, variant, mask);
                for (int32_t quarter = 0; quarter < 4; ++quarter) {
                    const int32_t x0 = (quarter % 2) * quarter_pixels;
                    const int32_t y0 = (quarter / 2) * quarter_pixels;
                    above.clear();
                    open_above.clear();
                    for (int32_t y = y0; y < y0 + quarter_pixels; ++y) {
                        runs.clear();
                        open.clear();
                        const std::size_t row = static_cast<std::size_t>(y) * tile_pixels;
                        for (int32_t x = x0; x < x0 + quarter_pixels;) {
                            const std::size_t texel = row + static_cast<std::size_t>(x);
                            if (art.opaque[texel] == 0) {
                                ++x;
                                continue;
                            }
                            const uint8_t index = art.index[texel];
                            const int32_t begin = x;
                            while (x < x0 + quarter_pixels &&
                                   art.opaque[row + static_cast<std::size_t>(x)] != 0 &&
                                   art.index[row + static_cast<std::size_t>(x)] == index)
                                ++x;
                            runs.push_back(
                                {static_cast<uint8_t>(begin), static_cast<uint8_t>(x), index}
                            );
                        }
                        // A run that matches one of the row above grows its
                        // rectangle down a row; any other opens one.
                        for (const Run& run : runs) {
                            std::size_t grown = shapes.boxes.size();
                            for (std::size_t at = 0; at < above.size(); ++at)
                                if (above[at].begin == run.begin && above[at].end == run.end &&
                                    above[at].index == run.index) {
                                    grown = open_above[at];
                                    break;
                                }
                            if (grown == shapes.boxes.size())
                                shapes.boxes.push_back(
                                    {run.begin,
                                     static_cast<uint8_t>(y),
                                     static_cast<uint8_t>(run.end - run.begin),
                                     0,
                                     run.index}
                                );
                            ++shapes.boxes[grown].height;
                            open.push_back(grown);
                        }
                        above.swap(runs);
                        open_above.swap(open);
                    }
                }
            }
    shapes.first.push_back(static_cast<uint32_t>(shapes.boxes.size()));
}

void build_dither_texels(std::vector<card::Texel>& texels) {
    texels.assign(std::size_t{dither_page_edge} * dither_page_edge, card::Texel{});
    for (uint32_t y = 0; y < dither_page_edge; ++y)
        for (uint32_t x = 0; x < dither_page_edge; ++x)
            if (((x + y) & 1U) == 0)
                texels[std::size_t{y} * dither_page_edge + x] = {255, 255, 255, 255};
}

uint32_t append_unseen_terrain_classic(
    card::CardFrame& frame,
    const wr::FogGrid& grid,
    const ClassicShapes& shapes,
    const gw::TerrainAtlas& atlas,
    std::span<const card::PageHandle> greyed_pages,
    std::span<const GreyedLevel> levels,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
) {
    if (grid.tiles.empty() || !shapes.built() || levels.empty() ||
        levels.size() > most_greyed_levels || greyed_pages.size() < atlas.pages.size() ||
        atlas.grid_width == 0 || atlas.grid_height == 0)
        return 0;
    for (const auto& level : levels)
        if (level.level >= gw::tile_level_count)
            return 0;
    std::vector<ClassicPiece> pieces;
    collect_classic_pieces(grid, shapes, atlas, placement, pieces);
    if (pieces.empty())
        return 0;
    uint32_t quads = 0;
    for (std::size_t pass = 0; pass < levels.size(); ++pass) {
        const auto& drawn = levels[pass];
        float later = 0.0F;
        for (std::size_t after = pass + 1; after < levels.size(); ++after)
            later += levels[after].share;
        const card::Colour colour{1.0F, 1.0F, 1.0F, pass_alpha(1.0F, drawn.share, later)};
        card::Batch* batch = nullptr;
        card::PageHandle batch_page{};
        for (const auto& piece : pieces) {
            const auto rect = gw::tile_rect(atlas, piece.slot, drawn.level);
            if (!rect || rect->page >= atlas.pages.size())
                continue;
            const auto& level = atlas.pages[rect->page].levels[drawn.level];
            // The tile's texels a map pixel spans at the level.
            const double texels_per_pixel =
                static_cast<double>(rect->edge) / static_cast<double>(tile_pixels);
            const auto texel = [&](uint32_t start, int32_t pixels, uint32_t size) {
                return static_cast<float>(
                    (static_cast<double>(start) + pixels * texels_per_pixel) /
                    static_cast<double>(size)
                );
            };
            open_batch(
                frame,
                batch,
                batch_page,
                greyed_pages[rect->page],
                drawn.level,
                drawn.sampling,
                target,
                scissor
            );
            const auto [left, right] = placed_edges(placement, true, piece.map_x, piece.width);
            const auto [top, bottom] = placed_edges(placement, false, piece.map_z, piece.height);
            card::append_quad(
                frame,
                left,
                top,
                right - left,
                bottom - top,
                texel(rect->x, piece.in_x, level.width),
                texel(rect->y, piece.in_z, level.height),
                texel(rect->x, piece.in_x + piece.width, level.width),
                texel(rect->y, piece.in_z + piece.height, level.height),
                colour
            );
            batch->index_count += quad_indices;
            ++quads;
        }
    }
    return quads;
}

uint32_t append_unseen_dither_classic(
    card::CardFrame& frame,
    const wr::FogGrid& grid,
    const ClassicShapes& shapes,
    card::PageHandle page,
    const card::Colour& dither,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
) {
    if (grid.tiles.empty() || !shapes.built() || page == card::PageHandle{})
        return 0;
    const card::Colour colour{dither.red, dither.green, dither.blue, 1.0F};
    uint32_t quads = 0;
    card::Batch* batch = nullptr;
    card::PageHandle batch_page{};
    for (int32_t row = 0; row < grid.height; ++row) {
        const int32_t tile_z = placement.camera_z + grid.offset_z + row * tile_pixels;
        for (int32_t column = 0; column < grid.width; ++column) {
            const uint8_t mask = greyed_mask(grid.at(column, row));
            if (mask == 0)
                continue;
            const int32_t tile_x = placement.camera_x + grid.offset_x + column * tile_pixels;
            open_batch(frame, batch, batch_page, page, 0, card::Sampling::nearest, target, scissor);
            if (mask == wr::fog_mask_full) {
                append_box_quad(
                    frame, placement, tile_x, tile_z, tile_pixels, tile_pixels, 0, 0, colour
                );
                batch->index_count += quad_indices;
                ++quads;
                continue;
            }
            for (const ShapeBox& box :
                 shapes.at(wr::FogTileSet::gray, tile_variant(grid, column, row), mask)) {
                append_box_quad(
                    frame,
                    placement,
                    tile_x + box.x,
                    tile_z + box.y,
                    box.width,
                    box.height,
                    box.x,
                    box.y,
                    colour
                );
                batch->index_count += quad_indices;
                ++quads;
            }
        }
    }
    return quads;
}

uint32_t append_unmapped_classic(
    card::CardFrame& frame,
    const wr::FogGrid& grid,
    const ClassicShapes& shapes,
    const card::Colour& colour,
    std::span<const card::Colour, OA_PALETTE_COLORS> palette,
    const FogPlacement& placement,
    card::TargetHandle target,
    const card::Rect* scissor
) {
    if (grid.tiles.empty() || !shapes.built())
        return 0;
    const card::Colour fill{colour.red, colour.green, colour.blue, 1.0F};
    uint32_t quads = 0;
    card::Batch* batch = nullptr;
    card::PageHandle batch_page{};
    for (int32_t row = 0; row < grid.height; ++row) {
        const int32_t tile_z = placement.camera_z + grid.offset_z + row * tile_pixels;
        for (int32_t column = 0; column < grid.width;) {
            const uint8_t mask = grid.at(column, row).unmapped;
            if (mask == 0) {
                ++column;
                continue;
            }
            const int32_t tile_x = placement.camera_x + grid.offset_x + column * tile_pixels;
            open_batch(frame, batch, batch_page, {}, 0, card::Sampling::nearest, target, scissor);
            if (mask == wr::fog_mask_full) {
                // Tiles wholly never mapped that follow one another in a row
                // are one quad.
                int32_t end = column + 1;
                while (end < grid.width && grid.at(end, row).unmapped == wr::fog_mask_full)
                    ++end;
                append_box_quad(
                    frame,
                    placement,
                    tile_x,
                    tile_z,
                    (end - column) * tile_pixels,
                    tile_pixels,
                    0,
                    0,
                    fill
                );
                batch->index_count += quad_indices;
                ++quads;
                column = end;
                continue;
            }
            for (const ShapeBox& box :
                 shapes.at(wr::FogTileSet::black, tile_variant(grid, column, row), mask)) {
                const card::Colour& texels = palette[box.index];
                append_box_quad(
                    frame,
                    placement,
                    tile_x + box.x,
                    tile_z + box.y,
                    box.width,
                    box.height,
                    0,
                    0,
                    {texels.red, texels.green, texels.blue, 1.0F}
                );
                batch->index_count += quad_indices;
                ++quads;
            }
            ++column;
        }
    }
    return quads;
}

Cover greyed_cover(
    const wr::FogGrid& grid,
    const ClassicShapes* shapes,
    const FogPlacement& placement,
    float left,
    float top,
    float right,
    float bottom,
    std::vector<CoverPiece>& pieces
) {
    pieces.clear();
    if (grid.tiles.empty() || grid.width <= 0 || grid.height <= 0 || !(left < right) ||
        !(top < bottom) || !(placement.scale > 0.0F) || (shapes != nullptr && !shapes->built()))
        return Cover::none;
    // The tiles the rectangle reaches: column c spans the map pixels from
    // the camera's, past the grid's offset, plus c tiles.
    const auto tile_of = [&](float target, float origin, int32_t offset) {
        const double map = (static_cast<double>(target) - origin) / placement.scale - offset;
        return static_cast<int32_t>(std::floor(map / tile_pixels));
    };
    const int32_t first_column = tile_of(left, placement.origin_x, grid.offset_x);
    const int32_t last_column = tile_of(right, placement.origin_x, grid.offset_x);
    const int32_t first_row = tile_of(top, placement.origin_y, grid.offset_z);
    const int32_t last_row = tile_of(bottom, placement.origin_y, grid.offset_z);
    // Wholly covered only where every tile it reaches is on the grid and
    // wholly out of sight.
    bool whole =
        first_column >= 0 && first_row >= 0 && last_column < grid.width && last_row < grid.height;
    bool any = false;
    const auto add_box = [&](int32_t map_x, int32_t map_z, int32_t width, int32_t height) {
        const auto [box_left, box_right] = placed_edges(placement, true, map_x, width);
        const auto [box_top, box_bottom] = placed_edges(placement, false, map_z, height);
        CoverPiece piece;
        piece.points[0] = {box_left, box_top, 1.0F};
        piece.points[1] = {box_right, box_top, 1.0F};
        piece.points[2] = {box_right, box_bottom, 1.0F};
        piece.points[3] = {box_left, box_bottom, 1.0F};
        piece.count = 4;
        cut_piece(piece, left, top, right, bottom);
        if (piece.count >= 3)
            pieces.push_back(piece);
    };
    for (int32_t row = std::max(first_row, 0); row <= std::min(last_row, grid.height - 1); ++row) {
        const int32_t tile_z = placement.camera_z + grid.offset_z + row * tile_pixels;
        for (int32_t column = std::max(first_column, 0);
             column <= std::min(last_column, grid.width - 1);
             ++column) {
            const int32_t tile_x = placement.camera_x + grid.offset_x + column * tile_pixels;
            // A tile the rectangle only touches at its edge takes no part.
            const auto [tile_left, tile_right] = placed_edges(placement, true, tile_x, tile_pixels);
            const auto [tile_top, tile_bottom] =
                placed_edges(placement, false, tile_z, tile_pixels);
            if (tile_right <= left || tile_left >= right || tile_bottom <= top ||
                tile_top >= bottom)
                continue;
            const uint8_t mask = greyed_mask(grid.at(column, row));
            if (mask != wr::fog_mask_full)
                whole = false;
            if (mask == 0)
                continue;
            any = true;
            if (shapes != nullptr) {
                if (mask == wr::fog_mask_full) {
                    add_box(tile_x, tile_z, tile_pixels, tile_pixels);
                    continue;
                }
                for (const ShapeBox& box :
                     shapes->at(wr::FogTileSet::gray, tile_variant(grid, column, row), mask))
                    add_box(tile_x + box.x, tile_z + box.y, box.width, box.height);
                continue;
            }
            const auto corners = corner_alphas(mask);
            for (int32_t quarter_z = 0; quarter_z < 2; ++quarter_z)
                for (int32_t quarter_x = 0; quarter_x < 2; ++quarter_x) {
                    const auto [quarter_left, quarter_right] = placed_edges(
                        placement, true, tile_x + quarter_x * quarter_pixels, quarter_pixels
                    );
                    const auto [quarter_top, quarter_bottom] = placed_edges(
                        placement, false, tile_z + quarter_z * quarter_pixels, quarter_pixels
                    );
                    if (quarter_right <= left || quarter_left >= right || quarter_bottom <= top ||
                        quarter_top >= bottom)
                        continue;
                    const float u0 = quarter_x == 0 ? 0.0F : half_tile;
                    const float v0 = quarter_z == 0 ? 0.0F : half_tile;
                    const FogPoint top_left{quarter_left, quarter_top, alpha_at(corners, u0, v0)};
                    const FogPoint top_right{
                        quarter_right, quarter_top, alpha_at(corners, u0 + half_tile, v0)
                    };
                    const FogPoint bottom_left{
                        quarter_left, quarter_bottom, alpha_at(corners, u0, v0 + half_tile)
                    };
                    const FogPoint bottom_right{
                        quarter_right,
                        quarter_bottom,
                        alpha_at(corners, u0 + half_tile, v0 + half_tile)
                    };
                    // The quad's two triangles, as append_corner_quad lays them.
                    for (const auto& triangle :
                         {std::array<FogPoint, 3>{top_left, top_right, bottom_right},
                          std::array<FogPoint, 3>{top_left, bottom_right, bottom_left}}) {
                        CoverPiece piece;
                        std::copy(triangle.begin(), triangle.end(), piece.points.begin());
                        piece.count = 3;
                        cut_piece(piece, left, top, right, bottom);
                        if (piece.count >= 3)
                            pieces.push_back(piece);
                    }
                }
        }
    }
    if (!any)
        return Cover::none;
    if (whole) {
        pieces.clear();
        return Cover::whole;
    }
    return Cover::part;
}

} // namespace oa::app::full_fog
