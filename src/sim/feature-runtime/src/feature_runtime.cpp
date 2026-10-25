// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/feature_runtime.hpp"
#include "oa/base/bytes.hpp"

#include <cstring>
#include <initializer_list>

namespace oa::sim::feature_runtime {
namespace {

constexpr uint32_t percent = 100;
constexpr int32_t fire_spread_radius = 3;
constexpr int32_t fire_trail_steps = 5;
constexpr int32_t cell_to_world_shift = 20;
constexpr int32_t lcg_rand_range = 0x8000;
constexpr int32_t plot_world_units = 16;
constexpr const char* treeburn_sound = "treeburn";

/// Reads a 32-bit little-endian two's-complement value.
///
/// @param bytes the first of four bytes
/// @return the value
int32_t load_i32(const uint8_t* bytes) noexcept {
    return static_cast<int32_t>(base::bytes::load_le32(bytes));
}

oa_fixed wrap_add(oa_fixed left, oa_fixed right) noexcept {
    return static_cast<oa_fixed>(static_cast<uint32_t>(left) + static_cast<uint32_t>(right));
}

/// Returns the unit standing on the plot's ground layer, 0 when there is none.
uint16_t plot_ground_unit(const MapPlot& plot) noexcept {
    return plot.ground_unit;
}

/// Returns the highest map height over the plot and its neighbours in x and z.
uint8_t plot_high_height(const MapPlot& plot) noexcept {
    return plot.high_height;
}

/// Returns the lowest map height over the same cells as plot_high_height().
uint8_t plot_low_height(const MapPlot& plot) noexcept {
    return plot.low_height;
}

// Continuation cells keep the z offset in the low byte and x in the high byte.
uint8_t back_z(const MapPlot& plot) noexcept {
    return static_cast<uint8_t>(plot.feature_record & 0xffu);
}

uint8_t back_x(const MapPlot& plot) noexcept {
    return static_cast<uint8_t>(plot.feature_record >> 8);
}

/// Returns the feature type a burnt-out feature leaves.
///
/// @param def feature type that burnt out
/// @return its featureburnt index, 0xffff for none
uint16_t burnt_feature(const FeatureDef& def) noexcept {
    return def.burnt_feature;
}

/// Returns the feature type a reclaimed feature leaves.
///
/// @param def feature type being reclaimed
/// @return its featurereclamate index, 0xffff for none
uint16_t reclamate_feature(const FeatureDef& def) noexcept {
    return def.reclamate_feature;
}

bool is_sprite(const FeatureDef& def) noexcept {
    return (def.flags & OA_FEATURE_FLAG_SPRITE) != 0;
}

int32_t map_width(const World& world) noexcept {
    return world.game.map_width;
}

std::size_t plot_count(const World& world) noexcept {
    if (world.plots == nullptr || world.game.map_width <= 0 || world.game.map_height <= 0)
        return 0;
    return static_cast<std::size_t>(world.game.map_width) *
           static_cast<std::size_t>(world.game.map_height);
}

MapPlot* plot_at(World& world, int32_t x, int32_t z) noexcept {
    return world_plot(&world, x, z);
}

const MapPlot* plot_at(const World& world, int32_t x, int32_t z) noexcept {
    return world_plot(const_cast<World*>(&world), x, z);
}

std::size_t plot_index(const World& world, const MapPlot* plot) noexcept {
    return static_cast<std::size_t>(plot - world.plots);
}

// FeatureDef by table index, or null past the table.
const FeatureDef* feature_def(const World& world, uint16_t index) noexcept {
    if (world.feature_defs == nullptr || index >= world.feature_def_count)
        return nullptr;
    return &world.feature_defs[index];
}

uint32_t random(const FeatureHost& host, uint32_t limit) noexcept {
    return host.random != nullptr ? host.random(host.context, limit) : 0u;
}

bool lookup_frame(
    const FeatureHost& host, oa_ref32 sequence, uint16_t frame, FeatureSequenceFrame& out
) noexcept {
    out = {};
    return sequence != 0 && host.sequence_frame != nullptr &&
           host.sequence_frame(host.context, sequence, frame, &out);
}

/// Returns how many ticks the cursor's frame stays up.
///
/// @param host feature host resolving the frame
/// @param cursor cursor to read
/// @return the frame duration, 0 when the frame is unknown, 0xffff with no sequence
uint16_t cursor_duration(const FeatureHost& host, const FeatureCursor& cursor) noexcept {
    if (cursor.sequence == 0)
        return 0xffff;
    FeatureSequenceFrame frame;
    return lookup_frame(host, cursor.sequence, cursor.frame, frame) ? frame.duration : 0;
}

/// Starts a cursor on a sequence at a frame.
///
/// @param host feature host resolving the sequence
/// @param[out] cursor cursor to start
/// @param sequence sequence ref
/// @param frame starting frame; frame 0 when out of range
void start_cursor(
    const FeatureHost& host, FeatureCursor& cursor, oa_ref32 sequence, int32_t frame
) noexcept {
    FeatureSequenceFrame info;
    lookup_frame(host, sequence, 0, info);
    cursor.frame =
        frame < static_cast<int32_t>(info.frame_count) ? static_cast<uint16_t>(frame) : 0;
    cursor.sequence = sequence;
    cursor.remaining = cursor_duration(host, cursor);
    cursor.repeat = info.repeat;
}

/// Advances a cursor one tick.
///
/// A finished one-shot sequence clears its reference; a repeating one wraps.
///
/// @param host feature host resolving the frames
/// @param[in,out] cursor cursor to advance
/// @return true when the frame changed or the sequence finished
bool step_cursor(const FeatureHost& host, FeatureCursor& cursor) noexcept {
    if (cursor.sequence == 0)
        return false;
    if (cursor.remaining >= 2) {
        --cursor.remaining;
        return false;
    }
    ++cursor.frame;
    FeatureSequenceFrame info;
    lookup_frame(host, cursor.sequence, cursor.frame, info);
    if (info.frame_count <= cursor.frame) {
        if (cursor.repeat == 0) {
            cursor.sequence = 0;
            return true;
        }
        cursor.frame = 0;
    }
    cursor.remaining = cursor_duration(host, cursor);
    return true;
}

// A placed feature's cursor and a feature type's share one layout, field for field.
static_assert(sizeof(FeatureCursor) == sizeof(FeatureDefCursor));
static_assert(offsetof(FeatureCursor, frame) == offsetof(FeatureDefCursor, frame));
static_assert(offsetof(FeatureCursor, remaining) == offsetof(FeatureDefCursor, remaining));
static_assert(offsetof(FeatureCursor, repeat) == offsetof(FeatureDefCursor, repeat));
static_assert(offsetof(FeatureCursor, sequence) == offsetof(FeatureDefCursor, sequence));

/// Steps one of a feature definition's sequence cursors by a tick.
///
/// @param host frame lookups for the cursor's sequence
/// @param[in,out] def_cursor FeatureDef.animation_cursor or FeatureDef.shadow_cursor
void step_def_cursor(const FeatureHost& host, FeatureDefCursor& def_cursor) noexcept {
    FeatureCursor cursor;
    std::memcpy(static_cast<void*>(&cursor), &def_cursor, sizeof(cursor));
    step_cursor(host, cursor);
    std::memcpy(&def_cursor, &cursor, sizeof(cursor));
}

void notify_footprint(
    const FeatureHost& host, const World& world, const MapPlot* plot, const FeatureDef& def
) noexcept {
    if (host.footprint_changed == nullptr)
        return;
    const auto index = static_cast<int32_t>(plot_index(world, plot));
    const auto width = map_width(world);
    host.footprint_changed(
        host.context,
        static_cast<int16_t>(index % width),
        static_cast<int16_t>(index / width),
        def.footprint_x,
        def.footprint_z
    );
}

void feature_changed(
    const FeatureHost& host, FeatureChange change, int32_t x, int32_t z, const Unit* reclaimer
) noexcept {
    if (host.feature_changed != nullptr)
        host.feature_changed(host.context, change, x, z, reclaimer);
}

// Initialises a sprite record's sequence and optional shadow sequence.
void start_sprite_sequences(
    const FeatureHost& host, PlacedFeature& record, oa_ref32 sequence, oa_ref32 shadow
) noexcept {
    start_cursor(host, record.sprite.animation, sequence, 0);
    if (shadow == 0) {
        record.state = static_cast<uint8_t>(record.state & ~state_has_shadow);
    } else {
        start_cursor(host, record.sprite.shadow, shadow, 0);
        record.state = static_cast<uint8_t>(record.state | state_has_shadow);
    }
}

/// Returns the FeatureDef a plot names, following a continuation cell once.
///
/// @param world world holding the plots and feature table
/// @param plot plot to read; may be null
/// @return the definition, or null for no plot, a reserved word or an index past the table
const FeatureDef* feature_on_plot(const World& world, const MapPlot* plot) noexcept {
    if (plot == nullptr)
        return nullptr;
    auto word = plot->feature;
    if (word == feature_continuation) {
        const auto back =
            static_cast<std::size_t>(back_z(*plot)) * static_cast<std::size_t>(map_width(world)) +
            back_x(*plot);
        const auto index = plot_index(world, plot);
        if (back > index)
            return nullptr;
        word = world.plots[index - back].feature;
    }
    if (word >= OA_PLOT_FEATURE_RESERVED)
        return nullptr;
    return feature_def(world, word);
}

} // namespace

PlacedFeature* feature_records(World& world) noexcept {
    return reinterpret_cast<PlacedFeature*>(world.placed_features);
}

PlacedFeature* feature_record(World& world, int32_t slot) noexcept {
    if (world.placed_features == nullptr || slot < 0 ||
        static_cast<uint32_t>(slot) >= world.placed_feature_count)
        return nullptr;
    return &feature_records(world)[slot];
}

const PlacedFeature* feature_record(const World& world, int32_t slot) noexcept {
    if (world.placed_features == nullptr || slot < 0 ||
        static_cast<uint32_t>(slot) >= world.placed_feature_count)
        return nullptr;
    return &reinterpret_cast<const PlacedFeature*>(world.placed_features)[slot];
}

int32_t feature_list_head(const World& world, FeatureList list) noexcept {
    switch (list) {
    case FeatureList::active:
        return world.game.feature_active_head;
    case FeatureList::settled:
        return world.game.feature_settled_head;
    case FeatureList::free_slots:
        break;
    }
    return world.game.feature_free_head;
}

void set_feature_list_head(World& world, FeatureList list, int32_t slot) noexcept {
    switch (list) {
    case FeatureList::active:
        world.game.feature_active_head = slot;
        return;
    case FeatureList::settled:
        world.game.feature_settled_head = slot;
        return;
    case FeatureList::free_slots:
        break;
    }
    world.game.feature_free_head = slot;
}

int32_t reproduce_cursor(const World& world) noexcept {
    return load_i32(world.game.reproduce_cursor);
}

void set_reproduce_cursor(World& world, int32_t index) noexcept {
    std::memcpy(world.game.reproduce_cursor, &index, sizeof(index));
}

uint8_t plot_height(const World& world, int16_t cell_x, int16_t cell_z) noexcept {
    const auto* plot = plot_at(world, cell_x, cell_z);
    return plot != nullptr ? plot->height : 0;
}

int32_t sample_height(const World& world, oa_fixed x, oa_fixed z) noexcept {
    const auto world_x = static_cast<int32_t>(static_cast<int16_t>(static_cast<uint32_t>(x) >> 16));
    const auto world_z = static_cast<int32_t>(static_cast<int16_t>(static_cast<uint32_t>(z) >> 16));
    const auto cell_x = world_x >> 4;
    const auto cell_z = world_z >> 4;
    const auto width = world.game.map_width;
    if (world.plots == nullptr || cell_x < 0 || cell_x + 1 >= width || cell_z < 0 ||
        cell_z + 1 >= world.game.map_height)
        return -1;
    const auto fraction_x = world_x & 0xf;
    const auto fraction_z = world_z & 0xf;
    const auto* top = &world.plots
                           [static_cast<std::size_t>(width) * static_cast<std::size_t>(cell_z) +
                            static_cast<std::size_t>(cell_x)];
    const auto* bottom = top + width;
    const int32_t upper = (top[1].height - top[0].height) * fraction_x / 16 + top[0].height;
    const int32_t lower =
        (bottom[1].height - bottom[0].height) * fraction_x / 16 + bottom[0].height;
    return (lower - upper) * fraction_z / 16 + upper;
}

int32_t mean_plot_height(const World& world, oa_fixed x, oa_fixed z) noexcept {
    const auto world_x = static_cast<int32_t>(static_cast<int16_t>(static_cast<uint32_t>(x) >> 16));
    const auto world_z = static_cast<int32_t>(static_cast<int16_t>(static_cast<uint32_t>(z) >> 16));
    const auto* plot = plot_at(world, world_x / 16, world_z / 16);
    if (plot == nullptr)
        return -1;
    return (static_cast<int32_t>(plot_low_height(*plot)) + plot_high_height(*plot)) >> 1;
}

FixedVec3
feature_center(const World& world, int16_t cell_x, int16_t cell_z, const FeatureDef& def) noexcept {
    const auto axis = [](int16_t footprint, int16_t cell) noexcept {
        const auto sum = static_cast<uint32_t>(static_cast<int32_t>(footprint) + cell * 2);
        return static_cast<oa_fixed>(sum << 19);
    };
    FixedVec3 position{};
    position.x = axis(def.footprint_x, cell_x);
    position.z = axis(def.footprint_z, cell_z);
    position.y = static_cast<oa_fixed>(
        static_cast<uint32_t>(sample_height(world, position.x, position.z)) << 16
    );
    return position;
}

oa_fixed feature_drawn_height(const World& world, int32_t cell_x, int32_t cell_z) noexcept {
    const MapPlot* origin = plot_at(world, cell_x, cell_z);
    if (origin == nullptr)
        return 0;
    if ((origin->flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0)
        if (const PlacedFeature* record = feature_record(world, origin->feature_record);
            record != nullptr) {
            const FeatureDef* def = feature_def(world, origin->feature);
            if (def != nullptr && !is_sprite(*def))
                return record->model.position.y;
        }
    return static_cast<oa_fixed>(static_cast<uint32_t>(origin->height) << 16);
}

bool init_feature_pool(World& world) noexcept {
    if (world.placed_features == nullptr ||
        world.placed_feature_count < static_cast<uint32_t>(slot_capacity))
        return false;
    std::memset(
        world.placed_features, 0, static_cast<std::size_t>(slot_capacity) * sizeof(PlacedFeature)
    );
    set_feature_list_head(world, FeatureList::active, no_slot);
    set_feature_list_head(world, FeatureList::settled, no_slot);
    set_feature_list_head(world, FeatureList::free_slots, 0);
    auto* records = feature_records(world);
    for (int32_t slot = 0; slot < slot_capacity; ++slot) {
        records[slot].next = static_cast<int16_t>(slot + 1);
        records[slot].prev = static_cast<int16_t>(slot - 1);
    }
    records[0].prev = no_slot;
    records[slot_capacity - 1].next = no_slot;
    return true;
}

void release_feature_objects(World& world, const FeatureHost& host) noexcept {
    for (const auto list : {FeatureList::settled, FeatureList::active}) {
        auto slot = feature_list_head(world, list);
        while (slot != no_slot) {
            auto* record = feature_record(world, slot);
            if (record == nullptr)
                break;
            const auto* def = feature_def(world, record->def_index);
            if (def != nullptr && !is_sprite(*def) && host.destroy_object != nullptr)
                host.destroy_object(host.context, record->model.object);
            slot = record->next;
        }
    }
}

int32_t alloc_feature_slot(World& world) noexcept {
    const auto slot = feature_list_head(world, FeatureList::free_slots);
    if (slot == no_slot)
        return slot_capacity;
    move_feature_slot(world, slot, FeatureList::active);
    auto* record = feature_record(world, slot);
    record->state = static_cast<uint8_t>(record->state & ~state_burning);
    return slot;
}

void move_feature_slot(World& world, int32_t slot, FeatureList to) noexcept {
    auto* record = feature_record(world, slot);
    if (record == nullptr)
        return;
    auto* records = feature_records(world);
    if (record->prev == no_slot) {
        for (const auto list :
             {FeatureList::active, FeatureList::settled, FeatureList::free_slots}) {
            if (feature_list_head(world, list) == slot) {
                set_feature_list_head(world, list, record->next);
                break;
            }
        }
    } else {
        records[record->prev].next = record->next;
    }
    if (record->next != no_slot)
        records[record->next].prev = record->prev;
    record->prev = no_slot;
    record->next = static_cast<int16_t>(feature_list_head(world, to));
    set_feature_list_head(world, to, slot);
    if (record->next != no_slot)
        records[record->next].prev = static_cast<int16_t>(slot);
}

bool clear_plot_feature(
    World& world, const FeatureHost& host, std::size_t index, bool force
) noexcept {
    const auto cells = plot_count(world);
    if (index >= cells)
        return false;
    const auto width = static_cast<std::size_t>(map_width(world));
    if (world.plots[index].feature == feature_continuation) {
        const auto back = static_cast<std::size_t>(back_z(world.plots[index])) * width +
                          back_x(world.plots[index]);
        if (back > index)
            return false;
        index -= back;
    }
    auto& origin = world.plots[index];
    if (origin.feature >= OA_PLOT_FEATURE_RESERVED)
        return false;
    const auto* def = feature_def(world, origin.feature);
    if (def == nullptr || (!force && (def->flags & OA_FEATURE_FLAG_INDESTRUCTIBLE) != 0))
        return false;
    if ((origin.flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0) {
        if (auto* record = feature_record(world, origin.feature_record); record != nullptr) {
            if (!is_sprite(*def)) {
                if (host.destroy_object != nullptr)
                    host.destroy_object(host.context, record->model.object);
                record->model.object = 0;
            }
            move_feature_slot(world, origin.feature_record, FeatureList::free_slots);
        }
    }
    origin.feature = no_feature;
    origin.flags = static_cast<uint8_t>(origin.flags & ~OA_PLOT_FLAG_ANIMATING_FEATURE);
    for (int32_t row = 0; row < def->footprint_z; ++row) {
        for (int32_t column = 0; column < def->footprint_x; ++column) {
            const auto cell =
                index + static_cast<std::size_t>(row) * width + static_cast<std::size_t>(column);
            if (cell >= cells || world.plots[cell].feature != feature_continuation)
                continue;
            world.plots[cell].feature = no_feature;
            world.plots[cell].flags =
                static_cast<uint8_t>(world.plots[cell].flags & ~OA_PLOT_FLAG_ANIMATING_FEATURE);
        }
    }
    notify_footprint(host, world, &origin, *def);
    return true;
}

PlacedFeature* place_feature(
    World& world,
    const FeatureHost& host,
    std::size_t index,
    uint16_t def_index,
    const FixedVec3* position,
    const int16_t* orientation,
    uint8_t player
) noexcept {
    const auto cells = plot_count(world);
    if (def_index == no_feature || index >= cells)
        return nullptr;
    auto& origin = world.plots[index];
    if (def_index == feature_marker) {
        origin.feature = feature_marker;
        return nullptr;
    }
    const auto* def = feature_def(world, def_index);
    if (def == nullptr)
        return nullptr;
    const auto width = map_width(world);
    const auto cell_x = static_cast<int16_t>(static_cast<int32_t>(index) % width);
    const auto cell_z = static_cast<int16_t>(static_cast<int32_t>(index) / width);
    if (cell_x + def->footprint_x > width || cell_z + def->footprint_z > world.game.map_height)
        return nullptr;
    for (int32_t row = 0; row < def->footprint_z; ++row) {
        for (int32_t column = 0; column < def->footprint_x; ++column) {
            const auto cell = index + static_cast<std::size_t>(row * width + column);
            if (world.plots[cell].feature != no_feature &&
                !clear_plot_feature(world, host, cell, false))
                return nullptr;
        }
    }
    PlacedFeature* record = nullptr;
    if (!is_sprite(*def)) {
        const auto slot = alloc_feature_slot(world);
        if (slot >= slot_capacity)
            return nullptr;
        record = feature_record(world, slot);
        record->damage = 0;
        record->def_index = def_index;
        record->cell_x = cell_x;
        record->cell_z = cell_z;
        record->model.position =
            position != nullptr ? *position : feature_center(world, cell_x, cell_z, *def);
        if (orientation == nullptr)
            std::memset(record->orientation, 0, sizeof(record->orientation));
        else
            std::memcpy(record->orientation, orientation, sizeof(record->orientation));
        record->model.object =
            host.create_object != nullptr ? host.create_object(host.context, def) : 0;
        origin.flags = static_cast<uint8_t>(origin.flags | OA_PLOT_FLAG_ANIMATING_FEATURE);
        origin.feature = def_index;
        origin.feature_record = static_cast<uint16_t>(slot);
    } else {
        origin.feature_record = 0;
        origin.flags = static_cast<uint8_t>(origin.flags & ~OA_PLOT_FLAG_ANIMATING_FEATURE);
        origin.feature = def_index;
    }
    origin.flags = static_cast<uint8_t>(
        ((player & 0xfu) << plot_player_shift) | (origin.flags & ~OA_PLOT_FLAG_PLAYER_FEATURE_MASK)
    );
    for (int32_t row = 0; row < def->footprint_z; ++row) {
        for (int32_t column = 0; column < def->footprint_x; ++column) {
            if (row == 0 && column == 0)
                continue;
            auto& cell = world.plots[index + static_cast<std::size_t>(row * width + column)];
            cell.flags = static_cast<uint8_t>(cell.flags & ~OA_PLOT_FLAG_ANIMATING_FEATURE);
            cell.feature = feature_continuation;
            cell.feature_record = static_cast<uint16_t>((column << 8) | (row & 0xff));
        }
    }
    if ((def->flags & OA_FEATURE_FLAG_GEOTHERMAL) != 0 && host.emit_feature_fx != nullptr) {
        const auto vent =
            position != nullptr ? *position : feature_center(world, cell_x, cell_z, *def);
        host.emit_feature_fx(host.context, &vent, geothermal_smoke_layer);
    }
    notify_footprint(host, world, &origin, *def);
    return record;
}

void clear_all_features(World& world, const FeatureHost& host) noexcept {
    const auto cells = plot_count(world);
    for (std::size_t index = 0; index < cells; ++index) {
        const auto word = world.plots[index].feature;
        if (word < OA_PLOT_FEATURE_RESERVED || word == feature_continuation)
            clear_plot_feature(world, host, index, true);
    }
}

void replace_with_remnant(
    World& world, const FeatureHost& host, int32_t cell_x, int32_t cell_z, bool reclaimed
) noexcept {
    auto* plot = plot_at(world, cell_x, cell_z);
    if (plot == nullptr || plot->feature >= OA_PLOT_FEATURE_RESERVED)
        return;
    const auto* def = feature_def(world, plot->feature);
    if (def == nullptr)
        return;
    auto next = reclaimed ? reclamate_feature(*def) : def->dead_feature;
    const auto index = plot_index(world, plot);
    if ((plot->flags & OA_PLOT_FLAG_ANIMATING_FEATURE) == 0) {
        clear_plot_feature(world, host, index, false);
        place_feature(world, host, index, next, nullptr, nullptr, no_player);
        return;
    }
    // The freed record keeps its bytes; its position and orientation seed the remnant.
    const auto* record = feature_record(world, plot->feature_record);
    if (record == nullptr)
        return;
    if ((record->state & state_reclaimed) != 0)
        next = reclamate_feature(*def);
    const auto position = record->model.position;
    int16_t orientation[3];
    std::memcpy(orientation, record->orientation, sizeof(orientation));
    clear_plot_feature(world, host, index, false);
    place_feature(world, host, index, next, &position, orientation, no_player);
}

void start_feature_sequence(
    World& world, const FeatureHost& host, int32_t cell_x, int32_t cell_z, bool reclaimed
) noexcept {
    auto* plot = plot_at(world, cell_x, cell_z);
    if (plot == nullptr)
        return;
    if (plot->feature == feature_continuation) {
        cell_x -= back_x(*plot);
        cell_z -= back_z(*plot);
        plot = plot_at(world, cell_x, cell_z);
        if (plot == nullptr)
            return;
    }
    if (plot->feature >= OA_PLOT_FEATURE_RESERVED)
        return;
    const auto* def = feature_def(world, plot->feature);
    if (def == nullptr)
        return;
    if (is_sprite(*def)) {
        const auto sequence = reclaimed ? def->seq_name_reclamate : def->seq_name_die;
        const auto shadow = reclaimed ? def->seq_name_reclamate_shadow : def->seq_name_die_shadow;
        if (sequence != 0) {
            if ((plot->flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0)
                return;
            const auto slot = alloc_feature_slot(world);
            if (slot >= slot_capacity)
                return;
            auto* record = feature_record(world, slot);
            record->def_index = plot->feature;
            record->state = static_cast<uint8_t>(
                (reclaimed ? state_reclaim_sequence : 0) | (record->state & ~state_reclaim_sequence)
            );
            plot->flags = static_cast<uint8_t>(plot->flags | OA_PLOT_FLAG_ANIMATING_FEATURE);
            plot->feature_record = static_cast<uint16_t>(slot);
            start_sprite_sequences(host, *record, sequence, shadow);
            record->cell_x = static_cast<int16_t>(cell_x);
            record->state = static_cast<uint8_t>(
                (reclaimed ? state_reclaimed : 0) |
                (record->state & ~(state_burning | state_reclaimed))
            );
            record->cell_z = static_cast<int16_t>(cell_z);
            return;
        }
    }
    replace_with_remnant(world, host, cell_x, cell_z, reclaimed);
}

void ignite_feature(
    World& world, const FeatureHost& host, int32_t cell_x, int32_t cell_z, bool mirrored
) noexcept {
    auto* plot = plot_at(world, cell_x, cell_z);
    if (plot == nullptr || plot->feature >= OA_PLOT_FEATURE_RESERVED)
        return;
    const auto* def = feature_def(world, plot->feature);
    if (def == nullptr || def->seq_name_burn == 0 ||
        (plot->flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0)
        return;
    const auto slot = alloc_feature_slot(world);
    if (slot >= slot_capacity)
        return;
    auto* record = feature_record(world, slot);
    record->def_index = plot->feature;
    plot->flags = static_cast<uint8_t>(plot->flags | OA_PLOT_FLAG_ANIMATING_FEATURE);
    plot->feature_record = static_cast<uint16_t>(slot);
    start_sprite_sequences(host, *record, def->seq_name_burn, def->seq_name_burn_shadow);
    record->state = static_cast<uint8_t>(record->state | state_burning);
    record->cell_x = static_cast<int16_t>(cell_x);
    record->cell_z = static_cast<int16_t>(cell_z);
    const auto half_spark = static_cast<uint32_t>(static_cast<uint16_t>(def->spark_time) >> 1);
    record->spread_countdown = static_cast<uint8_t>(random(host, half_spark) + half_spark);
    record->state =
        static_cast<uint8_t>((mirrored ? state_no_spread : 0) | (record->state & ~state_no_spread));
    if (host.play_sound != nullptr) {
        const FixedVec3 at{
            static_cast<oa_fixed>(static_cast<uint32_t>(cell_x) << cell_to_world_shift),
            0,
            static_cast<oa_fixed>(static_cast<uint32_t>(cell_z) << cell_to_world_shift)
        };
        host.play_sound(host.context, treeburn_sound, &at);
    }
    if (!mirrored)
        feature_changed(host, FeatureChange::ignited, cell_x, cell_z, nullptr);
}

void spread_fire(
    World& world, const FeatureHost& host, const FeatureDef& def, int16_t cell_x, int16_t cell_z
) noexcept {
    const auto try_ignite = [&](int32_t x, int32_t z) {
        const auto* plot = plot_at(world, x, z);
        if (plot == nullptr || plot->feature >= OA_PLOT_FEATURE_RESERVED ||
            (plot->flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0)
            return;
        const auto* neighbour = feature_def(world, plot->feature);
        if (neighbour == nullptr || (neighbour->flags & OA_FEATURE_FLAG_FLAMABLE) == 0)
            return;
        if (static_cast<int32_t>(random(host, percent)) <
            static_cast<int32_t>(static_cast<uint8_t>(neighbour->spread_chance)))
            ignite_feature(world, host, x, z, false);
    };
    for (int32_t z = cell_z - fire_spread_radius; z <= cell_z + fire_spread_radius; ++z)
        for (int32_t x = cell_x - fire_spread_radius; x <= cell_x + fire_spread_radius; ++x)
            if (x != cell_x || z != cell_z)
                try_ignite(x, z);
    // Five steps downwind, two wind units per step, in 16.16 cells.
    auto trail_x = static_cast<int32_t>(static_cast<uint32_t>(cell_x) << 16);
    auto trail_z = static_cast<int32_t>(static_cast<uint32_t>(cell_z) << 16);
    int32_t last_x = cell_x;
    int32_t last_z = cell_z;
    const auto wind_step = [](oa_fixed wind) noexcept {
        return static_cast<int32_t>((static_cast<int64_t>(wind) * 0x20000) >> 16);
    };
    for (int32_t step = 0; step < fire_trail_steps; ++step) {
        trail_x = static_cast<int32_t>(
            static_cast<uint32_t>(trail_x) +
            static_cast<uint32_t>(wind_step(world.game.wind_vector.x))
        );
        trail_z = static_cast<int32_t>(
            static_cast<uint32_t>(trail_z) +
            static_cast<uint32_t>(wind_step(world.game.wind_vector.z))
        );
        const auto x =
            static_cast<int32_t>(static_cast<int16_t>(static_cast<uint32_t>(trail_x) >> 16));
        const auto z =
            static_cast<int32_t>(static_cast<int16_t>(static_cast<uint32_t>(trail_z) >> 16));
        if (x == last_x && z == last_z)
            continue;
        try_ignite(x, z);
        last_x = x;
        last_z = z;
    }
    if (def.burn_weapon != 0 && host.burn_weapon != nullptr) {
        const auto at = feature_center(world, cell_x, cell_z, def);
        host.burn_weapon(host.context, def.burn_weapon, &at);
    }
}

void burn_out_feature(World& world, const FeatureHost& host, const PlacedFeature& record) noexcept {
    auto* plot = plot_at(world, record.cell_x, record.cell_z);
    if (plot == nullptr)
        return;
    const auto* def = feature_def(world, record.def_index);
    const auto next = def != nullptr ? burnt_feature(*def) : no_feature;
    const auto index = plot_index(world, plot);
    clear_plot_feature(world, host, index, false);
    if (next != no_feature)
        place_feature(world, host, index, next, nullptr, nullptr, no_player);
}

FixedVec3 smoke_position(
    const World& world, const FeatureHost& host, const FeatureDef& def, const PlacedFeature& record
) noexcept {
    auto position = feature_center(world, record.cell_x, record.cell_z, def);
    FeatureSequenceFrame frame;
    if (!lookup_frame(host, record.sprite.animation.sequence, record.sprite.animation.frame, frame))
        return position;
    const auto lcg = [&]() noexcept -> int64_t {
        return host.lcg_random != nullptr ? host.lcg_random(host.context) : 0;
    };
    const auto width = static_cast<uint16_t>(frame.width);
    const auto height = static_cast<uint16_t>(frame.height);
    const auto jitter_x = static_cast<int16_t>(lcg() * (width >> 1) / lcg_rand_range);
    const auto high_x = static_cast<uint16_t>(static_cast<uint32_t>(position.x) >> 16);
    const auto new_x = static_cast<uint16_t>(high_x + jitter_x + (width >> 2) - frame.origin_x);
    position.x = static_cast<oa_fixed>(
        (static_cast<uint32_t>(new_x) << 16) | (static_cast<uint32_t>(position.x) & 0xffffu)
    );
    const auto jitter_y = static_cast<int16_t>(lcg() * (height >> 1) / lcg_rand_range);
    const auto high_y = static_cast<uint16_t>(static_cast<uint32_t>(position.y) >> 16);
    const auto new_y =
        static_cast<uint16_t>(high_y + (frame.origin_y - jitter_y) * 2 - (height >> 2) * 2);
    position.y = static_cast<oa_fixed>(
        (static_cast<uint32_t>(new_y) << 16) | (static_cast<uint32_t>(position.y) & 0xffffu)
    );
    return position;
}

void damage_feature(
    World& world,
    const FeatureHost& host,
    std::size_t index,
    int32_t cell_x,
    int32_t cell_z,
    const WeaponDef& weapon
) noexcept {
    if ((world.game.console_flags & OA_CONSOLE_FLAG_TREE_DEATH) == 0 || index >= plot_count(world))
        return;
    auto& plot = world.plots[index];
    if (plot.feature >= OA_PLOT_FEATURE_RESERVED)
        return;
    const auto* def = feature_def(world, plot.feature);
    if (def == nullptr || (def->flags & OA_FEATURE_FLAG_INDESTRUCTIBLE) != 0)
        return;
    if (host.feature_hit_elsewhere != nullptr &&
        host.feature_hit_elsewhere(host.context, oa_weapon_def_id(&weapon), cell_x, cell_z))
        return;
    const auto hit_points = static_cast<uint16_t>(def->damage);
    const bool burns = (def->flags & OA_FEATURE_FLAG_FLAMABLE) != 0 && weapon.fire_starter != 0;
    const bool has_record = (plot.flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0;
    auto sequence_x = cell_x;
    auto sequence_z = cell_z;
    bool finished = false;
    if (burns && !has_record) {
        ignite_feature(world, host, cell_x, cell_z, false);
    } else if (!burns && !has_record) {
        // Features without a record keep their damage in the plot's record word.
        const auto taken = static_cast<uint32_t>(weapon.damage_default) + plot.feature_record;
        if (taken < hit_points)
            plot.feature_record = static_cast<uint16_t>(taken);
        else
            finished = true;
    } else if (!is_sprite(*def)) {
        auto* record = feature_record(world, plot.feature_record);
        if (record == nullptr || record->cell_x != cell_x || record->cell_z != cell_z)
            return;
        sequence_x = record->cell_x;
        sequence_z = record->cell_z;
        record->damage = static_cast<uint16_t>(record->damage + weapon.damage_default);
        finished = record->damage >= hit_points;
    }
    if (finished) {
        start_feature_sequence(world, host, sequence_x, sequence_z, false);
        feature_changed(host, FeatureChange::destroyed, cell_x, cell_z, nullptr);
    }
}

bool reclaim_feature(
    World& world, const FeatureHost& host, Unit& unit, const FixedVec3& position
) noexcept {
    const auto cell_x = position.x / 0x100000;
    const auto cell_z = position.z / 0x100000;
    const auto* plot = plot_at(world, cell_x, cell_z);
    const auto* def = feature_on_plot(world, plot);
    if (def == nullptr || ((plot->flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0 && is_sprite(*def)))
        return false;
    if (host.credit_reclaim != nullptr)
        host.credit_reclaim(host.context, &unit, def->energy, def->metal);
    start_feature_sequence(world, host, cell_x, cell_z, true);
    feature_changed(host, FeatureChange::reclaimed, cell_x, cell_z, &unit);
    return true;
}

void tick_features(World& world, const FeatureHost& host) noexcept {
    for (uint32_t index = 0; world.feature_defs != nullptr && index < world.feature_def_count &&
                             static_cast<int32_t>(index) < world.game.feature_def_count;
         ++index) {
        auto& def = world.feature_defs[index];
        if ((def.flags & OA_FEATURE_FLAG_ANIMATING) != 0) {
            step_def_cursor(host, def.animation_cursor);
            step_def_cursor(host, def.shadow_cursor);
        }
    }

    // Reproduction visits one plot per tick, walking the map backwards.
    auto cursor = reproduce_cursor(world) - 1;
    set_reproduce_cursor(world, cursor);
    const auto width = world.game.map_width;
    if (cursor < 0) {
        set_reproduce_cursor(world, world.game.map_height * width - 1);
    } else if (static_cast<std::size_t>(cursor) < plot_count(world)) {
        const auto& source = world.plots[cursor];
        const auto* def = source.feature < OA_PLOT_FEATURE_RESERVED
                              ? feature_def(world, source.feature)
                              : nullptr;
        if (def != nullptr && (source.flags & OA_PLOT_FLAG_ANIMATING_FEATURE) == 0 &&
            static_cast<int32_t>(random(host, percent)) <
                static_cast<int32_t>(static_cast<uint8_t>(def->reproduce))) {
            // The row divides by the map height, not the width.
            auto x = cursor % width;
            auto z = cursor / world.game.map_height;
            const auto area = static_cast<uint32_t>(static_cast<uint8_t>(def->reproduce_area));
            x += static_cast<int32_t>(random(host, area) - (area >> 1));
            z += static_cast<int32_t>(random(host, area) - (area >> 1));
            const auto* target = plot_at(world, x, z);
            if (target != nullptr && plot_ground_unit(source) == 0 && target->feature == no_feature)
                place_feature(
                    world,
                    host,
                    plot_index(world, target),
                    source.feature,
                    nullptr,
                    nullptr,
                    no_player
                );
        }
    }

    const bool smoke_tick = world.game.tick % smoke_period == 0;
    auto slot = feature_list_head(world, FeatureList::active);
    while (slot != no_slot) {
        auto* record = feature_record(world, slot);
        if (record == nullptr)
            return;
        const int32_t next = record->next;
        const auto* def = feature_def(world, record->def_index);
        if (def == nullptr) {
            slot = next;
            continue;
        }
        if (!is_sprite(*def)) {
            auto& model = record->model;
            if (model.velocity.x == 0 && model.velocity.z == 0 && model.velocity.y == 0) {
                move_feature_slot(world, slot, FeatureList::settled);
            } else {
                model.position.x = wrap_add(model.position.x, model.velocity.x);
                model.position.y = wrap_add(model.position.y, model.velocity.y);
                model.position.z = wrap_add(model.position.z, model.velocity.z);
                const auto ground = static_cast<oa_fixed>(
                    static_cast<uint32_t>(
                        mean_plot_height(world, model.position.x, model.position.z)
                    )
                    << 16
                );
                if (model.position.y <= ground) {
                    model.velocity = {};
                    model.position.y = ground;
                } else if (
                    model.position.y <
                    static_cast<oa_fixed>(static_cast<uint32_t>(world.game.sea_level) << 16)
                ) {
                    model.velocity.x = 0;
                    model.velocity.y = wreck_sink_speed;
                    model.velocity.z = 0;
                } else {
                    model.velocity.y = wrap_add(model.velocity.y, -world.game.gravity);
                }
            }
        } else if ((record->state & state_burning) != 0) {
            if (smoke_tick && host.emit_smoke != nullptr) {
                const auto at = smoke_position(world, host, *def, *record);
                host.emit_smoke(host.context, &at, burning_smoke_layer);
            }
            step_cursor(host, record->sprite.animation);
            if ((record->state & state_has_shadow) != 0)
                step_cursor(host, record->sprite.shadow);
            if (record->sprite.animation.sequence == 0) {
                burn_out_feature(world, host, *record);
            } else if (record->spread_countdown != 0 && (record->state & state_no_spread) == 0) {
                if (--record->spread_countdown == 0)
                    spread_fire(world, host, *def, record->cell_x, record->cell_z);
            }
        } else {
            step_cursor(host, record->sprite.animation);
            if ((record->state & state_has_shadow) != 0)
                step_cursor(host, record->sprite.shadow);
            if (record->sprite.animation.sequence == 0)
                replace_with_remnant(world, host, record->cell_x, record->cell_z, false);
        }
        slot = next;
    }
}

void feature_placement_cell(
    const FeatureDef& def, const FeaturePlacement& placement, int32_t* cell_x, int32_t* cell_z
) noexcept {
    *cell_x = placement.x;
    *cell_z = placement.z;
    if (!is_sprite(def)) {
        *cell_z -= def.footprint_z / 2;
        *cell_x -= def.footprint_x / 2;
    }
}

void apply_feature_placements(
    World& world,
    const FeatureHost& host,
    const FeaturePlacement* placements,
    int32_t count,
    uint16_t (*find_feature)(void* context, const char* name)
) noexcept {
    if (placements == nullptr || find_feature == nullptr)
        return;
    for (int32_t i = 0; i < count; ++i) {
        const auto& placement = placements[i];
        if (placement.name[0] == '\0')
            continue;
        char name[sizeof(placement.name) + 1] = {};
        std::memcpy(name, placement.name, sizeof(placement.name));
        const auto index = find_feature(host.context, name);
        if (index == no_feature)
            continue;
        const auto* def = feature_def(world, index);
        if (def == nullptr)
            continue;
        int32_t x = 0;
        int32_t z = 0;
        feature_placement_cell(*def, placement, &x, &z);
        const auto* plot = plot_at(world, x, z);
        if (plot != nullptr)
            place_feature(world, host, plot_index(world, plot), index, nullptr, nullptr, no_player);
    }
}

void void_hidden_edges(World& world, bool lava_world) noexcept {
    auto& game = world.game;
    game.map_pixel_width = game.map_width_world - hidden_right_edge;
    game.map_pixel_height = game.map_height_world - hidden_bottom_edge;
    if (plot_count(world) == 0)
        return;
    const int32_t width = game.map_width;
    const int32_t height = game.map_height;
    const auto hide = [](MapPlot* plot) {
        if (plot->feature == no_feature || plot->feature == feature_continuation)
            plot->feature = hidden_edge;
    };
    // A plot is drawn half its height above its row.
    const auto drawn_row = [&world](int32_t x, int32_t z) {
        return z * plot_world_units - (plot_at(world, x, z)->height >> 1);
    };
    if (width >= 2)
        for (int32_t z = 0; z < height; ++z) {
            hide(plot_at(world, width - 2, z));
            hide(plot_at(world, width - 1, z));
        }
    for (int32_t x = 0; x < width; ++x)
        for (int32_t z = 0; z < height && drawn_row(x, z) < 0; ++z)
            hide(plot_at(world, x, z));
    // The plot hidden is the one above the plot tested.
    for (int32_t x = 0; x < width; ++x)
        for (int32_t z = height - 1; z >= 1 && drawn_row(x, z) > game.map_pixel_height; --z)
            hide(plot_at(world, x, z - 1));
    if (lava_world)
        for (std::size_t index = 0; index < plot_count(world); ++index)
            if (plot_low_height(world.plots[index]) <= game.sea_level)
                hide(&world.plots[index]);
}

} // namespace oa::sim::feature_runtime
