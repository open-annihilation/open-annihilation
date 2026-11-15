// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_live_models.hpp"
#include "oa/present/model/model_draw.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>

namespace oa::app::renderer_live_scene {
namespace {

namespace gpu = renderer_live_gpu;
namespace model = present::model;

/// Matches the immutable model, prepared textures and complete piece object order.
/// @param type the retained model type
/// @param instance the model and current pieces
/// @param prepared the prepared textures used by the draw
/// @param detail none for complete geometry, otherwise a coarse body variant
/// @return true when the retained type describes the instance
bool same_card_type(
    const CardType& type,
    const sim::model_runtime::Instance& instance,
    const present::model::PreparedModel& prepared,
    std::optional<body_lod::Quality> detail = {}
) noexcept {
    const auto& handle = instance.model_handle();
    const auto pieces = instance.pieces();
    if (!handle || type.prepared != &prepared || type.body_detail != detail ||
        type.model.owner_before(handle) || handle.owner_before(type.model) ||
        type.objects.size() != pieces.size())
        return false;
    for (std::size_t piece = 0; piece < pieces.size(); ++piece)
        if (type.objects[piece] != pieces[piece].object_index)
            return false;
    return true;
}

/// The most corners, vertices and pieces a type holds, and textures.
constexpr uint32_t field_limit = 1U << gpu::model_piece_shift;
constexpr uint32_t slot_limit = 1U << gpu::model_texture_bits;
constexpr uint32_t vertex_mask = field_limit - 1;
constexpr uint32_t slot_mask = slot_limit - 1;
constexpr uint32_t polygon_corner_mask = 3;
constexpr uint32_t corner_mask = (1U << gpu::model_corner_bits) - 1;
constexpr std::size_t shared_pose_count = 1024;
constexpr std::size_t shared_pose_points = 131072;
constexpr std::size_t pose_lookup_slots = shared_pose_count * 2;
constexpr std::size_t pose_lookup_probes = 8;
constexpr std::size_t recent_pose_types = 1024;
constexpr uint64_t pose_digest_start = 14695981039346656037ULL;
constexpr uint64_t pose_digest_factor = 1099511628211ULL;
constexpr float fixed_scale = formats::objects3d::kThreeDoUnitsPerWorldUnit;

/// Forgets lookup entries without releasing their bounded memory.
void forget_poses(ModelCards& cards) noexcept {
    cards.poses.clear();
    cards.pose_points.clear();
    if (++cards.pose_epoch == 0) {
        std::fill(cards.pose_slots.begin(), cards.pose_slots.end(), CardPoseSlot{});
        std::fill(cards.recent_poses.begin(), cards.recent_poses.end(), CardPoseRecent{});
        cards.pose_epoch = 1;
    }
}

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

/// Builds a coarse type from real piece vertices and the original materials.
/// Clear image-key faces retain their ordinary topology; the other faces form
/// a closed, lower-detail body and still follow every piece's current pose.
bool fill_body_type(
    CardType& type,
    std::vector<uint32_t>& records,
    const formats::objects3d::Model& source,
    std::span<const sim::model_runtime::PieceState> pieces,
    body_lod::Quality quality
) {
    CardType complete;
    complete.prepared = type.prepared;
    std::vector<uint32_t> complete_records;
    if (!fill_type(complete, complete_records, source, pieces))
        return false;
    type.objects = complete.objects;
    type.slots = std::move(complete.slots);
    uint32_t vertices{};
    for (uint32_t piece = 0; piece < pieces.size(); ++piece) {
        const auto& object = source.objects[type.objects[piece]];
        type.source_vertex_counts.push_back(static_cast<uint32_t>(object.vertices.size()));
        type.piece_vertices.push_back(vertices);
        type.piece_polygons.push_back(static_cast<uint32_t>(type.polygons.size()));
        const auto first = complete.piece_polygons[piece];
        const auto last = complete.piece_polygons[piece + 1];
        std::vector<uint32_t> primitives;
        for (uint32_t at = first; at < last; ++at) {
            const auto& polygon = complete.polygons[at];
            if (!polygon.colored || polygon.color != model::image_key)
                primitives.push_back(polygon.primitive);
        }
        const auto body = body_lod::make_piece(object, primitives, quality);
        if (!body.valid)
            return false;

        struct Face {
            std::array<uint16_t, 3> vertices{};
            uint32_t primitive{};
            bool reduced{};
        };

        std::vector<Face> faces;
        for (const auto& triangle : body.triangles)
            faces.push_back({triangle.vertices, triangle.primitive, body.reduced});
        for (uint32_t at = first; at < last; ++at) {
            const auto& polygon = complete.polygons[at];
            if (!polygon.colored || polygon.color != model::image_key)
                continue;
            const auto& corners = object.primitives[polygon.primitive].vertex_indices;
            for (std::size_t fan = 1; fan + 1 < corners.size(); ++fan)
                faces.push_back(
                    {{corners[0], corners[fan], corners[fan + 1]}, polygon.primitive, false}
                );
        }
        std::vector<uint32_t> compact(object.vertices.size(), field_limit);
        uint32_t piece_vertices{};
        for (const auto& face : faces)
            for (const auto vertex : face.vertices) {
                if (vertex >= object.vertices.size())
                    return false;
                if (compact[vertex] == field_limit) {
                    if (piece_vertices >= field_limit - vertices)
                        return false;
                    compact[vertex] = piece_vertices++;
                    type.source_vertices.push_back(vertex);
                }
            }
        for (const auto& face : faces) {
            const auto found = std::find_if(
                complete.polygons.begin() + first,
                complete.polygons.begin() + last,
                [&](const CardPolygon& polygon) { return polygon.primitive == face.primitive; }
            );
            if (found == complete.polygons.begin() + last)
                return false;
            CardPolygon polygon = *found;
            polygon.record = static_cast<uint32_t>(records.size() / 2);
            polygon.corners = 3;
            polygon.body_lod = true;
            if (polygon.record > field_limit - polygon.corners)
                return false;
            const auto& original = object.primitives[face.primitive].vertex_indices;
            const uint32_t textures = uint32_t{polygon.first_slot} |
                                      (uint32_t{polygon.running_slot} << gpu::model_texture_bits);
            for (std::size_t corner = 0; corner < face.vertices.size(); ++corner) {
                const auto compact_vertex = compact[face.vertices[corner]];
                polygon.body_vertices[corner] = static_cast<uint16_t>(compact_vertex);
                uint32_t texture_corner = static_cast<uint32_t>(corner);
                if (!face.reduced) {
                    const auto original_corner =
                        std::find(original.begin(), original.end(), face.vertices[corner]);
                    if (original_corner == original.end())
                        return false;
                    texture_corner = static_cast<uint32_t>(original_corner - original.begin());
                }
                records.push_back((vertices + compact_vertex) | (piece << gpu::model_piece_shift));
                records.push_back(
                    textures |
                    ((texture_corner & polygon_corner_mask) << gpu::model_polygon_corner_shift)
                );
            }
            type.polygons.push_back(polygon);
        }
        vertices += piece_vertices;
    }
    type.piece_vertices.push_back(vertices);
    type.piece_polygons.push_back(static_cast<uint32_t>(type.polygons.size()));
    return true;
}

} // namespace

void prepare_shadow_runs(CardType& type) noexcept {
    type.shadow_runs_ready = false;
    type.shadow_runs.clear();
    type.piece_shadow_runs.clear();
    constexpr uint32_t triangle_corners = 3;
    constexpr uint32_t type_corners = corner_mask + 1;
    if (!type.drawable || type.record_count > type_corners || type.objects.size() > field_limit ||
        type.polygons.size() > type.record_count / triangle_corners ||
        type.piece_polygons.size() != type.objects.size() + 1 || type.piece_polygons.front() != 0 ||
        type.piece_polygons.back() != type.polygons.size())
        return;
    try {
        std::vector<CardCornerRun> runs;
        std::vector<uint32_t> pieces;
        runs.reserve(type.polygons.size());
        pieces.reserve(type.piece_polygons.size());
        uint32_t next_corner{};
        for (uint32_t piece = 0; piece < type.objects.size(); ++piece) {
            const auto first = type.piece_polygons[piece];
            const auto last = type.piece_polygons[piece + 1];
            if (first > last || last > type.polygons.size())
                return;
            pieces.push_back(static_cast<uint32_t>(runs.size()));
            const auto first_run = runs.size();
            for (uint32_t at = first; at < last; ++at) {
                const auto& polygon = type.polygons[at];
                if (polygon.piece != piece || polygon.record != next_corner ||
                    polygon.corners < triangle_corners ||
                    polygon.corners - 2 > type_corners / triangle_corners)
                    return;
                const uint32_t count = (polygon.corners - 2) * triangle_corners;
                if (count > type.record_count - next_corner)
                    return;
                next_corner += count;
                if (polygon.colored && polygon.color == model::image_key)
                    continue;
                if (runs.size() > first_run &&
                    runs.back().first + runs.back().count == polygon.record)
                    runs.back().count += count;
                else
                    runs.push_back({polygon.record, count});
            }
        }
        if (next_corner != type.record_count)
            return;
        pieces.push_back(static_cast<uint32_t>(runs.size()));
        type.shadow_runs = std::move(runs);
        type.piece_shadow_runs = std::move(pieces);
        type.shadow_runs_ready = true;
    } catch (const std::bad_alloc&) {
        // Optional metadata cannot turn a drawable model into a failed frame.
    } catch (const std::length_error&) {
        // The checked source limits normally prevent an oversized allocation.
    }
}

int32_t card_type(
    CardTypes& types,
    const sim::model_runtime::Instance& instance,
    const present::model::PreparedModel& prepared
) {
    return card_type(types, instance, prepared, {});
}

int32_t card_type(
    CardTypes& types,
    const sim::model_runtime::Instance& instance,
    const present::model::PreparedModel& prepared,
    std::optional<body_lod::Quality> detail
) {
    const auto& handle = instance.model_handle();
    if (!handle)
        return -1;
    const auto pieces = instance.pieces();
    auto& made = types.by_model[handle.get()];
    for (const uint32_t index : made) {
        const auto& type = types.types[index];
        if (same_card_type(type, instance, prepared, detail))
            return type.drawable ? static_cast<int32_t>(index) : -1;
    }
    CardType type;
    type.model = handle;
    type.prepared = &prepared;
    type.body_detail = detail;
    std::vector<uint32_t> records;
    // A model the card cannot draw is kept too, its pieces alone, so that
    // the frames after find it at once.
    type.drawable = detail ? fill_body_type(type, records, *handle, pieces, *detail)
                           : fill_type(type, records, *handle, pieces);
    if (!type.drawable) {
        type.objects.clear();
        for (const auto& piece : pieces)
            type.objects.push_back(piece.object_index);
        type.piece_vertices.clear();
        type.source_vertices.clear();
        type.source_vertex_counts.clear();
        type.polygons.clear();
        type.piece_polygons.clear();
        type.slots.clear();
        records.clear();
    }
    type.first_record = static_cast<uint32_t>(types.records.size() / 2);
    type.record_count = static_cast<uint32_t>(records.size() / 2);
    if (type.drawable)
        prepare_shadow_runs(type);
    if (types.revision == 0)
        types.revision = new_revision();
    types.records.insert(types.records.end(), records.begin(), records.end());
    made.push_back(static_cast<uint32_t>(types.types.size()));
    types.types.push_back(std::move(type));
    return types.types.back().drawable ? static_cast<int32_t>(types.types.size() - 1) : -1;
}

bool cached_card_type(
    const CardTypes& types,
    const sim::model_runtime::Instance& instance,
    const present::model::PreparedModel& prepared
) noexcept {
    const auto& handle = instance.model_handle();
    if (!handle)
        return false;
    const auto found = types.by_model.find(handle.get());
    if (found == types.by_model.end())
        return false;
    for (const uint32_t index : found->second)
        if (index < types.types.size() && same_card_type(types.types[index], instance, prepared))
            return types.types[index].drawable;
    return false;
}

void trim_card_types(CardTypes& types) {
    if (types.records.size() / 2 <= card_records_kept)
        return;
    types.types.clear();
    types.by_model.clear();
    types.records.clear();
    types.revision = new_revision();
}

void prepare_shadow_index_patterns(CardTypes& types) noexcept {
    const auto current = types.shadow_patterns;
    if (types.revision == 0) {
        types.shadow_patterns.reset();
        return;
    }
    try {
        if (!types.shadow_pattern_owner.value)
            types.shadow_pattern_owner.value = std::make_shared<ShadowIndexOwner>();
        const auto& owner = types.shadow_pattern_owner.value;
        const bool same_owner = current && !current->append_owner.owner_before(owner) &&
                                !owner.owner_before(current->append_owner);
        if (current && current->type_revision == types.revision && same_owner)
            return;
        auto next = current && current->type_revision == types.revision
                        ? std::make_shared<ShadowIndexPatterns>(*current)
                        : std::make_shared<ShadowIndexPatterns>();
        next->revision = new_revision();
        next->type_revision = types.revision;
        next->append_owner = owner;
        types.shadow_patterns = std::move(next);
    } catch (const std::bad_alloc&) {
        types.shadow_patterns.reset();
    } catch (const std::length_error&) {
        types.shadow_patterns.reset();
    }
}

namespace {

/// Visits the exact ordinary selected type-local corner sequence.
template <typename Take>
bool visit_shadow_pattern(const CardType& type, ShadowIndexKey key, Take&& take) {
    const auto pieces = type.objects.size();
    if (!type.drawable || !type.shadow_runs_ready || pieces == 0 || pieces > 64 ||
        type.record_count > field_limit || type.piece_polygons.size() != pieces + 1 ||
        type.piece_shadow_runs.size() != pieces + 1 || key.selected_mask == 0 ||
        (pieces < 64 && (key.selected_mask >> pieces) != 0))
        return false;
    for (std::size_t piece = 0; piece < pieces; ++piece) {
        if ((key.selected_mask & (uint64_t{1} << piece)) == 0)
            continue;
        if (key.walls) {
            const auto first = type.piece_polygons[piece];
            const auto last = type.piece_polygons[piece + 1];
            if (first > last || last > type.polygons.size())
                return false;
            if (first == last)
                continue;
            const auto& tail = type.polygons[last - 1];
            if (tail.corners < 3 || tail.corners - 2 > type.record_count / 3)
                return false;
            const auto begin = type.polygons[first].record;
            const auto end = uint64_t{tail.record} + uint64_t{tail.corners - 2} * 3;
            if (begin > end || end > type.record_count)
                return false;
            for (uint32_t corner = begin; corner < end; ++corner)
                take(corner);
        } else {
            const auto first = type.piece_shadow_runs[piece];
            const auto last = type.piece_shadow_runs[piece + 1];
            if (first > last || last > type.shadow_runs.size())
                return false;
            for (auto run = first; run < last; ++run) {
                const auto& span = type.shadow_runs[run];
                if (span.first > type.record_count || span.count > type.record_count - span.first ||
                    span.count % 3 != 0)
                    return false;
                for (uint32_t corner = 0; corner < span.count; ++corner)
                    take(span.first + corner);
            }
        }
    }
    return true;
}

} // namespace

void admit_shadow_index_patterns(
    CardTypes& types, std::span<const ShadowIndexKey> candidates
) noexcept {
    prepare_shadow_index_patterns(types);
    const auto current = types.shadow_patterns;
    if (!current || current->keys.size() != current->patterns.size() ||
        current->patterns.size() > gpu::shadow_template_pattern_limit ||
        current->indices.size() > gpu::shadow_template_byte_limit / sizeof(uint32_t))
        return;
    try {
        std::shared_ptr<ShadowIndexPatterns> next;
        std::size_t admitted{}, bytes{};
        for (const auto& key : candidates.first(
                 std::min<std::size_t>(candidates.size(), gpu::shadow_template_pattern_limit)
             )) {
            const auto& store = next ? *next : *current;
            if (admitted == gpu::shadow_template_admissions_per_frame ||
                store.patterns.size() >= gpu::shadow_template_pattern_limit)
                break;
            if (key.type_revision != types.revision || key.type >= types.types.size() ||
                std::find(store.keys.begin(), store.keys.end(), key) != store.keys.end())
                continue;
            const auto& type = types.types[key.type];
            std::size_t count{};
            if (!visit_shadow_pattern(type, key, [&](uint32_t) { ++count; }) || count == 0 ||
                count % 3 != 0 ||
                count > gpu::shadow_template_admission_bytes /
                            (gpu::model_run_instances * sizeof(uint32_t)))
                continue;
            const auto words = count * gpu::model_run_instances;
            const auto added = words * sizeof(uint32_t);
            if (added > gpu::shadow_template_admission_bytes - bytes ||
                added > gpu::shadow_template_byte_limit - store.indices.size() * sizeof(uint32_t))
                continue;
            if (!next)
                next = std::make_shared<ShadowIndexPatterns>(*current);
            const auto first = static_cast<uint32_t>(next->indices.size());
            next->indices.resize(next->indices.size() + words);
            std::size_t at = first;
            for (uint32_t relative = 0; relative < gpu::model_run_instances; ++relative)
                (void)visit_shadow_pattern(type, key, [&](uint32_t corner) {
                    next->indices[at++] = (relative << gpu::model_corner_bits) | corner;
                });
            next->keys.push_back(key);
            next->patterns.push_back(
                {first,
                 static_cast<uint32_t>(count),
                 type.first_record,
                 type.record_count,
                 key.selected_mask}
            );
            bytes += added;
            ++admitted;
        }
        if (next)
            types.shadow_patterns = std::move(next);
    } catch (const std::bad_alloc&) {
        // Optional next-frame metadata never changes this frame's shadows.
    } catch (const std::length_error&) {
        // Keep the previous complete immutable snapshot on refusal.
    }
}

uint32_t shadow_index_pattern(const ShadowIndexStream& stream, ShadowIndexKey key) noexcept {
    const auto& store = stream.patterns;
    if (!stream.enabled || !store || store->type_revision != key.type_revision ||
        store->keys.size() != store->patterns.size() ||
        store->patterns.size() > gpu::shadow_template_pattern_limit)
        return 0;
    const auto found = std::find(store->keys.begin(), store->keys.end(), key);
    return found == store->keys.end() ? 0 : static_cast<uint32_t>(found - store->keys.begin() + 1);
}

void remember_shadow_index_key(ShadowIndexStream& stream, ShadowIndexKey key) noexcept {
    if (key.selected_mask == 0 || stream.candidate_count >= stream.candidates.size())
        return;
    const auto end = stream.candidates.begin() + stream.candidate_count;
    if (std::find(stream.candidates.begin(), end, key) == end)
        stream.candidates[stream.candidate_count++] = key;
}

bool materialize_shadow_index_run(ShadowIndexStream& stream, std::size_t named) {
    if (stream.procedural || named >= stream.runs.size())
        return false;
    auto& run = stream.runs[named];
    if (run.pattern == 0)
        return true;
    if (named + 1 != stream.runs.size())
        return false;
    const auto& store = stream.patterns;
    if (!store || run.pattern > store->patterns.size() || run.cast_count == 0 ||
        run.cast_count > gpu::model_run_instances)
        return false;
    const auto& pattern = store->patterns[run.pattern - 1];
    const uint64_t words = uint64_t{pattern.indices_per_cast} * run.cast_count;
    if (words != run.index_count || pattern.first_index > store->indices.size() ||
        words > store->indices.size() - pattern.first_index || words > gpu::largest_vertex_count ||
        stream.indices.size() > gpu::largest_vertex_count - words)
        return false;
    const auto first = static_cast<uint32_t>(stream.indices.size());
    stream.indices.insert(
        stream.indices.end(),
        store->indices.begin() + pattern.first_index,
        store->indices.begin() + pattern.first_index + static_cast<std::ptrdiff_t>(words)
    );
    run.pattern = 0;
    run.first_index = first;
    return true;
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
    cards.markers.clear();
    cards.marker_runs.clear();
    cards.marker_uv = {};
    cards.marker_first_index = 0;
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
    forget_poses(cards);
    auto& shadow = cards.shadow_indices;
    shadow.enabled = false;
    shadow.procedural = false;
    shadow.patterns.reset();
    shadow.indices.clear();
    shadow.runs.clear();
    shadow.casts.clear();
    shadow.logical_indices = 0;
    shadow.candidate_count = 0;
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
        cards.vertices,
        cards.markers.size(),
        cards.marker_runs.size(),
        cards.marker_first_index,
        cards.marker_uv
    };
    cards.draw_instance = -1;
    const auto& shadow = cards.shadow_indices;
    cards.mark.shadow_indices = {
        shadow.indices.size(),
        shadow.runs.size(),
        shadow.logical_indices,
        shadow.candidate_count,
        shadow.runs.empty() ? gpu::ShadowIndexRun{} : shadow.runs.back(),
        shadow.casts.size()
    };
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
    cards.markers.resize(mark.markers);
    cards.marker_runs.resize(mark.marker_runs);
    cards.marker_first_index = mark.marker_first_index;
    cards.marker_uv = mark.marker_uv;
    if (!cards.marker_runs.empty()) {
        auto& run = cards.marker_runs.back();
        run.instance_count = static_cast<uint32_t>(mark.markers - run.first_instance);
    }
    cards.draw_instance = -1;
    auto& shadow = cards.shadow_indices;
    const auto& shadow_mark = mark.shadow_indices;
    shadow.indices.resize(shadow_mark.indices);
    shadow.runs.resize(shadow_mark.runs);
    shadow.casts.resize(shadow_mark.casts);
    shadow.logical_indices = shadow_mark.logical_indices;
    shadow.candidate_count = shadow_mark.candidates;
    if (!shadow.runs.empty())
        shadow.runs.back() = shadow_mark.last_run;
    // A material retry may have discarded a range that a lookup named.
    forget_poses(cards);
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

namespace {

/// Lists only a coarse type's selected points, retaining exact pose-cache checks.
int64_t body_card_positions(
    ModelCards& cards,
    uint32_t type_index,
    const CardType& type,
    std::span<const sim::model_runtime::PieceState> pieces,
    std::span<const std::vector<formats::objects3d::FixedVector3>> shown
) {
    using Point = formats::objects3d::FixedVector3;
    if (type.piece_vertices.size() != pieces.size() + 1 || type.piece_vertices.front() != 0 ||
        type.source_vertex_counts.size() != pieces.size())
        return -1;
    const std::size_t count = type.piece_vertices.back();
    if (type.source_vertices.size() != count)
        return -1;
    const auto points = [&](std::size_t piece) -> std::span<const Point> {
        return piece < shown.size() ? std::span<const Point>{shown[piece]}
                                    : std::span<const Point>{pieces[piece].transformed_vertices};
    };
    for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
        const auto first = type.piece_vertices[piece];
        const auto last = type.piece_vertices[piece + 1];
        if (first > last || last > count ||
            points(piece).size() != type.source_vertex_counts[piece])
            return -1;
        for (std::size_t at = first; at < last; ++at)
            if (type.source_vertices[at] >= points(piece).size())
                return -1;
    }
    const auto visit = [&](auto&& action) {
        for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
            const auto source = points(piece);
            for (std::size_t at = type.piece_vertices[piece]; at < type.piece_vertices[piece + 1];
                 ++at)
                action(source[type.source_vertices[at]]);
        }
    };
    const auto matches = [&](const CardPose& pose) {
        if (pose.type != type_index || pose.point_count != count)
            return false;
        std::size_t at = pose.first_point;
        bool equal = true;
        visit([&](const Point& point) {
            const auto& before = cards.pose_points[at++];
            equal &= point.x == before.x && point.y == before.y && point.z == before.z;
        });
        return equal;
    };
    if (type_index < cards.recent_poses.size()) {
        const auto recent = cards.recent_poses[type_index];
        if (recent.epoch == cards.pose_epoch && recent.pose < cards.poses.size() &&
            matches(cards.poses[recent.pose]))
            return cards.poses[recent.pose].first_position;
    }
    uint64_t digest = (pose_digest_start ^ type_index) * pose_digest_factor;
    visit([&](const Point& point) {
        for (const int32_t coordinate : {point.x, point.y, point.z})
            digest = (digest ^ static_cast<uint32_t>(coordinate)) * pose_digest_factor;
    });
    if (cards.pose_slots.empty()) {
        cards.pose_slots.resize(pose_lookup_slots);
        cards.recent_poses.resize(recent_pose_types);
        cards.poses.reserve(shared_pose_count);
        cards.pose_points.reserve(shared_pose_points);
        if (cards.pose_epoch == 0)
            cards.pose_epoch = 1;
    }
    std::size_t empty = cards.pose_slots.size();
    for (std::size_t probe = 0; probe < pose_lookup_probes; ++probe) {
        const std::size_t at = (digest + probe) & (pose_lookup_slots - 1);
        const auto slot = cards.pose_slots[at];
        if (slot.epoch != cards.pose_epoch) {
            empty = at;
            break;
        }
        if (slot.digest == digest && slot.pose < cards.poses.size() &&
            matches(cards.poses[slot.pose])) {
            if (type_index < cards.recent_poses.size())
                cards.recent_poses[type_index] = {slot.pose, cards.pose_epoch};
            return cards.poses[slot.pose].first_position;
        }
    }
    const std::size_t first = cards.positions.size() / 3;
    if (count > gpu::largest_vertex_count || first > gpu::largest_vertex_count - count)
        return -1;
    cards.positions.resize(cards.positions.size() + count * 3);
    std::size_t at = first * 3;
    visit([&](const Point& point) {
        cards.positions[at++] = point.x / fixed_scale;
        cards.positions[at++] = point.y / fixed_scale;
        cards.positions[at++] = -(point.z / fixed_scale);
    });
    if (empty != cards.pose_slots.size() && cards.poses.size() < shared_pose_count &&
        count <= shared_pose_points - cards.pose_points.size()) {
        const auto pose = static_cast<uint32_t>(cards.poses.size());
        cards.poses.push_back(
            {type_index,
             static_cast<uint32_t>(first),
             static_cast<uint32_t>(cards.pose_points.size()),
             static_cast<uint32_t>(count)}
        );
        visit([&](const Point& point) { cards.pose_points.push_back(point); });
        cards.pose_slots[empty] = {digest, pose, cards.pose_epoch};
        if (type_index < cards.recent_poses.size())
            cards.recent_poses[type_index] = {pose, cards.pose_epoch};
    }
    return static_cast<int64_t>(first);
}

} // namespace

int64_t card_positions(
    ModelCards& cards,
    uint32_t type_index,
    const CardType& type,
    std::span<const sim::model_runtime::PieceState> pieces,
    std::span<const std::vector<formats::objects3d::FixedVector3>> shown
) {
    if (type.body_detail)
        return body_card_positions(cards, type_index, type, pieces, shown);
    using Point = formats::objects3d::FixedVector3;
    static_assert(sizeof(Point) == 3 * sizeof(int32_t));
    if (type.piece_vertices.size() != pieces.size() + 1 || type.piece_vertices.front() != 0)
        return -1;
    const auto points = [&](std::size_t piece) -> std::span<const Point> {
        return piece < shown.size() ? std::span<const Point>{shown[piece]}
                                    : std::span<const Point>{pieces[piece].transformed_vertices};
    };
    for (std::size_t piece = 0; piece < pieces.size(); ++piece)
        if (type.piece_vertices[piece + 1] < type.piece_vertices[piece] ||
            points(piece).size() != type.piece_vertices[piece + 1] - type.piece_vertices[piece])
            return -1;
    const std::size_t count = type.piece_vertices.back();
    const auto matches = [&](const CardPose& pose) {
        if (pose.type != type_index || pose.point_count != count)
            return false;
        std::size_t at = pose.first_point;
        for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
            const auto vertices = points(piece);
            if (!vertices.empty() &&
                std::memcmp(
                    cards.pose_points.data() + at, vertices.data(), vertices.size_bytes()
                ) != 0)
                return false;
            at += vertices.size();
        }
        return true;
    };
    if (type_index < cards.recent_poses.size()) {
        const auto recent = cards.recent_poses[type_index];
        if (recent.epoch == cards.pose_epoch && recent.pose < cards.poses.size() &&
            matches(cards.poses[recent.pose]))
            return cards.poses[recent.pose].first_position;
    }
    uint64_t digest = (pose_digest_start ^ type_index) * pose_digest_factor;
    for (std::size_t piece = 0; piece < pieces.size(); ++piece)
        for (const auto& point : points(piece))
            for (const int32_t coordinate : {point.x, point.y, point.z})
                digest = (digest ^ static_cast<uint32_t>(coordinate)) * pose_digest_factor;
    if (cards.pose_slots.empty()) {
        cards.pose_slots.resize(pose_lookup_slots);
        cards.recent_poses.resize(recent_pose_types);
        cards.poses.reserve(shared_pose_count);
        cards.pose_points.reserve(shared_pose_points);
        if (cards.pose_epoch == 0)
            cards.pose_epoch = 1;
    }
    std::size_t empty = cards.pose_slots.size();
    for (std::size_t probe = 0; probe < pose_lookup_probes; ++probe) {
        const std::size_t at = (digest + probe) & (pose_lookup_slots - 1);
        const auto slot = cards.pose_slots[at];
        if (slot.epoch != cards.pose_epoch) {
            empty = at;
            break;
        }
        if (slot.digest == digest && slot.pose < cards.poses.size() &&
            matches(cards.poses[slot.pose])) {
            if (type_index < cards.recent_poses.size())
                cards.recent_poses[type_index] = {slot.pose, cards.pose_epoch};
            return cards.poses[slot.pose].first_position;
        }
    }
    const std::size_t first = cards.positions.size() / 3;
    if (count > gpu::largest_vertex_count || first > gpu::largest_vertex_count - count)
        return -1;
    cards.positions.resize(cards.positions.size() + count * 3);
    std::size_t at = first * 3;
    for (std::size_t piece = 0; piece < pieces.size(); ++piece)
        for (const auto& point : points(piece)) {
            cards.positions[at++] = point.x / fixed_scale;
            cards.positions[at++] = point.y / fixed_scale;
            cards.positions[at++] = -(point.z / fixed_scale);
        }
    if (empty != cards.pose_slots.size() && cards.poses.size() < shared_pose_count &&
        count <= shared_pose_points - cards.pose_points.size()) {
        const auto pose = static_cast<uint32_t>(cards.poses.size());
        cards.poses.push_back(
            {type_index,
             static_cast<uint32_t>(first),
             static_cast<uint32_t>(cards.pose_points.size()),
             static_cast<uint32_t>(count)}
        );
        for (std::size_t piece = 0; piece < pieces.size(); ++piece) {
            const auto vertices = points(piece);
            cards.pose_points.insert(cards.pose_points.end(), vertices.begin(), vertices.end());
        }
        cards.pose_slots[empty] = {digest, pose, cards.pose_epoch};
        if (type_index < cards.recent_poses.size())
            cards.recent_poses[type_index] = {pose, cards.pose_epoch};
    }
    return static_cast<int64_t>(first);
}

CardCorner card_corner(
    const CardTypes& types, const ModelCards& cards, const CardUnit& unit, uint32_t index
) noexcept {
    if (unit.marker) {
        const auto& point = cards.markers[unit.instance].corners[index];
        return {point[0], point[1], point[2], cards.marker_uv[0], cards.marker_uv[1]};
    }
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
        instance.origin[0] + cards.positions[vertex],
        instance.origin[1] + cards.positions[vertex + 1],
        instance.origin[2] + cards.positions[vertex + 2],
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
    if (unit.marker) {
        const auto& marker = cards.markers[unit.instance];
        vertex.brightness = marker.brightness;
        vertex.red = marker.color[0];
        vertex.green = marker.color[1];
        vertex.blue = marker.color[2];
    } else
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
    draws.markers = cards.markers;
    draws.marker_runs = cards.marker_runs;
    draws.marker_uv = cards.marker_uv;
    const auto& shadow = cards.shadow_indices;
    if (shadow.enabled) {
        draws.shadow_index_encoding = shadow.procedural ? gpu::ShadowIndexEncoding::procedural
                                                       : gpu::ShadowIndexEncoding::compact;
        draws.shadow_index_runs = shadow.runs;
        draws.shadow_dynamic_indices = shadow.indices;
        draws.logical_shadow_index_count = shadow.logical_indices;
        if (shadow.procedural)
            draws.shadow_index_casts = shadow.casts;
        if (shadow.patterns) {
            draws.shadow_patterns = shadow.patterns->patterns;
            draws.shadow_pattern_indices = shadow.patterns->indices;
            draws.shadow_patterns_revision = shadow.patterns->revision;
            draws.shadow_patterns_type_revision = shadow.patterns->type_revision;
        }
    }
    return draws;
}

} // namespace oa::app::renderer_live_scene
