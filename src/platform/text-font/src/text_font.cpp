// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The font stack: FreeType reads the fonts from their files through a stream
// of the engine's own, so that a folder whose path is not in the system's
// code page opens on Windows too, and draws each glyph once per size, weight
// and rendering. The interface faces, Barlow and Barlow Condensed, go first
// in their families' chains; the base faces draw what they lack.

#include "oa/platform/text_font.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_MULTIPLE_MASTERS_H

#include <algorithm>
#include <cstdio>
#if defined(_WIN32)
#include <share.h>
#endif
#include <cstdlib>
#include <list>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace oa::platform::text_font {

namespace {

/// The design weight of Noto Emoji's weight axis for each Weight, in the
/// order of its values: regular, bold, medium and semibold.
constexpr std::array<FT_Fixed, 4> emoji_weights{
    400 * 65536,
    700 * 65536,
    500 * 65536,
    600 * 65536,
};

/// A glyph key's face index: bits 2..7, so a base face, a pack face and an
/// interface face at the same glyph index stay apart, and a removed pack
/// face's glyphs can be dropped by that index.
constexpr int glyph_key_face_shift = 2;
constexpr uint64_t glyph_key_face_mask = 0x3F;
/// A glyph key's rendering: bit 1.
constexpr int glyph_key_rendering_shift = 1;
/// A glyph key's pixel size: bits 8..16.
constexpr int glyph_key_size_shift = 8;
/// A glyph key's weight, Noto Emoji's alone, whose axis follows it: bits
/// 17..18.
constexpr int glyph_key_weight_shift = 17;
/// A glyph key's glyph index: from bit 24.
constexpr int glyph_key_index_shift = 24;

/// Tells whether a weight looks in DejaVu Sans Bold first: bold and
/// semibold do.
bool bold_first(Weight weight) noexcept {
    return weight == Weight::bold || weight == Weight::semibold;
}

/// Gives the interface face a family draws a weight in.
///
/// @param family the family
/// @param weight the weight
/// @return the face; none for the sans family
std::optional<Face> family_face(Family family, Weight weight) noexcept {
    switch (family) {
    case Family::sans:
        return std::nullopt;
    case Family::interface:
        switch (weight) {
        case Weight::regular:
            return barlow_regular;
        case Weight::medium:
            return barlow_medium;
        case Weight::semibold:
            return barlow_semibold;
        case Weight::bold:
            return barlow_bold;
        }
        return barlow_regular;
    case Family::interface_condensed:
        return weight == Weight::bold ? barlow_condensed_bold : barlow_condensed_semibold;
    }
    return std::nullopt;
}

/// Tells whether a character is a letter of an alphabet beyond ASCII: the
/// Latin-1 letters, and every character from Latin Extended up to the
/// general punctuation, among them the Greek and Cyrillic letters and the
/// combining marks. Symbols and punctuation (the multiplication and
/// division signs, and from U+2000 on, the CJK characters among them) are
/// not.
///
/// @param character the code point
/// @return true for such a letter
bool script_letter(char32_t character) noexcept {
    constexpr char32_t first_letter = 0xC0;
    constexpr char32_t general_punctuation = 0x2000;
    constexpr char32_t multiplication_sign = 0xD7;
    constexpr char32_t division_sign = 0xF7;
    if (character < first_letter || character >= general_punctuation)
        return false;
    return character != multiplication_sign && character != division_sign;
}

/// The fonts each weight looks a character up in, in order.
constexpr std::array<Face, 4> bold_chain{
    Face::dejavu_sans_bold,
    Face::dejavu_sans,
    Face::endonyms,
    Face::noto_emoji,
};
constexpr std::array<Face, 3> regular_chain{
    Face::dejavu_sans,
    Face::endonyms,
    Face::noto_emoji,
};

/// Rounds a 26.6 fixed-point length to whole pixels.
int32_t whole_pixels(FT_Pos length) noexcept {
    return static_cast<int32_t>((length + 32) >> 6);
}

/// Reads a font file for FreeType: seeks when count is 0, else reads up to
/// count bytes at offset.
unsigned long
read_file(FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count) {
    auto* file = static_cast<std::FILE*>(stream->descriptor.pointer);
    if (offset > static_cast<unsigned long>(stream->size) ||
        std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0)
        return count == 0 ? 1 : 0;
    if (count == 0)
        return 0;
    return static_cast<unsigned long>(std::fread(buffer, 1, count, file));
}

/// Closes a font file when FreeType is done with its face.
void close_file(FT_Stream stream) {
    if (stream->descriptor.pointer != nullptr)
        std::fclose(static_cast<std::FILE*>(stream->descriptor.pointer));
    stream->descriptor.pointer = nullptr;
}

/// Opens a file for reading, by its wide path on Windows, letting others read
/// and write it too; null when it cannot be opened.
std::FILE* open_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    return _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

#if defined(_WIN32)
/// The most bytes a pack's font file may hold where it is read whole.
constexpr long most_pack_font_bytes = 64L * 1024L * 1024L;

/// Reads a font file whole and closes it.
///
/// @param file the file, open for reading; closed on return
/// @return its bytes; empty when it cannot be read whole, is empty or holds
///         more than most_pack_font_bytes
std::vector<uint8_t> read_whole(std::FILE* file) {
    std::vector<uint8_t> bytes;
    if (std::fseek(file, 0, SEEK_END) == 0) {
        const long size = std::ftell(file);
        if (size > 0 && size <= most_pack_font_bytes && std::fseek(file, 0, SEEK_SET) == 0) {
            bytes.resize(static_cast<std::size_t>(size));
            if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
                bytes.clear();
        }
    }
    std::fclose(file);
    return bytes;
}
#endif

/// Opens a file already opened as a scalable face. FreeType closes the
/// stream when the face cannot be made; this closes it when the file is
/// empty. On failure face is null and stream is cleared.
bool open_scalable_face(FT_Library library, FT_StreamRec& stream, FT_Face& face, std::FILE* file) {
    face = nullptr;
    stream = FT_StreamRec{};
    stream.descriptor.pointer = file;
    stream.read = &read_file;
    stream.close = &close_file;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        close_file(&stream);
        stream = FT_StreamRec{};
        return false;
    }
    const long size = std::ftell(file);
    if (size <= 0) {
        close_file(&stream);
        stream = FT_StreamRec{};
        return false;
    }
    stream.size = static_cast<unsigned long>(size);
    FT_Open_Args arguments{};
    arguments.flags = FT_OPEN_STREAM;
    arguments.stream = &stream;
    // FreeType closes the stream itself when the face cannot be made.
    if (FT_Open_Face(library, &arguments, 0, &face) != 0) {
        face = nullptr;
        stream = FT_StreamRec{};
        return false;
    }
    if (FT_IS_SCALABLE(face) == 0) {
        FT_Done_Face(face);
        face = nullptr;
        stream = FT_StreamRec{};
        return false;
    }
    return true;
}

/// FreeType's memory, from the C library.
void* allocate(FT_Memory, long size) {
    return std::malloc(static_cast<std::size_t>(size));
}

void release(FT_Memory, void* block) {
    std::free(block);
}

void* reallocate(FT_Memory, long, long new_size, void* block) {
    return std::realloc(block, static_cast<std::size_t>(new_size));
}

/// One drawn glyph.
struct Glyph {
    int32_t left{};    ///< columns from the pen to the bitmap's left edge
    int32_t top{};     ///< rows from the baseline up to the bitmap's top row
    int32_t width{};   ///< bitmap columns
    int32_t rows{};    ///< bitmap rows
    int32_t advance{}; ///< pixels the pen moves, rounded
    std::vector<uint8_t> alpha{};
};

/// A glyph in the store, with its place in the order of use.
struct KeptGlyph {
    Glyph glyph{};
    std::list<uint64_t>::iterator place{};
};

/// One character laid out: its glyph, font and pen.
struct Laid {
    Placement placement{};
    const Glyph* glyph{};
};

} // namespace

/// The library, the open fonts and the glyphs drawn from them.
struct FontStack::Fonts {
    /// Base faces, then up to most_pack_faces pack faces, then the interface
    /// faces.
    static constexpr std::size_t slot_count = face_count + most_pack_faces + interface_face_count;

    FT_MemoryRec_ memory{};
    FT_Library library{};
    std::array<FT_StreamRec, slot_count> streams{};
    std::array<FT_Face, slot_count> faces{};
    /// the pixel size each face was last set to, 0 for none
    std::array<int32_t, slot_count> sizes{};
    /// how each pack face is drawn, in the order it was added
    std::array<FaceRole, most_pack_faces> pack_roles{};
    /// On Windows each pack face's font file, read whole, which its face
    /// reads from until it is done; empty elsewhere and for no face.
    std::array<std::vector<uint8_t>, most_pack_faces> pack_bytes{};
    /// pack faces open, at most most_pack_faces
    std::size_t pack_count{};
    /// the weight Noto Emoji's axis was last set to
    std::optional<Weight> emoji_weight{};
    std::unordered_map<uint64_t, KeptGlyph> glyphs{};
    /// the keys of the kept glyphs, the one used last first
    std::list<uint64_t> order{};
    /// glyphs drawn since the stack opened
    std::size_t drawn_count{};

    Fonts() = default;
    Fonts(const Fonts&) = delete;
    Fonts& operator=(const Fonts&) = delete;

    ~Fonts() {
        for (FT_Face face : faces)
            if (face != nullptr)
                FT_Done_Face(face);
        if (library != nullptr)
            FT_Done_Library(library);
    }

    /// Opens a file as a scalable face in a slot. The file is closed on failure.
    bool open_slot(std::size_t index, std::FILE* file) {
        return open_scalable_face(library, streams[index], faces[index], file);
    }

    /// Sets a face to a pixel size, unless it has it.
    bool set_size(Face which, int32_t pixel_size) {
        const auto index = static_cast<std::size_t>(which);
        if (index >= slot_count || faces[index] == nullptr)
            return false;
        if (sizes[index] == pixel_size)
            return true;
        if (FT_Set_Pixel_Sizes(faces[index], 0, static_cast<FT_UInt>(pixel_size)) != 0)
            return false;
        sizes[index] = pixel_size;
        return true;
    }

    /// Gives the pixel size a face is drawn at in a style.
    ///
    /// DejaVu, the interface faces and letters pack faces take the style's
    /// size. The endonym face and ideographs pack faces take the related
    /// size and no less than the style's least size. Noto Emoji takes the
    /// related size.
    int32_t face_pixel_size(Face which, const Style& style) const noexcept {
        const auto index = static_cast<std::size_t>(which);
        const bool letters = which == Face::dejavu_sans_bold || which == Face::dejavu_sans ||
                             is_interface_face(which) ||
                             (index >= face_count && index < face_count + pack_count &&
                              pack_roles[index - face_count] == FaceRole::letters);
        if (letters)
            return style.pixel_size;
        const int32_t related = related_pixel_size(style.pixel_size);
        if (which == Face::noto_emoji)
            return related;
        return std::max(related, style.least_cjk_pixel_size);
    }

    /// The faces a weight looks a character up in, the family's interface
    /// face first. Closed faces are left out.
    std::vector<Face> chain(Weight weight, Family family) const {
        std::vector<Face> found;
        found.reserve(face_count + pack_count + 1);
        const auto push_open = [&](Face which) {
            const auto index = static_cast<std::size_t>(which);
            if (index < slot_count && faces[index] != nullptr)
                found.push_back(which);
        };
        if (const std::optional<Face> first = family_face(family, weight))
            push_open(*first);
        if (bold_first(weight))
            push_open(Face::dejavu_sans_bold);
        push_open(Face::dejavu_sans);
        for (std::size_t index = 0; index < pack_count; ++index)
            if (pack_roles[index] == FaceRole::letters)
                push_open(pack_face(index));
        for (std::size_t index = 0; index < pack_count; ++index)
            if (pack_roles[index] == FaceRole::ideographs)
                push_open(pack_face(index));
        push_open(Face::endonyms);
        push_open(Face::noto_emoji);
        return found;
    }

    /// Gives the family a line is drawn in: the style's, or the sans family
    /// when the line holds a letter beyond ASCII that the family's face
    /// lacks or the face is not open, so that a word in another alphabet is
    /// drawn in one typeface, its marks and digits with it.
    Family line_family(const std::vector<char32_t>& characters, const Style& style) const {
        const std::optional<Face> first = family_face(style.family, style.weight);
        if (!first)
            return Family::sans;
        FT_Face face = faces[static_cast<std::size_t>(*first)];
        if (face == nullptr)
            return Family::sans;
        for (const char32_t character : characters)
            if (script_letter(character) && FT_Get_Char_Index(face, character) == 0)
                return Family::sans;
        return style.family;
    }

    /// Sets Noto Emoji's weight axis, unless it has the weight; a font
    /// without the axis keeps its one weight.
    void set_emoji_weight(Weight weight) {
        if (emoji_weight == weight)
            return;
        FT_Face face = faces[static_cast<std::size_t>(Face::noto_emoji)];
        FT_MM_Var* axes = nullptr;
        if (FT_HAS_MULTIPLE_MASTERS(face) && FT_Get_MM_Var(face, &axes) == 0) {
            std::vector<FT_Fixed> coordinates(axes->num_axis);
            for (FT_UInt axis = 0; axis < axes->num_axis; ++axis) {
                const FT_Var_Axis& found = axes->axis[axis];
                coordinates[axis] = found.tag == FT_MAKE_TAG('w', 'g', 'h', 't')
                                        ? std::clamp(
                                              emoji_weights[static_cast<std::size_t>(weight)],
                                              found.minimum,
                                              found.maximum
                                          )
                                        : found.def;
            }
            FT_Set_Var_Design_Coordinates(face, axes->num_axis, coordinates.data());
            FT_Done_MM_Var(library, axes);
            // The face's scaled metrics follow the instance: set the size again.
            sizes[static_cast<std::size_t>(Face::noto_emoji)] = 0;
        }
        emoji_weight = weight;
    }

    /// Returns a glyph drawn at a size, drawing it the first time.
    const Glyph* glyph(Face which, FT_UInt index, int32_t pixel_size, const Style& style) {
        const bool emoji = which == Face::noto_emoji;
        const uint64_t key =
            (uint64_t{index} << glyph_key_index_shift) |
            (static_cast<uint64_t>(pixel_size) << glyph_key_size_shift) |
            (static_cast<uint64_t>(which) << glyph_key_face_shift) |
            (static_cast<uint64_t>(style.rendering) << glyph_key_rendering_shift) |
            (emoji ? static_cast<uint64_t>(style.weight) << glyph_key_weight_shift : 0U);
        if (const auto found = glyphs.find(key); found != glyphs.end()) {
            order.splice(order.begin(), order, found->second.place);
            return &found->second.glyph;
        }
        if (emoji)
            set_emoji_weight(style.weight);
        if (!set_size(which, pixel_size))
            return nullptr;
        FT_Face face = faces[static_cast<std::size_t>(which)];
        const bool mono = style.rendering == Rendering::mono;
        const FT_Int32 load =
            FT_LOAD_NO_BITMAP | (mono ? FT_LOAD_TARGET_MONO : FT_LOAD_TARGET_NORMAL);
        if (FT_Load_Glyph(face, index, load) != 0 ||
            FT_Render_Glyph(face->glyph, mono ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL) != 0)
            return nullptr;
        const FT_GlyphSlot slot = face->glyph;
        const FT_Bitmap& bitmap = slot->bitmap;
        Glyph drawn;
        drawn.left = slot->bitmap_left;
        drawn.top = slot->bitmap_top;
        drawn.width = static_cast<int32_t>(bitmap.width);
        drawn.rows = static_cast<int32_t>(bitmap.rows);
        drawn.advance = whole_pixels(slot->advance.x);
        drawn.alpha.assign(
            static_cast<std::size_t>(drawn.width) * static_cast<std::size_t>(drawn.rows), 0
        );
        for (int32_t row = 0; row < drawn.rows; ++row) {
            const unsigned char* line =
                bitmap.buffer + static_cast<std::ptrdiff_t>(row) * bitmap.pitch;
            for (int32_t column = 0; column < drawn.width; ++column) {
                const auto covered = static_cast<uint8_t>(
                    bitmap.pixel_mode == FT_PIXEL_MODE_MONO
                        ? ((line[column >> 3] & (0x80 >> (column & 7))) != 0 ? 255 : 0)
                        : line[column]
                );
                drawn.alpha[static_cast<std::size_t>(row * drawn.width + column)] = covered;
            }
        }
        ++drawn_count;
        order.push_front(key);
        auto& kept = glyphs.emplace(key, KeptGlyph{std::move(drawn), order.begin()}).first->second;
        return &kept.glyph;
    }

    /// Makes room for a line of a number of characters: while the line could
    /// take the store past kept_glyphs, forgets the glyph used longest ago.
    /// Only this forgets glyphs, so the glyphs one line draws stay in the
    /// store while the line is drawn.
    void make_room(std::size_t characters) {
        while (!order.empty() && glyphs.size() + characters > kept_glyphs) {
            glyphs.erase(order.back());
            order.pop_back();
        }
    }

    /// Finds the face and glyph that draw a character, the chain's first
    /// open face's missing-glyph box when no face has it. A closed face is
    /// skipped.
    std::pair<Face, FT_UInt> lookup(char32_t character, Weight weight, Family family) const {
        const std::vector<Face> found = chain(weight, family);
        for (Face which : found) {
            FT_Face face = faces[static_cast<std::size_t>(which)];
            if (face == nullptr)
                continue;
            if (const FT_UInt index = FT_Get_Char_Index(face, character); index != 0)
                return {which, index};
        }
        for (Face which : found)
            if (faces[static_cast<std::size_t>(which)] != nullptr)
                return {which, FT_UInt{0}};
        return {Face::dejavu_sans_bold, FT_UInt{0}};
    }
};

std::string freetype_version() {
    return std::to_string(FREETYPE_MAJOR) + "." + std::to_string(FREETYPE_MINOR) + "." +
           std::to_string(FREETYPE_PATCH);
}

std::span<const Face> fallback_chain(Weight weight) noexcept {
    if (bold_first(weight))
        return bold_chain;
    return regular_chain;
}

int32_t related_pixel_size(int32_t pixel_size) noexcept {
    return std::max((pixel_size * 6 + 3) / 7, std::min(pixel_size, 12));
}

std::optional<std::vector<char32_t>> decode_utf8(std::string_view text) {
    std::vector<char32_t> characters;
    characters.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        const auto lead = static_cast<uint8_t>(text[at]);
        std::size_t length = 1;
        char32_t value = lead;
        char32_t least = 0;
        if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            value = lead & 0x07U;
            least = 0x10000;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            value = lead & 0x0FU;
            least = 0x800;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            value = lead & 0x1FU;
            least = 0x80;
        } else if (lead >= 0x80) {
            return std::nullopt;
        }
        if (text.size() - at < length)
            return std::nullopt;
        for (std::size_t next = 1; next < length; ++next) {
            const auto byte = static_cast<uint8_t>(text[at + next]);
            if ((byte & 0xC0U) != 0x80U)
                return std::nullopt;
            value = (value << 6) | (byte & 0x3FU);
        }
        if (value < least || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
            return std::nullopt;
        characters.push_back(value);
        at += length;
    }
    return characters;
}

bool is_invisible(char32_t character) noexcept {
    return character < 0x20 || (character >= 0x7F && character < 0xA0) ||
           (character >= 0x200B && character <= 0x200F) || character == 0x2060 ||
           character == 0xFEFF || (character >= 0xFE00 && character <= 0xFE0F) ||
           (character >= 0xE0100 && character <= 0xE01EF);
}

FontStack::FontStack(std::unique_ptr<Fonts> fonts) noexcept : fonts_(std::move(fonts)) {
}

FontStack::~FontStack() = default;

std::unique_ptr<FontStack> FontStack::open(const std::filesystem::path& directory) {
    auto fonts = std::make_unique<Fonts>();
    fonts->memory.alloc = &allocate;
    fonts->memory.free = &release;
    fonts->memory.realloc = &reallocate;
    // The library is made without FreeType's default properties, which an
    // environment variable could change: every machine draws alike.
    if (FT_New_Library(&fonts->memory, &fonts->library) != 0)
        return nullptr;
    FT_Add_Default_Modules(fonts->library);
    for (std::size_t index = 0; index < face_count; ++index) {
        std::FILE* file = open_file(directory / std::filesystem::path(face_files[index]));
        if (file == nullptr)
            return nullptr;
        if (!fonts->open_slot(index, file))
            return nullptr;
    }
    // An interface face that is missing or does not open leaves its family
    // to the sans faces.
    for (std::size_t index = 0; index < interface_face_count; ++index) {
        std::FILE* file = open_file(directory / std::filesystem::path(interface_face_files[index]));
        if (file != nullptr)
            std::ignore = fonts->open_slot(static_cast<std::size_t>(interface_face(index)), file);
    }
    return std::unique_ptr<FontStack>(new FontStack(std::move(fonts)));
}

bool FontStack::add_face(const std::filesystem::path& file, FaceRole role) {
    if (fonts_->pack_count >= most_pack_faces)
        return false;
    std::FILE* opened = open_file(file);
    if (opened == nullptr)
        return false;
    const std::size_t index = face_count + fonts_->pack_count;
#if defined(_WIN32)
    // Windows does not move a folder while a file in it is open, so a pack's
    // font is read whole and its file closed: the pack can then be updated,
    // rolled back or removed while its font is shown.
    std::vector<uint8_t> bytes = read_whole(opened);
    if (bytes.empty())
        return false;
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(
            fonts_->library, bytes.data(), static_cast<FT_Long>(bytes.size()), 0, &face
        ) != 0)
        return false;
    if (FT_IS_SCALABLE(face) == 0) {
        FT_Done_Face(face);
        return false;
    }
    fonts_->faces[index] = face;
    fonts_->streams[index] = FT_StreamRec{};
    fonts_->pack_bytes[fonts_->pack_count] = std::move(bytes);
#else
    if (!fonts_->open_slot(index, opened))
        return false;
#endif
    fonts_->pack_roles[fonts_->pack_count] = role;
    ++fonts_->pack_count;
    return true;
}

void FontStack::remove_pack_faces() noexcept {
    for (auto kept = fonts_->glyphs.begin(); kept != fonts_->glyphs.end();) {
        const uint64_t face_bits = (kept->first >> glyph_key_face_shift) & glyph_key_face_mask;
        if (face_bits >= face_count && face_bits < face_count + most_pack_faces) {
            fonts_->order.erase(kept->second.place);
            kept = fonts_->glyphs.erase(kept);
        } else {
            ++kept;
        }
    }
    for (std::size_t index = 0; index < fonts_->pack_count; ++index) {
        const std::size_t slot = face_count + index;
        if (fonts_->faces[slot] != nullptr) {
            FT_Done_Face(fonts_->faces[slot]);
            fonts_->faces[slot] = nullptr;
        }
        // The face is done with the bytes it read from.
        fonts_->pack_bytes[index] = {};
        fonts_->streams[slot] = FT_StreamRec{};
        fonts_->sizes[slot] = 0;
    }
    fonts_->pack_count = 0;
}

std::size_t FontStack::pack_face_count() const noexcept {
    return fonts_->pack_count;
}

bool FontStack::has_face(Face face) const noexcept {
    const auto index = static_cast<std::size_t>(face);
    if (index >= face_count + fonts_->pack_count && !is_interface_face(face))
        return false;
    return fonts_->faces[index] != nullptr;
}

std::vector<Face> FontStack::chain(Weight weight, Family family) const {
    return fonts_->chain(weight, family);
}

bool FontStack::face_file_opens(const std::filesystem::path& file) {
    std::FILE* opened = open_file(file);
    if (opened == nullptr)
        return false;
    FT_MemoryRec_ memory{};
    memory.alloc = &allocate;
    memory.free = &release;
    memory.realloc = &reallocate;
    FT_Library library = nullptr;
    if (FT_New_Library(&memory, &library) != 0) {
        std::fclose(opened);
        return false;
    }
    FT_Add_Default_Modules(library);
    FT_StreamRec stream{};
    FT_Face face = nullptr;
    const bool scalable = open_scalable_face(library, stream, face, opened);
    if (face != nullptr)
        FT_Done_Face(face);
    FT_Done_Library(library);
    return scalable;
}

std::optional<Face> FontStack::face_for(char32_t character, Weight weight, Family family) const {
    if (is_invisible(character))
        return std::nullopt;
    return fonts_->lookup(character, weight, family).first;
}

bool FontStack::draws(std::string_view text, Weight weight, Family family) const {
    const auto characters = decode_utf8(text);
    if (!characters)
        return false;
    for (char32_t character : *characters) {
        if (is_invisible(character))
            continue;
        // Glyph index 0 is the face's missing-glyph box: the chain has no
        // face that holds the character.
        if (fonts_->lookup(character, weight, family).second == 0)
            return false;
    }
    return true;
}

namespace {

/// Tells whether a style is one the stack draws.
bool in_range(const Style& style) noexcept {
    const auto weight = static_cast<std::size_t>(style.weight);
    const auto family = static_cast<std::size_t>(style.family);
    constexpr auto last_weight = static_cast<std::size_t>(Weight::semibold);
    constexpr auto last_family = static_cast<std::size_t>(Family::interface_condensed);
    return weight <= last_weight && family <= last_family && style.pixel_size >= 1 &&
           style.pixel_size <= max_pixel_size && style.letter_spacing >= 0 &&
           style.letter_spacing <= max_letter_spacing && style.least_cjk_pixel_size >= 0 &&
           style.least_cjk_pixel_size <= max_pixel_size;
}

} // namespace

Family FontStack::line_family(std::string_view text, const Style& style) const {
    const auto characters = decode_utf8(text);
    if (!characters)
        return Family::sans;
    return fonts_->line_family(*characters, style);
}

std::optional<LineMetrics> FontStack::metrics(const Style& style) {
    if (!in_range(style))
        return std::nullopt;
    LineMetrics line;
    // Pack faces never change the line's rows. A closed base face is left out.
    for (std::size_t index = 0; index < face_count; ++index) {
        const auto which = static_cast<Face>(index);
        if (fonts_->faces[index] == nullptr)
            continue;
        if (which == Face::dejavu_sans_bold && !bold_first(style.weight))
            continue;
        if (which == Face::noto_emoji)
            fonts_->set_emoji_weight(style.weight);
        if (!fonts_->set_size(which, fonts_->face_pixel_size(which, style)))
            return std::nullopt;
        const FT_Size_Metrics& size = fonts_->faces[index]->size->metrics;
        line.ascent = std::max(line.ascent, static_cast<int32_t>((size.ascender + 63) >> 6));
        line.descent = std::max(line.descent, static_cast<int32_t>((-size.descender + 63) >> 6));
    }
    // An interface family's face, when it is open, joins them.
    if (const std::optional<Face> first = family_face(style.family, style.weight);
        first && has_face(*first)) {
        if (!fonts_->set_size(*first, fonts_->face_pixel_size(*first, style)))
            return std::nullopt;
        const FT_Size_Metrics& size =
            fonts_->faces[static_cast<std::size_t>(*first)]->size->metrics;
        line.ascent = std::max(line.ascent, static_cast<int32_t>((size.ascender + 63) >> 6));
        line.descent = std::max(line.descent, static_cast<int32_t>((-size.descender + 63) >> 6));
    }
    return line;
}

std::optional<FaceMetrics> FontStack::face_metrics(Face face, const Style& style) {
    if (!in_range(style) || !has_face(face))
        return std::nullopt;
    // The emoji face's scaled rows follow its weight, so the weight is set
    // before the size, as metrics does.
    if (face == Face::noto_emoji)
        fonts_->set_emoji_weight(style.weight);
    const int32_t pixels = fonts_->face_pixel_size(face, style);
    if (!fonts_->set_size(face, pixels))
        return std::nullopt;
    const FT_Size_Metrics& size = fonts_->faces[static_cast<std::size_t>(face)]->size->metrics;
    return FaceMetrics{
        pixels,
        static_cast<int32_t>((size.ascender + 63) >> 6),
        static_cast<int32_t>((-size.descender + 63) >> 6),
    };
}

std::optional<std::vector<Placement>> FontStack::layout(std::string_view text, const Style& style) {
    if (!in_range(style) || text.size() > max_text_bytes)
        return std::nullopt;
    const auto characters = decode_utf8(text);
    if (!characters)
        return std::nullopt;
    fonts_->make_room(characters->size());
    const Family family = fonts_->line_family(*characters, style);
    std::vector<Placement> placements;
    int32_t pen = 0;
    for (char32_t character : *characters) {
        if (is_invisible(character))
            continue;
        const auto [which, index] = fonts_->lookup(character, style.weight, family);
        const Glyph* drawn =
            fonts_->glyph(which, index, fonts_->face_pixel_size(which, style), style);
        if (drawn == nullptr)
            return std::nullopt;
        // A mark that does not move the pen gets no spacing either.
        const int32_t advance = drawn->advance + (drawn->advance > 0 ? style.letter_spacing : 0);
        placements.push_back({character, which, pen, advance});
        pen += advance;
    }
    return placements;
}

std::optional<Coverage> FontStack::draw(std::string_view text, const Style& style) {
    const auto placements = layout(text, style);
    const auto line = metrics(style);
    if (!placements || !line)
        return std::nullopt;
    // The glyphs come again from the store, where layout has just put every
    // one of them; nothing empties it before the next line.
    std::vector<Laid> laid;
    laid.reserve(placements->size());
    int32_t left = 0;
    int32_t right = 0;
    int32_t above = line->ascent;
    int32_t below = line->descent;
    // The family layout drew the line in: decode_utf8 accepted the text there.
    const Family family =
        fonts_->line_family(decode_utf8(text).value_or(std::vector<char32_t>{}), style);
    for (const Placement& placement : *placements) {
        const auto [which, index] = fonts_->lookup(placement.character, style.weight, family);
        const Glyph* drawn =
            fonts_->glyph(which, index, fonts_->face_pixel_size(which, style), style);
        if (drawn == nullptr)
            return std::nullopt;
        laid.push_back({placement, drawn});
        right = std::max(right, placement.pen + placement.advance);
        if (drawn->width > 0 && drawn->rows > 0) {
            left = std::min(left, placement.pen + drawn->left);
            right = std::max(right, placement.pen + drawn->left + drawn->width);
            above = std::max(above, drawn->top);
            below = std::max(below, drawn->rows - drawn->top);
        }
    }
    Coverage coverage;
    coverage.origin = -left;
    coverage.width = std::min(right - left, max_line_width);
    coverage.baseline = above;
    coverage.height = above + below;
    coverage.advance = laid.empty() ? 0 : laid.back().placement.pen + laid.back().placement.advance;
    coverage.alpha.assign(
        static_cast<std::size_t>(coverage.width) * static_cast<std::size_t>(coverage.height), 0
    );
    for (const Laid& each : laid) {
        const Glyph& drawn = *each.glyph;
        const int32_t x = coverage.origin + each.placement.pen + drawn.left;
        const int32_t y = coverage.baseline - drawn.top;
        for (int32_t row = 0; row < drawn.rows; ++row)
            for (int32_t column = 0; column < drawn.width; ++column) {
                const int32_t px = x + column;
                if (px < 0 || px >= coverage.width)
                    continue;
                auto& pixel =
                    coverage.alpha[static_cast<std::size_t>((y + row) * coverage.width + px)];
                pixel = std::max(
                    pixel, drawn.alpha[static_cast<std::size_t>(row * drawn.width + column)]
                );
            }
    }
    return coverage;
}

std::size_t FontStack::cached_glyphs() const noexcept {
    return fonts_->glyphs.size();
}

std::size_t FontStack::drawn_glyphs() const noexcept {
    return fonts_->drawn_count;
}

} // namespace oa::platform::text_font
