// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How a Full classic photo is split into tiles when the card cannot draw it
// in one piece. Pure: no window, no drawing and no files. The runtime draws
// each tile the way it draws a whole Full photo, crops the border and places
// the kept piece. Grain and the anti-aliasing factor are arguments, so this
// module does not include the Full tier.
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

} // namespace oa::present::photo
