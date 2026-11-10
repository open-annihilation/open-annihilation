// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The tiles of a photo: Full classic against the card, and the turned view
// against the live renderer's edge.
#include "oa/present/photo/photo_tiles.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

using oa::present::photo::Tile;
using oa::present::photo::TilePlan;
using oa::present::photo::TileReach;
using oa::present::photo::TileRequest;
using oa::present::photo::TurnedTileRequest;
using oa::present::photo::draws_whole;
using oa::present::photo::full_tile_reach;
using oa::present::photo::kLiveLightTile;
using oa::present::photo::plan_tiles;
using oa::present::photo::plan_turned_tiles;
using oa::present::photo::tile_border;
using oa::present::photo::turned_tile_border;
using oa::present::photo::turned_tile_viewport;

/// True when the kept pieces cover the photo exactly once.
///
/// @param plan the tiles
/// @param width the photo
/// @param height the photo
/// @return true when they do
bool covers_once(const TilePlan& plan, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0 || !plan.fits)
        return false;
    std::vector<int> hits(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    for (const Tile& tile : plan.tiles) {
        if (tile.x < 0 || tile.y < 0 || tile.width <= 0 || tile.height <= 0)
            return false;
        if (tile.x > width - tile.width || tile.y > height - tile.height)
            return false;
        for (int32_t row = 0; row < tile.height; ++row) {
            for (int32_t column = 0; column < tile.width; ++column) {
                const std::size_t at =
                    static_cast<std::size_t>(tile.y + row) * static_cast<std::size_t>(width) +
                    static_cast<std::size_t>(tile.x + column);
                ++hits[at];
            }
        }
    }
    for (const int hit : hits)
        if (hit != 1)
            return false;
    return true;
}

/// True when every inner border is at least the reach, and the photo's own
/// edges have none.
///
/// @param plan the tiles
/// @param width the photo
/// @param height the photo
/// @param reach the reach the border must cover
/// @return true when the borders do
bool borders_cover(const TilePlan& plan, int32_t width, int32_t height, int32_t reach) {
    for (const Tile& tile : plan.tiles) {
        const int32_t before = tile.x == 0 ? 0 : reach;
        const int32_t after = tile.x + tile.width == width ? 0 : reach;
        const int32_t above = tile.y == 0 ? 0 : reach;
        const int32_t below = tile.y + tile.height == height ? 0 : reach;
        if (tile.border_left < before || tile.border_right < after || tile.border_top < above ||
            tile.border_bottom < below)
            return false;
        if (tile.x == 0 && tile.border_left != 0)
            return false;
        if (tile.y == 0 && tile.border_top != 0)
            return false;
        if (tile.x + tile.width == width && tile.border_right != 0)
            return false;
        if (tile.y + tile.height == height && tile.border_bottom != 0)
            return false;
    }
    return true;
}

/// True when every tile's target fits and its draw starts on a whole map pixel.
///
/// @param plan the tiles
/// @param request the limit and the zoom
/// @return true when they do
bool targets_fit(const TilePlan& plan, const TileRequest& request) {
    if (!(request.zoom > 0.0F))
        return false;
    for (const Tile& tile : plan.tiles) {
        if (request.texture_limit > 0 && tile.texture_edge > request.texture_limit)
            return false;
        if (tile.draw_x < 0 || tile.draw_y < 0)
            return false;
        const double column = static_cast<double>(tile.draw_x) / static_cast<double>(request.zoom);
        const double row = static_cast<double>(tile.draw_y) / static_cast<double>(request.zoom);
        if (std::abs(column - std::round(column)) > 1e-3 || std::abs(row - std::round(row)) > 1e-3)
            return false;
        if (tile.map_column != static_cast<int32_t>(std::lround(column)) ||
            tile.map_row != static_cast<int32_t>(std::lround(row)))
            return false;
        if (tile.draw_width != tile.width + tile.border_left + tile.border_right ||
            tile.draw_height != tile.height + tile.border_top + tile.border_bottom)
            return false;
    }
    return true;
}

/// Checks one plan that should fit.
///
/// @param request the photo
void check_fit(const TileRequest& request) {
    const TilePlan plan = plan_tiles(request);
    CHECK(plan.fits);
    CHECK(covers_once(plan, request.width, request.height));
    CHECK(borders_cover(plan, request.width, request.height, tile_border(request.reach)));
    CHECK(targets_fit(plan, request));
    CHECK(
        plan.border >= tile_border(request.reach) ||
        draws_whole(plan, request.width, request.height)
    );
}

} // namespace

int main() {
    const TileReach at_two = full_tile_reach(2.0F);
    CHECK(at_two.heat == 6);
    CHECK(at_two.light == 258);
    CHECK(at_two.fog == 64);
    CHECK(at_two.water == 20);
    CHECK(at_two.shadow == 8);
    CHECK(tile_border(at_two) == 264);
    CHECK(full_tile_reach(0.0F).fog == full_tile_reach(1.0F).fog);
    CHECK(full_tile_reach(-2.0F).light == 258);

    TileReach custom;
    custom.heat = 4;
    custom.light = 10;
    custom.fog = 3;
    custom.water = 7;
    custom.shadow = 1;
    CHECK(tile_border(custom) == 14);

    TileRequest whole;
    whole.width = 320;
    whole.height = 180;
    whole.zoom = 1.0F;
    whole.supersample = 2;
    whole.grain = 12;
    whole.texture_limit = 4096;
    whole.reach = full_tile_reach(1.0F);
    const TilePlan one = plan_tiles(whole);
    CHECK(draws_whole(one, 320, 180));
    CHECK(
        one.tiles.size() == 1 && one.tiles[0].border_left == 0 && one.tiles[0].texture_edge <= 4096
    );
    check_fit(whole);

    TileRequest forced = whole;
    forced.width = 1280;
    forced.height = 960;
    forced.zoom = 2.0F;
    forced.supersample = 4;
    forced.grain = 12;
    forced.texture_limit = 8192;
    forced.tile_edge = 320;
    forced.reach = full_tile_reach(2.0F);
    const TilePlan split = plan_tiles(forced);
    CHECK(split.fits);
    CHECK(!draws_whole(split, 1280, 960));
    CHECK(split.tiles.size() > 1);
    CHECK(split.border >= 264);
    check_fit(forced);
    for (const Tile& tile : split.tiles)
        CHECK(tile.width <= 320 && tile.height <= 320);

    const float zooms[] = {1.0F, 1.5F, 2.0F, 3.0F, 4.0F};
    const int32_t limits[] = {2048, 4096, 8192};
    const int32_t sides[] = {400, 960, 1600};
    for (const float zoom : zooms) {
        for (const int32_t limit : limits) {
            for (const int32_t side : sides) {
                TileRequest request;
                request.width = side;
                request.height = side * 3 / 4;
                request.zoom = zoom;
                request.supersample = zoom >= 1.0F ? 4 : 1;
                request.grain = 12;
                request.texture_limit = limit;
                request.reach = full_tile_reach(zoom);
                const TilePlan plan = plan_tiles(request);
                if (!plan.fits)
                    continue;
                CHECK(covers_once(plan, request.width, request.height));
                CHECK(
                    borders_cover(plan, request.width, request.height, tile_border(request.reach))
                );
                CHECK(targets_fit(plan, request));
            }
        }
    }

    TileRequest tight;
    tight.width = 400;
    tight.height = 320;
    tight.zoom = 4.0F;
    tight.supersample = 2;
    tight.grain = 12;
    tight.texture_limit = 500;
    tight.reach = full_tile_reach(4.0F);
    const TilePlan refused = plan_tiles(tight);
    CHECK(!refused.fits);
    CHECK(refused.tiles.empty());

    TileRequest huge;
    huge.width = 20000;
    huge.height = 20000;
    huge.zoom = 1.0F;
    huge.supersample = 2;
    huge.grain = 12;
    huge.texture_limit = 1024;
    huge.reach = full_tile_reach(1.0F);
    CHECK(!plan_tiles(huge).fits);

    TileRequest open;
    open.width = 100;
    open.height = 80;
    open.zoom = 1.0F;
    open.supersample = 1;
    open.grain = 1;
    open.texture_limit = 0;
    open.reach = full_tile_reach(1.0F);
    CHECK(draws_whole(plan_tiles(open), 100, 80));

    TileRequest foggy;
    foggy.width = 900;
    foggy.height = 600;
    foggy.zoom = 8.0F;
    foggy.supersample = 2;
    foggy.grain = 12;
    foggy.texture_limit = 8192;
    foggy.tile_edge = 400;
    foggy.reach = full_tile_reach(8.0F);
    CHECK(tile_border(foggy.reach) >= 256);
    const TilePlan fog_plan = plan_tiles(foggy);
    if (fog_plan.fits) {
        CHECK(fog_plan.border >= tile_border(foggy.reach));
        check_fit(foggy);
    }

    CHECK(turned_tile_border(0) == kLiveLightTile + 6);
    CHECK(turned_tile_border(128) == kLiveLightTile + 6 + 128);
    CHECK(turned_tile_border(-4) == kLiveLightTile + 6);

    TurnedTileRequest square;
    square.width = 4096;
    square.height = 4096;
    square.zoom = 1.0F;
    square.draw_limit = 4096;
    CHECK(draws_whole(plan_turned_tiles(square), 4096, 4096));

    square.tile_edge = 2048;
    const TilePlan halved = plan_turned_tiles(square);
    CHECK(halved.fits && halved.tiles.size() == 4);
    CHECK(halved.border >= turned_tile_border(0) && halved.border % kLiveLightTile == 0);
    CHECK(covers_once(halved, 4096, 4096));
    for (const Tile& tile : halved.tiles) {
        CHECK(tile.width == 2048 && tile.height == 2048);
        CHECK(tile.draw_width <= 4096 && tile.draw_height <= 4096);
        CHECK(tile.draw_x % kLiveLightTile == 0 && tile.draw_y % kLiveLightTile == 0);
        CHECK(tile.map_column == 0 && tile.map_row == 0);
        CHECK(tile.target_width == tile.draw_width && tile.target_height == tile.draw_height);
    }

    square.blur = 128;
    const TilePlan blurred = plan_turned_tiles(square);
    CHECK(blurred.fits && blurred.tiles.size() == 4);
    CHECK(blurred.border >= turned_tile_border(128) && blurred.border % kLiveLightTile == 0);
    CHECK(covers_once(blurred, 4096, 4096));
    for (const Tile& tile : blurred.tiles)
        CHECK(tile.draw_width <= 4096 && tile.draw_height <= 4096);

    TurnedTileRequest wide;
    wide.width = 8000;
    wide.height = 8000;
    wide.zoom = 1.0F;
    wide.draw_limit = 4096;
    const TilePlan wide_plan = plan_turned_tiles(wide);
    CHECK(wide_plan.fits && wide_plan.tiles.size() > 1);
    CHECK(covers_once(wide_plan, 8000, 8000));
    for (const Tile& tile : wide_plan.tiles) {
        CHECK(tile.draw_width <= 4096 && tile.draw_height <= 4096);
        CHECK(tile.draw_x % kLiveLightTile == 0 && tile.draw_y % kLiveLightTile == 0);
    }

    wide.zoom = 4.0F;
    const TilePlan zoomed = plan_turned_tiles(wide);
    CHECK(zoomed.fits && covers_once(zoomed, 8000, 8000));
    for (const Tile& tile : zoomed.tiles)
        CHECK(tile.draw_x % kLiveLightTile == 0 && tile.draw_y % kLiveLightTile == 0);

    TurnedTileRequest past;
    past.width = 20000;
    past.height = 20000;
    past.draw_limit = 4096;
    CHECK(!plan_turned_tiles(past).fits);

    TurnedTileRequest odd;
    odd.width = 1600;
    odd.height = 1600;
    odd.zoom = 1.0F;
    odd.draw_limit = 4096;
    odd.tile_edge = 500;
    const TilePlan snapped = plan_turned_tiles(odd);
    CHECK(snapped.fits && snapped.tiles.size() > 1);
    CHECK(covers_once(snapped, 1600, 1600));
    for (const Tile& tile : snapped.tiles) {
        CHECK(tile.width <= 500 && tile.height <= 500);
        CHECK(tile.width % kLiveLightTile == 0 || tile.x + tile.width == 1600);
        CHECK(tile.height % kLiveLightTile == 0 || tile.y + tile.height == 1600);
        CHECK(tile.draw_x % kLiveLightTile == 0 && tile.draw_y % kLiveLightTile == 0);
    }

    TurnedTileRequest one_piece;
    one_piece.width = 4000;
    one_piece.height = 2000;
    one_piece.draw_limit = 4096;
    CHECK(draws_whole(plan_turned_tiles(one_piece), 4000, 2000));

    // A card whose viewport holds the whole photo keeps that viewport.
    TurnedTileRequest roomy;
    roomy.width = 4096;
    roomy.height = 4096;
    roomy.zoom = 1.0F;
    roomy.draw_limit = 4096;
    roomy.viewport_limit = 16384;
    roomy.viewport_samples = 2;
    CHECK(draws_whole(plan_turned_tiles(roomy), 4096, 4096));
    const auto roomy_view =
        turned_tile_viewport(4096, 4096, 0, 0, 4096, 4096, 2, 16384, -32768.0F, 32767.0F);
    CHECK(!roomy_view.shifted && roomy_view.fits);
    CHECK(roomy_view.width <= 16384.0F && roomy_view.height <= 16384.0F);

    // A forced limit the whole photo's viewport passes, and a tile's does not.
    TurnedTileRequest capped;
    capped.width = 4096;
    capped.height = 4096;
    capped.zoom = 1.0F;
    capped.draw_limit = 4096;
    capped.viewport_limit = 6000;
    capped.viewport_samples = 2;
    const TilePlan capped_plan = plan_turned_tiles(capped);
    CHECK(capped_plan.fits && capped_plan.tiles.size() > 1);
    CHECK(covers_once(capped_plan, 4096, 4096));
    for (const Tile& tile : capped_plan.tiles) {
        const auto view = turned_tile_viewport(
            capped.width,
            capped.height,
            tile.draw_x,
            tile.draw_y,
            tile.draw_width,
            tile.draw_height,
            2,
            6000,
            -8192.0F,
            8191.0F
        );
        CHECK(view.shifted && view.fits);
        CHECK(view.width <= 6000.0F && view.height <= 6000.0F);
        CHECK(view.x >= -8192.0F && view.y >= -8192.0F);
        CHECK(view.x + view.width <= 8191.0F && view.y + view.height <= 8191.0F);
    }

    TurnedTileRequest under;
    under.width = 2000;
    under.height = 1000;
    under.draw_limit = 4096;
    under.viewport_limit = 6000;
    under.viewport_samples = 2;
    CHECK(draws_whole(plan_turned_tiles(under), 2000, 1000));

    TurnedTileRequest cramped;
    cramped.width = 4096;
    cramped.height = 4096;
    cramped.zoom = 1.0F;
    cramped.draw_limit = 4096;
    cramped.viewport_limit = 32;
    cramped.viewport_samples = 2;
    CHECK(!plan_turned_tiles(cramped).fits);

    TurnedTileRequest still_past;
    still_past.width = 20000;
    still_past.height = 20000;
    still_past.draw_limit = 4096;
    still_past.viewport_limit = 100000;
    still_past.viewport_samples = 1;
    CHECK(!plan_turned_tiles(still_past).fits);

    square.tile_edge = 2048;
    square.blur = 0;
    square.viewport_limit = 16384;
    square.viewport_samples = 1;
    const TilePlan held = plan_turned_tiles(square);
    CHECK(held.fits && held.tiles.size() == 4);
    for (const Tile& tile : held.tiles) {
        const auto view = turned_tile_viewport(
            4096,
            4096,
            tile.draw_x,
            tile.draw_y,
            tile.draw_width,
            tile.draw_height,
            1,
            16384,
            -32768.0F,
            32767.0F
        );
        CHECK(!view.shifted && view.fits);
        CHECK(view.width <= 16384.0F && view.height <= 16384.0F);
    }
    const auto corner = turned_tile_viewport(100, 100, 50, 0, 40, 100, 1, 200, 0.0F, 100.0F);
    CHECK(corner.shifted && corner.fits);
    const auto too_wide = turned_tile_viewport(80, 80, 0, 0, 80, 80, 2, 50, -100.0F, 100.0F);
    CHECK(too_wide.shifted && !too_wide.fits);

    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    return 0;
}
