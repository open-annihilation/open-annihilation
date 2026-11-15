// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

namespace {

namespace joined_cards = oa::app::renderer_live_scene;
namespace joined_gpu = oa::app::renderer_live_gpu;
constexpr size_t joined_batch_vertex_base = 23;

/// Expands retained patterns independently of the shadow producer and backend.
/// @param draws the submitted dynamic words, immutable patterns and ordered runs
/// @return the original ordered shadow words and ordinary run ranges
std::pair<std::vector<uint32_t>, std::vector<oa::app::renderer_live_gpu::ModelRun>>
expanded_shadow_templates(const oa::app::renderer_live_gpu::ModelDraws& draws) {
    namespace gpu = oa::app::renderer_live_gpu;
    std::pair<std::vector<uint32_t>, std::vector<gpu::ModelRun>> result;
    if (draws.shadow_index_encoding == gpu::ShadowIndexEncoding::expanded) {
        result.first.assign(draws.cast_indices.begin(), draws.cast_indices.end());
        result.second.assign(draws.cast_runs.begin(), draws.cast_runs.end());
        return result;
    }
    for (const auto& run : draws.shadow_index_runs) {
        OA_CHECK(run.cast_count != 0 && run.cast_count <= gpu::model_run_instances);
        const auto first = static_cast<uint32_t>(result.first.size());
        if (run.pattern == 0) {
            OA_CHECK(run.first_index <= draws.shadow_dynamic_indices.size());
            OA_CHECK(run.index_count <= draws.shadow_dynamic_indices.size() - run.first_index);
            if (run.first_index > draws.shadow_dynamic_indices.size() ||
                run.index_count > draws.shadow_dynamic_indices.size() - run.first_index)
                return {};
            const auto words =
                draws.shadow_dynamic_indices.subspan(run.first_index, run.index_count);
            result.first.insert(result.first.end(), words.begin(), words.end());
        } else {
            OA_CHECK(run.pattern <= draws.shadow_patterns.size());
            if (run.pattern > draws.shadow_patterns.size())
                return {};
            const auto& pattern = draws.shadow_patterns[run.pattern - 1];
            OA_CHECK(pattern.indices_per_cast != 0 && pattern.indices_per_cast % 3 == 0);
            OA_CHECK(uint64_t{pattern.indices_per_cast} * run.cast_count == run.index_count);
            OA_CHECK(run.first_index == 0);
            OA_CHECK(pattern.first_index <= draws.shadow_pattern_indices.size());
            OA_CHECK(run.index_count <= draws.shadow_pattern_indices.size() - pattern.first_index);
            if (pattern.first_index > draws.shadow_pattern_indices.size() ||
                run.index_count > draws.shadow_pattern_indices.size() - pattern.first_index)
                return {};
            const auto words =
                draws.shadow_pattern_indices.subspan(pattern.first_index, run.index_count);
            result.first.insert(result.first.end(), words.begin(), words.end());
        }
        result.second.push_back({run.vertex, first, run.index_count, run.first_cast});
    }
    OA_CHECK(result.first.size() == draws.logical_shadow_index_count);
    return result;
}

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

/// Captures every owned record and cache field, excluding vector capacities and padding.
std::vector<uint64_t> joined_card_records(const joined_cards::ModelCards& cards) {
    std::vector<uint64_t> result;
    const auto word = [&](uint64_t value) { result.push_back(value); };
    const auto number = [&](float value) { word(std::bit_cast<uint32_t>(value)); };
    const auto words = [&](const auto& values) {
        word(values.size());
        for (const auto value : values)
            word(value);
    };
    const auto numbers = [&](const auto& values) {
        word(values.size());
        for (const auto value : values)
            number(value);
    };
    word(cards.instances.size());
    for (const auto& instance : cards.instances) {
        for (const auto value : std::array{
                 instance.corners,
                 instance.corner_count,
                 instance.positions,
                 instance.position_count,
                 instance.pieces,
                 instance.piece_count,
                 instance.textures,
                 instance.texture_count
             })
            word(value);
        number(instance.brightness);
        numbers(instance.origin);
    }
    numbers(cards.positions);
    words(cards.piece_frames);
    numbers(cards.textures);
    words(cards.slot_states);
    word(cards.tables.size());
    for (const auto& table : cards.tables) {
        word(table.type);
        word(table.team);
        word(table.first);
    }
    words(cards.indices);
    const auto runs = [&](const auto& values) {
        word(values.size());
        for (const auto& run : values) {
            word(run.vertex);
            word(run.first_index);
            word(run.index_count);
            word(run.first_instance);
        }
    };
    runs(cards.runs);
    word(cards.units.size());
    for (const auto& unit : cards.units) {
        word(unit.unit_id);
        word(unit.vertex);
        word(unit.instance);
        word(unit.type);
        word(unit.first_index);
        word(unit.index_count);
        for (const auto value : std::array{unit.left, unit.top, unit.right, unit.bottom})
            number(value);
        word(unit.bounded);
        word(unit.marker);
        word(unit.body_lod);
    }
    word(cards.markers.size());
    for (const auto& marker : cards.markers) {
        for (const auto& corner : marker.corners)
            numbers(corner);
        number(marker.brightness);
        numbers(marker.color);
    }
    word(cards.marker_runs.size());
    for (const auto& run : cards.marker_runs) {
        word(run.vertex);
        word(run.first_instance);
        word(run.instance_count);
    }
    numbers(cards.marker_uv);
    word(cards.marker_first_index);
    word(cards.vertices);
    word(cards.casts.size());
    for (const auto& cast : cards.casts) {
        word(cast.instance);
        word(cast.kind);
        for (const auto value : std::array{
                 cast.base,
                 cast.ground_half,
                 cast.least_height,
                 cast.light,
                 cast.depth,
                 cast.across,
                 cast.down
             })
            number(value);
    }
    words(cards.cast_indices);
    runs(cards.cast_runs);
    const auto shadow_run = [&](const joined_gpu::ShadowIndexRun& run) {
        word(run.vertex);
        word(run.first_cast);
        word(run.cast_count);
        word(run.first_index);
        word(run.index_count);
        word(run.pattern);
    };
    const auto& compact = cards.shadow_indices;
    word(compact.enabled);
    word(compact.patterns != nullptr);
    if (compact.patterns) {
        const auto& patterns = *compact.patterns;
        word(patterns.revision);
        word(patterns.type_revision);
        word(patterns.keys.size());
        for (const auto& key : patterns.keys) {
            word(key.type_revision);
            word(key.type);
            word(key.selected_mask);
            word(key.walls);
        }
        word(patterns.patterns.size());
        for (const auto& pattern : patterns.patterns) {
            word(pattern.first_index);
            word(pattern.indices_per_cast);
            word(pattern.type_corners);
            word(pattern.type_corner_count);
            word(pattern.selected_mask);
        }
        words(patterns.indices);
    }
    words(compact.indices);
    word(compact.runs.size());
    for (const auto& run : compact.runs)
        shadow_run(run);
    word(compact.logical_indices);
    word(compact.candidate_count);
    for (uint32_t at = 0; at < compact.candidate_count; ++at) {
        const auto& key = compact.candidates[at];
        word(key.type_revision);
        word(key.type);
        word(key.selected_mask);
        word(key.walls);
    }
    word(cards.cast_depths);
    for (const auto value :
         std::array{cards.shadow_map.left, cards.shadow_map.top, cards.shadow_map.scale})
        number(value);
    word(cards.shadow_map.width);
    word(cards.shadow_map.height);
    word(cards.shadow_vertex);
    word(cards.shadow_ground);
    word(static_cast<uint64_t>(cards.draw_instance));
    const auto& mark = cards.mark;
    for (const auto value : std::array<uint64_t, 14>{
             mark.instances,
             mark.positions,
             mark.piece_frames,
             mark.indices,
             mark.runs,
             mark.units,
             mark.casts,
             mark.cast_indices,
             mark.cast_runs,
             mark.cast_depths,
             mark.vertices,
             mark.markers,
             mark.marker_runs,
             mark.marker_first_index
         })
        word(value);
    numbers(mark.marker_uv);
    word(mark.shadow_indices.indices);
    word(mark.shadow_indices.runs);
    word(mark.shadow_indices.logical_indices);
    word(mark.shadow_indices.candidates);
    shadow_run(mark.shadow_indices.last_run);
    word(cards.poses.size());
    for (const auto& pose : cards.poses) {
        word(pose.type);
        word(pose.first_position);
        word(pose.first_point);
        word(pose.point_count);
    }
    word(cards.pose_points.size());
    for (const auto& point : cards.pose_points) {
        word(static_cast<uint32_t>(point.x));
        word(static_cast<uint32_t>(point.y));
        word(static_cast<uint32_t>(point.z));
    }
    word(cards.pose_slots.size());
    for (const auto& slot : cards.pose_slots) {
        word(slot.digest);
        word(slot.pose);
        word(slot.epoch);
    }
    word(cards.recent_poses.size());
    for (const auto& recent : cards.recent_poses) {
        word(recent.pose);
        word(recent.epoch);
    }
    word(cards.pose_epoch);
    return result;
}

/// Exercises command-hit interpolation on each actual model triangle and pick identity.
std::vector<std::array<float, 6>> joined_unit_hits(const joined_cards::ModelCards& cards) {
    std::vector<std::array<float, 6>> result;
    for (const auto& unit : cards.units) {
        std::array<std::array<float, 3>, 3> triangle{};
        if (unit.marker) {
            for (size_t corner = 0; corner < triangle.size(); ++corner)
                triangle[corner] =
                    cards.markers[unit.instance].corners[cards.indices[unit.first_index + corner]];
        } else {
            const auto run =
                std::find_if(cards.runs.begin(), cards.runs.end(), [&](const auto& candidate) {
                    return unit.first_index >= candidate.first_index &&
                           unit.first_index + 3 <= candidate.first_index + candidate.index_count;
                });
            OA_CHECK(run != cards.runs.end());
            if (run == cards.runs.end())
                continue;
            constexpr uint32_t mask = (uint32_t{1} << joined_gpu::model_corner_bits) - 1;
            for (size_t corner = 0; corner < triangle.size(); ++corner) {
                const auto encoded = cards.indices[unit.first_index + corner];
                const auto index = run->first_instance + (encoded >> joined_gpu::model_corner_bits);
                OA_CHECK(index == unit.instance);
                const auto& instance = cards.instances[index];
                for (size_t axis = 0; axis < triangle[corner].size(); ++axis)
                    triangle[corner][axis] =
                        cards.positions[(instance.positions + (encoded & mask)) * 3 + axis] +
                        instance.origin[axis];
            }
        }
        const float x = triangle[0][0] * 0.5F + triangle[1][0] * 0.25F + triangle[2][0] * 0.25F;
        const float y = triangle[0][1] * 0.5F + triangle[1][1] * 0.25F + triangle[2][1] * 0.25F;
        std::array<float, 3> weights{};
        float depth{};
        const bool hit =
            oa::app::renderer_live_camera::triangle_hit(triangle, x, y, weights, depth);
        OA_CHECK(hit);
        result.push_back(
            {float(unit.unit_id), float(hit), depth, weights[0], weights[1], weights[2]}
        );
    }
    return result;
}

/// Makes a serial prefix with a real marker and lookup caches that joins must preserve.
joined_cards::ModelCards joined_marker_prefix() {
    auto prefix = joined_card_fixture(2, 10, false);
    prefix.markers.resize(1);
    prefix.markers[0].corners = {{{11, 12, 13}, {12, 12, 13}, {12, 13, 13}, {11, 13, 13}}};
    prefix.marker_runs.push_back({9, 0, 1});
    prefix.marker_uv = {0.25F, 0.75F};
    prefix.marker_first_index = static_cast<uint32_t>(prefix.indices.size());
    prefix.indices.insert(prefix.indices.end(), {0, 1, 2, 0, 2, 3});
    auto marker = prefix.units[0];
    marker.marker = true;
    marker.instance = 0;
    marker.unit_id = 99;
    marker.vertex = 9;
    marker.left = 11;
    marker.top = 12;
    marker.right = 12;
    marker.bottom = 13;
    marker.first_index = prefix.marker_first_index;
    marker.index_count = 6;
    prefix.units.push_back(marker);
    prefix.vertices += 6;
    prefix.draw_instance = 1;
    prefix.mark.instances = 1;
    prefix.mark.marker_first_index = 77;
    prefix.mark.marker_uv = {0.1F, 0.9F};
    prefix.poses.push_back({0, 0, 0, 3});
    prefix.pose_points.push_back({7, 8, 9});
    prefix.pose_slots.push_back({37, 0, 1});
    prefix.recent_poses.push_back({0, 1});
    prefix.pose_epoch = 1;
    return prefix;
}

/// Batch validation, rebasing and reservation preserve the sequential join's exact records.
void parallel_model_card_batch_join() {
    const auto prefix = joined_marker_prefix();
    const auto first = joined_card_fixture(257, 100, false);
    const auto second = joined_card_fixture(260, 400, true);
    joined_cards::ModelCards empty;
    auto shadow_only = joined_card_fixture(1, 800, false);
    shadow_only.indices.clear();
    shadow_only.runs.clear();
    shadow_only.units.clear();
    shadow_only.vertices = 0;
    const std::array<const joined_cards::ModelCards*, 4> bands{
        &first, &empty, &second, &shadow_only
    };
    const auto first_before = joined_card_records(first);
    const auto second_before = joined_card_records(second);
    const auto shadow_before = joined_card_records(shadow_only);
    constexpr size_t vertex_base = joined_batch_vertex_base;
    const auto compare = [&](const auto& initial, const auto& sources) {
        auto sequential = initial;
        for (const auto* band : sources)
            OA_CHECK(joined_cards::append_parallel_model_cards(sequential, *band, vertex_base));
        auto batch = initial;
        OA_CHECK(joined_cards::append_parallel_model_card_bands(batch, sources, vertex_base));
        OA_CHECK(joined_card_records(batch) == joined_card_records(sequential));
        OA_CHECK(joined_body_triangles(batch) == joined_body_triangles(sequential));
        OA_CHECK(joined_shadow_triangles(batch) == joined_shadow_triangles(sequential));
        OA_CHECK(joined_unit_hits(batch) == joined_unit_hits(sequential));
    };
    compare(prefix, bands);
    compare(
        prefix, std::array<const joined_cards::ModelCards*, 4>{&first, &first, &empty, &shadow_only}
    );
    compare(prefix, std::array<const joined_cards::ModelCards*, 2>{&empty, &empty});
    auto no_bands = prefix;
    OA_CHECK(joined_cards::append_parallel_model_card_bands(no_bands, {}, vertex_base));
    OA_CHECK(joined_card_records(no_bands) == joined_card_records(prefix));
    OA_CHECK(joined_card_records(first) == first_before);
    OA_CHECK(joined_card_records(second) == second_before);
    OA_CHECK(joined_card_records(shadow_only) == shadow_before);

    // A bad later band must be rejected before any reservation or earlier append.
    const auto refuses =
        [&](auto& destination, const auto& sources, size_t base = joined_batch_vertex_base) {
            const auto before = joined_card_records(destination);
            ground_allocations = refused_ground_allocations = 0;
            ground_allocation_to_refuse = std::numeric_limits<size_t>::max();
            watch_ground_allocations = true;
            const bool appended =
                joined_cards::append_parallel_model_card_bands(destination, sources, base);
            watch_ground_allocations = false;
            OA_CHECK(!appended && ground_allocations == 0);
            OA_CHECK(joined_card_records(destination) == before);
        };
    auto checked = prefix;
    refuses(checked, std::array<const joined_cards::ModelCards*, 2>{&first, nullptr});
    refuses(checked, std::array<const joined_cards::ModelCards*, 2>{&first, &checked});
    refuses(checked, std::array<const joined_cards::ModelCards*, 2>{&first, &prefix});
    refuses(
        checked,
        std::array<const joined_cards::ModelCards*, 5>{&empty, &empty, &empty, &empty, &empty}
    );
    auto invalid = joined_card_fixture(1, 900, false);
    invalid.units[0].first_index = 9;
    refuses(checked, std::array<const joined_cards::ModelCards*, 2>{&first, &invalid});
    invalid = joined_card_fixture(1, 900, false);
    invalid.casts[0].depth = std::nextafter(invalid.casts[0].depth, 0.0F);
    refuses(checked, std::array<const joined_cards::ModelCards*, 2>{&first, &invalid});
    invalid = joined_card_fixture(1, 900, false);
    invalid.indices[0] = 3;
    refuses(checked, std::array<const joined_cards::ModelCards*, 2>{&first, &invalid});
    invalid = joined_card_fixture(1, 900, false);
    refuses(
        checked,
        std::array<const joined_cards::ModelCards*, 2>{&first, &invalid},
        std::numeric_limits<size_t>::max()
    );

    // Each source fits separately. Their cumulative logical vertices do not.
    const auto one = joined_card_fixture(1, 1000, false);
    const std::array<const joined_cards::ModelCards*, 3> three{&one, &one, &one};
    auto vertex_full = joined_card_fixture(1, 1, false);
    vertex_full.vertices = joined_gpu::largest_vertex_count - 6;
    OA_CHECK(joined_cards::can_append_parallel_model_cards(vertex_full, one, 0));
    refuses(vertex_full, three, 0);
    {
        auto instance_full =
            joined_card_fixture(joined_gpu::largest_model_instance_count - 1, 1, false);
        OA_CHECK(joined_cards::can_append_parallel_model_cards(instance_full, one, 0));
        refuses(instance_full, three, 0);
    }
    {
        auto depth_full = joined_card_fixture(1, 1, false);
        constexpr uint32_t depth_count = 64999;
        depth_full.casts.resize(depth_count, depth_full.casts[0]);
        depth_full.cast_runs.clear();
        depth_full.cast_indices.clear();
        depth_full.cast_depths = depth_count;
        for (uint32_t index = 0; index < depth_count; ++index)
            depth_full.casts[index].depth = static_cast<float>(65534U - index) / 65535.0F;
        OA_CHECK(joined_cards::can_append_parallel_model_cards(depth_full, one, 0));
        refuses(depth_full, std::array<const joined_cards::ModelCards*, 2>{&one, &one}, 0);
    }

    // Probe every allocation in a successful join. Failed reserve operations
    // may grow capacities, but no owned size, record, marker or cache may change.
    auto counted = prefix;
    ground_allocations = 0;
    ground_allocation_to_refuse = std::numeric_limits<size_t>::max();
    watch_ground_allocations = true;
    const bool counted_success =
        joined_cards::append_parallel_model_card_bands(counted, bands, vertex_base);
    watch_ground_allocations = false;
    const auto allocations = ground_allocations;
    constexpr size_t maximum_reservations = 12;
    OA_CHECK(counted_success && allocations != 0 && allocations <= maximum_reservations);
    for (size_t fail = 0; fail < std::min(allocations, maximum_reservations); ++fail) {
        auto candidate = prefix;
        const auto before = joined_card_records(candidate);
        ground_allocations = refused_ground_allocations = 0;
        ground_allocation_to_refuse = fail;
        bool appended{};
        watch_ground_allocations = true;
        try {
            appended =
                joined_cards::append_parallel_model_card_bands(candidate, bands, vertex_base);
        } catch (const std::bad_alloc&) {
        } catch (...) {
            watch_ground_allocations = false;
            throw;
        }
        watch_ground_allocations = false;
        OA_CHECK(!appended && refused_ground_allocations == 1);
        OA_CHECK(joined_card_records(candidate) == before);
    }
    ground_allocation_to_refuse = std::numeric_limits<size_t>::max();
    const int original_mode = std::fegetround();
    for (const int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        OA_CHECK(std::fesetround(mode) == 0);
        const auto rounded_prefix = joined_marker_prefix();
        const auto rounded_first = joined_card_fixture(257, 100, false);
        const auto rounded_second = joined_card_fixture(260, 400, true);
        auto rounded_shadow = joined_card_fixture(1, 800, false);
        rounded_shadow.indices.clear();
        rounded_shadow.runs.clear();
        rounded_shadow.units.clear();
        rounded_shadow.vertices = 0;
        compare(
            rounded_prefix,
            std::array<const joined_cards::ModelCards*, 4>{
                &rounded_first, &empty, &rounded_second, &rounded_shadow
            }
        );
        OA_CHECK(std::fegetround() == mode);
    }
    OA_CHECK(std::fesetround(original_mode) == 0);
}

/// Makes one exact three-corner pattern for all relative cast names.
std::shared_ptr<const joined_cards::ShadowIndexPatterns> joined_shadow_patterns() {
    auto patterns = std::make_shared<joined_cards::ShadowIndexPatterns>();
    patterns->revision = 17;
    patterns->type_revision = 23;
    patterns->keys.push_back({23, 0, 1, false});
    patterns->patterns.push_back({0, 3, 0, 3, 1});
    for (uint32_t cast = 0; cast < joined_gpu::model_run_instances; ++cast)
        for (uint32_t corner = 0; corner < 3; ++corner)
            patterns->indices.push_back((cast << joined_gpu::model_corner_bits) | corner);
    return patterns;
}

/// Retains ordinary run boundaries while storing warm whole runs once.
void joined_compact_shadow_indices(
    joined_cards::ModelCards& cards,
    std::shared_ptr<const joined_cards::ShadowIndexPatterns> patterns
) {
    auto& stream = cards.shadow_indices;
    stream.enabled = true;
    stream.patterns = std::move(patterns);
    stream.logical_indices = cards.cast_indices.size();
    for (size_t at = 0; at < cards.cast_runs.size(); ++at) {
        const auto& run = cards.cast_runs[at];
        const auto end = at + 1 < cards.cast_runs.size()
                             ? cards.cast_runs[at + 1].first_instance
                             : static_cast<uint32_t>(cards.casts.size());
        const auto count = end - run.first_instance;
        const bool retained = stream.patterns && count >= joined_gpu::shadow_template_minimum_casts;
        const auto first = retained ? 0U : static_cast<uint32_t>(stream.indices.size());
        if (!retained)
            stream.indices.insert(
                stream.indices.end(),
                cards.cast_indices.begin() + run.first_index,
                cards.cast_indices.begin() + run.first_index + run.index_count
            );
        stream.runs.push_back(
            {run.vertex, run.first_instance, count, first, run.index_count, retained ? 1U : 0U}
        );
    }
    cards.cast_indices.clear();
    cards.cast_runs.clear();
}

/// Batch joins preserve compact IDs, dynamic offsets and cast depths exactly.
void parallel_model_card_compact_join() {
    const auto patterns = joined_shadow_patterns();
    std::array ordinary{
        joined_card_fixture(16, 100, false),
        joined_card_fixture(257, 200, false),
        joined_card_fixture(3, 500, true),
        joined_cards::ModelCards{}
    };
    auto compact = ordinary;
    for (auto& cards : compact)
        joined_compact_shadow_indices(cards, patterns);
    auto expected = joined_marker_prefix();
    auto actual = expected;
    joined_compact_shadow_indices(actual, patterns);
    for (const auto& band : ordinary)
        OA_CHECK(
            joined_cards::append_parallel_model_cards(expected, band, joined_batch_vertex_base)
        );
    const std::array<const joined_cards::ModelCards*, 4> bands{
        &compact[0], &compact[1], &compact[2], &compact[3]
    };
    OA_CHECK(
        joined_cards::append_parallel_model_card_bands(actual, bands, joined_batch_vertex_base)
    );
    auto successive = joined_marker_prefix();
    joined_compact_shadow_indices(successive, patterns);
    for (const auto* band : bands)
        OA_CHECK(
            joined_cards::append_parallel_model_card_bands(
                successive, {&band, 1}, joined_batch_vertex_base
            )
        );
    OA_CHECK(joined_card_records(actual) == joined_card_records(successive));
    joined_cards::CardTypes types;
    types.revision = patterns->type_revision;
    const auto draws = joined_cards::card_draws(types, actual);
    const auto expanded = expanded_shadow_templates(draws);
    auto restored = actual;
    restored.cast_indices = expanded.first;
    restored.cast_runs = expanded.second;
    OA_CHECK(restored.cast_indices == expected.cast_indices);
    OA_CHECK(joined_shadow_triangles(restored) == joined_shadow_triangles(expected));
    OA_CHECK(joined_body_triangles(actual) == joined_body_triangles(expected));
    OA_CHECK(joined_unit_hits(actual) == joined_unit_hits(expected));
    OA_CHECK(actual.vertices == expected.vertices && actual.cast_depths == expected.cast_depths);
    OA_CHECK(joined_cards::shadow_index_count(actual) == expected.cast_indices.size());
    const auto before = joined_card_records(actual);
    const auto refuses = [&](const joined_cards::ModelCards& band) {
        const std::array<const joined_cards::ModelCards*, 2> invalid{&compact[0], &band};
        ground_allocations = refused_ground_allocations = 0;
        watch_ground_allocations = true;
        const bool appended = joined_cards::append_parallel_model_card_bands(actual, invalid, 0);
        watch_ground_allocations = false;
        OA_CHECK(!appended && ground_allocations == 0);
        OA_CHECK(joined_card_records(actual) == before);
    };
    refuses(ordinary[0]);
    auto foreign = compact[0];
    foreign.shadow_indices.patterns = joined_shadow_patterns();
    refuses(foreign);
    foreign = compact[0];
    foreign.shadow_indices.patterns.reset();
    refuses(foreign);
    foreign = compact[0];
    foreign.shadow_indices.runs[0].pattern = 2;
    refuses(foreign);
    foreign = compact[0];
    ++foreign.shadow_indices.logical_indices;
    refuses(foreign);
    foreign = compact[1];
    foreign.shadow_indices.indices[0] = joined_gpu::model_run_instances
                                        << joined_gpu::model_corner_bits;
    refuses(foreign);
    OA_CHECK(!joined_cards::can_append_parallel_model_cards(actual, compact[0], 0));
    OA_CHECK(!joined_cards::append_parallel_model_cards(actual, compact[0], 0));
    OA_CHECK(joined_card_records(actual) == before);
    const auto make_destination = [&] {
        auto destination = joined_marker_prefix();
        joined_compact_shadow_indices(destination, patterns);
        return destination;
    };
    auto allocation_probe = make_destination();
    ground_allocations = refused_ground_allocations = 0;
    watch_ground_allocations = true;
    const bool appended = joined_cards::append_parallel_model_card_bands(
        allocation_probe, bands, joined_batch_vertex_base
    );
    watch_ground_allocations = false;
    const auto allocations = ground_allocations;
    OA_CHECK(appended && allocations != 0 && allocations <= 14);
    for (size_t refused_at = 0; refused_at < allocations; ++refused_at) {
        auto destination = make_destination();
        const auto unchanged = joined_card_records(destination);
        ground_allocations = refused_ground_allocations = 0;
        ground_allocation_to_refuse = refused_at;
        bool completed{};
        watch_ground_allocations = true;
        try {
            completed = joined_cards::append_parallel_model_card_bands(
                destination, bands, joined_batch_vertex_base
            );
        } catch (const std::bad_alloc&) {
        }
        watch_ground_allocations = false;
        ground_allocation_to_refuse = std::numeric_limits<size_t>::max();
        OA_CHECK(!completed && refused_ground_allocations == 1);
        OA_CHECK(joined_card_records(destination) == unchanged);
    }
    // Allocation failure can leave a dynamic-only frame without a snapshot.
    auto dynamic = joined_card_fixture(3, 900, false);
    auto dynamic_expected = joined_card_fixture(1, 1, false);
    OA_CHECK(joined_cards::append_parallel_model_cards(dynamic_expected, dynamic, 0));
    joined_compact_shadow_indices(dynamic, {});
    auto dynamic_actual = joined_card_fixture(1, 1, false);
    joined_compact_shadow_indices(dynamic_actual, {});
    const joined_cards::ModelCards* dynamic_band = &dynamic;
    OA_CHECK(joined_cards::append_parallel_model_card_bands(dynamic_actual, {&dynamic_band, 1}, 0));
    const auto dynamic_words =
        expanded_shadow_templates(joined_cards::card_draws(types, dynamic_actual));
    OA_CHECK(dynamic_words.first == dynamic_expected.cast_indices);
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
    parallel_model_card_batch_join();
    parallel_model_card_compact_join();
}

} // namespace
