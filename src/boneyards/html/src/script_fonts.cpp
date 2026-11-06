// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The pages' glyph chain, drawn with FreeType as one-bit coverage.

#include "oa/boneyards/html_scripts.hpp"

#include "oa/platform/text_font.hpp"

#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H

namespace oa::boneyards::html {
namespace {

constexpr int32_t k_max_em = 255;
constexpr int32_t k_emoji_weight = 400;
constexpr FT_Pos k_pixel = 64;
constexpr uint8_t k_full = 0xFF;
constexpr int32_t k_cp1252_high_first = 0x80;
constexpr int32_t k_cp1252_high_count = 0x20;
constexpr uint16_t k_cp1252_high[k_cp1252_high_count] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

// DejaVu takes the page face's em. CJK and emoji take the size the engine
// draws beside a sans face of that em: six sevenths, but no less than the
// em itself or 12, whichever is smaller.
int32_t related_em(int32_t em) {
    const int32_t related = (em * 6 + 3) / 7;
    const int32_t floor = em < 12 ? em : 12;
    return related > floor ? related : floor;
}

bool read_file(const char* path, std::vector<uint8_t>* out) {
    if (path == nullptr || path[0] == '\0')
        return false;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (!file || size <= 0)
        return false;
    file.seekg(0, std::ios::beg);
    out->resize(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(size));
    if (!file || file.gcount() != static_cast<std::streamsize>(size)) {
        out->clear();
        return false;
    }
    return true;
}

enum class Slot : int32_t { dejavu, dejavu_bold, cjk, emoji, count };

struct FaceSlot {
    std::vector<uint8_t> bytes;
    FT_Face face = nullptr;
    int32_t pixel_size = 0;
    bool related = false; // CJK and emoji follow related_em
};

} // namespace

struct FallbackFonts {
    FT_Library library = nullptr;
    FaceSlot faces[static_cast<int32_t>(Slot::count)]{};
    std::vector<uint8_t> scratch;
    // Parent of the DejaVu file: the game's fonts folder, which also holds
    // the engine's endonym face and any language-pack faces.
    std::filesystem::path text_fonts;
    // Filled on the first CJK lookup. fallback_has is const; the open is not.
    mutable std::unique_ptr<oa::platform::text_font::FontStack> cjk;
    mutable bool cjk_tried = false;
};

namespace {

bool set_size(FaceSlot& slot, int32_t em) {
    if (em < 1)
        em = 1;
    if (em > k_max_em)
        em = k_max_em;
    const int32_t pixels = slot.related ? related_em(em) : em;
    if (slot.pixel_size == pixels)
        return true;
    if (FT_Set_Pixel_Sizes(slot.face, 0, static_cast<FT_UInt>(pixels)) != 0)
        return false;
    slot.pixel_size = pixels;
    return true;
}

bool set_emoji_weight(FT_Library library, FT_Face face) {
    if (!FT_HAS_MULTIPLE_MASTERS(face))
        return true;
    FT_MM_Var* axes = nullptr;
    if (FT_Get_MM_Var(face, &axes) != 0)
        return false;
    std::vector<FT_Fixed> coordinates(axes->num_axis);
    bool found = false;
    for (FT_UInt axis = 0; axis < axes->num_axis; ++axis) {
        const FT_Var_Axis& found_axis = axes->axis[axis];
        if (found_axis.tag != FT_MAKE_TAG('w', 'g', 'h', 't')) {
            coordinates[axis] = found_axis.def;
            continue;
        }
        FT_Fixed weight = static_cast<FT_Fixed>(k_emoji_weight) * 65536;
        if (weight < found_axis.minimum)
            weight = found_axis.minimum;
        if (weight > found_axis.maximum)
            weight = found_axis.maximum;
        coordinates[axis] = weight;
        found = true;
    }
    const bool ok =
        found && FT_Set_Var_Design_Coordinates(face, axes->num_axis, coordinates.data()) == 0;
    FT_Done_MM_Var(library, axes);
    return ok;
}

FT_Face open_face(FT_Library library, std::vector<uint8_t>* bytes, int32_t index) {
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(
            library, bytes->data(), static_cast<FT_Long>(bytes->size()), index, &face
        ) != 0)
        return nullptr;
    return face;
}

FT_Face open_cjk(FT_Library library, std::vector<uint8_t>* bytes) {
    for (int32_t index = 0; index < 16; ++index) {
        FT_Face face = open_face(library, bytes, index);
        if (face == nullptr)
            break;
        const bool match = face->family_name != nullptr && face->style_name != nullptr &&
                           std::strcmp(face->family_name, "Noto Sans CJK SC") == 0 &&
                           std::strcmp(face->style_name, "Medium") == 0;
        if (match)
            return face;
        FT_Done_Face(face);
    }
    return nullptr;
}

bool open_slot(FallbackFonts& fonts, Slot slot, const char* path, bool collection, bool related) {
    FaceSlot& face = fonts.faces[static_cast<int32_t>(slot)];
    if (!read_file(path, &face.bytes))
        return false;
    face.face = collection ? open_cjk(fonts.library, &face.bytes)
                           : open_face(fonts.library, &face.bytes, 0);
    if (face.face == nullptr) {
        face.bytes.clear();
        return false;
    }
    face.related = related;
    if (slot == Slot::emoji && !set_emoji_weight(fonts.library, face.face)) {
        FT_Done_Face(face.face);
        face.face = nullptr;
        face.bytes.clear();
        return false;
    }
    return true;
}

void unpack(const FT_Bitmap& bitmap, std::vector<uint8_t>* coverage, ScriptGlyph* out) {
    out->width = static_cast<int32_t>(bitmap.width);
    out->rows = static_cast<int32_t>(bitmap.rows);
    if (bitmap.pitch <= 0 || out->width <= 0 || out->rows <= 0 || bitmap.buffer == nullptr) {
        out->width = 0;
        out->rows = 0;
        out->coverage = nullptr;
        return;
    }
    coverage->assign(static_cast<std::size_t>(out->width) * static_cast<std::size_t>(out->rows), 0);
    for (int32_t row = 0; row < out->rows; ++row) {
        const uint8_t* source = bitmap.buffer + static_cast<std::ptrdiff_t>(row) * bitmap.pitch;
        uint8_t* target = coverage->data() + static_cast<std::ptrdiff_t>(row) * out->width;
        for (int32_t col = 0; col < out->width; ++col) {
            if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
                if ((source[col / 8] & (0x80u >> (col % 8))) != 0)
                    target[col] = k_full;
            } else if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
                target[col] = source[col];
            }
        }
    }
    out->coverage = coverage->data();
}

bool draw_slot(FallbackFonts& fonts, Slot slot, uint32_t code_point, int32_t em, ScriptGlyph* out) {
    FaceSlot& face = fonts.faces[static_cast<int32_t>(slot)];
    if (face.face == nullptr || !set_size(face, em))
        return false;
    const FT_UInt glyph = FT_Get_Char_Index(face.face, code_point);
    if (glyph == 0)
        return false;
    if (FT_Load_Glyph(face.face, glyph, FT_LOAD_TARGET_MONO | FT_LOAD_NO_AUTOHINT) != 0)
        return false;
    FT_GlyphSlot slot_glyph = face.face->glyph;
    out->advance = static_cast<int32_t>((slot_glyph->advance.x + k_pixel / 2) / k_pixel);
    if (FT_Render_Glyph(slot_glyph, FT_RENDER_MODE_MONO) != 0)
        return false;
    out->left = slot_glyph->bitmap_left;
    out->top = slot_glyph->bitmap_top;
    unpack(slot_glyph->bitmap, &fonts.scratch, out);
    return true;
}

// UTF-8, or nullopt-style failure through a bool. Overlong forms, surrogates
// and values past U+10FFFF are rejected.
bool decode_utf8(const char* text, int32_t length, std::vector<uint32_t>* out) {
    out->clear();
    for (int32_t at = 0; at < length;) {
        const auto lead = static_cast<uint8_t>(text[at]);
        int32_t size = 1;
        uint32_t value = lead;
        uint32_t least = 0;
        if (lead >= 0xF0 && lead <= 0xF4) {
            size = 4;
            value = lead & 0x07u;
            least = 0x10000;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            size = 3;
            value = lead & 0x0Fu;
            least = 0x800;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            size = 2;
            value = lead & 0x1Fu;
            least = 0x80;
        } else if (lead >= 0x80) {
            return false;
        }
        if (length - at < size)
            return false;
        for (int32_t next = 1; next < size; ++next) {
            const auto byte = static_cast<uint8_t>(text[at + next]);
            if ((byte & 0xC0u) != 0x80u)
                return false;
            value = (value << 6) | (byte & 0x3Fu);
        }
        if (value < least || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
            return false;
        out->push_back(value);
        at += size;
    }
    return true;
}

std::string g_directory;
FallbackFonts* g_shared = nullptr;
FallbackFonts* g_override = nullptr;
bool g_tried = false;

std::filesystem::path text_font_directory(const char* path) {
    if (path == nullptr || path[0] == '\0')
        return {};
    return std::filesystem::path(path).parent_path();
}

// One code point, as UTF-8. Empty when the value is not a scalar.
std::string utf8_of(uint32_t code_point) {
    std::string text;
    if (code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF))
        return text;
    if (code_point < 0x80) {
        text.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        text.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        text.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point < 0x10000) {
        text.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        text.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        text.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        text.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
    return text;
}

// CJK comes from the engine's chain: the endonym face, or a language pack's
// face. DejaVu and Noto Emoji stay on this chain's own slots, so a character
// those faces hold is not taken from the engine's copy of them.
bool cjk_face_wanted(oa::platform::text_font::Face which) {
    using oa::platform::text_font::face_count;
    using oa::platform::text_font::most_pack_faces;
    if (which == oa::platform::text_font::Face::endonyms)
        return true;
    const auto index = static_cast<std::size_t>(which);
    return index >= face_count && index < face_count + most_pack_faces;
}

oa::platform::text_font::FontStack* cjk_stack(const FallbackFonts& fonts) {
    if (fonts.cjk_tried)
        return fonts.cjk.get();
    fonts.cjk_tried = true;
    if (fonts.text_fonts.empty())
        return nullptr;
    try {
        fonts.cjk = oa::platform::text_font::FontStack::open(fonts.text_fonts);
    } catch (const std::exception&) {
        fonts.cjk.reset();
    }
    return fonts.cjk.get();
}

bool cjk_wanted(const FallbackFonts& fonts, uint32_t code_point, bool bold) {
    using namespace oa::platform::text_font;
    FontStack* stack = cjk_stack(fonts);
    if (stack == nullptr)
        return false;
    const std::string text = utf8_of(code_point);
    if (text.empty())
        return false;
    const Weight weight = bold ? Weight::bold : Weight::regular;
    if (!stack->draws(text, weight))
        return false;
    const std::optional<Face> chosen = stack->face_for(static_cast<char32_t>(code_point), weight);
    return chosen.has_value() && cjk_face_wanted(*chosen);
}

bool draw_cjk(FallbackFonts& fonts, uint32_t code_point, int32_t em, bool bold, ScriptGlyph* out) {
    using namespace oa::platform::text_font;
    if (!cjk_wanted(fonts, code_point, bold) || fonts.cjk == nullptr)
        return false;
    int32_t pixels = em;
    if (pixels < 1)
        pixels = 1;
    if (pixels > max_pixel_size)
        pixels = max_pixel_size;
    Style style;
    style.pixel_size = pixels;
    style.weight = bold ? Weight::bold : Weight::regular;
    style.rendering = Rendering::mono;
    style.least_cjk_pixel_size = related_em(pixels);
    const std::optional<Coverage> coverage = fonts.cjk->draw(utf8_of(code_point), style);
    if (!coverage.has_value() || coverage->width < 0 || coverage->height < 0)
        return false;
    const auto bytes =
        static_cast<std::size_t>(coverage->width) * static_cast<std::size_t>(coverage->height);
    if (coverage->alpha.size() != bytes)
        return false;
    fonts.scratch = coverage->alpha;
    out->advance = coverage->advance;
    out->left = -coverage->origin;
    out->top = coverage->baseline;
    out->width = coverage->width;
    out->rows = coverage->height;
    out->coverage = fonts.scratch.empty() ? nullptr : fonts.scratch.data();
    return true;
}

} // namespace

int32_t markup_cp1252_byte(uint32_t code_point) {
    if (code_point < 0x80 || (code_point >= 0xA0 && code_point <= 0xFF))
        return static_cast<int32_t>(code_point);
    for (int32_t i = 0; i < k_cp1252_high_count; ++i)
        if (k_cp1252_high[i] == code_point)
            return k_cp1252_high_first + i;
    return -1;
}

bool markup_character_visible(uint32_t code_point) {
    if (code_point < 0x20)
        return false;
    if (code_point >= 0x7F && code_point < 0xA0)
        return false;
    if (code_point >= 0x200B && code_point <= 0x200F)
        return false;
    if (code_point == 0x2060 || code_point == 0xFEFF)
        return false;
    if (code_point >= 0xFE00 && code_point <= 0xFE0F)
        return false;
    if (code_point >= 0xE0100 && code_point <= 0xE01EF)
        return false;
    return true;
}

int32_t markup_characters(const char* text, int32_t length, uint32_t* out, int32_t capacity) {
    if (text == nullptr || length <= 0)
        return 0;
    std::vector<uint32_t> decoded;
    const bool utf8 = decode_utf8(text, length, &decoded);
    if (!utf8) {
        decoded.resize(static_cast<std::size_t>(length));
        for (int32_t i = 0; i < length; ++i) {
            const auto byte = static_cast<uint8_t>(text[i]);
            decoded[static_cast<std::size_t>(i)] =
                byte >= k_cp1252_high_first && byte < k_cp1252_high_first + k_cp1252_high_count
                    ? k_cp1252_high[byte - k_cp1252_high_first]
                    : byte;
        }
    }
    const int32_t count = static_cast<int32_t>(decoded.size());
    const int32_t write = count < capacity ? count : capacity;
    for (int32_t i = 0; i < write; ++i)
        out[i] = decoded[static_cast<std::size_t>(i)];
    return count;
}

void page_fonts_set_directory(const char* directory) {
    const std::string next = directory != nullptr ? directory : "";
    if (next == g_directory)
        return;
    g_directory = next;
    g_tried = false;
    if (g_shared != nullptr) {
        fallback_fonts_close(g_shared);
        g_shared = nullptr;
    }
}

void page_script_fonts_use(FallbackFonts* fonts) {
    g_override = fonts;
}

FallbackFonts* page_script_fonts() {
    if (g_override != nullptr)
        return g_override;
    if (g_tried || g_directory.empty())
        return g_shared;
    g_tried = true;
    const std::string dejavu = g_directory + "/DejaVuSans.ttf";
    const std::string dejavu_bold = g_directory + "/DejaVuSans-Bold.ttf";
    const std::string emoji = g_directory + "/NotoEmoji.ttf";
    const FallbackFiles files{dejavu.c_str(), dejavu_bold.c_str(), nullptr, emoji.c_str()};
    g_shared = fallback_fonts_open_files(files);
    return g_shared;
}

FallbackFonts* fallback_fonts_open_files(const FallbackFiles& files) {
    auto* fonts = new (std::nothrow) FallbackFonts{};
    if (fonts == nullptr)
        return nullptr;
    if (FT_Init_FreeType(&fonts->library) != 0) {
        delete fonts;
        return nullptr;
    }
    fonts->text_fonts = text_font_directory(files.dejavu_regular);
    if (fonts->text_fonts.empty())
        fonts->text_fonts = text_font_directory(files.dejavu_bold);
    bool any = false;
    any = open_slot(*fonts, Slot::dejavu, files.dejavu_regular, false, false) || any;
    any = open_slot(*fonts, Slot::dejavu_bold, files.dejavu_bold, false, false) || any;
    // files.cjk_medium is ignored. CJK is the engine's chain, opened on use
    // from the DejaVu file's directory. It does not by itself keep the chain.
    any = open_slot(*fonts, Slot::emoji, files.emoji, false, true) || any;
    if (!any) {
        fallback_fonts_close(fonts);
        return nullptr;
    }
    return fonts;
}

void fallback_fonts_close(FallbackFonts* fonts) {
    if (fonts == nullptr)
        return;
    for (FaceSlot& face : fonts->faces)
        if (face.face != nullptr)
            FT_Done_Face(face.face);
    if (fonts->library != nullptr)
        FT_Done_FreeType(fonts->library);
    delete fonts;
}

bool fallback_lookup(void* user, uint32_t code_point, int32_t em, bool bold, ScriptGlyph* out) {
    auto* fonts = static_cast<FallbackFonts*>(user);
    if (fonts == nullptr || out == nullptr)
        return false;
    const Slot order_bold[] = {Slot::dejavu_bold, Slot::dejavu, Slot::cjk, Slot::emoji};
    const Slot order_regular[] = {Slot::dejavu, Slot::cjk, Slot::emoji};
    const Slot* order = bold ? order_bold : order_regular;
    const int32_t count = bold ? 4 : 3;
    for (int32_t i = 0; i < count; ++i) {
        if (order[i] == Slot::cjk) {
            if (draw_cjk(*fonts, code_point, em, bold, out))
                return true;
            continue;
        }
        if (draw_slot(*fonts, order[i], code_point, em, out))
            return true;
    }
    return false;
}

bool fallback_has(const FallbackFonts* fonts, uint32_t code_point, bool bold) {
    if (fonts == nullptr)
        return false;
    const Slot order_bold[] = {Slot::dejavu_bold, Slot::dejavu, Slot::cjk, Slot::emoji};
    const Slot order_regular[] = {Slot::dejavu, Slot::cjk, Slot::emoji};
    const Slot* order = bold ? order_bold : order_regular;
    const int32_t count = bold ? 4 : 3;
    for (int32_t i = 0; i < count; ++i) {
        if (order[i] == Slot::cjk) {
            if (cjk_wanted(*fonts, code_point, bold))
                return true;
            continue;
        }
        const FaceSlot& face = fonts->faces[static_cast<int32_t>(order[i])];
        if (face.face != nullptr && FT_Get_Char_Index(face.face, code_point) != 0)
            return true;
    }
    return false;
}

} // namespace oa::boneyards::html
