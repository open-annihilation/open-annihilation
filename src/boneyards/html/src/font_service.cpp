// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Host-font TextBackend (platform scaffolding).

#include <cctype>
#include <cstring>

#include "oa/boneyards/html_fonts.hpp"

namespace oa::boneyards::html {

namespace {

constexpr const char* k_arial_family[] = {
    "Arial", "Liberation Sans", "Arimo", "Helvetica", nullptr
};
constexpr const char* k_haettenschweiler_family[] = {
    "Haettenschweiler", "Saira ExtraCondensed ExtraBold", nullptr
};
constexpr const char* k_times_family[] = {
    "Times New Roman", "Liberation Serif", "Tinos", "Times", nullptr
};

struct FaceFamily {
    const char* name;
    const char* const* faces;
};

constexpr FaceFamily k_families[] = {
    {"ARIAL", k_arial_family},
    {"HELVETICA", k_arial_family},
    {"HAETTENSCHWEILER", k_haettenschweiler_family},
    {"TIMES NEW ROMAN", k_times_family},
    {"TIMES", k_times_family},
};

// Compared like the cache key: at most 31 characters.
constexpr std::size_t k_face_compare = k_face_bytes - 1;

bool same_face_ignoring_case(const char* a, const char* b) {
    for (std::size_t i = 0;; ++i) {
        const int ca = std::toupper(static_cast<unsigned char>(a[i]));
        const int cb = std::toupper(static_cast<unsigned char>(b[i]));
        if (ca != cb)
            return false;
        if (ca == 0)
            return true;
    }
}

const FaceFamily* family_of(const char* face) {
    for (const FaceFamily& family : k_families)
        if (same_face_ignoring_case(face, family.name))
            return &family;
    return nullptr;
}

int32_t append_faces(const char* const* faces, const char** out, int32_t count, int32_t capacity) {
    for (; *faces != nullptr && count < capacity; ++faces)
        out[count++] = *faces;
    return count;
}

bool same_key(const CachedFont& entry, const MarkupFont& font) {
    return entry.height == font.height && entry.weight == font.weight &&
           entry.italic == font.italic && entry.underline == font.underline &&
           std::strncmp(entry.face, font.face, k_face_compare) == 0;
}

void copy_face(char* out, const char* face) {
    const std::size_t length = ::strnlen(face, k_face_bytes - 1);
    std::memcpy(out, face, length);
    out[length] = '\0';
}

CachedFont open_font(const FontService& service, const MarkupFont& font) {
    CachedFont entry{};
    entry.height = font.height;
    entry.weight = font.weight;
    entry.italic = font.italic;
    entry.underline = font.underline;
    copy_face(entry.face, font.face);
    if (service.open == nullptr)
        return entry;
    const char* faces[k_max_face_candidates]{};
    const int32_t count = face_candidates(entry.face, faces, k_max_face_candidates);
    for (int32_t i = 0; i < count; ++i) {
        const FontRequest request{font.height, font.weight, font.italic, faces[i]};
        void* handle = service.open(service.user, request);
        if (handle == nullptr)
            continue;
        entry.handle = handle;
        copy_face(entry.resolved, faces[i]);
        entry.metrics = service.metrics(service.user, handle);
        if (entry.metrics.underline_rows < 1)
            entry.metrics.underline_rows = 1;
        break;
    }
    return entry;
}

void plot(Surface& surface, int32_t x, int32_t y, uint8_t color) {
    if (x < 0 || y < 0 || x >= surface.width || y >= surface.height)
        return;
    surface.pixels[static_cast<std::ptrdiff_t>(y) * surface.pitch + x] = color;
}

ServiceFonts& fonts_of(void* user) {
    return *static_cast<ServiceFonts*>(user);
}

TextBackend fallback_of(const ServiceFonts& fonts) {
    return fnt_text_backend(fonts.fallback);
}

TextExtent extent(void* user, const MarkupFont& style, const char* text, int32_t length) {
    ServiceFonts& fonts = fonts_of(user);
    const CachedFont& font = service_font(fonts, style);
    if (font.handle == nullptr) {
        if (fonts.fallback == nullptr)
            return TextExtent{};
        const TextBackend fnt = fallback_of(fonts);
        return fnt.extent(fnt.user, style, text, length);
    }
    const FontService& service = fonts.service;
    return TextExtent{
        service.advance(service.user, font.handle, text, length), font.metrics.height
    };
}

// The device context's stock font is not a markup font; without the FNT
// fonts the default markup font's space stands in for it.
TextExtent stock_space(void* user) {
    ServiceFonts& fonts = fonts_of(user);
    if (fonts.fallback != nullptr) {
        const TextBackend fnt = fallback_of(fonts);
        return fnt.stock_space(fnt.user);
    }
    MarkupFont style{};
    style.height = k_default_font_height;
    style.weight = k_weight_normal;
    copy_face(style.face, k_default_face);
    return extent(user, style, " ", 1);
}

TextMetrics metrics(void* user, const MarkupFont& style) {
    ServiceFonts& fonts = fonts_of(user);
    const CachedFont& font = service_font(fonts, style);
    if (font.handle == nullptr) {
        if (fonts.fallback == nullptr)
            return TextMetrics{};
        const TextBackend fnt = fallback_of(fonts);
        return fnt.metrics(fnt.user, style);
    }
    return TextMetrics{font.metrics.height, font.metrics.ascent};
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
    ServiceFonts& fonts = fonts_of(user);
    const CachedFont& font = service_font(fonts, style);
    if (font.handle == nullptr) {
        if (fonts.fallback != nullptr) {
            const TextBackend fnt = fallback_of(fonts);
            fnt.draw(fnt.user, surface, style, text, length, x, y, color);
        }
        return;
    }
    const FontService& service = fonts.service;
    const int32_t advance = service.advance(service.user, font.handle, text, length);
    const int32_t cell = font.metrics.height;
    // The face's own glyphs fit the cell. A fallback glyph may stand half a
    // cell below that, and italic or bold may overhang the advance by a cell.
    const int32_t height = cell > 0 ? cell + cell / 2 : 0;
    const int32_t width = advance + cell;
    if (width <= 0 || height <= 0)
        return;
    fonts.coverage.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    service.render(service.user, font.handle, text, length, fonts.coverage.data(), width, height);
    for (int32_t row = 0; row < height; ++row)
        for (int32_t col = 0; col < width; ++col)
            if (fonts.coverage[static_cast<std::size_t>(row) * width + col] >= k_coverage_threshold)
                plot(surface, x + col, y + row, color);
    if (font.underline)
        for (int32_t row = 0; row < font.metrics.underline_rows; ++row)
            for (int32_t col = 0; col < advance; ++col)
                plot(surface, x + col, y + font.metrics.underline_top + row, color);
}

} // namespace

int32_t face_candidates(const char* face, const char** out, int32_t capacity) {
    int32_t count = 0;
    const FaceFamily* family = family_of(face);
    if (family != nullptr)
        count = append_faces(family->faces, out, count, capacity);
    else if (face[0] != '\0' && count < capacity)
        out[count++] = face;
    if (family == nullptr || family->faces != k_arial_family)
        count = append_faces(k_arial_family, out, count, capacity);
    return count;
}

const CachedFont& service_font(ServiceFonts& fonts, const MarkupFont& font) {
    for (const CachedFont& entry : fonts.cache)
        if (same_key(entry, font))
            return entry;
    fonts.cache.push_back(open_font(fonts.service, font));
    return fonts.cache.back();
}

void service_fonts_clear(ServiceFonts& fonts) {
    for (const CachedFont& entry : fonts.cache)
        if (entry.handle != nullptr && fonts.service.close != nullptr)
            fonts.service.close(fonts.service.user, entry.handle);
    fonts.cache.clear();
}

bool service_fonts_usable(const ServiceFonts& fonts) {
    return fonts.service.open != nullptr || fonts.fallback != nullptr;
}

TextBackend service_text_backend(ServiceFonts* fonts) {
    return TextBackend{fonts, extent, stock_space, metrics, draw};
}

} // namespace oa::boneyards::html
