// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

namespace {

/// Expands each cast in linear order, independently of any prefix lookup.
std::pair<std::vector<uint32_t>, std::vector<joined_gpu::ModelRun>>
procedural_shadow_expansion(const joined_gpu::ModelDraws& draws) {
    std::pair<std::vector<uint32_t>, std::vector<joined_gpu::ModelRun>> expanded;
    OA_CHECK(draws.shadow_index_encoding == joined_gpu::ShadowIndexEncoding::procedural);
    OA_CHECK(draws.shadow_index_casts.size() == draws.casts.size());
    if (draws.shadow_index_casts.size() != draws.casts.size())
        return expanded;
    for (const auto& run : draws.shadow_index_runs) {
        const auto first = static_cast<uint32_t>(expanded.first.size());
        OA_CHECK(run.first_index == 0 && run.pattern == 0);
        OA_CHECK(run.cast_count <= joined_gpu::model_run_instances);
        for (uint32_t relative = 0; relative < run.cast_count; ++relative) {
            const auto& source = draws.shadow_index_casts[run.first_cast + relative];
            std::span<const uint32_t> words;
            if (source.pattern != 0) {
                OA_CHECK(source.pattern <= draws.shadow_patterns.size());
                if (source.pattern > draws.shadow_patterns.size())
                    return {};
                const auto& pattern = draws.shadow_patterns[source.pattern - 1];
                OA_CHECK(source.first_index == 0 && source.index_count == pattern.indices_per_cast);
                words =
                    draws.shadow_pattern_indices.subspan(pattern.first_index, source.index_count);
            } else {
                words =
                    draws.shadow_dynamic_indices.subspan(source.first_index, source.index_count);
            }
            for (const auto corner : words) {
                OA_CHECK(corner < (uint32_t{1} << joined_gpu::model_corner_bits));
                expanded.first.push_back((relative << joined_gpu::model_corner_bits) | corner);
            }
        }
        OA_CHECK(expanded.first.size() - first == run.index_count);
        expanded.second.push_back({run.vertex, first, run.index_count, run.first_cast});
    }
    OA_CHECK(expanded.first.size() == draws.logical_shadow_index_count);
    return expanded;
}

/// Compares original triangles, cast depths, surfaces, allowances and actual picks.
void check_procedural_shadow_scene(
    const char* name,
    const live::Inputs& in,
    const live::Scene& ordinary,
    const live::Scene& procedural
) {
    const auto expanded =
        procedural_shadow_expansion(live::card_draws(procedural.card_types, procedural.cards));
    auto restored = procedural;
    restored.cards.cast_indices = expanded.first;
    restored.cards.cast_runs = expanded.second;
    check_caster_index_payload(ordinary, restored);
    check_marker_frames(procedural, ordinary);
    OA_CHECK(procedural.cards.vertices == ordinary.cards.vertices);
    OA_CHECK(live::shadow_index_count(procedural.cards) == ordinary.cards.cast_indices.size());
    std::printf(
        "PROCEDURAL_SHADOW_SIGNATURE %s %016llx\n",
        name,
        static_cast<unsigned long long>(interior_cull_signature(restored, in))
    );
}

/// Mixed masks, hidden casts and unsupported buildings retain their original runs.
void shadow_index_procedural_scene_frames(uint32_t population, bool coarse) {
    namespace runtime = oa::sim::model_runtime;
    constexpr auto cached = static_cast<uint16_t>(runtime::PieceFlag::cached);
    constexpr auto visible = static_cast<uint16_t>(runtime::PieceFlag::visible);
    BodyLodFixture fixture;
    BodyLodFixture other;
    oa::UnitDef definition{};
    for (auto* instance : {&fixture.instance, &other.instance}) {
        instance->pieces()[0].flags = visible | cached;
        instance->pieces()[1].flags = visible;
    }
    auto full = fixture.instance;
    for (auto& piece : full.pieces())
        piece.flags = visible | cached;
    auto other_full = other.instance;
    for (auto& piece : other_full.pieces())
        piece.flags = visible | cached;
    auto empty = fixture.instance;
    for (auto& piece : empty.pieces())
        piece.flags = visible;
    oa::present::model::ModelState building{};
    building.image = oa::present::create_sprite(8, 8);
    building.shift = fixture.state.shift;
    building.shadow = oa::present::create_sprite(8, 8);
    building.image_unfinished = true;
    auto& battle = fixture.battle;
    battle.inputs.unit_marker_pixels = coarse ? 5 : 0;
    battle.inputs.pool = nullptr;
    std::vector<oa::Unit> units(population, fixture.unit);
    battle.draws.models.resize(population, battle.draws.models[0]);
    for (uint32_t at = 0; at < population; ++at) {
        units[at].id = static_cast<uint16_t>(at + 1);
        units[at].type_index = 1;
        units[at].position.x += static_cast<int32_t>(at % 12) * 65536;
        auto& draw = battle.draws.models[at];
        draw.model.unit = &units[at];
        draw.model.def = &definition;
        draw.model.instance = at % 2 ? &full : &fixture.instance;
        if (at % 4 >= 2) {
            draw.model.instance = at % 2 ? &other_full : &other.instance;
            draw.model.prepared = &other.prepared;
            draw.model.state = &other.state;
        }
        if (!coarse && at % 37 == 0)
            draw.model.instance = &empty;
        if (!coarse && at % 53 == 1) {
            units[at].flags = OA_UNIT_FLAG_BUILDING;
            draw.model.state = &building;
        }
    }
    live::Scene ordinary, procedural;
    for (uint32_t frame = 0; frame < 3; ++frame) {
        battle.inputs.shadow_index_templates = battle.inputs.shadow_index_procedural = false;
        OA_CHECK(live::build(battle.inputs, ordinary, battle.error));
        battle.inputs.shadow_index_templates = battle.inputs.shadow_index_procedural = true;
        OA_CHECK(live::build(battle.inputs, procedural, battle.error));
        check_procedural_shadow_scene(
            coarse ? "mixed-coarse" : "mixed-full-boundary", battle.inputs, ordinary, procedural
        );
        const auto& stream = procedural.cards.shadow_indices;
        OA_CHECK(stream.enabled && stream.procedural);
        OA_CHECK(stream.casts.size() == procedural.cards.casts.size());
        uint32_t cached_casts{}, empty_casts{};
        std::array<bool, joined_gpu::shadow_template_pattern_limit> patterns{};
        for (const auto& cast : stream.casts) {
            cached_casts += cast.pattern != 0;
            empty_casts += cast.index_count == 0;
            if (cast.pattern != 0)
                patterns[cast.pattern - 1] = true;
        }
        if (frame == 0)
            OA_CHECK(cached_casts == 0);
        else {
            OA_CHECK(cached_casts >= joined_gpu::shadow_template_minimum_casts);
            OA_CHECK(std::count(patterns.begin(), patterns.end(), true) >= 2);
            if (frame == 2) {
                std::vector<uint32_t> types;
                for (const auto& source : stream.casts)
                    if (source.pattern != 0)
                        types.push_back(stream.patterns->patterns[source.pattern - 1].type_corners);
                std::sort(types.begin(), types.end());
                OA_CHECK(std::unique(types.begin(), types.end()) - types.begin() >= 2);
            }
        }
        if (!coarse) {
            OA_CHECK(empty_casts != 0 && stream.runs.size() >= 2);
            OA_CHECK(stream.runs[0].cast_count == joined_gpu::model_run_instances);
        } else {
            OA_CHECK(procedural.body_lod_counts.body_lod_models == population);
        }
    }
}

/// Lists local rather than relatively encoded dynamic words for synthetic bands.
void procedural_shadow_band(
    live::ModelCards& cards, std::shared_ptr<const live::ShadowIndexPatterns> patterns
) {
    auto& stream = cards.shadow_indices;
    stream.enabled = stream.procedural = true;
    stream.patterns = std::move(patterns);
    stream.logical_indices = cards.cast_indices.size();
    for (size_t cast = 0; cast < cards.casts.size(); ++cast) {
        if (cast % 2 == 0 && cards.casts[cast].kind == joined_gpu::shadow_cast_picture)
            stream.casts.push_back({0, 3, 1});
        else {
            stream.casts.push_back({static_cast<uint32_t>(stream.indices.size()), 3, 0});
            stream.indices.insert(stream.indices.end(), {0, 1, 2});
        }
    }
    for (size_t at = 0; at < cards.cast_runs.size(); ++at) {
        const auto& run = cards.cast_runs[at];
        const auto end = at + 1 < cards.cast_runs.size()
                             ? cards.cast_runs[at + 1].first_instance
                             : static_cast<uint32_t>(cards.casts.size());
        stream.runs.push_back(
            {run.vertex, run.first_instance, end - run.first_instance, 0, run.index_count, 0}
        );
    }
    cards.cast_indices.clear();
    cards.cast_runs.clear();
}

/// Captures new fields as well as the existing field-safe complete packet snapshot.
std::vector<uint64_t> procedural_shadow_records(const live::ModelCards& cards) {
    auto result = joined_card_records(cards);
    result.push_back(cards.shadow_indices.procedural);
    result.push_back(cards.mark.shadow_indices.casts);
    result.push_back(cards.shadow_indices.casts.size());
    for (const auto& cast : cards.shadow_indices.casts) {
        result.push_back(cast.first_index);
        result.push_back(cast.index_count);
        result.push_back(cast.pattern);
    }
    return result;
}

/// Four-band joins, retry rollback and late refusal retain every original cast.
void shadow_index_procedural_scene_join() {
    const auto patterns = joined_shadow_patterns();
    std::array ordinary{
        joined_card_fixture(17, 100, false),
        joined_card_fixture(257, 200, false),
        joined_card_fixture(3, 500, true),
        live::ModelCards{}
    };
    auto bands = ordinary;
    for (auto& band : bands)
        procedural_shadow_band(band, patterns);
    auto expected = joined_marker_prefix();
    auto actual = expected;
    procedural_shadow_band(actual, patterns);
    for (const auto& band : ordinary)
        OA_CHECK(live::append_parallel_model_cards(expected, band, 23));
    const std::array<const live::ModelCards*, 4> sources{
        &bands[0], &bands[1], &bands[2], &bands[3]
    };
    OA_CHECK(live::append_parallel_model_card_bands(actual, sources, 23));
    auto successive = joined_marker_prefix();
    procedural_shadow_band(successive, patterns);
    for (const auto* band : sources)
        OA_CHECK(live::append_parallel_model_card_bands(successive, {&band, 1}, 23));
    OA_CHECK(procedural_shadow_records(actual) == procedural_shadow_records(successive));
    live::CardTypes types;
    types.revision = patterns->type_revision;
    const auto expanded = procedural_shadow_expansion(live::card_draws(types, actual));
    auto restored = actual;
    restored.cast_indices = expanded.first;
    restored.cast_runs = expanded.second;
    OA_CHECK(restored.cast_indices == expected.cast_indices);
    OA_CHECK(joined_shadow_triangles(restored) == joined_shadow_triangles(expected));
    OA_CHECK(joined_body_triangles(actual) == joined_body_triangles(expected));
    OA_CHECK(joined_unit_hits(actual) == joined_unit_hits(expected));
    auto invalid = bands[0];
    invalid.shadow_indices.casts.pop_back();
    const auto before = procedural_shadow_records(actual);
    const std::array<const live::ModelCards*, 2> late_bad{&bands[1], &invalid};
    ground_allocations = 0;
    watch_ground_allocations = true;
    const bool appended = live::append_parallel_model_card_bands(actual, late_bad, 0);
    watch_ground_allocations = false;
    OA_CHECK(!appended && ground_allocations == 0);
    OA_CHECK(procedural_shadow_records(actual) == before);
    const auto refuses = [&](const live::ModelCards& band) {
        const auto* source = &band;
        ground_allocations = 0;
        watch_ground_allocations = true;
        const bool appended = live::append_parallel_model_card_bands(actual, {&source, 1}, 0);
        watch_ground_allocations = false;
        OA_CHECK(!appended && ground_allocations == 0);
        OA_CHECK(procedural_shadow_records(actual) == before);
    };
    refuses(ordinary[0]);
    invalid = bands[0];
    invalid.shadow_indices.patterns = joined_shadow_patterns();
    refuses(invalid);
    invalid = bands[0];
    invalid.shadow_indices.indices[0] |= uint32_t{1} << joined_gpu::model_corner_bits;
    refuses(invalid);
    invalid = bands[0];
    invalid.instances[0].corners = 1;
    refuses(invalid);
    const auto destination = [&] {
        auto cards = joined_marker_prefix();
        procedural_shadow_band(cards, patterns);
        return cards;
    };
    auto probe = destination();
    ground_allocations = refused_ground_allocations = 0;
    watch_ground_allocations = true;
    const bool counted = live::append_parallel_model_card_bands(probe, sources, 23);
    watch_ground_allocations = false;
    const auto allocations = ground_allocations;
    OA_CHECK(counted && allocations != 0 && allocations <= 15);
    for (size_t fail = 0; fail < allocations; ++fail) {
        auto cards = destination();
        const auto unchanged = procedural_shadow_records(cards);
        ground_allocations = refused_ground_allocations = 0;
        ground_allocation_to_refuse = fail;
        bool complete{};
        watch_ground_allocations = true;
        try {
            complete = live::append_parallel_model_card_bands(cards, sources, 23);
        } catch (const std::bad_alloc&) {
        }
        watch_ground_allocations = false;
        ground_allocation_to_refuse = std::numeric_limits<size_t>::max();
        OA_CHECK(!complete && refused_ground_allocations == 1);
        OA_CHECK(procedural_shadow_records(cards) == unchanged);
    }
    live::mark_cards(actual);
    auto marked = actual;
    actual.casts.push_back(actual.casts.back());
    ++actual.cast_depths;
    actual.shadow_indices.casts.push_back(
        {static_cast<uint32_t>(actual.shadow_indices.indices.size()), 3, 0}
    );
    actual.shadow_indices.indices.insert(actual.shadow_indices.indices.end(), {0, 1, 2});
    ++actual.shadow_indices.runs.back().cast_count;
    actual.shadow_indices.runs.back().index_count += 3;
    actual.shadow_indices.logical_indices += 3;
    live::unmark_cards(actual);
    // Material retries invalidate shared-pose lookups, retaining their storage.
    OA_CHECK(!marked.poses.empty() && !marked.pose_points.empty());
    OA_CHECK(actual.poses.empty() && actual.pose_points.empty());
    marked.poses.clear();
    marked.pose_points.clear();
    ++marked.pose_epoch;
    OA_CHECK(actual.pose_epoch == marked.pose_epoch && actual.draw_instance == -1);
    OA_CHECK(procedural_shadow_records(actual) == procedural_shadow_records(marked));
}

/// Public Scene packets preserve mixed local sequences and ordinary run boundaries.
void shadow_index_procedural_scene() {
    shadow_index_procedural_scene_frames(260, false);
    shadow_index_procedural_scene_frames(32, true);
    shadow_index_procedural_scene_join();
}

} // namespace
