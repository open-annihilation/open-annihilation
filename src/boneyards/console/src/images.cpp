// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// GAF-backed image store used by the console screens.
#include "oa/boneyards/console/images.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "oa/boneyards/console/menu_overlay_art.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/formats/gaf.hpp"

namespace oa::boneyards::console {

namespace {

struct Entry {
    bool live = false;
    int32_t refs = 0;
    int32_t delay = 0; // animation ticks
    Sprite sprite{};
    std::vector<uint8_t> pixels;
};

struct CachedArchive {
    std::string name; // lower-case resource path
    bool loaded = false;
    // The file's own bytes. Frames are drawn from them, so the archive does
    // not also keep every frame's pixels.
    std::vector<uint8_t> bytes;
    formats::gaf::Archive archive;
};

bool equal_ignore_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

struct ConsoleImages {
    FileReader reader{};
    Surface* target = nullptr;
    std::vector<std::unique_ptr<Entry>> entries;
    std::vector<std::unique_ptr<CachedArchive>> archives;
};

namespace {

// The embedded strip graphic. Its bytes stay where the program keeps them,
// and frames are drawn from those bytes.
struct EmbeddedMenu {
    formats::gaf::Archive archive;
    const uint8_t* bytes = nullptr;
    std::size_t size = 0;
    bool tried = false;
};

EmbeddedMenu& embedded_slot() {
    static EmbeddedMenu menu;
    return menu;
}

EmbeddedMenu& embedded_menu() {
    EmbeddedMenu& menu = embedded_slot();
    if (menu.tried)
        return menu;
    menu.tried = true;
    const std::span<const uint8_t> art = menu_overlay_art();
    formats::gaf::ParseResult parsed = formats::gaf::parse(art, formats::gaf::PixelData::checked);
    if (!parsed.ok())
        return menu;
    menu.archive = std::move(*parsed.archive);
    menu.bytes = art.data();
    menu.size = art.size();
    return menu;
}

/// The embedded main-menu graphic, parsed once.
///
/// It is not stored in the archive cache. A later mount of the expansion's
/// archive is then read when a sequence this graphic does not hold is asked
/// for, and the rest of that archive stays available.
///
/// @return the archive, or null when the embedded bytes do not parse
const formats::gaf::Archive* embedded_menu_archive() {
    EmbeddedMenu& menu = embedded_menu();
    if (menu.bytes == nullptr || menu.archive.sequences.empty())
        return nullptr;
    return &menu.archive;
}

const formats::gaf::Sequence*
find_sequence(const formats::gaf::Archive* archive, const char* name) {
    if (archive == nullptr || name == nullptr)
        return nullptr;
    for (const formats::gaf::Sequence& candidate : archive->sequences) {
        if (equal_ignore_case(candidate.name, name))
            return &candidate;
    }
    return nullptr;
}

/// Reads an archive's bytes: the store's reader first, then the files the
/// client session reads.
///
/// The new consoles' art (anims/BY_New.gaf) travels in coreprime.ccx with
/// their layouts. A profile that does not mount that archive leaves it out of
/// the game's files, and the client session's reader still reaches it, as it
/// does the layouts. The main console graphic is read from the game's files
/// alone: when they do not hold it, the embedded copy is drawn instead.
///
/// @param images the store
/// @param path lower-case path under anims/
/// @param[out] bytes the archive's bytes
/// @return true when either reader found the file
bool read_archive_bytes(
    const ConsoleImages* images, const std::string& path, std::vector<uint8_t>* bytes
) {
    if (images->reader.read != nullptr &&
        images->reader.read(images->reader.user, path.c_str(), bytes))
        return true;
    if (path == "anims/main_console.gaf")
        return false;
    return rover_session_read_file(path.c_str(), bytes);
}

const formats::gaf::Archive* find_archive(ConsoleImages* images, const char* gaf_name) {
    const std::string path = lower(std::string(k_console_gaf_directory) + gaf_name);
    for (const auto& cached : images->archives) {
        if (cached->name == path)
            return cached->loaded ? &cached->archive : nullptr;
    }
    auto cached = std::make_unique<CachedArchive>();
    cached->name = path;
    std::vector<uint8_t> bytes;
    if (read_archive_bytes(images, path, &bytes)) {
        formats::gaf::ParseResult parsed =
            formats::gaf::parse(bytes, formats::gaf::PixelData::checked);
        if (parsed.ok()) {
            cached->bytes = std::move(bytes);
            cached->archive = std::move(*parsed.archive);
            cached->loaded = true;
        }
    }
    // A miss of the main console graphic is not cached. Caching it would
    // hide the expansion's archive after it is mounted, and the main frame
    // would then miss every sequence the embedded graphic does not hold.
    if (!cached->loaded) {
        if (path == "anims/main_console.gaf")
            return embedded_menu_archive();
        images->archives.push_back(std::move(cached));
        return nullptr;
    }
    images->archives.push_back(std::move(cached));
    return &images->archives.back()->archive;
}

/// The archive a named sequence is drawn from.
///
/// The strip's sequences are the embedded graphic, the same pixels as the
/// expansion's copies, so naming one does not read the rest of that archive.
/// An empty name, or a sequence the graphic does not hold, uses the file.
///
/// @param images the store
/// @param gaf_name archive name under anims/
/// @param sequence sequence name; empty selects the file
/// @return the archive, or null when it cannot be read
const formats::gaf::Archive*
archive_for(ConsoleImages* images, const char* gaf_name, const char* sequence) {
    const std::string path = lower(std::string(k_console_gaf_directory) + gaf_name);
    if (path == "anims/main_console.gaf" && sequence != nullptr && sequence[0] != '\0') {
        const formats::gaf::Archive* embedded = embedded_menu_archive();
        if (find_sequence(embedded, sequence) != nullptr)
            return embedded;
    }
    return find_archive(images, gaf_name);
}

struct SourceBytes {
    const uint8_t* data = nullptr;
    std::size_t size = 0;
};

bool read_source(void* context, uint32_t offset, std::span<uint8_t> output) {
    const auto* source = static_cast<const SourceBytes*>(context);
    if (source == nullptr || source->data == nullptr)
        return false;
    const uint64_t end = static_cast<uint64_t>(offset) + static_cast<uint64_t>(output.size());
    if (end > source->size)
        return false;
    if (!output.empty())
        std::memcpy(output.data(), source->data + offset, output.size());
    return true;
}

SourceBytes bytes_of(const ConsoleImages* images, const formats::gaf::Archive* archive) {
    const EmbeddedMenu& menu = embedded_menu();
    if (archive == &menu.archive)
        return SourceBytes{menu.bytes, menu.size};
    for (const auto& cached : images->archives) {
        if (&cached->archive == archive)
            return SourceBytes{cached->bytes.data(), cached->bytes.size()};
    }
    return {};
}

formats::gaf::RenderResult render_frame(
    const ConsoleImages* images,
    const formats::gaf::Archive* archive,
    const formats::gaf::Frame& frame
) {
    SourceBytes source = bytes_of(images, archive);
    formats::gaf::ReadHooks hooks;
    hooks.context = &source;
    hooks.read = read_source;
    return formats::gaf::render_ranged(frame, hooks);
}

Handle allocate(ConsoleImages* images) {
    for (std::size_t i = 0; i < images->entries.size(); ++i) {
        if (!images->entries[i]->live) {
            images->entries[i]->live = true;
            images->entries[i]->refs = 1;
            return static_cast<Handle>(i);
        }
    }
    images->entries.push_back(std::make_unique<Entry>());
    images->entries.back()->live = true;
    images->entries.back()->refs = 1;
    return static_cast<Handle>(images->entries.size() - 1);
}

/// Returns the delay every loaded image starts with.
///
/// @return 0 animation ticks
int32_t image_default_delay() {
    return 0;
}

Entry* entry_of(const ConsoleImages* images, Handle image) {
    if (image < 0 || static_cast<std::size_t>(image) >= images->entries.size())
        return nullptr;
    Entry* entry = images->entries[static_cast<std::size_t>(image)].get();
    return entry->live ? entry : nullptr;
}

/// Takes a free image entry for a new picture drawn with a transparent index.
///
/// @param[in,out] images the store
/// @param key transparent palette index
/// @return the new image with no pixels yet
Handle insert_image(ConsoleImages* images, uint8_t key) {
    const Handle handle = allocate(images);
    Entry* entry = entry_of(images, handle);
    entry->delay = 0;
    entry->sprite.key = key;
    return handle;
}

void bind_pixels(Entry* entry) {
    entry->sprite.data = entry->pixels.empty() ? nullptr : entry->pixels.data();
    entry->sprite.encoding = OA_SPRITE_RAW;
    entry->sprite.child_count = 0;
    entry->sprite.aux = nullptr;
}

void service_release(void* self, Handle image) {
    auto* images = static_cast<ConsoleImages*>(self);
    Entry* entry = entry_of(images, image);
    if (entry == nullptr)
        return;
    if (--entry->refs <= 0) {
        entry->live = false;
        entry->pixels.clear();
        entry->sprite = Sprite{};
    }
}

void service_add_ref(void* self, Handle image) {
    Entry* entry = entry_of(static_cast<ConsoleImages*>(self), image);
    if (entry != nullptr)
        ++entry->refs;
}

int32_t service_frame_count(void* self, const char* file, const char* sequence, int32_t index) {
    if (file == nullptr || sequence == nullptr)
        return 0;
    const formats::gaf::Archive* archive =
        sequence[0] == '\0' ? find_archive(static_cast<ConsoleImages*>(self), file)
                            : archive_for(static_cast<ConsoleImages*>(self), file, sequence);
    if (archive == nullptr)
        return 0;
    if (sequence[0] == '\0') {
        if (index < 0 || static_cast<std::size_t>(index) >= archive->sequences.size())
            return 0;
        return static_cast<int32_t>(
            archive->sequences[static_cast<std::size_t>(index)].frames.size()
        );
    }
    for (const formats::gaf::Sequence& candidate : archive->sequences) {
        if (equal_ignore_case(candidate.name, sequence))
            return static_cast<int32_t>(candidate.frames.size());
    }
    return 0;
}

Handle
service_load(void* self, const char* file, const char* sequence, int32_t frame, int32_t key) {
    return console_images_load(
        static_cast<ConsoleImages*>(self), file, sequence, frame, static_cast<uint8_t>(key)
    );
}

void service_size(void* self, Handle image, int32_t* width, int32_t* height) {
    const Entry* entry = entry_of(static_cast<ConsoleImages*>(self), image);
    *width = entry != nullptr ? entry->sprite.width : 0;
    *height = entry != nullptr ? entry->sprite.height : 0;
}

/// Makes an image of one colour with a transparent index (the ImageService create_blank slot).
///
/// A size with no pixels fails, as in 3.1c.
///
/// @param self the ConsoleImages store
/// @param width image width in pixels, 1..65535
/// @param height image height in pixels, 1..65535
/// @param fill palette index every pixel starts with
/// @param key transparent palette index
/// @return the image, or k_no_handle for a size outside 1..65535
Handle service_create_blank(void* self, int32_t width, int32_t height, uint8_t fill, int32_t key) {
    if (width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX)
        return k_no_handle;
    auto* images = static_cast<ConsoleImages*>(self);
    const Handle handle = insert_image(images, static_cast<uint8_t>(key));
    Entry* entry = entry_of(images, handle);
    entry->pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), fill);
    entry->sprite.width = static_cast<uint16_t>(width);
    entry->sprite.height = static_cast<uint16_t>(height);
    entry->sprite.origin_x = 0;
    entry->sprite.origin_y = 0;
    bind_pixels(entry);
    return handle;
}

Sprite* service_sprite(void* self, Handle image) {
    return console_images_sprite(static_cast<ConsoleImages*>(self), image);
}

/// Returns an image's animation delay (the ImageService frame_delay slot).
///
/// The native store has no 0x400-entry limit.
///
/// @param self the ConsoleImages store
/// @param image the image
/// @return the delay, or 0 for a free or out-of-range handle
int32_t service_frame_delay(void* self, Handle image) {
    const Entry* entry = entry_of(static_cast<ConsoleImages*>(self), image);
    return entry != nullptr ? entry->delay : 0;
}

void service_draw(void* self, int32_t x, int32_t y, int32_t, int32_t, Handle image) {
    auto* images = static_cast<ConsoleImages*>(self);
    const Entry* entry = entry_of(images, image);
    if (entry == nullptr || images->target == nullptr)
        return;
    blit_sprite(images->target, entry->sprite, x, y);
}

} // namespace

void console_images_release_embedded_menu() {
    embedded_slot() = EmbeddedMenu{};
}

ConsoleImages* console_images_create(FileReader reader) {
    auto* images = new ConsoleImages;
    images->reader = reader;
    return images;
}

void console_images_destroy(ConsoleImages* images) {
    delete images;
}

Handle console_images_load(
    ConsoleImages* images, const char* gaf_name, const char* sequence, int32_t frame, uint8_t key
) {
    if (images == nullptr || gaf_name == nullptr || sequence == nullptr)
        return k_no_handle;
    const formats::gaf::Archive* archive = archive_for(images, gaf_name, sequence);
    if (archive == nullptr)
        return k_no_handle;
    const formats::gaf::Sequence* found = find_sequence(archive, sequence);
    const formats::gaf::Frame* source = formats::gaf::frame_at(found, frame);
    if (source == nullptr)
        return k_no_handle;
    formats::gaf::RenderResult rendered = render_frame(images, archive, *source);
    if (!rendered.ok())
        return k_no_handle;
    const Handle handle = allocate(images);
    Entry* entry = entry_of(images, handle);
    entry->pixels = std::move(rendered.frame->pixels);
    entry->sprite.width = rendered.frame->width;
    entry->sprite.height = rendered.frame->height;
    entry->sprite.origin_x = rendered.frame->origin_x;
    entry->sprite.origin_y = rendered.frame->origin_y;
    entry->sprite.key = key;
    entry->delay = image_default_delay();
    bind_pixels(entry);
    return handle;
}

Handle console_images_clone(ConsoleImages* images, Handle image, uint8_t key) {
    if (images == nullptr || entry_of(images, image) == nullptr)
        return k_no_handle;
    const Handle handle = insert_image(images, key);
    const Entry* source = entry_of(images, image);
    Entry* copy = entry_of(images, handle);
    copy->pixels = source->pixels;
    copy->sprite = source->sprite;
    copy->sprite.key = key;
    bind_pixels(copy);
    return handle;
}

Sprite* console_images_sprite(ConsoleImages* images, Handle image) {
    Entry* entry = images != nullptr ? entry_of(images, image) : nullptr;
    return entry != nullptr ? &entry->sprite : nullptr;
}

int32_t console_images_live_count(const ConsoleImages* images) {
    int32_t count = 0;
    for (const auto& entry : images->entries)
        count += entry->live ? 1 : 0;
    return count;
}

void console_images_set_target(ConsoleImages* images, Surface* target) {
    images->target = target;
}

Surface* console_images_target(const ConsoleImages* images) {
    return images != nullptr ? images->target : nullptr;
}

ImageService console_images_service(ConsoleImages* images) {
    ImageService service{};
    service.self = images;
    service.release = service_release;
    service.add_ref = service_add_ref;
    service.frame_count = service_frame_count;
    service.load = service_load;
    service.size = service_size;
    service.draw = service_draw;
    service.create_blank = service_create_blank;
    service.sprite = service_sprite;
    service.frame_delay = service_frame_delay;
    return service;
}

bool console_images_shade(
    ConsoleImages* images, Handle image, int32_t level, const ShadeTables& tables
) {
    Entry* entry = images != nullptr ? entry_of(images, image) : nullptr;
    if (entry == nullptr)
        return false;
    const uint8_t* row = nullptr;
    if (level < 0) {
        if (tables.shade == nullptr)
            return false;
        const int32_t clamped = std::max(level, -static_cast<int32_t>(k_shade_levels));
        row = tables.shade +
              static_cast<std::size_t>(clamped + static_cast<int32_t>(k_shade_levels)) * 256;
    } else {
        if (tables.light == nullptr)
            return false;
        const int32_t clamped = std::min(level, static_cast<int32_t>(k_shade_levels) - 1);
        row = tables.light + static_cast<std::size_t>(clamped) * 256;
    }
    for (uint8_t& pixel : entry->pixels)
        pixel = row[pixel];
    return true;
}

void blit_sprite(Surface* target, const Sprite& sprite, int32_t x, int32_t y) {
    if (target == nullptr || target->pixels == nullptr || sprite.data == nullptr ||
        sprite.encoding != OA_SPRITE_RAW)
        return;
    const int32_t left_edge = x - sprite.origin_x;
    const int32_t top_edge = y - sprite.origin_y;
    const auto* source = static_cast<const uint8_t*>(sprite.data);
    const int32_t left = std::max({left_edge, target->clip.x1, 0});
    const int32_t top = std::max({top_edge, target->clip.y1, 0});
    const int32_t right =
        std::min({left_edge + sprite.width - 1, target->clip.x2, target->width - 1});
    const int32_t bottom =
        std::min({top_edge + sprite.height - 1, target->clip.y2, target->height - 1});
    for (int32_t row = top; row <= bottom; ++row) {
        const uint8_t* in = source + static_cast<std::size_t>(row - top_edge) * sprite.width;
        uint8_t* out = target->pixels + static_cast<std::ptrdiff_t>(row) * target->pitch;
        for (int32_t column = left; column <= right; ++column) {
            const uint8_t value = in[column - left_edge];
            if (value != sprite.key)
                out[column] = value;
        }
    }
}

} // namespace oa::boneyards::console
