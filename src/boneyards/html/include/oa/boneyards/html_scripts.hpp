// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Glyphs the page faces do not contain: DejaVu Sans, then the engine's text
// faces for Chinese, Japanese and Korean (the endonym face, and a language
// pack's faces when that stack holds them), then Noto Emoji, drawn as
// coverage. The same chain, drawn as bitmaps, fills in an FNT font when the
// bundled faces cannot be opened. The CJK collection is not a file of this
// chain.
#pragma once

#include <cstdint>

#include "oa/boneyards/html_fnt.hpp"

namespace oa::boneyards::html {

struct FallbackFonts;

// Files of the chain. A null path leaves that face out; the others still open.
struct FallbackFiles {
    const char* dejavu_regular; // DejaVu Sans
    const char* dejavu_bold;    // DejaVu Sans Bold
    const char* cjk_medium;     // ignored; CJK is the engine's text faces
    const char* emoji;          // Noto Emoji
};

/// Names the fonts directory the pages' chain is opened from.
///
/// The directory holds DejaVu Sans, DejaVu Sans Bold, Noto Emoji and the
/// engine's text faces. Chinese, Japanese and Korean come from those faces,
/// not from a collection file in the directory. An empty or null path clears
/// the name. The chain already opened is left as it is.
///
/// @param directory fonts directory, or null
void page_fonts_set_directory(const char* directory);

/// Returns the shared chain, opening it from the directory on first use.
///
/// @return the chain, or null when no directory is set or no face opened
FallbackFonts* page_script_fonts();

/// Uses a chain in place of the shared one.
///
/// A test opens its own chain and installs it here. Null returns to the
/// shared chain. The chain is not closed.
///
/// @param fonts chain to use; null restores the shared chain
void page_script_fonts_use(FallbackFonts* fonts);

/// Opens the chain from the named files.
///
/// A face whose file is missing or unreadable is left out. Noto Emoji is set
/// to its regular weight.
///
/// @param files paths of the four faces
/// @return the chain, or null when none of the faces opened
FallbackFonts* fallback_fonts_open_files(const FallbackFiles& files);

/// Closes a chain opened with fallback_fonts_open_files.
///
/// The shared chain is not closed this way. Null is ignored.
///
/// @param fonts chain to close
void fallback_fonts_close(FallbackFonts* fonts);

/// Finds a character in the chain. See ScriptFonts::lookup.
///
/// @param user a FallbackFonts
/// @param code_point Unicode scalar value
/// @param em pixels per em of the page face
/// @param bold whether DejaVu Sans Bold is tried before DejaVu Sans
/// @param[out] out the picture
/// @return true when a face of the chain has the character
bool fallback_lookup(void* user, uint32_t code_point, int32_t em, bool bold, ScriptGlyph* out);

/// Reports whether the chain has a glyph for a character.
///
/// @param fonts chain to ask; null has nothing
/// @param code_point Unicode scalar value
/// @param bold whether the bold DejaVu face is tried first
/// @return true when some face of the chain has the character
bool fallback_has(const FallbackFonts* fonts, uint32_t code_point, bool bold);

/// Reads the characters of a markup run.
///
/// A run of valid UTF-8 is read as UTF-8. Anything else is Windows-1252, one
/// byte a character, including the bytes the markup's entities write.
///
/// @param text run bytes; null is an empty run
/// @param length number of bytes
/// @param[out] out receives the characters, up to `capacity`
/// @param capacity entries `out` holds
/// @return how many characters the run holds, which may exceed `capacity`
int32_t markup_characters(const char* text, int32_t length, uint32_t* out, int32_t capacity);

/// Reports whether a character takes room on a line.
///
/// A control, a variation selector or a zero-width character does not.
///
/// @param code_point Unicode scalar value
/// @return false when the line passes over the character
bool markup_character_visible(uint32_t code_point);

/// Returns the Windows-1252 byte of a character.
///
/// @param code_point Unicode scalar value
/// @return the byte, or -1 when Windows-1252 has no byte for it
int32_t markup_cp1252_byte(uint32_t code_point);

} // namespace oa::boneyards::html
