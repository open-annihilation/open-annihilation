// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The canvas's primitives in both forms: the crisp form through the artless
// painter at a whole scale in the game's fonts, the smooth form through the
// painter at any scale in the modern fonts, every edge on a whole pixel; and
// a coverage map blended at either form's own pixels.

#include "oa/ui/kit/canvas.hpp"

#include "oa/platform/text_font.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/paint/painter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace oa::ui::kit {

namespace renderer = oa::ui::frontend_renderer;
namespace paint = oa::ui::paint;
namespace text_font = oa::platform::text_font;

namespace {

/// Tells whether a canvas draws in its smooth form.
///
/// @param canvas the canvas
/// @return true while its smooth form has a painter
bool is_smooth(const Canvas& canvas) noexcept {
    return canvas.smooth.painter != nullptr;
}

/// Returns a rectangle of a smooth canvas's points as the painter's pixels:
/// each edge lround((origin + edge) * scale) on its own (to_canvas).
///
/// @param canvas the canvas, smooth
/// @param rect the rectangle, in points
/// @return the pixels
paint::Box pixels_of(const Canvas& canvas, const Rect& rect) noexcept {
    const SmoothCanvas& smooth = canvas.smooth;
    const Rect drawn = to_canvas(
        {smooth.origin.x + rect.x, smooth.origin.y + rect.y, rect.width, rect.height},
        smooth.scale_percent
    );
    return {drawn.x, drawn.y, drawn.width, drawn.height};
}

/// Returns a length in points as whole pixels on a smooth canvas, never
/// below one: max(1, lround(points * scale)).
///
/// @param canvas the canvas, smooth
/// @param points the length, in points
/// @return the pixels
int32_t pixel_length(const Canvas& canvas, int32_t points) noexcept {
    return std::max(int32_t{1}, to_canvas({0, 0, points, 0}, canvas.smooth.scale_percent).width);
}

/// Returns a colour as the painter takes it.
///
/// @param ink the colour
/// @return the painter's colour, its opacity kept
paint::Rgba rgba_of(Colour ink) noexcept {
    return {ink.r, ink.g, ink.b, ink.a};
}

/// Readies a smooth canvas's painter for one primitive: its clip set to the
/// canvas's own.
///
/// @param canvas the canvas, smooth
/// @return the painter; null when the canvas's clip holds no pixel
paint::Painter* painter_of(const Canvas& canvas) noexcept {
    paint::Painter* painter = canvas.smooth.painter;
    if (!canvas.smooth.clip) {
        painter->reset_clip();
        return painter;
    }
    if (canvas.smooth.clip->empty())
        return nullptr;
    painter->set_clip(*canvas.smooth.clip);
    return painter;
}

/// Paints a raised edge of whole pixels round the inside of a box: the light
/// edges along the top and left stop short of the dark ones, which run the
/// box's whole width and height.
///
/// @param painter the painter
/// @param box the box
/// @param thickness each edge's pixels
/// @param light the top and left edges' colour
/// @param dark the bottom and right edges' colour
void paint_bevel(
    paint::Painter& painter, paint::Box box, int32_t thickness, Colour light, Colour dark
) noexcept {
    if (box.empty())
        return;
    const int32_t edge = std::min({thickness, box.width, box.height});
    painter.fill_rect({box.x, box.y, box.width - edge, edge}, rgba_of(light));
    painter.fill_rect({box.x, box.y, edge, box.height - edge}, rgba_of(light));
    painter.fill_rect({box.x, box.y + box.height - edge, box.width, edge}, rgba_of(dark));
    painter.fill_rect({box.x + box.width - edge, box.y, edge, box.height}, rgba_of(dark));
}

/// The pixel size of a font's text on a smooth canvas, and its weight.
struct SmoothFace {
    int32_t pixel_size{}; ///< pixels per em
    bool bold{};          ///< the bold face goes first
};

/// Returns the size and weight a font's text takes on a smooth canvas.
///
/// @param canvas the canvas, smooth
/// @param role which font
/// @return lround(smooth_text_points * scale), at least 1, and smooth_text_bold
SmoothFace smooth_face(const Canvas& canvas, FontRole role) noexcept {
    return {pixel_length(canvas, smooth_text_points(role)), smooth_text_bold(role)};
}

/// Returns a line's width in pixels with extra pixels after each character
/// but the last.
///
/// @param fonts the modern fonts
/// @param text the text, in UTF-8
/// @param face its size and weight
/// @param tracking the extra pixels
/// @return the width, in pixels
int32_t tracked_line_width(
    text_font::FontStack& fonts, std::string_view text, SmoothFace face, int32_t tracking
) {
    if (tracking == 0)
        return paint::text_width(fonts, text, face.pixel_size, face.bold);
    int32_t width = 0;
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t bytes = character_bytes(text.substr(at));
        if (bytes == 0)
            break;
        if (at > 0)
            width += tracking;
        width += paint::text_width(fonts, text.substr(at, bytes), face.pixel_size, face.bold);
        at += bytes;
    }
    return width;
}

/// Returns the rows a face's capitals stand above its baseline: those of its
/// capital H at least half covered.
///
/// @param fonts the modern fonts
/// @param face the size and weight
/// @return the rows; 0 when the fonts cannot draw it
int32_t capital_rows(text_font::FontStack& fonts, SmoothFace face) {
    const paint::TextLine line = paint::draw_line(fonts, "H", face.pixel_size, face.bold);
    const text_font::Coverage& drawn = line.coverage;
    if (!line.drawn || drawn.width <= 0)
        return 0;
    constexpr uint8_t half_covered = 128;
    for (int32_t row = 0; row < drawn.height && row < drawn.baseline; ++row)
        for (int32_t column = 0; column < drawn.width; ++column)
            if (drawn.alpha[static_cast<std::size_t>(row * drawn.width + column)] >= half_covered)
                return drawn.baseline - row;
    return 0;
}

/// Draws a line of text on a smooth canvas in a box, as box_text says.
///
/// @param canvas the canvas, smooth, with fonts
/// @param role which font
/// @param text the text, in UTF-8
/// @param box the box, in points
/// @param align where the text sits along the box
/// @param ink the colour
/// @param tracking extra points after each character but the last
void smooth_text(
    const Canvas& canvas,
    FontRole role,
    std::string_view text,
    const Rect& box,
    Align align,
    Colour ink,
    int32_t tracking
) {
    text_font::FontStack& fonts = *canvas.smooth.fonts;
    const paint::Box pixels = pixels_of(canvas, box);
    SmoothFace face = smooth_face(canvas, role);
    const int32_t spacing = tracking == 0 ? 0 : pixel_length(canvas, tracking);
    const int32_t width = tracked_line_width(fonts, text, face, spacing);
    int32_t pen = pixels.x;
    if (align == Align::centre)
        pen = pixels.x + (pixels.width - width) / 2;
    else if (align == Align::right)
        pen = pixels.x + pixels.width - width;
    const int32_t capitals_tall = capital_rows(fonts, face);
    const int32_t baseline = pixels.y + (pixels.height - capitals_tall) / 2 + capitals_tall;
    // Capitals only. The line box is taller than the glyphs, and two boxes
    // that share that padding are not two texts crossing.
    int32_t ink_y = pixels.y;
    int32_t ink_height = pixels.height;
    if (capitals_tall > 0) {
        ink_y = baseline - capitals_tall;
        ink_height = capitals_tall;
    }
    // The face stays at its size. A line wider than its box is recorded, not
    // shrunk. Ink the clip hides crosses nothing.
    if (canvas.smooth.clip && canvas.smooth.clip->empty())
        note_text_fit(text, width, pixels.x, ink_y, pixels.width, ink_height, pen, 0, 0, -1, 0);
    else if (canvas.smooth.clip)
        note_text_fit(
            text,
            width,
            pixels.x,
            ink_y,
            pixels.width,
            ink_height,
            pen,
            static_cast<int32_t>(canvas.smooth.clip->x),
            static_cast<int32_t>(canvas.smooth.clip->y),
            static_cast<int32_t>(canvas.smooth.clip->width),
            static_cast<int32_t>(canvas.smooth.clip->height)
        );
    else
        note_text_fit(text, width, pixels.x, ink_y, pixels.width, ink_height, pen);
    paint::Painter* painter = painter_of(canvas);
    if (painter == nullptr)
        return;
    if (spacing == 0) {
        paint::paint_line(
            *painter,
            paint::draw_line(fonts, text, face.pixel_size, face.bold),
            pen,
            baseline,
            rgba_of(ink)
        );
        return;
    }
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t bytes = character_bytes(text.substr(at));
        if (bytes == 0)
            break;
        const std::string_view character = text.substr(at, bytes);
        paint::paint_line(
            *painter,
            paint::draw_line(fonts, character, face.pixel_size, face.bold),
            pen,
            baseline,
            rgba_of(ink)
        );
        pen += paint::text_width(fonts, character, face.pixel_size, face.bold) + spacing;
        at += bytes;
    }
}

} // namespace

bool can_draw(const Canvas& canvas) noexcept {
    return canvas.surface != nullptr || is_smooth(canvas);
}

Canvas clipped(const Canvas& canvas, const Rect& rect) {
    Canvas out = canvas;
    if (is_smooth(canvas)) {
        const paint::Box pixels = pixels_of(canvas, rect);
        out.smooth.clip =
            canvas.smooth.clip ? paint::intersect(*canvas.smooth.clip, pixels) : pixels;
        if (out.smooth.clip->empty())
            out.smooth.clip = paint::Box{};
        return out;
    }
    const Rect& outer = canvas.placement.clip;
    if (outer.width <= 0 || outer.height <= 0) {
        out.placement.clip = rect;
        return out;
    }
    const int32_t left = std::max(rect.x, outer.x);
    const int32_t top = std::max(rect.y, outer.y);
    const int32_t right = std::min(rect.x + rect.width, outer.x + outer.width);
    const int32_t bottom = std::min(rect.y + rect.height, outer.y + outer.height);
    out.placement.clip = {left, top, std::max(right - left, 0), std::max(bottom - top, 0)};
    if (out.placement.clip.width == 0 || out.placement.clip.height == 0)
        out.placement.scale = 0;
    return out;
}

Canvas placed_at(const Canvas& canvas, Point origin) {
    Canvas shifted = canvas;
    if (is_smooth(canvas)) {
        shifted.smooth.origin.x += origin.x;
        shifted.smooth.origin.y += origin.y;
        return shifted;
    }
    const int32_t scale = canvas.placement.scale;
    shifted.placement.x += origin.x * scale;
    shifted.placement.y += origin.y * scale;
    if (shifted.placement.clip.width > 0 && shifted.placement.clip.height > 0) {
        shifted.placement.clip.x -= origin.x;
        shifted.placement.clip.y -= origin.y;
    }
    return shifted;
}

void fill_rect(const Canvas& canvas, const Rect& rect, Colour ink) {
    if (is_smooth(canvas)) {
        paint::Painter* painter = painter_of(canvas);
        if (painter == nullptr)
            return;
        paint::Box pixels = pixels_of(canvas, rect);
        if (canvas.smooth.clip)
            pixels = paint::intersect(pixels, *canvas.smooth.clip);
        painter->fill_rect(pixels, rgba_of(ink));
        // A translucent fill leaves the text under it showing.
        if (ink.a == 255)
            cover_text_fit(pixels.x, pixels.y, pixels.width, pixels.height);
        return;
    }
    if (canvas.surface == nullptr || canvas.placement.scale == 0)
        return;
    renderer::fill_source_rect(*canvas.surface, canvas.placement, rect, rgb(ink));
    // The crisp painter fills opaquely. Only the part inside the clip shows,
    // and it covers the text under it.
    Rect covered = rect;
    const Rect& clip = canvas.placement.clip;
    if (clip.width > 0 && clip.height > 0) {
        const int32_t left = std::max(rect.x, clip.x);
        const int32_t top = std::max(rect.y, clip.y);
        const int32_t right = std::min(rect.x + rect.width, clip.x + clip.width);
        const int32_t bottom = std::min(rect.y + rect.height, clip.y + clip.height);
        covered = {
            left, top, std::max(right - left, int32_t{0}), std::max(bottom - top, int32_t{0})
        };
    }
    cover_text_fit(covered.x, covered.y, covered.width, covered.height);
}

void blend_rect(const Canvas& canvas, const Rect& rect, Colour ink, uint32_t opacity) {
    if (is_smooth(canvas)) {
        // The share in 256ths as the painter's opacity in 255ths, to the nearest.
        Colour over = ink;
        over.a = static_cast<uint8_t>(std::min<uint32_t>((opacity * 255U + 128U) / 256U, 255U));
        if (paint::Painter* painter = painter_of(canvas))
            painter->fill_rect(pixels_of(canvas, rect), rgba_of(over));
        return;
    }
    if (canvas.surface != nullptr)
        renderer::blend_source_rect(*canvas.surface, canvas.placement, rect, rgb(ink), opacity);
}

void outline_rect(const Canvas& canvas, const Rect& rect, Colour ink) {
    bevel_rect(canvas, rect, ink, ink);
}

void bevel_rect(const Canvas& canvas, const Rect& rect, Colour light, Colour dark) {
    if (is_smooth(canvas)) {
        if (rect.width <= 0 || rect.height <= 0)
            return;
        if (paint::Painter* painter = painter_of(canvas))
            paint_bevel(*painter, pixels_of(canvas, rect), pixel_length(canvas, 1), light, dark);
        return;
    }
    if (canvas.surface != nullptr)
        renderer::draw_bevel(*canvas.surface, canvas.placement, rect, rgb(light), rgb(dark));
}

namespace {

/// The most pixels across or down a rounded rectangle is sampled at. A
/// larger one is drawn square, rather than not at all.
constexpr int32_t kMostRoundedPixels = 2048;
/// The samples across, and down, each pixel of a rounded rectangle.
constexpr int64_t kRoundSamples = 4;

/// Tells whether a sample, in eighths of a pixel, lies in a capsule.
///
/// @param sx the sample, across
/// @param sy the sample, down
/// @param width8 the capsule's width, in eighths
/// @param height8 the capsule's height, in eighths
/// @param rad8 the ends' radius, in eighths
/// @return true when the sample is inside
bool in_capsule(int64_t sx, int64_t sy, int64_t width8, int64_t height8, int64_t rad8) noexcept {
    if (rad8 <= 0 || width8 <= 0 || height8 <= 0)
        return false;
    const int64_t centre_y = height8 / 2;
    int64_t centre_x = sx;
    if (sx < rad8)
        centre_x = rad8;
    else if (sx > width8 - rad8)
        centre_x = width8 - rad8;
    const int64_t across = sx - centre_x;
    const int64_t down = sy - centre_y;
    return across * across + down * down <= rad8 * rad8;
}

/// Tells whether a sample lies in a rectangle whose four corners are
/// quarter circles of a radius, in eighths of a pixel.
///
/// @param sx the sample's column, in eighths
/// @param sy the sample's row, in eighths
/// @param width8 the rectangle's width, in eighths
/// @param height8 the rectangle's height, in eighths
/// @param rad8 the corners' radius, in eighths
/// @return true when the sample is inside
bool in_cornered(int64_t sx, int64_t sy, int64_t width8, int64_t height8, int64_t rad8) noexcept {
    if (rad8 <= 0 || width8 <= 0 || height8 <= 0)
        return false;
    if (sx < 0 || sy < 0 || sx > width8 || sy > height8)
        return false;
    const int64_t centre_x = std::clamp(sx, rad8, std::max(width8 - rad8, rad8));
    const int64_t centre_y = std::clamp(sy, rad8, std::max(height8 - rad8, rad8));
    const int64_t across = sx - centre_x;
    const int64_t down = sy - centre_y;
    return across * across + down * down <= rad8 * rad8;
}

/// Returns how much of each pixel a capsule covers, or of its outline when
/// the outline is inset by hole pixels.
///
/// Each pixel counts which of its 16 samples, at the middles of a 4 by 4
/// grid, lie in the capsule and, for an outline, outside the inset one.
/// At a stepped scale a pixel more than half covered is covered and the
/// rest are not, so a corner at 1x is a step.
///
/// @param columns the pixels across
/// @param rows the pixels down
/// @param radius_px the ends' radius, in pixels
/// @param hole_px the outline's thickness, in pixels; 0 is the filled capsule
/// @param stepped true rounds each pixel to covered or not
/// @return columns times rows bytes, rows top to bottom
std::vector<uint8_t> capsule_coverage(
    int32_t columns, int32_t rows, int32_t radius_px, int32_t hole_px, bool stepped, bool corners
) {
    const auto inside_shape = [corners](int64_t x, int64_t y, int64_t w, int64_t h, int64_t r) {
        return corners ? in_cornered(x, y, w, h, r) : in_capsule(x, y, w, h, r);
    };
    std::vector<uint8_t> coverage(
        static_cast<std::size_t>(std::max(columns, 0)) * static_cast<std::size_t>(std::max(rows, 0))
    );
    if (columns <= 0 || rows <= 0 || radius_px <= 0)
        return coverage;
    const int64_t rad8 = static_cast<int64_t>(radius_px) * 8;
    const int64_t width8 = static_cast<int64_t>(columns) * 8;
    const int64_t height8 = static_cast<int64_t>(rows) * 8;
    const bool hole = hole_px > 0 && columns > 2 * hole_px && rows > 2 * hole_px;
    const int64_t inner_rad8 =
        hole ? static_cast<int64_t>(std::max(radius_px - hole_px, 0)) * 8 : 0;
    const int64_t inner_w8 = hole ? static_cast<int64_t>(columns - 2 * hole_px) * 8 : 0;
    const int64_t inner_h8 = hole ? static_cast<int64_t>(rows - 2 * hole_px) * 8 : 0;
    const int64_t origin8 = hole ? static_cast<int64_t>(hole_px) * 8 : 0;
    constexpr int64_t samples = kRoundSamples * kRoundSamples;
    for (int32_t row = 0; row < rows; ++row) {
        for (int32_t column = 0; column < columns; ++column) {
            int64_t inside = 0;
            for (int64_t down = 0; down < kRoundSamples; ++down) {
                for (int64_t across = 0; across < kRoundSamples; ++across) {
                    const int64_t sx = static_cast<int64_t>(column) * 8 + 2 * across + 1;
                    const int64_t sy = static_cast<int64_t>(row) * 8 + 2 * down + 1;
                    if (!inside_shape(sx, sy, width8, height8, rad8))
                        continue;
                    if (hole &&
                        inside_shape(sx - origin8, sy - origin8, inner_w8, inner_h8, inner_rad8))
                        continue;
                    ++inside;
                }
            }
            uint8_t byte = static_cast<uint8_t>((inside * 255 + samples / 2) / samples);
            if (stepped)
                byte = byte >= 128 ? uint8_t{255} : uint8_t{0};
            coverage
                [static_cast<std::size_t>(row) * static_cast<std::size_t>(columns) +
                 static_cast<std::size_t>(column)] = byte;
        }
    }
    return coverage;
}

/// The capsules whose coverage a thread keeps.
constexpr std::size_t kKeptCapsules = 16;
/// The most pixels of a capsule whose coverage is kept.
constexpr std::size_t kMostKeptCapsulePixels = std::size_t{512} * 512;

/// A capsule's coverage with what it was worked out from.
struct KeptCapsule {
    bool filled{};                 ///< the coverage is worked out
    int32_t columns{};             ///< the pixels across
    int32_t rows{};                ///< the pixels down
    int32_t radius_px{};           ///< the ends' radius, in pixels
    int32_t hole_px{};             ///< the outline's thickness, in pixels
    bool stepped{};                ///< each pixel rounded to covered or not
    bool corners{};                ///< rounded corners instead of round ends
    std::vector<uint8_t> coverage; ///< capsule_coverage's bytes
};

/// Returns capsule_coverage's bytes for a capsule, kept for the last
/// kKeptCapsules capsules a thread drew up to kMostKeptCapsulePixels: a
/// screen draws the same buttons and rows on every frame it draws.
///
/// @param columns the pixels across
/// @param rows the pixels down
/// @param radius_px the ends' radius, in pixels
/// @param hole_px the outline's thickness, in pixels; 0 is the filled capsule
/// @param stepped true rounds each pixel to covered or not
/// @param corners true rounds the four corners instead of the ends
/// @return the bytes, until the thread asks for another capsule's
const std::vector<uint8_t>& kept_capsule_coverage(
    int32_t columns, int32_t rows, int32_t radius_px, int32_t hole_px, bool stepped, bool corners
) {
    thread_local std::array<KeptCapsule, kKeptCapsules> kept{};
    thread_local std::size_t oldest = 0;
    thread_local std::vector<uint8_t> unkept;
    for (const KeptCapsule& capsule : kept)
        if (capsule.filled && capsule.columns == columns && capsule.rows == rows &&
            capsule.radius_px == radius_px && capsule.hole_px == hole_px &&
            capsule.stepped == stepped && capsule.corners == corners)
            return capsule.coverage;
    const std::size_t pixels = static_cast<std::size_t>(std::max(columns, 0)) *
                               static_cast<std::size_t>(std::max(rows, 0));
    if (pixels > kMostKeptCapsulePixels) {
        unkept = capsule_coverage(columns, rows, radius_px, hole_px, stepped, corners);
        return unkept;
    }
    KeptCapsule& slot = kept[oldest];
    oldest = (oldest + 1) % kept.size();
    slot.filled = true;
    slot.columns = columns;
    slot.rows = rows;
    slot.radius_px = radius_px;
    slot.hole_px = hole_px;
    slot.stepped = stepped;
    slot.corners = corners;
    slot.coverage = capsule_coverage(columns, rows, radius_px, hole_px, stepped, corners);
    return slot.coverage;
}

/// Returns a rounded rectangle's radius in the canvas's own pixels, clamped
/// to half its shorter side.
///
/// @param canvas the canvas
/// @param rect the rectangle, in points
/// @param radius the radius, in points
/// @return the radius, in pixels; 0 when the rectangle covers none
int32_t round_radius_px(const Canvas& canvas, const Rect& rect, int32_t radius) noexcept {
    const Point size = covered_pixels(canvas, rect);
    if (size.x <= 0 || size.y <= 0 || radius <= 0)
        return 0;
    int32_t pixels = radius;
    if (is_smooth(canvas))
        pixels = std::max(to_canvas({0, 0, radius, 0}, canvas.smooth.scale_percent).width, 0);
    else
        pixels = radius * std::max(canvas.placement.scale, int32_t{1});
    return std::min(pixels, std::min(size.x, size.y) / 2);
}

/// Returns a one-point outline's thickness in the canvas's own pixels.
///
/// @param canvas the canvas
/// @return the pixels
int32_t round_outline_px(const Canvas& canvas) noexcept {
    if (is_smooth(canvas))
        return pixel_length(canvas, 1);
    return std::max(canvas.placement.scale, int32_t{1});
}

/// Tells whether a rounded corner at this canvas is a step: the crisp form
/// at scale 1. Every other scale, and the smooth form, keeps a partial edge.
///
/// @param canvas the canvas
/// @return true for a stepped corner
bool round_stepped(const Canvas& canvas) noexcept {
    return !is_smooth(canvas) && canvas.placement.scale == 1;
}

/// Blends a capsule, or its outline, over a rectangle.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param radius the ends' radius, in points
/// @param hole_px the outline's thickness, in pixels; 0 fills the capsule
/// @param ink the colour
void blend_round(
    const Canvas& canvas,
    const Rect& rect,
    int32_t radius,
    int32_t hole_px,
    Colour ink,
    bool corners = false
) {
    const Point size = covered_pixels(canvas, rect);
    if (size.x <= 0 || size.y <= 0)
        return;
    if (size.x > kMostRoundedPixels || size.y > kMostRoundedPixels) {
        if (hole_px > 0)
            outline_rect(canvas, rect, ink);
        else
            fill_rect(canvas, rect, ink);
        return;
    }
    const int32_t radius_px = round_radius_px(canvas, rect, radius);
    if (radius_px <= 0) {
        if (hole_px > 0)
            outline_rect(canvas, rect, ink);
        else
            fill_rect(canvas, rect, ink);
        return;
    }
    const std::vector<uint8_t>& coverage =
        kept_capsule_coverage(size.x, size.y, radius_px, hole_px, round_stepped(canvas), corners);
    blend_coverage(canvas, rect, coverage, ink);
}

} // namespace

void fill_round_rect(const Canvas& canvas, const Rect& rect, int32_t radius, Colour ink) {
    blend_round(canvas, rect, radius, 0, ink);
}

void outline_round_rect(const Canvas& canvas, const Rect& rect, int32_t radius, Colour ink) {
    blend_round(canvas, rect, radius, round_outline_px(canvas), ink);
}

void fill_corner_rect(const Canvas& canvas, const Rect& rect, int32_t radius, Colour ink) {
    blend_round(canvas, rect, radius, 0, ink, true);
}

void outline_corner_rect(const Canvas& canvas, const Rect& rect, int32_t radius, Colour ink) {
    blend_round(canvas, rect, radius, round_outline_px(canvas), ink, true);
}

void rule_rect(const Canvas& canvas, const Rect& rect, Colour ink) {
    if (!is_smooth(canvas)) {
        // A rule is a fill one pixel high or wide at a whole scale.
        fill_rect(canvas, rect, ink);
        return;
    }
    if (rect.width <= 0 || rect.height <= 0)
        return;
    paint::Box pixels = pixels_of(canvas, rect);
    // Across, its rows; down, its columns: as thick as its points make
    // every rule, from its first edge.
    if (rect.height <= rect.width)
        pixels.height = pixel_length(canvas, rect.height);
    else
        pixels.width = pixel_length(canvas, rect.width);
    if (paint::Painter* painter = painter_of(canvas))
        painter->fill_rect(pixels, rgba_of(ink));
}

void box_text(
    const Canvas& canvas,
    FontRole role,
    std::string_view text,
    const Rect& box,
    Align align,
    Colour ink,
    int32_t tracking
) {
    if (is_smooth(canvas)) {
        if (canvas.smooth.fonts != nullptr && !text.empty())
            smooth_text(canvas, role, text, box, align, ink, tracking);
        return;
    }
    if (canvas.surface == nullptr || canvas.fonts == nullptr)
        return;
    draw_boxed_text(
        *canvas.surface, canvas.placement, *canvas.fonts, role, text, box, align, rgb(ink), tracking
    );
}

int32_t measure_text(const Canvas& canvas, FontRole role, std::string_view text) {
    if (is_smooth(canvas)) {
        if (canvas.smooth.fonts == nullptr || text.empty())
            return 0;
        const SmoothFace face = smooth_face(canvas, role);
        const int32_t pixels =
            paint::text_width(*canvas.smooth.fonts, text, face.pixel_size, face.bold);
        const int32_t percent = std::max(canvas.smooth.scale_percent, int32_t{1});
        return (pixels * 100 + percent - 1) / percent;
    }
    if (canvas.fonts == nullptr)
        return 0;
    return text_width(*canvas.fonts, role, text);
}

void draw_picture(const Canvas& canvas, const Rect& rect, const renderer::RgbaPicture& picture) {
    if (is_smooth(canvas)) {
        if (!renderer::picture_drawable(picture))
            return;
        if (paint::Painter* painter = painter_of(canvas))
            painter->draw_rgba(
                picture.pixels,
                static_cast<int>(picture.width),
                static_cast<int>(picture.height),
                pixels_of(canvas, rect)
            );
        return;
    }
    if (canvas.surface != nullptr)
        renderer::draw_picture(*canvas.surface, canvas.placement, rect, picture);
}

Point covered_pixels(const Canvas& canvas, const Rect& rect) noexcept {
    if (rect.width <= 0 || rect.height <= 0)
        return {};
    if (is_smooth(canvas)) {
        const paint::Box pixels = pixels_of(canvas, rect);
        return {std::max(pixels.width, 0), std::max(pixels.height, 0)};
    }
    const int32_t scale = canvas.placement.scale;
    if (scale < 1)
        return {};
    return {rect.width * scale, rect.height * scale};
}

void blend_coverage(
    const Canvas& canvas, const Rect& rect, std::span<const uint8_t> coverage, Colour ink
) {
    const Point size = covered_pixels(canvas, rect);
    if (size.x <= 0 || size.y <= 0)
        return;
    const std::size_t count = static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y);
    if (coverage.size() < count)
        return;
    if (is_smooth(canvas)) {
        paint::Painter* painter = painter_of(canvas);
        if (painter == nullptr)
            return;
        const paint::Box box = pixels_of(canvas, rect);
        painter->draw_coverage(coverage.first(count), size.x, size.y, box.x, box.y, rgba_of(ink));
        return;
    }
    if (canvas.surface == nullptr)
        return;
    // One source pixel a surface pixel, from the rectangle's top left: the
    // placement's clip, in points, becomes the same rows and columns of
    // surface pixels.
    const int32_t scale = canvas.placement.scale;
    renderer::Placement fine;
    fine.x = canvas.placement.x + rect.x * scale;
    fine.y = canvas.placement.y + rect.y * scale;
    fine.scale = 1;
    const Rect& clip = canvas.placement.clip;
    if (clip.width > 0 && clip.height > 0)
        fine.clip = {
            (clip.x - rect.x) * scale,
            (clip.y - rect.y) * scale,
            clip.width * scale,
            clip.height * scale,
        };
    for (int32_t row = 0; row < size.y; ++row)
        for (int32_t column = 0; column < size.x; ++column) {
            const uint32_t covered = coverage
                [static_cast<std::size_t>(row) * static_cast<std::size_t>(size.x) +
                 static_cast<std::size_t>(column)];
            if (covered == 0)
                continue;
            // The share in 255ths as the painter's opacity in 256ths, to the nearest.
            const uint32_t opacity = (covered * renderer::blend_opaque + 127U) / 255U;
            renderer::blend_source_rect(
                *canvas.surface, fine, {column, row, 1, 1}, rgb(ink), opacity
            );
        }
}

void draw_mark(const Canvas& canvas, const renderer::Mark& bits, Point at, Colour ink) {
    if (is_smooth(canvas)) {
        const auto pixels = static_cast<std::size_t>(std::max(bits.width, 0)) *
                            static_cast<std::size_t>(std::max(bits.height, 0));
        if (bits.bits.size() < pixels)
            return;
        // Each set pixel a point's whole pixels, its edges rounded as a fill's.
        for (int32_t row = 0; row < bits.height; ++row)
            for (int32_t column = 0; column < bits.width; ++column)
                if (bits.bits[static_cast<std::size_t>(row * bits.width + column)] != 0)
                    fill_rect(canvas, {at.x + column, at.y + row, 1, 1}, ink);
        return;
    }
    if (canvas.surface != nullptr)
        renderer::draw_mark(*canvas.surface, canvas.placement, bits, at.x, at.y, rgb(ink));
}

namespace {

/// The thread's one offscreen buffer. The smooth canvas object stays put so
/// the painter can keep its pointer while the pixel vector grows.
struct OffscreenBuffer {
    renderer::Surface crisp{};
    paint::Canvas smooth{};
    std::optional<paint::Painter> painter{};
    int claims{0};
    bool crisp_ready{false};
    bool smooth_ready{false};
};

/// Returns this thread's buffer.
///
/// @return the buffer
OffscreenBuffer& offscreen_buffer() noexcept {
    thread_local OffscreenBuffer held;
    return held;
}

/// Cosine of an angle in degrees, in thousandths. 1000 is 1.
///
/// @param degrees the angle, in degrees
/// @return the cosine, 0 to 1000 for an angle folded into a quarter turn
int32_t cosine_thousandths(int32_t degrees) noexcept {
    static constexpr int32_t kCosine[91] = {
        1000, 1000, 999, 999, 998, 996, 995, 993, 990, 988, 985, 982, 978, 974, 970, 966,
        961,  956,  951, 946, 940, 934, 927, 921, 914, 906, 899, 891, 883, 875, 866, 857,
        848,  839,  829, 819, 809, 799, 788, 777, 766, 755, 743, 731, 719, 707, 695, 682,
        669,  656,  643, 629, 616, 602, 588, 574, 559, 545, 530, 515, 500, 485, 469, 454,
        438,  423,  407, 391, 375, 358, 342, 326, 309, 292, 276, 259, 242, 225, 208, 191,
        174,  156,  139, 122, 105, 87,  70,  52,  35,  17,  0,
    };
    int32_t turned = degrees % 360;
    if (turned < 0)
        turned += 360;
    if (turned > 180)
        turned = 360 - turned;
    if (turned > 90)
        turned = 180 - turned;
    return kCosine[turned];
}

/// Clears and sizes the crisp buffer. The vector grows and is never shrunk.
///
/// @param[in,out] held the buffer
/// @param width pixels
/// @param height pixels
void prepare_crisp(OffscreenBuffer& held, int32_t width, int32_t height) {
    const auto bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U;
    if (held.crisp.rgb.size() < bytes)
        held.crisp.rgb.resize(bytes);
    std::fill_n(held.crisp.rgb.begin(), bytes, static_cast<uint8_t>(0));
    held.crisp.width = static_cast<uint32_t>(width);
    held.crisp.height = static_cast<uint32_t>(height);
    held.crisp_ready = true;
}

/// Clears and sizes the smooth buffer. The canvas object is not replaced.
///
/// @param[in,out] held the buffer
/// @param width pixels
/// @param height pixels
void prepare_smooth(OffscreenBuffer& held, int width, int height) {
    const auto bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    if (held.smooth.rgba.size() < bytes)
        held.smooth.rgba.resize(bytes);
    std::fill_n(held.smooth.rgba.begin(), bytes, static_cast<uint8_t>(0));
    held.smooth.width = width;
    held.smooth.height = height;
    if (!held.painter)
        held.painter.emplace(held.smooth);
    held.painter->reset_clip();
    held.smooth_ready = true;
}

/// One pixel of the smooth buffer.
struct Sampled {
    uint8_t r{};
    uint8_t g{};
    uint8_t b{};
    uint8_t a{};
};

/// Returns a smooth pixel, clamped into the buffer.
///
/// @param source the buffer
/// @param column the column
/// @param row the row
/// @return the pixel
Sampled smooth_at(const paint::Canvas& source, int column, int row) noexcept {
    if (source.width <= 0 || source.height <= 0)
        return {};
    if (column < 0)
        column = 0;
    if (row < 0)
        row = 0;
    if (column >= source.width)
        column = source.width - 1;
    if (row >= source.height)
        row = source.height - 1;
    const auto index = (static_cast<std::size_t>(row) * static_cast<std::size_t>(source.width) +
                        static_cast<std::size_t>(column)) *
                       4U;
    return {
        source.rgba[index], source.rgba[index + 1], source.rgba[index + 2], source.rgba[index + 3]
    };
}

/// Mixes two values by a fraction in thousandths.
///
/// @param from the value at 0
/// @param to the value at 1000
/// @param fraction the fraction, 0 to 1000
/// @return the mix
int32_t mix_thousandths(int32_t from, int32_t to, int32_t fraction) noexcept {
    return (from * (1000 - fraction) + to * fraction) / 1000;
}

/// Samples the smooth buffer by blending the source pixels around the sample.
///
/// across and down run from 0 at the first destination pixel to 1000 at the
/// last. The sample position is that fraction times the source size, so it
/// starts on the first source pixel and reaches one past the last pixel at
/// the far edge. A position between two pixels mixes them by the distance.
/// The last sample clamps to the last pixel.
///
/// @param source the buffer
/// @param across thousandths across the width
/// @param down thousandths down the height
/// @return the sampled pixel
Sampled sample_filtered(const paint::Canvas& source, int32_t across, int32_t down) noexcept {
    if (source.width <= 0 || source.height <= 0)
        return {};
    const auto axis = [](int64_t position, int limit, int& index, int& fraction) {
        if (limit <= 1 || position <= 0) {
            index = 0;
            fraction = 0;
            return;
        }
        const int64_t last = static_cast<int64_t>(limit - 1) * 1000;
        if (position >= last) {
            index = limit - 1;
            fraction = 0;
            return;
        }
        index = static_cast<int>(position / 1000);
        fraction = static_cast<int>(position % 1000);
    };
    int column = 0;
    int column_fraction = 0;
    int row = 0;
    int row_fraction = 0;
    // No half-pixel bias: a sample on the boundary between two source pixels
    // has to mix them. A centre adjustment lands a same-size copy inside one
    // pixel and the smooth form stays unfiltered.
    axis(static_cast<int64_t>(across) * source.width, source.width, column, column_fraction);
    axis(static_cast<int64_t>(down) * source.height, source.height, row, row_fraction);
    const Sampled top_left = smooth_at(source, column, row);
    const Sampled top_right = smooth_at(source, column + 1, row);
    const Sampled bottom_left = smooth_at(source, column, row + 1);
    const Sampled bottom_right = smooth_at(source, column + 1, row + 1);
    const auto channel = [&](uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        return static_cast<uint8_t>(mix_thousandths(
            mix_thousandths(a, b, column_fraction),
            mix_thousandths(c, d, column_fraction),
            row_fraction
        ));
    };
    return {
        channel(top_left.r, top_right.r, bottom_left.r, bottom_right.r),
        channel(top_left.g, top_right.g, bottom_left.g, bottom_right.g),
        channel(top_left.b, top_right.b, bottom_left.b, bottom_right.b),
        channel(top_left.a, top_right.a, bottom_left.a, bottom_right.a),
    };
}

/// Blends a colour over one crisp pixel by a share in 256ths.
///
/// @param pixel the pixel's three bytes
/// @param red the colour's red
/// @param green the colour's green
/// @param blue the colour's blue
/// @param share the colour's share, 0 to 256
void blend_crisp(uint8_t* pixel, uint8_t red, uint8_t green, uint8_t blue, int32_t share) noexcept {
    if (share >= 256) {
        pixel[0] = red;
        pixel[1] = green;
        pixel[2] = blue;
        return;
    }
    if (share <= 0)
        return;
    const int32_t keep = 256 - share;
    const uint8_t channels[3] = {red, green, blue};
    for (int channel = 0; channel < 3; ++channel)
        pixel[channel] = static_cast<uint8_t>(
            (static_cast<int32_t>(pixel[channel]) * keep +
             static_cast<int32_t>(channels[channel]) * share + 128) /
            256
        );
}

/// The opacity as a share in 256ths.
///
/// @param opacity the opacity, 0 to 1
/// @return the share
int32_t share_of(float opacity) noexcept {
    const float clamped = std::clamp(opacity, 0.F, 1.F);
    return static_cast<int32_t>(std::lround(static_cast<double>(clamped) * 256.0));
}

/// Copies one trapezoid pixel. across and down are thousandths of the source.
///
/// @param across thousandths across the source
/// @param down thousandths down the source
/// @param share the opacity in 256ths
/// @param filtered true samples the smooth buffer filtered
/// @param[in,out] crisp the crisp destination pixel; null for the smooth form
/// @param[in,out] smooth the smooth destination pixel; null for the crisp form
void copy_sample(
    int32_t across, int32_t down, int32_t share, bool filtered, uint8_t* crisp, uint8_t* smooth
) noexcept {
    const OffscreenBuffer& held = offscreen_buffer();
    if (filtered) {
        if (!held.smooth_ready || smooth == nullptr)
            return;
        const Sampled sampled = sample_filtered(held.smooth, across, down);
        if (sampled.a == 0 || share <= 0)
            return;
        const uint32_t coverage = static_cast<uint32_t>(
            (static_cast<int64_t>(share) * sampled.a * 255 + 128 * 255) / (256 * 255)
        );
        paint::blend_pixel(smooth, paint::Rgba{sampled.r, sampled.g, sampled.b, 255}, coverage);
        return;
    }
    if (!held.crisp_ready || crisp == nullptr || held.crisp.width == 0 || held.crisp.height == 0)
        return;
    int32_t column = static_cast<int32_t>(
        (static_cast<int64_t>(across) * static_cast<int32_t>(held.crisp.width)) / 1000
    );
    int32_t row = static_cast<int32_t>(
        (static_cast<int64_t>(down) * static_cast<int32_t>(held.crisp.height)) / 1000
    );
    if (column >= static_cast<int32_t>(held.crisp.width))
        column = static_cast<int32_t>(held.crisp.width) - 1;
    if (row >= static_cast<int32_t>(held.crisp.height))
        row = static_cast<int32_t>(held.crisp.height) - 1;
    if (column < 0)
        column = 0;
    if (row < 0)
        row = 0;
    const auto index =
        (static_cast<std::size_t>(row) * held.crisp.width + static_cast<std::size_t>(column)) * 3U;
    blend_crisp(
        crisp, held.crisp.rgb[index], held.crisp.rgb[index + 1], held.crisp.rgb[index + 2], share
    );
}

} // namespace

bool claim_offscreen() noexcept {
    OffscreenBuffer& held = offscreen_buffer();
    if (held.claims != 0)
        return false;
    held.claims = 1;
    return true;
}

void release_offscreen() noexcept {
    OffscreenBuffer& held = offscreen_buffer();
    if (held.claims > 0)
        --held.claims;
}

Canvas begin_offscreen(const Canvas& dest, int32_t width, int32_t height) {
    Canvas off{};
    if (!can_draw(dest) || width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return off;
    OffscreenBuffer& held = offscreen_buffer();
    if (is_smooth(dest)) {
        const Rect pixels = to_canvas({0, 0, width, height}, dest.smooth.scale_percent);
        if (pixels.width <= 0 || pixels.height <= 0)
            return off;
        prepare_smooth(held, pixels.width, pixels.height);
        off.smooth.painter = &*held.painter;
        off.smooth.fonts = dest.smooth.fonts;
        off.smooth.scale_percent = dest.smooth.scale_percent;
        off.fonts = dest.fonts;
        off.icon = dest.icon;
        return off;
    }
    const int32_t whole = dest.placement.scale;
    if (whole < 1 || dest.surface == nullptr)
        return off;
    prepare_crisp(held, width * whole, height * whole);
    off.surface = &held.crisp;
    off.placement = {0, 0, whole, {}};
    off.fonts = dest.fonts;
    off.icon = dest.icon;
    return off;
}

void warp_offscreen(
    const Canvas& dest,
    int32_t source_width,
    int32_t source_height,
    const Rect& place,
    Perspective how
) {
    if (source_width <= 0 || source_height <= 0 || place.width <= 0 || place.height <= 0)
        return;
    const int32_t share = share_of(how.opacity);
    if (share <= 0)
        return;
    const bool filtered = is_smooth(dest);
    int32_t pixel_width = 0;
    int32_t pixel_height = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    if (filtered) {
        if (dest.smooth.painter == nullptr)
            return;
        const Rect pixels = to_canvas(
            {dest.smooth.origin.x + place.x,
             dest.smooth.origin.y + place.y,
             place.width,
             place.height},
            dest.smooth.scale_percent
        );
        pixel_width = pixels.width;
        pixel_height = pixels.height;
        origin_x = pixels.x;
        origin_y = pixels.y;
    } else {
        const int32_t whole = dest.placement.scale;
        if (whole < 1 || dest.surface == nullptr)
            return;
        pixel_width = place.width * whole;
        pixel_height = place.height * whole;
        origin_x = dest.placement.x + place.x * whole;
        origin_y = dest.placement.y + place.y * whole;
    }
    if (pixel_width <= 0 || pixel_height <= 0)
        return;
    const int32_t cosine = cosine_thousandths(static_cast<int32_t>(std::lround(how.degrees)));
    int32_t short_edge =
        static_cast<int32_t>((static_cast<int64_t>(pixel_height) * cosine + 500) / 1000);
    if (short_edge < 1)
        short_edge = 1;
    if (short_edge > pixel_height)
        short_edge = pixel_height;
    const bool shorten_right = how.degrees >= 0.F;
    const int32_t left_edge = shorten_right ? pixel_height : short_edge;
    const int32_t right_edge = shorten_right ? short_edge : pixel_height;
    const Rect& point_clip = dest.placement.clip;
    const bool point_clipped = !filtered && point_clip.width > 0 && point_clip.height > 0;
    paint::Canvas* smooth_dest = filtered ? &dest.smooth.painter->canvas() : nullptr;
    for (int32_t row = 0; row < pixel_height; ++row) {
        for (int32_t column = 0; column < pixel_width; ++column) {
            const int32_t across =
                pixel_width <= 1 ? 0
                                 : static_cast<int32_t>(
                                       (static_cast<int64_t>(column) * 1000) / (pixel_width - 1)
                                   );
            const int32_t edge =
                left_edge + static_cast<int32_t>(
                                (static_cast<int64_t>(right_edge - left_edge) * across) / 1000
                            );
            const int32_t top = (pixel_height - edge) / 2;
            if (row < top || row >= top + edge)
                continue;
            const int32_t down =
                edge <= 1
                    ? 0
                    : static_cast<int32_t>((static_cast<int64_t>(row - top) * 1000) / (edge - 1));
            const int32_t dest_x = origin_x + column;
            const int32_t dest_y = origin_y + row;
            if (filtered) {
                if (dest.smooth.clip) {
                    const paint::Box& clip = *dest.smooth.clip;
                    if (clip.empty() || dest_x < clip.x || dest_y < clip.y ||
                        dest_x >= clip.x + clip.width || dest_y >= clip.y + clip.height)
                        continue;
                }
                if (smooth_dest == nullptr || dest_x < 0 || dest_y < 0 ||
                    dest_x >= smooth_dest->width || dest_y >= smooth_dest->height)
                    continue;
                uint8_t* pixel =
                    smooth_dest->rgba.data() + (static_cast<std::size_t>(dest_y) *
                                                    static_cast<std::size_t>(smooth_dest->width) +
                                                static_cast<std::size_t>(dest_x)) *
                                                   4U;
                copy_sample(across, down, share, true, nullptr, pixel);
                continue;
            }
            if (point_clipped) {
                const int32_t point_x = place.x + column / dest.placement.scale;
                const int32_t point_y = place.y + row / dest.placement.scale;
                if (point_x < point_clip.x || point_y < point_clip.y ||
                    point_x >= point_clip.x + point_clip.width ||
                    point_y >= point_clip.y + point_clip.height)
                    continue;
            }
            if (dest_x < 0 || dest_y < 0 || dest_x >= static_cast<int32_t>(dest.surface->width) ||
                dest_y >= static_cast<int32_t>(dest.surface->height))
                continue;
            uint8_t* pixel =
                dest.surface->rgb.data() + (static_cast<std::size_t>(dest_y) * dest.surface->width +
                                            static_cast<std::size_t>(dest_x)) *
                                               3U;
            copy_sample(across, down, share, false, pixel, nullptr);
        }
    }
}

} // namespace oa::ui::kit
