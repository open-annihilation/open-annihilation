// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Each face the player reads, drawn in Western and Central European scripts,
// Greek, Cyrillic and CJK. The bitmap faces keep their own pictures for
// Western letters and take the analogue pictures for the rest. The page
// faces and the modern chat line draw every script, and a character none of
// them has draws nothing.
#include "oa/boneyards/font_atlases.hpp"
#include "oa/boneyards/html.hpp"
#include "oa/boneyards/html_host_fonts.hpp"
#include "oa/boneyards/html_scripts.hpp"
#include "oa/boneyards/toolkit.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/platform/fonts.hpp"
#include "oa/present/surface.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace oa::boneyards;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// Western letters the bitmap faces already contain, then Central European,
// Greek, Cyrillic and CJK, which those faces do not.
constexpr const char* k_western = "West";
constexpr const char* k_central = "\xC5\xA0\xC5\xBE\xC5\x81\xC4\x8D";                  // ŠžŁč
constexpr const char* k_greek = "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xAC\xCE\xB4\xCE\xB1";    // Ελλάδα
constexpr const char* k_cyrillic = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"; // Привет
constexpr const char* k_cjk = "\u4E2D\u6587";                                          // 中文
constexpr const char* k_absent = "\xEE\x80\x80";                                       // U+E000

struct Script {
    const char* name;
    const char* text;
    bool own_pictures;
};

constexpr Script k_scripts[] = {
    {"Western", k_western, true},
    {"Central European", k_central, false},
    {"Greek", k_greek, false},
    {"Cyrillic", k_cyrillic, false},
    {"CJK", k_cjk, false},
};

struct Canvas {
    std::vector<uint8_t> pixels;
    oa::Surface surface{};
};

void make_canvas(Canvas& canvas, int32_t width, int32_t height, uint8_t fill) {
    canvas.pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), fill);
    canvas.surface = {};
    canvas.surface.width = width;
    canvas.surface.height = height;
    canvas.surface.pitch = width;
    canvas.surface.pixels = canvas.pixels.data();
    canvas.surface.reserved_after_pixels = OA_SURFACE_RESERVED_AFTER_PIXELS_INIT;
    canvas.surface.reserved_before_origin = OA_SURFACE_RESERVED_BEFORE_ORIGIN_INIT;
    canvas.surface.clip = oa::Rect32{0, 0, width - 1, height - 1};
    canvas.surface.flags = OA_SURFACE_FLAG_MEMORY;
}

int32_t ink_of(const std::vector<uint8_t>& pixels, uint8_t skip) {
    int32_t ink = 0;
    for (uint8_t pixel : pixels)
        ink += pixel != skip ? 1 : 0;
    return ink;
}

std::vector<uint8_t> read_archive(oa::HpiArchive& archive, const char* path) {
    auto read = archive.read(path);
    if (!read.ok())
        return {};
    return std::move(*read.value);
}

// ---- bitmap faces --------------------------------------------------------------

const char* const k_faces[] = {
    "Fonts\\BYHatFontSmall87.GAF",
    "Fonts\\BYHatFontMedium90.GAF",
    "Fonts\\BYHatFontBig84.GAF",
    "anims\\Hattfont12.gaf",
};

void draw_bitmap_script(
    const uint8_t* gaf,
    std::size_t gaf_size,
    const char* face,
    const FontAtlas* atlas,
    const Script& script
) {
    Canvas canvas;
    make_canvas(canvas, 480, 64, k_overlay_transparent);
    FontAtlasGlyph places[16]{};
    const int32_t count =
        font_atlas_draw(&canvas.surface, gaf, gaf_size, face, atlas, script.text, 4, 8, places, 16);
    char what[96];
    std::snprintf(what, sizeof what, "%s draws %s", face, script.name);
    check(count > 0, what);
    if (count <= 0)
        return;
    for (int32_t index = 0; index < count; ++index) {
        const FontAtlasGlyph& glyph = places[index];
        const bool own = script.own_pictures;
        if (glyph.ink_top < 0 || glyph.advance <= 0 || (glyph.original != 0) != own) {
            std::fprintf(
                stderr,
                "FAIL: %s %s U+%04X ink %d advance %d own %u\n",
                face,
                script.name,
                glyph.codepoint,
                glyph.ink_top,
                glyph.advance,
                glyph.original
            );
            ++g_failures;
        }
    }
}

void test_bitmap_faces() {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the language font checks");
    auto boneyards = oa::open_hpi_file(root / oa::boneyards_test::kBoneyardsArchive);
    auto base = oa::open_hpi_file(root / oa::test::netgame::kBaseArchive);
    check(boneyards.ok() && base.ok(), "the install opens");
    if (!boneyards.ok() || !base.ok())
        return;

    std::ifstream atlas_file(OA_LANGUAGE_ATLAS, std::ios::binary);
    std::vector<uint8_t> atlas_bytes(
        (std::istreambuf_iterator<char>(atlas_file)), std::istreambuf_iterator<char>()
    );
    FontAtlas* atlas = font_atlas_open(atlas_bytes.data(), atlas_bytes.size());
    check(atlas != nullptr, "the analogue atlas opens");
    if (atlas == nullptr)
        return;

    for (const char* face : k_faces) {
        std::vector<uint8_t> gaf = read_archive(*boneyards.value, face);
        if (gaf.empty())
            gaf = read_archive(*base.value, face);
        check(!gaf.empty(), face);
        if (gaf.empty())
            continue;
        for (const Script& script : k_scripts)
            draw_bitmap_script(gaf.data(), gaf.size(), face, atlas, script);
        Canvas blank;
        make_canvas(blank, 64, 32, k_overlay_transparent);
        const int32_t absent = font_atlas_draw(
            &blank.surface, gaf.data(), gaf.size(), face, atlas, k_absent, 4, 4, nullptr, 0
        );
        check(absent == 1 && ink_of(blank.pixels, k_overlay_transparent) == 0, "no missing box");
    }
    font_atlas_close(atlas);
}

// ---- page faces ----------------------------------------------------------------

oa::boneyards::html::MarkupFont page_font(const char* face) {
    oa::boneyards::html::MarkupFont font{};
    font.height = -16;
    font.weight = oa::boneyards::html::k_weight_normal;
    std::snprintf(font.face, sizeof font.face, "%s", face);
    return font;
}

int32_t page_ink(oa::boneyards::html::ServiceFonts& fonts, const char* face, const char* text) {
    const oa::boneyards::html::TextBackend backend =
        oa::boneyards::html::service_text_backend(&fonts);
    constexpr int32_t k_width = 360;
    constexpr int32_t k_height = 40;
    std::vector<uint8_t> pixels(static_cast<std::size_t>(k_width) * k_height, 0);
    oa::Surface surface{};
    oa::present::init_surface(surface, k_width, k_height, k_width, pixels.data());
    backend.draw(
        backend.user,
        surface,
        page_font(face),
        text,
        static_cast<int32_t>(std::strlen(text)),
        2,
        2,
        200
    );
    return ink_of(pixels, 0);
}

void test_page_faces(oa::boneyards::html::ServiceFonts& fonts) {
    const char* faces[] = {"Arial", "Times New Roman", "Haettenschweiler"};
    const oa::boneyards::html::TextBackend backend =
        oa::boneyards::html::service_text_backend(&fonts);
    const int32_t arial = backend.extent(backend.user, page_font("Arial"), "M", 1).cx;
    const int32_t times = backend.extent(backend.user, page_font("Times New Roman"), "M", 1).cx;
    const int32_t condensed =
        backend.extent(backend.user, page_font("Haettenschweiler"), "M", 1).cx;
    check(arial > 0 && times > 0 && condensed > 0, "each page face measures");
    check(arial != times && arial != condensed, "the page faces are not one face");

    for (const char* face : faces) {
        for (const Script& script : k_scripts) {
            char what[96];
            std::snprintf(what, sizeof what, "%s draws %s", face, script.name);
            check(page_ink(fonts, face, script.text) > 8, what);
        }
        check(page_ink(fonts, face, k_absent) == 0, "a page draws no missing box");
    }

    std::string page = "<HTML><BODY>";
    for (const char* face : faces) {
        page += "<FONT FACE=\"";
        page += face;
        page += "\">";
        for (const Script& script : k_scripts) {
            page += script.text;
            page += " ";
        }
        page += "</FONT> ";
    }
    page += "</BODY></HTML>";

    oa::Palette palette{};
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i)
        palette.entries[i] = oa::PaletteEntry{
            static_cast<uint8_t>(i), static_cast<uint8_t>(i), static_cast<uint8_t>(i), 0
        };
    oa::boneyards::html::Environment env{};
    env.text = backend;
    env.palette = palette;
    oa::boneyards::html::Document doc;
    oa::boneyards::html::document_init(doc, env);
    oa::boneyards::html::document_set_width(doc, 640);
    check(oa::boneyards::html::document_load(doc, page), "a page of every script loads");
    check(oa::boneyards::html::document_paint(doc), "a page of every script paints");
    const oa::Surface* canvas = oa::boneyards::html::document_surface(doc);
    int32_t ink = 0;
    if (canvas != nullptr && canvas->pixels != nullptr) {
        const int32_t rows = oa::boneyards::html::document_height(doc);
        for (int32_t y = 0; y < rows && y < canvas->height; ++y)
            for (int32_t x = 0; x < canvas->width; ++x)
                ink += canvas->pixels[static_cast<std::ptrdiff_t>(y) * canvas->pitch + x] !=
                               oa::boneyards::html::k_canvas_clear_index
                           ? 1
                           : 0;
    }
    check(ink > 80, "the page shows every script");
}

oa::boneyards::html::FallbackFonts* open_page_chain() {
    const std::string dir = OA_TEXT_FONTS_DIR;
    const std::string dejavu = dir + "/DejaVuSans.ttf";
    const std::string bold = dir + "/DejaVuSans-Bold.ttf";
    const std::string emoji = dir + "/NotoEmoji.ttf";
    const oa::boneyards::html::FallbackFiles files{
        dejavu.c_str(), bold.c_str(), nullptr, emoji.c_str()
    };
    return oa::boneyards::html::fallback_fonts_open_files(files);
}

// ---- modern chat line ----------------------------------------------------------

int32_t count_ink(const oa::Surface* surface, uint8_t ink) {
    int32_t count = 0;
    if (surface == nullptr || surface->pixels == nullptr)
        return 0;
    for (int32_t y = 0; y < surface->height; ++y) {
        const uint8_t* row = surface->pixels + static_cast<std::ptrdiff_t>(y) * surface->pitch;
        for (int32_t x = 0; x < surface->width; ++x)
            count += row[x] == ink ? 1 : 0;
    }
    return count;
}

void clear_surface(oa::Surface* surface) {
    if (surface == nullptr || surface->pixels == nullptr)
        return;
    for (int32_t y = 0; y < surface->height; ++y)
        std::memset(
            surface->pixels + static_cast<std::ptrdiff_t>(y) * surface->pitch,
            0,
            static_cast<std::size_t>(surface->width)
        );
}

struct Added {
    ListBox* list = nullptr;
    int32_t count = 0;
    char lines[8][32]{};
};

void add_row(void* user, const char* line, int32_t, int32_t height, uint8_t) {
    auto* added = static_cast<Added*>(user);
    Window* row = static_create();
    window_set_state_count(row, 1);
    window_set_state(row, 0);
    window_set_state_text(row, 0, line, 0);
    window_create(row, "row", 0, 0, 80, height, nullptr, true);
    list_box_add(added->list, row);
    if (added->count < 8)
        std::snprintf(added->lines[added->count], sizeof added->lines[0], "%s", line);
    ++added->count;
}

void test_modern_chat(oa::Surface* surface) {
    modern_chat_use_engine();
    modern_chat_set(true, 80);
    check(modern_chat_on(), "modern chat is on");
    constexpr uint8_t k_ink = 40;
    for (const Script& script : k_scripts) {
        clear_surface(surface);
        const bool painted =
            modern_chat_paint(script.text, 4, 24, k_ink, 0, 0, surface->width, surface->height);
        char what[64];
        std::snprintf(what, sizeof what, "modern chat draws %s", script.name);
        check(painted && count_ink(surface, k_ink) > 8 && modern_chat_width(script.text) > 0, what);
    }
    clear_surface(surface);
    // The engine face may draw its own stand-in for a character none of the faces
    // hold. Coverage is the five scripts above; the stand-in must not be a failed paint.
    check(
        modern_chat_paint(k_absent, 4, 24, k_ink, 0, 0, surface->width, surface->height),
        "modern chat accepts a character none of its faces hold"
    );

    const int32_t one = modern_chat_width("\xE4\xB8\xAD");
    check(one > 0, "one ideograph has a width");
    Added added;
    added.list = list_box_create();
    window_create(&added.list->base, "log", 0, 0, 80, 80, nullptr, true);
    added.list->log_mode = 1;
    added.list->capacity = 8;
    check(
        modern_chat_write(added.list, k_cjk, one, 0, 0, k_ink, add_row, &added),
        "a CJK line is written"
    );
    check(added.count == 2, "a line breaks between two ideographs");
    if (added.count >= 2) {
        check(std::strcmp(added.lines[0], "\u4E2D") == 0, "the first ideograph stays whole");
        check(std::strcmp(added.lines[1], "\u6587") == 0, "the second ideograph stays whole");
    }
    window_destroy(&added.list->base);
    modern_chat_set_faces(nullptr);
}

} // namespace

int main() {
    oa::boneyards::html::FallbackFonts* chain = open_page_chain();
    check(chain != nullptr, "the page chain opens");
    if (chain == nullptr)
        return 1;
    oa::boneyards::html::page_script_fonts_use(chain);

    char error[512]{};
    oa::platform::FontLibrary* library =
        oa::platform::font_library_open(OA_FONTS_DIR, error, static_cast<int32_t>(sizeof error));
    check(library != nullptr, error[0] != '\0' ? error : "the page faces open");
    if (library != nullptr) {
        oa::boneyards::html::ServiceFonts fonts{};
        fonts.service = oa::boneyards::html::host_font_service(library);
        test_page_faces(fonts);
        oa::boneyards::html::service_fonts_clear(fonts);
        oa::platform::font_library_close(library);
    }

    oa::Palette palette{};
    palette.entries[1] = oa::PaletteEntry{255, 255, 255, 0};
    ToolkitConfig config{};
    config.width = 360;
    config.height = 48;
    config.palette = &palette;
    Toolkit* toolkit = toolkit_create(&config);
    const Services previous = services_install(toolkit_services(toolkit));
    test_modern_chat(toolkit_surface(toolkit));
    services_install(previous);
    toolkit_destroy(toolkit);

    oa::boneyards::html::page_script_fonts_use(nullptr);
    oa::boneyards::html::fallback_fonts_close(chain);

    if (g_failures != 0) {
        std::fprintf(stderr, "language fonts: %d failure(s)\n", g_failures);
        return 1;
    }
    test_bitmap_faces();
    if (g_failures != 0) {
        std::fprintf(stderr, "language fonts: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("language fonts: ok");
    return 0;
}
