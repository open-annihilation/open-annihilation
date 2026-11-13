// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_live_models.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <span>
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
    return draws;
}

} // namespace oa::app::renderer_live_scene
