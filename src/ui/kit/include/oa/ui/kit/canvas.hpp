// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// The canvas the kit's components draw on, and the few primitives every one
// of them draws through: a fill, a blend, an outline, a raised edge, a
// rounded fill and outline (a capsule, or a rectangle with rounded
// corners), a hairline rule, a line of text in a box and its
// width, a picture, a one-bit mark, a coverage map at the canvas's own
// pixels, and a clip. Places and
// sizes are in points. A canvas has two forms.
// The crisp form draws with the artless painter at a whole scale in the
// game's own fonts. The smooth form draws with the painter of
// oa/ui/paint/painter.hpp at any scale, a fractional one among them, in the
// modern fonts: every edge of a rectangle falls on a whole pixel, each
// rounded on its own, and text is drawn at its own size in pixels.
#pragma once

#include "oa/platform/text_font.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/paint/painter.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace oa::ui::kit {

/// The smooth form of a canvas: the painter's canvas at a scale in percent,
/// such as 150, its text in the bundled modern fonts. A point at (x, y) is
/// the pixel edge lround((origin + x) * scale), and the same down.
struct SmoothCanvas {
    /// Paints the canvas's pixels; null for a canvas of the crisp form.
    oa::ui::paint::Painter* painter{};
    /// The bundled modern fonts the text is drawn in; null draws no text.
    oa::platform::text_font::FontStack* fonts{};
    int32_t scale_percent{100}; ///< canvas pixels a point, in percent; 150 is one and a half
    /// The point of the painter's canvas this canvas's point (0, 0) lies at
    /// (placed_at).
    Point origin{};
    /// The canvas pixels drawing may touch: none clips to the painter's
    /// canvas alone, and an empty box draws nothing.
    std::optional<oa::ui::paint::Box> clip{};
};

/// Where a control is drawn: the crisp form, an artless surface at a whole
/// scale in the game's fonts, or, while its smooth form has a painter, the
/// smooth form. Either way the game's fonts lay the screens out.
struct Canvas {
    /// The crisp form's picture; null draws nothing there.
    oa::ui::frontend_renderer::Surface* surface{};
    oa::ui::frontend_renderer::Placement placement{}; ///< where points land, and its clip
    const Fonts* fonts{};                             ///< the game's fonts; null draws no text
    oa::ui::frontend_renderer::RgbaPicture
        icon{}; ///< the picture a header or an OA button draws; empty draws the OA mark
    SmoothCanvas
        smooth{}; ///< the smooth form; the crisp form's surface is not read while it paints
};

/// The size the smooth form draws a font's text at, in points: its game
/// font's height, 12 for the regular font and 10 for the small one. At a
/// scale the text is lround(points * scale) pixels per em.
///
/// @param role which font
/// @return the size, in points
[[nodiscard]] constexpr int32_t smooth_text_points(FontRole role) noexcept {
    return role == FontRole::regular ? 12 : 10;
}

/// Tells whether the smooth form draws a font's text bold: the regular
/// font, the game's heavy font of the title, the labels and the values, is;
/// the small one is not.
///
/// @param role which font
/// @return true for bold
[[nodiscard]] constexpr bool smooth_text_bold(FontRole role) noexcept {
    return role == FontRole::regular;
}

/// Tells whether a canvas has anything to draw on.
///
/// @param canvas the canvas
/// @return true when it has a surface, or its smooth form a painter
[[nodiscard]] bool can_draw(const Canvas& canvas) noexcept;

/// Returns a canvas that draws only inside a rectangle, and inside the
/// canvas's own clip when it has one. Where the two do not meet, nothing
/// draws. On the smooth form the clip is the rectangle's pixels.
///
/// @param canvas the canvas
/// @param rect the rectangle, in points
/// @return the canvas, clipped
[[nodiscard]] Canvas clipped(const Canvas& canvas, const Rect& rect);

/// Returns a canvas whose point (0, 0) is a point of another: what is drawn
/// on it lands that far right and down, and its clip stays where it was.
///
/// @param canvas the canvas
/// @param origin where the new canvas's origin lies, in the canvas's points
/// @return the canvas, moved
[[nodiscard]] Canvas placed_at(const Canvas& canvas, Point origin);

/// Fills a rectangle with a colour.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param ink the colour
void fill_rect(const Canvas& canvas, const Rect& rect, Colour ink);

/// Blends a colour over a rectangle.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param ink the colour
/// @param opacity its share of each pixel, in 256ths
void blend_rect(const Canvas& canvas, const Rect& rect, Colour ink, uint32_t opacity);

/// Draws a one-point outline round the inside of a rectangle. On the smooth
/// form it is max(1, lround(scale)) whole pixels thick.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points; its inside is left as it is
/// @param ink the outline's colour
void outline_rect(const Canvas& canvas, const Rect& rect, Colour ink);

/// Fills a rectangle whose ends are semicircles of the given radius, clamped
/// to half its shorter side: a pill when the radius is half the height. The
/// shape is worked out at the canvas's own pixels. At scale 1 the corner is
/// stepped; at any other scale, and on the smooth form, the edge is blended
/// by how much of each pixel the shape covers. A rectangle too large to
/// sample is filled square.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param radius the ends' radius, in points; 0 fills the rectangle square
/// @param ink the colour
void fill_round_rect(const Canvas& canvas, const Rect& rect, int32_t radius, Colour ink);

/// Draws a one-point outline of that rounded rectangle. On the smooth form
/// it is max(1, lround(scale)) whole pixels thick, as outline_rect is. The
/// inside is left as it is. A rectangle too large to sample is outlined
/// square.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param radius the ends' radius, in points; 0 outlines the rectangle square
/// @param ink the outline's colour
void outline_round_rect(const Canvas& canvas, const Rect& rect, int32_t radius, Colour ink);

/// Fills a rectangle whose four corners are quarter circles of the given
/// radius, clamped to half its shorter side; its straight edges stay whole.
/// It is worked out at the canvas's own pixels as fill_round_rect is: a
/// step at scale 1, a blended edge at every other scale and on the smooth
/// form. A rectangle too large to sample is filled square.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param radius the corners' radius, in points; 0 fills the rectangle square
/// @param ink the colour
void fill_corner_rect(const Canvas& canvas, const Rect& rect, int32_t radius, Colour ink);

/// Blends a colour over a rectangle with rounded corners, as
/// fill_corner_rect fills one: each pixel takes the colour by its share of
/// the shape times the opacity. A rectangle too large to sample is blended
/// square.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param radius the corners' radius, in points; 0 blends the rectangle square
/// @param ink the colour
/// @param opacity its share of each covered pixel, in 256ths
void blend_corner_rect(
    const Canvas& canvas, const Rect& rect, int32_t radius, Colour ink, uint32_t opacity
);

/// Draws a one-point outline of that rectangle with rounded corners, as
/// outline_round_rect draws a capsule's. The inside is left as it is.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param radius the corners' radius, in points; 0 outlines the rectangle square
/// @param ink the outline's colour
void outline_corner_rect(const Canvas& canvas, const Rect& rect, int32_t radius, Colour ink);

/// Draws a ring of a whole number of points round the inside of a
/// rectangle with rounded corners: the focus ring and a field's soft ring.
/// It is that rectangle's shape less the same shape thickness points in,
/// worked out at the canvas's own pixels as fill_corner_rect is. On the
/// smooth form it is max(1, lround(thickness * scale)) whole pixels thick.
///
/// @param canvas where it is drawn
/// @param rect the ring's outside, in points; the inside is left as it is
/// @param radius the outside corners' radius, in points; 0 draws a square ring
/// @param thickness the ring's thickness, in points
/// @param ink the ring's colour
void ring_corner_rect(
    const Canvas& canvas, const Rect& rect, int32_t radius, int32_t thickness, Colour ink
);

/// Draws a one-point raised edge round the inside of a rectangle: light along
/// the top and left, dark along the bottom and right, the dark edges running
/// the whole width and height. On the smooth form each edge is
/// max(1, lround(scale)) whole pixels thick.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points; its inside is left as it is
/// @param light the top and left edges' colour
/// @param dark the bottom and right edges' colour
void bevel_rect(const Canvas& canvas, const Rect& rect, Colour light, Colour dark);

/// Draws a hairline rule: a rectangle one point high across, or one point
/// wide down. On the smooth form its thickness is max(1, lround(points *
/// scale)) whole pixels from its first edge, so every rule is as thick as
/// every other; its length's edges are rounded as a fill's are.
///
/// @param canvas where it is drawn
/// @param rect the rule, in points
/// @param ink the colour
void rule_rect(const Canvas& canvas, const Rect& rect, Colour ink);

/// Draws a line of text in a box, its capitals centred on the box's height.
/// Nothing is drawn without fonts.
///
/// The smooth form draws it in the modern fonts, antialiased, at
/// lround(smooth_text_points * scale) pixels per em, bold as
/// smooth_text_bold says, its tracking lround(tracking * scale) pixels.
/// Text wider than the box at that size keeps its size and is recorded.
///
/// @param canvas where it is drawn
/// @param role which font
/// @param text the text, in UTF-8, as it is shown
/// @param box the box, in points
/// @param align where the text sits along the box's width
/// @param ink the text's colour
/// @param tracking extra points after each character but the last
void box_text(
    const Canvas& canvas,
    FontRole role,
    std::string_view text,
    const Rect& box,
    Align align,
    Colour ink,
    int32_t tracking = 0
);

/// Returns a line of text's width as the canvas draws it.
///
/// @param canvas the canvas
/// @param role which font
/// @param text the text, in UTF-8
/// @return the width, in points, on the smooth form its pixels at its
///     size over the scale, rounded up; 0 without fonts
[[nodiscard]] int32_t measure_text(const Canvas& canvas, FontRole role, std::string_view text);

/// Draws a picture scaled to fill a rectangle, such as the Open Annihilation
/// icon or a mod's badge. The smooth form draws it filtered
/// (oa::ui::paint::Painter::draw_rgba).
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param picture the picture; an empty one draws nothing
void draw_picture(
    const Canvas& canvas, const Rect& rect, const oa::ui::frontend_renderer::RgbaPicture& picture
);

/// Draws a one-bit mark in one colour: only its set pixels, each a point, on
/// the smooth form a fill of that point's pixels.
///
/// @param canvas where it is drawn
/// @param bits the mark
/// @param at the mark's top left, in points
/// @param ink the colour
void draw_mark(
    const Canvas& canvas, const oa::ui::frontend_renderer::Mark& bits, Point at, Colour ink
);

/// Returns how many of a canvas's own pixels a rectangle covers across and
/// down: on the crisp form its points times the whole scale, on the smooth
/// form its edges rounded as a fill's are. A shape worked out at that many
/// pixels and drawn with blend_coverage is as crisp at every scale as at 1x.
///
/// @param canvas the canvas
/// @param rect the rectangle, in points
/// @return the columns and rows; 0 for an empty rectangle or a canvas that draws nothing
[[nodiscard]] Point covered_pixels(const Canvas& canvas, const Rect& rect) noexcept;

/// Blends a colour over a rectangle through a coverage map at the canvas's
/// own pixels, one byte a pixel of covered_pixels, rows top to bottom: 0
/// leaves the pixel as it is, 255 paints the colour and the others mix it
/// in by that share. Clipped as every primitive is.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param coverage covered_pixels' columns times its rows; a shorter map draws nothing
/// @param ink the colour
void blend_coverage(
    const Canvas& canvas, const Rect& rect, std::span<const uint8_t> coverage, Colour ink
);

/// Returns how much of each pixel of a rounded rectangle's top left corner
/// the shape covers, or its ring of a thickness round the inside, worked out
/// as fill_corner_rect and ring_corner_rect work out every corner: 16
/// samples a pixel, and at a stepped scale a pixel more than half covered is
/// covered and the rest are not. The other three corners are the same
/// mirrored. The OA layer rounds a window's corners with it.
///
/// @param radius_px the corner's radius, in pixels
/// @param hole_px the ring's thickness, in pixels; 0 for the filled shape
/// @param stepped true rounds each pixel to covered (255) or not (0)
/// @return radius_px times radius_px bytes, rows top to bottom, 0 to 255;
///     empty for a radius of 0 or less
[[nodiscard]] std::vector<uint8_t>
corner_coverage(int32_t radius_px, int32_t hole_px, bool stepped);

/// How a rectangle is drawn on after it has been laid out flat: turned about
/// its vertical axis, and covered by an opacity.
struct Perspective {
    /// Degrees about the vertical axis. Positive shortens the right edge and
    /// negative the left, both edges staying centred on the rectangle's
    /// height. 0 leaves the rectangle flat.
    float degrees{0.F};
    /// How much of each destination pixel the drawing covers, from 0 to 1.
    float opacity{1.F};
};

/// Claims the thread's offscreen buffer. A claim that finds the buffer
/// already claimed fails, so a drawing that calls perspective_blit cannot
/// nest another one.
///
/// @return true when this call holds the buffer
[[nodiscard]] bool claim_offscreen() noexcept;

/// Gives the thread's offscreen buffer back after claim_offscreen.
void release_offscreen() noexcept;

/// Prepares the reused offscreen buffer and returns a canvas that draws into
/// it, at the destination's scale, with its origin at the buffer's top left.
///
/// The buffer is kept for the thread and grown when a larger drawing asks,
/// and cleared each time. A later call replaces what an earlier one drew.
/// The canvas is empty when the destination cannot draw or the size is empty.
/// Call claim_offscreen first: this does not claim the buffer itself.
///
/// @param dest the canvas the drawing will be copied onto
/// @param width the drawing's width, in points
/// @param height the drawing's height, in points
/// @return a canvas over the buffer
[[nodiscard]] Canvas begin_offscreen(const Canvas& dest, int32_t width, int32_t height);

/// Copies the offscreen buffer onto a rectangle as a trapezoid.
///
/// One vertical edge keeps the rectangle's height and the other is shorter
/// by the cosine of the angle, so the drawing looks turned about its
/// vertical axis. On the crisp form each destination pixel copies the one
/// nearest source pixel. On the smooth form each destination pixel blends
/// the source pixels around its sample. The sample runs from the source's
/// first pixel at the trapezoid's first pixel toward one past the last
/// pixel at the far edge, so a sample between two pixels mixes them by the
/// distance. The copy is clipped to the destination and blended by the
/// opacity. Nothing is drawn when the buffer was not begun for this size.
///
/// @param dest where it is drawn
/// @param source_width the drawing's width, in points, as begun
/// @param source_height the drawing's height, in points, as begun
/// @param place the rectangle the trapezoid fills, in points
/// @param how the turn and the opacity
void warp_offscreen(
    const Canvas& dest,
    int32_t source_width,
    int32_t source_height,
    const Rect& place,
    Perspective how
);

/// Draws a laid-out rectangle into the reused offscreen buffer, then onto
/// the canvas as a trapezoid.
///
/// draw is called with a canvas whose origin is the drawing's top left and
/// whose size is drawn's width and height. It must cover that rectangle and
/// must not call perspective_blit or scale_blit: the buffer is the one
/// buffer, and a nested call draws nothing. The first call of a size takes
/// the buffer; a later call of the same or a smaller size does not allocate.
///
/// @param dest where the trapezoid is drawn
/// @param drawn the drawing's size, in points; its place is ignored
/// @param place the rectangle the trapezoid fills, in points
/// @param how the turn and the opacity
/// @param draw draws the rectangle into the offscreen canvas
template <class Draw>
void perspective_blit(
    const Canvas& dest, const Rect& drawn, const Rect& place, Perspective how, Draw&& draw
) {
    if (drawn.width <= 0 || drawn.height <= 0 || place.width <= 0 || place.height <= 0)
        return;
    if (how.opacity <= 0.F)
        return;
    if (!claim_offscreen())
        return;

    struct Release {
        ~Release() { release_offscreen(); }
    } release;

    static_cast<void>(release);
    Canvas offscreen = begin_offscreen(dest, drawn.width, drawn.height);
    if (!can_draw(offscreen))
        return;

    // The buffer's text is copied onto the screen as pixels. Its notes stay
    // out of the screen's ink, or a card's words would cross the title.
    struct Space {
        Space() { begin_text_fit_space(); }

        ~Space() { end_text_fit_space(); }
    } space;

    static_cast<void>(space);
    draw(offscreen);
    warp_offscreen(dest, drawn.width, drawn.height, place, how);
}

/// Draws a laid-out rectangle into the reused offscreen buffer, then scaled
/// into a rectangle. The crisp form copies the nearest source pixel. The
/// smooth form blends the source pixels around the sample, as
/// perspective_blit does.
///
/// The same buffer and the same rule against nesting as perspective_blit.
///
/// @param dest where it is drawn
/// @param drawn the drawing's size, in points; its place is ignored
/// @param place the rectangle it is scaled into, in points
/// @param draw draws the rectangle into the offscreen canvas
template <class Draw>
void scale_blit(const Canvas& dest, const Rect& drawn, const Rect& place, Draw&& draw) {
    perspective_blit(dest, drawn, place, Perspective{}, static_cast<Draw&&>(draw));
}

} // namespace oa::ui::kit
