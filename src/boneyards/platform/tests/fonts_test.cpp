// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The bundled-font text service: face map resolution, GDI line metrics,
// advances against the Windows faces' published widths, and mono, grey
// and simulated-style rasterisation.

#include "oa/platform/fonts.hpp"
#include "oa/platform/files.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H

namespace {

using namespace oa::platform;

int g_failures = 0;

void check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void check_equal(int32_t actual, int32_t expected, const char* what, int line) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL line %d: %s is %d, expected %d\n", line, what, actual, expected);
        ++g_failures;
    }
}

#define CHECK_EQ(actual, expected) check_equal((actual), (expected), #actual, __LINE__)

constexpr const char* k_sample = "Waiting for contents list to load.";
constexpr int32_t k_first_printable = 0x20;
constexpr int32_t k_printable_count = 0x5F;

// Arial's advance widths (2048 units per em) for 0x20..0x7E.
constexpr int32_t k_arial_regular_advances[k_printable_count] = {
    569,  569,  727,  1139, 1139, 1821, 1366, 391,  682,  682,  797,  1196, 569,  682,  569,  569,
    1139, 1139, 1139, 1139, 1139, 1139, 1139, 1139, 1139, 1139, 569,  569,  1196, 1196, 1196, 1139,
    2079, 1366, 1366, 1479, 1479, 1366, 1251, 1593, 1479, 569,  1024, 1366, 1139, 1706, 1479, 1593,
    1366, 1593, 1479, 1366, 1251, 1479, 1366, 1933, 1366, 1366, 1251, 569,  569,  569,  961,  1139,
    682,  1139, 1139, 1024, 1139, 1139, 569,  1139, 1139, 455,  455,  1024, 455,  1706, 1139, 1139,
    1139, 1139, 682,  1024, 569,  1139, 1024, 1479, 1024, 1024, 1024, 684,  532,  684,  1196,
};
constexpr int32_t k_arial_units_per_em = 2048;

// Widths Arial's device metrics (hdmx) give at 12, 13 and 16 pixels per em,
// summed over k_sample and over 0x20..0x7E.
struct DeviceWidths {
    int32_t em;
    int32_t sample;
    int32_t printable;
};

constexpr DeviceWidths k_arial_regular_device[] = {{12, 170, 598}, {13, 184, 644}, {16, 215, 780}};
constexpr DeviceWidths k_arial_bold_device[] = {{12, 179, 614}, {13, 201, 676}, {16, 239, 839}};

FontRequest request(
    const char* face,
    int32_t height,
    int32_t weight = k_weight_normal,
    bool italic = false,
    FontRenderMode mode = FontRenderMode::mono
) {
    return FontRequest{height, weight, italic, face, mode};
}

int32_t printable_advance(const Font* font) {
    int32_t width = 0;
    for (int32_t c = 0; c < k_printable_count; ++c)
        width += font_char_advance(font, static_cast<uint8_t>(k_first_printable + c));
    return width;
}

struct Raster {
    int32_t width = 0;
    int32_t height = 0;
    std::vector<uint8_t> coverage;

    uint8_t at(int32_t x, int32_t y) const {
        return coverage[static_cast<std::size_t>(y) * width + x];
    }
};

Raster render(const Font* font, const char* text) {
    const FontMetrics metrics = font_metrics(font);
    const int32_t length = static_cast<int32_t>(std::strlen(text));
    Raster raster;
    raster.width = font_text_advance(font, text, length) + metrics.height;
    raster.height = metrics.height;
    raster.coverage.assign(static_cast<std::size_t>(raster.width) * raster.height, 0);
    font_render(
        font, text, length, raster.coverage.data(), raster.width, raster.height, raster.width
    );
    return raster;
}

int32_t ink(const Raster& raster) {
    int32_t count = 0;
    for (const uint8_t value : raster.coverage)
        count += value != 0 ? 1 : 0;
    return count;
}

// Leftmost inked column of a row, or -1.
int32_t first_ink(const Raster& raster, int32_t row) {
    for (int32_t x = 0; x < raster.width; ++x)
        if (raster.at(x, row) != 0)
            return x;
    return -1;
}

void dump(const Raster& raster) {
    for (int32_t y = 0; y < raster.height; ++y) {
        for (int32_t x = 0; x < raster.width; ++x)
            std::fputc(raster.at(x, y) == 0 ? '.' : raster.at(x, y) == 0xFF ? '#' : '+', stderr);
        std::fputc('\n', stderr);
    }
}

void test_face_map() {
    FaceMatch match{};
    CHECK(face_resolve("Arial", 400, false, &match));
    CHECK(std::strcmp(match.file, "liberation/LiberationSans-Regular.ttf") == 0);
    CHECK(std::strcmp(match.family, "Liberation Sans") == 0);
    CHECK(!match.synthetic_bold && !match.synthetic_italic);
    CHECK(match.line_metrics != nullptr);

    // The markup's bold weight takes the real bold face, not a simulation.
    CHECK(face_resolve("ARIAL", 800, false, &match));
    CHECK(std::strcmp(match.file, "liberation/LiberationSans-Bold.ttf") == 0);
    CHECK(!match.synthetic_bold);
    CHECK(face_resolve("arial", 400, true, &match));
    CHECK(std::strcmp(match.file, "liberation/LiberationSans-Italic.ttf") == 0);
    CHECK(face_resolve("Arial", 700, true, &match));
    CHECK(std::strcmp(match.file, "liberation/LiberationSans-BoldItalic.ttf") == 0);
    CHECK(!match.synthetic_bold && !match.synthetic_italic);
    CHECK(face_resolve("Arial", 599, false, &match));
    CHECK(std::strcmp(match.file, "liberation/LiberationSans-Regular.ttf") == 0);
    CHECK(face_resolve("Arial", 600, false, &match));
    CHECK(std::strcmp(match.file, "liberation/LiberationSans-Bold.ttf") == 0);

    // Haettenschweiler has one style; the others are simulated.
    const char* saira = "saira-extracondensed/SairaExtraCondensed-ExtraBold.ttf";
    CHECK(face_resolve("Haettenschweiler", 400, false, &match));
    CHECK(std::strcmp(match.file, saira) == 0);
    CHECK(!match.synthetic_bold && !match.synthetic_italic);
    CHECK(match.line_metrics != nullptr);
    CHECK(face_resolve("HAETTENSCHWEILER", 800, false, &match));
    CHECK(std::strcmp(match.file, saira) == 0);
    CHECK(match.synthetic_bold && !match.synthetic_italic);
    CHECK(face_resolve("Haettenschweiler", 800, true, &match));
    CHECK(match.synthetic_bold && match.synthetic_italic);

    CHECK(face_resolve("MS Sans Serif", 400, false, &match));
    CHECK(std::strcmp(match.file, "liberation/LiberationSans-Regular.ttf") == 0);
    CHECK(match.line_metrics == nullptr);
    CHECK(face_resolve("Liberation Sans", 700, false, &match));
    CHECK(std::strcmp(match.file, "liberation/LiberationSans-Bold.ttf") == 0);
    CHECK(match.line_metrics == nullptr);
    CHECK(face_resolve("Saira ExtraCondensed ExtraBold", 400, false, &match));
    CHECK(std::strcmp(match.file, saira) == 0);

    // Times New Roman keeps a serif; the pages walk a glyph chain behind it.
    CHECK(face_resolve("Times New Roman", 400, false, &match));
    CHECK(std::strcmp(match.file, "liberation/LiberationSerif-Regular.ttf") == 0);
    CHECK(std::strcmp(match.family, "Liberation Serif") == 0);
    CHECK(match.line_metrics == nullptr);
    CHECK(face_resolve("times", 700, true, &match));
    CHECK(std::strcmp(match.file, "liberation/LiberationSerif-BoldItalic.ttf") == 0);
    CHECK(!match.synthetic_bold && !match.synthetic_italic);
    CHECK(face_resolve("Liberation Serif", 400, false, &match));
    CHECK(std::strcmp(match.file, "liberation/LiberationSerif-Regular.ttf") == 0);

    // Nothing is substituted for a face the map lacks; the caller walks its
    // own fallback chain.
    CHECK(!face_resolve("Impact", 400, false, &match));
    CHECK(!face_resolve("Arial Black", 400, false, &match));
    CHECK(!face_resolve("Aria", 400, false, &match));
    CHECK(!face_resolve("", 400, false, &match));
    CHECK(!face_resolve(nullptr, 400, false, &match));
    CHECK(!face_resolve("Saira ExtraCondensed ExtraBoldX", 400, false, &match));

    const char* const* files = nullptr;
    const int32_t count = bundled_font_files(&files);
    CHECK_EQ(count, 11);
    for (int32_t i = 0; i < count; ++i) {
        const std::string path = std::string(OA_FONTS_DIR) + "/" + files[i];
        std::FILE* file = oa::platform::open_file(path.c_str(), "rb");
        CHECK(file != nullptr);
        if (file != nullptr)
            std::fclose(file);
    }
}

void test_missing_fonts() {
    char error[256]{};
    CHECK(font_library_open("/nonexistent-oa-fonts", error, sizeof error) == nullptr);
    CHECK(std::strstr(error, "missing bundled font file") != nullptr);
    CHECK(std::strstr(error, "/nonexistent-oa-fonts/liberation/") != nullptr);
}

void test_line_metrics(FontLibrary* library) {
    struct Case {
        const char* face;
        int32_t height;
        int32_t weight;
        bool italic;
        int32_t em;
        int32_t ascent;
        int32_t descent;
    };

    const Case cases[] = {
        // The markup's sizes in Arial; GDI's values, not the stand-in's.
        {"ARIAL", -12, 400, false, 12, 12, 3},
        {"ARIAL", -14, 400, false, 14, 13, 3},
        {"ARIAL", -16, 400, false, 16, 15, 3},
        {"ARIAL", -18, 400, false, 18, 17, 4},
        {"ARIAL", -20, 400, false, 20, 19, 4},
        {"ARIAL", -16, 800, false, 16, 15, 4},
        {"ARIAL", -16, 400, true, 16, 16, 3},
        {"ARIAL", -10, 800, true, 10, 9, 2},
        // Past the table the Windows ascent and descent scale.
        {"ARIAL", -40, 400, false, 40, 36, 8},
        // The contents-list placeholder: tmHeight 19 at SIZE=+2.
        {"Haettenschweiler", -16, 400, false, 16, 15, 4},
        {"Haettenschweiler", -16, 800, true, 16, 15, 4},
        // Named by their own names the stand-ins report their own tables.
        {"Saira ExtraCondensed ExtraBold", -16, 400, false, 16, 18, 7},
        {"Liberation Sans", -12, 400, false, 12, 11, 3},
        // A positive height is the cell: the tallest em that fits.
        {"ARIAL", 18, 400, false, 16, 15, 3},
        {"ARIAL", 20, 400, false, 17, 15, 4},
        {"ARIAL", 0, 400, false, 16, 15, 3},
    };
    for (const Case& c : cases) {
        Font* font = font_open(library, request(c.face, c.height, c.weight, c.italic));
        CHECK(font != nullptr);
        if (font == nullptr)
            continue;
        const FontMetrics metrics = font_metrics(font);
        if (metrics.em != c.em || metrics.ascent != c.ascent || metrics.descent != c.descent) {
            std::fprintf(
                stderr,
                "FAIL %s %d w%d%s: em %d ascent %d descent %d, expected %d %d %d\n",
                c.face,
                c.height,
                c.weight,
                c.italic ? " italic" : "",
                metrics.em,
                metrics.ascent,
                metrics.descent,
                c.em,
                c.ascent,
                c.descent
            );
            ++g_failures;
        }
        CHECK_EQ(metrics.height, metrics.ascent + metrics.descent);
        CHECK_EQ(metrics.internal_leading, metrics.height - metrics.em);
        CHECK(metrics.underline_rows >= 1);
        CHECK(metrics.underline_top > metrics.ascent - metrics.underline_rows);
        CHECK(metrics.underline_top < metrics.height);
        font_close(font);
    }

    Font* arial = font_open(library, request("Arial", -16));
    const FontMetrics metrics = font_metrics(arial);
    // tmAveCharWidth: Arial's xAvgCharWidth 904 at 16 pixels per em.
    CHECK_EQ(metrics.average_width, 7);
    CHECK_EQ(metrics.external_leading, 1);
    CHECK_EQ(metrics.weight, 400);
    font_close(arial);
    Font* bold = font_open(library, request("Arial", -12, 800));
    CHECK_EQ(font_metrics(bold).weight, 700);
    // Arial Bold's xAvgCharWidth is 980.
    CHECK_EQ(font_metrics(bold).average_width, 6);
    font_close(bold);

    Font* times = font_open(library, request("Times New Roman", -12));
    CHECK(times != nullptr);
    if (times != nullptr) {
        const FontMetrics serif = font_metrics(times);
        CHECK(serif.em == 12);
        CHECK(serif.height == serif.ascent + serif.descent);
        CHECK(serif.height > 0);
        font_close(times);
    }
    CHECK(font_open(nullptr, request("Arial", -12)) == nullptr);
}

// The bundled regular face's own design widths (hmtx) are Arial's.
void test_design_advances() {
    FT_Library freetype = nullptr;
    FT_Face face = nullptr;
    const std::string path = std::string(OA_FONTS_DIR) + "/liberation/LiberationSans-Regular.ttf";
    CHECK(FT_Init_FreeType(&freetype) == 0);
    CHECK(FT_New_Face(freetype, path.c_str(), 0, &face) == 0);
    if (face == nullptr)
        return;
    CHECK_EQ(face->units_per_EM, k_arial_units_per_em);
    for (int32_t c = 0; c < k_printable_count; ++c) {
        FT_Fixed advance = 0;
        const FT_UInt glyph = FT_Get_Char_Index(face, static_cast<FT_ULong>(k_first_printable + c));
        CHECK(FT_Get_Advance(face, glyph, FT_LOAD_NO_SCALE, &advance) == 0);
        CHECK_EQ(static_cast<int32_t>(advance), k_arial_regular_advances[c]);
    }
    FT_Done_Face(face);
    FT_Done_FreeType(freetype);
}

// Hinted for monochrome output its whole-pixel widths are Arial's device
// widths.
void test_advances(FontLibrary* library) {
    for (const DeviceWidths& device : k_arial_regular_device) {
        Font* font = font_open(library, request("Arial", -device.em));
        CHECK_EQ(
            font_text_advance(font, k_sample, static_cast<int32_t>(std::strlen(k_sample))),
            device.sample
        );
        CHECK_EQ(printable_advance(font), device.printable);
        font_close(font);
    }
    for (const DeviceWidths& device : k_arial_bold_device) {
        Font* font = font_open(library, request("Arial", -device.em, 800));
        CHECK_EQ(
            font_text_advance(font, k_sample, static_cast<int32_t>(std::strlen(k_sample))),
            device.sample
        );
        CHECK_EQ(printable_advance(font), device.printable);
        font_close(font);
    }

    Font* font = font_open(library, request("Arial", -16));
    CHECK_EQ(font_text_advance(font, "", 0), 0);
    CHECK_EQ(
        font_text_advance(font, "ab", 2),
        font_char_advance(font, 'a') + font_char_advance(font, 'b')
    );
    // Windows-1252: the euro sign is at 0x80 with a figure's width.
    CHECK_EQ(font_char_advance(font, 0x80), font_char_advance(font, '0'));
    font_close(font);
}

// "H" at 16 pixels per em, hinted for monochrome output: the cell's top
// left at (0, 0), the baseline under row 14 (ascent 15), one column of
// left side bearing. Arial's own hinting gives the same 9x12 box.
constexpr const char* k_mono_h[] = {
    "...........",
    "...........",
    "...........",
    ".##.....##.",
    ".##.....##.",
    ".##.....##.",
    ".##.....##.",
    ".##.....##.",
    ".#########.",
    ".##.....##.",
    ".##.....##.",
    ".##.....##.",
    ".##.....##.",
    ".##.....##.",
    ".##.....##.",
    "...........",
    "...........",
    "...........",
};

void test_mono_glyph(FontLibrary* library) {
    Font* font = font_open(library, request("Arial", -16));
    const Raster raster = render(font, "H");
    CHECK_EQ(font_char_advance(font, 'H'), 11);
    CHECK_EQ(raster.height, 18);
    bool binary = true;
    for (const uint8_t value : raster.coverage)
        binary = binary && (value == 0 || value == 0xFF);
    CHECK(binary);
    bool same = true;
    for (int32_t y = 0; y < raster.height; ++y)
        for (int32_t x = 0; x < raster.width; ++x) {
            const bool expected = x < 11 && k_mono_h[y][x] == '#';
            same = same && (raster.at(x, y) != 0) == expected;
        }
    CHECK(same);
    if (!same)
        dump(raster);
    font_close(font);

    Font* grey = font_open(library, request("Arial", -16, 400, false, FontRenderMode::grey));
    const Raster smooth = render(grey, "O");
    bool partial = false;
    for (const uint8_t value : smooth.coverage)
        partial = partial || (value != 0 && value != 0xFF);
    CHECK(partial);
    font_close(grey);
}

void test_simulated_styles(FontLibrary* library) {
    Font* regular = font_open(library, request("Haettenschweiler", -16));
    Font* bold = font_open(library, request("Haettenschweiler", -16, 800));
    Font* italic = font_open(library, request("Haettenschweiler", -16, 400, true));
    for (int32_t c = 'A'; c <= 'z'; ++c) {
        const auto character = static_cast<uint8_t>(c);
        CHECK_EQ(font_char_advance(bold, character), font_char_advance(regular, character) + 1);
        CHECK_EQ(font_char_advance(italic, character), font_char_advance(regular, character));
    }
    const Raster plain = render(regular, "l");
    const Raster heavy = render(bold, "l");
    const Raster slanted = render(italic, "l");
    CHECK(ink(heavy) > ink(plain));
    // The overstrike grows to the right; the left edge stays.
    int32_t plain_bottom = plain.height - 1;
    while (plain_bottom > 0 && first_ink(plain, plain_bottom) < 0)
        --plain_bottom;
    CHECK_EQ(first_ink(heavy, plain_bottom), first_ink(plain, plain_bottom));
    // The slant leans the top of the stem right of its foot.
    int32_t top = 0;
    while (top < slanted.height && first_ink(slanted, top) < 0)
        ++top;
    int32_t bottom = slanted.height - 1;
    while (bottom > 0 && first_ink(slanted, bottom) < 0)
        --bottom;
    CHECK(first_ink(slanted, top) > first_ink(slanted, bottom));
    font_close(regular);
    font_close(bold);
    font_close(italic);
}

void test_render_clipping(FontLibrary* library) {
    Font* font = font_open(library, request("Arial", -16));
    constexpr int32_t k_width = 6;
    constexpr int32_t k_pitch = 10;
    constexpr int32_t k_guard = 0x5A;
    const FontMetrics metrics = font_metrics(font);
    std::vector<uint8_t> buffer(static_cast<std::size_t>(k_pitch) * metrics.height, k_guard);
    for (int32_t y = 0; y < metrics.height; ++y)
        std::memset(buffer.data() + static_cast<std::ptrdiff_t>(y) * k_pitch, 0, k_width);
    font_render(font, "WWW", 3, buffer.data(), k_width, metrics.height, k_pitch);
    bool guard_intact = true;
    for (int32_t y = 0; y < metrics.height; ++y)
        for (int32_t x = k_width; x < k_pitch; ++x)
            guard_intact =
                guard_intact && buffer[static_cast<std::size_t>(y) * k_pitch + x] == k_guard;
    CHECK(guard_intact);
    font_close(font);
}

} // namespace

int main() {
    test_face_map();
    test_missing_fonts();
    test_design_advances();
    char error[512]{};
    FontLibrary* library = font_library_open(OA_FONTS_DIR, error, sizeof error);
    if (library == nullptr) {
        std::fprintf(stderr, "FAIL: %s (run tools/bootstrap_fonts.py)\n", error);
        return 1;
    }
    test_line_metrics(library);
    test_advances(library);
    test_mono_glyph(library);
    test_simulated_styles(library);
    test_render_clipping(library);
    font_library_close(library);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("platform fonts ok");
    return 0;
}
