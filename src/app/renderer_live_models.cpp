// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_live_models.hpp"

#include <atomic>
#include <map>
#include <span>
#include <utility>

namespace oa::app::renderer_live_scene {
namespace {

namespace gpu = renderer_live_gpu;
namespace model = present::model;

/// The most corners, vertices and pieces a type holds, and textures.
constexpr uint32_t field_limit = 1U << gpu::model_piece_shift;
constexpr uint32_t slot_limit = 1U << gpu::model_texture_bits;
constexpr uint32_t vertex_mask = field_limit - 1;
constexpr uint32_t slot_mask = slot_limit - 1;
constexpr uint32_t polygon_corner_mask = 3;
constexpr uint32_t corner_mask = (1U << gpu::model_corner_bits) - 1;

/// What tells two textures of a type apart: a colour, a primitive's fixed
/// frame, a team's frames, a sequence's first frame, or a primitive's
/// running frame.
enum class SlotKind : uint8_t { color, fixed, team, first, running };

/// Returns a revision no scene has had before.
uint64_t new_revision() {
    static std::atomic<uint64_t> next{0};
    return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace

namespace {

/// Fills a type in for an instance's model and pieces, and its triangle
/// corners, counted from its own first.
///
/// @param[out] type the type, its model and textures already set
/// @param[out] records its triangle corners
/// @param source the model
/// @param pieces the instance's pieces
/// @return false where the card cannot draw the model
bool fill_type(
    CardType& type,
    std::vector<uint32_t>& records,
    const formats::objects3d::Model& source,
    std::span<const sim::model_runtime::PieceState> pieces
) {
    if (pieces.size() > field_limit)
        return false;
    const auto& prepared = *type.prepared;
    std::map<std::pair<SlotKind, const void*>, uint16_t> slots;
    // Each texture once, as the first primitive to draw with it finds it.
    const auto slot = [&](SlotKind kind, const void* key, CardSlot made) -> int32_t {
        const auto found = slots.find({kind, key});
        if (found != slots.end())
            return found->second;
        if (type.slots.size() >= slot_limit)
            return -1;
        slots.emplace(std::pair{kind, key}, static_cast<uint16_t>(type.slots.size()));
        type.slots.push_back(made);
        return static_cast<int32_t>(type.slots.size() - 1);
    };
    uint32_t vertices = 0;
    for (uint32_t piece = 0; piece < pieces.size(); ++piece) {
        const uint32_t object_index = pieces[piece].object_index;
        if (object_index >= source.objects.size() || object_index >= prepared.objects.size())
            return false;
        const auto& object = source.objects[object_index];
        type.objects.push_back(object_index);
        type.piece_vertices.push_back(vertices);
        type.piece_polygons.push_back(static_cast<uint32_t>(type.polygons.size()));
        if (object.vertices.size() > field_limit - vertices)
            return false;
        for (const auto& primitive : prepared.objects[object_index].primitives) {
            if (static_cast<int32_t>(primitive.source_index) == object.selection_primitive)
                continue;
            if (primitive.source_index >= object.primitives.size())
                return false;
            const auto& corners = object.primitives[primitive.source_index].vertex_indices;
            if (corners.size() < 3)
                continue;
            const bool colored = (primitive.flags & model::primitive_colored) != 0;
            // A textured polygon draws only with four corners.
            if (!colored && corners.size() != 4)
                continue;
            for (const uint16_t corner : corners)
                if (corner >= object.vertices.size())
                    return false;
            int32_t first = 0;
            int32_t running = 0;
            if (colored) {
                first = running = slot(
                    SlotKind::color,
                    reinterpret_cast<const void*>(uintptr_t{primitive.color}),
                    {nullptr, false, primitive.color}
                );
            } else if ((primitive.flags & model::primitive_animated) == 0) {
                first = running = slot(SlotKind::fixed, primitive.frame, {&primitive, true});
            } else if ((primitive.flags & model::primitive_team) != 0) {
                first = running = slot(SlotKind::team, primitive.texture, {&primitive, true});
            } else {
                first = slot(SlotKind::first, primitive.texture, {&primitive, true});
                running = slot(SlotKind::running, &primitive, {&primitive, false});
            }
            if (first < 0 || running < 0)
                return false;
            CardPolygon polygon;
            polygon.piece = piece;
            polygon.primitive = primitive.source_index;
            polygon.record = static_cast<uint32_t>(records.size() / 2);
            polygon.corners = static_cast<uint32_t>(corners.size());
            polygon.first_slot = static_cast<uint16_t>(first);
            polygon.running_slot = static_cast<uint16_t>(running);
            polygon.colored = colored;
            polygon.color = primitive.color;
            if ((corners.size() - 2) * 3 > field_limit - polygon.record)
                return false;
            // A fan from the first corner, as the scene lists a polygon.
            const uint32_t textures = uint32_t{polygon.first_slot} |
                                      (uint32_t{polygon.running_slot} << gpu::model_texture_bits);
            for (std::size_t fan = 1; fan + 1 < corners.size(); ++fan)
                for (const std::size_t corner : {std::size_t{0}, fan, fan + 1}) {
                    records.push_back(
                        (vertices + corners[corner]) | (piece << gpu::model_piece_shift)
                    );
                    records.push_back(
                        textures | ((static_cast<uint32_t>(corner) & polygon_corner_mask)
                                    << gpu::model_polygon_corner_shift)
                    );
                }
            type.polygons.push_back(polygon);
        }
        vertices += static_cast<uint32_t>(object.vertices.size());
    }
    type.piece_vertices.push_back(vertices);
    type.piece_polygons.push_back(static_cast<uint32_t>(type.polygons.size()));
    return true;
}

} // namespace

int32_t card_type(
    CardTypes& types,
    const sim::model_runtime::Instance& instance,
    const present::model::PreparedModel& prepared
) {
    const auto& handle = instance.model_handle();
    if (!handle)
        return -1;
    const auto pieces = instance.pieces();
    auto& made = types.by_model[handle.get()];
    for (const uint32_t index : made) {
        const auto& type = types.types[index];
        if (type.prepared != &prepared || type.model.owner_before(handle) ||
            handle.owner_before(type.model) || type.objects.size() != pieces.size())
            continue;
        bool same = true;
        for (std::size_t piece = 0; same && piece < pieces.size(); ++piece)
            same = type.objects[piece] == pieces[piece].object_index;
        if (same)
            return type.drawable ? static_cast<int32_t>(index) : -1;
    }
    CardType type;
    type.model = handle;
    type.prepared = &prepared;
    std::vector<uint32_t> records;
    // A model the card cannot draw is kept too, its pieces alone, so that
    // the frames after find it at once.
    type.drawable = fill_type(type, records, *handle, pieces);
    if (!type.drawable) {
        type.objects.clear();
        for (const auto& piece : pieces)
            type.objects.push_back(piece.object_index);
        type.piece_vertices.clear();
        type.polygons.clear();
        type.piece_polygons.clear();
        type.slots.clear();
        records.clear();
    }
    type.first_record = static_cast<uint32_t>(types.records.size() / 2);
    type.record_count = static_cast<uint32_t>(records.size() / 2);
    if (types.revision == 0)
        types.revision = new_revision();
    types.records.insert(types.records.end(), records.begin(), records.end());
    made.push_back(static_cast<uint32_t>(types.types.size()));
    types.types.push_back(std::move(type));
    return types.types.back().drawable ? static_cast<int32_t>(types.types.size() - 1) : -1;
}

void trim_card_types(CardTypes& types) {
    if (types.records.size() / 2 <= card_records_kept)
        return;
    types.types.clear();
    types.by_model.clear();
    types.records.clear();
    types.revision = new_revision();
}

void clear_cards(ModelCards& cards) noexcept {
    cards.instances.clear();
    cards.positions.clear();
    cards.piece_frames.clear();
    cards.textures.clear();
    cards.slot_states.clear();
    cards.tables.clear();
    cards.indices.clear();
    cards.runs.clear();
    cards.units.clear();
    cards.vertices = 0;
    cards.casts.clear();
    cards.cast_indices.clear();
    cards.cast_runs.clear();
    cards.cast_depths = 0;
    cards.shadow_map = {};
    cards.shadow_vertex = 0;
    cards.shadow_ground = 0;
    cards.draw_instance = -1;
    cards.mark = {};
}

void mark_cards(ModelCards& cards) noexcept {
    cards.mark = {
        cards.instances.size(),
        cards.positions.size(),
        cards.piece_frames.size(),
        cards.indices.size(),
        cards.runs.size(),
        cards.units.size(),
        cards.casts.size(),
        cards.cast_indices.size(),
        cards.cast_runs.size(),
        cards.cast_depths,
        cards.vertices
    };
    cards.draw_instance = -1;
}

void unmark_cards(ModelCards& cards) noexcept {
    const auto& mark = cards.mark;
    cards.instances.resize(mark.instances);
    cards.positions.resize(mark.positions);
    cards.piece_frames.resize(mark.piece_frames);
    cards.indices.resize(mark.indices);
    cards.runs.resize(mark.runs);
    cards.units.resize(mark.units);
    cards.casts.resize(mark.casts);
    cards.cast_indices.resize(mark.cast_indices);
    cards.cast_runs.resize(mark.cast_runs);
    cards.cast_depths = mark.cast_depths;
    cards.vertices = mark.vertices;
    cards.draw_instance = -1;
    // A run the draw grew goes back to what it held before.
    if (!cards.runs.empty()) {
        auto& run = cards.runs.back();
        run.index_count = static_cast<uint32_t>(mark.indices - run.first_index);
    }
    if (!cards.cast_runs.empty()) {
        auto& run = cards.cast_runs.back();
        run.index_count = static_cast<uint32_t>(mark.cast_indices - run.first_index);
    }
}

CardCorner card_corner(
    const CardTypes& types, const ModelCards& cards, const CardUnit& unit, uint32_t index
) noexcept {
    const auto& instance = cards.instances[unit.instance];
    const std::size_t record = (std::size_t{instance.corners} + (index & corner_mask)) * 2;
    const uint32_t place = types.records[record];
    const uint32_t drawn = types.records[record + 1];
    const std::size_t vertex = (std::size_t{instance.positions} + (place & vertex_mask)) * 3;
    const bool first = cards.piece_frames[instance.pieces + (place >> gpu::model_piece_shift)] != 0;
    const uint32_t slot =
        first ? drawn & slot_mask : (drawn >> gpu::model_texture_bits) & slot_mask;
    const std::size_t texture = (std::size_t{instance.textures} + slot) * 4;
    const uint32_t corner = (drawn >> gpu::model_polygon_corner_shift) & polygon_corner_mask;
    // Corners 1 and 2 take the second place across, 2 and 3 the second down.
    return {
        cards.positions[vertex],
        cards.positions[vertex + 1],
        cards.positions[vertex + 2],
        cards.textures[texture + (corner == 1 || corner == 2 ? 2 : 0)],
        cards.textures[texture + (corner >= 2 ? 3 : 1)]
    };
}

renderer_live_gpu::Vertex card_vertex(
    const CardTypes& types, const ModelCards& cards, const CardUnit& unit, uint32_t index
) noexcept {
    const auto corner = card_corner(types, cards, unit, index);
    gpu::Vertex vertex{};
    vertex.x = corner.x;
    vertex.height = corner.height;
    vertex.z = corner.z;
    vertex.u = corner.u;
    vertex.v = corner.v;
    vertex.brightness = cards.instances[unit.instance].brightness;
    return vertex;
}

renderer_live_gpu::ModelDraws card_draws(const CardTypes& types, const ModelCards& cards) noexcept {
    gpu::ModelDraws draws;
    draws.corners = types.records;
    draws.corners_revision = types.revision;
    draws.instances = cards.instances;
    draws.positions = cards.positions;
    draws.piece_frames = cards.piece_frames;
    draws.textures = cards.textures;
    draws.indices = cards.indices;
    draws.runs = cards.runs;
    draws.casts = cards.casts;
    draws.cast_indices = cards.cast_indices;
    draws.cast_runs = cards.cast_runs;
    draws.shadow_map = cards.shadow_map;
    draws.shadow_vertex = cards.shadow_vertex;
    draws.shadow_ground = cards.shadow_ground;
    return draws;
}

} // namespace oa::app::renderer_live_scene
