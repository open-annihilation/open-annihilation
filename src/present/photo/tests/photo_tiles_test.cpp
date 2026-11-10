// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The tiles of a Full classic photo: coverage, the card's limit, the border
// and the map grid.
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
using oa::present::photo::draws_whole;
using oa::present::photo::full_tile_reach;
using oa::present::photo::plan_tiles;
using oa::present::photo::tile_border;

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

    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    return 0;
}
