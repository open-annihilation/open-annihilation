// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Draws a bitmap face with its own glyphs, and an analogue atlas for every
// character the face does not cover. The atlas is the file rasterize_font_atlas
// writes: little-endian, version 1, shared by the four faces.
#include "oa/boneyards/font_atlases.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

#include "oa/boneyards/toolkit.hpp"
#include "oa/boneyards/utf8.hpp"
#include "oa/formats/gaf.hpp"

namespace oa::boneyards {
namespace {

constexpr uint32_t k_atlas_magic = 0x41465942; // 'BYFA' as a little-endian word
constexpr int k_atlas_version = 1;
constexpr int k_glyphs = 256;
constexpr int k_line_height_glyph = 'I';
constexpr int k_max_name = 64;
constexpr int k_max_sources = 8;
constexpr uint32_t k_max_glyphs = 200000;
constexpr int k_max_side = 512;

struct Reader {
    const uint8_t* cursor = nullptr;
    const uint8_t* end = nullptr;
    bool ok = true;

    uint8_t u8() {
        if (cursor >= end) {
            ok = false;
            return 0;
        }
        return *cursor++;
    }

    uint16_t u16() {
        if (end - cursor < 2) {
            ok = false;
            return 0;
        }
        const uint16_t value = static_cast<uint16_t>(cursor[0] | (cursor[1] << 8));
        cursor += 2;
        return value;
    }

    uint32_t u32() {
        if (end - cursor < 4) {
            ok = false;
            return 0;
        }
        const uint32_t value =
            static_cast<uint32_t>(cursor[0]) | (static_cast<uint32_t>(cursor[1]) << 8) |
            (static_cast<uint32_t>(cursor[2]) << 16) | (static_cast<uint32_t>(cursor[3]) << 24);
        cursor += 4;
        return value;
    }

    int16_t i16() { return static_cast<int16_t>(u16()); }

    void skip(size_t count) {
        if (static_cast<size_t>(end - cursor) < count) {
            ok = false;
            cursor = end;
            return;
        }
        cursor += count;
    }

    void text(char* dest, int capacity) {
        const int length = u8();
        if (!ok || length <= 0 || length >= capacity || end - cursor < length) {
            ok = false;
            dest[0] = '\0';
            return;
        }
        std::memcpy(dest, cursor, static_cast<size_t>(length));
        dest[length] = '\0';
        cursor += length;
    }
};

struct AtlasGlyph {
    uint32_t codepoint = 0;
    int16_t advance = 0;
    int16_t left = 0;
    int16_t top = 0;
    uint16_t width = 0;
    uint16_t rows = 0;
    const uint8_t* bits = nullptr;
};

struct AtlasSource {
    char name[k_max_name]{};
    uint16_t pixels = 0;
    uint16_t cap_rows = 0;
    uint32_t glyph_count = 0;
    const AtlasGlyph* glyphs = nullptr;
};

struct AtlasFace {
    char name[k_max_name]{};
    int16_t line_height = 0;
    uint16_t cap_rows = 0;
    uint8_t source_count = 0;
    uint8_t sources[k_max_sources]{};
};

struct OwnGlyph {
    int32_t advance = 0;
    int16_t origin_x = 0;
    int16_t origin_y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t key = 0;
    const uint8_t* pixels = nullptr;
    bool present = false;
    bool covered = false;
};

struct OwnFace {
    int32_t line_height = 0;
    uint8_t body = 0;
    uint8_t edge = 0;
    bool shaded = false;
    OwnGlyph glyphs[k_glyphs]{};
    std::vector<uint8_t> storage;
};

bool bit_on(const AtlasGlyph& glyph, int column, int row) {
    const size_t bit = static_cast<size_t>(row) * glyph.width + static_cast<size_t>(column);
    return (glyph.bits[bit >> 3] & (0x80u >> (bit & 7))) != 0;
}

const AtlasGlyph* find_glyph(const AtlasSource& source, uint32_t codepoint) {
    uint32_t low = 0;
    uint32_t high = source.glyph_count;
    while (low < high) {
        const uint32_t mid = low + (high - low) / 2;
        const uint32_t found = source.glyphs[mid].codepoint;
        if (found == codepoint) {
            return &source.glyphs[mid];
        }
        if (found < codepoint) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return nullptr;
}

/// Decodes UTF-8. A bad sequence rejects the whole string.
bool decode_utf8(const char* text, std::vector<uint32_t>& codes) {
    if (text == nullptr) {
        return false;
    }
    for (int32_t at = 0; text[at] != '\0';) {
        const utf8::Unit unit = utf8::read(text + at);
        if (!unit.well_formed || unit.bytes <= 0) {
            return false;
        }
        if (unit.code >= 0xD800 && unit.code <= 0xDFFF) {
            return false;
        }
        codes.push_back(unit.code);
        at += unit.bytes;
        if (codes.size() > k_max_glyphs) {
            return false;
        }
    }
    return true;
}

bool same_picture(const OwnGlyph& a, const OwnGlyph& b) {
    if (!a.present || !b.present || a.width != b.width || a.height != b.height ||
        a.origin_x != b.origin_x || a.origin_y != b.origin_y || a.key != b.key) {
        return false;
    }
    const size_t count = static_cast<size_t>(a.width) * a.height;
    return count == 0 || std::memcmp(a.pixels, b.pixels, count) == 0;
}

/// The colour most of the ink uses, and the colour on the ink's right edge
/// when that differs. A face painted in one colour stays flat.
void learn_ink(OwnFace& face) {
    int counts[k_glyphs] = {};
    int edges[k_glyphs] = {};
    for (const OwnGlyph& glyph : face.glyphs) {
        if (!glyph.covered || glyph.pixels == nullptr) {
            continue;
        }
        for (int row = 0; row < glyph.height; ++row) {
            const uint8_t* line = glyph.pixels + static_cast<size_t>(row) * glyph.width;
            for (int column = 0; column < glyph.width; ++column) {
                const uint8_t pixel = line[column];
                if (pixel == glyph.key) {
                    continue;
                }
                ++counts[pixel];
                const bool right_clear = column + 1 >= glyph.width || line[column + 1] == glyph.key;
                if (right_clear) {
                    ++edges[pixel];
                }
            }
        }
    }
    int body = 0;
    int edge = 0;
    for (int index = 1; index < k_glyphs; ++index) {
        if (counts[index] > counts[body]) {
            body = index;
        }
        if (edges[index] > edges[edge]) {
            edge = index;
        }
    }
    if (counts[body] == 0) {
        body = k_overlay_transparent;
    }
    if (edges[edge] == 0 || edge == k_overlay_transparent) {
        edge = body;
    }
    face.body = static_cast<uint8_t>(body);
    face.edge = static_cast<uint8_t>(edge);
    face.shaded = face.edge != face.body;
}

bool load_face(const uint8_t* gaf, size_t gaf_size, OwnFace& face) {
    const auto parsed = formats::gaf::parse(std::span<const uint8_t>(gaf, gaf_size));
    if (!parsed.ok() || parsed.archive->sequences.empty()) {
        return false;
    }
    const formats::gaf::Sequence& sequence = parsed.archive->sequences[0];
    const int count = static_cast<int>(sequence.frames.size()) < k_glyphs
                          ? static_cast<int>(sequence.frames.size())
                          : k_glyphs;
    size_t bytes = 0;
    for (int index = 0; index < count; ++index) {
        const auto rendered =
            formats::gaf::render_normal(sequence.frames[static_cast<size_t>(index)]);
        if (!rendered.ok()) {
            continue;
        }
        bytes += rendered.frame->pixels.size();
    }
    face.storage.resize(bytes);
    size_t used = 0;
    for (int index = 0; index < count; ++index) {
        const auto rendered =
            formats::gaf::render_normal(sequence.frames[static_cast<size_t>(index)]);
        if (!rendered.ok()) {
            continue;
        }
        const formats::gaf::RenderedFrame& image = *rendered.frame;
        if (image.pixels.size() != static_cast<size_t>(image.width) * image.height) {
            continue;
        }
        OwnGlyph& glyph = face.glyphs[index];
        glyph.present = true;
        glyph.width = image.width;
        glyph.height = image.height;
        glyph.origin_x = image.origin_x;
        glyph.origin_y = image.origin_y;
        glyph.key = image.transparency_index;
        glyph.advance = static_cast<int32_t>(image.width) - image.origin_x;
        glyph.pixels = face.storage.data() + used;
        if (!image.pixels.empty()) {
            std::memcpy(face.storage.data() + used, image.pixels.data(), image.pixels.size());
            used += image.pixels.size();
        }
    }
    for (int index = 0; index < k_glyphs; ++index) {
        OwnGlyph& glyph = face.glyphs[index];
        const bool ascii = index >= 0x20 && index <= 0x7E;
        glyph.covered = glyph.present && (ascii || !same_picture(glyph, face.glyphs[0]));
    }
    if (!face.glyphs[k_line_height_glyph].present) {
        return false;
    }
    face.line_height = face.glyphs[k_line_height_glyph].height;
    learn_ink(face);
    return face.line_height > 0;
}

void ignore_extension(char* name) {
    char* dot = nullptr;
    for (char* cursor = name; *cursor != '\0'; ++cursor) {
        if (*cursor == '.') {
            dot = cursor;
        }
    }
    if (dot != nullptr && dot != name) {
        *dot = '\0';
    }
}

/// The file's base name: directories and a final extension are not part of
/// the face's name.
void face_key(const char* name, char* dest, int capacity) {
    const char* start = name;
    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            start = cursor + 1;
        }
    }
    std::snprintf(dest, static_cast<size_t>(capacity), "%s", start);
    ignore_extension(dest);
}

bool same_name(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        const auto left = static_cast<unsigned char>(*a);
        const auto right = static_cast<unsigned char>(*b);
        const int folded_left = left >= 'A' && left <= 'Z' ? left - 'A' + 'a' : left;
        const int folded_right = right >= 'A' && right <= 'Z' ? right - 'A' + 'a' : right;
        if (folded_left != folded_right) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

void plot(
    Surface* target, int32_t x, int32_t y, uint8_t color, int32_t& ink_top, int32_t& ink_bottom
) {
    if (y < target->clip.y1 || y > target->clip.y2 || x < target->clip.x1 || x > target->clip.x2) {
        return;
    }
    target->pixels[static_cast<ptrdiff_t>(y) * target->pitch + x] = color;
    if (ink_top < 0 || y < ink_top) {
        ink_top = y;
    }
    if (y > ink_bottom) {
        ink_bottom = y;
    }
}

void draw_own(
    Surface* target,
    int32_t pen_x,
    int32_t pen_y,
    const OwnGlyph& glyph,
    int32_t& ink_top,
    int32_t& ink_bottom
) {
    if (glyph.pixels == nullptr) {
        return;
    }
    const int32_t left = pen_x - glyph.origin_x;
    const int32_t top = pen_y - glyph.origin_y;
    for (int row = 0; row < glyph.height; ++row) {
        const uint8_t* line = glyph.pixels + static_cast<size_t>(row) * glyph.width;
        for (int column = 0; column < glyph.width; ++column) {
            if (line[column] == glyph.key) {
                continue;
            }
            plot(target, left + column, top + row, line[column], ink_top, ink_bottom);
        }
    }
}

struct Ink {
    uint8_t body = 0;
    uint8_t edge = 0;
    bool shaded = false;
};

void draw_analogue(
    Surface* target,
    int32_t pen_x,
    int32_t pen_y,
    const AtlasGlyph& glyph,
    const Ink& ink,
    int32_t& ink_top,
    int32_t& ink_bottom
) {
    const int32_t left = pen_x + glyph.left;
    const int32_t top = pen_y - glyph.top;
    for (int row = 0; row < glyph.rows; ++row) {
        for (int column = 0; column < glyph.width; ++column) {
            if (!bit_on(glyph, column, row)) {
                continue;
            }
            const bool right_clear = column + 1 >= glyph.width || !bit_on(glyph, column + 1, row);
            const uint8_t color = ink.shaded && right_clear ? ink.edge : ink.body;
            plot(target, left + column, top + row, color, ink_top, ink_bottom);
        }
    }
}

} // namespace

struct FontAtlas {
    std::vector<uint8_t> bytes;
    std::vector<AtlasGlyph> glyphs;
    std::vector<AtlasSource> sources;
    std::vector<AtlasFace> faces;
};

namespace {

const AtlasGlyph* glyph_in_face(const FontAtlas& atlas, const AtlasFace& face, uint32_t code) {
    for (uint8_t source_index = 0; source_index < face.source_count; ++source_index) {
        const AtlasGlyph* glyph = find_glyph(atlas.sources[face.sources[source_index]], code);
        if (glyph != nullptr) {
            return glyph;
        }
    }
    return nullptr;
}

} // namespace

const AtlasFace* find_face(const FontAtlas& atlas, const char* name) {
    char key[k_max_name];
    face_key(name, key, k_max_name);
    for (const AtlasFace& face : atlas.faces) {
        if (same_name(face.name, key)) {
            return &face;
        }
    }
    return nullptr;
}

FontAtlas* font_atlas_open(const uint8_t* bytes, size_t size) {
    if (bytes == nullptr || size < 10) {
        return nullptr;
    }
    Reader reader{bytes, bytes + size, true};
    if (reader.u32() != k_atlas_magic || reader.u16() != k_atlas_version) {
        return nullptr;
    }
    const uint16_t source_count = reader.u16();
    const uint16_t face_count = reader.u16();
    if (!reader.ok || source_count == 0 || source_count > 16 || face_count == 0 ||
        face_count > 16) {
        return nullptr;
    }
    auto* atlas = new FontAtlas;
    atlas->bytes.assign(bytes, bytes + size);
    // Re-read from the owned copy so glyph pictures point at it.
    reader = Reader{atlas->bytes.data(), atlas->bytes.data() + atlas->bytes.size(), true};
    reader.skip(4 + 2 + 2 + 2);
    uint32_t glyph_total = 0;

    struct Pending {
        char name[k_max_name];
        uint16_t pixels;
        uint16_t cap_rows;
        uint32_t count;
        size_t first;
    };

    std::vector<Pending> pending(source_count);
    for (uint16_t source_index = 0; source_index < source_count; ++source_index) {
        Pending& source = pending[source_index];
        reader.text(source.name, k_max_name);
        source.pixels = reader.u16();
        source.cap_rows = reader.u16();
        source.count = reader.u32();
        if (!reader.ok || source.count > k_max_glyphs ||
            glyph_total > k_max_glyphs - source.count) {
            delete atlas;
            return nullptr;
        }
        source.first = glyph_total;
        glyph_total += source.count;
        uint32_t previous = 0;
        bool first = true;
        for (uint32_t glyph_index = 0; glyph_index < source.count; ++glyph_index) {
            AtlasGlyph glyph;
            glyph.codepoint = reader.u32();
            glyph.advance = reader.i16();
            glyph.left = reader.i16();
            glyph.top = reader.i16();
            glyph.width = reader.u16();
            glyph.rows = reader.u16();
            if (!reader.ok || glyph.width > k_max_side || glyph.rows > k_max_side ||
                (!first && glyph.codepoint <= previous) || glyph.codepoint > 0x10FFFF) {
                delete atlas;
                return nullptr;
            }
            const size_t bits = (static_cast<size_t>(glyph.width) * glyph.rows + 7) / 8;
            if (static_cast<size_t>(reader.end - reader.cursor) < bits) {
                delete atlas;
                return nullptr;
            }
            glyph.bits = bits == 0 ? nullptr : reader.cursor;
            reader.skip(bits);
            atlas->glyphs.push_back(glyph);
            previous = glyph.codepoint;
            first = false;
        }
    }
    atlas->sources.resize(source_count);
    for (uint16_t source_index = 0; source_index < source_count; ++source_index) {
        AtlasSource& source = atlas->sources[source_index];
        std::memcpy(source.name, pending[source_index].name, k_max_name);
        source.pixels = pending[source_index].pixels;
        source.cap_rows = pending[source_index].cap_rows;
        source.glyph_count = pending[source_index].count;
        source.glyphs =
            source.glyph_count == 0 ? nullptr : atlas->glyphs.data() + pending[source_index].first;
    }
    atlas->faces.resize(face_count);
    for (uint16_t face_index = 0; face_index < face_count; ++face_index) {
        AtlasFace& face = atlas->faces[face_index];
        reader.text(face.name, k_max_name);
        face.line_height = reader.i16();
        face.cap_rows = reader.u16();
        face.source_count = reader.u8();
        if (!reader.ok || face.source_count == 0 || face.source_count > k_max_sources) {
            delete atlas;
            return nullptr;
        }
        for (uint8_t index = 0; index < face.source_count; ++index) {
            face.sources[index] = reader.u8();
            if (!reader.ok || face.sources[index] >= source_count) {
                delete atlas;
                return nullptr;
            }
        }
    }
    if (!reader.ok || reader.cursor != reader.end) {
        delete atlas;
        return nullptr;
    }
    return atlas;
}

void font_atlas_close(FontAtlas* atlas) {
    delete atlas;
}

int32_t font_atlas_draw(
    Surface* target,
    const uint8_t* gaf,
    size_t gaf_size,
    const char* face_name,
    const FontAtlas* atlas,
    const char* text,
    int32_t x,
    int32_t y,
    FontAtlasGlyph* places,
    int32_t places_capacity
) {
    if (target == nullptr || target->pixels == nullptr || gaf == nullptr || face_name == nullptr ||
        atlas == nullptr || text == nullptr) {
        return -1;
    }
    const AtlasFace* analogue = find_face(*atlas, face_name);
    if (analogue == nullptr) {
        return -1;
    }
    std::vector<uint32_t> codes;
    if (!decode_utf8(text, codes)) {
        return -1;
    }
    OwnFace face;
    if (!load_face(gaf, gaf_size, face)) {
        return -1;
    }
    const int32_t pen_y = y + face.line_height;
    int32_t pen_x = x;
    for (size_t index = 0; index < codes.size(); ++index) {
        const uint32_t code = codes[index];
        FontAtlasGlyph place{};
        place.codepoint = code;
        place.x = pen_x;
        place.ink_top = -1;
        place.ink_bottom = -1;
        const bool own = code < k_glyphs && face.glyphs[code].covered;
        if (own) {
            const OwnGlyph& glyph = face.glyphs[code];
            // A space keeps its advance and is not blitted, matching the
            // byte-oriented run: its frame may hold a pixel, but the player
            // sees a gap.
            if (code != static_cast<uint32_t>(' ')) {
                draw_own(target, pen_x, pen_y, glyph, place.ink_top, place.ink_bottom);
            }
            place.advance = glyph.advance;
            place.original = 1;
        } else {
            const AtlasGlyph* glyph = glyph_in_face(*atlas, *analogue, code);
            if (glyph != nullptr) {
                const Ink ink{face.body, face.edge, face.shaded};
                draw_analogue(target, pen_x, pen_y, *glyph, ink, place.ink_top, place.ink_bottom);
                place.advance = glyph->advance;
            }
        }
        if (places != nullptr && static_cast<int32_t>(index) < places_capacity) {
            places[index] = place;
        }
        pen_x += place.advance;
    }
    return static_cast<int32_t>(codes.size());
}

bool font_atlas_lookup(
    const FontAtlas* atlas, const char* face_name, uint32_t code, FontAtlasPlacement* place
) {
    if (place != nullptr) {
        *place = {};
    }
    if (atlas == nullptr || face_name == nullptr || place == nullptr) {
        return false;
    }
    const AtlasFace* face = find_face(*atlas, face_name);
    if (face == nullptr) {
        return false;
    }
    const AtlasGlyph* glyph = glyph_in_face(*atlas, *face, code);
    if (glyph == nullptr) {
        return false;
    }
    place->advance = glyph->advance;
    place->left = glyph->left;
    place->top = glyph->top;
    place->width = glyph->width;
    place->rows = glyph->rows;
    return true;
}

int32_t font_atlas_plot(
    Surface* target,
    const FontAtlas* atlas,
    const char* face_name,
    uint32_t code,
    int32_t pen_x,
    int32_t pen_y,
    uint8_t body,
    uint8_t edge,
    bool shaded
) {
    if (target == nullptr || atlas == nullptr || face_name == nullptr) {
        return 0;
    }
    const AtlasFace* face = find_face(*atlas, face_name);
    if (face == nullptr) {
        return 0;
    }
    const AtlasGlyph* glyph = glyph_in_face(*atlas, *face, code);
    if (glyph == nullptr) {
        return 0;
    }
    int32_t ink_top = -1;
    int32_t ink_bottom = -1;
    const Ink ink{body, edge, shaded};
    draw_analogue(target, pen_x, pen_y, *glyph, ink, ink_top, ink_bottom);
    return glyph->advance;
}

} // namespace oa::boneyards
