// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/base/bytes.hpp"
#include "oa/formats/hpi.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace oa::formats::fnt {
namespace limit {
inline constexpr std::size_t input_bytes = 4U * 1024U * 1024U;
inline constexpr uint16_t glyph_height = 128;
inline constexpr uint8_t glyph_width = 128;
inline constexpr std::size_t glyph_count = 256;
} // namespace limit

// FNT header: height, a second word, then 256 little-endian offsets. 516 bytes.
inline constexpr std::size_t file_header_bytes = 4;
inline constexpr std::size_t offset_table_bytes = limit::glyph_count * sizeof(uint16_t);
inline constexpr std::size_t file_preamble_bytes = file_header_bytes + offset_table_bytes;
inline constexpr uint8_t foreground_index = 255;
inline constexpr uint8_t bits_per_pixel = 1;

struct Glyph {
    uint16_t width = 0, height = 0;
    int16_t origin_x = 0, origin_y = 0;
    std::vector<uint8_t> pixels, coverage;
};

struct Font {
    // The FNT header's second word, kept as read. Its low byte, signed, is
    // the rows every glyph starts above the pen row (see row_lift); shipped
    // fonts hold 1, 2 or 3 in it. Zero for a GAF-backed font.
    uint16_t word_after_height = 0;
    uint16_t nominal_height = 0;
    std::array<std::optional<Glyph>, limit::glyph_count> glyphs;
};

/// Parses a 1-bit FNT bitmap font.
///
/// Each present glyph becomes foreground_index pixels plus a coverage mask.
///
/// @param bytes the whole file
/// @return the font with nominal_height and word_after_height from the
///         header; or the first error at its file offset: an input over the
///         size limit, a short header, a height, width or glyph offset out
///         of range, or a truncated glyph bitmap
[[nodiscard]] base::bytes::Decoded<Font> parse_fnt(std::span<const uint8_t> bytes);
/// Parses a GAF-backed GUI font: the first sequence's frames are glyphs 0..255.
///
/// When glyph 0x49 exists its height becomes the nominal height and is
/// subtracted from every glyph's origin_y, as the game does when it loads a
/// GUI font.
///
/// @param bytes the whole GAF file
/// @return the font; or an input over the size limit, an unparsable GAF (its
///         own error code in detail), no sequence, more than 256 frames, a
///         frame or layer over 128 pixels, refused before it is decoded, or
///         a glyph that cannot be rendered or is over 128 pixels (the glyph
///         number in detail)
[[nodiscard]] base::bytes::Decoded<Font> parse_gaf(std::span<const uint8_t> bytes);
/// Reads and parses an FNT font from the game's assets.
///
/// The asset store's own errors, such as a missing file, propagate as it
/// reports them.
///
/// @param assets asset store searched
/// @param resource virtual path of the FNT file
/// @return the font, or parse_fnt's error
[[nodiscard]] base::bytes::Decoded<Font> load_fnt(AssetStore& assets, std::string_view resource);
/// Reads and parses a GUI font GAF from the game's assets (see parse_gaf).
///
/// The asset store's own errors, such as a missing file, propagate as it
/// reports them.
///
/// @param assets asset store searched
/// @param resource virtual path of the GAF file
/// @return the font, with every glyph lowered by the height of glyph 0x49;
///         or parse_gaf's error
[[nodiscard]] base::bytes::Decoded<Font> load_gaf(AssetStore& assets, std::string_view resource);
/// Loads fonts\\<name>.FNT, trying a language directory first.
///
/// The last dotted suffix is dropped before ".FNT" is appended, including a
/// dot in the language directory. A missing or empty file ends the search:
/// no other path is tried, and the game treats it as fatal.
///
/// @param assets asset store searched
/// @param name font name, e.g. "smlfont"
/// @param language language string; when nonempty fonts-<language>\\<name>.FNT
///        is used if that file opens
/// @return the font; or not_found for a missing or empty file,
///         limit_exceeded for a path over 255 bytes, or parse_fnt's error
[[nodiscard]] base::bytes::Decoded<Font>
load_named_fnt(AssetStore& assets, std::string_view name, std::string_view language);
/// Returns the width of a text in pixels: the sum of its glyph widths.
///
/// Control bytes below 0x20 and missing glyphs add nothing; the sum saturates.
///
/// @param font font measured
/// @param text bytes of the text
/// @return width in pixels
[[nodiscard]] uint32_t measure_text(const Font& font, std::string_view text) noexcept;
/// Returns the longest start of a text that fits a width, as measure_text
/// measures it: the text less the last characters that would take it past
/// the width, as the game trims a label that is too wide.
///
/// Characters that add nothing to the width stay, after the last that fits
/// as anywhere else; a width too narrow for the first character keeps none.
///
/// @param font font measured
/// @param text bytes of the text
/// @param width widest the start may be, in pixels
/// @return the start of `text` that fits
[[nodiscard]] std::string_view
fit_text(const Font& font, std::string_view text, uint32_t width) noexcept;
/// Returns the line height: the height of glyph 'I' (0x49) plus two pixels.
///
/// @param font font measured; its nominal height stands in when glyph 'I' is missing
/// @return height in pixels
[[nodiscard]] uint16_t line_height(const Font& font) noexcept;
/// Returns the rows the game draws a glyph above its pen row.
///
/// The game draws every FNT glyph from the pen row less the low byte of the
/// header's second word, read signed: one row up for COMIX, CONSOLE and
/// SMLFONT, two for the briefing fonts. raster_text applies it, so a caller
/// passes the pen row the game passes.
///
/// @param font font drawn with
/// @return rows above the pen row; negative moves the glyphs down
[[nodiscard]] int32_t row_lift(const Font& font) noexcept;

struct IndexedSurface {
    uint32_t width = 0, height = 0;
    std::size_t stride = 0;
    std::span<uint8_t> pixels;
    // Optional buffer with the same stride and bounds. A set byte records an
    // actual glyph write even when its palette index equals the clear value.
    std::span<uint8_t> coverage;
};

/// Draws text as palette indices with origin offsets and clipping.
///
/// Each glyph's rows start the font's row_lift above the pen row, as the
/// game places every FNT glyph. Control bytes and missing glyphs are
/// skipped; a space advances without drawing. Throws std::runtime_error when
/// the surface or a glyph has inconsistent buffers.
///
/// @param surface destination pixels and optional coverage mask
/// @param font font drawn with
/// @param text bytes of the text
/// @param x pen column in surface pixels
/// @param y pen row in surface pixels; glyph origins and the font's
///        row_lift are subtracted
/// @return the final pen x
[[nodiscard]] int32_t
raster_text(IndexedSurface surface, const Font& font, std::string_view text, int32_t x, int32_t y);
} // namespace oa::formats::fnt
