// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

namespace {

/// Expands the ordinary polygon shadow loop without using cached ranges.
std::vector<uint32_t> polygon_shadow_corners(const oa::app::renderer_live_scene::CardType& type) {
    std::vector<uint32_t> result;
    for (std::size_t piece = 0; piece < type.objects.size(); ++piece)
        for (uint32_t at = type.piece_polygons[piece]; at < type.piece_polygons[piece + 1]; ++at) {
            const auto& polygon = type.polygons[at];
            if (polygon.colored && polygon.color == oa::present::model::image_key)
                continue;
            for (uint32_t corner = 0; corner < (polygon.corners - 2) * 3; ++corner)
                result.push_back(polygon.record + corner);
        }
    return result;
}

/// Expands only the prepared ordinary shadow ranges, piece by piece.
std::vector<uint32_t> cached_shadow_corners(const oa::app::renderer_live_scene::CardType& type) {
    std::vector<uint32_t> result;
    for (std::size_t piece = 0; piece < type.objects.size(); ++piece)
        for (uint32_t at = type.piece_shadow_runs[piece]; at < type.piece_shadow_runs[piece + 1];
             ++at) {
            const auto& run = type.shadow_runs[at];
            for (uint32_t corner = 0; corner < run.count; ++corner)
                result.push_back(run.first + corner);
        }
    return result;
}

/// Full and coarse types retain exact ordered shadow corners and clear-key gaps.
void caster_index_runs_tests() {
    namespace cards = oa::app::renderer_live_scene;
    namespace models = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr int32_t fixed = models::kThreeDoUnitsPerWorldUnit;
    constexpr uint8_t opaque_color = 2;
    auto source = std::make_shared<models::Model>();
    source->objects.resize(3);
    source->objects[0].first_child = 1;
    source->objects[1].parent = source->objects[2].parent = 0;
    source->objects[1].next_sibling = 2;
    for (auto& object : source->objects)
        object.vertices = {{0, 0, 0}, {fixed, 0, 0}, {fixed, fixed, 0}, {0, fixed, 0}};
    const std::vector<std::vector<uint16_t>> faces{
        {0, 1, 2}, {0, 1, 2, 3}, {0, 2, 3}, {0, 1, 2, 3}, {0, 1, 3}, {0, 1, 2, 3}
    };
    for (const auto& corners : faces) {
        auto& polygon = source->objects[0].primitives.emplace_back();
        polygon.vertex_indices = corners;
        polygon.is_colored = 1;
    }
    source->objects[0].primitives[3].is_colored = 0;
    source->objects[0].primitives[3].texture_name = "body";
    auto& final_face = source->objects[2].primitives.emplace_back();
    final_face.vertex_indices = {0, 1, 2};
    final_face.is_colored = 1;
    drawing::PreparedModel prepared;
    prepared.objects.resize(source->objects.size());
    for (std::size_t piece = 0; piece < source->objects.size(); ++piece)
        for (uint32_t face = 0; face < source->objects[piece].primitives.size(); ++face) {
            auto& polygon = prepared.objects[piece].primitives.emplace_back();
            polygon.source_index = face;
            polygon.flags = drawing::primitive_colored;
            polygon.color = opaque_color;
            if (piece == 0 && (face == 2 || face == 5))
                polygon.color = drawing::image_key;
            if (piece == 0 && face == 3) {
                polygon.flags = 0;
                // A textured primitive is retained even if this unused colour equals the key.
                polygon.color = drawing::image_key;
            }
        }
    auto instance = runtime::make_instance(source);
    instance.rebuild_transforms({});
    cards::CardTypes types;
    for (const auto detail :
         {std::optional<oa::app::body_lod::Quality>{},
          std::optional{oa::app::body_lod::Quality::low},
          std::optional{oa::app::body_lod::Quality::medium}}) {
        const auto index = cards::card_type(types, instance, prepared, detail);
        OA_CHECK(index >= 0);
        if (index < 0)
            continue;
        const auto& type = types.types[index];
        OA_CHECK(type.shadow_runs_ready);
        OA_CHECK(type.record_count == 30);
        OA_CHECK(type.piece_shadow_runs.size() == instance.pieces().size() + 1);
        OA_CHECK(cached_shadow_corners(type) == polygon_shadow_corners(type));
        OA_CHECK(type.shadow_runs.size() <= type.polygons.size());
        const std::vector<cards::CardCornerRun> expected =
            detail ? std::vector<cards::CardCornerRun>{{0, 18}, {27, 3}}
                   : std::vector<cards::CardCornerRun>{{0, 9}, {12, 9}, {27, 3}};
        const std::vector<uint32_t> offsets =
            detail ? std::vector<uint32_t>{0, 1, 1, 2} : std::vector<uint32_t>{0, 2, 2, 3};
        OA_CHECK(type.shadow_runs == expected && type.piece_shadow_runs == offsets);
    }

    // Real reduced tetrahedral and medium hulls retain their exact new corner stream.
    auto dense_source = std::make_shared<models::Model>();
    dense_source->objects.push_back(coarse_cube());
    drawing::PreparedModel dense_prepared;
    dense_prepared.objects.resize(1);
    for (uint32_t face = 0; face < dense_source->objects[0].primitives.size(); ++face) {
        auto& polygon = dense_prepared.objects[0].primitives.emplace_back();
        polygon.source_index = face;
        polygon.flags = drawing::primitive_colored;
        polygon.color = opaque_color;
    }
    auto dense_instance = runtime::make_instance(dense_source);
    dense_instance.rebuild_transforms({});
    cards::CardTypes dense_types;
    const auto dense_full = cards::card_type(dense_types, dense_instance, dense_prepared);
    const auto dense_low = cards::card_type(
        dense_types, dense_instance, dense_prepared, oa::app::body_lod::Quality::low
    );
    const auto dense_medium = cards::card_type(
        dense_types, dense_instance, dense_prepared, oa::app::body_lod::Quality::medium
    );
    OA_CHECK(dense_full >= 0 && dense_low >= 0 && dense_medium >= 0);
    if (dense_full >= 0 && dense_low >= 0 && dense_medium >= 0) {
        const auto full_corners = dense_types.types[dense_full].record_count;
        OA_CHECK(dense_types.types[dense_low].record_count < full_corners);
        OA_CHECK(dense_types.types[dense_medium].record_count < full_corners);
        for (const auto index : {dense_full, dense_low, dense_medium}) {
            const auto& type = dense_types.types[index];
            OA_CHECK(type.shadow_runs_ready);
            OA_CHECK(cached_shadow_corners(type) == polygon_shadow_corners(type));
            OA_CHECK(
                type.shadow_runs == (std::vector<cards::CardCornerRun>{{0, type.record_count}})
            );
            OA_CHECK(type.piece_shadow_runs == (std::vector<uint32_t>{0, 1}));
        }
    }

    // Adjacent pieces must stay separate so either piece can be hidden or uncached.
    cards::CardType adjacent;
    adjacent.drawable = true;
    adjacent.objects = {0, 1};
    adjacent.piece_polygons = {0, 1, 2};
    adjacent.record_count = 6;
    adjacent.polygons.resize(2);
    adjacent.polygons[0].corners = adjacent.polygons[1].corners = 3;
    adjacent.polygons[1].piece = 1;
    adjacent.polygons[1].record = 3;
    cards::prepare_shadow_runs(adjacent);
    OA_CHECK(adjacent.shadow_runs_ready);
    OA_CHECK(adjacent.shadow_runs == (std::vector<cards::CardCornerRun>{{0, 3}, {3, 3}}));
    OA_CHECK(adjacent.piece_shadow_runs == (std::vector<uint32_t>{0, 1, 2}));
    for (auto& polygon : adjacent.polygons) {
        polygon.colored = true;
        polygon.color = drawing::image_key;
    }
    cards::prepare_shadow_runs(adjacent);
    OA_CHECK(adjacent.shadow_runs_ready && adjacent.shadow_runs.empty());
    OA_CHECK(adjacent.piece_shadow_runs == (std::vector<uint32_t>{0, 0, 0}));

    cards::CardType empty;
    empty.drawable = true;
    empty.piece_polygons = {0};
    cards::prepare_shadow_runs(empty);
    OA_CHECK(empty.shadow_runs_ready && empty.shadow_runs.empty());
    OA_CHECK(empty.piece_shadow_runs == (std::vector<uint32_t>{0}));

    const auto& valid = types.types.front();
    const auto refuses = [&](auto change) {
        auto invalid = valid;
        change(invalid);
        cards::prepare_shadow_runs(invalid);
        OA_CHECK(!invalid.shadow_runs_ready);
        OA_CHECK(invalid.shadow_runs.empty() && invalid.piece_shadow_runs.empty());
    };
    refuses([](auto& type) { type.drawable = false; });
    refuses([](auto& type) { type.piece_polygons.clear(); });
    refuses([](auto& type) { type.piece_polygons.front() = 1; });
    refuses([](auto& type) { ++type.piece_polygons.back(); });
    refuses([](auto& type) { type.piece_polygons[1] = type.piece_polygons[2] + 1; });
    refuses([](auto& type) { type.polygons[0].piece = 1; });
    refuses([](auto& type) { type.polygons[0].corners = 2; });
    refuses([](auto& type) { type.polygons[0].corners = std::numeric_limits<uint32_t>::max(); });
    refuses([](auto& type) { --type.polygons[1].record; });
    refuses([](auto& type) { ++type.polygons[1].record; });
    refuses([](auto& type) { --type.record_count; });
    refuses([](auto& type) { ++type.record_count; });
    refuses([](auto& type) { type.record_count = std::numeric_limits<uint32_t>::max(); });

    // The largest representable triangle-corner span stays one bounded run.
    constexpr uint32_t corner_limit = uint32_t{1} << oa::app::renderer_live_gpu::model_corner_bits;
    constexpr uint32_t triangle_corners = 3;
    cards::CardType limit;
    limit.drawable = true;
    limit.objects = {0};
    limit.piece_polygons = {0, 1};
    limit.record_count = corner_limit - 1;
    limit.polygons.resize(1);
    limit.polygons[0].corners = limit.record_count / triangle_corners + 2;
    cards::prepare_shadow_runs(limit);
    OA_CHECK(limit.shadow_runs_ready);
    OA_CHECK(limit.shadow_runs == (std::vector<cards::CardCornerRun>{{0, corner_limit - 1}}));
    limit.record_count = corner_limit + 1;
    cards::prepare_shadow_runs(limit);
    OA_CHECK(!limit.shadow_runs_ready && limit.shadow_runs.empty());

    // Optional metadata failure must leave the original drawable topology usable.
    // Every allocation a successful preparation makes is refused in turn.
    auto counted = valid;
    ground_allocations = 0;
    ground_allocation_to_refuse = std::numeric_limits<std::size_t>::max();
    watch_ground_allocations = true;
    cards::prepare_shadow_runs(counted);
    watch_ground_allocations = false;
    const auto allocations = ground_allocations;
    OA_CHECK(counted.shadow_runs_ready && allocations != 0);
    for (std::size_t refused = 0; refused < allocations; ++refused) {
        auto allocation = valid;
        ground_allocations = refused_ground_allocations = 0;
        ground_allocation_to_refuse = refused;
        watch_ground_allocations = true;
        cards::prepare_shadow_runs(allocation);
        watch_ground_allocations = false;
        ground_allocation_to_refuse = std::numeric_limits<std::size_t>::max();
        OA_CHECK(refused_ground_allocations == 1);
        OA_CHECK(!allocation.shadow_runs_ready && allocation.drawable);
        OA_CHECK(allocation.record_count == valid.record_count);
        OA_CHECK(allocation.polygons.size() == valid.polygons.size());
        cards::prepare_shadow_runs(allocation);
        OA_CHECK(allocation.shadow_runs_ready);
        OA_CHECK(cached_shadow_corners(allocation) == polygon_shadow_corners(valid));
    }
}

} // namespace
