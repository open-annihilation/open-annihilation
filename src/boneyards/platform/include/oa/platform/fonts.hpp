// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Host text service standing in for the Windows TrueType fonts 3.1c draws
// its markup text in. A face map turns a GDI face name into a freely
// redistributable font bundled beside the executable; FreeType measures and
// rasterises it the way GDI did on 8-bit surfaces (hinted, unsmoothed glyphs
// with whole-pixel advances). Text is Windows-1252 bytes, as the ANSI GDI
// calls took it.

#include <cstdint>

namespace oa::platform {

struct FontLibrary;
struct Font;

// Directory under the executable's directory holding the bundled fonts.
inline constexpr const char* k_bundled_fonts_directory = "fonts";
// LOGFONT lfFaceName holds 31 characters and a terminator; longer names
// compare on those.
inline constexpr int32_t k_face_name_chars = 31;
inline constexpr int32_t k_weight_normal = 400;
// Requests at or above this weight select a bold face (FW_SEMIBOLD).
inline constexpr int32_t k_weight_bold_threshold = 600;
// Em height of a zero-height request: 12 points at 96 dots per inch.
inline constexpr int32_t k_default_em_pixels = 16;

enum class FontRenderMode : int32_t {
    mono, // aliased: TrueType hinting for monochrome output, coverage 0 or 255
    grey, // antialiased: normal hinting, 256 coverage levels
};

// Vertical metrics a GDI face reports per pixel size, where they differ
// from what the stand-in's own tables give.
struct LineMetricsTable {
    int32_t units_per_em{};
    int32_t win_ascent{};
    int32_t win_descent{};
    int32_t first_em{}; // em of rows[0]
    int32_t count{};
    const uint8_t (*rows)[2]{}; // ascent, descent in pixels
};

// A GDI face request resolved against the face map.
struct FaceMatch {
    const char* face{};   // face map name the request matched
    const char* family{}; // family name of the bundled font
    const char* file{};   // path under the fonts directory
    bool synthetic_bold{};
    bool synthetic_italic{};
    const LineMetricsTable* line_metrics{}; // null: from the bundled font
};

/// Resolves a GDI face request to the bundled file standing in for it.
///
/// A family lacking the requested style is emboldened or slanted instead, as
/// GDI simulated it.
///
/// @param face face name, compared case-insensitively on its first 31 characters
/// @param weight 100..900; k_weight_bold_threshold and above select bold
/// @param italic whether an italic face is requested
/// @param[out] out the matched face, family, file and synthetic styles
/// @return false for a face the map does not know; nothing else is substituted
bool face_resolve(const char* face, int32_t weight, bool italic, FaceMatch* out);

/// Lists every file the face map opens, with each font's licence file.
///
/// @param[out] out receives the static array of paths under the fonts directory
/// @return how many paths there are
int32_t bundled_font_files(const char* const** out);

/// Loads every bundled font.
///
/// @param fonts_dir directory holding the bundled fonts
/// @param[out] error receives a message naming the missing or unreadable file
/// @param error_bytes bytes of error
/// @return the library, or null on failure
FontLibrary* font_library_open(const char* fonts_dir, char* error, int32_t error_bytes);
/// Releases a font library.
///
/// @param library library from font_library_open; null is ignored
void font_library_close(FontLibrary* library);

struct FontRequest {
    // LOGFONT lfHeight: negative is the em height in pixels, positive the
    // cell height (ascent plus descent), zero the default size.
    int32_t height{};
    int32_t weight{}; // 100..900; 0 is normal
    bool italic{};
    const char* face{};
    FontRenderMode mode{};
};

// GetTextMetrics fields, and where the face puts its underline.
struct FontMetrics {
    int32_t em{};     // pixels per em
    int32_t height{}; // ascent plus descent
    int32_t ascent{};
    int32_t descent{};
    int32_t internal_leading{}; // height less the em
    int32_t external_leading{};
    int32_t average_width{}; // weighted lowercase average, as OS/2 version 0 defined it
    int32_t weight{};
    bool italic{};
    int32_t underline_top{};  // rows below the cell top
    int32_t underline_rows{}; // at least 1
};

/// Opens the face a request resolves to at the requested size.
///
/// @param library loaded font library
/// @param request GDI-style height, weight, style, face and render mode
/// @return the font, or null when the face map has no stand-in for it
Font* font_open(FontLibrary* library, const FontRequest& request);
/// Releases a font.
///
/// @param font font from font_open; null is ignored
void font_close(Font* font);
/// Returns a font's GetTextMetrics-style metrics.
///
/// @param font open font
/// @return the metrics, in pixels
FontMetrics font_metrics(const Font* font);

/// Returns the whole-pixel advance of one character.
///
/// @param font open font
/// @param character Windows-1252 byte
/// @return the advance in pixels
int32_t font_char_advance(const Font* font, uint8_t character);

/// Returns the GetTextExtentPoint32 width of a text.
///
/// @param font open font
/// @param text Windows-1252 bytes
/// @param length number of bytes
/// @return the sum of the characters' advances in pixels, with no kerning
int32_t font_text_advance(const Font* font, const char* text, int32_t length);

/// Rasterises text into a coverage buffer with the cell's top-left corner at (0, 0).
///
/// Pixels outside the buffer are dropped; covered pixels keep the larger coverage.
///
/// @param font open font
/// @param text Windows-1252 bytes
/// @param length number of bytes
/// @param[in,out] coverage buffer receiving coverage 0..255
/// @param width buffer width in pixels
/// @param height buffer height in pixels
/// @param pitch bytes between buffer rows
void font_render(
    const Font* font,
    const char* text,
    int32_t length,
    uint8_t* coverage,
    int32_t width,
    int32_t height,
    int32_t pitch
);

/// Reports whether the font has a glyph for a character.
///
/// The missing-glyph box is not a glyph of the character.
///
/// @param font open font
/// @param code_point Unicode scalar value
/// @return true when the font can draw the character
bool font_has_character(const Font* font, uint32_t code_point);

/// Returns the whole-pixel advance of one character.
///
/// A Windows-1252 character uses the advance measured when the font was
/// opened. Any other character is measured the same way. A character the
/// font does not have advances nothing.
///
/// @param font open font
/// @param code_point Unicode scalar value
/// @return the advance in pixels
int32_t font_codepoint_advance(const Font* font, uint32_t code_point);

/// Rasterises one character at a pen position inside a coverage buffer.
///
/// The pen is the column the character's origin sits on. A character the
/// font does not have draws nothing, and no missing-glyph box is drawn.
///
/// @param font open font
/// @param code_point Unicode scalar value
/// @param pen column of the character's origin, in pixels
/// @param[in,out] coverage buffer receiving coverage 0..255
/// @param width buffer width in pixels
/// @param height buffer height in pixels
/// @param pitch bytes between buffer rows
void font_render_codepoint(
    const Font* font,
    uint32_t code_point,
    int32_t pen,
    uint8_t* coverage,
    int32_t width,
    int32_t height,
    int32_t pitch
);

} // namespace oa::platform
