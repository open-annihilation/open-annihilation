// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Deterministic text backend and helpers shared by the markup tests.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "oa/boneyards/html.hpp"

namespace html_test {

using namespace oa::boneyards::html;
using oa::Palette;
using oa::PaletteEntry;
using oa::Surface;

inline int g_failures = 0;

inline void check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(cond) html_test::check((cond), #cond, __LINE__)

// Each character is |height|/2 wide (bold adds one pixel per run); a line is
// |height|+3 tall with an ascent of |height|; the stock space is 4x16.
struct DrawCall {
    std::string text;
    int32_t x;
    int32_t y;
    uint8_t color;
    int32_t size;
    bool bold;
    bool underline;
};

struct FixedBackend {
    std::vector<DrawCall> calls;
    int32_t extent_calls = 0;
};

inline int32_t size_of(const MarkupFont& font) {
    return font.height < 0 ? -font.height : font.height;
}

inline TextExtent
fixed_extent(void* user, const MarkupFont& font, const char* text, int32_t length) {
    ++static_cast<FixedBackend*>(user)->extent_calls;
    (void)text;
    const int32_t size = size_of(font);
    const int32_t bold = font.weight > k_weight_normal && length > 0 ? 1 : 0;
    return TextExtent{length * (size / 2) + bold, size + 3};
}

inline TextExtent fixed_space(void*) {
    return TextExtent{4, 16};
}

inline TextMetrics fixed_metrics(void*, const MarkupFont& font) {
    return TextMetrics{size_of(font) + 3, size_of(font)};
}

// Fills the run's extent box with the colour so snapshots see it.
inline void fixed_draw(
    void* user,
    Surface& surface,
    const MarkupFont& font,
    const char* text,
    int32_t length,
    int32_t x,
    int32_t y,
    uint8_t color
) {
    auto* backend = static_cast<FixedBackend*>(user);
    backend->calls.push_back(
        DrawCall{
            std::string(text, static_cast<std::size_t>(length)),
            x,
            y,
            color,
            size_of(font),
            font.weight > k_weight_normal,
            font.underline
        }
    );
    const TextExtent box = fixed_extent(user, font, text, length);
    --backend->extent_calls;
    for (int32_t row = y; row < y + box.cy; ++row)
        for (int32_t col = x; col < x + box.cx; ++col)
            if (row >= 0 && col >= 0 && row < surface.height && col < surface.width)
                surface.pixels[row * surface.pitch + col] = color;
}

inline TextBackend fixed_backend(FixedBackend* backend) {
    return TextBackend{backend, fixed_extent, fixed_space, fixed_metrics, fixed_draw};
}

// A grey ramp palette: entry i is (i, i, i).
inline Palette grey_palette() {
    Palette palette{};
    for (int i = 0; i < OA_PALETTE_COLORS; ++i)
        palette.entries[i] = PaletteEntry{
            static_cast<uint8_t>(i), static_cast<uint8_t>(i), static_cast<uint8_t>(i), 0
        };
    return palette;
}

} // namespace html_test
