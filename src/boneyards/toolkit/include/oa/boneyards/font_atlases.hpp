// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Analogue glyphs for the bitmap faces. A face keeps its own pictures for
// the characters it covers. Every other character comes from an atlas drawn
// at the same baseline and coloured as that face's ink is.
#pragma once

#include <cstddef>
#include <cstdint>

#include "oa/present/surface.h"

namespace oa::boneyards {

/// One character of a line drawn by font_atlas_draw.
struct FontAtlasGlyph {
    uint32_t codepoint; ///< Unicode scalar value
    int32_t x;          ///< pen, in surface pixels, before the glyph
    int32_t advance;    ///< pixels the pen moves
    int32_t ink_top;    ///< topmost drawn row, or -1 when the glyph draws nothing
    int32_t ink_bottom; ///< bottommost drawn row, or -1 when the glyph draws nothing
    uint8_t original;   ///< 1 when the face's own picture was used
};

struct FontAtlas;

/// Reads an analogue atlas.
///
/// The atlas keeps its own copy of the bytes.
///
/// @param bytes the atlas file
/// @param size length of `bytes`
/// @return the atlas, or null when the bytes are not an atlas; free it with
///     font_atlas_close
FontAtlas* font_atlas_open(const uint8_t* bytes, size_t size);

/// Frees an atlas opened by font_atlas_open.
///
/// @param atlas atlas to free; null does nothing
void font_atlas_close(FontAtlas* atlas);

/// Where one analogue glyph sits relative to the pen.
struct FontAtlasPlacement {
    int32_t advance = 0; ///< pixels the pen moves
    int32_t left = 0;    ///< picture left of the pen
    int32_t top = 0;     ///< picture rows above the baseline
    int32_t width = 0;
    int32_t rows = 0;
};

/// Looks up one character in a face's fallback order.
///
/// @param atlas analogue glyphs
/// @param face_name the atlas face; a directory or an extension is ignored
/// @param code character to find
/// @param[out] place the glyph's advance and picture box; cleared when the
///     character is absent
/// @return true when the atlas has the character
bool font_atlas_lookup(
    const FontAtlas* atlas, const char* face_name, uint32_t code, FontAtlasPlacement* place
);

/// Plots one analogue glyph in a face's ink.
///
/// The pen is the baseline. A flat face uses `body` for every ink pixel; a
/// shaded face uses `edge` on the right edge of the ink.
///
/// @param target surface to draw on
/// @param atlas analogue glyphs
/// @param face_name the atlas face; a directory or an extension is ignored
/// @param code character to draw
/// @param pen_x pen, in surface pixels
/// @param pen_y baseline, in surface pixels
/// @param body fill colour
/// @param edge right-edge colour
/// @param shaded true when the right edge uses `edge`
/// @return the advance, or 0 when the face has no such glyph
int32_t font_atlas_plot(
    oa::Surface* target,
    const FontAtlas* atlas,
    const char* face_name,
    uint32_t code,
    int32_t pen_x,
    int32_t pen_y,
    uint8_t body,
    uint8_t edge,
    bool shaded
);

/// Draws a UTF-8 line in a bitmap face.
///
/// `y` is the top of the line, as the toolkit's text draw takes it. The pen
/// sits on the face's baseline, one line height below that. Characters the
/// face covers — the printable ASCII pictures, and any other picture of its
/// own that is not a copy of its missing-character box — are copied from the
/// face. Every other character comes from the atlas face of the same name,
/// in that face's fallback order, and is coloured with the face's own ink
/// (one flat colour, or the face's fill and the darker colour along the
/// right edge of its ink). An empty string draws nothing.
///
/// @param target surface to draw on; pixels of the face's key are left as
///     they are
/// @param gaf the face's glyph file
/// @param gaf_size length of `gaf`
/// @param face_name the atlas face, such as "BYHatFontSmall87" or
///     "Hattfont12"; a directory or an extension on the name is ignored
/// @param atlas analogue glyphs
/// @param text UTF-8, ending at the first zero byte
/// @param x left of the line, in surface pixels
/// @param y top of the line, in surface pixels
/// @param places receives one entry per character; null discards them
/// @param places_capacity entries available in `places`
/// @return the number of characters, or -1 when the face, the atlas or the
///     text cannot be drawn (the surface is then left unchanged)
int32_t font_atlas_draw(
    oa::Surface* target,
    const uint8_t* gaf,
    size_t gaf_size,
    const char* face_name,
    const FontAtlas* atlas,
    const char* text,
    int32_t x,
    int32_t y,
    FontAtlasGlyph* places,
    int32_t places_capacity
);

} // namespace oa::boneyards
