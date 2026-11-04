// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Each bitmap face draws "Grüße Привет Ελλάδα 東京 ✓". ASCII comes from the
// face itself, pixel for pixel; the other characters are present and sit on
// the same baseline.
#include "test_data.hpp"

#include <fstream>
#include <string>
#include <vector>

#include "oa/boneyards/font_atlases.hpp"
#include "oa/platform/system.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::testing;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// "Grüße Привет Ελλάδα 東京 ✓"
const char* const k_line = "Gr\xC3\xBC\xC3\x9F"
                           "e \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"
                           " \xCE\x95\xCE\xBB\xCE\xBB\xCE\xAC\xCE\xB4\xCE\xB1"
                           " \xE6\x9D\xB1\xE4\xBA\xAC"
                           " \xE2\x9C\x93";

const char* const k_faces[] = {
    "Fonts\\BYHatFontSmall87.GAF",
    "Fonts\\BYHatFontMedium90.GAF",
    "Fonts\\BYHatFontBig84.GAF",
    "anims\\Hattfont12.gaf",
};

struct Canvas {
    std::vector<uint8_t> pixels;
    Surface surface{};
};

/// Fills a canvas in place. The surface points at `pixels`, so the canvas
/// must not be moved afterwards.
void make_canvas(Canvas& canvas, int32_t width, int32_t height) {
    canvas.pixels.assign(
        static_cast<size_t>(width) * static_cast<size_t>(height), k_overlay_transparent
    );
    canvas.surface = {};
    canvas.surface.width = width;
    canvas.surface.height = height;
    canvas.surface.pitch = width;
    canvas.surface.pixels = canvas.pixels.data();
    canvas.surface.reserved_after_pixels = OA_SURFACE_RESERVED_AFTER_PIXELS_INIT;
    canvas.surface.reserved_before_origin = OA_SURFACE_RESERVED_BEFORE_ORIGIN_INIT;
    canvas.surface.clip = Rect32{0, 0, width - 1, height - 1};
    canvas.surface.flags = OA_SURFACE_FLAG_MEMORY;
}

void clear_canvas(Canvas& canvas) {
    std::memset(canvas.pixels.data(), k_overlay_transparent, canvas.pixels.size());
}

int32_t differing_pixels(const Surface& left, const Surface& right) {
    int32_t count = 0;
    for (int32_t y = 0; y < left.height; ++y) {
        for (int32_t x = 0; x < left.width; ++x) {
            count += left.pixels[y * left.pitch + x] != right.pixels[y * right.pitch + x] ? 1 : 0;
        }
    }
    return count;
}

std::filesystem::path find_atlas() {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    if (const auto dir = oa::platform::environment_value("OA_FONT_ATLASES_DIR")) {
        candidates.emplace_back(fs::path(*dir) / "boneyards-analogues.bin");
    }
    if (const auto deps = oa::platform::environment_value("OA_DEPS_DIR")) {
        candidates.emplace_back(fs::path(*deps) / "font-atlases" / "boneyards-analogues.bin");
    }
    fs::path cursor = fs::current_path();
    for (int step = 0; step < 8; ++step) {
        candidates.push_back(cursor / "data/boneyards/fonts/boneyards-analogues.bin");
        candidates.push_back(cursor / "local/deps/font-atlases/boneyards-analogues.bin");
        const fs::path parent = cursor.parent_path();
        if (parent == cursor) {
            break;
        }
        cursor = parent;
    }
    for (const fs::path& candidate : candidates) {
        std::error_code error;
        if (fs::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return {};
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
    );
}

void encode_utf8(uint32_t code, char out[5]) {
    if (code < 0x80) {
        out[0] = static_cast<char>(code);
        out[1] = '\0';
    } else if (code < 0x800) {
        out[0] = static_cast<char>(0xC0 | (code >> 6));
        out[1] = static_cast<char>(0x80 | (code & 0x3F));
        out[2] = '\0';
    } else {
        out[0] = static_cast<char>(0xE0 | (code >> 12));
        out[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (code & 0x3F));
        out[3] = '\0';
    }
}

bool face_owns(uint32_t code) {
    return code < 0x80 || code == 0xFC || code == 0xDF;
}

void remember_colors(const Surface& surface, bool seen[256]) {
    for (int32_t y = 0; y < surface.height; ++y) {
        for (int32_t x = 0; x < surface.width; ++x) {
            seen[surface.pixels[y * surface.pitch + x]] = true;
        }
    }
}

struct FaceFiles {
    oa::HpiArchive* boneyards = nullptr;
    oa::HpiArchive* base = nullptr;
};

uint8_t* copy_archive_file(oa::HpiArchive* archive, const char* path, size_t* size) {
    if (archive == nullptr) {
        return nullptr;
    }
    auto read = archive->read(path);
    if (!read.ok()) {
        return nullptr;
    }
    const std::vector<uint8_t>& bytes = *read.value;
    auto* out = static_cast<uint8_t*>(std::malloc(bytes.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, bytes.data(), bytes.size());
    *size = bytes.size();
    return out;
}

/// Hat faces live in by.ccx. Hattfont12 lives in the base archive.
uint8_t* read_face_file(void* user, const char* path, size_t* size) {
    auto* files = static_cast<FaceFiles*>(user);
    if (uint8_t* found = copy_archive_file(files->boneyards, path, size)) {
        return found;
    }
    return copy_archive_file(files->base, path, size);
}

void test_face(Toolkit* toolkit, FaceFiles* files, const FontAtlas* atlas, const char* path) {
    std::fprintf(stdout, "face %s\n", path);
    size_t gaf_size = 0;
    uint8_t* gaf = read_face_file(files, path, &gaf_size);
    check(gaf != nullptr && gaf_size > 0, "the face's glyph file is in the install");
    if (gaf == nullptr) {
        return;
    }
    const Services services = toolkit_services(toolkit);
    const Handle font = services.fonts->load(services.fonts->self, path);
    check(font != k_no_handle, "the toolkit loads the face");
    if (font == k_no_handle) {
        std::free(gaf);
        return;
    }

    Canvas line;
    make_canvas(line, 800, 80);
    FontAtlasGlyph places[40]{};
    const int32_t count =
        font_atlas_draw(&line.surface, gaf, gaf_size, path, atlas, k_line, 0, 20, places, 40);
    check(count > 8 && count < 40, "the line decodes");
    if (count < 0) {
        services.fonts->release(services.fonts->self, font);
        std::free(gaf);
        return;
    }

    Canvas own;
    Canvas atlas_ascii;
    make_canvas(own, 80, 40);
    make_canvas(atlas_ascii, 80, 40);
    const int32_t ascii =
        font_atlas_draw(&atlas_ascii.surface, gaf, gaf_size, path, atlas, "Gr", 0, 0, nullptr, 0);
    toolkit_draw_text_run(toolkit, &own.surface, font, "Gr", 2);
    check(ascii == 2, "Gr is two characters");
    const int32_t mismatch = differing_pixels(own.surface, atlas_ascii.surface);
    if (mismatch != 0) {
        std::fprintf(stderr, "FAIL: %s ASCII differs in %d pixels\n", path, mismatch);
        ++g_failures;
    }
    int32_t measured = 0;
    int32_t measured_height = 0;
    toolkit_measure_text(toolkit, font, "Gr", &measured, &measured_height);
    check(places[0].advance + places[1].advance == measured, "Gr keeps the face's advances");

    // The capital baseline is the bottom of H's letter, not a one-pixel
    // outline under it. The message face draws that outline, so H's lowest
    // ink sits one row below the letters.
    FontAtlasGlyph capital{};
    const int32_t drawn_h =
        font_atlas_draw(&line.surface, gaf, gaf_size, path, atlas, "H", 700, 20, &capital, 1);
    check(
        drawn_h == 1 && capital.original != 0 && capital.ink_bottom >= 0, "H is drawn from the face"
    );
    int32_t baseline = capital.ink_bottom;
    if (capital.ink_bottom > capital.ink_top) {
        uint8_t edge = 0;
        bool any = false;
        bool uniform = true;
        bool other_above = false;
        for (int32_t x = 680; x < 740; ++x) {
            const uint8_t pixel = line.surface.pixels[capital.ink_bottom * line.surface.pitch + x];
            if (pixel == k_overlay_transparent) {
                continue;
            }
            if (!any) {
                edge = pixel;
                any = true;
            } else if (pixel != edge) {
                uniform = false;
            }
            const uint8_t above =
                line.surface.pixels[(capital.ink_bottom - 1) * line.surface.pitch + x];
            if (above != k_overlay_transparent && above != edge) {
                other_above = true;
            }
        }
        if (any && uniform && other_above) {
            --baseline;
        }
    }

    bool saw_analogue = false;
    for (int32_t index = 0; index < count; ++index) {
        const FontAtlasGlyph& glyph = places[index];
        const bool expect_original = face_owns(glyph.codepoint);
        if (expect_original != (glyph.original != 0)) {
            std::fprintf(
                stderr, "FAIL: %s U+%04X original %u\n", path, glyph.codepoint, glyph.original
            );
            ++g_failures;
        }
        if (glyph.codepoint == ' ') {
            check(glyph.ink_top < 0 && glyph.advance > 0, "spaces advance and draw nothing");
            continue;
        }
        if (glyph.ink_top < 0 || glyph.ink_bottom < glyph.ink_top) {
            std::fprintf(stderr, "FAIL: %s U+%04X has no ink\n", path, glyph.codepoint);
            ++g_failures;
            continue;
        }
        if (glyph.ink_top > baseline || glyph.ink_bottom < baseline) {
            std::fprintf(
                stderr,
                "FAIL: %s U+%04X ink %d..%d misses baseline %d\n",
                path,
                glyph.codepoint,
                glyph.ink_top,
                glyph.ink_bottom,
                baseline
            );
            ++g_failures;
        }
        if (glyph.original == 0) {
            saw_analogue = true;
        }
    }
    check(saw_analogue, "the line uses analogue glyphs");

    bool face_colors[256] = {};
    Canvas sample;
    make_canvas(sample, 64, 48);
    for (int byte = 1; byte < 256; ++byte) {
        clear_canvas(sample);
        const char text[2] = {static_cast<char>(byte), '\0'};
        toolkit_draw_text_run(toolkit, &sample.surface, font, text, 1);
        remember_colors(sample.surface, face_colors);
    }
    const uint32_t analogues[] = {0x041F, 0x0395, 0x6771, 0x4EAC, 0x2713};
    for (uint32_t code : analogues) {
        clear_canvas(sample);
        char encoded[5];
        encode_utf8(code, encoded);
        FontAtlasGlyph one{};
        const int32_t drawn =
            font_atlas_draw(&sample.surface, gaf, gaf_size, path, atlas, encoded, 4, 16, &one, 1);
        check(drawn == 1 && one.original == 0 && one.ink_top >= 0, "an analogue glyph draws");
        for (int32_t y = 0; y < sample.surface.height; ++y) {
            for (int32_t x = 0; x < sample.surface.width; ++x) {
                const uint8_t pixel = sample.surface.pixels[y * sample.surface.pitch + x];
                if (pixel != k_overlay_transparent && !face_colors[pixel]) {
                    std::fprintf(
                        stderr,
                        "FAIL: %s U+%04X uses colour %u the face does not\n",
                        path,
                        code,
                        pixel
                    );
                    ++g_failures;
                    y = sample.surface.height;
                    break;
                }
            }
        }
    }

    services.fonts->release(services.fonts->self, font);
    std::free(gaf);
}

} // namespace

int main() {
    InstalledData data;
    open_installed_data(&data);
    const std::filesystem::path atlas_path = find_atlas();
    if (atlas_path.empty()) {
        std::fprintf(stderr, "FAIL: no analogue atlas\n");
        return 1;
    }
    const std::vector<uint8_t> bytes = read_file(atlas_path);
    FontAtlas* atlas = font_atlas_open(bytes.data(), bytes.size());
    check(atlas != nullptr, "the atlas opens");
    if (atlas == nullptr) {
        return 1;
    }

    const std::filesystem::path root = oa::boneyards_test::by_install();
    auto base_opened = oa::open_hpi_file(root / oa::test::netgame::kBaseArchive);
    check(base_opened.ok(), "the base archive opens");
    if (!base_opened.ok()) {
        font_atlas_close(atlas);
        return 1;
    }
    auto base = std::make_unique<oa::HpiArchive>(std::move(*base_opened.value));
    FaceFiles files{data.archive.get(), base.get()};

    ToolkitConfig config = installed_config(&data, 160, 120);
    config.files = ToolkitFiles{&files, read_face_file};
    Toolkit* toolkit = toolkit_create(&config);
    check(toolkit != nullptr, "toolkit");

    Canvas scratch;
    make_canvas(scratch, 32, 32);
    check(
        font_atlas_draw(
            &scratch.surface, nullptr, 0, "BYHatFontSmall87", atlas, "", 0, 0, nullptr, 0
        ) == -1,
        "a missing face file is refused"
    );
    size_t gaf_size = 0;
    uint8_t* gaf = read_face_file(&files, k_faces[0], &gaf_size);
    check(gaf != nullptr, "the small face is installed");
    if (gaf != nullptr) {
        check(
            font_atlas_draw(
                &scratch.surface, gaf, gaf_size, k_faces[0], atlas, "", 0, 0, nullptr, 0
            ) == 0,
            "an empty line draws nothing"
        );
        check(
            font_atlas_draw(
                &scratch.surface, gaf, gaf_size, k_faces[0], atlas, "\xFF", 0, 0, nullptr, 0
            ) == -1,
            "a bad byte is refused"
        );
        std::free(gaf);
    }

    for (const char* face : k_faces) {
        test_face(toolkit, &files, atlas, face);
    }

    font_atlas_close(atlas);
    toolkit_destroy(toolkit);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("boneyards font atlas tests passed");
    return 0;
}
