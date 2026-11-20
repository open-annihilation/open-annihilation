// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Texture binding, primitive order and display tables for 3DO drawing.
#include "oa/present/model/model_library.hpp"

#include "oa/formats/hpi.hpp"
#include "oa/present/colour_vision/colour_vision.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/present/span_sample.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <stdexcept>

namespace oa::present::model {
namespace {

constexpr std::size_t sampler_window = 0x10000;
constexpr uint16_t team_frame_count = 10;
constexpr std::string_view team_archive_name = "textures/logos.gaf";

std::string folded(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

bool readable_frame(const formats::gaf::Frame& frame) noexcept {
    return !frame.compressed && frame.layers.empty() && frame.width != 0 && frame.height != 0 &&
           frame.pixels.size() >= static_cast<std::size_t>(frame.width) * frame.height;
}

/// Returns the mean vertex Y of a primitive, the load-time sort key.
///
/// No corners give 0, and a corner past the vertices adds nothing.
///
/// @param object object holding the vertices
/// @param primitive primitive whose corners are averaged
/// @return the truncated mean of the corners' Y
int32_t mean_vertex_y(
    const formats::objects3d::Object& object, const formats::objects3d::Primitive& primitive
) noexcept {
    const auto count = static_cast<int32_t>(primitive.vertex_indices.size());
    if (count == 0)
        return 0;
    uint32_t sum = 0;
    for (const uint16_t index : primitive.vertex_indices)
        if (index < object.vertices.size())
            sum += static_cast<uint32_t>(object.vertices[index].y);
    return static_cast<int32_t>(sum) / count;
}

/// Returns whether step_texture_animations advances a primitive's cursor.
///
/// @param primitive prepared primitive
/// @return true for an animated texture that is not a team texture
bool steps_cursor(const PreparedPrimitive& primitive) noexcept {
    return (primitive.flags & primitive_animated) != 0 && (primitive.flags & primitive_team) == 0;
}

/// Erases the entries at the given model addresses, with the cursors they registered.
///
/// The cursors leave `cursors` before their entries are freed, and the rest
/// keep their registration order.
///
/// @param[in,out] library library whose entries and cursors are pruned
/// @param keys addresses of entries in library.models
void erase_prepared_models(
    ModelLibrary& library, std::span<const formats::objects3d::Model* const> keys
) {
    std::vector<const sim::sprite_animation::Cursor*> dropped;
    for (const auto* key : keys)
        for (const auto& object : library.models.at(key)->objects)
            for (const auto& primitive : object.primitives)
                if (steps_cursor(primitive))
                    dropped.push_back(&primitive.cursor);
    if (!dropped.empty()) {
        // Cursors of separate entries are compared through std::less, which
        // orders every pointer.
        const std::less<const sim::sprite_animation::Cursor*> before;
        std::sort(dropped.begin(), dropped.end(), before);
        std::erase_if(library.cursors, [&](const sim::sprite_animation::Cursor* cursor) {
            return std::binary_search(dropped.begin(), dropped.end(), cursor, before);
        });
    }
    for (const auto* key : keys)
        library.models.erase(key);
}

} // namespace

TextureLibrary load_texture_library(AssetStore& assets) {
    TextureLibrary library;
    const auto paths = assets.list_effective_in_mount_order("textures", ".gaf");
    std::vector<std::pair<const formats::gaf::Archive*, bool>> order;
    const auto load = [&](const std::string& path, bool team) {
        const auto data = assets.read(path);
        // An empty file wins its path and contributes nothing (oamod-standard
        // 4.1): a mod hides an archive's texture GAF with one.
        if (data.bytes.empty())
            return;
        auto parsed = formats::gaf::parse(data.bytes);
        if (!parsed.ok())
            throw std::runtime_error(
                "cannot parse texture archive '" + path + "': " + parsed.error->message
            );
        library.archives.push_back(
            std::make_unique<formats::gaf::Archive>(std::move(*parsed.archive))
        );
        order.emplace_back(library.archives.back().get(), team);
    };
    for (const auto& path : paths)
        if (folded(path) != team_archive_name)
            load(path, false);
    for (const auto& path : paths)
        if (folded(path) == team_archive_name)
            load(path, true);
    std::size_t bytes = 0;
    for (const auto& archive : library.archives)
        for (const auto& sequence : archive->sequences)
            for (const auto& frame : sequence.frames)
                if (readable_frame(frame))
                    bytes += static_cast<std::size_t>(frame.width) * frame.height;
    library.pixels.assign(bytes + sampler_window, 0);
    std::size_t cursor = 0;
    for (const auto& [archive, team] : order) {
        for (const auto& sequence : archive->sequences) {
            auto key = folded(sequence.name);
            if (library.sequences.contains(key))
                continue;
            TextureSequence entry;
            entry.sequence = &sequence;
            entry.team_archive = team && sequence.frames.size() == team_frame_count;
            for (const auto& frame : sequence.frames) {
                Sprite sprite{};
                if (readable_frame(frame)) {
                    const std::size_t size = static_cast<std::size_t>(frame.width) * frame.height;
                    std::memcpy(library.pixels.data() + cursor, frame.pixels.data(), size);
                    sprite.width = frame.width;
                    sprite.height = frame.height;
                    sprite.origin_x = frame.origin_x;
                    sprite.origin_y = frame.origin_y;
                    sprite.key = frame.transparency_index;
                    sprite.encoding = OA_SPRITE_RAW;
                    sprite.data = library.pixels.data() + cursor;
                    cursor += size;
                }
                entry.frames.push_back(sprite);
            }
            library.sequences.emplace(std::move(key), std::move(entry));
        }
    }
    return library;
}

void recolour_team_textures(TextureLibrary& library, std::span<const uint8_t> palette) {
    namespace vision = oa::present::colour_vision;
    const bool paint = vision::active_mode() != vision::Mode::off &&
                       palette.size() >= static_cast<std::size_t>(vision::kPaletteColours) * 4U;
    // Off, and never recoloured: the arena is already the loaded indices.
    if (!paint && !library.team_colours_applied)
        return;
    const uint8_t* const arena = library.pixels.data();
    const std::size_t arena_bytes = library.pixels.size();
    std::vector<vision::RawSequence> reading;

    struct ArenaFrame {
        uint8_t* pixels{};
        std::size_t count{};
        int key{-1};
        bool ready{};
    };

    std::vector<std::array<ArenaFrame, vision::kPlayerCount>> arenas;
    for (auto& [folded_name, sequence] : library.sequences) {
        (void)folded_name;
        if (!sequence.team_archive || sequence.sequence == nullptr)
            continue;
        const formats::gaf::Sequence& source = *sequence.sequence;
        if (source.frames.size() != sequence.frames.size() ||
            source.frames.size() != static_cast<std::size_t>(vision::kPlayerCount))
            continue;
        vision::RawSequence raw;
        std::array<ArenaFrame, vision::kPlayerCount> copied{};
        bool any = false;
        for (int player = 0; player < vision::kPlayerCount; ++player) {
            const formats::gaf::Frame& frame = source.frames[static_cast<std::size_t>(player)];
            Sprite& sprite = sequence.frames[static_cast<std::size_t>(player)];
            if (!readable_frame(frame) || sprite.data == nullptr || arena == nullptr)
                continue;
            const auto count = static_cast<std::size_t>(frame.width) * frame.height;
            auto* data = static_cast<uint8_t*>(sprite.data);
            const auto begin = reinterpret_cast<uintptr_t>(arena);
            const auto at = reinterpret_cast<uintptr_t>(data);
            if (at < begin || at - begin > arena_bytes || arena_bytes - (at - begin) < count)
                continue;
            std::memcpy(data, frame.pixels.data(), count);
            raw.frames[static_cast<std::size_t>(player)] = {
                frame.pixels.data(), count, static_cast<int>(frame.transparency_index)
            };
            copied[static_cast<std::size_t>(player)] = {
                data, count, frame.transparency_index, true
            };
            any = true;
        }
        if (!any)
            continue;
        reading.push_back(raw);
        arenas.push_back(copied);
    }
    if (!paint) {
        library.team_colours_applied = false;
        return;
    }
    const vision::TeamMaps maps = vision::team_maps(vision::active_mode(), palette, reading);
    if (!reading.empty())
        vision::remember_team_maps(maps);
    for (const auto& frames : arenas) {
        for (int player = 0; player < vision::kPlayerCount; ++player) {
            const ArenaFrame& frame = frames[static_cast<std::size_t>(player)];
            if (!frame.ready)
                continue;
            vision::remap_indices(
                std::span<uint8_t>(frame.pixels, frame.count),
                maps[static_cast<std::size_t>(player)],
                frame.key
            );
        }
    }
    library.team_colours_applied = true;
}

void sort_object_primitives(
    const formats::objects3d::Object& object, std::vector<uint32_t>& order
) {
    const auto count = static_cast<uint32_t>(object.primitives.size());
    order.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        order[i] = i;
    const int32_t selection = object.selection_primitive;
    if (selection != -1 && count > 0 && selection >= 0 && static_cast<uint32_t>(selection) < count)
        std::swap(order[0], order[static_cast<uint32_t>(selection)]);
    if (count < 3)
        return;
    bool swapped = false;
    do {
        swapped = false;
        for (uint32_t i = 1; i + 1 < count; ++i) {
            const int32_t here = mean_vertex_y(object, object.primitives[order[i]]);
            const int32_t next = mean_vertex_y(object, object.primitives[order[i + 1]]);
            if (next < here) {
                std::swap(order[i], order[i + 1]);
                swapped = true;
            }
        }
    } while (swapped);
}

const PreparedModel& prepare_model(
    ModelLibrary& library, const std::shared_ptr<const formats::objects3d::Model>& handle
) {
    if (!handle)
        throw std::invalid_argument("prepare_model needs a model handle");
    const formats::objects3d::Model& model = *handle;
    if (const auto found = library.models.find(&model); found != library.models.end()) {
        // While the entry's owner lives, the object at this address is still
        // the model it was prepared for, whichever handle reached it. Only an
        // expired owner marks an entry a freed model left at this address.
        if (!found->second->owner.expired())
            return *found->second;
        const formats::objects3d::Model* const stale[] = {&model};
        erase_prepared_models(library, stale);
    }
    auto prepared = std::make_unique<PreparedModel>();
    prepared->model = &model;
    prepared->owner = handle;
    prepared->objects.resize(model.objects.size());
    std::vector<uint32_t> order;
    for (std::size_t oi = 0; oi < model.objects.size(); ++oi) {
        const auto& object = model.objects[oi];
        auto& out = prepared->objects[oi];
        out.skips_first = object.selection_primitive != -1 && !object.primitives.empty();
        sort_object_primitives(object, order);
        out.primitives.resize(order.size());
        for (std::size_t pi = 0; pi < order.size(); ++pi) {
            const auto& source = object.primitives[order[pi]];
            auto& primitive = out.primitives[pi];
            primitive.source_index = order[pi];
            primitive.flags = static_cast<uint32_t>(source.is_colored);
            primitive.color = static_cast<uint8_t>(source.color_index);
            if (source.texture_name.empty())
                continue;
            primitive.flags &= ~primitive_team;
            const auto match = library.textures.sequences.find(folded(source.texture_name));
            if (match == library.textures.sequences.end()) {
                primitive.flags |= primitive_colored;
                primitive.color = missing_texture_color;
                continue;
            }
            const TextureSequence& texture = match->second;
            if (texture.team_archive)
                primitive.flags |= primitive_team;
            if (texture.sequence->frames.size() < 2) {
                primitive.flags &= ~primitive_animated;
                primitive.frame = texture.frames.empty() ? nullptr : &texture.frames.front();
            } else {
                primitive.texture = &texture;
                sim::sprite_animation::initialize(primitive.cursor, texture.sequence, 0);
                primitive.flags |= primitive_animated;
            }
        }
    }
    auto& stored = *library.models.emplace(&model, std::move(prepared)).first->second;
    for (auto& object : stored.objects)
        for (auto& primitive : object.primitives)
            if (steps_cursor(primitive))
                library.cursors.push_back(&primitive.cursor);
    return stored;
}

const PreparedModel*
find_prepared_model(const ModelLibrary& library, const formats::objects3d::Model& model) noexcept {
    const auto found = library.models.find(&model);
    // A live owner at this address is the caller's model: two live models
    // cannot share an address.
    if (found == library.models.end() || found->second->owner.expired())
        return nullptr;
    return found->second.get();
}

std::size_t release_expired_models(ModelLibrary& library) {
    std::vector<const formats::objects3d::Model*> expired;
    for (const auto& [key, prepared] : library.models)
        if (prepared->owner.expired())
            expired.push_back(key);
    erase_prepared_models(library, expired);
    return expired.size();
}

const Sprite* primitive_texture(
    const PreparedPrimitive& primitive, bool first_frame, uint8_t team_color
) noexcept {
    const Sprite* sprite = nullptr;
    if ((primitive.flags & primitive_animated) == 0) {
        sprite = primitive.frame;
    } else {
        uint32_t index = 0;
        if ((primitive.flags & primitive_team) != 0) {
            index = team_color;
        } else if (!first_frame) {
            if (primitive.cursor.sequence == nullptr)
                return nullptr;
            index = primitive.cursor.frame_index;
        }
        if (primitive.texture == nullptr || index >= primitive.texture->frames.size())
            return nullptr;
        sprite = &primitive.texture->frames[index];
    }
    return sprite != nullptr && sprite->data != nullptr ? sprite : nullptr;
}

void step_texture_animations(ModelLibrary& library) noexcept {
    present::step_all_cursors(
        reinterpret_cast<void* const*>(library.cursors.data()),
        static_cast<int32_t>(library.cursors.size()),
        [](void* cursor) -> int32_t {
            return sim::sprite_animation::tick(*static_cast<sim::sprite_animation::Cursor*>(cursor))
                       ? 1
                       : 0;
        }
    );
}

void build_model_display(ModelDisplay& display, const Palette& palette) {
    display.palette = palette;
    display.alpha.assign(present::alpha_table_size, 0);
    display.shade.assign(present::shade_table_size, 0);
    display.blue.assign(present::blue_table_size, 0);
    display.context.palette = palette;
    display.context.alpha_table = display.alpha.data();
    display.context.shade_table = display.shade.data();
    display.context.blue_table = display.blue.data();
    // The flags select the tables to build; the vectors own them.
    display.context.flags = present::display_flag_alpha_table | present::display_flag_shade_table |
                            present::display_flag_blue_table;
    present::build_alpha_table(display.context, palette);
    present::build_shade_table(display.context, palette);
    present::build_blue_table(display.context, palette);
}

DisplayScope enter_display(ModelDisplay* display) noexcept {
    DisplayScope scope{};
    scope.previous = present::display_context();
    const auto* current = scope.previous;
    const bool complete = current != nullptr && current->alpha_table != nullptr &&
                          current->shade_table != nullptr && current->blue_table != nullptr &&
                          (current->flags & present::display_flag_alpha_table) != 0;
    if (!complete && display != nullptr) {
        present::bind_display(&display->context);
        scope.bound = true;
    }
    return scope;
}

void leave_display(const DisplayScope& scope) noexcept {
    if (scope.bound)
        present::bind_display(scope.previous);
}

} // namespace oa::present::model
