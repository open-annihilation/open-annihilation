// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Markup text service over the platform's bundled fonts (platform
// scaffolding).

#include "oa/boneyards/html_host_fonts.hpp"

#include <new>
#include <vector>

#include "oa/boneyards/html_scripts.hpp"
#include "oa/platform/fonts.hpp"

namespace oa::boneyards::html {
namespace {

namespace platform = oa::platform;

constexpr int32_t k_weight_bold_face = 600;
// Printable ASCII: the characters every page face holds.
constexpr uint8_t k_first_printable = 0x20;
constexpr uint8_t k_last_printable = 0x7e;

struct HostContext {
    platform::FontLibrary* library;
};

// The face the markup named, plus the shared chain for characters it lacks.
struct HostFont {
    platform::Font* primary;
    int32_t em;
    bool bold;
};

HostFont* font_of(void* font) {
    return static_cast<HostFont*>(font);
}

/// Tests whether a run is all printable ASCII, which the face draws whole.
///
/// A control character is not: the face's own glyph for it is the
/// missing-glyph box, so it goes the way of every character the face may
/// lack.
///
/// @param text run bytes
/// @param length number of bytes
/// @return true when every byte is a printable ASCII character
bool is_printable_ascii(const char* text, int32_t length) {
    for (int32_t i = 0; i < length; ++i) {
        const auto byte = static_cast<uint8_t>(text[i]);
        if (byte < k_first_printable || byte > k_last_printable)
            return false;
    }
    return true;
}

void plot_glyph(
    const ScriptGlyph& glyph,
    int32_t pen,
    int32_t ascent,
    uint8_t* coverage,
    int32_t width,
    int32_t height
) {
    if (glyph.coverage == nullptr || glyph.width <= 0 || glyph.rows <= 0)
        return;
    // Keep the chain's baseline on the page face's baseline. A picture that
    // would start above the cell is dropped to the cell's top.
    int32_t top = ascent - glyph.top;
    if (top < 0)
        top = 0;
    for (int32_t row = 0; row < glyph.rows; ++row) {
        const int32_t y = top + row;
        if (y < 0 || y >= height)
            continue;
        for (int32_t col = 0; col < glyph.width; ++col) {
            const int32_t x = pen + glyph.left + col;
            if (x < 0 || x >= width)
                continue;
            const uint8_t value = glyph.coverage[static_cast<std::size_t>(row) * glyph.width + col];
            uint8_t& pixel = coverage[static_cast<std::ptrdiff_t>(y) * width + x];
            if (value > pixel)
                pixel = value;
        }
    }
}

void* open(void* user, const FontRequest& request) {
    const platform::FontRequest host{
        request.height,
        request.weight,
        request.italic,
        request.face,
        platform::FontRenderMode::mono,
    };
    platform::Font* primary = platform::font_open(static_cast<HostContext*>(user)->library, host);
    if (primary == nullptr)
        return nullptr;
    auto* font = new (std::nothrow) HostFont{};
    if (font == nullptr) {
        platform::font_close(primary);
        return nullptr;
    }
    font->primary = primary;
    const platform::FontMetrics metrics = platform::font_metrics(primary);
    font->em = metrics.em > 0 ? metrics.em : 1;
    font->bold = request.weight >= k_weight_bold_face;
    return font;
}

void close(void*, void* font) {
    HostFont* host = font_of(font);
    if (host == nullptr)
        return;
    platform::font_close(host->primary);
    delete host;
}

FontMetrics metrics(void*, void* font) {
    const platform::FontMetrics host = platform::font_metrics(font_of(font)->primary);
    return FontMetrics{host.height, host.ascent, host.underline_top, host.underline_rows};
}

int32_t advance(void*, void* font, const char* text, int32_t length) {
    HostFont* host = font_of(font);
    if (text == nullptr || length <= 0)
        return 0;
    // A run of printable ASCII is the face's own advances, summed as the
    // service sums them. Anything else is read character by character:
    // controls take no room and characters the face lacks come from the chain.
    if (is_printable_ascii(text, length))
        return platform::font_text_advance(host->primary, text, length);
    std::vector<uint32_t> characters(static_cast<std::size_t>(length));
    const int32_t count = markup_characters(text, length, characters.data(), length);
    FallbackFonts* chain = page_script_fonts();
    int32_t width = 0;
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t code_point = characters[static_cast<std::size_t>(i)];
        if (!markup_character_visible(code_point))
            continue;
        if (platform::font_has_character(host->primary, code_point)) {
            width += platform::font_codepoint_advance(host->primary, code_point);
            continue;
        }
        ScriptGlyph glyph{};
        if (chain != nullptr && fallback_lookup(chain, code_point, host->em, host->bold, &glyph))
            width += glyph.advance;
    }
    return width;
}

void render(
    void*,
    void* font,
    const char* text,
    int32_t length,
    uint8_t* coverage,
    int32_t width,
    int32_t height
) {
    HostFont* host = font_of(font);
    if (text == nullptr || length <= 0 || coverage == nullptr)
        return;
    if (is_printable_ascii(text, length)) {
        platform::font_render(host->primary, text, length, coverage, width, height, width);
        return;
    }
    std::vector<uint32_t> characters(static_cast<std::size_t>(length));
    const int32_t count = markup_characters(text, length, characters.data(), length);
    const platform::FontMetrics face = platform::font_metrics(host->primary);
    FallbackFonts* chain = page_script_fonts();
    int32_t pen = 0;
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t code_point = characters[static_cast<std::size_t>(i)];
        if (!markup_character_visible(code_point))
            continue;
        if (platform::font_has_character(host->primary, code_point)) {
            platform::font_render_codepoint(
                host->primary, code_point, pen, coverage, width, height, width
            );
            pen += platform::font_codepoint_advance(host->primary, code_point);
            continue;
        }
        ScriptGlyph glyph{};
        if (chain == nullptr || !fallback_lookup(chain, code_point, host->em, host->bold, &glyph))
            continue;
        plot_glyph(glyph, pen, face.ascent, coverage, width, height);
        pen += glyph.advance;
    }
}

} // namespace

FontService host_font_service(oa::platform::FontLibrary* library) {
    if (library == nullptr)
        return FontService{};
    static HostContext context{};
    context.library = library;
    return FontService{&context, open, close, metrics, advance, render};
}

} // namespace oa::boneyards::html
