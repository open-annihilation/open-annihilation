// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend_renderer/artless.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <string_view>
#include <vector>

namespace oa::ui::frontend_renderer {

namespace {

/// The first byte a font draws or advances over; lower bytes are skipped.
constexpr unsigned char first_printable = 0x20;

/// A rectangle in source pixels with 64-bit edges: left and top inclusive,
/// right and bottom exclusive.
struct Span64 {
    int64_t left{};   ///< first column
    int64_t top{};    ///< first row
    int64_t right{};  ///< column after the last
    int64_t bottom{}; ///< row after the last
};

/// Divides, rounding towards negative infinity.
///
/// @param numerator the dividend
/// @param denominator the divisor, above 0
/// @return the quotient rounded down
int64_t floor_divide(int64_t numerator, int64_t denominator) noexcept {
    const int64_t quotient = numerator / denominator;
    return (numerator % denominator != 0 && numerator < 0) ? quotient - 1 : quotient;
}

/// Clamps a value to the 32-bit range.
///
/// @param value the value
/// @return the nearest 32-bit value
int32_t clamp_to_int32(int64_t value) noexcept {
    return static_cast<int32_t>(std::clamp<int64_t>(
        value, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()
    ));
}

/// Narrows a source rectangle to a placement's clip, when it has one.
///
/// @param[in,out] span the rectangle, in source pixels
/// @param placement the placement
void clip_to_placement(Span64& span, const Placement& placement) noexcept {
    const SourceRect& clip = placement.clip;
    if (clip.width <= 0 || clip.height <= 0)
        return;
    span.left = std::max<int64_t>(span.left, clip.x);
    span.top = std::max<int64_t>(span.top, clip.y);
    span.right = std::min<int64_t>(span.right, int64_t{clip.x} + clip.width);
    span.bottom = std::min<int64_t>(span.bottom, int64_t{clip.y} + clip.height);
}

/// Returns the source pixels whose blocks touch the surface, within the
/// placement's clip.
///
/// @param surface the surface
/// @param placement where source pixels land, its scale at least 1
/// @return the source rectangle of every source pixel drawn at least in part
Span64 visible_source(const Surface& surface, const Placement& placement) noexcept {
    const int64_t scale = placement.scale;
    Span64 shown{
        floor_divide(-int64_t{placement.x}, scale),
        floor_divide(-int64_t{placement.y}, scale),
        floor_divide(int64_t{surface.width} - placement.x - 1, scale) + 1,
        floor_divide(int64_t{surface.height} - placement.y - 1, scale) + 1,
    };
    clip_to_placement(shown, placement);
    return shown;
}

/// Tells whether a surface's pixels fill its size.
///
/// @param surface the surface
/// @return true when it holds width x height RGB pixels
bool whole(const Surface& surface) noexcept {
    return surface.rgb.size() >= static_cast<std::size_t>(surface.width) * surface.height * 3U;
}

/// Returns a palette colour's brightness: 299 red + 587 green + 114 blue.
///
/// @param palette the palette, four bytes a colour
/// @param index the colour's index
/// @return the brightness, 0 to 255000
uint32_t brightness(const oa::PaletteBytes& palette, std::size_t index) noexcept {
    const std::size_t at = index * oa::palette_entry_bytes;
    return 299U * palette[at] + 587U * palette[at + 1] + 114U * palette[at + 2];
}

/// Draws text in one colour, each covered glyph pixel at the ink its palette
/// index has, or in the colour itself without ink.
///
/// @param[in,out] surface the surface
/// @param placement where source pixels land
/// @param font the font
/// @param ink the share of the colour each palette index draws, or nullptr for all of it
/// @param text the text, one byte a glyph
/// @param x the pen column, in source pixels
/// @param y the pen row, in source pixels, as raster_text takes it
/// @param color the colour
/// @return the pen column after the text, in source pixels
int32_t draw_glyphs(
    Surface& surface,
    const Placement& placement,
    const oa::formats::fnt::Font& font,
    const GlyphInk* ink,
    std::string_view text,
    int32_t x,
    int32_t y,
    Rgb color
) {
    // Bound the glyphs' pixels as raster_text places them, then only the part
    // the surface shows.
    Span64 box{
        std::numeric_limits<int64_t>::max(),
        std::numeric_limits<int64_t>::max(),
        std::numeric_limits<int64_t>::min(),
        std::numeric_limits<int64_t>::min(),
    };
    int64_t pen = x;
    for (const unsigned char byte : text) {
        if (byte < first_printable || !font.glyphs[byte])
            continue;
        const auto& glyph = *font.glyphs[byte];
        if (byte != first_printable && glyph.width != 0 && glyph.height != 0) {
            const int64_t left = pen - glyph.origin_x;
            const int64_t top = int64_t{y} - glyph.origin_y;
            box.left = std::min(box.left, left);
            box.top = std::min(box.top, top);
            box.right = std::max(box.right, left + glyph.width);
            box.bottom = std::max(box.bottom, top + glyph.height);
        }
        pen += glyph.width;
    }
    const int32_t end = clamp_to_int32(pen);
    if (placement.scale < 1 || !whole(surface))
        return end;
    const Span64 shown = visible_source(surface, placement);
    box.left = std::max(box.left, shown.left);
    box.top = std::max(box.top, shown.top);
    box.right = std::min(box.right, shown.right);
    box.bottom = std::min(box.bottom, shown.bottom);
    if (box.left >= box.right || box.top >= box.bottom)
        return end;

    const auto columns = static_cast<uint32_t>(box.right - box.left);
    const auto rows = static_cast<uint32_t>(box.bottom - box.top);
    const std::size_t count = static_cast<std::size_t>(columns) * rows;
    std::vector<uint8_t> indices(count);
    std::vector<uint8_t> coverage(count);
    const oa::formats::fnt::IndexedSurface scratch{columns, rows, columns, indices, coverage};
    static_cast<void>(oa::formats::fnt::raster_text(
        scratch,
        font,
        text,
        clamp_to_int32(int64_t{x} - box.left),
        clamp_to_int32(int64_t{y} - box.top)
    ));
    for (uint32_t row = 0; row < rows; ++row)
        for (uint32_t column = 0; column < columns; ++column) {
            const std::size_t at = static_cast<std::size_t>(row) * columns + column;
            if (coverage[at] == 0)
                continue;
            const uint32_t share = ink == nullptr ? blend_opaque : (*ink)[indices[at]];
            if (share == 0)
                continue;
            blend_source_rect(
                surface,
                placement,
                {static_cast<int32_t>(box.left + column),
                 static_cast<int32_t>(box.top + row),
                 1,
                 1},
                color,
                share
            );
        }
    return end;
}

/// Turns a picture drawn as rows of '#' (drawn) and '.' (not) into a mark's bytes.
///
/// @param rows the picture's rows, each `columns` characters
/// @return 1 where a row holds '#', 0 elsewhere
template <std::size_t columns, std::size_t rows>
constexpr std::array<uint8_t, columns * rows>
mark_bits(const std::array<std::string_view, rows>& picture) {
    std::array<uint8_t, columns * rows> bits{};
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t column = 0; column < columns; ++column)
            bits[row * columns + column] = picture[row][column] == '#' ? 1 : 0;
    return bits;
}

/// The thin OA mark's columns.
constexpr std::size_t thin_mark_columns = 9;
/// The thin OA mark's rows.
constexpr std::size_t thin_mark_rows = 5;
/// The thin OA mark's pixels.
constexpr auto thin_mark_bits =
    mark_bits<thin_mark_columns, thin_mark_rows>(std::array<std::string_view, thin_mark_rows>{
        ".##...##.",
        "#..#.#..#",
        "#..#.####",
        "#..#.#..#",
        ".##..#..#",
    });

/// The bold OA mark's columns.
constexpr std::size_t bold_mark_columns = 13;
/// The bold OA mark's rows.
constexpr std::size_t bold_mark_rows = 7;
/// The bold OA mark's pixels.
constexpr auto bold_mark_bits =
    mark_bits<bold_mark_columns, bold_mark_rows>(std::array<std::string_view, bold_mark_rows>{
        ".####...####.",
        "##..##.##..##",
        "##..##.##..##",
        "##..##.######",
        "##..##.##..##",
        "##..##.##..##",
        ".####..##..##",
    });

} // namespace

const Mark oa_mark_thin{
    static_cast<int32_t>(thin_mark_columns), static_cast<int32_t>(thin_mark_rows), thin_mark_bits
};
const Mark oa_mark_bold{
    static_cast<int32_t>(bold_mark_columns), static_cast<int32_t>(bold_mark_rows), bold_mark_bits
};

void fill_source_rect(
    Surface& surface, const Placement& placement, const SourceRect& rect, Rgb color
) noexcept {
    blend_source_rect(surface, placement, rect, color, blend_opaque);
}

void blend_source_rect(
    Surface& surface,
    const Placement& placement,
    const SourceRect& rect,
    Rgb color,
    uint32_t opacity
) noexcept {
    if (placement.scale < 1 || rect.width <= 0 || rect.height <= 0)
        return;
    // Clip in 64 bits first, so that no placement or rectangle overflows:
    // to the placement's clip in source pixels, then to the surface.
    Span64 source{rect.x, rect.y, int64_t{rect.x} + rect.width, int64_t{rect.y} + rect.height};
    clip_to_placement(source, placement);
    if (source.left >= source.right || source.top >= source.bottom)
        return;
    const int64_t scale = placement.scale;
    const int64_t left = std::max<int64_t>(placement.x + source.left * scale, 0);
    const int64_t top = std::max<int64_t>(placement.y + source.top * scale, 0);
    const int64_t right =
        std::min<int64_t>(placement.x + source.right * scale, int64_t{surface.width});
    const int64_t bottom =
        std::min<int64_t>(placement.y + source.bottom * scale, int64_t{surface.height});
    if (left >= right || top >= bottom)
        return;
    blend_rect(
        surface,
        static_cast<int>(left),
        static_cast<int>(top),
        static_cast<int>(right - left),
        static_cast<int>(bottom - top),
        color,
        opacity
    );
}

void draw_bevel(
    Surface& surface, const Placement& placement, const SourceRect& rect, Rgb light, Rgb dark
) noexcept {
    if (rect.width <= 0 || rect.height <= 0)
        return;
    const int32_t right = rect.x + rect.width - 1;
    const int32_t bottom = rect.y + rect.height - 1;
    fill_source_rect(surface, placement, {rect.x, rect.y, rect.width - 1, 1}, light);
    fill_source_rect(surface, placement, {rect.x, rect.y, 1, rect.height - 1}, light);
    fill_source_rect(surface, placement, {rect.x, bottom, rect.width, 1}, dark);
    fill_source_rect(surface, placement, {right, rect.y, 1, rect.height}, dark);
}

void draw_outline(
    Surface& surface, const Placement& placement, const SourceRect& rect, Rgb color
) noexcept {
    draw_bevel(surface, placement, rect, color, color);
}

int32_t draw_text(
    Surface& surface,
    const Placement& placement,
    const oa::formats::fnt::Font& font,
    std::string_view text,
    int32_t x,
    int32_t y,
    Rgb color
) {
    return draw_glyphs(surface, placement, font, nullptr, text, x, y, color);
}

int32_t draw_text(
    Surface& surface,
    const Placement& placement,
    const TextFont& font,
    std::string_view text,
    int32_t x,
    int32_t y,
    Rgb color
) {
    return draw_glyphs(surface, placement, font.font, &font.ink, text, x, y, color);
}

TextFont text_font(oa::formats::fnt::Font font, const oa::PaletteBytes& palette) {
    std::array<uint64_t, oa::palette_color_count> edge_pixels{};
    std::array<bool, oa::palette_color_count> held{};
    for (const auto& glyph : font.glyphs) {
        if (!glyph)
            continue;
        const auto count = static_cast<std::size_t>(glyph->width) * glyph->height;
        if (glyph->pixels.size() != count || glyph->coverage.size() != count)
            continue;
        const auto covered = [&](int64_t column, int64_t row) {
            return column >= 0 && row >= 0 && column < glyph->width && row < glyph->height &&
                   glyph->coverage
                           [static_cast<std::size_t>(row) * glyph->width +
                            static_cast<std::size_t>(column)] != 0;
        };
        for (int64_t row = 0; row < glyph->height; ++row)
            for (int64_t column = 0; column < glyph->width; ++column) {
                if (!covered(column, row))
                    continue;
                const uint8_t index = glyph->pixels
                                          [static_cast<std::size_t>(row) * glyph->width +
                                           static_cast<std::size_t>(column)];
                held[index] = true;
                if (!covered(column - 1, row) || !covered(column + 1, row) ||
                    !covered(column, row - 1) || !covered(column, row + 1))
                    ++edge_pixels[index];
            }
    }

    // The ring: the colour most edge pixels hold, the darker on a tie. The
    // brightest: the brightest colour held.
    std::optional<uint32_t> ring;
    std::optional<uint32_t> brightest;
    uint64_t ring_pixels = 0;
    for (std::size_t index = 0; index < held.size(); ++index) {
        if (!held[index])
            continue;
        const uint32_t level = brightness(palette, index);
        if (!brightest || level > *brightest)
            brightest = level;
        if (edge_pixels[index] > ring_pixels ||
            (edge_pixels[index] == ring_pixels && edge_pixels[index] != 0 && level < *ring)) {
            ring = level;
            ring_pixels = edge_pixels[index];
        }
    }

    TextFont readied{std::move(font), {}};
    if (!brightest)
        return readied;
    const uint32_t dark = ring.value_or(*brightest);
    for (std::size_t index = 0; index < held.size(); ++index) {
        if (!held[index])
            continue;
        const uint32_t level = brightness(palette, index);
        if (dark >= *brightest) {
            readied.ink[index] = static_cast<uint16_t>(blend_opaque);
        } else if (level > dark) {
            const uint32_t range = *brightest - dark;
            readied.ink[index] =
                static_cast<uint16_t>(((level - dark) * blend_opaque + range / 2) / range);
        }
    }
    return readied;
}

int32_t text_width(const oa::formats::fnt::Font& font, std::string_view text) noexcept {
    const uint32_t width = oa::formats::fnt::measure_text(font, text);
    return static_cast<int32_t>(std::min<uint32_t>(width, INT32_MAX));
}

int32_t text_width(const TextFont& font, std::string_view text) noexcept {
    return text_width(font.font, text);
}

bool picture_drawable(const RgbaPicture& picture) noexcept {
    if (picture.width == 0 || picture.height == 0)
        return false;
    return picture.pixels.size() / picture_pixel_bytes / picture.width >= picture.height;
}

void draw_picture(
    Surface& surface, const Placement& placement, const SourceRect& rect, const RgbaPicture& picture
) noexcept {
    if (placement.scale < 1 || rect.width <= 0 || rect.height <= 0 || !picture_drawable(picture) ||
        !whole(surface))
        return;
    Span64 source{rect.x, rect.y, int64_t{rect.x} + rect.width, int64_t{rect.y} + rect.height};
    clip_to_placement(source, placement);
    if (source.left >= source.right || source.top >= source.bottom)
        return;
    const int64_t scale = placement.scale;
    // The whole rectangle on the surface, and the part of it drawn.
    const int64_t rect_left = placement.x + int64_t{rect.x} * scale;
    const int64_t rect_top = placement.y + int64_t{rect.y} * scale;
    const int64_t rect_columns = int64_t{rect.width} * scale;
    const int64_t rect_rows = int64_t{rect.height} * scale;
    const int64_t left = std::max<int64_t>(placement.x + source.left * scale, 0);
    const int64_t top = std::max<int64_t>(placement.y + source.top * scale, 0);
    const int64_t right =
        std::min<int64_t>(placement.x + source.right * scale, int64_t{surface.width});
    const int64_t bottom =
        std::min<int64_t>(placement.y + source.bottom * scale, int64_t{surface.height});
    const int64_t picture_columns = picture.width;
    const int64_t picture_rows = picture.height;
    constexpr uint64_t opaque = 255;
    if (rect_columns == picture_columns && rect_rows == picture_rows) {
        // At the picture's own size each pixel is the picture's, laid over
        // the surface by its alpha.
        for (int64_t row = top; row < bottom; ++row) {
            const uint8_t* from = picture.pixels.data() +
                                  static_cast<std::size_t>(
                                      (row - rect_top) * picture_columns + (left - rect_left)
                                  ) * picture_pixel_bytes;
            uint8_t* to = surface.rgb.data() +
                          static_cast<std::size_t>(row * int64_t{surface.width} + left) * 3U;
            for (int64_t column = left; column < right;
                 ++column, from += picture_pixel_bytes, to += 3) {
                const uint64_t alpha = from[3];
                if (alpha == opaque) {
                    to[0] = from[0];
                    to[1] = from[1];
                    to[2] = from[2];
                    continue;
                }
                if (alpha == 0)
                    continue;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    to[channel] = static_cast<uint8_t>(
                        (from[channel] * alpha + to[channel] * (opaque - alpha) + opaque / 2) /
                        opaque
                    );
            }
        }
        return;
    }
    for (int64_t row = top; row < bottom; ++row) {
        const int64_t shown_row = row - rect_top;
        const int64_t first_row = shown_row * picture_rows / rect_rows;
        const int64_t end_row = std::max(first_row + 1, (shown_row + 1) * picture_rows / rect_rows);
        for (int64_t column = left; column < right; ++column) {
            const int64_t shown_column = column - rect_left;
            const int64_t first_column = shown_column * picture_columns / rect_columns;
            const int64_t end_column =
                std::max(first_column + 1, (shown_column + 1) * picture_columns / rect_columns);
            uint64_t alpha_sum = 0;
            std::array<uint64_t, 3> color_sums{};
            for (int64_t from_row = first_row; from_row < end_row; ++from_row) {
                const uint8_t* from =
                    picture.pixels.data() +
                    static_cast<std::size_t>(from_row * picture_columns + first_column) *
                        picture_pixel_bytes;
                for (int64_t from_column = first_column; from_column < end_column;
                     ++from_column, from += picture_pixel_bytes) {
                    const uint64_t alpha = from[3];
                    alpha_sum += alpha;
                    for (std::size_t channel = 0; channel < color_sums.size(); ++channel)
                        color_sums[channel] += from[channel] * alpha;
                }
            }
            if (alpha_sum == 0)
                continue;
            const auto count =
                static_cast<uint64_t>((end_row - first_row) * (end_column - first_column));
            const uint64_t alpha = (alpha_sum + count / 2) / count;
            uint8_t* to = surface.rgb.data() +
                          static_cast<std::size_t>(row * int64_t{surface.width} + column) * 3U;
            for (std::size_t channel = 0; channel < color_sums.size(); ++channel) {
                const uint64_t color = (color_sums[channel] + alpha_sum / 2) / alpha_sum;
                to[channel] = static_cast<uint8_t>(
                    (color * alpha + to[channel] * (opaque - alpha) + opaque / 2) / opaque
                );
            }
        }
    }
}

void draw_mark(
    Surface& surface, const Placement& placement, const Mark& mark, int32_t x, int32_t y, Rgb color
) noexcept {
    if (mark.width <= 0 || mark.height <= 0)
        return;
    const std::size_t count = static_cast<std::size_t>(mark.width) * mark.height;
    if (mark.bits.size() < count)
        return;
    for (int32_t row = 0; row < mark.height; ++row)
        for (int32_t column = 0; column < mark.width; ++column)
            if (mark.bits[static_cast<std::size_t>(row) * mark.width + column] != 0)
                fill_source_rect(surface, placement, {x + column, y + row, 1, 1}, color);
}

} // namespace oa::ui::frontend_renderer
