// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Draws the analogue fonts into one atlas the bitmap faces share.
//
// Fira Sans Extra Condensed SemiBold supplies Latin, Greek, Cyrillic and
// Vietnamese, sized so its capital H is 9 pixels for the two console faces
// and 11 for the title face and the message-box face. Noto Sans CJK SC
// Medium, then Noto Emoji at weight 500, follow it, both at 13 pixels.
// Pictures are one bit, with no smoothing. The library is opened without
// default properties, so the machine's environment cannot change a pixel.
//
// The file is little-endian:
//   'BYFA', uint16 version 1, uint16 source count, uint16 face count
//   each source: uint8 name length, name, uint16 pixel size, uint16 capital
//     rows (0 when the size is fixed), uint32 glyph count, then glyphs in
//     code-point order
//   each glyph: uint32 code point, int16 advance, int16 left, int16 top
//     (rows of the picture above the baseline), uint16 width, uint16 rows,
//     then the picture packed row by row, the high bit of each byte first
//   each face: uint8 name length, name, int16 line height, uint16 capital
//     rows, uint8 source count, that many source indexes
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_MULTIPLE_MASTERS_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr int k_version = 1;
constexpr int k_cjk_pixels = 13;
constexpr int k_emoji_pixels = 13;
constexpr int k_emoji_weight = 500;
constexpr int k_cap_small = 9;
constexpr int k_cap_tall = 11;

struct Glyph {
    uint32_t codepoint = 0;
    int16_t advance = 0;
    int16_t left = 0;
    int16_t top = 0;
    uint16_t width = 0;
    uint16_t rows = 0;
    std::vector<uint8_t> bits;
};

struct Source {
    const char* name = "";
    uint16_t pixels = 0;
    uint16_t cap_rows = 0;
    std::vector<Glyph> glyphs;
};

void fail(const char* message) {
    std::fprintf(stderr, "rasterize_font_atlas: %s\n", message);
    std::exit(1);
}

void* allocate(FT_Memory, long size) {
    return std::malloc(static_cast<std::size_t>(size));
}

void release(FT_Memory, void* block) {
    std::free(block);
}

void* reallocate(FT_Memory, long, long size, void* block) {
    return std::realloc(block, static_cast<std::size_t>(size));
}

const char* option(int argc, char** argv, const char* name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return nullptr;
}

int16_t clamp16(int32_t value) {
    if (value > 32767) {
        return 32767;
    }
    if (value < -32768) {
        return -32768;
    }
    return static_cast<int16_t>(value);
}

/// Packs a one-bit picture, row by row, high bit first. A negative pitch
/// stores the last row first; the packed rows run top to bottom.
void pack_mono(const FT_Bitmap& bitmap, Glyph& glyph) {
    if (bitmap.pixel_mode != FT_PIXEL_MODE_MONO || bitmap.width > 512 || bitmap.rows > 512) {
        fail("a glyph was not a one-bit picture of a readable size");
    }
    glyph.width = static_cast<uint16_t>(bitmap.width);
    glyph.rows = static_cast<uint16_t>(bitmap.rows);
    const std::size_t count = (static_cast<std::size_t>(glyph.width) * glyph.rows + 7) / 8;
    glyph.bits.assign(count, 0);
    if (glyph.width == 0 || glyph.rows == 0 || bitmap.buffer == nullptr) {
        return;
    }
    int pitch = bitmap.pitch;
    const uint8_t* rows = bitmap.buffer;
    if (pitch < 0) {
        rows += static_cast<std::ptrdiff_t>(glyph.rows - 1) * pitch;
        pitch = -pitch;
    }
    for (uint16_t row = 0; row < glyph.rows; ++row) {
        const uint8_t* line = rows + static_cast<std::ptrdiff_t>(row) * pitch;
        for (uint16_t column = 0; column < glyph.width; ++column) {
            const bool covered = (line[column >> 3] & (0x80u >> (column & 7))) != 0;
            if (!covered) {
                continue;
            }
            const std::size_t bit = static_cast<std::size_t>(row) * glyph.width + column;
            glyph.bits[bit >> 3] =
                static_cast<uint8_t>(glyph.bits[bit >> 3] | (0x80u >> (bit & 7)));
        }
    }
}

std::vector<uint32_t> codepoints_of(FT_Face face) {
    std::vector<uint32_t> codes;
    FT_UInt index = 0;
    FT_ULong code = FT_Get_First_Char(face, &index);
    while (index != 0) {
        if (code <= 0x10FFFF) {
            codes.push_back(static_cast<uint32_t>(code));
        }
        code = FT_Get_Next_Char(face, code, &index);
    }
    std::sort(codes.begin(), codes.end());
    codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
    return codes;
}

bool contains(const std::vector<uint32_t>& codes, uint32_t code) {
    return std::binary_search(codes.begin(), codes.end(), code);
}

void render_glyph(FT_Face face, uint32_t code, Glyph& glyph) {
    const FT_UInt index = FT_Get_Char_Index(face, code);
    if (index == 0) {
        fail("a character the face should hold is missing");
    }
    if (FT_Load_Glyph(face, index, FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_MONO) != 0 ||
        FT_Render_Glyph(face->glyph, FT_RENDER_MODE_MONO) != 0) {
        fail("a character could not be drawn");
    }
    const FT_GlyphSlot slot = face->glyph;
    glyph.codepoint = code;
    glyph.advance = clamp16(static_cast<int32_t>((slot->advance.x + 32) >> 6));
    glyph.left = clamp16(slot->bitmap_left);
    glyph.top = clamp16(slot->bitmap_top);
    pack_mono(slot->bitmap, glyph);
}

/// The smallest pixel size whose capital H is `rows` tall, or -1.
int fit_capitals(FT_Face face, int rows) {
    int found = -1;
    for (int pixels = 1; pixels <= 64; ++pixels) {
        if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixels)) != 0) {
            continue;
        }
        if (FT_Load_Char(face, 'H', FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_MONO) != 0 ||
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_MONO) != 0) {
            continue;
        }
        if (static_cast<int>(face->glyph->bitmap.rows) == rows) {
            found = pixels;
            break;
        }
    }
    return found;
}

Source draw_source(
    FT_Face face, const char* name, int pixels, int cap_rows, const std::vector<uint32_t>& codes
) {
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixels)) != 0) {
        fail("a face rejected its pixel size");
    }
    Source source;
    source.name = name;
    source.pixels = static_cast<uint16_t>(pixels);
    source.cap_rows = static_cast<uint16_t>(cap_rows);
    source.glyphs.reserve(codes.size());
    uint32_t drawn = 0;
    for (uint32_t code : codes) {
        if (FT_Get_Char_Index(face, code) == 0) {
            continue;
        }
        Glyph glyph;
        render_glyph(face, code, glyph);
        source.glyphs.push_back(std::move(glyph));
        ++drawn;
        if (drawn % 8000 == 0) {
            std::fprintf(stderr, "drawing %s: %u\n", name, drawn);
            std::fflush(stderr);
        }
    }
    std::sort(source.glyphs.begin(), source.glyphs.end(), [](const Glyph& a, const Glyph& b) {
        return a.codepoint < b.codepoint;
    });
    return source;
}

void require_glyph(const Source& source, uint32_t code, const char* what) {
    const auto found = std::lower_bound(
        source.glyphs.begin(), source.glyphs.end(), code, [](const Glyph& glyph, uint32_t wanted) {
            return glyph.codepoint < wanted;
        }
    );
    if (found == source.glyphs.end() || found->codepoint != code || found->width == 0 ||
        found->rows == 0) {
        std::fprintf(stderr, "rasterize_font_atlas: %s has no picture for U+%04X\n", what, code);
        std::exit(1);
    }
}

void set_emoji_weight(FT_Library library, FT_Face face) {
    if (!FT_HAS_MULTIPLE_MASTERS(face)) {
        return;
    }
    FT_MM_Var* axes = nullptr;
    if (FT_Get_MM_Var(face, &axes) != 0) {
        fail("Noto Emoji's weight could not be read");
    }
    std::vector<FT_Fixed> coordinates(axes->num_axis);
    bool found = false;
    for (FT_UInt axis = 0; axis < axes->num_axis; ++axis) {
        coordinates[axis] = axes->axis[axis].def;
        if (axes->axis[axis].tag != FT_MAKE_TAG('w', 'g', 'h', 't')) {
            continue;
        }
        FT_Fixed weight = static_cast<FT_Fixed>(k_emoji_weight) * 65536;
        if (weight < axes->axis[axis].minimum) {
            weight = axes->axis[axis].minimum;
        }
        if (weight > axes->axis[axis].maximum) {
            weight = axes->axis[axis].maximum;
        }
        coordinates[axis] = weight;
        found = true;
    }
    if (!found || FT_Set_Var_Design_Coordinates(face, axes->num_axis, coordinates.data()) != 0) {
        FT_Done_MM_Var(library, axes);
        fail("Noto Emoji's weight could not be set");
    }
    FT_Done_MM_Var(library, axes);
}

FT_Face open_face(FT_Library library, const char* path, int index) {
    FT_Face face = nullptr;
    if (FT_New_Face(library, path, static_cast<FT_Long>(index), &face) != 0) {
        std::fprintf(stderr, "rasterize_font_atlas: could not open %s\n", path);
        std::exit(1);
    }
    return face;
}

FT_Face open_cjk(FT_Library library, const char* path) {
    for (int index = 0; index < 16; ++index) {
        FT_Face face = nullptr;
        if (FT_New_Face(library, path, index, &face) != 0) {
            break;
        }
        const bool match = face->family_name != nullptr && face->style_name != nullptr &&
                           std::strcmp(face->family_name, "Noto Sans CJK SC") == 0 &&
                           std::strcmp(face->style_name, "Medium") == 0;
        if (match) {
            return face;
        }
        FT_Done_Face(face);
    }
    fail("the collection has no Noto Sans CJK SC Medium face");
    return nullptr;
}

struct Writer {
    std::vector<uint8_t> bytes;

    void u8(uint8_t value) { bytes.push_back(value); }

    void u16(uint16_t value) {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
    }

    void u32(uint32_t value) {
        u16(static_cast<uint16_t>(value));
        u16(static_cast<uint16_t>(value >> 16));
    }

    void i16(int16_t value) { u16(static_cast<uint16_t>(value)); }

    void raw(const void* data, std::size_t size) {
        const auto* in = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), in, in + size);
    }

    void text(const char* value) {
        const std::size_t length = std::strlen(value);
        if (length > 255) {
            fail("a name is longer than the atlas allows");
        }
        u8(static_cast<uint8_t>(length));
        raw(value, length);
    }
};

void write_atlas(
    const std::filesystem::path& path,
    const Source& small,
    const Source& tall,
    const Source& cjk,
    const Source& emoji
) {
    struct Face {
        const char* name;
        int16_t line_height;
        uint16_t cap_rows;
        uint8_t sources[3];
        uint8_t source_count;
    };

    // Source 0 is the 9-pixel Fira, 1 the 11-pixel Fira, 2 CJK, 3 emoji.
    const Face faces[] = {
        {"BYHatFontSmall87", 9, k_cap_small, {0, 2, 3}, 3},
        {"BYHatFontMedium90", 9, k_cap_small, {0, 2, 3}, 3},
        {"BYHatFontBig84", 11, k_cap_tall, {1, 2, 3}, 3},
        {"Hattfont12", 12, k_cap_tall, {1, 2, 3}, 3},
    };
    const Source* sources[] = {&small, &tall, &cjk, &emoji};

    Writer out;
    out.raw("BYFA", 4);
    out.u16(k_version);
    out.u16(4);
    out.u16(4);
    for (const Source* source : sources) {
        out.text(source->name);
        out.u16(source->pixels);
        out.u16(source->cap_rows);
        out.u32(static_cast<uint32_t>(source->glyphs.size()));
        for (const Glyph& glyph : source->glyphs) {
            out.u32(glyph.codepoint);
            out.i16(glyph.advance);
            out.i16(glyph.left);
            out.i16(glyph.top);
            out.u16(glyph.width);
            out.u16(glyph.rows);
            if (!glyph.bits.empty()) {
                out.raw(glyph.bits.data(), glyph.bits.size());
            }
        }
    }
    for (const Face& face : faces) {
        out.text(face.name);
        out.i16(face.line_height);
        out.u16(face.cap_rows);
        out.u8(face.source_count);
        out.raw(face.sources, face.source_count);
    }

    const std::filesystem::path temporary = path.string() + ".tmp";
    std::filesystem::create_directories(path.parent_path());
    std::FILE* file = std::fopen(temporary.string().c_str(), "wb");
    if (file == nullptr) {
        fail("the atlas could not be created");
    }
    const std::size_t written = std::fwrite(out.bytes.data(), 1, out.bytes.size(), file);
    const int closed = std::fclose(file);
    if (written != out.bytes.size() || closed != 0) {
        std::filesystem::remove(temporary);
        fail("the atlas could not be written");
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        std::filesystem::rename(temporary, path, error);
    }
    if (error) {
        fail("the atlas could not be replaced");
    }
    std::printf(
        "analogue atlas: Fira capitals %d at %u pixels (%zu glyphs), %d at %u pixels (%zu), "
        "CJK %zu glyphs, emoji %zu glyphs\n",
        k_cap_small,
        small.pixels,
        small.glyphs.size(),
        k_cap_tall,
        tall.pixels,
        tall.glyphs.size(),
        cjk.glyphs.size(),
        emoji.glyphs.size()
    );
}

} // namespace

int main(int argc, char** argv) {
    const char* fira_path = option(argc, argv, "--fira");
    const char* cjk_path = option(argc, argv, "--cjk");
    const char* emoji_path = option(argc, argv, "--emoji");
    const char* out_path = option(argc, argv, "--out");
    if (fira_path == nullptr || cjk_path == nullptr || emoji_path == nullptr ||
        out_path == nullptr) {
        fail("usage: rasterize_font_atlas --fira FILE --cjk FILE --emoji FILE --out FILE");
    }

    FT_MemoryRec_ memory{};
    memory.alloc = allocate;
    memory.free = release;
    memory.realloc = reallocate;
    FT_Library library = nullptr;
    if (FT_New_Library(&memory, &library) != 0) {
        fail("the font library could not be opened");
    }
    FT_Add_Default_Modules(library);

    FT_Face fira = open_face(library, fira_path, 0);
    if (fira->family_name == nullptr ||
        std::strcmp(fira->family_name, "Fira Sans Extra Condensed") != 0 ||
        fira->style_name == nullptr || std::strcmp(fira->style_name, "SemiBold") != 0) {
        fail("the Latin face is not Fira Sans Extra Condensed SemiBold");
    }
    const int small_pixels = fit_capitals(fira, k_cap_small);
    const int tall_pixels = fit_capitals(fira, k_cap_tall);
    if (small_pixels < 0 || tall_pixels < 0) {
        fail("Fira's capital H does not match a bitmap face");
    }
    const std::vector<uint32_t> fira_codes = codepoints_of(fira);
    Source small =
        draw_source(fira, "Fira Sans Extra Condensed", small_pixels, k_cap_small, fira_codes);
    Source tall =
        draw_source(fira, "Fira Sans Extra Condensed", tall_pixels, k_cap_tall, fira_codes);
    require_glyph(small, 0x041F, "Fira"); // Cyrillic Pe
    require_glyph(small, 0x0395, "Fira"); // Greek Epsilon
    require_glyph(tall, 0x041F, "Fira");
    require_glyph(tall, 0x0395, "Fira");

    FT_Face cjk = open_cjk(library, cjk_path);
    std::vector<uint32_t> cjk_codes = codepoints_of(cjk);
    cjk_codes.erase(
        std::remove_if(
            cjk_codes.begin(),
            cjk_codes.end(),
            [&](uint32_t code) { return contains(fira_codes, code); }
        ),
        cjk_codes.end()
    );
    Source cjk_source = draw_source(cjk, "Noto Sans CJK SC Medium", k_cjk_pixels, 0, cjk_codes);
    require_glyph(cjk_source, 0x6771, "Noto Sans CJK"); // East
    require_glyph(cjk_source, 0x4EAC, "Noto Sans CJK"); // Capital
    require_glyph(cjk_source, 0x2713, "Noto Sans CJK"); // check mark

    FT_Face emoji = open_face(library, emoji_path, 0);
    if (emoji->family_name == nullptr || std::strcmp(emoji->family_name, "Noto Emoji") != 0) {
        fail("the emoji face is not Noto Emoji");
    }
    set_emoji_weight(library, emoji);
    std::vector<uint32_t> emoji_codes = codepoints_of(emoji);
    emoji_codes.erase(
        std::remove_if(
            emoji_codes.begin(),
            emoji_codes.end(),
            [&](uint32_t code) { return contains(fira_codes, code) || contains(cjk_codes, code); }
        ),
        emoji_codes.end()
    );
    Source emoji_source = draw_source(emoji, "Noto Emoji", k_emoji_pixels, 0, emoji_codes);
    require_glyph(emoji_source, 0x2705, "Noto Emoji");

    write_atlas(out_path, small, tall, cjk_source, emoji_source);

    FT_Done_Face(emoji);
    FT_Done_Face(cjk);
    FT_Done_Face(fira);
    FT_Done_Library(library);
    return 0;
}
