// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The host-font text backend over a synthetic text service: the font
// requests markup sizes and faces turn into, the font cache key, the face
// fallback chains, the FNT fallback and the coverage threshold.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "oa/boneyards/html_fonts.hpp"
#include "oa/present/surface.hpp"
#include "test_support.hpp"

using namespace html_test;

namespace {

// Each installed face's em is |height| pixels (or 4/5 of a positive cell
// height); characters are em/2 wide, the cell em + em/4 tall with an ascent
// of em, the underline one row at em + 1.
struct SyntheticFont {
    std::string face;
    int32_t em;
    int32_t weight;
    bool italic;
};

struct SyntheticService {
    std::vector<std::string> installed;
    std::vector<FontRequest> requests; // face pointers are not kept
    std::vector<std::string> request_faces;
    SyntheticFont fonts[32];
    int32_t opened = 0;
    int32_t closed = 0;
};

void* synthetic_open(void* user, const FontRequest& request) {
    auto* service = static_cast<SyntheticService*>(user);
    service->requests.push_back(request);
    service->request_faces.emplace_back(request.face);
    for (const std::string& face : service->installed)
        if (face == request.face && service->opened < 32) {
            SyntheticFont& font = service->fonts[service->opened++];
            font.face = face;
            font.em = request.height < 0 ? -request.height : request.height * 4 / 5;
            font.weight = request.weight;
            font.italic = request.italic;
            return &font;
        }
    return nullptr;
}

void synthetic_close(void* user, void*) {
    ++static_cast<SyntheticService*>(user)->closed;
}

FontMetrics synthetic_metrics(void*, void* handle) {
    const auto* font = static_cast<const SyntheticFont*>(handle);
    return FontMetrics{font->em + font->em / 4, font->em, font->em + 1, 0};
}

int32_t synthetic_advance(void*, void* handle, const char*, int32_t length) {
    return length * (static_cast<const SyntheticFont*>(handle)->em / 2);
}

// Columns repeat coverage 0x7f, 0x80, 0xff, 0x00 over the advance, on the
// rows above the baseline.
void synthetic_render(
    void*,
    void* handle,
    const char*,
    int32_t length,
    uint8_t* coverage,
    int32_t width,
    int32_t height
) {
    const auto* font = static_cast<const SyntheticFont*>(handle);
    const uint8_t pattern[4] = {0x7f, 0x80, 0xff, 0x00};
    const int32_t advance = length * (font->em / 2);
    for (int32_t row = 0; row < font->em && row < height; ++row)
        for (int32_t col = 0; col < advance && col < width; ++col)
            coverage[row * width + col] = pattern[col % 4];
}

FontService synthetic_service(SyntheticService* service) {
    return FontService{
        service,
        synthetic_open,
        synthetic_close,
        synthetic_metrics,
        synthetic_advance,
        synthetic_render
    };
}

MarkupFont markup_font(int32_t height, int32_t weight, const char* face) {
    MarkupFont font{};
    font.height = height;
    font.weight = weight;
    std::snprintf(font.face, sizeof font.face, "%s", face);
    return font;
}

// A three-glyph FNT font: space 3 wide, 'I' 10 high (line height 12), 'a'
// 5 wide and fully inked.
oa::formats::fnt::Font synthetic_fnt() {
    oa::formats::fnt::Font font{};
    oa::formats::fnt::Glyph space{};
    space.width = 3;
    space.height = 1;
    space.coverage.assign(3, 0);
    font.glyphs[' '] = space;
    oa::formats::fnt::Glyph tall{};
    tall.width = 2;
    tall.height = 10;
    tall.coverage.assign(20, 1);
    font.glyphs['I'] = tall;
    oa::formats::fnt::Glyph a{};
    a.width = 5;
    a.height = 8;
    a.coverage.assign(40, 1);
    font.glyphs['a'] = a;
    return font;
}

void test_face_candidates() {
    const char* faces[k_max_face_candidates]{};
    int32_t n = face_candidates("ARIAL", faces, k_max_face_candidates);
    CHECK(n == 4);
    CHECK(std::strcmp(faces[0], "Arial") == 0);
    CHECK(std::strcmp(faces[1], "Liberation Sans") == 0);

    n = face_candidates("Haettenschweiler", faces, k_max_face_candidates);
    CHECK(n == 6);
    CHECK(std::strcmp(faces[0], "Haettenschweiler") == 0);
    CHECK(std::strcmp(faces[1], "Saira ExtraCondensed ExtraBold") == 0);
    CHECK(std::strcmp(faces[2], "Arial") == 0);

    n = face_candidates("times new roman", faces, k_max_face_candidates);
    CHECK(n == 8);
    CHECK(std::strcmp(faces[0], "Times New Roman") == 0);
    CHECK(std::strcmp(faces[4], "Arial") == 0);

    // An unknown face is tried as given before the Arial family.
    n = face_candidates("Comic Sans MS", faces, k_max_face_candidates);
    CHECK(n == 5);
    CHECK(std::strcmp(faces[0], "Comic Sans MS") == 0);
    CHECK(std::strcmp(faces[1], "Arial") == 0);

    n = face_candidates("", faces, k_max_face_candidates);
    CHECK(n == 4 && std::strcmp(faces[0], "Arial") == 0);

    n = face_candidates("Haettenschweiler", faces, 1);
    CHECK(n == 1);
}

// FONT SIZE=+n adds 2n pixels to the enclosing font's character height
// (handle_font) over the default -12 (new_default_font); an absolute SIZE=n
// is 2n+6. The service is asked for exactly those LOGFONT heights.
void test_size_mapping() {
    SyntheticService service;
    service.installed = {"Arial"};
    ServiceFonts fonts{};
    fonts.service = synthetic_service(&service);
    Environment env{};
    env.text = service_text_backend(&fonts);
    env.palette = grey_palette();
    Document doc;
    document_init(doc, env);
    document_set_width(doc, 400);
    CHECK(document_load(
        doc,
        "<HTML><BODY>e <FONT SIZE=\"+1\">a</FONT> <FONT SIZE=\"+2\">b</FONT> "
        "<FONT SIZE=\"+3\">c</FONT> <FONT size=\"+4\">d</FONT> <FONT SIZE=3>f</FONT> "
        "<FONT SIZE=\"+4\"><B>g</B></FONT></BODY></HTML>"
    ));
    CHECK(document_paint(doc));

    // -12 serves e and f (and the stock space); +1..+4 are -14..-20; the
    // bold +4 is a sixth font.
    CHECK(service.requests.size() == 6);
    const int32_t expected[6] = {-12, -14, -16, -18, -20, -20};
    for (std::size_t i = 0; i < service.requests.size() && i < 6; ++i) {
        CHECK(service.requests[i].height == expected[i]);
        CHECK(service.request_faces[i] == "Arial");
        CHECK(service.requests[i].weight == (i == 5 ? k_weight_bold : k_weight_normal));
        CHECK(!service.requests[i].italic);
    }
    CHECK(fonts.cache.size() == 6);

    // Each run is measured in its own font: d is 20/2 wide and 25 tall.
    for (const TextRun& run : doc.texts) {
        const int32_t size = -doc.fonts[run.font].height;
        CHECK(run.cx == (size / 2) * static_cast<int32_t>(std::strlen(run.text)));
        CHECK(run.cy == size + size / 4);
        CHECK(run.ascent == size);
    }
    service_fonts_clear(fonts);
    CHECK(service.closed == 6);
    CHECK(fonts.cache.empty());
}

// The key is the LOGFONT height, weight, italic and underline flags and the
// face compared case-sensitively over 31 characters, as the game's font
// cache matches.
void test_cache_key() {
    SyntheticService service;
    service.installed = {"Arial"};
    ServiceFonts fonts{};
    fonts.service = synthetic_service(&service);

    MarkupFont base = markup_font(-12, k_weight_normal, "ARIAL");
    const CachedFont& first = service_font(fonts, base);
    CHECK(first.handle != nullptr);
    CHECK(std::strcmp(first.resolved, "Arial") == 0);
    CHECK(first.metrics.height == 15 && first.metrics.ascent == 12);
    CHECK(first.metrics.underline_rows == 1);
    service_font(fonts, base);
    base.colorref = 0x0000ff;
    base.dirty = true;
    service_font(fonts, base);
    CHECK(service.requests.size() == 1);

    MarkupFont underlined = base;
    underlined.underline = true;
    service_font(fonts, underlined);
    MarkupFont italic = base;
    italic.italic = true;
    service_font(fonts, italic);
    MarkupFont lower = markup_font(-12, k_weight_normal, "arial");
    service_font(fonts, lower);
    CHECK(service.requests.size() == 4);
    CHECK(fonts.cache.size() == 4);
    CHECK(service.requests[2].italic);
    CHECK(std::strcmp(fonts.cache[3].resolved, "Arial") == 0);
}

// A missing face walks its family, then the Arial family; with nothing
// installed the FNT fonts measure and draw; with neither, nothing does.
void test_face_fallback() {
    SyntheticService service;
    service.installed = {"Saira ExtraCondensed ExtraBold", "Arial"};
    ServiceFonts fonts{};
    fonts.service = synthetic_service(&service);

    const MarkupFont hatt_font = markup_font(-16, k_weight_normal, "Haettenschweiler");
    const CachedFont& hatt = service_font(fonts, hatt_font);
    CHECK(hatt.handle != nullptr);
    CHECK(std::strcmp(hatt.resolved, "Saira ExtraCondensed ExtraBold") == 0);
    CHECK(service.request_faces.size() == 2);
    CHECK(service.request_faces[0] == "Haettenschweiler");
    CHECK(service.requests[1].height == -16);

    const MarkupFont unknown_font = markup_font(-12, k_weight_normal, "Wingdings");
    const CachedFont& unknown = service_font(fonts, unknown_font);
    CHECK(std::strcmp(unknown.resolved, "Arial") == 0);
    CHECK(service.request_faces[2] == "Wingdings");
    CHECK(service.request_faces[3] == "Arial");

    // Nothing installed: the failure is cached and the FNT font stands in.
    SyntheticService empty;
    oa::formats::fnt::Font fnt = synthetic_fnt();
    FntFonts fnt_fonts{&fnt, nullptr, 0};
    ServiceFonts bare{};
    bare.service = synthetic_service(&empty);
    bare.fallback = &fnt_fonts;
    const TextBackend text = service_text_backend(&bare);
    const MarkupFont style = markup_font(-12, k_weight_normal, "ARIAL");
    const TextExtent extent = text.extent(text.user, style, "aa", 2);
    CHECK(extent.cx == 10 && extent.cy == 12);
    text.extent(text.user, style, "a", 1);
    CHECK(empty.requests.size() == 4);
    CHECK(bare.cache.size() == 1 && bare.cache[0].handle == nullptr);
    CHECK(text.metrics(text.user, style).height == 12);
    const TextExtent space = text.stock_space(text.user);
    CHECK(space.cx == 3 && space.cy == 12);
    std::vector<uint8_t> pixels(16 * 16, k_canvas_clear_index);
    Surface surface{};
    oa::present::init_surface(surface, 16, 16, 16, pixels.data());
    text.draw(text.user, surface, style, "a", 1, 1, 1, 200);
    CHECK(pixels[1 * 16 + 1] == 200 && pixels[8 * 16 + 5] == 200 && pixels[9 * 16 + 1] == 9);
    CHECK(service_fonts_usable(bare));

    // No service at all.
    ServiceFonts fnt_only{};
    fnt_only.fallback = &fnt_fonts;
    CHECK(service_fonts_usable(fnt_only));
    const TextBackend plain = service_text_backend(&fnt_only);
    CHECK(plain.extent(plain.user, style, "a", 1).cx == 5);

    ServiceFonts none{};
    CHECK(!service_fonts_usable(none));
    const TextBackend nothing = service_text_backend(&none);
    CHECK(nothing.extent(nothing.user, style, "a", 1).cx == 0);
    CHECK(nothing.metrics(nothing.user, style).height == 0);
}

// Unsmoothed text: a pixel takes the colour index from coverage 0x80 up;
// the underline spans the advance at the face's underline row.
void test_coverage_threshold() {
    SyntheticService service;
    service.installed = {"Arial"};
    ServiceFonts fonts{};
    fonts.service = synthetic_service(&service);
    const TextBackend text = service_text_backend(&fonts);
    std::vector<uint8_t> pixels(32 * 24, k_canvas_clear_index);
    Surface surface{};
    oa::present::init_surface(surface, 32, 24, 32, pixels.data());
    MarkupFont style = markup_font(-8, k_weight_normal, "ARIAL");
    style.underline = true;
    text.draw(text.user, surface, style, "abc", 3, 2, 3, 200);
    // em 8: 12 columns of pattern on rows 3..10, underline on row 3 + 9.
    int32_t inked = 0;
    for (int32_t row = 3; row < 11; ++row)
        for (int32_t col = 0; col < 12; ++col) {
            const uint8_t expect = (col % 4 == 1 || col % 4 == 2) ? 200 : 9;
            CHECK(pixels[row * 32 + 2 + col] == expect);
            inked += pixels[row * 32 + 2 + col] == 200;
        }
    CHECK(inked == 48);
    for (int32_t col = 0; col < 12; ++col)
        CHECK(pixels[12 * 32 + 2 + col] == 200);
    CHECK(pixels[12 * 32 + 14] == 9);
    CHECK(pixels[12 * 32 + 1] == 9);
    CHECK(pixels[11 * 32 + 4] == 9);
    CHECK(pixels[2 * 32 + 3] == 9);
}

// Characters the FNT font lacks come from the bitmap chain. A character the
// chain also lacks draws nothing: no missing-glyph box.
struct FakeChain {
    int32_t looked = 0;
};

bool fake_lookup(void* user, uint32_t code_point, int32_t, bool, ScriptGlyph* out) {
    ++static_cast<FakeChain*>(user)->looked;
    // Four by three, fully covered. Lives for the process.
    static const uint8_t block[] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    if (code_point != 0x0391)
        return false;
    out->advance = 4;
    out->left = 0;
    out->top = 6;
    out->width = 4;
    out->rows = 3;
    out->coverage = block;
    return true;
}

void test_fnt_script_fallback() {
    oa::formats::fnt::Font fnt = synthetic_fnt();
    FakeChain chain;
    ScriptFonts scripts{&chain, fake_lookup};
    FntFonts fnt_fonts{&fnt, nullptr, 0, &scripts};
    SyntheticService empty;
    ServiceFonts bare{};
    bare.service = synthetic_service(&empty);
    bare.fallback = &fnt_fonts;
    const TextBackend text = service_text_backend(&bare);
    const MarkupFont style = markup_font(-12, k_weight_normal, "ARIAL");

    // The FNT font's own 'a' is kept, and the chain is not asked.
    CHECK(text.extent(text.user, style, "aa", 2).cx == 10);
    CHECK(chain.looked == 0);
    std::vector<uint8_t> own(16 * 16, k_canvas_clear_index);
    Surface own_surface{};
    oa::present::init_surface(own_surface, 16, 16, 16, own.data());
    text.draw(text.user, own_surface, style, "a", 1, 1, 1, 200);
    CHECK(own[1 * 16 + 1] == 200 && own[8 * 16 + 5] == 200 && own[9 * 16 + 1] == 9);
    CHECK(chain.looked == 0);

    // U+0391 (Greek capital alpha) is not in the FNT font.
    const char alpha[] = "\xce\x91";
    CHECK(text.extent(text.user, style, alpha, 2).cx == 4);
    std::vector<uint8_t> pixels(16 * 16, k_canvas_clear_index);
    Surface surface{};
    oa::present::init_surface(surface, 16, 16, 16, pixels.data());
    text.draw(text.user, surface, style, alpha, 2, 1, 1, 200);
    int32_t ink = 0;
    for (uint8_t pixel : pixels)
        ink += pixel == 200 ? 1 : 0;
    CHECK(ink == 12);

    // A character no face has leaves the canvas untouched.
    const char missing[] = "\xee\x80\x80";
    const int32_t looked = chain.looked;
    CHECK(text.extent(text.user, style, missing, 3).cx == 0);
    std::fill(pixels.begin(), pixels.end(), k_canvas_clear_index);
    text.draw(text.user, surface, style, missing, 3, 1, 1, 200);
    bool clear = true;
    for (uint8_t pixel : pixels)
        clear = clear && pixel == k_canvas_clear_index;
    CHECK(clear);
    CHECK(chain.looked > looked);
}

} // namespace

int main() {
    test_face_candidates();
    test_size_mapping();
    test_cache_key();
    test_face_fallback();
    test_coverage_threshold();
    test_fnt_script_fallback();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("font service tests passed");
    return 0;
}
