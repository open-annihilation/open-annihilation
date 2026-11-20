// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Player markers and health-bar notches drawn while shape cues are in effect.
// The caller decides whether cues are in effect: these functions always draw,
// so a test can raster a marker without turning the preference on.
#pragma once

#include "oa/ui/hud/health_bar.hpp"

#include <array>
#include <cstdint>

namespace oa::ui::hud {

/// Columns of a player marker: the shape, a gap, and the player's number.
inline constexpr int32_t kPlayerMarkerWidth = 19;
/// Rows of a player marker.
inline constexpr int32_t kPlayerMarkerHeight = 13;
/// Column of the shape's centre inside the marker. The number sits to the right.
inline constexpr int32_t kPlayerMarkerCentreColumn = 6;
/// Row of the shape's centre inside the marker.
inline constexpr int32_t kPlayerMarkerCentreRow = 6;

/// One pixel of a player marker.
enum class MarkerPixel : uint8_t {
    clear,   ///< nothing is drawn
    shape,   ///< the player's colour
    outline, ///< the ink that shows on the shape's colour, around the shape
    numeral, ///< white, the player's number
    halo,    ///< black, around the number
};

/// What one pixel of a player's marker is.
///
/// Players 1 to 5 (indices 0 to 4) are a filled circle, square, triangle,
/// diamond and star. Players 6 to 10 (indices 5 to 9) are the same shapes
/// hollow. The number, 1 to 10, sits to the right of the shape, so the
/// shape's centre stays the player's colour when the shape is filled and
/// stays clear when it is hollow. A player outside 0 to 9, or a pixel
/// outside the marker, is clear.
///
/// @param player the player's seat, 0 to 9
/// @param x column inside the marker, 0 at the left
/// @param y row inside the marker, 0 at the top
/// @return the pixel's kind
[[nodiscard]] MarkerPixel player_marker_pixel(int player, int32_t x, int32_t y) noexcept;

/// Columns and rows of a compact marker: the shape alone, with no number.
inline constexpr int32_t kCompactMarkerSide = 7;
/// Column and row of the compact shape's centre.
inline constexpr int32_t kCompactMarkerCentre = 3;

/// Draws a player's marker into an RGB picture, centred on the shape.
///
/// The number is to the right of the shape. The shape's outline is the ink
/// that shows on the shape's colour, and the number's halo is black. Each
/// marker pixel is `scale` picture pixels on a side, nearest neighbour, and
/// the centre pixel's block covers the requested point. A plate is a black
/// border one marker pixel larger than the marker, drawn first. Pixels
/// outside the picture are left alone. A null picture, a non-positive size,
/// a scale below 1, or a player outside 0 to 9 draws nothing.
///
/// @param rgb the picture, 3 bytes a pixel, rows `width` apart; null draws nothing
/// @param width the picture's width in pixels
/// @param height the picture's height in pixels
/// @param centre_x column of the shape's centre
/// @param centre_y row of the shape's centre
/// @param player the player's seat, 0 to 9
/// @param red the shape's red
/// @param green the shape's green
/// @param blue the shape's blue
/// @param scale picture pixels on a side of one marker pixel; below 1 draws nothing
/// @param plate true to draw the dark plate under the marker
void stamp_player_marker(
    uint8_t* rgb,
    int32_t width,
    int32_t height,
    int32_t centre_x,
    int32_t centre_y,
    int player,
    uint8_t red,
    uint8_t green,
    uint8_t blue,
    int32_t scale = 1,
    bool plate = false
) noexcept;

/// What one pixel of a compact marker is.
///
/// The five shapes, filled and hollow, at most 7 by 7, with no number. A
/// player outside 0 to 9, or a pixel outside the marker, is clear.
///
/// @param player the player's seat, 0 to 9
/// @param x column inside the marker, 0 at the left
/// @param y row inside the marker, 0 at the top
/// @return the pixel's kind
[[nodiscard]] MarkerPixel compact_marker_pixel(int player, int32_t x, int32_t y) noexcept;

/// Where a compact marker is drawn once it has moved inside the picture.
struct CompactMarkerPlace {
    int32_t left{}; ///< the marker's left column
    int32_t top{};  ///< the marker's top row
};

/// Returns the top-left of a compact marker after it moves inside the picture.
///
/// A picture at least 7 pixels on a side keeps the whole marker inside it.
/// A smaller picture leaves the centre where it was asked. The marker's
/// centre is left + 3, top + 3.
///
/// @param width the picture's width in pixels
/// @param height the picture's height in pixels
/// @param centre_x column of the shape's centre before it moves
/// @param centre_y row of the shape's centre before it moves
/// @return the marker's top-left
[[nodiscard]] CompactMarkerPlace
compact_marker_place(int32_t width, int32_t height, int32_t centre_x, int32_t centre_y) noexcept;

/// Draws a compact marker into an RGB picture, centred on the shape.
///
/// The shape alone: no number. The whole marker is moved in at the picture's
/// edges so it is not cut, when the picture is at least 7 pixels on that
/// side. The outline is the ink that shows on the shape's colour. A null
/// picture, a non-positive size, or a player outside 0 to 9 draws nothing.
///
/// @param rgb the picture, 3 bytes a pixel, rows `width` apart; null draws nothing
/// @param width the picture's width in pixels
/// @param height the picture's height in pixels
/// @param centre_x column of the shape's centre
/// @param centre_y row of the shape's centre
/// @param player the player's seat, 0 to 9
/// @param red the shape's red
/// @param green the shape's green
/// @param blue the shape's blue
void stamp_compact_marker(
    uint8_t* rgb,
    int32_t width,
    int32_t height,
    int32_t centre_x,
    int32_t centre_y,
    int player,
    uint8_t red,
    uint8_t green,
    uint8_t blue
) noexcept;

/// Colour of a number drawn on a background.
///
/// A light background is black and a dark one is white. Light is a weighted
/// sum, red 299, green 587 and blue 114, above 140000.
///
/// @param red the background's red
/// @param green the background's green
/// @param blue the background's blue
/// @return black or white, three channels
[[nodiscard]] std::array<uint8_t, 3> numeral_ink(uint8_t red, uint8_t green, uint8_t blue) noexcept;

/// One notch in a bar: a one-pixel column, or empty when width is 0.
struct BarNotch {
    int32_t x{};      ///< left column
    int32_t y{};      ///< top row
    int32_t width{};  ///< columns; 0 draws nothing
    int32_t height{}; ///< rows
};

/// The one-third and two-thirds notches of a health or build bar.
struct BarNotches {
    BarNotch one_third{};  ///< a third of the way across the trough's inside
    BarNotch two_thirds{}; ///< two thirds of the way across the trough's inside
};

/// Places notches at one third and two thirds of a bar's trough.
///
/// The trough's inside is the rectangle one pixel in from its edges. Each
/// notch is one column of that inside, as tall as the inside. A trough whose
/// inside is under three pixels across, or no pixel tall, has empty notches.
/// The bar itself is not changed.
///
/// @param bar the bar's trough and fill
/// @return the two notches; empty when the inside is too small
[[nodiscard]] BarNotches health_bar_notches(const HealthBar& bar) noexcept;

} // namespace oa::ui::hud
