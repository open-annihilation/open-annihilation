// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

namespace {

/// Coarse cards keep type identity, material corners and current selected points.
void body_lod_card_tests() {
    namespace cards = oa::app::renderer_live_scene;
    namespace body = oa::app::body_lod;
    namespace geometry = oa::formats::objects3d;
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr int32_t fixed = geometry::kThreeDoUnitsPerWorldUnit;
    auto source = std::make_shared<geometry::Model>();
    source->objects.resize(2);
    auto& root = source->objects[0];
    root.first_child = 1;
    root.vertices = {
        {-fixed, -fixed, -fixed},
        {fixed, -fixed, -fixed},
        {-fixed, fixed, -fixed},
        {fixed, fixed, -fixed},
        {-fixed, -fixed, fixed},
        {fixed, -fixed, fixed},
        {-fixed, fixed, fixed},
        {fixed, fixed, fixed},
        {200 * fixed, 200 * fixed, 200 * fixed}
    };
    constexpr std::array<std::array<uint16_t, 4>, 6> cube{
        {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}}
    };
    for (const auto& face : cube) {
        auto& polygon = root.primitives.emplace_back();
        polygon.vertex_indices.assign(face.begin(), face.end());
        polygon.is_colored = 1;
    }
    auto& child = source->objects[1];
    child.parent = 0;
    child.offset_from_parent.y = 6 * fixed;
    child.vertices = {{0, 0, 0}, {fixed, 0, 0}, {0, fixed, 0}, {0, 0, fixed}};
    constexpr std::array<std::array<uint16_t, 3>, 4> tetra{
        {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}}
    };
    for (const auto& face : tetra) {
        auto& polygon = child.primitives.emplace_back();
        polygon.vertex_indices.assign(face.begin(), face.end());
        polygon.is_colored = 1;
    }
    drawing::PreparedModel prepared;
    prepared.objects.resize(source->objects.size());
    for (std::size_t object = 0; object < source->objects.size(); ++object)
        for (uint32_t face = 0; face < source->objects[object].primitives.size(); ++face) {
            auto& polygon = prepared.objects[object].primitives.emplace_back();
            polygon.source_index = face;
            polygon.flags = drawing::primitive_colored;
            polygon.color = 1;
        }
    auto instance = runtime::make_instance(source);
    instance.rebuild_transforms({});
    cards::CardTypes coarse_only;
    OA_CHECK(cards::card_type(coarse_only, instance, prepared, body::Quality::low) >= 0);
    OA_CHECK(!cards::cached_card_type(coarse_only, instance, prepared));
    OA_CHECK(cards::card_type(coarse_only, instance, prepared) >= 0);
    OA_CHECK(cards::cached_card_type(coarse_only, instance, prepared));
    cards::CardTypes types;
    const auto complete_index = cards::card_type(types, instance, prepared);
    const auto low_index = cards::card_type(types, instance, prepared, body::Quality::low);
    const auto medium_index = cards::card_type(types, instance, prepared, body::Quality::medium);
    OA_CHECK(complete_index >= 0 && low_index >= 0 && medium_index >= 0);
    if (complete_index < 0 || low_index < 0 || medium_index < 0)
        return;
    OA_CHECK(complete_index != low_index && low_index != medium_index);
    OA_CHECK(cards::card_type(types, instance, prepared) == complete_index);
    OA_CHECK(cards::card_type(types, instance, prepared, body::Quality::low) == low_index);
    OA_CHECK(cards::cached_card_type(types, instance, prepared));
    const auto& complete = types.types[complete_index];
    const auto& low = types.types[low_index];
    const auto& medium = types.types[medium_index];
    OA_CHECK(!complete.body_detail && complete.source_vertices.empty());
    OA_CHECK(low.body_detail == body::Quality::low && medium.body_detail == body::Quality::medium);
    OA_CHECK(low.record_count < complete.record_count);
    OA_CHECK(low.piece_vertices.back() < complete.piece_vertices.back());
    OA_CHECK(medium.record_count <= complete.record_count);
    OA_CHECK(medium.piece_vertices.back() < complete.piece_vertices.back());
    for (const auto* type : {&low, &medium}) {
        OA_CHECK(type->source_vertices.size() == type->piece_vertices.back());
        OA_CHECK(type->source_vertex_counts == (std::vector<uint32_t>{9, 4}));
        for (uint32_t at = 0; at < type->piece_vertices[1]; ++at)
            OA_CHECK(type->source_vertices[at] != 8);
    }
    cards::ModelCards frame;
    cards::clear_cards(frame);
    const auto first = cards::card_positions(frame, low_index, low, instance.pieces(), {});
    OA_CHECK(first == 0);
    const auto first_positions = frame.positions;
    OA_CHECK(frame.positions.size() == low.piece_vertices.back() * 3);
    for (std::size_t piece = 0; piece < instance.pieces().size(); ++piece)
        for (std::size_t at = low.piece_vertices[piece]; at < low.piece_vertices[piece + 1]; ++at) {
            const auto& point =
                instance.pieces()[piece].transformed_vertices[low.source_vertices[at]];
            OA_CHECK(frame.positions[at * 3] == point.x / float{fixed});
            OA_CHECK(frame.positions[at * 3 + 1] == point.y / float{fixed});
            OA_CHECK(frame.positions[at * 3 + 2] == -(point.z / float{fixed}));
        }
    for (uint32_t copy = 0; copy < 100; ++copy)
        OA_CHECK(cards::card_positions(frame, low_index, low, instance.pieces(), {}) == first);
    OA_CHECK(frame.positions == first_positions);
    // Unreferenced source points neither consume upload room nor split a shared pose.
    ++instance.pieces()[0].transformed_vertices[8].x;
    OA_CHECK(cards::card_positions(frame, low_index, low, instance.pieces(), {}) == first);
    // A real moving piece must change the compact pose, with exact fixed values.
    const auto chosen = low.source_vertices[0];
    instance.pieces()[0].transformed_vertices[chosen].y += fixed / 7;
    const auto moved = cards::card_positions(frame, low_index, low, instance.pieces(), {});
    OA_CHECK(moved == static_cast<int64_t>(low.piece_vertices.back()));
    OA_CHECK(
        frame.positions[moved * 3 + 1] ==
        instance.pieces()[0].transformed_vertices[chosen].y / float{fixed}
    );
    std::vector<std::vector<geometry::FixedVector3>> shown{
        instance.pieces()[0].transformed_vertices
    };
    shown[0][chosen].x += fixed / 11;
    const auto interpolated =
        cards::card_positions(frame, low_index, low, instance.pieces(), shown);
    OA_CHECK(interpolated > moved);
    OA_CHECK(frame.positions[interpolated * 3] == shown[0][chosen].x / float{fixed});
    const auto size = frame.positions.size();
    shown[0].pop_back();
    OA_CHECK(cards::card_positions(frame, low_index, low, instance.pieces(), shown) == -1);
    OA_CHECK(frame.positions.size() == size);
    auto invalid = low;
    invalid.source_vertices[0] = std::numeric_limits<uint16_t>::max();
    OA_CHECK(cards::card_positions(frame, low_index, invalid, instance.pieces(), {}) == -1);
    OA_CHECK(frame.positions.size() == size);
    // A fallback quad's second triangle keeps UV corners 0,2,3 rather than 0,1,2.
    for (const auto& polygon : medium.polygons) {
        const auto object = medium.objects[polygon.piece];
        const auto& original = source->objects[object].primitives[polygon.primitive].vertex_indices;
        const auto start = medium.piece_vertices[polygon.piece];
        for (std::size_t corner = 0; corner < polygon.body_vertices.size(); ++corner) {
            const auto original_vertex =
                medium.source_vertices[start + polygon.body_vertices[corner]];
            const auto source_corner = std::find(original.begin(), original.end(), original_vertex);
            OA_CHECK(source_corner != original.end());
            const auto record = (medium.first_record + polygon.record + corner) * 2;
            const auto actual =
                types.records[record + 1] >> oa::app::renderer_live_gpu::model_polygon_corner_shift;
            OA_CHECK(actual == static_cast<uint32_t>(source_corner - original.begin()));
        }
    }
    // Piece visibility and running-frame flags are per-instance, never type keys.
    instance.pieces()[1].flags = 0;
    OA_CHECK(cards::card_type(types, instance, prepared, body::Quality::low) == low_index);
}

} // namespace
