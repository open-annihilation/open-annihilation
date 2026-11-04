// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// FreeType implementation of the host text service.

#include "oa/platform/fonts.hpp"
#include "oa/platform/files.hpp"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

namespace oa::platform {

namespace {

constexpr int32_t k_characters = 256;
constexpr FT_Pos k_pixel = 64; // 26.6 fixed point
// GDI's simulated bold overstrikes each glyph one pixel to the right and
// widens its advance by that pixel.
constexpr FT_Pos k_embolden_width = k_pixel;
constexpr int32_t k_embolden_advance = 1;
// Slant of simulated italic: tan(12 degrees) in 16.16, FreeType's oblique.
constexpr FT_Fixed k_oblique_shear = 0x0366A;
constexpr FT_Fixed k_unit_scale = 0x10000;
// Largest em a positive (cell) height is fitted to.
constexpr int32_t k_max_em = 255;
constexpr int32_t k_bold_weight = 700;
constexpr uint8_t k_full_coverage = 0xFF;
constexpr int32_t k_mono_bits = 8;
constexpr uint8_t k_mono_first_bit = 0x80;

// Windows-1252 0x80..0x9F; the five unassigned bytes keep their C1 code
// points, which no bundled font maps.
constexpr int32_t k_cp1252_high_first = 0x80;
constexpr int32_t k_cp1252_high_count = 0x20;
constexpr uint16_t k_cp1252_high[k_cp1252_high_count] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

// Weights (per 1000) of the lowercase letters and the space in the average
// character width OS/2 version 0 defines. A metric-compatible stand-in
// reproduces the Windows face's xAvgCharWidth through them.
struct AverageWeight {
    char character{};
    int32_t weight{};
};

constexpr AverageWeight k_average_weights[] = {
    {'a', 64}, {'b', 14}, {'c', 27}, {'d', 35}, {'e', 100}, {'f', 20},  {'g', 14},
    {'h', 42}, {'i', 63}, {'j', 3},  {'k', 6},  {'l', 35},  {'m', 20},  {'n', 56},
    {'o', 56}, {'p', 17}, {'q', 4},  {'r', 49}, {'s', 56},  {'t', 71},  {'u', 31},
    {'v', 10}, {'w', 18}, {'x', 3},  {'y', 18}, {'z', 2},   {' ', 166},
};
constexpr int32_t k_average_divisor = 1000;

// FreeType's OS/2 version for a font without the table.
constexpr FT_UShort k_os2_missing = 0xFFFF;
constexpr const char k_font_suffix[] = ".ttf";
constexpr std::size_t k_font_suffix_chars = sizeof k_font_suffix - 1;

struct LoadedFile {
    const char* name{};
    std::vector<uint8_t> bytes{};
};

uint32_t code_point(uint8_t character) {
    if (character >= k_cp1252_high_first && character < k_cp1252_high_first + k_cp1252_high_count)
        return k_cp1252_high[character - k_cp1252_high_first];
    return character;
}

// The Windows-1252 byte of a character, or -1 when the encoding has none.
int32_t cp1252_byte(uint32_t code_point) {
    if (code_point < 0x80 || (code_point >= 0xA0 && code_point <= 0xFF))
        return static_cast<int32_t>(code_point);
    for (int32_t i = 0; i < k_cp1252_high_count; ++i)
        if (k_cp1252_high[i] == code_point)
            return k_cp1252_high_first + i;
    return -1;
}

// value * em / units_per_em, rounded half up.
int32_t scale_round(int32_t value, int32_t em, int32_t units_per_em) {
    const int64_t scaled = static_cast<int64_t>(value) * em;
    const int64_t half = units_per_em / 2;
    return static_cast<int32_t>(
        scaled >= 0 ? (scaled + half) / units_per_em : -((-scaled + half) / units_per_em)
    );
}

bool read_file(const std::string& path, std::vector<uint8_t>* out) {
    std::FILE* file = oa::platform::open_file(path.c_str(), "rb");
    if (file == nullptr)
        return false;
    bool ok = std::fseek(file, 0, SEEK_END) == 0;
    const long size = ok ? std::ftell(file) : -1;
    ok = ok && size > 0 && std::fseek(file, 0, SEEK_SET) == 0;
    if (ok) {
        out->resize(static_cast<std::size_t>(size));
        ok = std::fread(out->data(), 1, out->size(), file) == out->size();
    }
    std::fclose(file);
    return ok;
}

bool is_font_file(const char* name) {
    const std::size_t length = std::strlen(name);
    return length > k_font_suffix_chars &&
           std::strcmp(name + length - k_font_suffix_chars, k_font_suffix) == 0;
}

void set_error(char* error, int32_t error_bytes, const char* what, const std::string& path) {
    if (error != nullptr && error_bytes > 0)
        std::snprintf(error, static_cast<std::size_t>(error_bytes), "%s: %s", what, path.c_str());
}

struct LineMetrics {
    int32_t ascent{};
    int32_t descent{};
};

LineMetrics face_line_metrics(FT_Face face, const LineMetricsTable* table, int32_t em) {
    if (table != nullptr) {
        const int32_t row = em - table->first_em;
        if (row >= 0 && row < table->count)
            return LineMetrics{table->rows[row][0], table->rows[row][1]};
        return LineMetrics{
            scale_round(table->win_ascent, em, table->units_per_em),
            scale_round(table->win_descent, em, table->units_per_em),
        };
    }
    const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
    if (os2 != nullptr && os2->version != k_os2_missing)
        return LineMetrics{
            scale_round(os2->usWinAscent, em, face->units_per_EM),
            scale_round(os2->usWinDescent, em, face->units_per_EM),
        };
    return LineMetrics{
        scale_round(face->ascender, em, face->units_per_EM),
        scale_round(-face->descender, em, face->units_per_EM),
    };
}

// The em whose cell is the tallest not exceeding a positive LOGFONT height.
int32_t em_for_cell(FT_Face face, const LineMetricsTable* table, int32_t cell) {
    int32_t best = 1;
    for (int32_t em = 1; em <= k_max_em; ++em) {
        const LineMetrics line = face_line_metrics(face, table, em);
        if (line.ascent + line.descent > cell)
            break;
        best = em;
    }
    return best;
}

int32_t external_leading(FT_Face face, int32_t em) {
    const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
    const auto* hhea = static_cast<const TT_HoriHeader*>(FT_Get_Sfnt_Table(face, FT_SFNT_HHEA));
    if (os2 == nullptr || hhea == nullptr)
        return 0;
    const int32_t gap = hhea->Line_Gap - ((hhea->Ascender - hhea->Descender) -
                                          (os2->usWinAscent + os2->usWinDescent));
    return gap > 0 ? scale_round(gap, em, face->units_per_EM) : 0;
}

int32_t average_width(FT_Face face, int32_t em) {
    int64_t sum = 0;
    for (const AverageWeight& entry : k_average_weights) {
        const FT_UInt glyph = FT_Get_Char_Index(face, static_cast<FT_ULong>(entry.character));
        FT_Fixed advance = 0;
        if (glyph == 0 || FT_Get_Advance(face, glyph, FT_LOAD_NO_SCALE, &advance) != 0) {
            const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
            return os2 != nullptr ? scale_round(os2->xAvgCharWidth, em, face->units_per_EM) : 0;
        }
        sum += static_cast<int64_t>(advance) * entry.weight;
    }
    return scale_round(static_cast<int32_t>(sum / k_average_divisor), em, face->units_per_EM);
}

} // namespace

struct FontLibrary {
    FT_Library freetype{};
    std::vector<LoadedFile> files{};
};

struct Font {
    FT_Face face{};
    FontRenderMode mode{};
    bool synthetic_bold{};
    bool synthetic_italic{};
    FontMetrics metrics{};
    FT_UInt glyphs[k_characters]{};
    int32_t advances[k_characters]{}; // whole pixels, simulated bold included
};

namespace {

FT_Int32 load_flags(const Font& font) {
    const FT_Int32 target =
        font.mode == FontRenderMode::mono ? FT_LOAD_TARGET_MONO : FT_LOAD_TARGET_NORMAL;
    return target | FT_LOAD_NO_AUTOHINT;
}

// Loads a hinted glyph into the face's slot with any simulated style
// applied to its outline.
bool load_glyph(const Font& font, FT_UInt glyph) {
    if (FT_Load_Glyph(font.face, glyph, load_flags(font)) != 0)
        return false;
    FT_GlyphSlot slot = font.face->glyph;
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE)
        return true;
    if (font.synthetic_italic) {
        FT_Matrix shear{k_unit_scale, k_oblique_shear, 0, k_unit_scale};
        FT_Outline_Transform(&slot->outline, &shear);
    }
    if (font.synthetic_bold) {
        FT_Outline_EmboldenXY(&slot->outline, k_embolden_width, 0);
        // Emboldening grows both sides; keep the left edge where it was.
        FT_Outline_Translate(&slot->outline, k_embolden_width / 2, 0);
    }
    return true;
}

const LoadedFile* find_file(const FontLibrary& library, const char* name) {
    for (const LoadedFile& file : library.files)
        if (std::strcmp(file.name, name) == 0)
            return &file;
    return nullptr;
}

void blit(
    const FT_Bitmap& bitmap,
    int32_t left,
    int32_t top,
    uint8_t* coverage,
    int32_t width,
    int32_t height,
    int32_t pitch
) {
    if (bitmap.pitch <= 0)
        return;
    for (uint32_t row = 0; row < bitmap.rows; ++row) {
        const int32_t y = top + static_cast<int32_t>(row);
        if (y < 0 || y >= height)
            continue;
        const uint8_t* source = bitmap.buffer + static_cast<std::ptrdiff_t>(row) * bitmap.pitch;
        uint8_t* target = coverage + static_cast<std::ptrdiff_t>(y) * pitch;
        for (uint32_t col = 0; col < bitmap.width; ++col) {
            const int32_t x = left + static_cast<int32_t>(col);
            if (x < 0 || x >= width)
                continue;
            uint8_t value = 0;
            if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
                value = (source[col / k_mono_bits] & (k_mono_first_bit >> (col % k_mono_bits)))
                            ? k_full_coverage
                            : 0;
            else if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY)
                value = source[col];
            if (value > target[x])
                target[x] = value;
        }
    }
}

} // namespace

FontLibrary* font_library_open(const char* fonts_dir, char* error, int32_t error_bytes) {
    auto* library = new (std::nothrow) FontLibrary{};
    if (library == nullptr)
        return nullptr;
    if (FT_Init_FreeType(&library->freetype) != 0) {
        set_error(error, error_bytes, "FreeType did not initialise", fonts_dir);
        delete library;
        return nullptr;
    }
    const char* const* names = nullptr;
    const int32_t count = bundled_font_files(&names);
    library->files.resize(static_cast<std::size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        LoadedFile& file = library->files[static_cast<std::size_t>(i)];
        file.name = names[i];
        const std::string path = std::string(fonts_dir) + "/" + names[i];
        if (!read_file(path, &file.bytes)) {
            set_error(error, error_bytes, "missing bundled font file", path);
            font_library_close(library);
            return nullptr;
        }
        if (!is_font_file(names[i]))
            continue;
        FT_Face face = nullptr;
        if (FT_New_Memory_Face(
                library->freetype,
                file.bytes.data(),
                static_cast<FT_Long>(file.bytes.size()),
                0,
                &face
            ) != 0) {
            set_error(error, error_bytes, "unreadable bundled font file", path);
            font_library_close(library);
            return nullptr;
        }
        FT_Done_Face(face);
    }
    return library;
}

void font_library_close(FontLibrary* library) {
    if (library == nullptr)
        return;
    FT_Done_FreeType(library->freetype);
    delete library;
}

Font* font_open(FontLibrary* library, const FontRequest& request) {
    FaceMatch match{};
    const int32_t weight = request.weight != 0 ? request.weight : k_weight_normal;
    if (library == nullptr || !face_resolve(request.face, weight, request.italic, &match))
        return nullptr;
    const LoadedFile* file = find_file(*library, match.file);
    if (file == nullptr)
        return nullptr;
    auto* font = new (std::nothrow) Font{};
    if (font == nullptr)
        return nullptr;
    if (FT_New_Memory_Face(
            library->freetype,
            file->bytes.data(),
            static_cast<FT_Long>(file->bytes.size()),
            0,
            &font->face
        ) != 0) {
        delete font;
        return nullptr;
    }
    FT_Face face = font->face;
    int32_t em = k_default_em_pixels;
    if (request.height < 0)
        em = -request.height;
    else if (request.height > 0)
        em = em_for_cell(face, match.line_metrics, request.height);
    if (em > k_max_em)
        em = k_max_em;
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(em)) != 0) {
        font_close(font);
        return nullptr;
    }
    font->mode = request.mode;
    font->synthetic_bold = match.synthetic_bold;
    font->synthetic_italic = match.synthetic_italic;

    const LineMetrics line = face_line_metrics(face, match.line_metrics, em);
    FontMetrics& metrics = font->metrics;
    metrics.em = em;
    metrics.ascent = line.ascent;
    metrics.descent = line.descent;
    metrics.height = line.ascent + line.descent;
    metrics.internal_leading = metrics.height - em;
    metrics.external_leading = external_leading(face, em);
    metrics.average_width = average_width(face, em);
    metrics.weight = weight >= k_weight_bold_threshold ? k_bold_weight : k_weight_normal;
    metrics.italic = request.italic;
    metrics.underline_rows = scale_round(face->underline_thickness, em, face->units_per_EM);
    if (metrics.underline_rows < 1)
        metrics.underline_rows = 1;
    const int32_t below_baseline = scale_round(-face->underline_position, em, face->units_per_EM);
    metrics.underline_top = line.ascent + below_baseline - metrics.underline_rows / 2;

    for (int32_t c = 0; c < k_characters; ++c) {
        font->glyphs[c] = FT_Get_Char_Index(face, code_point(static_cast<uint8_t>(c)));
        int32_t advance = 0;
        if (load_glyph(*font, font->glyphs[c]))
            advance = static_cast<int32_t>((face->glyph->advance.x + k_pixel / 2) / k_pixel);
        if (font->synthetic_bold)
            advance += k_embolden_advance;
        font->advances[c] = advance;
    }
    return font;
}

void font_close(Font* font) {
    if (font == nullptr)
        return;
    if (font->face != nullptr)
        FT_Done_Face(font->face);
    delete font;
}

FontMetrics font_metrics(const Font* font) {
    return font->metrics;
}

int32_t font_char_advance(const Font* font, uint8_t character) {
    return font->advances[character];
}

int32_t font_text_advance(const Font* font, const char* text, int32_t length) {
    int32_t width = 0;
    for (int32_t i = 0; i < length; ++i)
        width += font->advances[static_cast<uint8_t>(text[i])];
    return width;
}

void font_render(
    const Font* font,
    const char* text,
    int32_t length,
    uint8_t* coverage,
    int32_t width,
    int32_t height,
    int32_t pitch
) {
    const FT_Render_Mode mode =
        font->mode == FontRenderMode::mono ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL;
    FT_GlyphSlot slot = font->face->glyph;
    int32_t pen = 0;
    for (int32_t i = 0; i < length; ++i) {
        const auto character = static_cast<uint8_t>(text[i]);
        if (load_glyph(*font, font->glyphs[character]) && FT_Render_Glyph(slot, mode) == 0)
            blit(
                slot->bitmap,
                pen + slot->bitmap_left,
                font->metrics.ascent - slot->bitmap_top,
                coverage,
                width,
                height,
                pitch
            );
        pen += font->advances[character];
    }
}

bool font_has_character(const Font* font, uint32_t code_point) {
    if (font == nullptr)
        return false;
    const int32_t byte = cp1252_byte(code_point);
    if (byte >= 0)
        return font->glyphs[byte] != 0;
    return FT_Get_Char_Index(font->face, code_point) != 0;
}

int32_t font_codepoint_advance(const Font* font, uint32_t code_point) {
    if (font == nullptr)
        return 0;
    const int32_t byte = cp1252_byte(code_point);
    if (byte >= 0)
        return font->glyphs[byte] != 0 ? font->advances[byte] : 0;
    const FT_UInt glyph = FT_Get_Char_Index(font->face, code_point);
    if (glyph == 0 || !load_glyph(*font, glyph))
        return 0;
    int32_t advance = static_cast<int32_t>((font->face->glyph->advance.x + k_pixel / 2) / k_pixel);
    if (font->synthetic_bold)
        advance += k_embolden_advance;
    return advance;
}

void font_render_codepoint(
    const Font* font,
    uint32_t code_point,
    int32_t pen,
    uint8_t* coverage,
    int32_t width,
    int32_t height,
    int32_t pitch
) {
    if (font == nullptr || !font_has_character(font, code_point))
        return;
    const int32_t byte = cp1252_byte(code_point);
    const FT_UInt glyph =
        byte >= 0 ? font->glyphs[byte] : FT_Get_Char_Index(font->face, code_point);
    const FT_Render_Mode mode =
        font->mode == FontRenderMode::mono ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL;
    FT_GlyphSlot slot = font->face->glyph;
    if (load_glyph(*font, glyph) && FT_Render_Glyph(slot, mode) == 0)
        blit(
            slot->bitmap,
            pen + slot->bitmap_left,
            font->metrics.ascent - slot->bitmap_top,
            coverage,
            width,
            height,
            pitch
        );
}

} // namespace oa::platform
