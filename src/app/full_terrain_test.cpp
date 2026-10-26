// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Full tier's terrain builder: the level rule at every kind of zoom, and
// the tiles' quads over a small map packed on three atlas pages: where each
// lands, which texels it reads, one batch per page whatever the grid's
// order, the view's tile range within the map and the frame well formed;
// and the same terrain drawn again where Water ripples move its art.
#include "full_terrain.hpp"

#include "oa/test/check.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {

namespace card = oa::app::card;
namespace gw = oa::present::gpu_world;
namespace ft = oa::app::full_terrain;

using Map = oa::formats::tnt::Map;

constexpr std::size_t tile_bytes = oa::formats::tnt::layout::tile_bytes;

/// A palette whose entries all differ.
oa::PaletteBytes test_palette() {
    oa::PaletteBytes palette{};
    for (std::size_t index = 0; index < oa::palette_color_count; ++index) {
        palette[index * oa::palette_entry_bytes] = static_cast<uint8_t>(index);
        palette[index * oa::palette_entry_bytes + 1] = static_cast<uint8_t>(index * 5U + 1U);
        palette[index * oa::palette_entry_bytes + 2] = static_cast<uint8_t>(255U - index);
    }
    return palette;
}

/// A map of 3 by 2 tiles naming 3 distinct tiles: the grid 0 1 2 / 2 1 0.
Map test_map() {
    Map map;
    map.tile_width = 3;
    map.tile_height = 2;
    map.tile_count = 3;
    map.tile_palette_indices.resize(static_cast<std::size_t>(map.tile_count) * tile_bytes);
    for (std::size_t i = 0; i < map.tile_palette_indices.size(); ++i)
        map.tile_palette_indices[i] = static_cast<uint8_t>((i * 37U + i / tile_bytes) & 0xffU);
    map.tile_indices = {0, 1, 2, 2, 1, 0};
    return map;
}

bool near(float a, float b) {
    return std::fabs(a - b) < 1.0e-5F;
}

/// The level rule: level 1 alone at 0.5, the blend between, level 2 under
/// level 1 below 0.5 and alone at 0.25 and below, level 0 NEAREST at whole
/// numbers, pixel-art or the target at other zooms above 1.
void test_plan() {
    using card::Blend;
    using card::Sampling;
    const auto half = ft::plan_terrain_draw(0.5F, false);
    OA_CHECK(half.pass_count == 1 && half.passes[0].level == 1);
    OA_CHECK(half.passes[0].sampling == Sampling::linear && half.passes[0].blend == Blend::none);
    OA_CHECK(!half.through_target);
    // Between 0.5 and 0.25 level 2 is drawn first and level 1 blended over
    // it, at the alpha of the same rule one level down.
    const auto below = ft::plan_terrain_draw(0.3F, false);
    OA_CHECK(below.pass_count == 2 && below.passes[0].level == 2 && below.passes[1].level == 1);
    OA_CHECK(below.passes[0].sampling == Sampling::linear && below.passes[1].blend == Blend::alpha);
    OA_CHECK(near(
        below.passes[1].alpha, static_cast<float>(2.0 - std::log2(1.0 / static_cast<double>(0.3F)))
    ));
    // At 0.25 and at the Full tier's floor of one sixth
    // (kMinFullBattlefieldZoom) level 2 alone, reduced.
    for (const float zoom : {0.25F, 1.0F / 6.0F, 0.1F}) {
        const auto far = ft::plan_terrain_draw(zoom, false);
        OA_CHECK(far.pass_count == 1 && far.passes[0].level == 2);
        OA_CHECK(far.passes[0].sampling == Sampling::linear && far.passes[0].blend == Blend::none);
        OA_CHECK(!far.through_target);
    }
    for (const float zoom : {0.6F, 0.75F, 0.9F, 0.999F}) {
        const auto plan = ft::plan_terrain_draw(zoom, false);
        OA_CHECK(plan.pass_count == 2);
        OA_CHECK(plan.passes[0].level == 1 && plan.passes[0].sampling == Sampling::linear);
        OA_CHECK(plan.passes[1].level == 0 && plan.passes[1].sampling == Sampling::linear);
        OA_CHECK(plan.passes[1].blend == Blend::alpha);
        OA_CHECK(near(
            plan.passes[1].alpha,
            static_cast<float>(1.0 - std::log2(1.0 / static_cast<double>(zoom)))
        ));
        OA_CHECK(!plan.through_target);
    }
    OA_CHECK(near(ft::plan_terrain_draw(0.75F, false).passes[1].alpha, 0.584963F));
    for (const float zoom : {1.0F, 2.0F, 3.0F, 4.0F})
        for (const bool pixel_art : {false, true}) {
            const auto plan = ft::plan_terrain_draw(zoom, pixel_art);
            OA_CHECK(plan.pass_count == 1 && plan.passes[0].level == 0);
            OA_CHECK(plan.passes[0].sampling == Sampling::nearest);
            OA_CHECK(plan.passes[0].blend == Blend::none && plan.passes[0].alpha == 1.0F);
            OA_CHECK(!plan.through_target);
        }
    for (const float zoom : {1.37F, 2.5F, 3.9F}) {
        const auto art = ft::plan_terrain_draw(zoom, true);
        OA_CHECK(art.pass_count == 1 && art.passes[0].sampling == Sampling::pixel_art);
        OA_CHECK(!art.through_target);
        const auto target = ft::plan_terrain_draw(zoom, false);
        OA_CHECK(target.pass_count == 1 && target.passes[0].sampling == Sampling::nearest);
        OA_CHECK(target.through_target);
        OA_CHECK(target.target_zoom == static_cast<uint32_t>(std::ceil(zoom)));
    }
    OA_CHECK(ft::plan_terrain_draw(0.0F, false).pass_count == 1);
}

/// The quads of the tiles a view shows, from an atlas of two pages.
void test_tiles() {
    const Map map = test_map();
    const auto palette = test_palette();
    gw::TerrainAtlas atlas;
    // Within the smallest page edge each page holds one slot, so three
    // distinct tiles take three pages.
    OA_CHECK(
        gw::build_terrain_atlas(
            map, map.tile_width, map.tile_height, palette, nullptr, gw::page_edge_minimum, atlas
        ) == gw::TerrainAtlasError::none
    );
    OA_CHECK(atlas.pages.size() == 3 && atlas.grid_width == 3 && atlas.grid_height == 2);
    const std::vector<card::PageHandle> pages{{1}, {2}, {3}};

    // Zoom 1 at an origin, the camera inside the first tile: every tile the
    // 70 by 40 view touches, three across and two down, at whole pixels.
    ft::TerrainView view;
    view.camera_x = 5;
    view.camera_y = 7;
    view.origin_x = 10.0F;
    view.origin_y = 20.0F;
    view.scale = 1.0F;
    view.width = 70;
    view.height = 40;
    const ft::TileRange range = ft::visible_tiles(atlas, view);
    OA_CHECK(range.first_column == 0 && range.end_column == 3);
    OA_CHECK(range.first_row == 0 && range.end_row == 2);
    card::CardFrame frame;
    const card::Rect scissor{10, 20, 70, 40};
    const ft::TerrainPass level_0{0, card::Sampling::nearest, card::Blend::none, 1.0F};
    const uint32_t quads = ft::append_terrain_tiles(
        frame, atlas, pages, view, level_0, card::TargetHandle{}, &scissor
    );
    OA_CHECK(quads == 6);
    OA_CHECK(frame.vertices.size() == 24 && frame.indices.size() == 36);
    // Each tile is on its own page, and the grid names the pages 1 2 3 /
    // 3 2 1, so the six tiles make three batches, one per page in page
    // order, each of two tiles: the pass never draws a page twice however
    // the grid alternates.
    OA_CHECK(frame.batches.size() == 3);
    const uint32_t expected_pages[] = {1, 2, 3};
    constexpr uint32_t indices_per_page = 12;
    uint32_t first_index = 0;
    for (std::size_t i = 0; i < frame.batches.size(); ++i) {
        const auto& batch = frame.batches[i];
        OA_CHECK(batch.operation == card::Operation::draw);
        OA_CHECK(batch.page.value == expected_pages[i]);
        OA_CHECK(batch.level == 0 && batch.blend == card::Blend::none);
        OA_CHECK(batch.sampling == card::Sampling::nearest);
        OA_CHECK(batch.scissored && batch.scissor.x == 10 && batch.scissor.width == 70);
        OA_CHECK(batch.first_index == first_index && batch.index_count == indices_per_page);
        OA_CHECK(batch.target == card::TargetHandle{});
        first_index += indices_per_page;
        // Every index of the batch names a corner of a tile whose slot is
        // on the batch's page: each quad's four vertices follow one
        // another, its first the top-left corner, whose place gives the
        // tile's column and row, and the grid its slot.
        for (uint32_t index = batch.first_index; index < batch.first_index + batch.index_count;
             ++index) {
            OA_CHECK(frame.indices[index] < frame.vertices.size());
            const auto& corner = frame.vertices[(frame.indices[index] / 4U) * 4U];
            const auto column = static_cast<uint32_t>(
                (corner.x - view.origin_x + static_cast<float>(view.camera_x)) / gw::tile_edge
            );
            const auto row = static_cast<uint32_t>(
                (corner.y - view.origin_y + static_cast<float>(view.camera_y)) / gw::tile_edge
            );
            OA_CHECK(column < 3 && row < 2);
            const auto corner_rect = gw::tile_rect(atlas, atlas.grid[row * 3 + column], 0);
            OA_CHECK(corner_rect.has_value() && corner_rect->page + 1 == batch.page.value);
        }
        // The quad's two triangles share the diagonal from its top-left to
        // its bottom-right corner, as card::append_quad lays them.
        const card::Index first = frame.indices[batch.first_index];
        OA_CHECK(frame.indices[batch.first_index + 1] == first + 1);
        OA_CHECK(frame.indices[batch.first_index + 2] == first + 2);
        OA_CHECK(frame.indices[batch.first_index + 3] == first);
        OA_CHECK(frame.indices[batch.first_index + 4] == first + 2);
        OA_CHECK(frame.indices[batch.first_index + 5] == first + 3);
    }
    // The second tile of the first row lands 32 pixels right of the first,
    // which lands at origin - camera, and reads its tile's texels without
    // the gutter.
    const auto& first = frame.vertices[0];
    OA_CHECK(near(first.x, 5.0F) && near(first.y, 13.0F));
    const auto& second = frame.vertices[4];
    OA_CHECK(near(second.x, 37.0F) && near(second.y, 13.0F));
    const auto& second_far = frame.vertices[4 + 2];
    OA_CHECK(near(second_far.x, 69.0F) && near(second_far.y, 45.0F));
    const auto rect = gw::tile_rect(atlas, atlas.grid[1], 0);
    OA_CHECK(rect.has_value());
    const auto& level = atlas.pages[rect->page].levels[0];
    OA_CHECK(near(second.u * static_cast<float>(level.width), static_cast<float>(rect->x)));
    OA_CHECK(near(second.v * static_cast<float>(level.height), static_cast<float>(rect->y)));
    OA_CHECK(near(
        second_far.u * static_cast<float>(level.width), static_cast<float>(rect->x + rect->edge)
    ));
    OA_CHECK(rect->edge == gw::tile_edge);
    OA_CHECK(first.colour.alpha == 1.0F && first.colour.red == 1.0F);
    OA_CHECK(card::check_frame(frame).empty());

    // Level 1 at zoom 0.5 over the same map pixels: the quads are half the
    // size at half the offsets, and read level 1's texels, 16 a side.
    ft::TerrainView half = view;
    half.scale = 0.5F;
    half.width = 35;
    half.height = 20;
    half.camera_x = 4;
    half.camera_y = 6;
    card::CardFrame zoomed;
    const ft::TerrainPass level_1{1, card::Sampling::linear, card::Blend::none, 1.0F};
    OA_CHECK(
        ft::append_terrain_tiles(
            zoomed, atlas, pages, half, level_1, card::TargetHandle{}, nullptr
        ) == 6
    );
    OA_CHECK(near(zoomed.vertices[0].x, 8.0F) && near(zoomed.vertices[0].y, 17.0F));
    OA_CHECK(near(zoomed.vertices[2].x, 24.0F) && near(zoomed.vertices[2].y, 33.0F));
    const auto rect_1 = gw::tile_rect(atlas, atlas.grid[0], 1);
    OA_CHECK(rect_1.has_value() && rect_1->edge == gw::tile_edge / 2);
    const auto& level_1_size = atlas.pages[rect_1->page].levels[1];
    OA_CHECK(near(
        zoomed.vertices[0].u * static_cast<float>(level_1_size.width), static_cast<float>(rect_1->x)
    ));
    OA_CHECK(zoomed.batches.front().level == 1 && !zoomed.batches.front().scissored);
    OA_CHECK(zoomed.batches.front().sampling == card::Sampling::linear);
    OA_CHECK(card::check_frame(zoomed).empty());

    // The blend's level-0 pass carries its alpha in every corner.
    card::CardFrame blended;
    const ft::TerrainPass near_pass{0, card::Sampling::linear, card::Blend::alpha, 0.25F};
    OA_CHECK(
        ft::append_terrain_tiles(
            blended, atlas, pages, half, near_pass, card::TargetHandle{7}, nullptr
        ) == 6
    );
    for (const auto& vertex : blended.vertices)
        OA_CHECK(vertex.colour.alpha == 0.25F);
    OA_CHECK(blended.batches.front().blend == card::Blend::alpha);
    OA_CHECK(blended.batches.front().target.value == 7);

    // At a sixth with whole pixels, where a tile spans 5 1/3 pixels: every
    // corner on a whole pixel, and each tile's right and bottom edges the
    // left and top edges of the next, so that no row or column lies
    // between them; without, the corners keep their fractions.
    ft::TerrainView sixth = view;
    sixth.scale = 1.0F / 6.0F;
    sixth.camera_x = 0;
    sixth.camera_y = 0;
    sixth.origin_x = 0.0F;
    sixth.origin_y = 0.0F;
    sixth.whole_pixels = true;
    card::CardFrame snapped;
    OA_CHECK(
        ft::append_terrain_tiles(
            snapped, atlas, pages, sixth, level_1, card::TargetHandle{}, nullptr
        ) == 6
    );
    for (const auto& vertex : snapped.vertices)
        OA_CHECK(vertex.x == std::round(vertex.x) && vertex.y == std::round(vertex.y));
    // The tiles in grid order, four corners each: the first row's three,
    // then the second's.
    OA_CHECK(snapped.vertices[1].x == snapped.vertices[4].x);
    OA_CHECK(snapped.vertices[5].x == snapped.vertices[8].x);
    OA_CHECK(snapped.vertices[3].y == snapped.vertices[12].y);
    OA_CHECK(near(snapped.vertices[1].x, 5.0F) && near(snapped.vertices[5].x, 11.0F));
    sixth.whole_pixels = false;
    card::CardFrame fractional;
    OA_CHECK(
        ft::append_terrain_tiles(
            fractional, atlas, pages, sixth, level_1, card::TargetHandle{}, nullptr
        ) == 6
    );
    OA_CHECK(near(fractional.vertices[1].x, 32.0F / 6.0F));

    // A view that starts in the last tile shows that tile alone, and one
    // past the map shows nothing; a view narrower than a tile shows the
    // tile under it and the one its far edge touches.
    ft::TerrainView corner = view;
    corner.camera_x = 70;
    corner.camera_y = 40;
    const auto last = ft::visible_tiles(atlas, corner);
    OA_CHECK(last.first_column == 2 && last.end_column == 3);
    OA_CHECK(last.first_row == 1 && last.end_row == 2);
    ft::TerrainView beyond = view;
    beyond.camera_x = 96;
    const auto none = ft::visible_tiles(atlas, beyond);
    OA_CHECK(none.first_column == none.end_column);
    card::CardFrame empty;
    OA_CHECK(
        ft::append_terrain_tiles(
            empty, atlas, pages, beyond, level_0, card::TargetHandle{}, nullptr
        ) == 0
    );
    OA_CHECK(empty.batches.empty());
    ft::TerrainView narrow = view;
    narrow.camera_x = 30;
    narrow.width = 4;
    narrow.height = 4;
    const auto two = ft::visible_tiles(atlas, narrow);
    OA_CHECK(two.first_column == 0 && two.end_column == 2 && two.end_row == 1);

    // A level past the tile levels, or too few pages, appends nothing.
    card::CardFrame refused;
    const ft::TerrainPass deep{3, card::Sampling::nearest, card::Blend::none, 1.0F};
    OA_CHECK(
        ft::append_terrain_tiles(
            refused, atlas, pages, view, deep, card::TargetHandle{}, nullptr
        ) == 0
    );
    const std::vector<card::PageHandle> short_pages{{1}};
    OA_CHECK(
        ft::append_terrain_tiles(
            refused, atlas, short_pages, view, level_0, card::TargetHandle{}, nullptr
        ) == 0
    );
}

/// A mosaic's chunks: one for a picture within the limit; past it, drawn
/// parts that follow one another across the whole picture, each texture
/// within the limit and holding one texel of each neighbour; none for a
/// level without a mosaic, a limit too small or an empty grid.
void test_mosaic_plan() {
    const ft::MosaicLevel whole = ft::plan_mosaic_level(3, 2, 1, 4096);
    OA_CHECK(whole.level == 1 && whole.width == 48 && whole.height == 32);
    OA_CHECK(whole.chunks.size() == 1);
    if (whole.chunks.size() == 1) {
        const ft::MosaicChunk& chunk = whole.chunks[0];
        OA_CHECK(chunk.x == 0 && chunk.y == 0 && chunk.width == 48 && chunk.height == 32);
        OA_CHECK(chunk.drawn_x == 0 && chunk.drawn_width == 48 && chunk.drawn_height == 32);
    }
    // Ten tiles at level 1 are 160 texels across: at most 64 a chunk, each
    // drawing 62 where it meets another.
    const ft::MosaicLevel cut = ft::plan_mosaic_level(10, 1, 1, 64);
    OA_CHECK(cut.width == 160 && cut.height == 16 && cut.chunks.size() == 3);
    uint32_t next = 0;
    for (std::size_t index = 0; index < cut.chunks.size(); ++index) {
        const ft::MosaicChunk& chunk = cut.chunks[index];
        OA_CHECK(chunk.width <= 64 && chunk.height == 16);
        OA_CHECK(chunk.drawn_x == next);
        OA_CHECK(chunk.x == (index == 0 ? 0U : chunk.drawn_x - 1U));
        const bool last = index + 1 == cut.chunks.size();
        OA_CHECK(chunk.x + chunk.width == chunk.drawn_x + chunk.drawn_width + (last ? 0U : 1U));
        next = chunk.drawn_x + chunk.drawn_width;
    }
    OA_CHECK(next == cut.width);
    // Level 2's tiles are 8 texels a side.
    const ft::MosaicLevel quarter = ft::plan_mosaic_level(3, 2, 2, 4096);
    OA_CHECK(quarter.width == 24 && quarter.height == 16 && quarter.chunks.size() == 1);
    OA_CHECK(ft::plan_mosaic_level(3, 2, 0, 4096).chunks.empty());
    OA_CHECK(ft::plan_mosaic_level(3, 2, 3, 4096).chunks.empty());
    OA_CHECK(ft::plan_mosaic_level(3, 2, 1, 2).chunks.empty());
    OA_CHECK(ft::plan_mosaic_level(0, 2, 1, 4096).chunks.empty());
}

/// A pass from a mosaic: one quad a chunk the view shows, at the chunk's
/// drawn part, reading the chunk's texels of that part, one batch a chunk
/// on its page with the pass's sampling, blend and alpha; clipped to the
/// view; nothing for another level's pass or too few pages.
void test_mosaic_quads() {
    const ft::MosaicLevel mosaic = ft::plan_mosaic_level(10, 1, 1, 64);
    const std::vector<card::PageHandle> pages{{1}, {2}, {3}};
    ft::TerrainView view;
    view.scale = 0.25F;
    view.width = 80;
    view.height = 8;
    const ft::TerrainPass pass{1, card::Sampling::linear, card::Blend::alpha, 0.5F};
    card::CardFrame frame;
    OA_CHECK(
        ft::append_terrain_mosaic(
            frame, mosaic, pages, view, pass, card::TargetHandle{}, nullptr
        ) == 3
    );
    OA_CHECK(
        frame.batches.size() == 3 && frame.vertices.size() == 12 && frame.indices.size() == 18
    );
    OA_CHECK(card::check_frame(frame).empty());
    // The chunks' drawn parts end at map pixels 124, 248 and 320, a
    // quarter of that in the window.
    const float ends[] = {31.0F, 62.0F, 80.0F};
    for (std::size_t index = 0; index < frame.batches.size() && index < 3; ++index) {
        const card::Batch& batch = frame.batches[index];
        OA_CHECK(batch.page == pages[index] && batch.level == 0);
        OA_CHECK(batch.sampling == card::Sampling::linear && batch.blend == card::Blend::alpha);
        OA_CHECK(batch.index_count == 6);
        const card::Vertex& corner = frame.vertices[index * 4 + 2];
        OA_CHECK(near(corner.x, ends[index]) && near(corner.y, 8.0F));
        OA_CHECK(near(corner.colour.alpha, 0.5F));
    }
    // The first chunk's texture starts at the picture's first texel: its
    // drawn part reads from 0 to 62 of its 63 texels.
    OA_CHECK(near(frame.vertices[0].u, 0.0F) && near(frame.vertices[2].u, 62.0F / 63.0F));
    // The second's starts a texel before its drawn part.
    const ft::MosaicChunk& second = mosaic.chunks[1];
    OA_CHECK(near(frame.vertices[4].u, 1.0F / static_cast<float>(second.width)));

    // A view from map pixel 100, 20 pixels wide, shows the first chunk from
    // 100 and the second to 180.
    ft::TerrainView part = view;
    part.camera_x = 100;
    part.width = 20;
    card::CardFrame clipped;
    OA_CHECK(
        ft::append_terrain_mosaic(
            clipped, mosaic, pages, part, pass, card::TargetHandle{}, nullptr
        ) == 2
    );
    OA_CHECK(clipped.vertices.size() == 8);
    if (clipped.vertices.size() == 8) {
        OA_CHECK(near(clipped.vertices[0].x, 0.0F) && near(clipped.vertices[2].x, 6.0F));
        OA_CHECK(near(clipped.vertices[6].x, 20.0F) || near(clipped.vertices[5].x, 20.0F));
        OA_CHECK(near(clipped.vertices[0].u, 50.0F / 63.0F));
    }

    card::CardFrame refused;
    const ft::TerrainPass other{2, card::Sampling::linear, card::Blend::none, 1.0F};
    OA_CHECK(
        ft::append_terrain_mosaic(
            refused, mosaic, pages, view, other, card::TargetHandle{}, nullptr
        ) == 0
    );
    const std::vector<card::PageHandle> short_pages{{1}};
    OA_CHECK(
        ft::append_terrain_mosaic(
            refused, mosaic, short_pages, view, pass, card::TargetHandle{}, nullptr
        ) == 0
    );
    OA_CHECK(refused.batches.empty() && refused.vertices.empty());
}

/// A motion over the whole test map's grid of `spacing`: `offset` from map
/// pixel `from` across on, none before it.
ft::ArtMotion test_motion(
    uint32_t spacing, uint32_t width, uint32_t height, float from, std::array<float, 2> offset
) {
    ft::ArtMotion motion;
    motion.spacing = spacing;
    motion.columns = width / spacing + 1;
    motion.rows = height / spacing + 1;
    motion.offsets.resize(static_cast<std::size_t>(motion.columns) * motion.rows);
    for (uint32_t row = 0; row < motion.rows; ++row)
        for (uint32_t column = 0; column < motion.columns; ++column)
            if (static_cast<float>(column * spacing) >= from)
                motion.offsets[static_cast<std::size_t>(row) * motion.columns + column] = offset;
    return motion;
}

/// A motion's grid: its points exact, the blend between, nothing past it.
void test_art_motion() {
    ft::ArtMotion motion = test_motion(8, 16, 16, 8, {2.0F, -1.0F});
    OA_CHECK(motion.columns == 3 && motion.rows == 3 && !motion.still());
    OA_CHECK((motion.at(8, 0) == std::array<float, 2>{2.0F, -1.0F}));
    OA_CHECK((motion.at(0, 8) == std::array<float, 2>{0.0F, 0.0F}));
    OA_CHECK(near(motion.at(4, 5)[0], 1.0F) && near(motion.at(4, 5)[1], -0.5F));
    OA_CHECK(near(motion.at(16, 16)[0], 2.0F));
    OA_CHECK((motion.at(-1, 0) == std::array<float, 2>{}));
    OA_CHECK((motion.at(16.5, 0) == std::array<float, 2>{}));
    motion.offsets.assign(motion.offsets.size(), {});
    OA_CHECK(motion.still());
}

/// The tiles drawn again where the motion moves their art: only the squares
/// that move, each corner where the tile pass landed it plus the motion,
/// reading the texels of its unmoved place, the squares of neighbouring
/// tiles meeting, one batch a page at the pass's look.
void test_moved_tiles() {
    const Map map = test_map();
    const auto palette = test_palette();
    gw::TerrainAtlas atlas;
    OA_CHECK(
        gw::build_terrain_atlas(
            map, map.tile_width, map.tile_height, palette, nullptr, gw::page_edge_minimum, atlas
        ) == gw::TerrainAtlasError::none
    );
    const std::vector<card::PageHandle> pages{{1}, {2}, {3}};
    ft::TerrainView view;
    view.origin_x = 10.0F;
    view.origin_y = 20.0F;
    view.scale = 2.0F;
    view.width = 192;
    view.height = 128;
    const ft::TerrainPass pass{0, card::Sampling::nearest, card::Blend::none, 1.0F};
    // The art moves from map pixel 48 across on, a pixel and a half right
    // and half a pixel up.
    const ft::ArtMotion motion = test_motion(8, 96, 64, 48, {1.5F, -0.5F});
    card::CardFrame base;
    ft::append_terrain_tiles(base, atlas, pages, view, pass, card::TargetHandle{}, nullptr);
    card::CardFrame frame;
    const uint32_t squares = ft::append_moved_terrain_tiles(
        frame, atlas, pages, view, pass, motion, card::TargetHandle{}, nullptr
    );
    // Columns of squares from 40, whose right corners move, to 96: seven
    // across, eight down.
    OA_CHECK(squares == 7 * 8);
    OA_CHECK(card::check_frame(frame).empty());
    OA_CHECK(frame.indices.size() == squares * 6U);
    OA_CHECK(frame.batches.size() == 3);
    for (const card::Batch& batch : frame.batches) {
        OA_CHECK(batch.blend == card::Blend::none && batch.sampling == card::Sampling::nearest);
        OA_CHECK(batch.level == 0 && !batch.scissored && batch.index_count % 6 == 0);
    }
    // Every corner lies at its map point at the view's scale plus the
    // motion's two target pixels per map pixel, and reads the texel the
    // tile pass reads at that point.
    for (const card::Batch& batch : frame.batches)
        for (uint32_t k = batch.first_index; k < batch.first_index + batch.index_count; ++k) {
            const card::Vertex& corner = frame.vertices[frame.indices[k]];
            // Find the map point from its texture coordinate on its tile.
            bool matched = false;
            for (uint32_t tile = 0; tile < 6 && !matched; ++tile) {
                const uint32_t column = tile % 3, row = tile / 3;
                const auto rect = gw::tile_rect(atlas, atlas.grid[tile], 0);
                if (!rect || rect->page + 1 != batch.page.value)
                    continue;
                const auto& level = atlas.pages[rect->page].levels[0];
                const float texel_x = corner.u * static_cast<float>(level.width) - rect->x;
                const float texel_y = corner.v * static_cast<float>(level.height) - rect->y;
                if (texel_x < -1.0e-3F || texel_y < -1.0e-3F || texel_x > 32.001F ||
                    texel_y > 32.001F)
                    continue;
                const float map_x = static_cast<float>(column * gw::tile_edge) + texel_x;
                const float map_y = static_cast<float>(row * gw::tile_edge) + texel_y;
                const auto moved = motion.at(map_x, map_y);
                if (near(corner.x, 10.0F + map_x * 2 + moved[0] * 2) &&
                    near(corner.y, 20.0F + map_y * 2 + moved[1] * 2))
                    matched = true;
            }
            OA_CHECK(matched);
        }
    // Two tiles' corners at the same map point land on the same pixel.
    std::vector<std::pair<float, float>> on_seam;
    for (const card::Vertex& corner : frame.vertices)
        if (near(corner.y, 20.0F + 32 * 2 + (-0.5F) * 2) && corner.x > 10.0F + 64 * 2)
            on_seam.emplace_back(corner.x, corner.y);
    OA_CHECK(on_seam.size() >= 2);
    // No motion, or a motion that stays, draws nothing.
    card::CardFrame none;
    OA_CHECK(
        ft::append_moved_terrain_tiles(
            none, atlas, pages, view, pass, ft::ArtMotion{}, card::TargetHandle{}, nullptr
        ) == 0
    );
    ft::ArtMotion still = motion;
    still.offsets.assign(still.offsets.size(), {});
    OA_CHECK(
        ft::append_moved_terrain_tiles(
            none, atlas, pages, view, pass, still, card::TargetHandle{}, nullptr
        ) == 0
    );
    OA_CHECK(none.vertices.empty() && none.batches.empty());
}

/// A mosaic's chunks drawn again moved: cut on the grid's lines and the
/// chunks' own edges, each square at its landing plus the motion, the
/// chunks meeting at their shared edge, one batch a chunk.
void test_moved_mosaic() {
    const ft::MosaicLevel mosaic = ft::plan_mosaic_level(10, 1, 1, 64);
    const std::vector<card::PageHandle> pages{{1}, {2}, {3}};
    ft::TerrainView view;
    view.scale = 0.25F;
    view.width = 80;
    view.height = 8;
    const ft::TerrainPass pass{1, card::Sampling::linear, card::Blend::alpha, 0.5F};
    const ft::ArtMotion motion = test_motion(16, 320, 32, 0, {2.0F, 1.0F});
    card::CardFrame frame;
    // The chunks' drawn parts end at map pixels 124, 248 and 320: eight
    // squares across the first, nine the second and five the third, two
    // down.
    OA_CHECK(
        ft::append_moved_terrain_mosaic(
            frame, mosaic, pages, view, pass, motion, card::TargetHandle{}, nullptr
        ) == (8 + 9 + 5) * 2
    );
    OA_CHECK(card::check_frame(frame).empty());
    OA_CHECK(frame.batches.size() == 3);
    for (std::size_t index = 0; index < frame.batches.size() && index < 3; ++index) {
        OA_CHECK(frame.batches[index].page == pages[index]);
        OA_CHECK(frame.batches[index].blend == card::Blend::alpha);
    }
    // Each chunk's corner at map pixel 124, 0 lands at 31, 0 moved by half
    // a pixel and a quarter, and the first chunk reads it at its texel 62.
    std::size_t seam = 0;
    for (const card::Vertex& corner : frame.vertices)
        if (near(corner.x, 31.5F) && near(corner.y, 0.25F)) {
            ++seam;
            OA_CHECK(near(corner.colour.alpha, 0.5F));
        }
    OA_CHECK(seam == 2);
    OA_CHECK(near(frame.vertices[0].x, 0.5F) && near(frame.vertices[0].u, 0.0F));
    card::CardFrame refused;
    const ft::TerrainPass other{2, card::Sampling::linear, card::Blend::none, 1.0F};
    OA_CHECK(
        ft::append_moved_terrain_mosaic(
            refused, mosaic, pages, view, other, motion, card::TargetHandle{}, nullptr
        ) == 0
    );
    OA_CHECK(refused.vertices.empty());
}

} // namespace

int main() {
    test_plan();
    test_tiles();
    test_mosaic_plan();
    test_mosaic_quads();
    test_art_motion();
    test_moved_tiles();
    test_moved_mosaic();
    return oa::test::check_exit_status();
}
