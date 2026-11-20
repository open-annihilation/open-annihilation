// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How far out the battlefield's view zooms, how far past the map's edges it
// moves, and how the battlefield is drawn farther out than its units are
// drawn whole: the far view. The view's zoom floor for each Maximum zoom out
// choice and the zoom at which the whole map fits the battlefield; the
// places the view's centre may take; the terrain reduced from a pyramid of each tile's
// means, so that a frame of the whole map costs about what a frame at the
// processor's floor costs; and the dots the far view draws for units. Pure:
// no SDL and no Runtime.
#pragma once

#include "oa/formats/hpi.hpp"
#include "oa/formats/tnt.hpp"
#include "oa/platform/job_pool.hpp"
#include "oa/ui/engine_settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace oa::app {

/// The farthest any view zooms out, whatever the map and the window, in
/// screen pixels per map pixel: a sixty-fourth of normal size, past where
/// the largest maps fit the smallest battlefield.
inline constexpr float furthest_battlefield_zoom = 1.0F / 64.0F;

/// Returns the zoom at which the whole shown map fits the battlefield: the
/// lesser of the battlefield's width over the map's and its height over the
/// map's, so that the map fills the battlefield in one direction and fits
/// within it in the other.
///
/// @param map_width map pixels across the shown map
/// @param map_height map pixels down it
/// @param battlefield_width screen pixels across the battlefield
/// @param battlefield_height screen pixels down it
/// @return screen pixels per map pixel; 0 for an empty map or battlefield
[[nodiscard]] float whole_map_zoom(
    int32_t map_width, int32_t map_height, int32_t battlefield_width, int32_t battlefield_height
) noexcept;

/// Returns the zoom floor of the battlefield's view for a Maximum zoom out
/// choice.
///
/// Automatic keeps the floor the drawing sets, `automatic_floor`, as the
/// view always has. Every other choice stops at its share of normal size
/// (oa::ui::engine_settings::zoom_out_share), Whole map at none, or where
/// the whole map fits the battlefield (whole_map_zoom) if that comes first,
/// and never past furthest_battlefield_zoom; none of them forces a zoom in,
/// so the floor is at most normal size. Without a map or a battlefield every
/// choice keeps Automatic's floor.
///
/// @param limit the Maximum zoom out choice
/// @param automatic_floor Automatic's floor: kMinFullBattlefieldZoom while
///        the graphics card draws the battlefield, else kMinBattlefieldZoom
/// @param map_width map pixels across the shown map
/// @param map_height map pixels down it
/// @param battlefield_width screen pixels across the battlefield
/// @param battlefield_height screen pixels down it
/// @return the least zoom the view may take, screen pixels per map pixel
[[nodiscard]] float least_battlefield_zoom(
    oa::ui::engine_settings::ZoomOutLimit limit,
    float automatic_floor,
    int32_t map_width,
    int32_t map_height,
    int32_t battlefield_width,
    int32_t battlefield_height
) noexcept;

// ---------------------------------------------------------------------------
// How far past the map's edges the view goes

/// The places the centre of the battlefield's view may take along one
/// axis, in map pixels.
struct ViewCentreSpan {
    double least{}; ///< the farthest toward the map's start
    double most{};  ///< the farthest toward its end
};

/// Returns how far the centre of the battlefield's view may go along one
/// axis.
///
/// Where the view shows no more of the axis than the map holds, at most
/// `share` of the view lies past either of the map's edges: at a half the
/// view's centre stays on the map, so that the map's edges and corners can
/// be brought to the middle of the battlefield, and at none the view stays
/// on the map. Where the view shows more than the map holds, the map's
/// centre stays within `share` of the view of the view's centre: at a half
/// it stays in the view, and at none the map is centred. The two meet where
/// the view is the map's size, so the span grows smoothly with the zoom.
///
/// @param map map pixels the shown map holds along the axis
/// @param visible map pixels the view shows along the axis
/// @param share the share of the view that may lie past the map's edge,
///        0 to 0.5 (oa::ui::engine_settings::past_map_edge_share)
/// @return the span, least at most most
[[nodiscard]] ViewCentreSpan view_centre_span(double map, double visible, double share) noexcept;

/// Returns where a view held within its limits lies along one axis.
///
/// The view's centre is held within view_centre_span, widened to take in
/// `from`: the centre of the view the limits held last, which may lie
/// past them, as a zoom about a point of the map leaves it. A view held so
/// never moves back toward the map; it only stops going further from it.
///
/// @param view the map pixel at the view's start along the axis, exact
/// @param visible map pixels the view shows along the axis
/// @param map map pixels the shown map holds along the axis
/// @param share the share of the view that may lie past the map's edge
///        (view_centre_span)
/// @param from the centre of the view held last, in map pixels; none holds
///        the view within the span alone
/// @return the view's start, held
[[nodiscard]] double held_view(
    double view, double visible, double map, double share, std::optional<double> from
) noexcept;

/// Returns the whole map pixel a camera held within the view's limits takes
/// along one axis (held_view): the camera itself when the limits leave it
/// or hold it by less than a map pixel, as the camera taken from an exact
/// place within them is, else the nearest whole map pixel within them.
///
/// @param camera the camera's map pixel along the axis
/// @param visible map pixels the view shows along the axis
/// @param map map pixels the shown map holds along the axis
/// @param share the share of the view that may lie past the map's edge
///        (view_centre_span)
/// @param from the centre of the view held last, in map pixels, or none
/// @return the camera's map pixel, held
[[nodiscard]] int32_t held_camera(
    int32_t camera, double visible, double map, double share, std::optional<double> from
) noexcept;

/// Tells whether a frame at a zoom draws the far view: farther out than the
/// zoom its units are drawn whole at, which is Automatic's floor for the
/// tier drawing it.
///
/// @param zoom the frame's zoom, screen pixels per map pixel
/// @param detail_floor the least zoom the tier draws units whole at
/// @return true below `detail_floor`
[[nodiscard]] constexpr bool far_view_zoom(float zoom, float detail_floor) noexcept {
    return zoom > 0.0F && zoom < detail_floor;
}

// ---------------------------------------------------------------------------
// The far view's terrain

/// The pyramid's levels: level L holds a texel for each square of 2 to the
/// L map pixels a side, 1 to 5, so that the last holds one texel a tile.
inline constexpr uint8_t first_pyramid_level = 1;
inline constexpr uint8_t last_pyramid_level = 5;

/// Texels of one tile across all its levels: 16x16, 8x8, 4x4, 2x2 and 1x1.
inline constexpr std::size_t pyramid_tile_texels = 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1;

/// Bytes of one pyramid texel: red, green and blue.
inline constexpr std::size_t pyramid_texel_bytes = 3;

/// Each tile of a map reduced, level by level: every texel the mean of the
/// tile's map pixels it covers in the palette's colours, each channel
/// rounded to the nearest, half up.
struct TerrainPyramid {
    uint32_t tile_count{}; ///< tiles of the map the pyramid was built from
    /// Tile by tile, its levels 1 to 5 in turn, each in rows of its edge.
    std::vector<uint8_t> rgb;
};

/// Builds a map's terrain pyramid in a palette.
///
/// @param map the map; its tile table must hold tile_count tiles
/// @param palette the palette the tiles are shown in, four bytes an entry
/// @return the pyramid; empty when the map's tile table is short
[[nodiscard]] TerrainPyramid
build_terrain_pyramid(const oa::formats::tnt::Map& map, const oa::PaletteBytes& palette);

/// Returns the pyramid level the far view's terrain reads at a zoom: the
/// level whose texels are as large as a screen pixel's share of the map or
/// half of it, so that each screen pixel averages one or two texels each
/// way; within first_pyramid_level and last_pyramid_level.
///
/// @param zoom screen pixels per map pixel, below 1
/// @return the level
[[nodiscard]] uint8_t far_terrain_level(float zoom) noexcept;

/// Averages the terrain under each destination pixel from the pyramid's
/// level for the zoom (far_terrain_level).
///
/// Each destination pixel covers the map pixels the nearest fill's steps
/// give it, at 16.16 screen pixels per map pixel, as the exact box filter's
/// pixels do; it takes the mean of the level's texels whose centres lie in
/// that span, rounded to the nearest. Ground past the shown map, and before
/// it, counts as black, as the nearest fill paints it. The rows are
/// averaged in bands on the pool's threads, or in order without one.
///
/// @param map the map
/// @param pyramid the map's pyramid (build_terrain_pyramid)
/// @param source_x map column of the first destination pixel; below 0 left of the map
/// @param source_y map row of the first destination pixel; below 0 above the map
/// @param shown_width map pixels across the view may show
/// @param shown_height map pixels down the view may show
/// @param dest_width destination pixels across
/// @param dest_height destination pixels down
/// @param zoom destination pixels per map pixel, below 1
/// @param[out] dest_rgb 3 bytes per destination pixel, rows dest_width apart
/// @param pool the drawing threads; null draws on the calling thread
/// @param phase_x how far into map column source_x the first destination
///     column starts, 16.16 parts of the zoom's step, below it
///     (oa/present/scene_grid.hpp's phase); the spans then step from there
/// @param phase_y the same for map row source_y and the first destination row
/// @return false when the map or the pyramid is malformed; the destination
///         is then left partly drawn
[[nodiscard]] bool filter_far_terrain(
    const oa::formats::tnt::Map& map,
    const TerrainPyramid& pyramid,
    int32_t source_x,
    int32_t source_y,
    uint32_t shown_width,
    uint32_t shown_height,
    int32_t dest_width,
    int32_t dest_height,
    float zoom,
    uint8_t* dest_rgb,
    oa::platform::job_pool::Pool* pool,
    uint32_t phase_x = 0,
    uint32_t phase_y = 0
);

// ---------------------------------------------------------------------------
// The far view's units

/// Pixels across and down a unit's dot in the far view, the size of the
/// megamap's square for a unit with no picture.
inline constexpr int32_t far_view_dot_side = 5;

/// Tells whether a screen pixel lies on a unit's dot in the far view: in
/// the square far_view_dot_side pixels a side centred on where the unit
/// shows, as draw_far_view_dots draws it, its frame left out.
///
/// @param dot_x the unit's screen column
/// @param dot_y its screen row
/// @param x the pixel's column
/// @param y its row
/// @return true when the dot covers the pixel
[[nodiscard]] constexpr bool
far_view_dot_covers(int32_t dot_x, int32_t dot_y, int32_t x, int32_t y) noexcept {
    const int32_t left = dot_x - far_view_dot_side / 2;
    const int32_t top = dot_y - far_view_dot_side / 2;
    return x >= left && x < left + far_view_dot_side && y >= top && y < top + far_view_dot_side;
}

/// A unit's dot in the far view: a square of its owner's colour centred on
/// where the unit shows, framed one pixel wide when it is selected.
/// A seat in `player` leaves the square out, so the caller can draw that
/// player's marker in its place before this draw. A framed seat still draws
/// the frame, nine pixels a side, one pixel outside the 7-pixel marker, at
/// the centre the caller gives. A unit outside the picture is not passed as
/// a framed seat.
struct FarViewDot {
    int32_t x{}; ///< the unit's screen column
    int32_t y{}; ///< its screen row
    std::array<uint8_t, 3> colour{};
    bool framed{};
    std::array<uint8_t, 3> frame_colour{};
    /// The owner's seat, 0 to 9. Below 0 keeps the square.
    int player{-1};
};

/// Draws the far view's dots, in order, clipped to the picture.
///
/// A dot whose player is a seat leaves the square out. Its frame, when the
/// dot is selected, is nine pixels, one pixel outside the marker the caller
/// stamped, centred on the dot as given. Any other dot is the square,
/// framed when it is selected.
///
/// @param[in,out] rgb the picture, 3 bytes a pixel, rows `width` apart
/// @param width the picture's width
/// @param height its height
/// @param dots the dots
void draw_far_view_dots(
    uint8_t* rgb, int32_t width, int32_t height, std::span<const FarViewDot> dots
) noexcept;

} // namespace oa::app
