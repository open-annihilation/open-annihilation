// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Splits a photo into tiles when one piece cannot hold it. Full classic tiles
// to the card's texture. The turned view tiles to the live renderer's edge,
// with a border for the lights, heat and depth-of-field blur.
#include "oa/present/photo/photo_tiles.hpp"

#include "oa/present/photo/photo_sizes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace oa::present::photo {
namespace {

/// The widest surface Full's effects read. A wider one is drawn with no
/// effects, so a tile stays inside it. A photo that fits in one piece is
/// not split for this.
constexpr int32_t kEffectSurface = 8192;

/// A count that is not negative.
///
/// @param value the count
/// @return `value`, or 0 when it is negative
int32_t non_negative(int32_t value) {
    return value > 0 ? value : 0;
}

/// A count rounded up to a multiple of a grain. A grain of 1 leaves it.
///
/// @param pixels the count
/// @param grain the grain, at least 1
/// @return the least multiple of the grain not below the count
int32_t rounded_up(int32_t pixels, int grain) {
    if (grain <= 1 || pixels <= 0)
        return pixels;
    const int32_t step = grain;
    return (pixels + step - 1) / step * step;
}

/// A count rounded up to a multiple of a step, clamped to a photo side.
///
/// @param value the count
/// @param step the step, at least 1
/// @return the snapped count
int32_t snap_up(int32_t value, int32_t step) {
    if (value <= 0)
        return 0;
    if (step <= 1)
        return value;
    const int64_t span = (static_cast<int64_t>(value) + step - 1) / step;
    const int64_t snapped = span * step;
    if (snapped > std::numeric_limits<int32_t>::max())
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(snapped);
}

/// A count rounded down to a multiple of a step.
///
/// @param value the count
/// @param step the step, at least 1
/// @return the snapped count, or 0 when `value` is shorter than the step
int32_t snap_down(int32_t value, int32_t step) {
    if (value <= 0)
        return 0;
    if (step <= 1)
        return value;
    return (value / step) * step;
}

/// Whole map pixels that land on whole photo pixels, the smallest such step.
struct GridStep {
    int32_t pixels{1};     ///< photo pixels
    int32_t map_pixels{1}; ///< whole map pixels those photo pixels cover
};

/// The smallest positive photo-pixel step that equals a whole number of map
/// pixels at `zoom`. The camera of a tile that starts on this step lands on
/// the grid.
///
/// @param zoom photo pixels a map pixel, positive
/// @return the step
GridStep photo_grid(float zoom) {
    const double scale = static_cast<double>(zoom);
    for (int span = 1; span <= 4096; ++span) {
        const double pixels = static_cast<double>(span) * scale;
        const double rounded = std::round(pixels);
        if (rounded < 1.0 || rounded > static_cast<double>(std::numeric_limits<int32_t>::max()))
            continue;
        const double slack = 1e-4 * std::max(1.0, rounded);
        if (std::abs(pixels - rounded) <= slack)
            return {static_cast<int32_t>(rounded), span};
    }
    return {};
}

/// True when the factor and the zoom build a world target.
///
/// @param request the photo
/// @return true when the target is built
bool makes_world_target(const TileRequest& request) {
    return request.supersample > 1 && request.zoom >= 1.0F;
}

/// The texture's longest side for a draw.
///
/// @param draw_width the draw
/// @param draw_height the draw
/// @param request the factor, the grain and the zoom
/// @return the side, or the largest photo side when it would not fit in one
int32_t texture_edge_of(int32_t draw_width, int32_t draw_height, const TileRequest& request) {
    if (!makes_world_target(request))
        return std::max(draw_width, draw_height);
    const int32_t grain = std::max(request.grain, 1);
    const int32_t supersample = std::max(request.supersample, 1);
    const int32_t target_side =
        std::max(rounded_up(draw_width, grain), rounded_up(draw_height, grain));
    const int64_t edge = static_cast<int64_t>(target_side) * supersample;
    if (edge > std::numeric_limits<int32_t>::max())
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(edge);
}

/// True when the draw's textures fit the card. A limit of 0 fits anything.
///
/// @param draw_width the draw
/// @param draw_height the draw
/// @param request the limit, the factor and the grain
/// @return true when the textures fit
bool fits_texture(int32_t draw_width, int32_t draw_height, const TileRequest& request) {
    if (draw_width <= 0 || draw_height <= 0)
        return false;
    if (request.texture_limit <= 0)
        return true;
    if (std::max(draw_width, draw_height) > request.texture_limit)
        return false;
    return texture_edge_of(draw_width, draw_height, request) <= request.texture_limit;
}

/// True when a tile's draw fits the card and the surface the effects read.
///
/// @param draw_width the draw
/// @param draw_height the draw
/// @param request the limits
/// @return true when the draw fits
bool fits_tile_draw(int32_t draw_width, int32_t draw_height, const TileRequest& request) {
    if (std::max(draw_width, draw_height) > kEffectSurface)
        return false;
    return fits_texture(draw_width, draw_height, request);
}

/// The longest side a tile's draw may have.
///
/// @param request the limits
/// @return the side, or 0 when nothing fits
int32_t max_draw_side(const TileRequest& request) {
    int32_t room = kEffectSurface;
    if (request.texture_limit > 0)
        room = std::min(room, request.texture_limit);
    if (!makes_world_target(request) || request.texture_limit <= 0)
        return room;
    const int32_t supersample = std::max(request.supersample, 1);
    const int32_t grain = std::max(request.grain, 1);
    const int32_t max_target = request.texture_limit / supersample;
    if (max_target <= 0)
        return 0;
    const int32_t grained = (max_target / grain) * grain;
    return std::min(room, grained);
}

/// One kept span along a side.
struct Span {
    int32_t origin{0}; ///< photo pixels from the photo's edge
    int32_t length{0}; ///< photo pixels kept
};

/// Splits one side into kept spans of at most `content`, each but the last
/// exactly `content`, every origin on the map step.
///
/// @param length the side
/// @param content the kept span, a multiple of the map step
/// @return the spans
std::vector<Span> split_side(int32_t length, int32_t content) {
    std::vector<Span> spans;
    if (length <= 0 || content <= 0)
        return spans;
    if (length <= content) {
        spans.push_back({0, length});
        return spans;
    }
    int32_t origin = 0;
    while (origin < length) {
        const int32_t remain = length - origin;
        const int32_t take = remain > content ? content : remain;
        spans.push_back({origin, take});
        origin += take;
    }
    return spans;
}

/// Fills a tile's target from its draw.
///
/// @param[in,out] tile the tile
/// @param request the factor, the grain and the zoom
void place_target(Tile& tile, const TileRequest& request) {
    if (makes_world_target(request)) {
        const int grain = std::max(request.grain, 1);
        tile.target_width = rounded_up(tile.draw_width, grain);
        tile.target_height = rounded_up(tile.draw_height, grain);
    } else {
        tile.target_width = tile.draw_width;
        tile.target_height = tile.draw_height;
    }
    tile.texture_edge = texture_edge_of(tile.draw_width, tile.draw_height, request);
}

/// The whole photo as one tile with no border.
///
/// @param request the photo
/// @return the tile
Tile whole_tile(const TileRequest& request) {
    Tile tile;
    tile.width = request.width;
    tile.height = request.height;
    tile.draw_width = request.width;
    tile.draw_height = request.height;
    place_target(tile, request);
    return tile;
}

/// The largest kept span an interior tile can draw, a multiple of the map
/// step, at least the border, and within the check's edge when that is set.
///
/// @param request the photo and the limits
/// @param border the inner border, already on the map step
/// @param step the map step, in photo pixels
/// @return the span, or 0 when no interior tile fits
int32_t largest_content(const TileRequest& request, int32_t border, int32_t step) {
    const int32_t window = max_draw_side(request);
    if (window <= 0 || step <= 0)
        return 0;
    int32_t cap = std::max(request.width, request.height);
    if (request.tile_edge > 0)
        cap = std::min(cap, request.tile_edge);
    if (border > 0) {
        if (window <= border * 2)
            return 0;
        cap = std::min(cap, window - border * 2);
    } else {
        cap = std::min(cap, window);
    }
    cap = snap_down(cap, step);
    if (cap < step || cap < border)
        return 0;
    for (int32_t content = cap; content >= border; content -= step) {
        const int32_t draw = content + border * 2;
        if (fits_tile_draw(draw, draw, request))
            return content;
        if (content < step)
            break;
    }
    return 0;
}

/// One tile from a column span and a row span.
///
/// @param column the kept columns
/// @param row the kept rows
/// @param width the photo
/// @param height the photo
/// @param border the inner border
/// @param grid the map step
/// @param request the factor and the grain
/// @param[out] tile the tile
/// @return false when the draw would start off the photo or miss the card
bool make_tile(
    Span column,
    Span row,
    int32_t width,
    int32_t height,
    int32_t border,
    GridStep grid,
    const TileRequest& request,
    Tile& tile
) {
    tile = {};
    tile.x = column.origin;
    tile.y = row.origin;
    tile.width = column.length;
    tile.height = row.length;
    tile.border_left = column.origin == 0 ? 0 : border;
    tile.border_right = column.origin + column.length == width ? 0 : border;
    tile.border_top = row.origin == 0 ? 0 : border;
    tile.border_bottom = row.origin + row.length == height ? 0 : border;
    tile.draw_x = tile.x - tile.border_left;
    tile.draw_y = tile.y - tile.border_top;
    if (tile.draw_x < 0 || tile.draw_y < 0 || tile.width <= 0 || tile.height <= 0)
        return false;
    const int64_t draw_width =
        static_cast<int64_t>(tile.width) + tile.border_left + tile.border_right;
    const int64_t draw_height =
        static_cast<int64_t>(tile.height) + tile.border_top + tile.border_bottom;
    if (draw_width > std::numeric_limits<int32_t>::max() ||
        draw_height > std::numeric_limits<int32_t>::max())
        return false;
    tile.draw_width = static_cast<int32_t>(draw_width);
    tile.draw_height = static_cast<int32_t>(draw_height);
    if (grid.pixels <= 0 || tile.draw_x % grid.pixels != 0 || tile.draw_y % grid.pixels != 0)
        return false;
    const int64_t map_column = static_cast<int64_t>(tile.draw_x / grid.pixels) * grid.map_pixels;
    const int64_t map_row = static_cast<int64_t>(tile.draw_y / grid.pixels) * grid.map_pixels;
    if (map_column > std::numeric_limits<int32_t>::max() ||
        map_row > std::numeric_limits<int32_t>::max())
        return false;
    tile.map_column = static_cast<int32_t>(map_column);
    tile.map_row = static_cast<int32_t>(map_row);
    if (!fits_tile_draw(tile.draw_width, tile.draw_height, request))
        return false;
    place_target(tile, request);
    return true;
}

/// The least common multiple of two positive steps.
///
/// @param left one step
/// @param right the other
/// @return the multiple, or the largest count when it would not fit
int32_t lowest_common_multiple(int32_t left, int32_t right) {
    if (left <= 0 || right <= 0)
        return 0;
    int32_t a = left;
    int32_t b = right;
    while (b != 0) {
        const int32_t rest = a % b;
        a = b;
        b = rest;
    }
    if (a <= 0)
        return 0;
    const int64_t multiple = (static_cast<int64_t>(left) / a) * right;
    if (multiple > std::numeric_limits<int32_t>::max())
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(multiple);
}

/// The longest side one turned-view tile may draw. Zero means the live edge.
///
/// @param draw_limit the sampling edge, or 0
/// @return the side, at most 4096
int32_t turned_draw_cap(int32_t draw_limit) {
    if (draw_limit <= 0)
        return kTurnedEdge;
    return std::min(draw_limit, kTurnedEdge);
}

/// The whole turned-view photo as one tile with no border. The camera stays
/// where it is, so the map shift is zero.
///
/// @param width the photo
/// @param height the photo
/// @return the tile
Tile turned_whole_tile(int32_t width, int32_t height) {
    Tile tile;
    tile.width = width;
    tile.height = height;
    tile.draw_width = width;
    tile.draw_height = height;
    tile.target_width = width;
    tile.target_height = height;
    tile.texture_edge = std::max(width, height);
    return tile;
}

/// The largest kept span a turned-view tile can draw, on the light grid.
///
/// @param width the photo
/// @param height the photo
/// @param tile_edge the check's kept edge, or 0
/// @param border the inner border, already on the light grid
/// @param step the light grid, in photo pixels
/// @param cap the longest draw
/// @return the span, or 0 when no tile fits
int32_t turned_content(
    int32_t width, int32_t height, int32_t tile_edge, int32_t border, int32_t step, int32_t cap
) {
    if (step <= 0 || cap <= 0 || border < 0)
        return 0;
    const int64_t both = static_cast<int64_t>(border) * 2;
    if (both >= cap)
        return 0;
    const int32_t room = cap - static_cast<int32_t>(both);
    int32_t span = std::max(width, height);
    if (tile_edge > 0)
        span = std::min(span, snap_down(tile_edge, step));
    span = std::min(span, room);
    span = snap_down(span, step);
    if (span < step)
        return 0;
    return span;
}

/// One turned-view tile. Its draw starts on a light square and stays within
/// the cap. The map shift stays zero: the runtime moves the projection.
///
/// @param column the kept columns
/// @param row the kept rows
/// @param width the photo
/// @param height the photo
/// @param border the inner border
/// @param step the light grid
/// @param cap the longest draw
/// @param[out] tile the tile
/// @return false when the draw would start off the photo or pass the cap
bool make_turned_tile(
    Span column,
    Span row,
    int32_t width,
    int32_t height,
    int32_t border,
    int32_t step,
    int32_t cap,
    Tile& tile
) {
    tile = {};
    tile.x = column.origin;
    tile.y = row.origin;
    tile.width = column.length;
    tile.height = row.length;
    tile.border_left = column.origin == 0 ? 0 : border;
    tile.border_right = column.origin + column.length == width ? 0 : border;
    tile.border_top = row.origin == 0 ? 0 : border;
    tile.border_bottom = row.origin + row.length == height ? 0 : border;
    tile.draw_x = tile.x - tile.border_left;
    tile.draw_y = tile.y - tile.border_top;
    if (tile.draw_x < 0 || tile.draw_y < 0 || tile.width <= 0 || tile.height <= 0)
        return false;
    const int64_t draw_width =
        static_cast<int64_t>(tile.width) + tile.border_left + tile.border_right;
    const int64_t draw_height =
        static_cast<int64_t>(tile.height) + tile.border_top + tile.border_bottom;
    if (draw_width > cap || draw_height > cap)
        return false;
    tile.draw_width = static_cast<int32_t>(draw_width);
    tile.draw_height = static_cast<int32_t>(draw_height);
    if (step <= 0 || tile.draw_x % step != 0 || tile.draw_y % step != 0)
        return false;
    tile.target_width = tile.draw_width;
    tile.target_height = tile.draw_height;
    tile.texture_edge = std::max(tile.draw_width, tile.draw_height);
    return true;
}

} // namespace

int32_t tile_border(TileReach reach) {
    const int32_t around = std::max(
        std::max(non_negative(reach.light), non_negative(reach.fog)),
        std::max(non_negative(reach.water), non_negative(reach.shadow))
    );
    const int64_t sum = static_cast<int64_t>(non_negative(reach.heat)) + around;
    if (sum > std::numeric_limits<int32_t>::max())
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(sum);
}

TileReach full_tile_reach(float zoom) {
    if (!(zoom > 0.0F))
        zoom = 1.0F;
    const auto map_pixels = [zoom](int32_t cells) {
        const double pixels = static_cast<double>(zoom) * static_cast<double>(cells);
        if (!(pixels > 0.0))
            return int32_t{0};
        if (pixels >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        return static_cast<int32_t>(std::ceil(pixels));
    };
    TileReach reach;
    reach.heat = kHeatBorder;
    reach.light = kLightReach;
    reach.fog = map_pixels(kFogCell);
    reach.water = map_pixels(kWaterReach);
    reach.shadow = map_pixels(kShadowReach);
    return reach;
}

TilePlan plan_tiles(const TileRequest& request) {
    TilePlan plan;
    TileRequest sane = request;
    if (!(sane.zoom > 0.0F))
        sane.zoom = 1.0F;
    sane.supersample = std::max(sane.supersample, 1);
    sane.grain = std::max(sane.grain, 1);
    sane.tile_edge = std::max(sane.tile_edge, int32_t{0});
    if (sane.width <= 0 || sane.height <= 0)
        return plan;

    const bool whole = fits_texture(sane.width, sane.height, sane);
    const bool forced =
        sane.tile_edge > 0 && (sane.width > sane.tile_edge || sane.height > sane.tile_edge);
    if (whole && !forced) {
        plan.tiles.push_back(whole_tile(sane));
        plan.fits = true;
        return plan;
    }
    if (sane.width > kProcessorEdge || sane.height > kProcessorEdge)
        return plan;

    const GridStep grid = photo_grid(sane.zoom);
    const int32_t border = snap_up(tile_border(sane.reach), grid.pixels);
    plan.border = border;
    const int32_t content = largest_content(sane, border, grid.pixels);
    if (content <= 0)
        return plan;
    const std::vector<Span> columns = split_side(sane.width, content);
    const std::vector<Span> rows = split_side(sane.height, content);
    if (columns.empty() || rows.empty())
        return plan;
    plan.tiles.reserve(columns.size() * rows.size());
    for (const Span& row : rows) {
        for (const Span& column : columns) {
            Tile tile;
            if (!make_tile(column, row, sane.width, sane.height, border, grid, sane, tile)) {
                plan.tiles.clear();
                return plan;
            }
            plan.tiles.push_back(tile);
        }
    }
    plan.fits = !plan.tiles.empty();
    return plan;
}

bool draws_whole(const TilePlan& plan, int32_t width, int32_t height) {
    if (!plan.fits || plan.tiles.size() != 1)
        return false;
    const Tile& tile = plan.tiles[0];
    return tile.x == 0 && tile.y == 0 && tile.width == width && tile.height == height &&
           tile.border_left == 0 && tile.border_top == 0 && tile.border_right == 0 &&
           tile.border_bottom == 0;
}

int32_t turned_tile_border(int32_t blur) {
    const int64_t sum = static_cast<int64_t>(kLiveLightTile) + kHeatBorder + non_negative(blur);
    if (sum > std::numeric_limits<int32_t>::max())
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(sum);
}

TilePlan plan_turned_tiles(const TurnedTileRequest& request) {
    TilePlan plan;
    TurnedTileRequest sane = request;
    if (!(sane.zoom > 0.0F))
        sane.zoom = 1.0F;
    sane.tile_edge = std::max(sane.tile_edge, int32_t{0});
    sane.blur = non_negative(sane.blur);
    if (sane.width <= 0 || sane.height <= 0)
        return plan;

    int32_t cap = turned_draw_cap(sane.draw_limit);
    limit_turned_draw(cap, sane.viewport_limit, sane.viewport_samples);
    const bool fits_one = sane.width <= cap && sane.height <= cap;
    const bool forced =
        sane.tile_edge > 0 && (sane.width > sane.tile_edge || sane.height > sane.tile_edge);
    if (fits_one && !forced) {
        plan.tiles.push_back(turned_whole_tile(sane.width, sane.height));
        plan.fits = true;
        return plan;
    }
    if (sane.width > kProcessorEdge || sane.height > kProcessorEdge)
        return plan;

    const GridStep grid = photo_grid(sane.zoom);
    const int32_t step = lowest_common_multiple(grid.pixels, kLiveLightTile);
    const int32_t border = snap_up(turned_tile_border(sane.blur), step);
    plan.border = border;
    const int32_t content =
        turned_content(sane.width, sane.height, sane.tile_edge, border, step, cap);
    if (content <= 0)
        return plan;
    const std::vector<Span> columns = split_side(sane.width, content);
    const std::vector<Span> rows = split_side(sane.height, content);
    if (columns.empty() || rows.empty())
        return plan;
    plan.tiles.reserve(columns.size() * rows.size());
    for (const Span& row : rows) {
        for (const Span& column : columns) {
            Tile tile;
            if (!make_turned_tile(column, row, sane.width, sane.height, border, step, cap, tile)) {
                plan.tiles.clear();
                plan.border = 0;
                return plan;
            }
            plan.tiles.push_back(tile);
        }
    }
    plan.fits = !plan.tiles.empty();
    return plan;
}

} // namespace oa::present::photo
