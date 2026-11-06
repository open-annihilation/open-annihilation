// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Platform scaffolding: a TextBackend over the game's bitmap FNT fonts. 3.1c
// uses Windows TrueType fonts (Arial); this backend only approximates
// them (two sizes, synthetic bold, no italic) and stands in where no host
// text service or installed face is available (see html_fonts.hpp).
#pragma once

#include "oa/boneyards/html.hpp"
#include "oa/formats/fnt.hpp"

namespace oa::boneyards::html {

// One character drawn from the bitmap chain behind an FNT font. `coverage`
// stays valid until the next lookup on the same chain.
struct ScriptGlyph {
    int32_t advance;
    int32_t left; // columns to the right of the pen; negative reaches left
    int32_t top;  // rows of the picture above the baseline
    int32_t width;
    int32_t rows;
    const uint8_t* coverage; // 0..255, `width` * `rows` bytes; null when empty
};

// Bitmap chain for characters an FNT font does not contain. A false lookup
// draws nothing: the missing-glyph box is never used.
struct ScriptFonts {
    void* user;
    /// Finds a character in the chain at `em` pixels per em.
    ///
    /// @param user the chain
    /// @param code_point Unicode scalar value
    /// @param em pixels per em of the page face, at least 1
    /// @param bold whether the markup run is bold
    /// @param[out] out the picture; untouched when the chain has no glyph
    /// @return true when the chain has the character
    bool (*lookup)(void* user, uint32_t code_point, int32_t em, bool bold, ScriptGlyph* out);
};

struct FntFonts {
    const oa::formats::fnt::Font* regular; // used up to `large_from - 1` pixels
    const oa::formats::fnt::Font* large;   // optional, used from `large_from` pixels
    int32_t large_from;
    // Optional. Characters the FNT font lacks are drawn from here, as bitmaps.
    ScriptFonts* scripts = nullptr;
};

/// Returns a TextBackend over the game's bitmap FNT fonts.
///
/// A font whose character height reaches `large_from` pixels uses `large`
/// when it is set, the others `regular`. Bold is drawn twice one pixel apart
/// (and measures one pixel wider), italic is ignored, and an underline runs
/// one row below the ascent (the line height less 2). The stock-font space is
/// the regular font's space glyph.
///
/// @param fonts fonts to use; must outlive every document using the backend
/// @return the backend
TextBackend fnt_text_backend(FntFonts* fonts);

} // namespace oa::boneyards::html
