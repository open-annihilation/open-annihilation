// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How a photo is split into tiles when one piece cannot hold it. Full classic
// tiles to the card's texture. The turned view tiles to the live renderer's
// edge. Pure: no window, no drawing and no files. The runtime draws each
// tile the way it draws a whole photo of that mode, crops the border and
// places the kept piece. Grain and the anti-aliasing factor are arguments,
// so this module does not include either renderer.
#pragma once

#include <cstdint>
#include <vector>

namespace oa::present::photo {

/// Heat's sampling border, in photo pixels. Four pixels is the most heat
/// moves a sample, at 200%, and two more cover the sample beside that pixel.
inline constexpr int32_t kHeatBorder = 6;

/// The widest a light reaches, in photo pixels: the cap on a light, and the
/// extra column its edge reads on either side.
inline constexpr int32_t kLightReach = 258;

/// Smooth fog fades across one sight cell. The cell is this many map pixels.
inline constexpr int32_t kFogCell = 32;

/// The water effects' sampling, in map pixels, past the waves' own reach.
inline constexpr int32_t kWaterReach = 10;

/// The soft aircraft shadow's edge, in map pixels.
inline constexpr int32_t kShadowReach = 4;

/// The turned view packs lights into squares of this many output pixels,
/// aligned to the picture's origin.
inline constexpr int32_t kLiveLightTile = 32;

/// How far each Full pass reads past a kept pixel, in photo pixels. A map
/// pass's reach is already converted at the photo's zoom.
struct TileReach {
    int32_t heat{0};   ///< heat distortion's sampling
    int32_t light{0};  ///< the widest light
    int32_t fog{0};    ///< Smooth fog across a sight cell
    int32_t water{0};  ///< the water effects
    int32_t shadow{0}; ///< the soft aircraft shadow
};

/// The border an inner edge needs, in photo pixels: heat past the widest of
/// the other passes, since heat samples pixels those passes have already
/// drawn. Never negative.
///
/// @param reach how far each pass reads
/// @return the border, in photo pixels
[[nodiscard]] int32_t tile_border(TileReach reach);

/// The reaches Full's passes need at a photo's zoom. A zoom that is not
/// positive is read as 1. The map passes are converted to photo pixels.
///
/// @param zoom photo pixels a map pixel
/// @return the reaches
[[nodiscard]] TileReach full_tile_reach(float zoom);

/// A photo to split, and what a tile may use.
struct TileRequest {
    int32_t width{0};  ///< the photo, in photo pixels
    int32_t height{0}; ///< the photo, in photo pixels
    /// Photo pixels a map pixel. A zoom that is not positive is read as 1.
    float zoom{1.0F};
    /// The window's anti-aliasing factor. It is never lowered here. A factor
    /// above 1 at a zoom of 1 or more builds a world target.
    int supersample{1};
    /// The world target's side is a multiple of this, in photo pixels.
    int grain{1};
    /// The card's texture limit, in pixels a side. Zero means no cap.
    int32_t texture_limit{0};
    /// A check-only cap on a tile's kept edge, in photo pixels. Zero leaves
    /// the texture limit to decide. Smaller than the card's own limit, it
    /// forces tiles without changing the anti-aliasing factor.
    int32_t tile_edge{0};
    TileReach reach{}; ///< how far each pass reads
};

/// One tile: the kept piece of the photo, the border drawn around it, and
/// the target the card builds for the draw.
struct Tile {
    int32_t x{0};             ///< kept piece, photo pixels from the photo's left
    int32_t y{0};             ///< kept piece, photo pixels from the photo's top
    int32_t width{0};         ///< kept piece
    int32_t height{0};        ///< kept piece
    int32_t border_left{0};   ///< drawn past the kept piece; 0 on the photo's edge
    int32_t border_top{0};    ///< drawn past the kept piece; 0 on the photo's edge
    int32_t border_right{0};  ///< drawn past the kept piece; 0 on the photo's edge
    int32_t border_bottom{0}; ///< drawn past the kept piece; 0 on the photo's edge
    int32_t draw_x{0};        ///< the draw's left, in photo pixels; never negative
    int32_t draw_y{0};        ///< the draw's top, in photo pixels; never negative
    int32_t draw_width{0};    ///< the draw, kept piece plus the borders
    int32_t draw_height{0};   ///< the draw, kept piece plus the borders
    int32_t target_width{0};  ///< the world target, in photo pixels
    int32_t target_height{0}; ///< the world target, in photo pixels
    /// The texture's longest side: the world target times the factor when a
    /// world target is built, otherwise the draw's longest side.
    int32_t texture_edge{0};
    int32_t map_column{0}; ///< whole map pixels from the photo's camera
    int32_t map_row{0};    ///< whole map pixels from the photo's camera
};

/// The tiles of one photo.
struct TilePlan {
    std::vector<Tile> tiles; ///< empty when `fits` is false
    int32_t border{0};       ///< the inner edge, in photo pixels, snapped to the map
    bool fits{false};        ///< every tile's target fits, and the tiles cover the photo
};

/// Splits a photo into tiles. A photo whose own target fits, and whose sides
/// are within `tile_edge` when that is set, is one tile with no border: the
/// caller draws it whole. Otherwise each tile's target fits the texture
/// limit, each inner border is at least the reach, and every tile starts on
/// a whole map pixel so its camera lands on the grid. After the borders are
/// cropped, the kept pieces cover the photo exactly once. A picture past
/// 16384 photo pixels a side is split only when it does not already fit in
/// one piece; a tiled picture past that is not drawn. A tile's draw also
/// stays within the surface Full's effects read.
///
/// @param request the photo and the limits
/// @return the tiles
[[nodiscard]] TilePlan plan_tiles(const TileRequest& request);

/// True when the plan is the whole photo in one piece, with no border.
///
/// @param plan the tiles
/// @param width the photo
/// @param height the photo
/// @return true when the caller draws the photo as one piece
[[nodiscard]] bool draws_whole(const TilePlan& plan, int32_t width, int32_t height);

/// How far a turned-view tile draws past a kept pixel before the border is
/// snapped onto the light grid: one light square, heat's reach, and the
/// depth-of-field blur. The blur is already the picture's own radius.
///
/// @param blur the blur, in photo pixels; 0 when depth of field is off
/// @return the border, in photo pixels, never negative
[[nodiscard]] int32_t turned_tile_border(int32_t blur);

/// A turned-view photo to split, and what one tile may use.
struct TurnedTileRequest {
    int32_t width{0};  ///< the photo's draw, in photo pixels
    int32_t height{0}; ///< the photo's draw, in photo pixels
    /// Photo pixels a map pixel. A zoom that is not positive is read as 1.
    /// Tile origins land on a whole map pixel and on a light square.
    float zoom{1.0F};
    /// The longest side one tile's draw may have: the sampling edge, and
    /// never more than 4096. Zero means 4096.
    int32_t draw_limit{0};
    /// A check-only cap on a tile's kept edge, in photo pixels. Zero leaves
    /// the draw limit to decide. The kept edge is snapped down onto the
    /// light grid, so an edge that is not a multiple of that grid draws shorter.
    int32_t tile_edge{0};
    /// Depth-of-field blur, in photo pixels, already scaled by the picture's
    /// size and smooth step. The same radius on every tile. 0 when depth of
    /// field is off.
    int32_t blur{0};
    /// The card's largest viewport, in pixels. Zero does not cap the draw
    /// for it. A tile's colour viewport is its draw times `viewport_samples`.
    int32_t viewport_limit{0};
    /// The supersample a tile keeps. At least 1 when `viewport_limit` is set.
    int viewport_samples{1};
};

/// Shortens a turned-view draw cap so each tile's colour viewport fits the
/// card. A limit of 0 leaves `cap` as it is. A limit shorter than one sample
/// leaves no room.
///
/// @param cap the draw cap, in photo pixels
/// @param viewport_limit the card's largest viewport, in pixels, or 0
/// @param viewport_samples the supersample the tile keeps
inline void limit_turned_draw(int32_t& cap, int32_t viewport_limit, int viewport_samples) {
    if (viewport_limit <= 0 || cap <= 0)
        return;
    const int samples = viewport_samples > 0 ? viewport_samples : 1;
    const int32_t room = viewport_limit / samples;
    if (room < cap)
        cap = room;
}

/// Splits a turned-view photo into tiles the live renderer can draw. A photo
/// whose draw fits `draw_limit`, and whose sides are within `tile_edge` when
/// that is set, is one tile with no border. Otherwise each tile's draw is at
/// most 4096 pixels a side and within `draw_limit`, each inner border covers
/// the lights' squares, heat and the blur, and every tile's draw starts on a
/// light square so a pixel's lights are the whole photo's. The camera is not
/// moved: `draw_x` and `draw_y` are the shift of the photo's projection.
/// After the borders are cropped, the kept pieces cover the photo exactly
/// once. A picture past 16384 photo pixels a side is not drawn in tiles.
/// When `viewport_limit` is set, a tile's draw times `viewport_samples` also
/// stays within that viewport.
///
/// @param request the photo and the limits
/// @return the tiles
[[nodiscard]] TilePlan plan_turned_tiles(const TurnedTileRequest& request);

/// The colour viewport one turned-view tile is drawn through.
struct TurnedViewport {
    float x{0};          ///< left, in pixels of the colour picture
    float y{0};          ///< top, in pixels of the colour picture
    float width{0};      ///< pixels of the colour picture
    float height{0};     ///< pixels of the colour picture
    bool shifted{false}; ///< the projection is offset and the viewport is the tile
    bool fits{false};    ///< the viewport is within the card's limit
};

/// The viewport one turned-view tile is drawn through. The whole photo's
/// viewport when that fits `viewport_limit` and its corners, and the tile's
/// own viewport, with the projection offset onto the tile, when it does not.
/// A limit of 0 always returns the whole photo's viewport. `fits` is false
/// when even the tile's own viewport is past the limit: the caller draws
/// nothing rather than a viewport the card rejects.
///
/// @param photo_width the whole photo, in photo pixels
/// @param photo_height the whole photo, in photo pixels
/// @param origin_x the tile's draw, photo pixels from the photo's left
/// @param origin_y the tile's draw, photo pixels from the photo's top
/// @param tile_width the tile's draw
/// @param tile_height the tile's draw
/// @param supersample samples along each axis of the colour picture; 0 is read as 1
/// @param viewport_limit the card's largest viewport, in pixels, or 0
/// @param min_corner the least coordinate a viewport may start or end at
/// @param max_corner the greatest coordinate a viewport may start or end at
/// @return the colour viewport, in pixels of that picture
[[nodiscard]] inline TurnedViewport turned_tile_viewport(
    int32_t photo_width,
    int32_t photo_height,
    int32_t origin_x,
    int32_t origin_y,
    int32_t tile_width,
    int32_t tile_height,
    int32_t supersample,
    int32_t viewport_limit,
    float min_corner,
    float max_corner
) {
    const auto fits = [&](float x, float y, float width, float height) {
        if (!(width > 0.0F) || !(height > 0.0F))
            return false;
        if (viewport_limit > 0 && (width > static_cast<float>(viewport_limit) ||
                                   height > static_cast<float>(viewport_limit)))
            return false;
        const float right = x + width;
        const float bottom = y + height;
        return x >= min_corner && y >= min_corner && right <= max_corner && bottom <= max_corner;
    };
    const int32_t samples = supersample > 0 ? supersample : 1;
    const float scale = static_cast<float>(samples);
    TurnedViewport whole;
    whole.x = -static_cast<float>(origin_x) * scale;
    whole.y = -static_cast<float>(origin_y) * scale;
    whole.width = static_cast<float>(photo_width) * scale;
    whole.height = static_cast<float>(photo_height) * scale;
    if (viewport_limit <= 0 || fits(whole.x, whole.y, whole.width, whole.height)) {
        whole.fits = true;
        return whole;
    }
    TurnedViewport tile;
    tile.shifted = true;
    tile.width = static_cast<float>(tile_width) * scale;
    tile.height = static_cast<float>(tile_height) * scale;
    tile.fits = fits(tile.x, tile.y, tile.width, tile.height);
    return tile;
}

} // namespace oa::present::photo
