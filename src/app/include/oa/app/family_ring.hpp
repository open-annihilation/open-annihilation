// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The kit's focus ring around a control that is not one of the kit's own
// screens: the original menus' gadgets and an extension's windows. The
// pixels are what kit::draw_focus_ring paints, coverage included, and
// nothing else.
#pragma once

#include <cstdint>
#include <vector>

namespace oa::app {

/// One pixel of the kit's focus ring, in the window's own pixels.
struct FamilyRingPixel {
    int32_t x{}; ///< its column
    int32_t y{}; ///< its row
    /// How much of the pixel the ring covers.
    ///
    /// At a whole scale (100, 200, …) this is the kit's blend share, from 0
    /// to 256, the same share `blend_rect` takes. At any other scale it is
    /// the opacity the smooth painter stores, from 0 to 255. `blend_family_coverage`
    /// lays the ring's colour over a pixel with that reading.
    uint16_t coverage{};
};

/// Lays one pixel of the ring over a ground pixel.
///
/// The ground is replaced by what `kit::draw_focus_ring` would leave there:
/// the crisp blend at a whole scale, the smooth painter's blend otherwise.
/// A coverage of 0 leaves the ground as it is.
///
/// @param[in,out] red the ground's red, then the mixed red
/// @param[in,out] green the ground's green, then the mixed green
/// @param[in,out] blue the ground's blue, then the mixed blue
/// @param coverage the pixel's coverage, as FamilyRingPixel keeps it
/// @param crisp true at a whole scale, false for the smooth painter
void blend_family_coverage(
    uint8_t& red, uint8_t& green, uint8_t& blue, uint16_t coverage, bool crisp
) noexcept;

/// Returns the pixels `kit::draw_focus_ring` paints around a control.
///
/// The rectangle is the control's place in the window's own pixels, the
/// rectangle the tree gives. The ring is the kit's, drawn around the
/// control at `scale_percent` (100, 150, 200 and the other scales the OA
/// layer draws at): one point of the ring's thickness and its inset for
/// every hundred percent. Each pixel the ring covers is kept, a rounded
/// corner's partial pixels included, with the coverage the kit blended.
/// An empty rectangle returns none.
///
/// @param window_x the control's left column, in the window's pixels
/// @param window_y the control's top row, in the window's pixels
/// @param window_width the control's width, in the window's pixels
/// @param window_height the control's height, in the window's pixels
/// @param scale_percent the OA layer's scale, in percent
/// @return the ring's pixels
[[nodiscard]] std::vector<FamilyRingPixel> family_ring_pixels(
    int32_t window_x,
    int32_t window_y,
    int32_t window_width,
    int32_t window_height,
    int32_t scale_percent
);

/// A control's rectangle, either in the window's pixels or on the canvas.
struct FamilyControlBox {
    /// True when `x`, `y`, `width` and `height` are already the window's
    /// pixels, as an extension window reports them. False when they are
    /// canvas pixels a 3.1c control reports.
    bool window_pixels{};
    int32_t x{};      ///< its left column
    int32_t y{};      ///< its top row
    int32_t width{};  ///< its width
    int32_t height{}; ///< its height
};

/// How canvas pixels land in the window.
///
/// An identity map copies the canvas point and then applies `density`. Any
/// other map is the picture's place: a canvas point `(column, row)` lands at
/// `origin + (column, row) * scale`, and that place is then multiplied by
/// `density`. The 640 by 480 picture in a 1280 by 720 window is the origin
/// (160, 0) and the scale 1.5, the 160-pixel letterbox on each side.
struct FamilyWindowMap {
    bool identity{true}; ///< canvas pixels are already window coordinates
    float origin_x{};    ///< where canvas column 0 lands, before density
    float origin_y{};    ///< where canvas row 0 lands, before density
    float scale_x{1.0F}; ///< window coordinates across for one canvas pixel
    float scale_y{1.0F}; ///< window coordinates down for one canvas pixel
    float density{1.0F}; ///< window pixels in one window coordinate
};

/// Returns the control's rectangle in the window's pixels.
///
/// A control that already states its window pixels keeps that rectangle.
/// A canvas rectangle is mapped through `map` and then through the density,
/// each edge rounded to the nearest pixel on its own, as the tree's window
/// rectangle is.
///
/// @param box the control's rectangle
/// @param map where the canvas lies in the window; ignored when the box is
///     already in window pixels
/// @param[out] left its left column, in the window's pixels
/// @param[out] top its top row, in the window's pixels
/// @param[out] width its width, in the window's pixels
/// @param[out] height its height, in the window's pixels
/// @return false when the rectangle is empty or the map has no scale
[[nodiscard]] bool control_window_rect(
    const FamilyControlBox& box,
    const FamilyWindowMap& map,
    int32_t& left,
    int32_t& top,
    int32_t& width,
    int32_t& height
) noexcept;

} // namespace oa::app
