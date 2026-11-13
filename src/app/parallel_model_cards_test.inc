// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

namespace {

namespace joined_cards = oa::app::renderer_live_scene;
namespace joined_gpu = oa::app::renderer_live_gpu;

/// Makes ordinary model records with a different pose, texture and pick for each unit.
joined_cards::ModelCards joined_card_fixture(uint32_t count, uint16_t first_unit, bool buildings) {
    joined_cards::ModelCards cards;
    for (uint32_t index = 0; index < count; ++index) {
        const auto position = static_cast<uint32_t>(cards.positions.size() / 3);
        const float coordinate = static_cast<float>(first_unit + index);
        cards.positions.insert(
            cards.positions.end(), {coordinate, 0, 0, coordinate + 1, 0, 0, coordinate, 1, 0}
        );
        cards.piece_frames.push_back(index % 2);
        const float texture = coordinate / 131072.0F;
        cards.textures.insert(cards.textures.end(), {texture, 0.25F, texture + 0.5F, 0.75F});
        cards.slot_states.push_back(1);
        cards.tables.push_back({0, static_cast<uint8_t>(index % 2), index});
        cards.instances.push_back({0, 3, position, 3, index, 1, index, 1, 0.75F, {1, 2, 3}});
        if (index % joined_gpu::model_run_instances == 0)
            cards.runs.push_back({3, static_cast<uint32_t>(cards.indices.size()), 0, index});
        auto& run = cards.runs.back();
        const uint32_t relative = (index - run.first_instance) << joined_gpu::model_corner_bits;
        cards.indices.insert(cards.indices.end(), {relative, relative | 1, relative | 2});
        run.index_count += 3;
        joined_cards::CardUnit unit;
        unit.unit_id = static_cast<uint16_t>(first_unit + index);
        unit.vertex = 3;
        unit.instance = index;
        unit.first_index = static_cast<uint32_t>(cards.indices.size() - 3);
        unit.index_count = 3;
        unit.left = coordinate;
        unit.top = coordinate + 1;
        unit.right = coordinate + 2;
        unit.bottom = coordinate + 3;
        cards.units.push_back(unit);
        const float depth = static_cast<float>(65534U - index) / 65535.0F;
        const auto add_cast = [&](uint32_t kind) {
            const auto cast = static_cast<uint32_t>(cards.casts.size());
            cards.casts.push_back({index, kind, 4, 2, -8, 0.5F, depth, coordinate, coordinate + 3});
            if (cards.cast_runs.empty() ||
                cast - cards.cast_runs.back().first_instance >= joined_gpu::model_run_instances)
                cards.cast_runs.push_back(
                    {0, static_cast<uint32_t>(cards.cast_indices.size()), 0, cast}
                );
            auto& cast_run = cards.cast_runs.back();
            const auto named = (cast - cast_run.first_instance) << joined_gpu::model_corner_bits;
            cards.cast_indices.insert(cards.cast_indices.end(), {named, named | 1, named | 2});
            cast_run.index_count += 3;
        };
        if (buildings) {
            add_cast(joined_gpu::shadow_cast_cut);
            add_cast(joined_gpu::shadow_cast_sheared);
        } else {
            add_cast(joined_gpu::shadow_cast_picture);
        }
        ++cards.cast_depths;
        cards.vertices += 3;
    }
    return cards;
}

/// Resolves actual drawn triangle corners, rather than comparing rebased offsets.
std::vector<std::array<float, 10>>
joined_body_triangles(const joined_cards::ModelCards& cards, std::size_t vertex_offset = 0) {
    std::vector<std::array<float, 10>> result;
    constexpr uint32_t corner_mask = (uint32_t{1} << joined_gpu::model_corner_bits) - 1;
    for (const auto& run : cards.runs)
        for (std::size_t at = run.first_index; at < std::size_t{run.first_index} + run.index_count;
             ++at) {
            const auto encoded = cards.indices[at];
            const auto& instance =
                cards.instances[run.first_instance + (encoded >> joined_gpu::model_corner_bits)];
            const auto corner = encoded & corner_mask;
            const auto position = std::size_t{instance.positions + corner} * 3;
            const auto texture = std::size_t{instance.textures} * 4;
            result.push_back(
                {cards.positions[position] + instance.origin[0],
                 cards.positions[position + 1] + instance.origin[1],
                 cards.positions[position + 2] + instance.origin[2],
                 instance.brightness,
                 cards.textures[texture],
                 cards.textures[texture + 1],
                 cards.textures[texture + 2],
                 cards.textures[texture + 3],
                 static_cast<float>(cards.piece_frames[instance.pieces]),
                 static_cast<float>(run.vertex + vertex_offset)}
            );
        }
    return result;
}

/// Resolves each cast's model and its actual depth and geometry.
std::vector<std::array<float, 13>>
joined_shadow_triangles(const joined_cards::ModelCards& cards, std::size_t vertex_offset = 0) {
    std::vector<std::array<float, 13>> result;
    constexpr uint32_t corner_mask = (uint32_t{1} << joined_gpu::model_corner_bits) - 1;
    for (const auto& run : cards.cast_runs)
        for (std::size_t at = run.first_index; at < std::size_t{run.first_index} + run.index_count;
             ++at) {
            const auto encoded = cards.cast_indices[at];
            const auto& cast =
                cards.casts[run.first_instance + (encoded >> joined_gpu::model_corner_bits)];
            const auto& instance = cards.instances[cast.instance];
            const auto position = std::size_t{instance.positions + (encoded & corner_mask)} * 3;
            result.push_back(
                {cards.positions[position] + instance.origin[0],
                 cards.positions[position + 1] + instance.origin[1],
                 cards.positions[position + 2] + instance.origin[2],
                 static_cast<float>(cast.kind),
                 cast.base,
                 cast.ground_half,
                 cast.least_height,
                 cast.light,
                 cast.depth,
                 cast.across,
                 cast.down,
                 instance.brightness,
                 static_cast<float>(run.vertex + vertex_offset)}
            );
        }
    return result;
}

/// Model bands retain exact triangles, shared shadow depths and unit picks after joining.
void parallel_model_card_join() {
    auto destination = joined_card_fixture(257, 1, false);
    const auto source = joined_card_fixture(260, 300, true);
    const auto source_body = joined_body_triangles(source);
    const auto source_shadow = joined_shadow_triangles(source);
    constexpr std::size_t vertex_offset = 23;
    auto expected_body = joined_body_triangles(destination);
    const auto following_body = joined_body_triangles(source, vertex_offset);
    expected_body.insert(expected_body.end(), following_body.begin(), following_body.end());
    auto expected_shadow = joined_shadow_triangles(destination);
    auto rebased_source = source;
    for (auto& cast : rebased_source.casts)
        cast.depth = static_cast<float>(65534U - (257U + cast.instance)) / 65535.0F;
    const auto following_shadow = joined_shadow_triangles(rebased_source, vertex_offset);
    expected_shadow.insert(expected_shadow.end(), following_shadow.begin(), following_shadow.end());
    const auto source_instances = source.instances.size();
    const auto source_indices = source.indices;
    const auto original_mark = destination.mark;
    destination.draw_instance = 5;
    OA_CHECK(joined_cards::can_append_parallel_model_cards(destination, source, vertex_offset));
    OA_CHECK(joined_cards::append_parallel_model_cards(destination, source, vertex_offset));
    OA_CHECK(joined_body_triangles(destination) == expected_body);
    OA_CHECK(joined_shadow_triangles(destination) == expected_shadow);
    OA_CHECK(source.instances.size() == source_instances && source.indices == source_indices);
    OA_CHECK(joined_body_triangles(source) == source_body);
    OA_CHECK(joined_shadow_triangles(source) == source_shadow);
    OA_CHECK(destination.instances.size() == 517 && destination.cast_depths == 517);
    OA_CHECK(
        destination.draw_instance == -1 && destination.mark.instances == original_mark.instances
    );
    OA_CHECK(destination.runs.size() == 4 && destination.cast_runs.size() == 5);
    for (std::size_t index = 0; index < source.units.size(); ++index) {
        const auto& expected = source.units[index];
        const auto& actual = destination.units[257 + index];
        const auto& instance = destination.instances[actual.instance];
        OA_CHECK(actual.unit_id == expected.unit_id && actual.type == expected.type);
        OA_CHECK(actual.vertex == expected.vertex + vertex_offset);
        OA_CHECK(actual.first_index == expected.first_index + 257 * 3);
        OA_CHECK(actual.index_count == expected.index_count && actual.bounded == expected.bounded);
        OA_CHECK(actual.left == expected.left && actual.right == expected.right);
        OA_CHECK(actual.top == expected.top && actual.bottom == expected.bottom);
        OA_CHECK(destination.positions[3 * instance.positions] == expected.left);
    }

    // A serial prefix's marker fan stays at its original place.
    auto markers = joined_card_fixture(1, 600, false);
    markers.markers.resize(1);
    markers.markers[0].corners[0] = {11, 12, 13};
    markers.marker_runs.push_back({9, 0, 1});
    markers.marker_uv = {0.25F, 0.75F};
    markers.marker_first_index = static_cast<uint32_t>(markers.indices.size());
    markers.indices.insert(markers.indices.end(), {0, 1, 2, 0, 2, 3});
    auto marker_unit = markers.units[0];
    marker_unit.marker = true;
    marker_unit.instance = 0;
    marker_unit.first_index = markers.marker_first_index;
    marker_unit.index_count = 6;
    markers.units.push_back(marker_unit);
    OA_CHECK(joined_cards::append_parallel_model_cards(markers, source, 17));
    OA_CHECK(markers.marker_first_index == 3 && markers.marker_uv == (std::array{0.25F, 0.75F}));
    OA_CHECK(
        markers.markers.size() == 1 &&
        markers.markers[0].corners[0] == (std::array{11.0F, 12.0F, 13.0F})
    );
    OA_CHECK(markers.marker_runs.size() == 1 && markers.marker_runs[0].vertex == 9);
    OA_CHECK(markers.units[1].marker && markers.units[1].first_index == 3);

    // Empty and shadow-only bands are valid; they need no dummy body run.
    joined_cards::ModelCards empty;
    OA_CHECK(joined_cards::append_parallel_model_cards(empty, {}, 0));
    auto shadow_only = joined_card_fixture(1, 700, false);
    shadow_only.indices.clear();
    shadow_only.runs.clear();
    shadow_only.units.clear();
    shadow_only.vertices = 0;
    OA_CHECK(joined_cards::append_parallel_model_cards(empty, shadow_only, 0));
    OA_CHECK(empty.instances.size() == 1 && empty.casts.size() == 1 && empty.units.empty());

    // Every structural rejection leaves both inputs' records unchanged.
    auto checked = joined_card_fixture(1, 800, false);
    const auto checked_body = joined_body_triangles(checked);
    const auto checked_shadow = joined_shadow_triangles(checked);
    const auto refuses = [&](const joined_cards::ModelCards& invalid, std::size_t offset = 0) {
        OA_CHECK(!joined_cards::append_parallel_model_cards(checked, invalid, offset));
        OA_CHECK(checked.instances.size() == 1 && checked.casts.size() == 1);
        OA_CHECK(joined_body_triangles(checked) == checked_body);
        OA_CHECK(joined_shadow_triangles(checked) == checked_shadow);
    };
    refuses(markers);
    auto invalid = joined_card_fixture(1, 900, false);
    invalid.instances[0].positions = std::numeric_limits<uint32_t>::max();
    refuses(invalid);
    invalid = joined_card_fixture(1, 900, false);
    invalid.indices[0] = joined_gpu::model_run_instances << joined_gpu::model_corner_bits;
    refuses(invalid);
    invalid = joined_card_fixture(1, 900, false);
    invalid.indices[0] = 3;
    refuses(invalid);
    invalid = joined_card_fixture(1, 900, false);
    invalid.textures.pop_back();
    refuses(invalid);
    invalid = joined_card_fixture(1, 900, false);
    invalid.casts[0].depth = std::nextafter(invalid.casts[0].depth, 0.0F);
    refuses(invalid);
    invalid = joined_card_fixture(1, 900, false);
    refuses(invalid, std::numeric_limits<std::size_t>::max());
    OA_CHECK(!joined_cards::append_parallel_model_cards(checked, checked, 0));
    auto full = joined_card_fixture(32767, 1, false);
    OA_CHECK(!joined_cards::append_parallel_model_cards(full, checked, 0));
    checked.vertices = joined_gpu::largest_vertex_count;
    OA_CHECK(!joined_cards::append_parallel_model_cards(checked, invalid, 0));

    // A worker and its join use the same mode, including directed rounding.
    const int original_mode = std::fegetround();
    for (const int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        OA_CHECK(std::fesetround(mode) == 0);
        auto prior = joined_card_fixture(3, 1, false);
        const auto following = joined_card_fixture(3, 5, true);
        OA_CHECK(joined_cards::append_parallel_model_cards(prior, following, 0));
        for (std::size_t index = 3; index < prior.casts.size(); ++index) {
            const auto& cast = prior.casts[index];
            const float expected = static_cast<float>(65534U - cast.instance) / 65535.0F;
            OA_CHECK(cast.depth == expected);
        }
    }
    OA_CHECK(std::fesetround(original_mode) == 0);
}

} // namespace
