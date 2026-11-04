// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Bitmap-font TextBackend (platform scaffolding).

#include "oa/boneyards/html_fnt.hpp"
#include "oa/boneyards/html_scripts.hpp"

#include <vector>

namespace oa::boneyards::html {

namespace {

constexpr unsigned char k_first_printable = 0x20;
constexpr uint8_t k_ink = 0x80;

const oa::formats::fnt::Font& pick(const FntFonts& fonts, const MarkupFont& style) {
    const int32_t size = style.height < 0 ? -style.height : style.height;
    if (fonts.large != nullptr && size >= fonts.large_from)
        return *fonts.large;
    return *fonts.regular;
}

bool is_bold(const MarkupFont& style) {
    return style.weight > k_weight_normal;
}

int32_t ascent_of(const oa::formats::fnt::Font& font) {
    return static_cast<int32_t>(oa::formats::fnt::line_height(font)) - 2;
}

int32_t em_of(const MarkupFont& style) {
    const int32_t size = style.height < 0 ? -style.height : style.height;
    return size > 0 ? size : 1;
}

bool chain_ready(const FntFonts& fonts) {
    return fonts.scripts != nullptr && fonts.scripts->lookup != nullptr;
}

// Width of a run whose characters the FNT font lacks come from the bitmap chain.
int32_t
chain_extent(const FntFonts& fonts, const MarkupFont& style, const char* text, int32_t length) {
    const auto& font = pick(fonts, style);
    std::vector<uint32_t> characters(static_cast<std::size_t>(length > 0 ? length : 0));
    const int32_t count =
        markup_characters(text, length, characters.empty() ? nullptr : characters.data(), length);
    int32_t width = 0;
    bool fnt_glyph = false;
    const int32_t em = em_of(style);
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t code_point = characters[static_cast<std::size_t>(i)];
        const int32_t byte = markup_cp1252_byte(code_point);
        if (byte >= k_first_printable && font.glyphs[byte]) {
            width += font.glyphs[byte]->width;
            fnt_glyph = true;
            continue;
        }
        if (!markup_character_visible(code_point))
            continue;
        ScriptGlyph glyph{};
        if (fonts.scripts->lookup(fonts.scripts->user, code_point, em, is_bold(style), &glyph))
            width += glyph.advance;
    }
    if (is_bold(style) && fnt_glyph)
        ++width;
    return width;
}

TextExtent extent(void* user, const MarkupFont& style, const char* text, int32_t length) {
    const auto& fonts = *static_cast<FntFonts*>(user);
    const auto& font = pick(fonts, style);
    if (chain_ready(fonts)) {
        return TextExtent{
            chain_extent(fonts, style, text, length),
            static_cast<int32_t>(oa::formats::fnt::line_height(font))
        };
    }
    auto width = static_cast<int32_t>(oa::formats::fnt::measure_text(
        font, std::string_view(text, static_cast<std::size_t>(length))
    ));
    if (is_bold(style) && length > 0)
        ++width;
    return TextExtent{width, static_cast<int32_t>(oa::formats::fnt::line_height(font))};
}

TextExtent stock_space(void* user) {
    const auto& fonts = *static_cast<FntFonts*>(user);
    const auto& glyph = fonts.regular->glyphs[' '];
    return TextExtent{
        glyph ? glyph->width : 0,
        static_cast<int32_t>(oa::formats::fnt::line_height(*fonts.regular))
    };
}

TextMetrics metrics(void* user, const MarkupFont& style) {
    const auto& font = pick(*static_cast<FntFonts*>(user), style);
    return TextMetrics{static_cast<int32_t>(oa::formats::fnt::line_height(font)), ascent_of(font)};
}

void plot(Surface& surface, int32_t x, int32_t y, uint8_t color) {
    if (x < 0 || y < 0 || x >= surface.width || y >= surface.height)
        return;
    surface.pixels[static_cast<std::ptrdiff_t>(y) * surface.pitch + x] = color;
}

void draw_script(
    Surface& surface, const ScriptGlyph& glyph, int32_t x, int32_t baseline, uint8_t color
) {
    if (glyph.coverage == nullptr || glyph.width <= 0 || glyph.rows <= 0)
        return;
    const int32_t top = baseline - glyph.top;
    for (int32_t row = 0; row < glyph.rows; ++row)
        for (int32_t col = 0; col < glyph.width; ++col)
            if (glyph.coverage[static_cast<std::size_t>(row) * glyph.width + col] >= k_ink)
                plot(surface, x + glyph.left + col, top + row, color);
}

void draw_chain(
    const FntFonts& fonts,
    Surface& surface,
    const MarkupFont& style,
    const char* text,
    int32_t length,
    int32_t x,
    int32_t y,
    uint8_t color
) {
    const auto& font = pick(fonts, style);
    const int32_t passes = is_bold(style) ? 2 : 1;
    const int32_t start = x;
    const int32_t baseline = y + ascent_of(font);
    const int32_t em = em_of(style);
    std::vector<uint32_t> characters(static_cast<std::size_t>(length > 0 ? length : 0));
    const int32_t count =
        markup_characters(text, length, characters.empty() ? nullptr : characters.data(), length);
    bool fnt_glyph = false;
    for (int32_t i = 0; i < count; ++i) {
        const uint32_t code_point = characters[static_cast<std::size_t>(i)];
        const int32_t byte = markup_cp1252_byte(code_point);
        if (byte >= k_first_printable && font.glyphs[byte]) {
            const auto& glyph = *font.glyphs[byte];
            fnt_glyph = true;
            if (byte != k_first_printable)
                for (int32_t pass = 0; pass < passes; ++pass)
                    for (int32_t row = 0; row < glyph.height; ++row)
                        for (int32_t col = 0; col < glyph.width; ++col)
                            if (glyph.coverage[static_cast<std::size_t>(row) * glyph.width + col])
                                plot(surface, x + col + pass, y + row, color);
            x += glyph.width;
            continue;
        }
        if (!markup_character_visible(code_point))
            continue;
        ScriptGlyph glyph{};
        if (!fonts.scripts->lookup(fonts.scripts->user, code_point, em, passes > 1, &glyph))
            continue;
        draw_script(surface, glyph, x, baseline, color);
        x += glyph.advance;
    }
    if (style.underline) {
        const int32_t line_y = baseline + 1;
        const int32_t end = x + (fnt_glyph ? passes - 1 : 0);
        for (int32_t px = start; px < end; ++px)
            plot(surface, px, line_y, color);
    }
}

void draw(
    void* user,
    Surface& surface,
    const MarkupFont& style,
    const char* text,
    int32_t length,
    int32_t x,
    int32_t y,
    uint8_t color
) {
    const auto& fonts = *static_cast<FntFonts*>(user);
    if (chain_ready(fonts)) {
        draw_chain(fonts, surface, style, text, length, x, y, color);
        return;
    }
    const auto& font = pick(fonts, style);
    const int32_t passes = is_bold(style) ? 2 : 1;
    const int32_t start = x;
    for (int32_t i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < k_first_printable || !font.glyphs[c])
            continue;
        const auto& glyph = *font.glyphs[c];
        if (c != k_first_printable)
            for (int32_t pass = 0; pass < passes; ++pass)
                for (int32_t row = 0; row < glyph.height; ++row)
                    for (int32_t col = 0; col < glyph.width; ++col)
                        if (glyph.coverage[static_cast<std::size_t>(row) * glyph.width + col])
                            plot(surface, x + col + pass, y + row, color);
        x += glyph.width;
    }
    if (style.underline) {
        const int32_t line_y = y + ascent_of(font) + 1;
        for (int32_t px = start; px < x + passes - 1; ++px)
            plot(surface, px, line_y, color);
    }
}

} // namespace

TextBackend fnt_text_backend(FntFonts* fonts) {
    return TextBackend{fonts, extent, stock_space, metrics, draw};
}

} // namespace oa::boneyards::html
