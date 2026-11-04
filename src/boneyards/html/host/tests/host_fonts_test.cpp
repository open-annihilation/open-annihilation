// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The markup text backend over the bundled fonts: face chains resolving to
// the shipped stand-ins, Windows line metrics and widths for the markup sizes,
// and aliased drawing in the text colour.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "oa/boneyards/html.hpp"
#include "oa/boneyards/html_host_fonts.hpp"
#include "oa/boneyards/html_scripts.hpp"
#include "oa/platform/fonts.hpp"
#include "oa/present/surface.hpp"

namespace {

using namespace oa::boneyards::html;

int g_failures = 0;

void check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

constexpr const char* k_sample = "Waiting for contents list to load.";
constexpr uint8_t k_text_color = 200;

MarkupFont markup_font(int32_t height, int32_t weight, const char* face, bool underline = false) {
    MarkupFont font{};
    font.height = height;
    font.weight = weight;
    font.underline = underline;
    std::snprintf(font.face, sizeof font.face, "%s", face);
    return font;
}

int32_t length_of(const char* text) {
    return static_cast<int32_t>(std::strlen(text));
}

void test_face_chains(ServiceFonts& fonts) {
    const MarkupFont body_font =
        markup_font(k_default_font_height, k_weight_normal, k_default_face);
    const CachedFont& body = service_font(fonts, body_font);
    CHECK(body.handle != nullptr);
    CHECK(std::strcmp(body.resolved, "Arial") == 0);
    CHECK(body.metrics.height == 15 && body.metrics.ascent == 12);

    // The contents-list placeholder, FACE="Haettenschweiler" SIZE="+2".
    const MarkupFont waiting_font = markup_font(-16, k_weight_normal, "Haettenschweiler");
    const CachedFont& waiting = service_font(fonts, waiting_font);
    CHECK(waiting.handle != nullptr);
    CHECK(std::strcmp(waiting.resolved, "Haettenschweiler") == 0);
    CHECK(waiting.metrics.height == 19 && waiting.metrics.ascent == 15);

    // Times keeps Liberation Serif, not the Arial stand-in.
    const MarkupFont times_font = markup_font(-12, k_weight_normal, "Times New Roman");
    const CachedFont& times = service_font(fonts, times_font);
    CHECK(std::strcmp(times.resolved, "Times New Roman") == 0);
    CHECK(times.metrics.height > 0 && times.metrics.ascent > 0);
    const MarkupFont unknown_font = markup_font(-12, k_weight_bold, "Wingdings");
    const CachedFont& unknown = service_font(fonts, unknown_font);
    CHECK(std::strcmp(unknown.resolved, "Arial") == 0);
    CHECK(unknown.metrics.height == 15);
}

void test_extents(ServiceFonts& fonts) {
    const TextBackend text = service_text_backend(&fonts);
    // Arial's device widths of the sample at 16 pixels per em.
    const TextExtent regular = text.extent(
        text.user, markup_font(-16, k_weight_normal, "ARIAL"), k_sample, length_of(k_sample)
    );
    CHECK(regular.cx == 215 && regular.cy == 18);
    const TextExtent bold = text.extent(
        text.user, markup_font(-16, k_weight_bold, "ARIAL"), k_sample, length_of(k_sample)
    );
    CHECK(bold.cx == 239 && bold.cy == 19);
    const TextMetrics metrics = text.metrics(text.user, markup_font(-20, k_weight_bold, "ARIAL"));
    CHECK(metrics.height == 24 && metrics.ascent == 19);
    // Without the FNT fonts the default markup font's space stands in for
    // the stock font's.
    const TextExtent space = text.stock_space(text.user);
    CHECK(space.cx == 3 && space.cy == 15);
}

void test_draw(ServiceFonts& fonts, oa::platform::FontLibrary* library) {
    const TextBackend text = service_text_backend(&fonts);
    constexpr int32_t k_width = 48;
    constexpr int32_t k_height = 28;
    constexpr int32_t k_x = 3;
    constexpr int32_t k_y = 4;
    std::vector<uint8_t> pixels(static_cast<std::size_t>(k_width) * k_height, 0);
    oa::Surface surface{};
    oa::present::init_surface(surface, k_width, k_height, k_width, pixels.data());
    const MarkupFont style = markup_font(-16, k_weight_normal, "ARIAL");
    text.draw(text.user, surface, style, "Hag", 3, k_x, k_y, k_text_color);

    // Every pixel the service covers takes the text colour; nothing else
    // is touched.
    oa::platform::Font* font = oa::platform::font_open(
        library,
        oa::platform::FontRequest{-16, 400, false, "Arial", oa::platform::FontRenderMode::mono}
    );
    const oa::platform::FontMetrics metrics = oa::platform::font_metrics(font);
    const int32_t cell_width = oa::platform::font_text_advance(font, "Hag", 3) + metrics.height;
    std::vector<uint8_t> coverage(static_cast<std::size_t>(cell_width) * metrics.height, 0);
    oa::platform::font_render(
        font, "Hag", 3, coverage.data(), cell_width, metrics.height, cell_width
    );
    bool same = true;
    int32_t drawn = 0;
    for (int32_t y = 0; y < k_height; ++y)
        for (int32_t x = 0; x < k_width; ++x) {
            const int32_t cx = x - k_x;
            const int32_t cy = y - k_y;
            const bool covered = cx >= 0 && cy >= 0 && cx < cell_width && cy < metrics.height &&
                                 coverage[static_cast<std::size_t>(cy) * cell_width + cx] != 0;
            const uint8_t pixel = pixels[static_cast<std::size_t>(y) * k_width + x];
            same = same && pixel == (covered ? k_text_color : 0);
            drawn += pixel != 0 ? 1 : 0;
        }
    CHECK(same);
    CHECK(drawn > 0);

    // An underline is a rule the width of the advance at the face's
    // underline rows.
    std::fill(pixels.begin(), pixels.end(), 0);
    const MarkupFont underlined = markup_font(-16, k_weight_normal, "ARIAL", true);
    text.draw(text.user, surface, underlined, "ii", 2, k_x, k_y, k_text_color);
    const int32_t advance = oa::platform::font_text_advance(font, "ii", 2);
    const int32_t rule = k_y + metrics.underline_top;
    bool ruled = true;
    for (int32_t x = k_x; x < k_x + advance; ++x)
        ruled = ruled && pixels[static_cast<std::size_t>(rule) * k_width + x] == k_text_color;
    CHECK(ruled);
    oa::platform::font_close(font);
}

// Grüße Привет Ελλάδα 中文 ✓ 🙂
// 中文 is in the engine's endonym face. 東京 is not, so this sample would
// draw no Chinese, Japanese or Korean without a language pack.
constexpr char k_scripts[] = "Gr\xc3\xbc\xc3\x9f"
                             "e "
                             "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 "
                             "\xce\x95\xce\xbb\xce\xbb\xce\xac\xce\xb4\xce\xb1 "
                             "\u4E2D\u6587 "
                             "\xe2\x9c\x93 "
                             "\xf0\x9f\x99\x82";

int32_t ink_of(const std::vector<uint8_t>& pixels, uint8_t color) {
    int32_t ink = 0;
    for (uint8_t pixel : pixels)
        ink += pixel == color ? 1 : 0;
    return ink;
}

// Every character of the sample is in the face or in the chain. U+E000 is in neither.
void test_no_missing_glyphs(oa::platform::FontLibrary* library) {
    const char* faces[] = {"Arial", "Times New Roman", "Haettenschweiler"};
    uint32_t characters[64]{};
    const int32_t count =
        markup_characters(k_scripts, static_cast<int32_t>(std::strlen(k_scripts)), characters, 64);
    CHECK(count > 10 && count <= 64);
    FallbackFonts* chain = page_script_fonts();
    CHECK(chain != nullptr);
    for (const char* face : faces) {
        oa::platform::Font* font = oa::platform::font_open(
            library,
            oa::platform::FontRequest{-12, 400, false, face, oa::platform::FontRenderMode::mono}
        );
        CHECK(font != nullptr);
        if (font == nullptr)
            continue;
        int32_t missing = 0;
        for (int32_t i = 0; i < count; ++i) {
            const uint32_t code_point = characters[i];
            if (!markup_character_visible(code_point))
                continue;
            if (oa::platform::font_has_character(font, code_point))
                continue;
            if (fallback_has(chain, code_point, false))
                continue;
            std::fprintf(stderr, "missing U+%04X in %s\n", code_point, face);
            ++missing;
        }
        CHECK(missing == 0);
        CHECK(!oa::platform::font_has_character(font, 0xE000));
        CHECK(!fallback_has(chain, 0xE000, false));
        oa::platform::font_close(font);
    }
}

int32_t draw_ink(ServiceFonts& fonts, const char* face, const char* text) {
    const TextBackend backend = service_text_backend(&fonts);
    constexpr int32_t k_width = 480;
    constexpr int32_t k_height = 48;
    std::vector<uint8_t> pixels(static_cast<std::size_t>(k_width) * k_height, 0);
    oa::Surface surface{};
    oa::present::init_surface(surface, k_width, k_height, k_width, pixels.data());
    backend.draw(
        backend.user,
        surface,
        markup_font(-12, k_weight_normal, face),
        text,
        static_cast<int32_t>(std::strlen(text)),
        2,
        2,
        k_text_color
    );
    return ink_of(pixels, k_text_color);
}

void test_mixed_scripts_page(ServiceFonts& fonts, oa::platform::FontLibrary* library) {
    test_no_missing_glyphs(library);
    // Count ink from the service, which is what a page paints.
    const char* faces[] = {"Arial", "Times New Roman", "Haettenschweiler"};
    for (const char* face : faces)
        CHECK(draw_ink(fonts, face, k_scripts) > 40);
    // A character nobody has does not draw a box.
    CHECK(draw_ink(fonts, "Arial", "\xee\x80\x80") == 0);

    const std::string page = std::string("<HTML><BODY>") + "<FONT FACE=\"Arial\">" + k_scripts +
                             "</FONT> " + "<FONT FACE=\"Times New Roman\">" + k_scripts +
                             "</FONT> " + "<FONT FACE=\"Haettenschweiler\">" + k_scripts +
                             "</FONT>" + "</BODY></HTML>";
    oa::Palette palette{};
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i)
        palette.entries[i] = oa::PaletteEntry{
            static_cast<uint8_t>(i), static_cast<uint8_t>(i), static_cast<uint8_t>(i), 0
        };
    Environment env{};
    env.text = service_text_backend(&fonts);
    env.palette = palette;
    Document doc;
    document_init(doc, env);
    document_set_width(doc, 720);
    CHECK(document_load(doc, page));
    CHECK(document_paint(doc));
    const oa::Surface* canvas = document_surface(doc);
    CHECK(canvas != nullptr && canvas->pixels != nullptr);
    int32_t ink = 0;
    if (canvas != nullptr && canvas->pixels != nullptr) {
        const int32_t rows = document_height(doc);
        for (int32_t y = 0; y < rows && y < canvas->height; ++y)
            for (int32_t x = 0; x < canvas->width; ++x)
                ink += canvas->pixels[static_cast<std::ptrdiff_t>(y) * canvas->pitch + x] !=
                               k_canvas_clear_index
                           ? 1
                           : 0;
    }
    CHECK(ink > 80);

    // Times is a serif: its "M" is not Arial's "M".
    const TextBackend backend = service_text_backend(&fonts);
    const int32_t arial =
        backend.extent(backend.user, markup_font(-16, k_weight_normal, "Arial"), "M", 1).cx;
    const int32_t times =
        backend.extent(backend.user, markup_font(-16, k_weight_normal, "Times New Roman"), "M", 1)
            .cx;
    CHECK(arial > 0 && times > 0 && arial != times);
}

/// Returns the canvas of a page laid out 200 pixels wide in the bundled faces.
///
/// @param fonts service fonts the page is set in
/// @param page markup
/// @return the canvas rows, each 200 bytes
std::vector<uint8_t> paint_page(ServiceFonts& fonts, const char* page) {
    Environment env{};
    env.text = service_text_backend(&fonts);
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i)
        env.palette.entries[i] = oa::PaletteEntry{
            static_cast<uint8_t>(i), static_cast<uint8_t>(i), static_cast<uint8_t>(i), 0
        };
    Document doc;
    document_init(doc, env);
    document_set_width(doc, 200);
    CHECK(document_load(doc, page));
    CHECK(document_paint(doc));
    std::vector<uint8_t> rows;
    const oa::Surface* canvas = document_surface(doc);
    if (canvas == nullptr)
        return rows;
    for (int32_t y = 0; y < canvas->height; ++y)
        rows.insert(
            rows.end(),
            canvas->pixels + static_cast<std::ptrdiff_t>(y) * canvas->pitch,
            canvas->pixels + static_cast<std::ptrdiff_t>(y) * canvas->pitch + canvas->width
        );
    return rows;
}

// Control characters draw nothing and take no room, in the face's own runs
// and in runs the chain helps with; no missing-glyph box is drawn for them.
// A page saved with CR LF line ends paints as one saved with LF.
void test_invisible_characters(ServiceFonts& fonts) {
    const TextBackend backend = service_text_backend(&fonts);
    const MarkupFont style = markup_font(-12, k_weight_normal, "Arial");
    const char* invisible[] = {"\r", "\t", "\n", "\x01", "\x1f", "\x7f"};
    for (const char* text : invisible) {
        CHECK(draw_ink(fonts, "Arial", text) == 0);
        CHECK(backend.extent(backend.user, style, text, 1).cx == 0);
    }
    CHECK(draw_ink(fonts, "Arial", "a\rb") == draw_ink(fonts, "Arial", "ab"));
    CHECK(
        backend.extent(backend.user, style, "a\rb", 3).cx ==
        backend.extent(backend.user, style, "ab", 2).cx
    );
    // A run with a character from the chain keeps its controls out too.
    CHECK(
        draw_ink(fonts, "Arial", "\r\xd0\x9f\t") == draw_ink(fonts, "Arial", "\xd0\x9f") &&
        draw_ink(fonts, "Arial", "\xd0\x9f") > 0
    );
    const std::vector<uint8_t> crlf = paint_page(
        fonts,
        "<BODY>\r\n<B>Title</B><BR>\r\n\r\n&nbsp;<P>\r\n&nbsp;&nbsp;Words\tand\r\nmore<BR>\r\n</"
        "BODY>"
    );
    const std::vector<uint8_t> lf =
        paint_page(fonts, "<BODY><B>Title</B><BR> &nbsp;<P> &nbsp;&nbsp;Words and more<BR></BODY>");
    CHECK(!crlf.empty() && crlf == lf);
}

// The FNT fonts stand in when the bundled faces cannot be opened. Their own
// glyphs stay; the chain supplies the rest, and a character it lacks draws nothing.
void test_fnt_chain_fallback() {
    oa::formats::fnt::Font fnt{};
    oa::formats::fnt::Glyph letter{};
    letter.width = 3;
    letter.height = 5;
    letter.coverage.assign(15, 1);
    fnt.glyphs['A'] = letter;
    ScriptFonts scripts{page_script_fonts(), fallback_lookup};
    FntFonts fnt_fonts{&fnt, nullptr, 0, &scripts};
    ServiceFonts bare{};
    bare.fallback = &fnt_fonts;
    const TextBackend text = service_text_backend(&bare);
    const MarkupFont style = markup_font(-12, k_weight_normal, "ARIAL");
    CHECK(text.extent(text.user, style, "A", 1).cx == 3);
    CHECK(
        text.extent(text.user, style, k_scripts, static_cast<int32_t>(std::strlen(k_scripts))).cx >
        3
    );
    CHECK(draw_ink(bare, "ARIAL", k_scripts) > 40);
    CHECK(draw_ink(bare, "ARIAL", "\xee\x80\x80") == 0);
    CHECK(draw_ink(bare, "Times New Roman", k_scripts) > 40);
    CHECK(draw_ink(bare, "Haettenschweiler", k_scripts) > 40);
}

} // namespace

FallbackFonts* open_page_chain() {
    const std::string dir = OA_TEXT_FONTS_DIR;
    const std::string dejavu = dir + "/DejaVuSans.ttf";
    const std::string bold = dir + "/DejaVuSans-Bold.ttf";
    const std::string emoji = dir + "/NotoEmoji.ttf";
    const FallbackFiles files{dejavu.c_str(), bold.c_str(), nullptr, emoji.c_str()};
    return fallback_fonts_open_files(files);
}

int main() {
    FallbackFonts* chain = open_page_chain();
    if (chain == nullptr) {
        std::fprintf(stderr, "FAIL: script chain did not open\n");
        return 1;
    }
    page_script_fonts_use(chain);

    char error[512]{};
    oa::platform::FontLibrary* library =
        oa::platform::font_library_open(OA_FONTS_DIR, error, sizeof error);
    if (library == nullptr) {
        std::fprintf(stderr, "FAIL: %s (run tools/bootstrap_fonts.py)\n", error);
        page_script_fonts_use(nullptr);
        fallback_fonts_close(chain);
        return 1;
    }
    CHECK(host_font_service(nullptr).open == nullptr);
    ServiceFonts fonts{};
    fonts.service = host_font_service(library);
    test_face_chains(fonts);
    test_extents(fonts);
    test_draw(fonts, library);
    test_mixed_scripts_page(fonts, library);
    test_invisible_characters(fonts);
    service_fonts_clear(fonts);
    CHECK(fonts.cache.empty());
    test_fnt_chain_fallback();
    page_script_fonts_use(nullptr);
    fallback_fonts_close(chain);
    oa::platform::font_library_close(library);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("boneyards host fonts ok");
    return 0;
}
