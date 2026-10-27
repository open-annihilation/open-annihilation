// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Full tier's fog passes over a seeded map, from fog grids built by the
// processor's own builder over synthetic sight: the corner alphas and the
// alphas of levels drawn one over another; the greyed pass covering exactly
// the map pixels under the fog tiles drawn greyed, once each, whole terrain
// tiles where their four fog tiles are wholly greyed and quarters
// elsewhere, every quad's corner alpha the fog tile's own interpolated to
// that corner, its texels the right part of its tile on the greyed pages,
// its batches one for each run on a page, at an aligned camera and a
// camera between tiles, through a placement, and at two levels; the black
// pass and the dithered pass with their runs merged and their alphas; the
// fog mask, its texels the cells at the tile corners, read LINEAR through
// its quad to the alphas the corners interpolate to; what is refused; and
// the Classic fog: its shapes covering the FOG.GAF art, the dither's
// checkerboard, and its black, greyed and dithered passes drawing exactly
// the processor's tiles.
#include "full_fog.hpp"

#include "oa/present/world_renderer/world_fog.hpp"
#include "oa/sim/visibility_state.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace {

namespace fog = oa::app::full_fog;
namespace gw = oa::present::gpu_world;
namespace wr = oa::present::world_renderer;
namespace card = oa::app::card;
using oa::formats::tnt::Map;

/// Seed of the test's map and sight.
constexpr uint32_t test_seed = 20261003;
/// The seeded map's tiles each way.
constexpr uint32_t map_tiles_wide = 9;
constexpr uint32_t map_tiles_high = 7;
/// Map pixels in a tile edge, and bytes in a tile.
constexpr int32_t tile_edge = static_cast<int32_t>(gw::tile_edge);
constexpr std::size_t tile_bytes = oa::formats::tnt::layout::tile_bytes;
/// The view's size in map pixels: four tiles and a half each way.
constexpr int32_t view_width = 144;
constexpr int32_t view_height = 144;
/// How near two floats must be to count as equal.
constexpr float tolerance = 1e-5F;
/// The viewer's player, whose bit the mapped cells carry.
constexpr uint8_t viewer = 3;
/// Vertices and indices a quad adds.
constexpr std::size_t quad_vertices = 4;
constexpr std::size_t quad_indices = 6;

/// A seeded map whose every tile is distinct.
///
/// @param random the source of the tiles' pixels
/// @return the map
Map test_map(std::mt19937& random) {
    Map map;
    map.tile_width = map_tiles_wide;
    map.tile_height = map_tiles_high;
    map.tile_count = map_tiles_wide * map_tiles_high;
    map.tile_palette_indices.resize(static_cast<std::size_t>(map.tile_count) * tile_bytes);
    for (auto& index : map.tile_palette_indices)
        index = static_cast<uint8_t>(random());
    map.tile_indices.resize(static_cast<std::size_t>(map.tile_count));
    for (std::size_t cell = 0; cell < map.tile_indices.size(); ++cell)
        map.tile_indices[cell] = static_cast<uint16_t>(cell);
    return map;
}

/// The map's atlas, with the greyed pages' handles numbered from 1.
struct Atlas {
    gw::TerrainAtlas atlas;
    std::vector<card::PageHandle> pages;
};

/// Builds the seeded map's atlas within the largest page edge.
///
/// @return the atlas and a handle for each page
Atlas build_atlas() {
    std::mt19937 random(test_seed);
    const Map map = test_map(random);
    oa::PaletteBytes palette{};
    for (std::size_t byte = 0; byte < palette.size(); ++byte)
        palette[byte] = static_cast<uint8_t>(random());
    Atlas built;
    OA_CHECK(
        gw::build_terrain_atlas(
            map, map.tile_width, map.tile_height, palette, nullptr, gw::page_edge_limit, built.atlas
        ) == gw::TerrainAtlasError::none
    );
    for (std::size_t page = 0; page < built.atlas.pages.size(); ++page)
        built.pages.push_back({static_cast<uint32_t>(page + 1)});
    return built;
}

/// Synthetic sight over the map: a cell for each tile.
struct Sight {
    oa::sim::visibility_state::PlayerSightGrid grid;
    std::vector<uint16_t> coverage;

    /// Makes every cell seen and mapped.
    Sight() {
        grid.width = static_cast<int32_t>(map_tiles_wide);
        grid.height = static_cast<int32_t>(map_tiles_high);
        grid.viewpoint_player = viewer;
        const auto cells = static_cast<std::size_t>(grid.width) * grid.height;
        coverage.assign(cells, 1);
        grid.player_bits.assign(cells, static_cast<uint16_t>(1U << viewer));
    }

    /// Returns a cell's index.
    ///
    /// @param x the cell's column
    /// @param z the cell's row
    /// @return the index into coverage and player_bits
    [[nodiscard]] std::size_t cell(int32_t x, int32_t z) const {
        return static_cast<std::size_t>(z) * static_cast<std::size_t>(grid.width) +
               static_cast<std::size_t>(x);
    }

    /// Takes a cell out of sight.
    ///
    /// @param x the cell's column
    /// @param z the cell's row
    void unsee(int32_t x, int32_t z) { coverage[cell(x, z)] = 0; }

    /// Takes a cell's mapping away, and its sight.
    ///
    /// @param x the cell's column
    /// @param z the cell's row
    void unmap(int32_t x, int32_t z) {
        coverage[cell(x, z)] = 0;
        grid.player_bits[cell(x, z)] = 0;
    }

    /// Builds the fog grid for a camera over the view.
    ///
    /// @param camera_x map pixel at the view's left edge
    /// @param camera_z map pixel at the view's top edge
    /// @return the grid
    [[nodiscard]] wr::FogGrid fog(int32_t camera_x, int32_t camera_z) const {
        return wr::build_fog_grid(
            grid,
            grid.viewpoint_player,
            coverage,
            {true, true},
            camera_x,
            camera_z,
            view_width,
            view_height
        );
    }
};

/// Tells whether two floats are within tolerance.
///
/// @param a a float
/// @param b another
/// @return true when they are near
[[nodiscard]] bool near(float a, float b) {
    return std::fabs(a - b) <= tolerance;
}

/// A quad of a frame, read back from its vertices.
struct Quad {
    std::array<card::Vertex, quad_vertices>
        corners{}; ///< top-left, top-right, bottom-right, bottom-left
    std::size_t batch{};
};

/// Reads the quads of a frame's draw batches, each batch's indices as runs
/// of six, checking the index pattern of a quad.
///
/// @param frame the frame
/// @return the quads in index order
std::vector<Quad> quads_of(const card::CardFrame& frame) {
    std::vector<Quad> quads;
    for (std::size_t batch = 0; batch < frame.batches.size(); ++batch) {
        const auto& drawn = frame.batches[batch];
        OA_CHECK(drawn.operation == card::Operation::draw);
        OA_CHECK(drawn.index_count % quad_indices == 0);
        for (card::Index at = drawn.first_index;
             at + quad_indices <= drawn.first_index + drawn.index_count;
             at += quad_indices) {
            const auto first = frame.indices[at];
            const bool pattern =
                frame.indices[at + 1] == first + 1 && frame.indices[at + 2] == first + 2 &&
                frame.indices[at + 3] == first && frame.indices[at + 4] == first + 2 &&
                frame.indices[at + 5] == first + 3;
            OA_CHECK(pattern);
            Quad quad;
            for (std::size_t corner = 0; corner < quad_vertices; ++corner)
                quad.corners[corner] = frame.vertices[first + corner];
            quad.batch = batch;
            quads.push_back(quad);
        }
    }
    return quads;
}

/// The placement of a view at scale 1 with the camera's map pixel at the
/// target's origin.
///
/// @param camera_x map pixel at the view's left edge
/// @param camera_z map pixel at the view's top edge
/// @return the placement
fog::FogPlacement plain(int32_t camera_x, int32_t camera_z) {
    fog::FogPlacement placement;
    placement.camera_x = camera_x;
    placement.camera_z = camera_z;
    return placement;
}

/// The corner alphas a fog tile's mask gives, as the grid's painter reads
/// them: bit 1 top-left, 2 top-right, 4 bottom-left, 8 bottom-right.
void corner_alphas_follow_the_mask() {
    const auto alphas = [](uint8_t mask) { return fog::corner_alphas(mask); };
    OA_CHECK((alphas(0) == std::array<float, 4>{0, 0, 0, 0}));
    OA_CHECK((alphas(wr::fog_corner_top_left) == std::array<float, 4>{1, 0, 0, 0}));
    OA_CHECK((alphas(wr::fog_corner_top_right) == std::array<float, 4>{0, 1, 0, 0}));
    OA_CHECK((alphas(wr::fog_corner_bottom_left) == std::array<float, 4>{0, 0, 1, 0}));
    OA_CHECK((alphas(wr::fog_corner_bottom_right) == std::array<float, 4>{0, 0, 0, 1}));
    OA_CHECK((alphas(wr::fog_mask_full) == std::array<float, 4>{1, 1, 1, 1}));
    OA_CHECK(
        (alphas(wr::fog_corner_top_right | wr::fog_corner_bottom_left) ==
         std::array<float, 4>{0, 1, 1, 0})
    );
}

/// Levels drawn one over another at pass_alpha's alphas leave the corner's
/// alpha of their weighted picture over the ground: a corner wholly out of
/// sight shows the levels' blend alone, one in sight the ground alone.
void pass_alphas_compose_the_weighted_picture() {
    // One level: the corner's alpha itself.
    OA_CHECK(near(fog::pass_alpha(0.0F, 1.0F, 0.0F), 0.0F));
    OA_CHECK(near(fog::pass_alpha(0.5F, 1.0F, 0.0F), 0.5F));
    OA_CHECK(near(fog::pass_alpha(1.0F, 1.0F, 0.0F), 1.0F));
    // Two levels, the first at share t, the second at 1 - t: the first
    // pass's alpha is 1 at a fogged corner whatever t, since the second
    // then blends the rest in.
    for (const float t : {0.0F, 0.3F, 0.5F, 0.7F, 1.0F}) {
        OA_CHECK(near(fog::pass_alpha(1.0F, t, 1.0F - t), t == 0.0F ? 0.0F : 1.0F));
        OA_CHECK(near(fog::pass_alpha(1.0F, 1.0F - t, 0.0F), 1.0F - t));
        for (const float alpha : {0.0F, 0.25F, 0.5F, 0.75F, 1.0F}) {
            const float first = fog::pass_alpha(alpha, t, 1.0F - t);
            const float second = fog::pass_alpha(alpha, 1.0F - t, 0.0F);
            // Two levels' colours over the ground, drawn in order.
            const float level_1 = 0.8F;
            const float level_0 = 0.2F;
            const float ground = 0.6F;
            const float after_first = first * level_1 + (1.0F - first) * ground;
            const float after_second = second * level_0 + (1.0F - second) * after_first;
            const float wanted =
                alpha * (t * level_1 + (1.0F - t) * level_0) + (1.0F - alpha) * ground;
            OA_CHECK(near(after_second, wanted));
            OA_CHECK(first >= 0.0F && first <= 1.0F && second >= 0.0F && second <= 1.0F);
        }
    }
}

/// What the greyed pass should cover: for each map pixel of the view's
/// margin, the alpha its fog tile's corners interpolate to, or none where
/// the tile is not drawn greyed or the pixel lies past the map.
struct GreyedField {
    int32_t left{}; ///< the first map pixel across the field holds
    int32_t top{};
    int32_t width{};
    int32_t height{};
    std::vector<float> alpha;   ///< the alpha at each map pixel's centre; -1 for none
    std::vector<uint8_t> drawn; ///< 1 where a pixel lies under a tile drawn greyed

    /// Returns the index of a map pixel.
    ///
    /// @param map_x the pixel's column
    /// @param map_z the pixel's row
    /// @return the index into alpha and drawn
    [[nodiscard]] std::size_t at(int32_t map_x, int32_t map_z) const {
        return static_cast<std::size_t>(map_z - top) * static_cast<std::size_t>(width) +
               static_cast<std::size_t>(map_x - left);
    }

    /// Tells whether a map pixel lies in the field.
    ///
    /// @param map_x the pixel's column
    /// @param map_z the pixel's row
    /// @return true when it does
    [[nodiscard]] bool holds(int32_t map_x, int32_t map_z) const {
        return map_x >= left && map_z >= top && map_x < left + width && map_z < top + height;
    }
};

/// Works out the greyed field of a grid: a pixel under a fog tile with a
/// corner out of sight, not wholly never mapped, within the map.
///
/// @param grid the fog grid
/// @param camera_x map pixel at the view's left edge
/// @param camera_z map pixel at the view's top edge
/// @return the field over the grid's tiles
GreyedField greyed_field(const wr::FogGrid& grid, int32_t camera_x, int32_t camera_z) {
    GreyedField field;
    field.left = camera_x + grid.offset_x;
    field.top = camera_z + grid.offset_z;
    field.width = grid.width * tile_edge;
    field.height = grid.height * tile_edge;
    const auto pixels =
        static_cast<std::size_t>(field.width) * static_cast<std::size_t>(field.height);
    field.alpha.assign(pixels, -1.0F);
    field.drawn.assign(pixels, 0);
    const int32_t map_width = static_cast<int32_t>(map_tiles_wide) * tile_edge;
    const int32_t map_height = static_cast<int32_t>(map_tiles_high) * tile_edge;
    for (int32_t row = 0; row < grid.height; ++row)
        for (int32_t column = 0; column < grid.width; ++column) {
            const auto& tile = grid.at(column, row);
            if (tile.unseen == 0 || tile.unmapped == wr::fog_mask_full)
                continue;
            const auto corners = fog::corner_alphas(tile.unseen);
            for (int32_t z = 0; z < tile_edge; ++z)
                for (int32_t x = 0; x < tile_edge; ++x) {
                    const int32_t map_x = field.left + column * tile_edge + x;
                    const int32_t map_z = field.top + row * tile_edge + z;
                    if (map_x < 0 || map_z < 0 || map_x >= map_width || map_z >= map_height)
                        continue;
                    const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(tile_edge);
                    const float v = (static_cast<float>(z) + 0.5F) / static_cast<float>(tile_edge);
                    const auto index = field.at(map_x, map_z);
                    field.drawn[index] = 1;
                    field.alpha[index] = corners[0] * (1.0F - u) * (1.0F - v) +
                                         corners[1] * u * (1.0F - v) + corners[2] * (1.0F - u) * v +
                                         corners[3] * u * v;
                }
        }
    return field;
}

/// Returns the alpha the field's fog tile interpolates to at a map point
/// on a tile's or a quarter's corner, which lies on a fog tile's corner,
/// edge or centre: the mean of the alphas of the pixels around the point
/// that one fog tile holds, which for the piecewise bilinear field is the
/// value at the point.
///
/// @param grid the fog grid
/// @param camera_x map pixel at the view's left edge
/// @param camera_z map pixel at the view's top edge
/// @param map_x the point's column, a multiple of half a tile
/// @param map_z the point's row
/// @param inside_x which side of the point the fog tile lies: 0 for the left
/// @param inside_z 0 for above
/// @return the alpha
float field_alpha_at(
    const wr::FogGrid& grid,
    int32_t camera_x,
    int32_t camera_z,
    int32_t map_x,
    int32_t map_z,
    int inside_x,
    int inside_z
) {
    const int32_t left = camera_x + grid.offset_x;
    const int32_t top = camera_z + grid.offset_z;
    // The fog tile holding the point on the side asked for.
    const int32_t column = (map_x - left - (inside_x == 0 ? 1 : 0)) / tile_edge;
    const int32_t row = (map_z - top - (inside_z == 0 ? 1 : 0)) / tile_edge;
    if (column < 0 || row < 0 || column >= grid.width || row >= grid.height)
        return 0.0F;
    const auto& tile = grid.at(column, row);
    const uint8_t mask = tile.unmapped == wr::fog_mask_full ? 0 : tile.unseen;
    const auto corners = fog::corner_alphas(mask);
    const float u =
        static_cast<float>(map_x - left - column * tile_edge) / static_cast<float>(tile_edge);
    const float v =
        static_cast<float>(map_z - top - row * tile_edge) / static_cast<float>(tile_edge);
    return corners[0] * (1.0F - u) * (1.0F - v) + corners[1] * u * (1.0F - v) +
           corners[2] * (1.0F - u) * v + corners[3] * u * v;
}

/// Checks the greyed pass of a grid at one level against the field: every
/// pixel drawn greyed covered once, no other pixel covered, each quad a
/// whole tile or a quarter at its tile's texels, its corner alphas the
/// field's, whole tiles exactly where their four fog tiles are wholly
/// greyed, and one batch for each run of quads on a page.
///
/// @param built the atlas
/// @param grid the fog grid
/// @param camera_x map pixel at the view's left edge
/// @param camera_z map pixel at the view's top edge
/// @param level the level drawn
/// @param sampling how it is sampled
/// @return the quads drawn
std::size_t check_greyed_pass(
    const Atlas& built,
    const wr::FogGrid& grid,
    int32_t camera_x,
    int32_t camera_z,
    uint8_t level,
    card::Sampling sampling
) {
    card::CardFrame frame;
    const fog::GreyedLevel levels[] = {{level, sampling, 1.0F}};
    const card::Rect scissor{3, 4, 100, 90};
    const card::TargetHandle target{7};
    const uint32_t appended = fog::append_unseen_terrain(
        frame, grid, built.atlas, built.pages, levels, plain(camera_x, camera_z), target, &scissor
    );
    const auto quads = quads_of(frame);
    OA_CHECK(quads.size() == appended);
    OA_CHECK(frame.vertices.size() == quads.size() * quad_vertices);
    OA_CHECK(frame.indices.size() == quads.size() * quad_indices);
    const GreyedField field = greyed_field(grid, camera_x, camera_z);
    std::vector<uint8_t> covered(field.drawn.size(), 0);
    const auto& atlas = built.atlas;
    std::size_t whole = 0;
    for (const auto& quad : quads) {
        const auto& batch = frame.batches[quad.batch];
        OA_CHECK(batch.target == target && batch.scissored && batch.scissor.x == scissor.x);
        OA_CHECK(batch.blend == card::Blend::alpha && batch.sampling == sampling);
        OA_CHECK(batch.level == level);
        // The quad's map rectangle, at scale 1 from the camera.
        const auto map_x = static_cast<int32_t>(std::lround(quad.corners[0].x)) + camera_x;
        const auto map_z = static_cast<int32_t>(std::lround(quad.corners[0].y)) + camera_z;
        const auto span = static_cast<int32_t>(std::lround(quad.corners[1].x - quad.corners[0].x));
        OA_CHECK(near(quad.corners[2].y - quad.corners[0].y, static_cast<float>(span)));
        OA_CHECK(span == tile_edge || span == tile_edge / 2);
        OA_CHECK(map_x % span == 0 && map_z % span == 0);
        if (span == tile_edge)
            ++whole;
        // Covered once.
        for (int32_t z = map_z; z < map_z + span; ++z)
            for (int32_t x = map_x; x < map_x + span; ++x) {
                OA_CHECK(field.holds(x, z));
                if (!field.holds(x, z))
                    continue;
                OA_CHECK(field.drawn[field.at(x, z)] == 1);
                OA_CHECK(covered[field.at(x, z)] == 0);
                covered[field.at(x, z)] = 1;
            }
        // The texels: the tile at the map position, whole or its quarter.
        const int32_t tile_column = map_x / tile_edge;
        const int32_t tile_row = map_z / tile_edge;
        const auto slot = atlas.grid
                              [static_cast<std::size_t>(tile_row) * atlas.grid_width +
                               static_cast<std::size_t>(tile_column)];
        const auto rect = gw::tile_rect(atlas, slot, level);
        OA_CHECK(rect.has_value());
        if (!rect)
            continue;
        OA_CHECK(batch.page == built.pages[rect->page]);
        const auto& page_level = atlas.pages[rect->page].levels[level];
        const float texel_x = quad.corners[0].u * static_cast<float>(page_level.width);
        const float texel_y = quad.corners[0].v * static_cast<float>(page_level.height);
        const float texel_right = quad.corners[1].u * static_cast<float>(page_level.width);
        const float texel_bottom = quad.corners[2].v * static_cast<float>(page_level.height);
        const uint32_t edge = span == tile_edge ? rect->edge : rect->edge / 2;
        const uint32_t quarter_x =
            span == tile_edge ? 0 : static_cast<uint32_t>(map_x % tile_edge) / (tile_edge / 2);
        const uint32_t quarter_z =
            span == tile_edge ? 0 : static_cast<uint32_t>(map_z % tile_edge) / (tile_edge / 2);
        OA_CHECK(near(texel_x, static_cast<float>(rect->x + quarter_x * edge)));
        OA_CHECK(near(texel_y, static_cast<float>(rect->y + quarter_z * edge)));
        OA_CHECK(near(texel_right, static_cast<float>(rect->x + quarter_x * edge + edge)));
        OA_CHECK(near(texel_bottom, static_cast<float>(rect->y + quarter_z * edge + edge)));
        // The corner alphas: the field's at each corner, taken from inside
        // the quad, where the fog tile holding the corner is the quad's.
        const std::array<std::pair<int32_t, int32_t>, 4> at{
            {{map_x, map_z},
             {map_x + span, map_z},
             {map_x + span, map_z + span},
             {map_x, map_z + span}}
        };
        const std::array<std::pair<int, int>, 4> inside{{{1, 1}, {0, 1}, {0, 0}, {1, 0}}};
        for (std::size_t corner = 0; corner < quad_vertices; ++corner) {
            const auto& vertex = quad.corners[corner];
            OA_CHECK(
                vertex.colour.red == 1.0F && vertex.colour.green == 1.0F &&
                vertex.colour.blue == 1.0F
            );
            float wanted = 0.0F;
            if (span == tile_edge) {
                // A whole tile's corners lie at fog tile centres, every
                // corner of those tiles out of sight.
                wanted = 1.0F;
            } else {
                wanted = field_alpha_at(
                    grid,
                    camera_x,
                    camera_z,
                    at[corner].first,
                    at[corner].second,
                    inside[corner].first,
                    inside[corner].second
                );
            }
            OA_CHECK(near(vertex.colour.alpha, wanted));
        }
    }
    // Every greyed pixel covered.
    OA_CHECK(covered == field.drawn);
    // Whole tiles exactly where the four fog tiles around a terrain tile
    // are wholly greyed.
    std::size_t wanted_whole = 0;
    const int32_t left = camera_x + grid.offset_x;
    const int32_t top = camera_z + grid.offset_z;
    for (int32_t tile_row = 0; tile_row < static_cast<int32_t>(map_tiles_high); ++tile_row)
        for (int32_t tile_column = 0; tile_column < static_cast<int32_t>(map_tiles_wide);
             ++tile_column) {
            // The fog tile at the terrain tile's top-left corner.
            const int32_t column = (tile_column * tile_edge - left - tile_edge / 2) / tile_edge;
            const int32_t row = (tile_row * tile_edge - top - tile_edge / 2) / tile_edge;
            if (tile_column * tile_edge - left - tile_edge / 2 < 0 ||
                tile_row * tile_edge - top - tile_edge / 2 < 0)
                continue;
            bool all = true;
            for (int32_t dz = 0; dz < 2 && all; ++dz)
                for (int32_t dx = 0; dx < 2 && all; ++dx) {
                    if (column + dx >= grid.width || row + dz >= grid.height) {
                        all = false;
                        break;
                    }
                    const auto& tile = grid.at(column + dx, row + dz);
                    all = tile.unseen == wr::fog_mask_full && tile.unmapped != wr::fog_mask_full;
                }
            if (all)
                ++wanted_whole;
        }
    OA_CHECK(whole == wanted_whole);
    // Batches: one for each run of quads on a page, each run's indices
    // following the one before.
    for (std::size_t batch = 1; batch < frame.batches.size(); ++batch) {
        OA_CHECK(frame.batches[batch].page != frame.batches[batch - 1].page);
        OA_CHECK(
            frame.batches[batch].first_index ==
            frame.batches[batch - 1].first_index + frame.batches[batch - 1].index_count
        );
    }
    return quads.size();
}

/// Sight with a band of cells out of sight, a corner never mapped and a
/// lone cell out of sight in the middle, at a seeded scatter of others.
///
/// @return the sight
Sight mixed_sight() {
    Sight sight;
    std::mt19937 random(test_seed + 1);
    for (int32_t z = 0; z < sight.grid.height; ++z)
        for (int32_t x = 0; x < sight.grid.width; ++x) {
            if (x <= 1)
                sight.unsee(x, z);
            if (x >= 7 && z >= 5)
                sight.unmap(x, z);
            if (random() % 5 == 0)
                sight.unsee(x, z);
        }
    sight.unsee(4, 3);
    return sight;
}

/// Nothing is drawn where everything is seen; everything in view where
/// nothing is, as whole tiles but at the map's edges.
void greyed_pass_covers_what_is_out_of_sight() {
    const Atlas built = build_atlas();
    // Everything seen: nothing.
    {
        const Sight seen;
        OA_CHECK(check_greyed_pass(built, seen.fog(0, 0), 0, 0, 0, card::Sampling::nearest) == 0);
    }
    // Nothing seen, the camera on a tile: whole tiles.
    {
        Sight sight;
        for (int32_t z = 0; z < sight.grid.height; ++z)
            for (int32_t x = 0; x < sight.grid.width; ++x)
                sight.unsee(x, z);
        const auto quads =
            check_greyed_pass(built, sight.fog(0, 0), 0, 0, 0, card::Sampling::nearest);
        OA_CHECK(quads > 0);
    }
    // A mixed pattern at an aligned camera, at one between tiles, and at
    // level 1.
    {
        const Sight sight = mixed_sight();
        OA_CHECK(
            check_greyed_pass(built, sight.fog(32, 64), 32, 64, 0, card::Sampling::nearest) > 0
        );
        OA_CHECK(check_greyed_pass(built, sight.fog(37, 9), 37, 9, 0, card::Sampling::linear) > 0);
        OA_CHECK(check_greyed_pass(built, sight.fog(37, 9), 37, 9, 1, card::Sampling::linear) > 0);
        OA_CHECK(check_greyed_pass(built, sight.fog(0, 0), 0, 0, 2, card::Sampling::nearest) > 0);
    }
}

/// Two levels draw the same quads twice, the first at pass_alpha's alphas
/// for its share and the second at the rest.
void two_levels_draw_the_quads_twice() {
    const Atlas built = build_atlas();
    const Sight sight = mixed_sight();
    const auto grid = sight.fog(37, 9);
    const float t = 0.3F;
    card::CardFrame one;
    const fog::GreyedLevel single[] = {{0, card::Sampling::linear, 1.0F}};
    const uint32_t alone = fog::append_unseen_terrain(
        one, grid, built.atlas, built.pages, single, plain(37, 9), {}, nullptr
    );
    card::CardFrame two;
    const fog::GreyedLevel pair[] = {
        {1, card::Sampling::linear, t}, {0, card::Sampling::linear, 1.0F - t}
    };
    const uint32_t both = fog::append_unseen_terrain(
        two, grid, built.atlas, built.pages, pair, plain(37, 9), {}, nullptr
    );
    OA_CHECK(both == 2 * alone);
    const auto first = quads_of(one);
    const auto quads = quads_of(two);
    OA_CHECK(quads.size() == 2 * first.size());
    if (quads.size() != 2 * first.size())
        return;
    for (std::size_t index = 0; index < first.size(); ++index) {
        const auto& plain_quad = first[index];
        const auto& far = quads[index];
        const auto& near_quad = quads[index + first.size()];
        OA_CHECK(two.batches[far.batch].level == 1);
        OA_CHECK(two.batches[near_quad.batch].level == 0);
        for (std::size_t corner = 0; corner < quad_vertices; ++corner) {
            const float alpha = plain_quad.corners[corner].colour.alpha;
            OA_CHECK(near(far.corners[corner].x, plain_quad.corners[corner].x));
            OA_CHECK(near(near_quad.corners[corner].y, plain_quad.corners[corner].y));
            OA_CHECK(near(far.corners[corner].colour.alpha, fog::pass_alpha(alpha, t, 1.0F - t)));
            OA_CHECK(near(near_quad.corners[corner].colour.alpha, alpha * (1.0F - t)));
        }
    }
    // The first level's batches all come before the second's.
    bool ordered = true;
    bool seen_near = false;
    for (const auto& batch : two.batches) {
        if (batch.level == 0)
            seen_near = true;
        else if (seen_near)
            ordered = false;
    }
    OA_CHECK(ordered);
}

/// A placement moves and scales every corner: origin + (map - camera) * scale.
void placement_moves_and_scales_the_quads() {
    const Atlas built = build_atlas();
    const Sight sight = mixed_sight();
    const auto grid = sight.fog(37, 9);
    fog::FogPlacement placement;
    placement.camera_x = 37;
    placement.camera_z = 9;
    placement.origin_x = 10.5F;
    placement.origin_y = 20.25F;
    placement.scale = 0.75F;
    card::CardFrame placed;
    const fog::GreyedLevel single[] = {{0, card::Sampling::linear, 1.0F}};
    fog::append_unseen_terrain(
        placed, grid, built.atlas, built.pages, single, placement, {}, nullptr
    );
    card::CardFrame unplaced;
    fog::append_unseen_terrain(
        unplaced, grid, built.atlas, built.pages, single, plain(37, 9), {}, nullptr
    );
    OA_CHECK(placed.vertices.size() == unplaced.vertices.size());
    for (std::size_t index = 0; index < std::min(placed.vertices.size(), unplaced.vertices.size());
         ++index) {
        const auto& at = placed.vertices[index];
        const auto& from = unplaced.vertices[index];
        OA_CHECK(near(at.x, placement.origin_x + from.x * placement.scale));
        OA_CHECK(near(at.y, placement.origin_y + from.y * placement.scale));
        OA_CHECK(at.u == from.u && at.v == from.v && at.colour.alpha == from.colour.alpha);
    }
    // The black pass takes the same placement, over a view that reaches the
    // corner of the map never mapped.
    placement.camera_x = 150;
    placement.camera_z = 100;
    const auto corner_grid = sight.fog(150, 100);
    card::CardFrame black;
    fog::append_unmapped(black, corner_grid, {0.0F, 0.0F, 0.0F, 1.0F}, placement, {}, nullptr);
    card::CardFrame black_plain;
    fog::append_unmapped(
        black_plain, corner_grid, {0.0F, 0.0F, 0.0F, 1.0F}, plain(150, 100), {}, nullptr
    );
    OA_CHECK(black.vertices.size() == black_plain.vertices.size() && !black.vertices.empty());
    for (std::size_t index = 0;
         index < std::min(black.vertices.size(), black_plain.vertices.size());
         ++index) {
        OA_CHECK(near(
            black.vertices[index].x,
            placement.origin_x + black_plain.vertices[index].x * placement.scale
        ));
        OA_CHECK(near(
            black.vertices[index].y,
            placement.origin_y + black_plain.vertices[index].y * placement.scale
        ));
    }
}

/// With whole pixels at a sixth, where a fog tile spans 2 2/3 pixels, every
/// corner of the black and the greyed passes lies on a whole pixel, and
/// each row of tiles begins where the row above it ends, so that no row of
/// pixels lies between them.
void whole_pixels_meet_at_a_sixth() {
    const Atlas built = build_atlas();
    const Sight sight = mixed_sight();
    fog::FogPlacement placement;
    placement.camera_x = 150;
    placement.camera_z = 100;
    placement.origin_x = 10.25F;
    placement.origin_y = 20.5F;
    placement.scale = 1.0F / 6.0F;
    placement.whole_pixels = true;
    const auto grid = sight.fog(150, 100);
    card::CardFrame black;
    fog::append_unmapped(black, grid, {0.0F, 0.0F, 0.0F, 1.0F}, placement, {}, nullptr);
    card::CardFrame greyed;
    const fog::GreyedLevel single[] = {{0, card::Sampling::linear, 1.0F}};
    fog::append_unseen_terrain(
        greyed, grid, built.atlas, built.pages, single, placement, {}, nullptr
    );
    OA_CHECK(!black.vertices.empty() && !greyed.vertices.empty());
    for (const card::CardFrame* frame : {&black, &greyed}) {
        std::vector<float> tops;
        std::vector<float> bottoms;
        for (const auto& vertex : frame->vertices)
            OA_CHECK(vertex.x == std::round(vertex.x) && vertex.y == std::round(vertex.y));
        // Each quad's four corners, its top-left first and its bottom-left
        // last.
        for (std::size_t first = 0; first + 3 < frame->vertices.size(); first += 4) {
            tops.push_back(frame->vertices[first].y);
            bottoms.push_back(frame->vertices[first + 3].y);
        }
        const float highest = *std::min_element(tops.begin(), tops.end());
        for (const float top : tops)
            OA_CHECK(
                top == highest || std::find(bottoms.begin(), bottoms.end(), top) != bottoms.end()
            );
    }
}

/// Checks a solid pass over a layer of the grid: a quad for each tile with
/// a corner marked, tiles wholly marked that follow one another merged,
/// the colour kept, the alphas the corners' times the factor.
///
/// @param frame the frame the pass was appended to
/// @param grid the fog grid
/// @param camera_x map pixel at the view's left edge
/// @param camera_z map pixel at the view's top edge
/// @param mask_of returns the layer's mask of a tile
/// @param colour the pass's colour
/// @param factor what the corner alphas were scaled by
void check_solid_pass(
    const card::CardFrame& frame,
    const wr::FogGrid& grid,
    int32_t camera_x,
    int32_t camera_z,
    uint8_t (*mask_of)(const wr::FogTile&),
    const card::Colour& colour,
    float factor
) {
    const auto quads = quads_of(frame);

    // The runs and the single tiles the grid asks for, in row order.
    struct Wanted {
        int32_t map_x{};
        int32_t map_z{};
        int32_t width{};
        std::array<float, 4> alphas{};
    };

    std::vector<Wanted> wanted;
    const int32_t left = camera_x + grid.offset_x;
    const int32_t top = camera_z + grid.offset_z;
    for (int32_t row = 0; row < grid.height; ++row)
        for (int32_t column = 0; column < grid.width;) {
            const uint8_t mask = mask_of(grid.at(column, row));
            if (mask == 0) {
                ++column;
                continue;
            }
            Wanted one;
            one.map_x = left + column * tile_edge;
            one.map_z = top + row * tile_edge;
            if (mask == wr::fog_mask_full) {
                int32_t end = column + 1;
                while (end < grid.width && mask_of(grid.at(end, row)) == wr::fog_mask_full)
                    ++end;
                one.width = (end - column) * tile_edge;
                one.alphas = {1.0F, 1.0F, 1.0F, 1.0F};
                column = end;
            } else {
                one.width = tile_edge;
                one.alphas = fog::corner_alphas(mask);
                ++column;
            }
            wanted.push_back(one);
        }
    OA_CHECK(quads.size() == wanted.size());
    if (quads.size() != wanted.size())
        return;
    for (std::size_t index = 0; index < quads.size(); ++index) {
        const auto& quad = quads[index];
        const auto& one = wanted[index];
        OA_CHECK(near(quad.corners[0].x, static_cast<float>(one.map_x - camera_x)));
        OA_CHECK(near(quad.corners[0].y, static_cast<float>(one.map_z - camera_z)));
        OA_CHECK(near(quad.corners[1].x - quad.corners[0].x, static_cast<float>(one.width)));
        OA_CHECK(near(quad.corners[2].y - quad.corners[0].y, static_cast<float>(tile_edge)));
        // Corners in the frame's order: top-left, top-right, bottom-right, bottom-left.
        const std::array<std::size_t, 4> order{0, 1, 3, 2};
        for (std::size_t corner = 0; corner < quad_vertices; ++corner) {
            const auto& vertex = quad.corners[corner];
            OA_CHECK(
                vertex.colour.red == colour.red && vertex.colour.green == colour.green &&
                vertex.colour.blue == colour.blue
            );
            OA_CHECK(near(vertex.colour.alpha, one.alphas[order[corner]] * factor));
        }
        const auto& batch = frame.batches[quad.batch];
        OA_CHECK(
            batch.page == card::PageHandle{} && batch.blend == card::Blend::alpha &&
            batch.level == 0
        );
    }
    // One batch holds the pass.
    OA_CHECK(frame.batches.size() == (quads.empty() ? 0U : 1U));
}

/// The black pass over the cells never mapped, and the dithered pass over
/// those out of sight at half alpha, leaving the tiles wholly never mapped
/// to the black pass.
void solid_passes_merge_their_runs() {
    const Sight sight = mixed_sight();
    for (const auto& [camera_x, camera_z] :
         {std::pair{0, 0}, std::pair{37, 9}, std::pair{150, 100}}) {
        const auto grid = sight.fog(camera_x, camera_z);
        const card::Colour black{0.0F, 0.0F, 0.0F, 0.5F};
        card::CardFrame frame;
        const uint32_t appended =
            fog::append_unmapped(frame, grid, black, plain(camera_x, camera_z), {}, nullptr);
        OA_CHECK(appended == quads_of(frame).size());
        check_solid_pass(
            frame,
            grid,
            camera_x,
            camera_z,
            [](const wr::FogTile& tile) { return tile.unmapped; },
            black,
            1.0F
        );
        const card::Colour dither{0.1F, 0.2F, 0.3F, 1.0F};
        card::CardFrame dithered;
        fog::append_unseen_dither(dithered, grid, dither, plain(camera_x, camera_z), {}, nullptr);
        check_solid_pass(
            dithered,
            grid,
            camera_x,
            camera_z,
            [](const wr::FogTile& tile) {
                return tile.unmapped == wr::fog_mask_full ? uint8_t{0} : tile.unseen;
            },
            dither,
            0.5F
        );
    }
    // A corner of the map never mapped shows up as a merged run of black.
    const auto grid = sight.fog(150, 100);
    card::CardFrame frame;
    fog::append_unmapped(frame, grid, {}, plain(150, 100), {}, nullptr);
    bool merged = false;
    for (const auto& quad : quads_of(frame))
        merged = merged || quad.corners[1].x - quad.corners[0].x > static_cast<float>(tile_edge);
    OA_CHECK(merged);
}

/// A shade level is black by alpha at one less the row's scale, a light
/// level white added at one less the scale's reciprocal, each clamped.
void level_quads_follow_the_tables() {
    // The kill board's shade, row 8: scale 0.55.
    const auto board = fog::level_quad(-0x18);
    OA_CHECK(board.blend == card::Blend::alpha);
    OA_CHECK(board.colour.red == 0.0F && board.colour.green == 0.0F && board.colour.blue == 0.0F);
    OA_CHECK(near(board.colour.alpha, 1.0F - 8.0F * fog::shade_row_step));
    // Row 0 is black; row 15 and above change nothing; below row 0 clamps.
    OA_CHECK(near(fog::level_quad(-0x20).colour.alpha, 1.0F));
    OA_CHECK(near(fog::level_quad(-0x40).colour.alpha, 1.0F));
    OA_CHECK(near(fog::level_quad(-0x20 + 15).colour.alpha, 0.0F));
    OA_CHECK(near(fog::level_quad(-1).colour.alpha, 0.0F));
    // Light levels: the kill board's highlight rows, and the clamp.
    const auto highlight = fog::level_quad(0x1f);
    OA_CHECK(highlight.blend == card::Blend::additive);
    OA_CHECK(
        highlight.colour.red == 1.0F && highlight.colour.green == 1.0F &&
        highlight.colour.blue == 1.0F
    );
    OA_CHECK(near(highlight.colour.alpha, 1.0F - 1.0F / (1.0F + 31.0F * fog::light_row_step)));
    OA_CHECK(
        near(fog::level_quad(0x14).colour.alpha, 1.0F - 1.0F / (1.0F + 20.0F * fog::light_row_step))
    );
    OA_CHECK(near(fog::level_quad(0).colour.alpha, 0.0F));
    OA_CHECK(near(fog::level_quad(0x7f).colour.alpha, fog::level_quad(0x1f).colour.alpha));
}

/// Reads a mask's alpha at a point as a LINEAR draw reads a page: the four
/// texels whose centres are nearest, each weighted by how near, the texels
/// at the edges standing for those past them.
///
/// @param mask the mask
/// @param texel_x the point across, in texels from the mask's left edge
/// @param texel_y the point down
/// @return the alpha, 0 to 1
float read_linear(const fog::UnseenMask& mask, float texel_x, float texel_y) {
    const float x = texel_x - 0.5F;
    const float y = texel_y - 0.5F;
    const auto left = static_cast<int32_t>(std::floor(x));
    const auto top = static_cast<int32_t>(std::floor(y));
    const float across = x - static_cast<float>(left);
    const float down = y - static_cast<float>(top);
    const auto value = [&](int32_t column, int32_t row) {
        column = std::clamp(column, 0, static_cast<int32_t>(mask.width) - 1);
        row = std::clamp(row, 0, static_cast<int32_t>(mask.height) - 1);
        return static_cast<float>(mask.texels
                                      [static_cast<std::size_t>(row) * mask.width +
                                       static_cast<std::size_t>(column)]
                                          .alpha) /
               255.0F;
    };
    return value(left, top) * (1.0F - across) * (1.0F - down) +
           value(left + 1, top) * across * (1.0F - down) +
           value(left, top + 1) * (1.0F - across) * down + value(left + 1, top + 1) * across * down;
}

/// The fog mask holds each cell's state at the texel of its tile corner,
/// the last column and row repeated, and read LINEAR through the quad over
/// the grid it gives each point of a tile that reaches the map the alpha
/// the tile's corners interpolate to there, as the greyed pass's quads do; the quad lands on
/// whole pixels where the placement asks, and a mask not the grid's or a
/// page too small for it draws nothing.
void mask_reads_the_corner_alphas() {
    const Sight sight = mixed_sight();
    // The corners in the order of the alphas, as columns and rows past the
    // tile's top-left corner.
    constexpr std::array<std::pair<int32_t, int32_t>, 4> corner_steps{
        {{0, 0}, {1, 0}, {0, 1}, {1, 1}}
    };
    constexpr std::array<std::pair<float, float>, 6> points{
        {{0.0F, 0.0F}, {0.25F, 0.25F}, {0.5F, 0.75F}, {0.9F, 0.1F}, {1.0F, 1.0F}, {0.5F, 0.5F}}
    };
    for (const auto& [camera_x, camera_z] :
         {std::pair{0, 0}, std::pair{37, 9}, std::pair{150, 100}}) {
        const auto grid = sight.fog(camera_x, camera_z);
        fog::UnseenMask mask;
        fog::build_unseen_mask(grid, mask);
        OA_CHECK(mask.width == static_cast<uint32_t>(grid.width) + 2U);
        OA_CHECK(mask.height == static_cast<uint32_t>(grid.height) + 2U);
        OA_CHECK(mask.texels.size() == std::size_t{mask.width} * mask.height);
        if (mask.texels.size() != std::size_t{mask.width} * mask.height)
            return;
        const auto texel = [&](uint32_t column, uint32_t row) {
            return mask.texels[std::size_t{row} * mask.width + column];
        };
        // A tile reaches the map where the cells at its corners do: from
        // the one before the map's first, whose tile straddles the edge.
        const auto on_map = [&](int32_t column, int32_t row) {
            const int32_t x = grid.first_cell_x + column;
            const int32_t z = grid.first_cell_z + row;
            return x >= -1 && z >= -1 && x < static_cast<int32_t>(map_tiles_wide) &&
                   z < static_cast<int32_t>(map_tiles_high);
        };
        bool any_unseen = false;
        for (int32_t row = 0; row < grid.height; ++row)
            for (int32_t column = 0; column < grid.width; ++column) {
                const auto& tile = grid.at(column, row);
                any_unseen = any_unseen || tile.unseen != 0;
                if (!on_map(column, row))
                    continue;
                const auto alphas = fog::corner_alphas(tile.unseen);
                for (std::size_t corner = 0; corner < fog::fog_tile_corners; ++corner) {
                    const auto held = texel(
                        static_cast<uint32_t>(column + corner_steps[corner].first),
                        static_cast<uint32_t>(row + corner_steps[corner].second)
                    );
                    OA_CHECK(held.alpha == (alphas[corner] == 1.0F ? 255 : 0));
                    OA_CHECK(
                        held.red == held.alpha && held.green == held.alpha &&
                        held.blue == held.alpha
                    );
                }
            }
        OA_CHECK(mask.any_unseen == any_unseen);
        for (uint32_t row = 0; row < mask.height; ++row) {
            const auto last = texel(mask.width - 1, row);
            const auto before = texel(mask.width - 2, row);
            OA_CHECK(last.alpha == before.alpha && last.red == before.red);
        }
        for (uint32_t column = 0; column < mask.width; ++column) {
            const auto last = texel(column, mask.height - 1);
            const auto before = texel(column, mask.height - 2);
            OA_CHECK(last.alpha == before.alpha && last.red == before.red);
        }

        // The quad, from a page larger than the mask.
        const uint32_t page_width = mask.width + 5;
        const uint32_t page_height = mask.height + 3;
        const card::PageHandle page{7};
        card::CardFrame frame;
        OA_CHECK(
            fog::append_unseen_mask(
                frame,
                grid,
                mask,
                page,
                page_width,
                page_height,
                card::Blend::darken,
                plain(camera_x, camera_z),
                {},
                nullptr
            ) == 1
        );
        const auto quads = quads_of(frame);
        OA_CHECK(quads.size() == 1 && frame.batches.size() == 1);
        if (quads.size() != 1 || frame.batches.size() != 1)
            return;
        const auto& batch = frame.batches[0];
        OA_CHECK(
            batch.page == page && batch.level == 0 && batch.blend == card::Blend::darken &&
            batch.sampling == card::Sampling::linear && !batch.scissored
        );
        // Half a tile past the grid's tiles each way, textured by whole
        // texels: the cells' from the first's edge.
        const auto& corners = quads[0].corners;
        OA_CHECK(near(corners[0].x, static_cast<float>(grid.offset_x - tile_edge / 2)));
        OA_CHECK(near(corners[0].y, static_cast<float>(grid.offset_z - tile_edge / 2)));
        OA_CHECK(
            near(corners[2].x - corners[0].x, static_cast<float>((grid.width + 1) * tile_edge))
        );
        OA_CHECK(
            near(corners[2].y - corners[0].y, static_cast<float>((grid.height + 1) * tile_edge))
        );
        OA_CHECK(corners[0].u == 0.0F && corners[0].v == 0.0F);
        OA_CHECK(
            near(corners[2].u * static_cast<float>(page_width), static_cast<float>(grid.width + 1))
        );
        OA_CHECK(near(
            corners[2].v * static_cast<float>(page_height), static_cast<float>(grid.height + 1)
        ));
        for (const auto& corner : corners)
            OA_CHECK(
                corner.colour.red == 1.0F && corner.colour.green == 1.0F &&
                corner.colour.blue == 1.0F && corner.colour.alpha == 1.0F
            );
        // Each point of each tile, read through the quad's texture
        // coordinates as the card interpolates them across it.
        for (int32_t row = 0; row < grid.height; ++row)
            for (int32_t column = 0; column < grid.width; ++column) {
                if (!on_map(column, row))
                    continue;
                const auto alphas = fog::corner_alphas(grid.at(column, row).unseen);
                for (const auto& [u, v] : points) {
                    const float x =
                        corners[0].x + (static_cast<float>(column) + 0.5F + u) * tile_edge;
                    const float y = corners[0].y + (static_cast<float>(row) + 0.5F + v) * tile_edge;
                    const float across = (x - corners[0].x) / (corners[2].x - corners[0].x);
                    const float down = (y - corners[0].y) / (corners[2].y - corners[0].y);
                    const float texel_x =
                        (corners[0].u + across * (corners[2].u - corners[0].u)) * page_width;
                    const float texel_y =
                        (corners[0].v + down * (corners[2].v - corners[0].v)) * page_height;
                    const float wanted = alphas[0] * (1.0F - u) * (1.0F - v) +
                                         alphas[1] * u * (1.0F - v) + alphas[2] * (1.0F - u) * v +
                                         alphas[3] * u * v;
                    OA_CHECK(std::fabs(read_linear(mask, texel_x, texel_y) - wanted) <= 1e-4F);
                }
            }

        // A mask not the grid's, or a page smaller than the mask, draws
        // nothing.
        card::CardFrame refused;
        fog::UnseenMask other = mask;
        other.width += 1;
        OA_CHECK(
            fog::append_unseen_mask(
                refused,
                grid,
                other,
                page,
                page_width + 1,
                page_height,
                card::Blend::darken,
                plain(camera_x, camera_z),
                {},
                nullptr
            ) == 0
        );
        OA_CHECK(
            fog::append_unseen_mask(
                refused,
                grid,
                mask,
                page,
                mask.width - 1,
                page_height,
                card::Blend::darken,
                plain(camera_x, camera_z),
                {},
                nullptr
            ) == 0
        );
        OA_CHECK(refused.vertices.empty() && refused.batches.empty());
    }

    // At a sixth on whole pixels the quad's edges are whole pixels, its
    // blend the one asked for.
    {
        fog::FogPlacement placement;
        placement.camera_x = 150;
        placement.camera_z = 100;
        placement.origin_x = 10.25F;
        placement.origin_y = 20.5F;
        placement.scale = 1.0F / 6.0F;
        placement.whole_pixels = true;
        const auto grid = sight.fog(150, 100);
        fog::UnseenMask mask;
        fog::build_unseen_mask(grid, mask);
        card::CardFrame frame;
        OA_CHECK(
            fog::append_unseen_mask(
                frame,
                grid,
                mask,
                {3},
                mask.width,
                mask.height,
                card::Blend::modulate,
                placement,
                {},
                nullptr
            ) == 1
        );
        for (const auto& vertex : frame.vertices)
            OA_CHECK(vertex.x == std::round(vertex.x) && vertex.y == std::round(vertex.y));
        OA_CHECK(frame.batches.size() == 1 && frame.batches[0].blend == card::Blend::modulate);
    }

    // Everything seen: a mask with nothing out of sight; an empty grid: none.
    {
        const Sight seen;
        fog::UnseenMask mask;
        fog::build_unseen_mask(seen.fog(0, 0), mask);
        OA_CHECK(!mask.any_unseen && mask.width != 0);
        fog::build_unseen_mask({}, mask);
        OA_CHECK(mask.width == 0 && mask.height == 0 && mask.texels.empty() && !mask.any_unseen);
        card::CardFrame frame;
        OA_CHECK(
            fog::append_unseen_mask(
                frame, {}, mask, {3}, 16, 16, card::Blend::darken, plain(0, 0), {}, nullptr
            ) == 0
        );
        OA_CHECK(frame.vertices.empty() && frame.batches.empty());
    }
}

/// An empty grid, a level past the tile levels, too few pages and no level
/// add nothing.
void refusals_add_nothing() {
    const Atlas built = build_atlas();
    const Sight sight = mixed_sight();
    const auto grid = sight.fog(0, 0);
    card::CardFrame frame;
    const fog::GreyedLevel single[] = {{0, card::Sampling::nearest, 1.0F}};
    const fog::GreyedLevel deep[] = {{gw::tile_level_count, card::Sampling::nearest, 1.0F}};
    const fog::GreyedLevel three[] = {
        {0, card::Sampling::nearest, 0.4F},
        {1, card::Sampling::nearest, 0.3F},
        {2, card::Sampling::nearest, 0.3F},
    };
    OA_CHECK(
        fog::append_unseen_terrain(
            frame, {}, built.atlas, built.pages, single, plain(0, 0), {}, nullptr
        ) == 0
    );
    OA_CHECK(
        fog::append_unseen_terrain(
            frame, grid, built.atlas, built.pages, deep, plain(0, 0), {}, nullptr
        ) == 0
    );
    OA_CHECK(
        fog::append_unseen_terrain(
            frame, grid, built.atlas, built.pages, three, plain(0, 0), {}, nullptr
        ) == 0
    );
    OA_CHECK(
        fog::append_unseen_terrain(
            frame, grid, built.atlas, built.pages, {}, plain(0, 0), {}, nullptr
        ) == 0
    );
    OA_CHECK(
        fog::append_unseen_terrain(
            frame, grid, built.atlas, {}, single, plain(0, 0), {}, nullptr
        ) == 0
    );
    OA_CHECK(fog::append_unmapped(frame, {}, {}, plain(0, 0), {}, nullptr) == 0);
    OA_CHECK(fog::append_unseen_dither(frame, {}, {}, plain(0, 0), {}, nullptr) == 0);
    OA_CHECK(frame.vertices.empty() && frame.indices.empty() && frame.batches.empty());
}

/// A FOG.GAF tile set of seeded shapes: each tile's texels opaque at
/// random, in three palette indices.
///
/// @return the tiles
wr::FogTileSet seeded_tiles() {
    std::mt19937 random(test_seed + 2);
    wr::FogTileSet tiles;
    tiles.art.assign(2 * wr::FogTileSet::tiles_per_bank, {});
    for (auto& art : tiles.art)
        for (std::size_t texel = 0; texel < art.opaque.size(); ++texel) {
            art.opaque[texel] = random() % 3 != 0 ? 1 : 0;
            art.index[texel] = static_cast<uint8_t>(random() % 3);
        }
    return tiles;
}

/// The Classic shapes cover each tile's opaque texels once, in their own
/// palette index and within one quarter, and nothing else; a tile's runs
/// that match the row above grow one rectangle.
void classic_shapes_cover_the_art() {
    const wr::FogTileSet tiles = seeded_tiles();
    fog::ClassicShapes shapes;
    fog::build_classic_shapes(tiles, shapes);
    OA_CHECK(shapes.built());
    for (const auto bank : {wr::FogTileSet::black, wr::FogTileSet::gray})
        for (int32_t variant = 0; variant < wr::fog_tile_variants; ++variant)
            for (uint8_t mask = 1; mask < wr::fog_mask_full; ++mask) {
                const wr::FogTileArt& art = tiles.at(bank, variant, mask);
                std::array<uint8_t, tile_edge * tile_edge> covered{};
                for (const fog::ShapeBox& box : shapes.at(bank, variant, mask)) {
                    OA_CHECK(box.width >= 1 && box.height >= 1);
                    OA_CHECK(box.x / 16 == (box.x + box.width - 1) / 16);
                    OA_CHECK(box.y / 16 == (box.y + box.height - 1) / 16);
                    for (int32_t y = box.y; y < box.y + box.height; ++y)
                        for (int32_t x = box.x; x < box.x + box.width; ++x) {
                            const auto texel = static_cast<std::size_t>(y * tile_edge + x);
                            OA_CHECK(art.opaque[texel] != 0 && art.index[texel] == box.index);
                            ++covered[texel];
                        }
                }
                for (std::size_t texel = 0; texel < covered.size(); ++texel)
                    OA_CHECK(covered[texel] == (art.opaque[texel] != 0 ? 1 : 0));
            }
    // A solid quarter of one index is one rectangle.
    wr::FogTileSet solid;
    solid.art.assign(2 * wr::FogTileSet::tiles_per_bank, {});
    for (int32_t y = 0; y < tile_edge / 2; ++y)
        for (int32_t x = 0; x < tile_edge / 2; ++x)
            solid.at(wr::FogTileSet::black, 0, 1)
                .opaque[static_cast<std::size_t>(y * tile_edge + x)] = 1;
    fog::build_classic_shapes(solid, shapes);
    const auto one = shapes.at(wr::FogTileSet::black, 0, 1);
    OA_CHECK(one.size() == 1);
    if (one.size() == 1)
        OA_CHECK(one[0].x == 0 && one[0].y == 0 && one[0].width == 16 && one[0].height == 16);
    OA_CHECK(shapes.at(wr::FogTileSet::black, 0, 2).empty());
    // Masks and variants past the shapes, and art not loaded, have none.
    OA_CHECK(shapes.at(wr::FogTileSet::black, 0, 0).empty());
    OA_CHECK(shapes.at(wr::FogTileSet::black, 0, wr::fog_mask_full).empty());
    OA_CHECK(shapes.at(wr::FogTileSet::gray, wr::fog_tile_variants, 1).empty());
    fog::build_classic_shapes(wr::FogTileSet{}, shapes);
    OA_CHECK(!shapes.built() && shapes.at(wr::FogTileSet::black, 0, 1).empty());
}

/// The dither's page clears the texels whose column and row sum to an even
/// number, as the processor clears map pixels from a tile's even corner.
void dither_page_is_the_checkerboard() {
    std::vector<card::Texel> texels;
    fog::build_dither_texels(texels);
    OA_CHECK(texels.size() == std::size_t{fog::dither_page_edge} * fog::dither_page_edge);
    for (uint32_t y = 0; y < fog::dither_page_edge; ++y)
        for (uint32_t x = 0; x < fog::dither_page_edge; ++x) {
            const card::Texel& texel = texels[std::size_t{y} * fog::dither_page_edge + x];
            const uint8_t expected = ((x + y) & 1U) == 0 ? 255 : 0;
            OA_CHECK(
                texel.red == expected && texel.green == expected && texel.blue == expected &&
                texel.alpha == expected
            );
        }
}

/// What a Classic pass should draw at each map pixel under a grid's tiles.
struct ClassicField {
    int32_t left{}; ///< the map pixel of the grid's first column
    int32_t top{};  ///< and of its first row
    int32_t width{};
    int32_t height{};
    std::vector<int32_t> drawn; ///< -1 for nothing; else a palette index, or 256 for the fill

    /// Returns a pixel's place in drawn, or none past the grid.
    [[nodiscard]] std::optional<std::size_t> at(int32_t x, int32_t z) const {
        if (x < left || z < top || x >= left + width || z >= top + height)
            return std::nullopt;
        return static_cast<std::size_t>(z - top) * static_cast<std::size_t>(width) +
               static_cast<std::size_t>(x - left);
    }
};

/// The fill a ClassicField holds for a whole tile.
constexpr int32_t whole_fill = 256;

/// Works out what the processor draws of one layer of a grid, as
/// draw_fog_grid does: a whole tile filled, a partial one through its
/// FOG.GAF tile of the bank.
///
/// @param grid the fog grid
/// @param tiles the tile art
/// @param bank black for the never-mapped layer, gray for the greyed one
/// @param camera_x map pixel at the view's left edge
/// @param camera_z map pixel at the view's top edge
/// @return the field over the grid's tiles
ClassicField classic_field(
    const wr::FogGrid& grid,
    const wr::FogTileSet& tiles,
    wr::FogTileSet::Bank bank,
    int32_t camera_x,
    int32_t camera_z
) {
    ClassicField field;
    field.left = camera_x + grid.offset_x;
    field.top = camera_z + grid.offset_z;
    field.width = grid.width * tile_edge;
    field.height = grid.height * tile_edge;
    field.drawn.assign(static_cast<std::size_t>(field.width) * field.height, -1);
    for (int32_t row = 0; row < grid.height; ++row)
        for (int32_t column = 0; column < grid.width; ++column) {
            const wr::FogTile& tile = grid.at(column, row);
            const uint8_t mask = bank == wr::FogTileSet::black
                                     ? tile.unmapped
                                     : (tile.unmapped == wr::fog_mask_full ? 0 : tile.unseen);
            if (mask == 0)
                continue;
            const int32_t variant = (row + column + grid.variant_phase) & 3;
            for (int32_t y = 0; y < tile_edge; ++y)
                for (int32_t x = 0; x < tile_edge; ++x) {
                    const auto texel = static_cast<std::size_t>(y * tile_edge + x);
                    int32_t value = -1;
                    if (mask == wr::fog_mask_full)
                        value = whole_fill;
                    else if (tiles.at(bank, variant, mask).opaque[texel] != 0)
                        value = tiles.at(bank, variant, mask).index[texel];
                    field.drawn
                        [static_cast<std::size_t>(row * tile_edge + y) * field.width +
                         static_cast<std::size_t>(column * tile_edge + x)] = value;
                }
        }
    return field;
}

/// The Classic passes over grids of mixed sight, at an aligned camera and
/// one between tiles: the black pass draws exactly the processor's black
/// tiles in their palette colours and whole tiles in the fill, the greyed
/// pass the greyed terrain once under exactly the processor's gray tiles
/// within the map, at its tile's texels, and the dithered form the dither's
/// page under the same pixels, each texel at its pixel's place in its tile.
void classic_passes_draw_the_processors_tiles() {
    const Atlas built = build_atlas();
    const Sight sight = mixed_sight();
    const wr::FogTileSet tiles = seeded_tiles();
    fog::ClassicShapes shapes;
    fog::build_classic_shapes(tiles, shapes);
    std::array<card::Colour, OA_PALETTE_COLORS> palette{};
    for (std::size_t index = 0; index < palette.size(); ++index)
        palette[index] = {static_cast<float>(index) / 255.0F, 0.25F, 0.5F, 0.0F};
    const card::Colour fill{0.75F, 0.125F, 0.0F, 0.5F};
    const card::Colour dither{0.0F, 0.5F, 0.25F, 0.0F};
    const auto map_width = static_cast<int32_t>(map_tiles_wide) * tile_edge;
    const auto map_height = static_cast<int32_t>(map_tiles_high) * tile_edge;
    // Quads of each pass over every camera: each draws some.
    std::array<uint32_t, 3> passes_drawn{};
    for (const auto& [camera_x, camera_z] :
         {std::pair{0, 0}, std::pair{45, 23}, std::pair{128, 64}, std::pair{150, 77}}) {
        const auto grid = sight.fog(camera_x, camera_z);
        const auto placement = plain(camera_x, camera_z);

        // Reads a quad's map rectangle back.
        struct Box {
            int32_t x{};
            int32_t z{};
            int32_t width{};
            int32_t height{};
        };

        const auto box_of = [&](const Quad& quad) {
            const auto x = static_cast<int32_t>(std::lround(quad.corners[0].x));
            const auto z = static_cast<int32_t>(std::lround(quad.corners[0].y));
            return Box{
                x + camera_x,
                z + camera_z,
                static_cast<int32_t>(std::lround(quad.corners[2].x)) - x,
                static_cast<int32_t>(std::lround(quad.corners[2].y)) - z
            };
        };

        // The black pass.
        {
            const ClassicField field =
                classic_field(grid, tiles, wr::FogTileSet::black, camera_x, camera_z);
            card::CardFrame frame;
            const uint32_t appended = fog::append_unmapped_classic(
                frame, grid, shapes, fill, palette, placement, {}, nullptr
            );
            const auto quads = quads_of(frame);
            OA_CHECK(appended == quads.size());
            passes_drawn[0] += appended;
            std::vector<int32_t> drawn(field.drawn.size(), -1);
            for (const auto& quad : quads) {
                OA_CHECK(frame.batches[quad.batch].page == card::PageHandle{});
                OA_CHECK(frame.batches[quad.batch].blend == card::Blend::alpha);
                const card::Colour& colour = quad.corners[0].colour;
                OA_CHECK(colour.alpha == 1.0F);
                int32_t value = whole_fill;
                if (!(colour.red == fill.red && colour.green == fill.green))
                    value = static_cast<int32_t>(std::lround(colour.red * 255.0F));
                const Box box = box_of(quad);
                for (int32_t z = box.z; z < box.z + box.height; ++z)
                    for (int32_t x = box.x; x < box.x + box.width; ++x) {
                        const auto at = field.at(x, z);
                        OA_CHECK(at.has_value());
                        if (!at)
                            continue;
                        OA_CHECK(drawn[*at] == -1);
                        drawn[*at] = value;
                    }
            }
            OA_CHECK(drawn == field.drawn);
        }

        // The greyed pass, and its dithered form.
        const ClassicField field =
            classic_field(grid, tiles, wr::FogTileSet::gray, camera_x, camera_z);
        {
            card::CardFrame frame;
            const fog::GreyedLevel levels[] = {{0, card::Sampling::nearest, 1.0F}};
            const uint32_t appended = fog::append_unseen_terrain_classic(
                frame, grid, shapes, built.atlas, built.pages, levels, placement, {}, nullptr
            );
            const auto quads = quads_of(frame);
            OA_CHECK(appended == quads.size());
            passes_drawn[1] += appended;
            std::vector<uint8_t> covered(field.drawn.size(), 0);
            const auto& atlas = built.atlas;
            for (const auto& quad : quads) {
                OA_CHECK(near(quad.corners[0].colour.alpha, 1.0F));
                const Box box = box_of(quad);
                OA_CHECK(box.x >= 0 && box.z >= 0);
                OA_CHECK(box.x / tile_edge == (box.x + box.width - 1) / tile_edge);
                OA_CHECK(box.z / tile_edge == (box.z + box.height - 1) / tile_edge);
                for (int32_t z = box.z; z < box.z + box.height; ++z)
                    for (int32_t x = box.x; x < box.x + box.width; ++x) {
                        const auto at = field.at(x, z);
                        OA_CHECK(at.has_value());
                        if (!at)
                            continue;
                        OA_CHECK(field.drawn[*at] != -1 && covered[*at] == 0);
                        covered[*at] = 1;
                    }
                const auto slot =
                    atlas.grid
                        [static_cast<std::size_t>(box.z / tile_edge) * atlas.grid_width +
                         static_cast<std::size_t>(box.x / tile_edge)];
                const auto rect = gw::tile_rect(atlas, slot, 0);
                OA_CHECK(rect.has_value());
                if (!rect)
                    continue;
                OA_CHECK(frame.batches[quad.batch].page == built.pages[rect->page]);
                const auto& level = atlas.pages[rect->page].levels[0];
                OA_CHECK(near(
                    quad.corners[0].u * static_cast<float>(level.width),
                    static_cast<float>(rect->x + static_cast<uint32_t>(box.x % tile_edge))
                ));
                OA_CHECK(near(
                    quad.corners[2].v * static_cast<float>(level.height),
                    static_cast<float>(
                        rect->y + static_cast<uint32_t>(box.z % tile_edge + box.height)
                    )
                ));
            }
            // Every pixel the processor greys on the map is covered.
            for (int32_t z = field.top; z < field.top + field.height; ++z)
                for (int32_t x = field.left; x < field.left + field.width; ++x) {
                    const auto at = field.at(x, z);
                    const bool on_map = x >= 0 && z >= 0 && x < map_width && z < map_height;
                    OA_CHECK(covered[*at] == (on_map && field.drawn[*at] != -1 ? 1 : 0));
                }
        }
        {
            card::CardFrame frame;
            const card::PageHandle page{9};
            const uint32_t appended = fog::append_unseen_dither_classic(
                frame, grid, shapes, page, dither, placement, {}, nullptr
            );
            const auto quads = quads_of(frame);
            OA_CHECK(appended == quads.size());
            passes_drawn[2] += appended;
            std::vector<uint8_t> covered(field.drawn.size(), 0);
            for (const auto& quad : quads) {
                const auto& batch = frame.batches[quad.batch];
                OA_CHECK(batch.page == page && batch.sampling == card::Sampling::nearest);
                OA_CHECK(
                    quad.corners[0].colour.green == dither.green &&
                    quad.corners[0].colour.alpha == 1.0F
                );
                const Box box = box_of(quad);
                // The page's texel under the box's corner is its pixel's
                // place in its fog tile.
                const int32_t in_x = ((box.x - field.left) % tile_edge + tile_edge) % tile_edge;
                const int32_t in_z = ((box.z - field.top) % tile_edge + tile_edge) % tile_edge;
                OA_CHECK(near(
                    quad.corners[0].u * static_cast<float>(fog::dither_page_edge),
                    static_cast<float>(in_x)
                ));
                OA_CHECK(near(
                    quad.corners[0].v * static_cast<float>(fog::dither_page_edge),
                    static_cast<float>(in_z)
                ));
                for (int32_t z = box.z; z < box.z + box.height; ++z)
                    for (int32_t x = box.x; x < box.x + box.width; ++x) {
                        const auto at = field.at(x, z);
                        OA_CHECK(at.has_value());
                        if (at) {
                            OA_CHECK(covered[*at] == 0);
                            covered[*at] = 1;
                        }
                    }
            }
            for (std::size_t at = 0; at < covered.size(); ++at)
                OA_CHECK(covered[at] == (field.drawn[at] != -1 ? 1 : 0));
        }
    }
    OA_CHECK(passes_drawn[0] != 0 && passes_drawn[1] != 0 && passes_drawn[2] != 0);
    // Unbuilt shapes and an empty grid draw nothing.
    card::CardFrame frame;
    const fog::GreyedLevel levels[] = {{0, card::Sampling::nearest, 1.0F}};
    const fog::ClassicShapes none;
    const auto grid = sight.fog(0, 0);
    OA_CHECK(
        fog::append_unmapped_classic(frame, grid, none, fill, palette, plain(0, 0), {}, nullptr) ==
        0
    );
    OA_CHECK(
        fog::append_unseen_terrain_classic(
            frame, grid, none, built.atlas, built.pages, levels, plain(0, 0), {}, nullptr
        ) == 0
    );
    OA_CHECK(
        fog::append_unseen_dither_classic(
            frame, {}, shapes, {9}, dither, plain(0, 0), {}, nullptr
        ) == 0
    );
    OA_CHECK(
        fog::append_unseen_dither_classic(
            frame, grid, shapes, {}, dither, plain(0, 0), {}, nullptr
        ) == 0
    );
    OA_CHECK(frame.vertices.empty() && frame.batches.empty());
}

} // namespace

/// The alpha a triangle of a frame draws at a point, by its corners'
/// alphas interpolated linearly across it, as the card interpolates them;
/// none where the point lies outside it.
///
/// @param a a corner
/// @param b the next
/// @param c the last
/// @param x the point's column
/// @param y its row
/// @return the alpha, or none outside the triangle
std::optional<float> triangle_alpha(
    const fog::FogPoint& a, const fog::FogPoint& b, const fog::FogPoint& c, float x, float y
) {
    const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (std::fabs(area) < 1e-6F)
        return std::nullopt;
    const float wa = ((b.x - x) * (c.y - y) - (c.x - x) * (b.y - y)) / area;
    const float wb = ((c.x - x) * (a.y - y) - (a.x - x) * (c.y - y)) / area;
    const float wc = 1.0F - wa - wb;
    constexpr float inside = -1e-5F;
    if (wa < inside || wb < inside || wc < inside)
        return std::nullopt;
    return wa * a.alpha + wb * b.alpha + wc * c.alpha;
}

/// The alpha a list of triangles draws at a point: the first that holds it.
///
/// @param triangles the triangles, three points each
/// @param x the point's column
/// @param y its row
/// @return the alpha, 0 where none holds it
float alpha_over(const std::vector<std::array<fog::FogPoint, 3>>& triangles, float x, float y) {
    for (const auto& triangle : triangles)
        if (const auto alpha = triangle_alpha(triangle[0], triangle[1], triangle[2], x, y))
            return *alpha;
    return 0.0F;
}

/// The triangles of a frame's quads, as the card lays them out: top-left,
/// top-right, bottom-right, and top-left, bottom-right, bottom-left.
///
/// @param frame the frame
/// @return the triangles, with each corner's alpha
std::vector<std::array<fog::FogPoint, 3>> quad_triangles(const card::CardFrame& frame) {
    std::vector<std::array<fog::FogPoint, 3>> triangles;
    for (const Quad& quad : quads_of(frame)) {
        const auto point = [&](std::size_t corner) {
            return fog::FogPoint{
                quad.corners[corner].x, quad.corners[corner].y, quad.corners[corner].colour.alpha
            };
        };
        triangles.push_back({point(0), point(1), point(2)});
        triangles.push_back({point(0), point(2), point(3)});
    }
    return triangles;
}

/// The triangles of a greyed cover's pieces, each a fan from its first point.
///
/// @param pieces the pieces
/// @return the triangles
std::vector<std::array<fog::FogPoint, 3>>
cover_triangles(const std::vector<fog::CoverPiece>& pieces) {
    std::vector<std::array<fog::FogPoint, 3>> triangles;
    for (const auto& piece : pieces) {
        OA_CHECK(piece.count >= 3 && piece.count <= fog::most_cover_points);
        for (uint32_t point = 1; point + 1 < piece.count; ++point)
            triangles.push_back({piece.points[0], piece.points[point], piece.points[point + 1]});
    }
    return triangles;
}

/// A greyed cover draws, at every pixel of a rectangle, the alpha the
/// greyed pass draws the terrain at there and nothing past the rectangle:
/// the smooth pass's triangles and the Classic pass's rectangles, at an
/// aligned camera, one between tiles and through a placement; a rectangle
/// wholly in sight is covered by nothing and one wholly out of sight whole.
void greyed_cover_takes_the_terrains_alphas() {
    const Atlas built = build_atlas();
    const Sight sight = mixed_sight();
    const wr::FogTileSet tiles = seeded_tiles();
    fog::ClassicShapes shapes;
    fog::build_classic_shapes(tiles, shapes);
    const fog::GreyedLevel levels[] = {{0, card::Sampling::nearest, 1.0F}};
    std::size_t parts = 0;
    for (const auto& [camera_x, camera_z] :
         {std::pair{0, 0}, std::pair{45, 23}, std::pair{32, 64}, std::pair{150, 77}}) {
        const auto grid = sight.fog(camera_x, camera_z);
        for (const bool classic : {false, true})
            for (const bool moved : {false, true}) {
                fog::FogPlacement placement = plain(camera_x, camera_z);
                if (moved) {
                    placement.origin_x = 7.0F;
                    placement.origin_y = -5.0F;
                    placement.scale = 2.0F;
                }
                card::CardFrame frame;
                if (classic)
                    fog::append_unseen_terrain_classic(
                        frame,
                        grid,
                        shapes,
                        built.atlas,
                        built.pages,
                        levels,
                        placement,
                        {},
                        nullptr
                    );
                else
                    fog::append_unseen_terrain(
                        frame, grid, built.atlas, built.pages, levels, placement, {}, nullptr
                    );
                const auto terrain = quad_triangles(frame);
                // Rectangles over the view, on whole and on fractional pixels.
                for (const auto& rect :
                     {std::array<float, 4>{20.0F, 13.0F, 103.0F, 97.0F},
                      std::array<float, 4>{3.5F, 40.25F, 61.75F, 119.5F},
                      std::array<float, 4>{70.0F, 2.0F, 140.0F, 60.0F}}) {
                    std::vector<fog::CoverPiece> pieces;
                    const fog::Cover cover = fog::greyed_cover(
                        grid,
                        classic ? &shapes : nullptr,
                        placement,
                        rect[0],
                        rect[1],
                        rect[2],
                        rect[3],
                        pieces
                    );
                    const auto covering = cover_triangles(pieces);
                    OA_CHECK((cover == fog::Cover::part) == !pieces.empty());
                    parts += cover == fog::Cover::part ? 1 : 0;
                    float most = 0.0F;
                    bool past = false;
                    for (float y = rect[1] - 3.5F; y < rect[3] + 3.5F; y += 1.0F)
                        for (float x = rect[0] - 3.5F; x < rect[2] + 3.5F; x += 1.0F) {
                            const bool within =
                                x > rect[0] && x < rect[2] && y > rect[1] && y < rect[3];
                            const float drawn = cover == fog::Cover::whole
                                                    ? (within ? 1.0F : 0.0F)
                                                    : alpha_over(covering, x, y);
                            if (!within) {
                                past = past || drawn != 0.0F;
                                continue;
                            }
                            // Past the map's edges the terrain draws nothing,
                            // and a feature's frame is cut off by its scissor.
                            const float map_x = (x - placement.origin_x) / placement.scale +
                                                static_cast<float>(camera_x);
                            const float map_z = (y - placement.origin_y) / placement.scale +
                                                static_cast<float>(camera_z);
                            if (map_x < 0.0F || map_z < 0.0F ||
                                map_x >= static_cast<float>(map_tiles_wide * tile_edge) ||
                                map_z >= static_cast<float>(map_tiles_high * tile_edge))
                                continue;
                            most = std::max(most, std::fabs(drawn - alpha_over(terrain, x, y)));
                        }
                    OA_CHECK(most <= 1e-4F);
                    OA_CHECK(!past);
                    if (most > 1e-4F || past)
                        std::printf(
                            "greyed cover at %d,%d %s%s over %.2f,%.2f-%.2f,%.2f: most %.5f%s\n",
                            camera_x,
                            camera_z,
                            classic ? "classic" : "smooth",
                            moved ? " moved" : "",
                            rect[0],
                            rect[1],
                            rect[2],
                            rect[3],
                            static_cast<double>(most),
                            past ? ", past the rectangle" : ""
                        );
                }
            }
    }
    OA_CHECK(parts > 0);
    // Wholly in sight: nothing; wholly out of sight: whole; an empty
    // rectangle or an empty grid: nothing.
    {
        const Sight seen;
        std::vector<fog::CoverPiece> pieces;
        OA_CHECK(
            fog::greyed_cover(seen.fog(0, 0), nullptr, plain(0, 0), 10, 10, 90, 90, pieces) ==
            fog::Cover::none
        );
        Sight unseen;
        for (int32_t z = 0; z < unseen.grid.height; ++z)
            for (int32_t x = 0; x < unseen.grid.width; ++x)
                unseen.unsee(x, z);
        OA_CHECK(
            fog::greyed_cover(unseen.fog(0, 0), nullptr, plain(0, 0), 10, 10, 90, 90, pieces) ==
            fog::Cover::whole
        );
        OA_CHECK(pieces.empty());
        OA_CHECK(
            fog::greyed_cover(unseen.fog(0, 0), &shapes, plain(0, 0), 10, 10, 90, 90, pieces) ==
            fog::Cover::whole
        );
        OA_CHECK(
            fog::greyed_cover(unseen.fog(0, 0), nullptr, plain(0, 0), 10, 10, 10, 90, pieces) ==
            fog::Cover::none
        );
        OA_CHECK(
            fog::greyed_cover(wr::FogGrid{}, nullptr, plain(0, 0), 10, 10, 90, 90, pieces) ==
            fog::Cover::none
        );
        // Never mapped at every corner: the black pass covers it, not the grey.
        Sight unmapped;
        for (int32_t z = 0; z < unmapped.grid.height; ++z)
            for (int32_t x = 0; x < unmapped.grid.width; ++x)
                unmapped.unmap(x, z);
        OA_CHECK(
            fog::greyed_cover(unmapped.fog(0, 0), nullptr, plain(0, 0), 10, 10, 90, 90, pieces) ==
            fog::Cover::none
        );
    }
}

int main() {
    corner_alphas_follow_the_mask();
    pass_alphas_compose_the_weighted_picture();
    greyed_pass_covers_what_is_out_of_sight();
    two_levels_draw_the_quads_twice();
    placement_moves_and_scales_the_quads();
    whole_pixels_meet_at_a_sixth();
    solid_passes_merge_their_runs();
    level_quads_follow_the_tables();
    mask_reads_the_corner_alphas();
    refusals_add_nothing();
    classic_shapes_cover_the_art();
    dither_page_is_the_checkerboard();
    classic_passes_draw_the_processors_tiles();
    greyed_cover_takes_the_terrains_alphas();
    if (oa::test::check_exit_status() != 0)
        return 1;
    std::puts(
        "full fog: the greyed pass covers the ground out of sight once at its tiles' texels, "
        "the black and dithered passes their tiles with runs merged, at the corner alphas; "
        "the fog mask reads the corner alphas across each tile; shade and light levels as quads; "
        "the Classic passes draw the processor's FOG.GAF tiles; a greyed cover takes the "
        "terrain's alphas"
    );
    return 0;
}
