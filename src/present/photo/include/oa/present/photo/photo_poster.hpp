// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which poster a frozen battlefield can offer, and the strips a processor
// poster is drawn in. Pure: no window, no drawing and no files. The runtime
// supplies the view, the marked rectangle, the map and the memory still free.
#pragma once

#include "oa/present/photo/photo_sizes.hpp"

#include <cstdint>
#include <vector>

namespace oa::present::photo {

/// Bytes a poster asks of the processor for each of its pixels, until its
/// file is written: the picture (3), the writer's copy of its rows (3), the
/// filtered rows while their buffer grows (up to 5) and the compressor's
/// output bound (3). A 16384 by 16384 poster measured 11.9 bytes a pixel at
/// its peak in Off and 13.6 in Full classic.
inline constexpr uint64_t kPosterBytesPerPixel = 14;

/// Which area a poster shows, at one map pixel a picture pixel.
enum class PosterExtent : uint8_t {
    /// The area the view shows.
    view = 0,
    /// A larger area the player marks. An empty mark is the view.
    marked = 1,
    /// The whole map, from its origin.
    map = 2,
};

/// A rectangle of the map, in map pixels. `x` and `z` are its top left.
struct PosterArea {
    int32_t x{0};      ///< map column of the left edge
    int32_t z{0};      ///< map row of the top edge
    int32_t width{0};  ///< map pixels across
    int32_t height{0}; ///< map pixels down
};

/// What the player is looking at, and whether a poster may be taken.
struct PosterAsk {
    /// Map pixels the view shows. At zoom 1 these are the battlefield's pixels.
    int32_t view_width{0};
    int32_t view_height{0};
    int32_t map_width{0};  ///< the whole map
    int32_t map_height{0}; ///< the whole map
    int32_t camera_x{0};   ///< the view's left map column
    int32_t camera_z{0};   ///< the view's top map row
    PosterArea marked{};   ///< the marked area; empty means the view
    bool frozen{false};    ///< the picture is held still
    bool held{false};      ///< photo mode is showing a copied picture
};

/// One extent, as the panel lists it.
struct PosterOffer {
    PosterExtent extent{PosterExtent::view}; ///< which area
    PosterArea area{};                       ///< the map rectangle, at 1:1
    bool offered{false};                     ///< the player can choose it
    SizeLock lock{SizeLock::open};           ///< why it is not offered
    /// The pixel cap the reason names when `lock` is `edge`.
    int32_t limit{0};
};

/// The three extents, and the lock on the Poster button itself.
struct PosterPlan {
    /// Why Poster is not offered, whatever the extent. `open` when at least
    /// the button may be pressed. An extent can still be locked on its own.
    SizeLock button{SizeLock::open};
    int32_t limit{0}; ///< the cap `button` names when it is `edge`
    PosterOffer view{};
    PosterOffer marked{};
    PosterOffer map{};
};

/// One strip of a processor poster. The draw is `height` plus `skip_rows`,
/// from a camera on the area's 16-pixel row phase. The kept rows start
/// `skip_rows` down the draw and land at `dest_y` in the poster.
struct PosterStrip {
    int32_t camera_x{0};  ///< the draw's camera
    int32_t camera_z{0};  ///< the draw's camera, on the area's row phase
    int32_t width{0};     ///< the kept piece, and the draw's width
    int32_t height{0};    ///< the kept piece
    int32_t dest_x{0};    ///< where the kept piece lands, from the poster's left
    int32_t dest_y{0};    ///< where the kept piece lands, from the poster's top
    int32_t skip_rows{0}; ///< rows above the kept piece, drawn so the phase holds
};

/// The poster offered for each extent. A shared match, the turned view, a
/// held picture and a picture that is still moving lock the button, and
/// every extent with it. Otherwise each extent is judged on its own: a side
/// past 16384 is the photo's edge, and a picture the memory cannot hold is
/// not offered. Full classic pays for the assembled poster and one tile when
/// the poster is drawn in tiles, and for the whole target when it fits in
/// one piece. Smooth edges stay off: the picture is one pixel of the map.
///
/// @param ask the view, the mark, the map and whether the picture is still
/// @param machine the mode, its limit and the memory still free
/// @return the three extents and the button's lock
[[nodiscard]] PosterPlan plan_poster(const PosterAsk& ask, const Machine& machine);

/// Splits a poster into strips a view can draw. A strip is at most the view
/// across and down. Down the map, every strip but the last starts on the
/// area's 16-pixel row phase and is a whole number of those rows, so a unit's
/// draw row stays the row a photo of that ground would use. The last strip
/// keeps the rows that remain. Across, strips step by the view's width.
///
/// @param area the poster, in map pixels
/// @param view_width the view, in map pixels; 0 draws the area in one strip across
/// @param view_height the view, in map pixels; 0 draws the area in one strip down
/// @return the strips, empty when the area has no pixels
[[nodiscard]] std::vector<PosterStrip>
poster_strips(const PosterArea& area, int32_t view_width, int32_t view_height);

} // namespace oa::present::photo
