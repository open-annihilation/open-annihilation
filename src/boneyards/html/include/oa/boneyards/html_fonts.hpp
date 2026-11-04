// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Platform scaffolding: a TextBackend that sets markup text in installed
// TrueType faces through a host text service, as 3.1c draws Windows TrueType
// text onto its 8-bit canvas. The FNT backend stands in whenever there is no
// service or no face of a font's family is installed.
#pragma once

#include <cstdint>
#include <vector>

#include "oa/boneyards/html.hpp"
#include "oa/boneyards/html_fnt.hpp"

namespace oa::boneyards::html {

// Font a service is asked to open. `height` keeps the LOGFONT convention:
// negative is the em height in pixels, positive the cell height (ascent
// plus descent), zero the service's default size.
struct FontRequest {
    int32_t height;
    int32_t weight; // 100..900; 400 normal, 700 bold
    bool italic;
    const char* face;
};

// GetTextMetrics tmHeight and tmAscent, and where the face puts its
// underline.
struct FontMetrics {
    int32_t height;
    int32_t ascent;
    int32_t underline_top;  // rows below the cell top
    int32_t underline_rows; // thickness, at least 1
};

// Host text service. Text is Windows-1252 bytes, as 3.1c draws it.
// A zero `open` means no service.
struct FontService {
    void* user;
    // The face as named, or null when the host has none by that name. A
    // host may answer a name with a stand-in it ships for exactly that name,
    // never with an unrelated face; the caller walks the fallback chain.
    void* (*open)(void* user, const FontRequest& request);
    void (*close)(void* user, void* font);
    FontMetrics (*metrics)(void* user, void* font);
    // Advance width of `length` bytes of `text`.
    int32_t (*advance)(void* user, void* font, const char* text, int32_t length);
    // Coverage 0..255 of the text with the cell's top-left at (0, 0) into a
    // cleared `width` x `height` buffer whose rows are `width` bytes apart.
    void (*render)(
        void* user,
        void* font,
        const char* text,
        int32_t length,
        uint8_t* coverage,
        int32_t width,
        int32_t height
    );
};

// Windows draws unsmoothed text into a palette bitmap: a pixel takes the text
// colour's nearest palette entry when its coverage reaches this, and is left
// alone otherwise.
inline constexpr uint8_t k_coverage_threshold = 0x80;

// Faces tried for one markup face, most preferred first.
inline constexpr int32_t k_max_face_candidates = 8;

// One opened font, keyed by its height, weight, italic and underline style and face.
struct CachedFont {
    int32_t height;
    int32_t weight;
    bool italic;
    bool underline;
    char face[k_face_bytes];
    void* handle;                // null when no face of the chain is installed
    char resolved[k_face_bytes]; // face the service opened
    FontMetrics metrics;
};

struct ServiceFonts {
    FontService service;
    // Optional; measures and draws what the service cannot, and supplies the
    // stock-font space.
    FntFonts* fallback;
    std::vector<CachedFont> cache;
    std::vector<uint8_t> coverage;
};

/// Lists the faces tried for a markup face, most preferred first.
///
/// A known family (Arial or Helvetica, Haettenschweiler, Times New Roman or
/// Times, compared without case) gives its installed names and its
/// metric-compatible or look-alike stand-ins; an unknown face is tried as
/// given. The Arial family follows, unless it is the face's own, since that
/// is what the font mapper picks for a missing face with no pitch or family.
///
/// @param face markup face name; "" adds only the Arial family
/// @param[out] out receives the face names
/// @param capacity number of entries `out` holds
/// @return how many names were written
int32_t face_candidates(const char* face, const char** out, int32_t capacity);

/// Returns the cached font of a markup font, opening it through the first
/// face of its chain the service has when there is no entry yet.
///
/// Two markup fonts share an entry when their height, weight, italic and
/// underline flags and the first 31 characters of their faces are equal. An
/// entry whose chain has no face the service can open keeps a null handle.
///
/// @param[in,out] fonts service fonts holding the cache
/// @param font markup font to look up
/// @return the entry, valid until the cache grows
const CachedFont& service_font(ServiceFonts& fonts, const MarkupFont& font);

/// Closes every opened font and empties the cache.
///
/// @param[in,out] fonts service fonts to clear
void service_fonts_clear(ServiceFonts& fonts);

/// Tests whether text can be measured.
///
/// @param fonts service fonts to test
/// @return true when there is a service (an `open` slot) or fallback fonts
bool service_fonts_usable(const ServiceFonts& fonts);

/// Returns a TextBackend that sets markup text through the host service.
///
/// A font with no installed face in its chain is measured and drawn with the
/// fallback FNT fonts, or not at all without them. Drawn pixels take the
/// colour where the coverage reaches k_coverage_threshold, and underlines use
/// the face's underline rows. The stock-font space comes from the fallback
/// fonts, else from the default markup font.
///
/// @param fonts service and fallback fonts; must outlive every document using
///     the backend
/// @return the backend
TextBackend service_text_backend(ServiceFonts* fonts);

} // namespace oa::boneyards::html
