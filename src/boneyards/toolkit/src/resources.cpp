// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Default Boneyards services: GAF frame cache, image table, glyph fonts, text
// store, colours, sounds, cursor and key state, drawing into the overlay.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "list.hpp"
#include "oa/boneyards/font_atlases.hpp"
#include "oa/boneyards/toolkit.hpp"
#include "oa/boneyards/utf8.hpp"
#include "oa/formats/gaf.hpp"

namespace oa::boneyards {
namespace {

constexpr int32_t k_gaf_slots = 16;
constexpr int32_t k_frame_slots = 0x1000;
constexpr int32_t k_image_entries = 0x400;
constexpr int32_t k_font_slots = 15;
constexpr int32_t k_glyphs = 256;
constexpr int32_t k_line_height_glyph = 'I';
constexpr int32_t k_text_entries = 0x400;
constexpr int32_t k_sound_entries = 0x80;
constexpr int32_t k_key_states = 0x200;
constexpr int32_t k_image_file_bytes = 256;
constexpr int32_t k_text_draw_bytes = 0x200;
constexpr int32_t k_ui_sound_volume = -585; // hundredths of a dB
constexpr int32_t k_caret_width = 2;        // matches the single-line edit's caret bar
constexpr int32_t k_new_image_refs = 2;     // new image entries start with two references
constexpr int32_t k_cursor_slots = 32;
constexpr int32_t k_cursor_image_key = 9;
constexpr int32_t k_extension_chars = 3; // compared after a name's first '.'
// Animation ticks per cursor frame while no image manager is linked.
constexpr int32_t k_cursor_unlinked_delay = 2;

const char* const k_image_dir = "anims\\";
const char* const k_font_dir = "Fonts\\";
const char* const k_sound_dir = "sounds\\";
const char* const k_beep_file = "beep2.wav";
const char* const k_default_font = "BYHatFontSmall87.GAF";

// A GAF reader and the cache slot holding it: the file and the sequence
// and frame last selected in it.
struct GafFile {
    char* path; // the file the reader loaded, also the cache key
    formats::gaf::Archive* archive;
    // The file's own bytes. A frame is drawn from them, so the archive does
    // not also keep every frame's pixels.
    uint8_t* bytes{};
    std::size_t byte_count{};
    const formats::gaf::Sequence* sequence;
    const formats::gaf::Frame* frame;
    int32_t refs; // cache slot references
};

struct FrameSlot {
    int32_t refs;
    Sprite* sprite;
    int32_t gaf; // GafFile the frame came from, or -1
};

struct ImageEntry {
    char file[k_image_file_bytes];
    char* sequence;
    int32_t frame;
    int32_t refs;
    int32_t key;
    int32_t slot;
};

struct Glyph {
    int32_t advance;
    int32_t height;
    int32_t frame;
};

struct Font {
    int32_t line_height;
    int32_t first_char;
    char* name;
    int32_t refs;
    uint8_t kerning;
    Glyph glyphs[k_glyphs];
    // Face the analogue atlas knows. Empty uses `name`. A recolour keeps the
    // face it was copied from so characters past that face still measure.
    char face_key[64];
    uint8_t body;
    uint8_t edge;
    uint8_t shaded;
    uint8_t ink_known;
};

struct TextEntry {
    int32_t refs;
    char* text;
    int32_t style;
};

struct SoundEntry {
    char* name;
    int32_t refs;
};

// One cursor slot: a GAF sequence and the image of each frame.
struct CursorEntry {
    char* file;
    char* sequence;
    int32_t refs;
    int32_t frame;  // index into images
    Handle* images; // vector, one image per frame
    int32_t image_count;
};

struct CursorTable {
    CursorEntry entries[k_cursor_slots];
    const ImageService* images;
    uint8_t visible;
    int32_t default_cursor;
    int32_t current;   // -1 shows the default
    int32_t countdown; // ticks left on the shown frame
};

} // namespace

bool name_has_extension(const char* name, const char* extension) {
    if (name == nullptr || extension == nullptr) {
        return false;
    }
    while (*name != '.') {
        if (*name == '\0' || *name == '\n') {
            return false;
        }
        ++name;
    }
    const char* text = name + 1;
    for (int32_t i = 0; i < k_extension_chars; ++i) {
        const int a = std::tolower(static_cast<unsigned char>(text[i]));
        const int b = std::tolower(static_cast<unsigned char>(extension[i]));
        if (a != b) {
            return false;
        }
        if (a == 0) {
            break;
        }
    }
    return true;
}

struct Toolkit {
    ToolkitConfig config;
    Surface overlay;
    Surface* target; // replaces the overlay as the drawing surface when set
    Desktop desktop;
    GafFile gafs[k_gaf_slots];
    // Null until a frame, image, font or text is stored. The main menu does
    // not ask, so these tables are not allocated for it.
    FrameSlot* frames{};
    ImageEntry* images{};
    Font* fonts{};
    int32_t current_font;
    TextEntry* texts{};
    SoundEntry sounds[k_sound_entries];
    uint8_t keys[k_key_states];
    CursorTable cursors;
    Handle default_font;
    ImageService image_service;
    TextService text_service;
    FontService font_service;
    InputService input_service;
    ColorService color_service;
    CursorService cursor_service;
    SoundService sound_service;
    DrawService draw_service;
    const FontAtlas* atlas; // not owned; characters past a face's own pictures
    // Set once a table is walked. Destruction then frees what that walk
    // stored. Until then the table's pages stay untouched.
    bool used_frames{};
    bool used_images{};
    bool used_fonts{};
    bool used_texts{};
};

namespace {

char* duplicate(const char* text) {
    const size_t length = std::strlen(text) + 1;
    auto* copy = static_cast<char*>(std::malloc(length));
    if (copy == nullptr) {
        std::abort();
    }
    std::memcpy(copy, text, length);
    return copy;
}

bool equal_nocase(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

// "dir\" + name, unless the name already carries a directory.
char* join_path(const char* dir, const char* name) {
    const bool has_dir = std::strchr(name, '\\') != nullptr || std::strchr(name, '/') != nullptr;
    const char* prefix = has_dir ? "" : dir;
    const size_t length = std::strlen(prefix) + std::strlen(name) + 1;
    auto* path = static_cast<char*>(std::malloc(length));
    if (path == nullptr) {
        std::abort();
    }
    std::snprintf(path, length, "%s%s", prefix, name);
    return path;
}

void font_clear_glyphs(Font* font);
void prepare_frames(Toolkit* tk);
void prepare_images(Toolkit* tk);
void prepare_fonts(Toolkit* tk);
void prepare_texts(Toolkit* tk);

// ---------------------------------------------------------------------------
// Overlay drawing

void toolkit_create_overlay(Toolkit* tk, int32_t width, int32_t height);

void ensure_overlay_storage(Toolkit* tk) {
    if (tk->overlay.pixels == nullptr)
        toolkit_create_overlay(tk, tk->overlay.width, tk->overlay.height);
}

Surface* draw_surface(Toolkit* tk) {
    if (tk->target != nullptr)
        return tk->target;
    ensure_overlay_storage(tk);
    return &tk->overlay;
}

void blit_sprite(Surface* target, const Sprite* sprite, int32_t x, int32_t y) {
    if (target == nullptr || sprite == nullptr || target->pixels == nullptr) {
        return;
    }
    const int32_t left = x - sprite->origin_x;
    const int32_t top = y - sprite->origin_y;
    const auto* pixels = static_cast<const uint8_t*>(sprite->data);
    for (int32_t row = 0; row < sprite->height; ++row) {
        const int32_t sy = top + row;
        if (sy < target->clip.y1 || sy > target->clip.y2) {
            continue;
        }
        uint8_t* out = target->pixels + static_cast<ptrdiff_t>(sy) * target->pitch;
        const uint8_t* in = pixels + static_cast<ptrdiff_t>(row) * sprite->width;
        for (int32_t col = 0; col < sprite->width; ++col) {
            const int32_t sx = left + col;
            if (sx < target->clip.x1 || sx > target->clip.x2 || in[col] == sprite->key) {
                continue;
            }
            out[sx] = in[col];
        }
    }
}

void fill_rect(
    Surface* target, int32_t x, int32_t y, int32_t width, int32_t height, uint8_t color
) {
    if (target == nullptr || target->pixels == nullptr) {
        return;
    }
    const int32_t x1 = x < target->clip.x1 ? target->clip.x1 : x;
    const int32_t y1 = y < target->clip.y1 ? target->clip.y1 : y;
    const int32_t x2 = x + width - 1 > target->clip.x2 ? target->clip.x2 : x + width - 1;
    const int32_t y2 = y + height - 1 > target->clip.y2 ? target->clip.y2 : y + height - 1;
    for (int32_t row = y1; row <= y2; ++row) {
        for (int32_t col = x1; col <= x2; ++col) {
            target->pixels[static_cast<ptrdiff_t>(row) * target->pitch + col] = color;
        }
    }
}

Sprite*
make_sprite(int32_t width, int32_t height, int16_t origin_x, int16_t origin_y, uint8_t key) {
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    auto* sprite = static_cast<Sprite*>(std::calloc(1, sizeof(Sprite) + pixels));
    if (sprite == nullptr) {
        std::abort();
    }
    sprite->width = static_cast<uint16_t>(width);
    sprite->height = static_cast<uint16_t>(height);
    sprite->origin_x = origin_x;
    sprite->origin_y = origin_y;
    sprite->key = key;
    sprite->encoding = OA_SPRITE_RAW;
    sprite->data = reinterpret_cast<uint8_t*>(sprite + 1);
    return sprite;
}

// ---------------------------------------------------------------------------
// GAF frame cache

/// Frees a reader's archive and file name and clears its selection; its
/// reference count is left alone.
///
/// @param[in,out] reader reader to empty
void gaf_reader_free(GafFile& reader) {
    std::free(reader.path);
    reader.path = nullptr;
    delete reader.archive;
    reader.archive = nullptr;
    std::free(reader.bytes);
    reader.bytes = nullptr;
    reader.byte_count = 0;
    reader.sequence = nullptr;
    reader.frame = nullptr;
}

/// Selects a frame of the reader's selected sequence; a negative index
/// selects nothing.
///
/// @param[in,out] reader reader whose frame selection changes
/// @param index frame index; outside the sequence (or with no sequence
///     selected) no frame is selected
void gaf_select_frame(GafFile& reader, int32_t index) {
    reader.frame =
        reader.sequence != nullptr ? formats::gaf::frame_at(reader.sequence, index) : nullptr;
}

/// Selects the sequence at an index and its first frame; a negative index
/// selects nothing.
///
/// @param[in,out] reader reader whose selection changes
/// @param index sequence index; outside the file's sequences (or with no file
///     loaded) nothing is selected
void gaf_select_index(GafFile& reader, int32_t index) {
    reader.sequence = nullptr;
    reader.frame = nullptr;
    if (reader.archive == nullptr) {
        return;
    }
    if (index >= 0 && static_cast<size_t>(index) < reader.archive->sequences.size()) {
        reader.sequence = &reader.archive->sequences[static_cast<size_t>(index)];
        gaf_select_frame(reader, 0);
    }
}

/// Selects the first sequence with a name, ignoring case, and its first
/// frame.
///
/// @param[in,out] reader reader whose selection changes
/// @param name sequence name; when no sequence has it nothing is selected
void gaf_select_sequence(GafFile& reader, const char* name) {
    reader.sequence = nullptr;
    reader.frame = nullptr;
    if (reader.archive == nullptr) {
        return;
    }
    for (const formats::gaf::Sequence& s : reader.archive->sequences) {
        if (equal_nocase(s.name.c_str(), name)) {
            reader.sequence = &s;
            break;
        }
    }
    gaf_select_frame(reader, 0);
}

/// Returns the frame last selected in a reader.
///
/// @param reader reader to read
/// @return the selected frame, or null
const formats::gaf::Frame* gaf_selected_frame(const GafFile& reader) {
    return reader.frame;
}

/// Loads a GAF file into a reader and selects its first sequence, unless the
/// reader already holds that file (compared without case).
///
/// The file is read through the platform file hook with '\' turned into '/'.
/// The palette of a .pcx or 0x400-byte .pal file of the same name is not
/// loaded: no frame is drawn with it.
///
/// @param tk toolkit providing the file hook
/// @param[in,out] reader reader to load; emptied first when it held another
///     file
/// @param path file path as the toolkit names it
/// @return true when the reader holds the file
bool gaf_reader_load(Toolkit* tk, GafFile& reader, const char* path) {
    if (reader.path != nullptr && equal_nocase(path, reader.path)) {
        return true;
    }
    gaf_reader_free(reader);
    if (tk->config.files.read == nullptr) {
        return false;
    }
    char* file = duplicate(path);
    for (char* c = file; *c != '\0'; ++c) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    size_t size = 0;
    uint8_t* bytes = tk->config.files.read(tk->config.files.user, file, &size);
    std::free(file);
    if (bytes == nullptr) {
        return false;
    }
    formats::gaf::ParseResult parsed = formats::gaf::parse(
        std::span<const uint8_t>(bytes, size), formats::gaf::PixelData::checked
    );
    if (!parsed.ok()) {
        std::free(bytes);
        return false;
    }
    reader.archive = new formats::gaf::Archive(std::move(*parsed.archive));
    reader.bytes = bytes;
    reader.byte_count = size;
    reader.path = duplicate(path);
    gaf_select_index(reader, 0);
    return true;
}

/// Returns the reader in a cache slot.
///
/// @param tk toolkit holding the cache
/// @param slot cache slot, 0-15
/// @return the reader, or null for a slot outside the cache or unused
GafFile* gaf_reader(Toolkit* tk, int32_t slot) {
    if (slot < 0 || slot >= k_gaf_slots || tk->gafs[slot].refs == 0) {
        return nullptr;
    }
    return &tk->gafs[slot];
}

/// Opens a GAF file through the cache.
///
/// A slot already holding the path (compared case-sensitively) gains a
/// reference; otherwise the first free slot loads the file with one
/// reference.
///
/// @param[in,out] tk toolkit holding the cache
/// @param path file path; null or "" opens nothing
/// @return the cache slot, or -1 when the file cannot be loaded or no slot is
///     free
/// @quirk A file that fails to load is tried again in each later free slot.
int32_t gaf_open(Toolkit* tk, const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return -1;
    }
    for (int32_t i = 0; i < k_gaf_slots; ++i) {
        if (tk->gafs[i].path != nullptr && std::strcmp(tk->gafs[i].path, path) == 0) {
            ++tk->gafs[i].refs;
            return i;
        }
    }
    for (int32_t i = 0; i < k_gaf_slots; ++i) {
        GafFile& slot = tk->gafs[i];
        if (slot.refs != 0) {
            continue;
        }
        gaf_reader_free(slot);
        if (gaf_reader_load(tk, slot, path)) {
            ++slot.refs;
            return i;
        }
        gaf_reader_free(slot);
    }
    return -1;
}

/// Drops one reference to a cache slot; the last one frees its reader.
///
/// @param[in,out] tk toolkit holding the cache
/// @param slot cache slot; a slot outside the cache or unused is ignored
void gaf_release(Toolkit* tk, int32_t slot) {
    if (slot < 0 || slot >= k_gaf_slots || tk->gafs[slot].refs == 0) {
        return;
    }
    GafFile& gaf = tk->gafs[slot];
    if (--gaf.refs != 0) {
        return;
    }
    gaf_reader_free(gaf);
}

/// Counts the frames of a sequence in a .gaf or .fnt file; the file is read
/// afresh, not through the cache.
///
/// @param tk toolkit providing the file hook
/// @param path file path
/// @param sequence sequence name; "" selects the sequence at `index`
/// @param index sequence index used when `sequence` is ""
/// @return the frame count; 0 for a null name, another extension, a file
///     that cannot be read, or a missing sequence
int32_t gaf_frame_count(Toolkit* tk, const char* path, const char* sequence, int32_t index) {
    if (path == nullptr || sequence == nullptr ||
        (!name_has_extension(path, "gaf") && !name_has_extension(path, "fnt"))) {
        return 0;
    }
    GafFile reader{};
    gaf_reader_load(tk, reader, path);
    if (sequence[0] == '\0') {
        gaf_select_index(reader, index);
    } else {
        gaf_select_sequence(reader, sequence);
    }
    const int32_t count =
        reader.sequence != nullptr ? static_cast<int32_t>(reader.sequence->frames.size()) : 0;
    gaf_reader_free(reader);
    return count;
}

/// Stores a sprite the caller made in the first free frame slot, with one
/// reference and no GAF file behind it.
///
/// @param[in,out] tk toolkit holding the frame slots
/// @param sprite sprite to store; the toolkit takes ownership
/// @return the frame slot, or -1 when all 4096 slots are used (the sprite is
///     then freed)
int32_t frame_insert_sprite(Toolkit* tk, Sprite* sprite) {
    prepare_frames(tk);
    for (int32_t i = 0; i < k_frame_slots; ++i) {
        FrameSlot& slot = tk->frames[i];
        if (slot.refs == 0) {
            ++slot.refs;
            slot.sprite = sprite;
            slot.gaf = -1;
            return i;
        }
    }
    std::free(sprite);
    return -1;
}

bool read_gaf_file(void* context, uint32_t offset, std::span<uint8_t> output) {
    const auto* reader = static_cast<const GafFile*>(context);
    if (reader == nullptr || reader->bytes == nullptr)
        return false;
    const uint64_t end = static_cast<uint64_t>(offset) + static_cast<uint64_t>(output.size());
    if (end > reader->byte_count)
        return false;
    if (!output.empty())
        std::memcpy(output.data(), reader->bytes + offset, output.size());
    return true;
}

/// Loads one frame of a GAF sequence into the first free frame slot, with one
/// reference; the slot also holds a reference on the cached file.
///
/// Only GAF frames load; 3.1c also loads .pcx pictures.
///
/// @param[in,out] tk toolkit holding the cache and frame slots
/// @param path GAF file path
/// @param sequence sequence name; "" selects the first sequence
/// @param frame frame index within the sequence
/// @return the frame slot, or -1 for a null name, a path that is not a .gaf,
///     no free slot, or a file, sequence or frame that cannot be loaded
int32_t frame_load(Toolkit* tk, const char* path, const char* sequence, int32_t frame) {
    if (path == nullptr || sequence == nullptr) {
        return -1;
    }
    prepare_frames(tk);
    int32_t free_slot = -1;
    for (int32_t i = 0; i < k_frame_slots; ++i) {
        if (tk->frames[i].refs == 0) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0 || !name_has_extension(path, "gaf")) {
        return -1;
    }
    const int32_t gaf_slot = gaf_open(tk, path);
    if (gaf_slot < 0) {
        return -1;
    }
    GafFile* reader = gaf_reader(tk, gaf_slot);
    const formats::gaf::Frame* source = nullptr;
    if (reader != nullptr) {
        if (sequence[0] == '\0') {
            gaf_select_index(*reader, 0);
        } else {
            gaf_select_sequence(*reader, sequence);
        }
        gaf_select_frame(*reader, frame);
        source = gaf_selected_frame(*reader);
    }
    formats::gaf::RenderResult rendered;
    if (source != nullptr) {
        formats::gaf::ReadHooks hooks;
        hooks.context = reader;
        hooks.read = read_gaf_file;
        rendered = formats::gaf::render_ranged(*source, hooks);
    }
    if (!rendered.ok()) {
        gaf_release(tk, gaf_slot);
        return -1;
    }
    const formats::gaf::RenderedFrame& image = *rendered.frame;
    Sprite* sprite = make_sprite(
        image.width, image.height, image.origin_x, image.origin_y, image.transparency_index
    );
    // A frame with no pixels has no data pointer. The sprite is already clear.
    if (!image.pixels.empty())
        std::memcpy(sprite->data, image.pixels.data(), image.pixels.size());
    FrameSlot& slot = tk->frames[free_slot];
    ++slot.refs;
    slot.sprite = sprite;
    slot.gaf = gaf_slot;
    return free_slot;
}

/// Drops one reference to a frame slot; the last one frees the sprite and
/// releases the cached file.
///
/// @param[in,out] tk toolkit holding the frame slots
/// @param slot frame slot; a slot outside the table or unused is ignored
void frame_release(Toolkit* tk, int32_t slot) {
    if (!tk->used_frames || slot < 0 || slot >= k_frame_slots || tk->frames[slot].refs == 0) {
        return;
    }
    FrameSlot& frame = tk->frames[slot];
    if (--frame.refs != 0) {
        return;
    }
    std::free(frame.sprite);
    frame.sprite = nullptr;
    gaf_release(tk, frame.gaf);
    frame.gaf = -1;
}

const Sprite* frame_sprite(const Toolkit* tk, int32_t slot) {
    if (!tk->used_frames || slot < 0 || slot >= k_frame_slots || tk->frames[slot].refs == 0) {
        return nullptr;
    }
    return tk->frames[slot].sprite;
}

/// Reads the size of a loaded frame.
///
/// @param tk toolkit holding the frame slots
/// @param slot frame slot
/// @param[out] width frame width in pixels; left alone for an empty or
///     out-of-range slot
/// @param[out] height frame height in pixels; left alone likewise
void frame_size(const Toolkit* tk, int32_t slot, int32_t* width, int32_t* height) {
    const Sprite* sprite = frame_sprite(tk, slot);
    if (sprite != nullptr) {
        *width = sprite->width;
        *height = sprite->height;
    }
}

/// Draws a loaded frame onto the drawing surface (the target set by
/// toolkit_set_target, else the overlay).
///
/// The frame's origin lands on (x, y); pixels of the frame's key colour are
/// skipped and the surface's clip rectangle applies.
///
/// @param tk toolkit holding the frame slots
/// @param slot frame slot; an empty or out-of-range slot draws nothing
/// @param x x of the frame's origin in surface pixels
/// @param y y of the frame's origin in surface pixels
void frame_draw(Toolkit* tk, int32_t slot, int32_t x, int32_t y) {
    blit_sprite(draw_surface(tk), frame_sprite(tk, slot), x, y);
}

/// Draws a loaded frame onto a caller's surface, as frame_draw does onto the
/// drawing surface.
///
/// @param tk toolkit holding the frame slots
/// @param[in,out] target surface to draw onto
/// @param slot frame slot; an empty or out-of-range slot draws nothing
/// @param x x of the frame's origin in surface pixels
/// @param y y of the frame's origin in surface pixels
void frame_draw_into(Toolkit* tk, Surface* target, int32_t slot, int32_t x, int32_t y) {
    blit_sprite(target, frame_sprite(tk, slot), x, y);
}

// ---------------------------------------------------------------------------
// Image table (ImageService)

void image_clear_entry(Toolkit* tk, int32_t index) {
    ImageEntry& e = tk->images[index];
    e.file[0] = '\0';
    std::free(e.sequence);
    e.sequence = nullptr;
    e.frame = -1;
    e.refs = 0;
    e.key = k_overlay_transparent;
    frame_release(tk, e.slot);
    e.slot = -1;
}

bool image_valid(const Toolkit* tk, Handle image) {
    return tk->used_images && image >= 0 && image < k_image_entries && tk->images[image].refs != 0;
}

/// Drops one reference to an image entry; the last one clears the entry and
/// releases its frame (ImageService::release).
///
/// @param self the Toolkit
/// @param image image handle; an invalid or free handle is ignored
void image_table_release(void* self, Handle image) {
    auto* tk = static_cast<Toolkit*>(self);
    if (!tk->used_images || image < 0 || image >= k_image_entries || tk->images[image].refs == 0)
        return;
    if (--tk->images[image].refs == 0)
        image_clear_entry(tk, image);
}

/// Adds one reference to a live image entry (ImageService::add_ref).
///
/// @param self the Toolkit
/// @param image image handle; an invalid or free handle is ignored
void image_table_add_ref(void* self, Handle image) {
    auto* tk = static_cast<Toolkit*>(self);
    if (image_valid(tk, image)) {
        ++tk->images[image].refs;
    }
}

/// Counts the frames of a sequence in a file in the anims directory
/// (ImageService::frame_count).
///
/// @param self the Toolkit
/// @param file file name, looked up in the anims directory unless it names a
///     directory
/// @param sequence sequence name; "" selects the sequence at `index`
/// @param index sequence index used when `sequence` is ""
/// @return the frame count, or 0 (also for a null file)
int32_t image_table_frame_count(void* self, const char* file, const char* sequence, int32_t index) {
    auto* tk = static_cast<Toolkit*>(self);
    if (file == nullptr) {
        return 0;
    }
    char* path = join_path(k_image_dir, file);
    const int32_t count = gaf_frame_count(tk, path, sequence, index);
    std::free(path);
    return count;
}

/// Returns an image entry for one frame of a sequence in a file in the anims
/// directory (ImageService::load).
///
/// An entry with the same file and sequence (compared without case), frame
/// and key gains a reference. Otherwise the first free entry records the
/// request and loads the frame, starting with two references; the entry is
/// made even when the frame cannot be loaded.
///
/// @param self the Toolkit
/// @param file file name within the anims directory
/// @param sequence sequence name; "" selects the first sequence
/// @param frame frame index within the sequence
/// @param key transparency key recorded with the entry
/// @return the image handle, or k_no_handle for a null name or a full table
Handle
image_table_load(void* self, const char* file, const char* sequence, int32_t frame, int32_t key) {
    auto* tk = static_cast<Toolkit*>(self);
    if (file == nullptr || sequence == nullptr) {
        return k_no_handle;
    }
    prepare_images(tk);
    for (int32_t i = 0; i < k_image_entries; ++i) {
        ImageEntry& e = tk->images[i];
        if (e.refs != 0 && equal_nocase(e.file, file) && equal_nocase(e.sequence, sequence) &&
            e.frame == frame && e.key == key) {
            ++e.refs;
            return i;
        }
    }
    for (int32_t i = 0; i < k_image_entries; ++i) {
        ImageEntry& e = tk->images[i];
        if (e.refs != 0) {
            continue;
        }
        char path[k_image_file_bytes * 2];
        std::snprintf(path, sizeof(path), "%s%s", k_image_dir, file);
        e.slot = frame_load(tk, path, sequence, frame);
        std::snprintf(e.file, sizeof(e.file), "%s", file);
        std::free(e.sequence);
        e.sequence = duplicate(sequence);
        e.frame = frame;
        e.refs = k_new_image_refs;
        e.key = key;
        return i;
    }
    return k_no_handle;
}

/// Reads the size of an image's frame (ImageService::size).
///
/// @param self the Toolkit
/// @param image image handle
/// @param[out] width frame width in pixels; left alone for an invalid handle
///     or a frame that did not load
/// @param[out] height frame height in pixels; left alone likewise
void image_table_size(void* self, Handle image, int32_t* width, int32_t* height) {
    auto* tk = static_cast<Toolkit*>(self);
    if (image_valid(tk, image)) {
        frame_size(tk, tk->images[image].slot, width, height);
    }
}

/// Draws an image's frame with its origin at (x, y) (ImageService::draw).
///
/// @param self the Toolkit
/// @param x x of the frame's origin in surface pixels
/// @param y y of the frame's origin in surface pixels
/// @param width box width (ignored)
/// @param height box height (ignored)
/// @param image image handle; an invalid handle draws nothing
void image_table_draw(
    void* self, int32_t x, int32_t y, int32_t /*width*/, int32_t /*height*/, Handle image
) {
    auto* tk = static_cast<Toolkit*>(self);
    if (image_valid(tk, image)) {
        frame_draw(tk, tk->images[image].slot, x, y);
    }
}

// ---------------------------------------------------------------------------
// Fonts

Font* font_at(Toolkit* tk, Handle font) {
    if (!tk->used_fonts)
        return nullptr;
    if (font == -1) {
        font = tk->current_font;
        if (font == -1) {
            return nullptr;
        }
    }
    if (font < 0 || font >= k_font_slots || tk->fonts[font].refs == 0) {
        return nullptr;
    }
    return &tk->fonts[font];
}

/// Returns the glyph a font draws for a character.
///
/// Bytes of 0x80 and above index the table as unsigned values, so every byte stays inside it.
///
/// @param font the font
/// @param ch the character; only its low byte is used
/// @return the character's glyph
const Glyph& glyph_of(const Font* font, int32_t ch) {
    const int32_t index =
        (static_cast<int32_t>(static_cast<uint8_t>(ch)) - font->first_char) & (k_glyphs - 1);
    return font->glyphs[index];
}

/// Returns the kerning between two characters: only "PA" is kerned, by one
/// pixel.
///
/// @param previous character code drawn before
/// @param current character code drawn now
/// @return -1 for 'P' followed by 'A', else 0
int32_t kerning_delta(int32_t previous, int32_t current) {
    return previous == 'P' && current == 'A' ? -1 : 0;
}

void font_clear_glyphs(Font* font) {
    for (Glyph& g : font->glyphs) {
        g.advance = 0;
        g.height = 0;
        g.frame = -1;
    }
}

// The four tables are most of a toolkit. A menu that never asks for one
// leaves that memory unfaulted; the first request prepares it.
void prepare_frames(Toolkit* tk) {
    if (tk->used_frames)
        return;
    const size_t bytes = sizeof(FrameSlot) * static_cast<size_t>(k_frame_slots);
    tk->frames = static_cast<FrameSlot*>(std::malloc(bytes));
    if (tk->frames == nullptr)
        std::abort();
    std::memset(tk->frames, 0, bytes);
    for (int32_t i = 0; i < k_frame_slots; ++i)
        tk->frames[i].gaf = -1;
    tk->used_frames = true;
}

void prepare_images(Toolkit* tk) {
    if (tk->used_images)
        return;
    const size_t bytes = sizeof(ImageEntry) * static_cast<size_t>(k_image_entries);
    tk->images = static_cast<ImageEntry*>(std::malloc(bytes));
    if (tk->images == nullptr)
        std::abort();
    std::memset(tk->images, 0, bytes);
    for (int32_t i = 0; i < k_image_entries; ++i) {
        ImageEntry& e = tk->images[i];
        e.slot = -1;
        e.frame = -1;
        e.key = k_overlay_transparent;
    }
    tk->used_images = true;
}

void prepare_fonts(Toolkit* tk) {
    if (tk->used_fonts)
        return;
    const size_t bytes = sizeof(Font) * static_cast<size_t>(k_font_slots);
    tk->fonts = static_cast<Font*>(std::malloc(bytes));
    if (tk->fonts == nullptr)
        std::abort();
    std::memset(tk->fonts, 0, bytes);
    for (int32_t i = 0; i < k_font_slots; ++i)
        font_clear_glyphs(&tk->fonts[i]);
    tk->used_fonts = true;
}

void prepare_texts(Toolkit* tk) {
    if (tk->used_texts)
        return;
    const size_t bytes = sizeof(TextEntry) * static_cast<size_t>(k_text_entries);
    tk->texts = static_cast<TextEntry*>(std::malloc(bytes));
    if (tk->texts == nullptr)
        std::abort();
    std::memset(tk->texts, 0, bytes);
    tk->used_texts = true;
}

/// Loads a glyph font from the Fonts directory (FontService::load).
///
/// A loaded font with the same name (compared without case) gains a
/// reference. Otherwise the first free of the 15 font slots loads the file:
/// frame i of its first sequence is the glyph of character code i (up to
/// 256), each glyph advancing by its frame width less its origin x, and the
/// line height is the height of 'I'. A glyph whose frame fails to load
/// keeps a zero advance.
///
/// @param self the Toolkit
/// @param name font file name, looked up in the Fonts directory unless it
///     names a directory
/// @return the font handle, or k_no_handle for a null name, no free slot or a
///     file with no frames
Handle font_manager_load(void* self, const char* name) {
    auto* tk = static_cast<Toolkit*>(self);
    if (name == nullptr) {
        return k_no_handle;
    }
    prepare_fonts(tk);
    for (int32_t i = 0; i < k_font_slots; ++i) {
        Font& f = tk->fonts[i];
        if (f.refs != 0 && equal_nocase(f.name, name)) {
            ++f.refs;
            return i;
        }
    }
    int32_t index = -1;
    for (int32_t i = 0; i < k_font_slots; ++i) {
        if (tk->fonts[i].refs == 0) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        return k_no_handle;
    }
    Font& f = tk->fonts[index];
    char* path = join_path(k_font_dir, name);
    std::free(f.name);
    f.name = duplicate(name);
    int32_t count = gaf_frame_count(tk, path, "", 0);
    if (count == 0) {
        std::free(path);
        return k_no_handle;
    }
    if (count > k_glyphs) {
        count = k_glyphs;
    }
    font_clear_glyphs(&f);
    for (int32_t i = 0; i < count; ++i) {
        Glyph& g = f.glyphs[i];
        g.frame = frame_load(tk, path, "", i);
        int32_t width = 0;
        int32_t height = 0;
        frame_size(tk, g.frame, &width, &height);
        // A frame that failed to load has no origin; its glyph keeps a zero
        // advance.
        const Sprite* sprite = frame_sprite(tk, g.frame);
        g.advance = width - (sprite != nullptr ? sprite->origin_x : 0);
        g.height = height;
        if (i == k_line_height_glyph) {
            f.line_height = g.height;
        }
    }
    f.first_char = 0;
    f.face_key[0] = '\0';
    f.ink_known = 0;
    ++f.refs;
    std::free(path);
    return index;
}

/// Drops one reference to a font; the last one frees its name and glyph
/// frames and, when it was current, leaves no current font
/// (FontService::release).
///
/// @param self the Toolkit
/// @param font font handle; an invalid or free handle is ignored
void font_manager_release(void* self, Handle font) {
    auto* tk = static_cast<Toolkit*>(self);
    if (!tk->used_fonts || font < 0 || font >= k_font_slots || tk->fonts[font].refs == 0) {
        return;
    }
    Font& f = tk->fonts[font];
    if (--f.refs != 0) {
        return;
    }
    if (tk->current_font == font) {
        tk->current_font = -1;
    }
    std::free(f.name);
    f.name = nullptr;
    f.face_key[0] = '\0';
    f.ink_known = 0;
    for (Glyph& g : f.glyphs) {
        frame_release(tk, g.frame);
        g.frame = -1;
    }
}

/// Adds one reference to a loaded font (FontService::add_ref).
///
/// @param self the Toolkit
/// @param font font handle; an invalid or free handle is ignored
void font_manager_add_ref(void* self, Handle font) {
    auto* tk = static_cast<Toolkit*>(self);
    if (tk->used_fonts && font >= 0 && font < k_font_slots && tk->fonts[font].refs != 0) {
        ++tk->fonts[font].refs;
    }
}

/// Returns a font's line height, the height of its 'I' glyph
/// (FontService::line_height).
///
/// @param self the Toolkit
/// @param font font handle; -1 uses the current font
/// @return the line height in pixels, or 0 for an unknown font
int32_t font_manager_line_height(void* self, Handle font) {
    const Font* f = font_at(static_cast<Toolkit*>(self), font);
    return f != nullptr ? f->line_height : 0;
}

const char* atlas_face_name(const Font* font) {
    if (font->face_key[0] != '\0') {
        return font->face_key;
    }
    return font->name != nullptr ? font->name : "";
}

/// A character may break the line after it: an ideograph, kana or hangul.
bool break_after(char32_t code) {
    if (code >= 0x1100 && code <= 0x11FF) {
        return true;
    }
    if (code >= 0x3040 && code <= 0x30FF) {
        return true;
    }
    if (code >= 0x31F0 && code <= 0x31FF) {
        return true;
    }
    if (code >= 0x3400 && code <= 0x4DBF) {
        return true;
    }
    if (code >= 0x4E00 && code <= 0x9FFF) {
        return true;
    }
    if (code >= 0xAC00 && code <= 0xD7AF) {
        return true;
    }
    if (code >= 0xF900 && code <= 0xFAFF) {
        return true;
    }
    return false;
}

/// The colour most of the face's ink uses, and the colour on the ink's right
/// edge when that differs. A face painted in one colour stays flat.
void ensure_ink(Toolkit* tk, Font* font) {
    if (font->ink_known != 0) {
        return;
    }
    font->ink_known = 1;
    int counts[k_glyphs] = {};
    int edges[k_glyphs] = {};
    for (int32_t code = 0x20; code <= 0x7E; ++code) {
        const Sprite* sprite = frame_sprite(tk, font->glyphs[code].frame);
        if (sprite == nullptr || sprite->data == nullptr) {
            continue;
        }
        const auto* pixels = static_cast<const uint8_t*>(sprite->data);
        for (int32_t row = 0; row < sprite->height; ++row) {
            const uint8_t* line = pixels + static_cast<ptrdiff_t>(row) * sprite->width;
            for (int32_t column = 0; column < sprite->width; ++column) {
                const uint8_t pixel = line[column];
                if (pixel == sprite->key) {
                    continue;
                }
                ++counts[pixel];
                const bool right_clear =
                    column + 1 >= sprite->width || line[column + 1] == sprite->key;
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
    font->body = static_cast<uint8_t>(body);
    font->edge = static_cast<uint8_t>(edge);
    font->shaded = font->edge != font->body ? 1 : 0;
}

struct GlyphRun {
    int32_t advance = 0;
    int32_t above = 0;
    int32_t below = 0;
    bool analogue = false;
};

/// Advance and ink of one character. Pictures the face owns stay inside its line.
GlyphRun glyph_run(Toolkit* tk, Font* font, char32_t code) {
    GlyphRun run;
    if (code < static_cast<char32_t>(k_glyphs)) {
        run.advance = glyph_of(font, static_cast<int32_t>(code)).advance;
        return run;
    }
    if (tk->atlas == nullptr) {
        return run;
    }
    FontAtlasPlacement place{};
    if (!font_atlas_lookup(tk->atlas, atlas_face_name(font), code, &place)) {
        return run;
    }
    run.analogue = true;
    run.advance = place.advance;
    if (place.top > font->line_height) {
        run.above = place.top - font->line_height;
    }
    const int32_t under = place.rows - place.top;
    if (under > 0) {
        run.below = under;
    }
    return run;
}

void fit_text(Toolkit* tk, Font* font, const char* text, TextFit* fit) {
    fit->width = 0;
    fit->line_height = font->line_height;
    fit->above = 0;
    fit->below = 0;
    if (text == nullptr) {
        return;
    }
    int32_t previous = ' ';
    for (int32_t index = 0; text[index] != '\0';) {
        const utf8::Unit unit = utf8::read(text + index);
        const int32_t bytes = unit.bytes > 0 ? unit.bytes : 1;
        const char32_t code =
            unit.well_formed ? unit.code : static_cast<unsigned char>(text[index]);
        const GlyphRun run = glyph_run(tk, font, code);
        fit->width += run.advance;
        if (font->kerning != 0) {
            fit->width += kerning_delta(previous, static_cast<int32_t>(code));
        }
        if (run.above > fit->above) {
            fit->above = run.above;
        }
        if (run.below > fit->below) {
            fit->below = run.below;
        }
        previous = static_cast<int32_t>(code);
        index += bytes;
    }
}

void draw_unit(
    Toolkit* tk, Surface* target, Font* font, char32_t code, int32_t pen_x, int32_t pen_y
) {
    if (code < static_cast<char32_t>(k_glyphs)) {
        if (code != U' ') {
            frame_draw_into(
                tk, target, glyph_of(font, static_cast<int32_t>(code)).frame, pen_x, pen_y
            );
        }
        return;
    }
    if (tk->atlas == nullptr) {
        return;
    }
    ensure_ink(tk, font);
    font_atlas_plot(
        target,
        tk->atlas,
        atlas_face_name(font),
        code,
        pen_x,
        pen_y,
        font->body,
        font->edge,
        font->shaded != 0
    );
}

/// Breaks `src` into `dest`. A break between ideographs keeps both characters.
int32_t wrap_into(
    Toolkit* tk, int32_t width, const char* src, char* dest, int32_t dest_bytes, Handle font
) {
    if (dest == nullptr || dest_bytes <= 0) {
        return 0;
    }
    dest[0] = '\0';
    Font* face = font_at(tk, font);
    if (face == nullptr) {
        return 0;
    }
    std::string text = src != nullptr ? src : "";
    int32_t line_width = 0;
    int32_t lines = 0;
    int32_t previous = ' ';
    int32_t index = 0;
    int32_t break_at = 0;
    int32_t line_start = 0;
    bool insert_break = false;
    while (index < static_cast<int32_t>(text.size())) {
        if (width < line_width) {
            if (!insert_break && break_at == line_start) {
                int32_t prev = line_start;
                int32_t cursor = line_start;
                while (cursor < index) {
                    const utf8::Unit unit = utf8::read(text.c_str() + cursor);
                    const int32_t step = unit.bytes > 0 ? unit.bytes : 1;
                    if (cursor + step >= index) {
                        prev = cursor;
                        break;
                    }
                    cursor += step;
                }
                text.replace(
                    static_cast<std::size_t>(prev), static_cast<std::size_t>(index - prev), 1, '\n'
                );
                index = prev;
            } else if (insert_break) {
                text.insert(static_cast<std::size_t>(break_at), 1, '\n');
                index = break_at;
            } else {
                text[static_cast<std::size_t>(break_at)] = '\n';
                index = break_at;
            }
            line_start = index;
            break_at = index;
            insert_break = false;
            line_width = 0;
            ++lines;
        } else if (text[static_cast<std::size_t>(index)] == '\n') {
            if (index + 1 < static_cast<int32_t>(text.size())) {
                break_at = index + 1;
                line_start = index + 1;
                insert_break = false;
            }
            line_width = 0;
            ++lines;
        } else {
            const utf8::Unit unit = utf8::read(text.c_str() + index);
            const int32_t step = unit.bytes > 0 ? unit.bytes : 1;
            const char32_t code =
                unit.well_formed
                    ? unit.code
                    : static_cast<unsigned char>(text[static_cast<std::size_t>(index)]);
            if (code == U' ') {
                break_at = index;
                insert_break = false;
            }
            const GlyphRun run = glyph_run(tk, face, code);
            const int32_t before = line_width;
            line_width += run.advance;
            if (face->kerning != 0) {
                line_width += kerning_delta(previous, static_cast<int32_t>(code));
            }
            previous = static_cast<int32_t>(code);
            if (break_after(code) && before <= width && line_width <= width) {
                break_at = index + step;
                insert_break = true;
            }
            index += step - 1;
        }
        ++index;
    }
    utf8::copy(dest, static_cast<std::size_t>(dest_bytes), text.c_str(), 0);
    return lines + 1;
}

/// Draws the first characters of a string onto the drawing surface.
///
/// Each glyph hangs from the font's line height below (x, y) (its origin is
/// the baseline); spaces are skipped but advance, and "PA" is kerned when
/// the font kerns.
///
/// @param tk toolkit holding the fonts
/// @param x left edge in surface pixels
/// @param y top of the line in surface pixels
/// @param font font handle; -1 uses the current font; an unknown font draws
///     nothing
/// @param text string to draw
/// @param count maximum number of characters drawn
void font_draw_run(
    Toolkit* tk, int32_t x, int32_t y, Handle font, const char* text, int32_t count
) {
    Font* f = font_at(tk, font);
    if (f == nullptr || text == nullptr) {
        return;
    }
    Surface* target = draw_surface(tk);
    int32_t previous = ' ';
    int32_t used = 0;
    while (text[used] != '\0' && used < count) {
        const utf8::Unit unit = utf8::read(text + used);
        const int32_t step = unit.bytes > 0 ? unit.bytes : 1;
        if (used + step > count) {
            break;
        }
        const char32_t code = unit.well_formed ? unit.code : static_cast<unsigned char>(text[used]);
        const GlyphRun run = glyph_run(tk, f, code);
        draw_unit(tk, target, f, code, x, f->line_height + y);
        x += run.advance;
        if (f->kerning != 0) {
            x += kerning_delta(previous, static_cast<int32_t>(code));
        }
        previous = static_cast<int32_t>(code);
        used += step;
    }
}

/// Sums the advances of the first characters of a string, without kerning.
///
/// @param tk toolkit holding the fonts
/// @param font font handle; -1 uses the current font
/// @param text string to measure
/// @param last index of the last character measured (inclusive); the end of
///     the string also stops the sum
/// @return the width in pixels; 0 for an unknown font or a negative `last`
int32_t font_prefix_width(Toolkit* tk, Handle font, const char* text, int32_t last) {
    Font* f = font_at(tk, font);
    if (f == nullptr || last < 0 || text == nullptr) {
        return 0;
    }
    int32_t width = 0;
    for (int32_t index = 0; text[index] != '\0' && index <= last;) {
        const utf8::Unit unit = utf8::read(text + index);
        const int32_t step = unit.bytes > 0 ? unit.bytes : 1;
        if (index + step - 1 > last) {
            break;
        }
        const char32_t code =
            unit.well_formed ? unit.code : static_cast<unsigned char>(text[index]);
        width += glyph_run(tk, f, code).advance;
        index += step;
    }
    return width;
}

/// Measures a string in a font.
///
/// Kerning is applied as if each character followed a space, so it never
/// changes the width.
///
/// @param tk toolkit holding the fonts
/// @param font font handle; -1 uses the current font
/// @param text string to measure
/// @param[out] width sum of the characters' advances in pixels; left alone
///     for an unknown font
/// @param[out] height the font's line height in pixels; left alone for an
///     unknown font
void font_measure(Toolkit* tk, Handle font, const char* text, int32_t* width, int32_t* height) {
    Font* f = font_at(tk, font);
    if (f == nullptr) {
        return;
    }
    TextFit fit{};
    fit_text(tk, f, text, &fit);
    *width = fit.width;
    *height = f->line_height;
}

/// Breaks a string in place so no line of it is wider than `width`.
///
/// A line that grows too wide breaks at its last space, or inside the word
/// when it has none; existing line feeds start new lines.
///
/// @param tk toolkit holding the fonts
/// @param width maximum line width in pixels
/// @param[in,out] text string to break; spaces become line feeds
/// @param font font handle; -1 uses the current font
/// @return the number of lines, or 0 for an unknown font
/// @quirk A break inside a word overwrites the character before the break
///     point with the line feed.
int32_t font_wrap_lines(Toolkit* tk, int32_t width, char* text, Handle font) {
    if (text == nullptr) {
        return 0;
    }
    const int32_t owned = static_cast<int32_t>(std::strlen(text));
    std::vector<char> broken(static_cast<std::size_t>(owned) * 2 + 2);
    const int32_t lines =
        wrap_into(tk, width, text, broken.data(), static_cast<int32_t>(broken.size()), font);
    const int32_t written = static_cast<int32_t>(std::strlen(broken.data()));
    // The caller owns exactly the original bytes. A longer break is written by
    // toolkit_wrap_text, which is given room for the inserted line feeds.
    if (written <= owned) {
        std::memcpy(text, broken.data(), static_cast<std::size_t>(written) + 1);
    }
    return lines;
}

/// Draws a string inside a box, placed by text_style bits and optionally
/// wrapped.
///
/// The first 511 bytes are used, stopped on a character boundary. With
/// text_style::wrap a string wider
/// than the box is broken into lines. The lines sit at the top, at the
/// bottom or centred vertically (never above the box), and each line at the
/// left, at the right or centred on the part of it that fits the box width;
/// only the characters that fit are drawn. Lines stop once the next one would
/// pass the box's bottom.
///
/// @param tk toolkit holding the fonts
/// @param x left edge of the box in surface pixels
/// @param y top edge of the box in surface pixels
/// @param width box width in pixels
/// @param height box height in pixels
/// @param text string to draw
/// @param style text_style bits
/// @param font font handle; -1 uses the current font; an unknown font draws
///     nothing
void font_draw_aligned(
    Toolkit* tk,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const char* text,
    int32_t style,
    Handle font
) {
    Font* f = font_at(tk, font);
    if (f == nullptr) {
        return;
    }
    char buffer[k_text_draw_bytes];
    utf8::copy(buffer, sizeof buffer, text, 0);
    int32_t text_width = 0;
    int32_t line_height = 0;
    font_measure(tk, font, buffer, &text_width, &line_height);
    std::vector<char> broken(sizeof buffer * 2);
    const char* drawn = buffer;
    if (width < text_width && (style & text_style::wrap) != 0) {
        if (wrap_into(
                tk, width, buffer, broken.data(), static_cast<int32_t>(broken.size()), font
            ) == 0) {
            return;
        }
        drawn = broken.data();
    }

    struct LineBox {
        const char* text;
        int32_t above;
        int32_t below;
        int32_t pitch;
        int32_t width;
        int32_t fitting;
    };

    LineBox boxes[64];
    int32_t count = 0;
    int32_t block = 0;
    const char* cursor = drawn;
    while (*cursor != '\0' && count < 64) {
        LineBox& box = boxes[count];
        box.text = cursor;
        const char* end = cursor;
        int32_t line_width = 0;
        box.fitting = 0;
        while (*end != '\n' && *end != '\0') {
            const utf8::Unit unit = utf8::read(end);
            const int32_t step = unit.bytes > 0 ? unit.bytes : 1;
            const char32_t code = unit.well_formed ? unit.code : static_cast<unsigned char>(*end);
            const int32_t advance = glyph_run(tk, f, code).advance;
            if (line_width < width) {
                line_width += advance;
            }
            if (line_width <= width) {
                box.fitting += step;
            } else {
                line_width = width;
            }
            end += step;
        }
        char saved = *end;
        // Measure this line only. The following byte is restored.
        const_cast<char*>(end)[0] = '\0';
        TextFit fit{};
        fit_text(tk, f, cursor, &fit);
        const_cast<char*>(end)[0] = saved;
        box.above = fit.above;
        box.below = fit.below;
        box.pitch = f->line_height + fit.above + fit.below;
        if (box.pitch < f->line_height) {
            box.pitch = f->line_height;
        }
        box.width = line_width;
        block += box.pitch;
        ++count;
        if (*end == '\0') {
            break;
        }
        cursor = end + 1;
        if (*cursor == '\0') {
            break;
        }
    }
    if (count == 0) {
        return;
    }
    int32_t line_y = y;
    if ((style & text_style::top) == 0) {
        if ((style & text_style::bottom) == 0) {
            line_y = (height - block) / 2 + y;
        } else {
            line_y = (y - block) + height;
        }
        if (line_y < y) {
            line_y = y;
        }
    }
    for (int32_t i = 0; i < count; ++i) {
        const LineBox& box = boxes[i];
        int32_t line_x = x;
        if ((style & text_style::left) == 0) {
            line_x = (style & text_style::right) == 0 ? x + (width - box.width) / 2
                                                      : x + (width - box.width);
        }
        if (y <= line_y) {
            font_draw_run(tk, line_x, line_y + box.above, font, box.text, box.fitting);
        }
        const int32_t next = i + 1 < count ? boxes[i + 1].pitch : box.pitch;
        line_y += box.pitch;
        if (y + height < next + line_y) {
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Text store (TextService)

TextEntry* text_at(Toolkit* tk, Handle text) {
    if (!tk->used_texts || text < 0 || text >= k_text_entries || tk->texts[text].refs == 0) {
        return nullptr;
    }
    return &tk->texts[text];
}

/// Stores a string with its style (TextService::create).
///
/// An entry with the same string and style gains a reference; otherwise the
/// first free of the 1024 entries stores a copy with one reference.
///
/// @param self the Toolkit
/// @param text string to store; null or "" stores nothing
/// @param style text_style bits stored with the string
/// @return the text handle, or k_no_handle
Handle text_store_create(void* self, const char* text, int32_t style) {
    auto* tk = static_cast<Toolkit*>(self);
    if (text == nullptr || text[0] == '\0') {
        return k_no_handle;
    }
    prepare_texts(tk);
    for (int32_t i = 0; i < k_text_entries; ++i) {
        TextEntry& e = tk->texts[i];
        if (e.refs != 0 && e.text != nullptr && std::strcmp(e.text, text) == 0 &&
            e.style == style) {
            ++e.refs;
            return i;
        }
    }
    for (int32_t i = 0; i < k_text_entries; ++i) {
        TextEntry& e = tk->texts[i];
        if (e.refs == 0) {
            std::free(e.text);
            e.text = duplicate(text);
            ++e.refs;
            e.style = style;
            return i;
        }
    }
    return k_no_handle;
}

/// Drops one reference to a text; the last one frees its string
/// (TextService::release).
///
/// @param self the Toolkit
/// @param text text handle; an invalid or free handle is ignored
void text_store_release(void* self, Handle text) {
    TextEntry* e = text_at(static_cast<Toolkit*>(self), text);
    if (e != nullptr && --e->refs == 0) {
        std::free(e->text);
        e->text = nullptr;
    }
}

/// Adds one reference to a stored text (TextService::add_ref).
///
/// @param self the Toolkit
/// @param text text handle; an invalid or free handle is ignored
void text_store_add_ref(void* self, Handle text) {
    TextEntry* e = text_at(static_cast<Toolkit*>(self), text);
    if (e != nullptr) {
        ++e->refs;
    }
}

/// Measures a stored text in a font (TextService::measure, see
/// font_measure).
///
/// @param self the Toolkit
/// @param font font handle; -1 uses the current font
/// @param[out] width text width in pixels; left alone for an invalid handle
///     or unknown font
/// @param[out] height the font's line height; left alone likewise
/// @param text text handle
void text_store_measure(void* self, Handle font, int32_t* width, int32_t* height, Handle text) {
    auto* tk = static_cast<Toolkit*>(self);
    const TextEntry* e = text_at(tk, text);
    if (e != nullptr) {
        font_measure(tk, font, e->text, width, height);
    }
}

/// Returns the advance of one character (TextService::char_width).
///
/// @param self the Toolkit
/// @param font font handle; -1 uses the current font
/// @param ch character code
/// @return the advance in pixels, or 0 for an unknown font
int32_t text_store_char_width(void* self, Handle font, int32_t ch) {
    auto* tk = static_cast<Toolkit*>(self);
    Font* f = font_at(tk, font);
    if (f == nullptr || ch < 0) {
        return 0;
    }
    return glyph_run(tk, f, static_cast<char32_t>(ch)).advance;
}

/// Measures the first characters of a string (TextService::prefix_width, see
/// font_prefix_width).
///
/// @param self the Toolkit
/// @param font font handle; -1 uses the current font
/// @param text string to measure; null measures 0
/// @param last index of the last character measured (inclusive)
/// @return the width in pixels
int32_t text_store_prefix_width(void* self, Handle font, const char* text, int32_t last) {
    if (text == nullptr) {
        return 0;
    }
    return font_prefix_width(static_cast<Toolkit*>(self), font, text, last);
}

/// Draws a string inside a box (TextService::draw_string, see
/// font_draw_aligned).
///
/// @param self the Toolkit
/// @param font font handle; -1 uses the current font
/// @param x left edge of the box in surface pixels
/// @param y top edge of the box in surface pixels
/// @param width box width in pixels
/// @param height box height in pixels
/// @param text string to draw; null draws nothing
/// @param style text_style bits
void text_store_draw_string(
    void* self,
    Handle font,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const char* text,
    int32_t style
) {
    if (text != nullptr) {
        font_draw_aligned(static_cast<Toolkit*>(self), x, y, width, height, text, style, font);
    }
}

/// Measures a string (TextService::measure_string, see font_measure).
///
/// @param self the Toolkit
/// @param font font handle; -1 uses the current font
/// @param[out] width string width in pixels; left alone for a null string or
///     unknown font
/// @param[out] height the font's line height; left alone likewise
/// @param text string to measure
void text_store_measure_string(
    void* self, Handle font, int32_t* width, int32_t* height, const char* text
) {
    if (text != nullptr) {
        font_measure(static_cast<Toolkit*>(self), font, text, width, height);
    }
}

/// Draws a stored text inside a box with its stored style
/// (TextService::draw, see font_draw_aligned).
///
/// @param self the Toolkit
/// @param font font handle; -1 uses the current font
/// @param x left edge of the box in surface pixels
/// @param y top edge of the box in surface pixels
/// @param width box width in pixels
/// @param height box height in pixels
/// @param text text handle; an invalid handle draws nothing
void text_store_draw(
    void* self, Handle font, int32_t x, int32_t y, int32_t width, int32_t height, Handle text
) {
    auto* tk = static_cast<Toolkit*>(self);
    const TextEntry* e = text_at(tk, text);
    if (e != nullptr) {
        font_draw_aligned(tk, x, y, width, height, e->text, e->style & 0xFF, font);
    }
}

/// Returns a stored text's string (TextService::get).
///
/// @param self the Toolkit
/// @param text text handle
/// @return the string, or null for an invalid or free handle
const char* text_store_get(void* self, Handle text) {
    const TextEntry* e = text_at(static_cast<Toolkit*>(self), text);
    return e != nullptr ? e->text : nullptr;
}

// ---------------------------------------------------------------------------
// Input, colours, cursor, sounds and frame drawing

/// Reports a key state recorded by toolkit_set_key
/// (InputService::is_key_down).
///
/// @param self the Toolkit
/// @param key key code
/// @return true while the key is recorded as held; false outside 0-0x1FF
bool input_state_key_down(void* self, int32_t key) {
    auto* tk = static_cast<Toolkit*>(self);
    return key >= 0 && key < k_key_states && tk->keys[key] != 0;
}

// Nearest palette entry; handles are palette indexes.
Handle palette_color_create(void* self, uint8_t r, uint8_t g, uint8_t b, uint8_t) {
    auto* tk = static_cast<Toolkit*>(self);
    const Palette* palette = tk->config.palette;
    if (palette == nullptr) {
        return k_no_handle;
    }
    int32_t best = 0;
    int32_t best_distance = 0x7FFFFFFF;
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i) {
        const PaletteEntry& e = palette->entries[i];
        const int32_t dr = e.r - r;
        const int32_t dg = e.g - g;
        const int32_t db = e.b - b;
        const int32_t distance = dr * dr + dg * dg + db * db;
        if (distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}

void palette_color_release(void*, Handle) {
}

/// Fills a rectangle of the drawing surface with a palette index, clipped to
/// the surface (ColorService::fill).
///
/// @param self the Toolkit
/// @param x left edge in surface pixels
/// @param y top edge in surface pixels
/// @param width width in pixels
/// @param height height in pixels
/// @param color colour handle, a palette index; outside 0-255 nothing is
///     drawn
void palette_color_fill(
    void* self, int32_t x, int32_t y, int32_t width, int32_t height, Handle color
) {
    auto* tk = static_cast<Toolkit*>(self);
    if (color >= 0 && color < OA_PALETTE_COLORS) {
        fill_rect(draw_surface(tk), x, y, width, height, static_cast<uint8_t>(color));
    }
}

/// Empties the cursor table: every slot unloaded with empty names, no image
/// service linked, the cursor hidden and neither a current nor a default
/// cursor.
///
/// @param[out] table table to reset; nothing it held is freed
void cursor_table_init(CursorTable* table) {
    for (CursorEntry& e : table->entries) {
        e.file = duplicate("");
        e.sequence = duplicate("");
        e.refs = 0;
        e.frame = 0;
        e.images = nullptr;
        e.image_count = 0;
    }
    table->images = nullptr;
    table->visible = 0;
    table->default_cursor = -1;
    table->current = -1;
    table->countdown = 0;
}

void cursor_entry_append(CursorEntry* e, Handle image) {
    auto* grown = static_cast<Handle*>(
        std::realloc(e->images, sizeof(Handle) * static_cast<size_t>(e->image_count + 1))
    );
    if (grown == nullptr) {
        std::abort();
    }
    grown[e->image_count++] = image;
    e->images = grown;
}

/// Loads an animated cursor from a GAF sequence (CursorService::load).
///
/// A cursor already loaded under both names (compared without case, in any
/// slot) gains a reference. Otherwise the first unreferenced slot takes the
/// names and one image per frame of the sequence (key 9) from the linked
/// image service, or the single image -1 while none is linked.
///
/// @param self the Toolkit
/// @param file GAF file name, as the image service takes it
/// @param sequence sequence name
/// @return the cursor slot, or -1 for a null name or a full table
int32_t cursor_table_load(void* self, const char* file, const char* sequence) {
    CursorTable& table = static_cast<Toolkit*>(self)->cursors;
    if (file == nullptr || sequence == nullptr) {
        return -1;
    }
    for (int32_t i = 0; i < k_cursor_slots; ++i) {
        CursorEntry& e = table.entries[i];
        if (equal_nocase(e.file, file) && equal_nocase(e.sequence, sequence)) {
            ++e.refs;
            return i;
        }
    }
    for (int32_t i = 0; i < k_cursor_slots; ++i) {
        CursorEntry& e = table.entries[i];
        if (e.refs != 0) {
            continue;
        }
        std::free(e.file);
        e.file = duplicate(file);
        std::free(e.sequence);
        e.sequence = duplicate(sequence);
        e.refs = 1;
        e.frame = 0;
        const ImageService* images = table.images;
        if (images == nullptr) {
            cursor_entry_append(&e, k_no_handle);
            return i;
        }
        const int32_t count = images->frame_count != nullptr
                                  ? images->frame_count(images->self, file, sequence, 0)
                                  : 0;
        for (int32_t frame = 0; frame < count; ++frame) {
            cursor_entry_append(
                &e,
                images->load != nullptr
                    ? images->load(images->self, file, sequence, frame, k_cursor_image_key)
                    : k_no_handle
            );
        }
        return i;
    }
    return -1;
}

/// Shows the cursor (CursorService::show).
///
/// @param self the Toolkit
void cursor_table_show(void* self) {
    static_cast<Toolkit*>(self)->cursors.visible = 1;
}

/// Hides the cursor (CursorService::hide).
///
/// @param self the Toolkit
void cursor_table_hide(void* self) {
    static_cast<Toolkit*>(self)->cursors.visible = 0;
}

/// Selects the cursor shown (CursorService::set).
///
/// @param self the Toolkit
/// @param cursor cursor slot; -1 returns to the default cursor; a slot that
///     is not loaded is ignored
void cursor_table_set(void* self, int32_t cursor) {
    CursorTable& table = static_cast<Toolkit*>(self)->cursors;
    if (cursor == -1) {
        table.current = -1;
    } else if (cursor >= 0 && cursor < k_cursor_slots && table.entries[cursor].refs != 0) {
        table.current = cursor;
    }
}

/// Selects the cursor shown while none is set (CursorService::set_default).
///
/// @param self the Toolkit
/// @param cursor any slot from -1 to 31, loaded or not; others are ignored
void cursor_table_set_default(void* self, int32_t cursor) {
    if (cursor > -2 && cursor < k_cursor_slots) {
        static_cast<Toolkit*>(self)->cursors.default_cursor = cursor;
    }
}

int32_t cursor_table_shown(const CursorTable& table) {
    return table.current != -1 ? table.current : table.default_cursor;
}

// The shown cursor's slot when its frame index is inside its images.
const CursorEntry* cursor_table_shown_frame(const CursorTable& table) {
    const int32_t shown = cursor_table_shown(table);
    if (shown == -1) {
        return nullptr;
    }
    const CursorEntry& e = table.entries[shown];
    return e.frame >= 0 && e.frame < e.image_count ? &e : nullptr;
}

/// Registers a UI sound by file name (SoundService::load).
///
/// A sound with the same name (compared without case) gains a reference;
/// otherwise the first free of the 128 entries stores the name with one
/// reference. Nothing is read until the sound plays.
///
/// @param self the Toolkit
/// @param name sound file name within the sounds directory
/// @return the sound handle, or k_no_handle for a null name or a full table
Handle sound_table_load(void* self, const char* name) {
    auto* tk = static_cast<Toolkit*>(self);
    if (name == nullptr) {
        return k_no_handle;
    }
    for (int32_t i = 0; i < k_sound_entries; ++i) {
        if (tk->sounds[i].refs != 0 && equal_nocase(tk->sounds[i].name, name)) {
            ++tk->sounds[i].refs;
            return i;
        }
    }
    for (int32_t i = 0; i < k_sound_entries; ++i) {
        SoundEntry& e = tk->sounds[i];
        if (e.refs == 0) {
            std::free(e.name);
            e.name = duplicate(name);
            ++e.refs;
            return i;
        }
    }
    return k_no_handle;
}

/// Drops one reference to a sound; the last one frees its name
/// (SoundService::release).
///
/// @param self the Toolkit
/// @param sound sound handle; an invalid or free handle is ignored
void sound_table_release(void* self, Handle sound) {
    auto* tk = static_cast<Toolkit*>(self);
    if (sound >= 0 && sound < k_sound_entries && tk->sounds[sound].refs != 0 &&
        --tk->sounds[sound].refs == 0) {
        std::free(tk->sounds[sound].name);
        tk->sounds[sound].name = nullptr;
    }
}

/// Plays a UI sound from the sounds directory through the platform audio
/// hook at -585 (-5.85 dB) (SoundService::play).
///
/// @param self the Toolkit
/// @param sound sound handle; an invalid handle, or no audio hook, plays
///     nothing
void sound_table_play(void* self, Handle sound) {
    auto* tk = static_cast<Toolkit*>(self);
    if (sound < 0 || sound >= k_sound_entries || tk->sounds[sound].refs == 0 ||
        tk->config.audio.play == nullptr) {
        return;
    }
    char* path = join_path(k_sound_dir, tk->sounds[sound].name);
    tk->config.audio.play(tk->config.audio.user, path, k_ui_sound_volume);
    std::free(path);
}

/// Plays the error beep through the platform audio hook at -585 (-5.85 dB)
/// (SoundService::beep).
///
/// @param self the Toolkit; with no audio hook the beep is silent
void sound_table_beep(void* self) {
    auto* tk = static_cast<Toolkit*>(self);
    if (tk->config.audio.play == nullptr) {
        return;
    }
    char* path = join_path(k_sound_dir, k_beep_file);
    tk->config.audio.play(tk->config.audio.user, path, k_ui_sound_volume);
    std::free(path);
}

/// Nearest palette index for the white focus frame and caret.
///
/// The overlay's transparent index is skipped, so the mark stays visible
/// when that entry is the closest white.
///
/// @param tk toolkit whose palette is searched
/// @return a palette index, or k_no_handle when the toolkit has no palette
Handle focus_ink(Toolkit* tk) {
    const Palette* palette = tk->config.palette;
    if (palette == nullptr) {
        return k_no_handle;
    }
    int32_t best = k_no_handle;
    int32_t best_distance = 0x7FFFFFFF;
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i) {
        if (i == k_overlay_transparent) {
            continue;
        }
        const PaletteEntry& entry = palette->entries[i];
        const int32_t dr = static_cast<int32_t>(entry.r) - 255;
        const int32_t dg = static_cast<int32_t>(entry.g) - 255;
        const int32_t db = static_cast<int32_t>(entry.b) - 255;
        const int32_t distance = dr * dr + dg * dg + db * db;
        if (distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}

/// Draws a one-pixel white rectangle outline (DrawService::focus_frame and
/// DrawService::frame).
///
/// A non-positive width or height, or no usable white in the palette, draws
/// nothing. The outline is clipped to the drawing surface.
///
/// @param self the Toolkit
/// @param x left edge in surface pixels
/// @param y top edge in surface pixels
/// @param width width in pixels
/// @param height height in pixels
void draw_outline(void* self, int32_t x, int32_t y, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    auto* tk = static_cast<Toolkit*>(self);
    const Handle color = focus_ink(tk);
    if (color < 0) {
        return;
    }
    Surface* surface = draw_surface(tk);
    const auto ink = static_cast<uint8_t>(color);
    fill_rect(surface, x, y, width, 1, ink);
    fill_rect(surface, x, y + height - 1, width, 1, ink);
    fill_rect(surface, x, y, 1, height, ink);
    fill_rect(surface, x + width - 1, y, 1, height, ink);
}

/// Draws the text caret as a two-pixel-wide white bar (DrawService::caret).
///
/// Nothing is drawn while `visible` is 0, while the height is not positive,
/// or when the palette has no usable white. The bar is clipped to the
/// drawing surface.
///
/// @param self the Toolkit
/// @param x left edge in surface pixels
/// @param y top edge in surface pixels
/// @param visible visibility flag; 0 hides the caret
/// @param height caret height in pixels
void draw_caret_bar(void* self, int32_t x, int32_t y, int32_t visible, int32_t height) {
    if (visible == 0 || height <= 0) {
        return;
    }
    auto* tk = static_cast<Toolkit*>(self);
    const Handle color = focus_ink(tk);
    if (color < 0) {
        return;
    }
    fill_rect(draw_surface(tk), x, y, k_caret_width, height, static_cast<uint8_t>(color));
}

void clear_overlay(Surface* surface) {
    if (surface->pixels != nullptr) {
        std::memset(
            surface->pixels,
            k_overlay_transparent,
            static_cast<size_t>(surface->pitch) * static_cast<size_t>(surface->height)
        );
    }
}

/// Replaces the "UI MAIN" overlay with a new 8-bit memory surface cleared to
/// the transparent index 9, clipped to its own bounds.
///
/// @param[in,out] tk toolkit whose overlay is replaced; the old pixels are
///     freed
/// @param width overlay width in pixels; negative counts as 0
/// @param height overlay height in pixels; negative counts as 0
void toolkit_create_overlay(Toolkit* tk, int32_t width, int32_t height) {
    std::free(tk->overlay.pixels);
    width = width > 0 ? width : 0;
    height = height > 0 ? height : 0;
    tk->overlay.width = width;
    tk->overlay.height = height;
    tk->overlay.pitch = width;
    tk->overlay.pixels = static_cast<uint8_t*>(
        std::malloc(static_cast<size_t>(width) * static_cast<size_t>(height) + 1)
    );
    tk->overlay.reserved_after_pixels = OA_SURFACE_RESERVED_AFTER_PIXELS_INIT;
    tk->overlay.reserved_before_origin = OA_SURFACE_RESERVED_BEFORE_ORIGIN_INIT;
    tk->overlay.clip = Rect32{0, 0, width - 1, height - 1};
    tk->overlay.flags = OA_SURFACE_FLAG_MEMORY;
    clear_overlay(&tk->overlay);
}

/// Finishes building a toolkit: gives the desktop the screen area, records
/// the overlay's size without its pixels, loads the default font
/// (Fonts\BYHatFontSmall87.GAF) and makes it current unless the toolkit
/// asked to defer its tables, links the cursor manager to the toolkit's
/// image service and shows the cursor.
///
/// The overlay's pixels are allocated the first time a draw has no target
/// of its own. The main menu always names a target, so it does not.
///
/// Installing the services is left to the toolkit's owner.
///
/// @param[in,out] tk toolkit to finish
/// @param x left edge of the screen area in pixels
/// @param y top edge of the screen area in pixels
/// @param width screen width in pixels; also the overlay width
/// @param height screen height in pixels; also the overlay height
void toolkit_init(Toolkit* tk, int32_t x, int32_t y, int32_t width, int32_t height) {
    desktop_init(&tk->desktop, x, y, width, height);
    std::free(tk->overlay.pixels);
    tk->overlay.pixels = nullptr;
    width = width > 0 ? width : 0;
    height = height > 0 ? height : 0;
    tk->overlay.width = width;
    tk->overlay.height = height;
    tk->overlay.pitch = width;
    tk->overlay.reserved_after_pixels = OA_SURFACE_RESERVED_AFTER_PIXELS_INIT;
    tk->overlay.reserved_before_origin = OA_SURFACE_RESERVED_BEFORE_ORIGIN_INIT;
    tk->overlay.clip = Rect32{0, 0, width - 1, height - 1};
    tk->overlay.flags = OA_SURFACE_FLAG_MEMORY;
    if (tk->config.defer_tables) {
        tk->default_font = k_no_handle;
        tk->current_font = k_no_handle;
    } else {
        tk->default_font = font_manager_load(tk, k_default_font);
        toolkit_select_font(tk, tk->default_font);
    }
    toolkit_set_cursor_images(tk, &tk->image_service);
    cursor_table_show(tk);
}

} // namespace

uint8_t toolkit_font_ink(Handle font) {
    const FontService* fonts = services().fonts;
    if (fonts == nullptr || fonts->self == nullptr) {
        return 0;
    }
    auto* toolkit = static_cast<Toolkit*>(fonts->self);
    Font* face = font_at(toolkit, font);
    if (face == nullptr) {
        return 0;
    }
    ensure_ink(toolkit, face);
    return face->body;
}

Surface* toolkit_drawing_surface() {
    const FontService* fonts = services().fonts;
    if (fonts == nullptr || fonts->self == nullptr) {
        return nullptr;
    }
    return draw_surface(static_cast<Toolkit*>(fonts->self));
}

Toolkit* toolkit_create(const ToolkitConfig* config) {
    // The frame, image, font and text tables are allocated on first use, so
    // this block does not hold them. A main menu that never asks pays none
    // of them.
    auto* tk = static_cast<Toolkit*>(std::malloc(sizeof(Toolkit)));
    if (tk == nullptr) {
        std::abort();
    }
    tk->config = *config;
    tk->overlay = {};
    tk->overlay.pixels = nullptr;
    tk->target = nullptr;
    tk->desktop = {};
    std::fill(std::begin(tk->gafs), std::end(tk->gafs), GafFile{});
    tk->frames = nullptr;
    tk->images = nullptr;
    tk->fonts = nullptr;
    tk->texts = nullptr;
    tk->current_font = -1;
    std::memset(tk->sounds, 0, sizeof(tk->sounds));
    std::memset(tk->keys, 0, sizeof(tk->keys));
    tk->default_font = k_no_handle;
    tk->atlas = nullptr;
    tk->used_frames = false;
    tk->used_images = false;
    tk->used_fonts = false;
    tk->used_texts = false;
    if (!config->defer_tables) {
        prepare_frames(tk);
        prepare_images(tk);
        prepare_fonts(tk);
        prepare_texts(tk);
    }
    cursor_table_init(&tk->cursors);

    tk->image_service = ImageService{
        tk,
        image_table_release,
        image_table_add_ref,
        image_table_frame_count,
        image_table_load,
        image_table_size,
        image_table_draw,
        nullptr, // no blank images: the console image store provides them
        nullptr,
        nullptr, // frame delays are all zero here
    };
    tk->text_service = TextService{
        tk,
        text_store_release,
        text_store_add_ref,
        text_store_create,
        text_store_measure,
        text_store_char_width,
        text_store_prefix_width,
        text_store_draw_string,
        text_store_measure_string,
        text_store_draw,
        text_store_get,
    };
    tk->font_service = FontService{
        tk, font_manager_load, font_manager_release, font_manager_add_ref, font_manager_line_height
    };
    tk->input_service = InputService{tk, input_state_key_down};
    tk->color_service =
        ColorService{tk, palette_color_release, palette_color_create, palette_color_fill};
    tk->cursor_service = CursorService{
        tk,
        cursor_table_load,
        cursor_table_show,
        cursor_table_hide,
        cursor_table_set,
        cursor_table_set_default,
    };
    tk->sound_service =
        SoundService{tk, sound_table_load, sound_table_release, sound_table_play, sound_table_beep};
    tk->draw_service = DrawService{tk, draw_outline, draw_outline, draw_caret_bar};
    toolkit_init(tk, 0, 0, config->width, config->height);
    return tk;
}

void toolkit_destroy(Toolkit* toolkit) {
    if (toolkit == nullptr) {
        return;
    }
    desktop_release(&toolkit->desktop);
    if (toolkit->used_fonts) {
        for (int32_t i = 0; i < k_font_slots; ++i)
            std::free(toolkit->fonts[i].name);
        std::free(toolkit->fonts);
    }
    if (toolkit->used_images) {
        toolkit_release_images_held_once(toolkit);
        for (int32_t i = 0; i < k_image_entries; ++i)
            std::free(toolkit->images[i].sequence);
        std::free(toolkit->images);
    }
    if (toolkit->used_texts) {
        for (int32_t i = 0; i < k_text_entries; ++i)
            std::free(toolkit->texts[i].text);
        std::free(toolkit->texts);
    }
    for (SoundEntry& e : toolkit->sounds) {
        std::free(e.name);
    }
    for (CursorEntry& e : toolkit->cursors.entries) {
        std::free(e.file);
        std::free(e.sequence);
        std::free(e.images);
    }
    if (toolkit->used_frames) {
        for (int32_t i = 0; i < k_frame_slots; ++i)
            std::free(toolkit->frames[i].sprite);
        std::free(toolkit->frames);
    }
    for (GafFile& g : toolkit->gafs) {
        std::free(g.path);
        delete g.archive;
    }
    std::free(toolkit->overlay.pixels);
    std::free(toolkit);
}

void toolkit_release_images_held_once(Toolkit* toolkit) {
    if (toolkit == nullptr || !toolkit->used_images)
        return;
    for (int32_t i = 0; i < k_image_entries; ++i) {
        if (toolkit->images[i].refs == 1) {
            image_table_release(toolkit, i);
        }
    }
}

Services toolkit_services(Toolkit* toolkit) {
    Services s{};
    s.desktop = &toolkit->desktop;
    s.images = &toolkit->image_service;
    s.texts = &toolkit->text_service;
    s.fonts = &toolkit->font_service;
    s.input = &toolkit->input_service;
    s.colors = &toolkit->color_service;
    s.cursors = &toolkit->cursor_service;
    s.sounds = &toolkit->sound_service;
    s.draw = &toolkit->draw_service;
    return s;
}

Desktop* toolkit_desktop(Toolkit* toolkit) {
    return &toolkit->desktop;
}

Surface* toolkit_surface(Toolkit* toolkit) {
    ensure_overlay_storage(toolkit);
    return &toolkit->overlay;
}

void toolkit_set_target(Toolkit* toolkit, Surface* target) {
    toolkit->target = target;
}

void toolkit_compose(Toolkit* toolkit, Surface* target) {
    const Surface& overlay = toolkit->overlay;
    if (target != nullptr && target->pixels != nullptr && overlay.pixels != nullptr) {
        const int32_t rows = overlay.height < target->height ? overlay.height : target->height;
        const int32_t cols = overlay.width < target->width ? overlay.width : target->width;
        for (int32_t row = 0; row < rows; ++row) {
            const uint8_t* in = overlay.pixels + static_cast<ptrdiff_t>(row) * overlay.pitch;
            uint8_t* out = target->pixels + static_cast<ptrdiff_t>(row) * target->pitch;
            for (int32_t col = 0; col < cols; ++col) {
                if (in[col] != k_overlay_transparent) {
                    out[col] = in[col];
                }
            }
        }
    }
    clear_overlay(&toolkit->overlay);
}

void toolkit_set_key(Toolkit* toolkit, int32_t key, bool down) {
    if (key >= 0 && key < k_key_states) {
        toolkit->keys[key] = down ? 1 : 0;
    }
}

void toolkit_set_cursor_images(Toolkit* toolkit, const ImageService* images) {
    toolkit->cursors.images = images;
}

void toolkit_step_cursor(Toolkit* toolkit) {
    CursorTable& table = toolkit->cursors;
    const CursorEntry* shown = cursor_table_shown_frame(table);
    if (shown == nullptr || --table.countdown >= 0) {
        return;
    }
    CursorEntry& e = table.entries[shown - table.entries];
    if (++e.frame >= e.image_count) {
        e.frame = 0;
    }
    const ImageService* images = table.images;
    if (images == nullptr) {
        table.countdown = k_cursor_unlinked_delay;
    } else {
        table.countdown = images->frame_delay != nullptr
                              ? images->frame_delay(images->self, e.images[e.frame])
                              : 0;
    }
}

void toolkit_draw_cursor(Toolkit* toolkit, int32_t x, int32_t y) {
    const CursorTable& table = toolkit->cursors;
    const CursorEntry* shown = cursor_table_shown_frame(table);
    if (table.visible == 0 || shown == nullptr) {
        return;
    }
    const ImageService* images = table.images;
    if (images != nullptr && images->draw != nullptr) {
        images->draw(images->self, x, y, 0, 0, shown->images[shown->frame]);
    }
}

int32_t toolkit_cursor(const Toolkit* toolkit) {
    return toolkit->cursors.current;
}

Handle toolkit_default_font(const Toolkit* toolkit) {
    return toolkit->default_font;
}

void toolkit_load_default_font(Toolkit* toolkit) {
    if (toolkit == nullptr || toolkit->default_font != k_no_handle ||
        toolkit->font_service.load == nullptr) {
        return;
    }
    toolkit->default_font = toolkit->font_service.load(toolkit, "BYHatFontSmall87.GAF");
    toolkit_select_font(toolkit, toolkit->default_font);
}

void toolkit_select_font(Toolkit* toolkit, Handle font) {
    if (toolkit->used_fonts && font >= 0 && font < k_font_slots && toolkit->fonts[font].refs != 0) {
        toolkit->current_font = font;
    }
}

Handle toolkit_font_tint(Toolkit* toolkit, Handle base, uint8_t color) {
    if (!toolkit->used_fonts || base < 0 || base >= k_font_slots ||
        toolkit->fonts[base].refs == 0) {
        return k_no_handle;
    }
    const Font& source = toolkit->fonts[base];
    for (int32_t index = 0; index < k_font_slots; ++index) {
        Font& f = toolkit->fonts[index];
        if (f.refs != 0) {
            continue;
        }
        std::free(f.name);
        f.name = duplicate("");
        for (int32_t i = 0; i < k_glyphs; ++i) {
            const Glyph& from = source.glyphs[i];
            Glyph& to = f.glyphs[i];
            const Sprite* sprite = frame_sprite(toolkit, from.frame);
            if (sprite == nullptr) {
                to.frame = -1;
            } else {
                int32_t width = 0;
                int32_t height = 0;
                frame_size(toolkit, from.frame, &width, &height);
                Sprite* tinted = make_sprite(
                    width, height, sprite->origin_x, sprite->origin_y, k_overlay_transparent
                );
                const auto* in = static_cast<const uint8_t*>(sprite->data);
                auto* out = static_cast<uint8_t*>(tinted->data);
                const size_t pixels = static_cast<size_t>(width) * height;
                for (size_t p = 0; p < pixels; ++p) {
                    out[p] = in[p] != k_overlay_transparent ? color : k_overlay_transparent;
                }
                to.frame = frame_insert_sprite(toolkit, tinted);
            }
            to.advance = from.advance;
            to.height = from.height;
        }
        f.line_height = source.line_height;
        f.first_char = source.first_char;
        f.kerning = source.kerning;
        const char* key = source.face_key[0] != '\0' ? source.face_key : source.name;
        f.face_key[0] = '\0';
        if (key != nullptr) {
            std::snprintf(f.face_key, sizeof f.face_key, "%s", key);
        }
        f.ink_known = 0;
        ++f.refs;
        return index;
    }
    return k_no_handle;
}

void toolkit_set_font_atlas(Toolkit* toolkit, const FontAtlas* atlas) {
    if (toolkit == nullptr) {
        return;
    }
    toolkit->atlas = atlas;
}

void toolkit_text_fit(Toolkit* toolkit, Handle font, const char* text, TextFit* fit) {
    if (fit == nullptr) {
        return;
    }
    Font* face = font_at(toolkit, font);
    if (face == nullptr) {
        return;
    }
    fit_text(toolkit, face, text, fit);
}

void toolkit_measure_text(
    Toolkit* toolkit, Handle font, const char* text, int32_t* width, int32_t* height
) {
    font_measure(toolkit, font, text, width, height);
}

int32_t toolkit_wrap_text(
    Toolkit* toolkit, int32_t width, const char* text, char* dest, int32_t dest_bytes, Handle font
) {
    return wrap_into(toolkit, width, text, dest, dest_bytes, font);
}

int32_t toolkit_wrap_lines(Toolkit* toolkit, int32_t width, char* text, Handle font) {
    return font_wrap_lines(toolkit, width, text, font);
}

void toolkit_draw_text_run(
    Toolkit* toolkit, Surface* target, Handle font, const char* text, int32_t count
) {
    Font* f = font_at(toolkit, font);
    if (f == nullptr || text == nullptr || target == nullptr) {
        return;
    }
    int32_t x = 0;
    int32_t previous = ' ';
    int32_t used = 0;
    while (text[used] != '\0' && used < count) {
        const utf8::Unit unit = utf8::read(text + used);
        const int32_t step = unit.bytes > 0 ? unit.bytes : 1;
        if (used + step > count) {
            break;
        }
        const char32_t code = unit.well_formed ? unit.code : static_cast<unsigned char>(text[used]);
        const GlyphRun run = glyph_run(toolkit, f, code);
        draw_unit(toolkit, target, f, code, x, f->line_height);
        x += run.advance;
        if (f->kerning != 0) {
            x += kerning_delta(previous, static_cast<int32_t>(code));
        }
        previous = static_cast<int32_t>(code);
        used += step;
    }
}

} // namespace oa::boneyards
