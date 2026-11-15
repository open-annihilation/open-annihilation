// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

namespace {

/// Requires identical legacy triangles, physical surfaces and command picks.
void check_shadow_template_frame(const live::Scene& ordinary, const live::Scene& compact) {
    const auto draws = live::card_draws(compact.card_types, compact.cards);
    OA_CHECK(
        draws.shadow_index_encoding == oa::app::renderer_live_gpu::ShadowIndexEncoding::compact
    );
    OA_CHECK(compact.cards.cast_indices.empty() && compact.cards.cast_runs.empty());
    const auto expanded = expanded_shadow_templates(draws);
    auto restored = compact;
    restored.cards.cast_indices = expanded.first;
    restored.cards.cast_runs = expanded.second;
    check_caster_index_payload(ordinary, restored);
    check_marker_frames(compact, ordinary);
    OA_CHECK(live::shadow_index_count(compact.cards) == ordinary.cards.cast_indices.size());
    OA_CHECK(compact.cards.vertices == ordinary.cards.vertices);
    OA_CHECK(compact.cards.cast_depths == ordinary.cards.cast_depths);
    OA_CHECK(compact.out_of_room == ordinary.out_of_room);
    OA_CHECK(compact.body_lod_counts.shadow_vertices == ordinary.body_lod_counts.shadow_vertices);
}

/// Places every shared unit inside the same picture without changing its model.
void shadow_template_population(ShadowViewFixture& fixture) {
    fixture.source->objects[0].primitives.resize(4);
    fixture.prepared.objects[0].primitives.resize(4);
    fixture.instance.pieces()[0].flags =
        static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible) |
        static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::cached);
    fixture.inputs.unit_marker_pixels = 0;
    fixture.inputs.body_lod_enabled = true;
    for (auto& unit : fixture.units)
        unit.position = {
            (fixture.view_origin + 24) * fixture.fixed,
            0,
            (fixture.view_origin + 32) * fixture.fixed
        };
}

/// Cold, short and complete ordinary runs retain their exact logical budgets.
void shadow_index_template_runs() {
    namespace gpu = oa::app::renderer_live_gpu;
    for (const uint32_t population : {15U, 16U, 256U, 260U}) {
        ShadowViewFixture fixture(population);
        shadow_template_population(fixture);
        live::Scene ordinary, compact;
        std::string error;
        std::shared_ptr<const live::ShadowIndexPatterns> first_snapshot;
        for (unsigned frame = 0; frame < 3; ++frame) {
            fixture.inputs.shadow_index_templates = false;
            OA_CHECK(live::build(fixture.inputs, ordinary, error));
            fixture.inputs.shadow_index_templates = true;
            OA_CHECK(live::build(fixture.inputs, compact, error));
            OA_CHECK(error.empty());
            check_shadow_template_frame(ordinary, compact);
            const auto& stream = compact.cards.shadow_indices;
            OA_CHECK(stream.runs.size() == (population + 255) / 256);
            uint32_t cached{}, dynamic{};
            for (const auto& run : stream.runs) {
                OA_CHECK(run.cast_count <= gpu::model_run_instances);
                (run.pattern ? cached : dynamic) += run.cast_count;
            }
            if (frame == 0) {
                OA_CHECK(cached == 0 && dynamic == population);
                first_snapshot = stream.patterns;
            } else {
                const auto expected = population < gpu::shadow_template_minimum_casts
                                          ? 0U
                                          : std::min(population, gpu::model_run_instances);
                OA_CHECK(cached == expected && dynamic == population - expected);
            }
            if (first_snapshot) {
                OA_CHECK(first_snapshot->patterns.empty());
                OA_CHECK(first_snapshot->indices.empty());
            }
        }
    }
}

/// A later differing mask flushes a tentative warm run without changing its order.
void shadow_index_template_mixed_masks() {
    constexpr uint32_t population = 16;
    ShadowViewFixture fixture(population);
    shadow_template_population(fixture);
    live::Scene ordinary, compact;
    std::string error;
    const auto compare = [&] {
        fixture.inputs.shadow_index_templates = false;
        OA_CHECK(live::build(fixture.inputs, ordinary, error));
        fixture.inputs.shadow_index_templates = true;
        OA_CHECK(live::build(fixture.inputs, compact, error));
        check_shadow_template_frame(ordinary, compact);
    };
    compare();
    compare();
    OA_CHECK(compact.cards.shadow_indices.runs.size() == 1);
    OA_CHECK(compact.cards.shadow_indices.runs[0].pattern != 0);
    auto moving = fixture.instance;
    moving.pieces()[0].flags = static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible);
    fixture.many.models.back().model.instance = &moving;
    compare();
    OA_CHECK(compact.cards.shadow_indices.runs.size() == 1);
    OA_CHECK(compact.cards.shadow_indices.runs[0].pattern == 0);
    OA_CHECK(compact.cards.shadow_indices.indices == ordinary.cards.cast_indices);
    fixture.many.models.back().model.instance = &fixture.instance;
    compare();
    OA_CHECK(compact.cards.shadow_indices.runs[0].pattern != 0);
}

/// Coarse selection, cached pieces and clear building walls retain ordinary policy.
void shadow_index_template_piece_policy() {
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr auto visible = static_cast<uint16_t>(runtime::PieceFlag::visible);
    constexpr auto cached = static_cast<uint16_t>(runtime::PieceFlag::cached);
    for (const float scale : {0.5F, 0.8F, 2.0F}) {
        BodyLodFixture fixture(8);
        oa::UnitDef definition{};
        auto& battle = fixture.battle;
        fixture.unit.type_index = 1;
        battle.draws.models[0].model.def = &definition;
        fixture.projection(scale);
        for (auto& piece : fixture.instance.pieces())
            piece.flags = visible | cached;
        fixture.instance.pieces()[1].flags = visible;
        std::vector<oa::Unit> units(16, fixture.unit);
        battle.draws.models.resize(units.size(), battle.draws.models[0]);
        for (size_t at = 0; at < units.size(); ++at) {
            units[at].id = static_cast<uint16_t>(at + 1);
            battle.draws.models[at].model.unit = &units[at];
        }
        live::Scene ordinary, compact;
        const auto compare = [&] {
            battle.inputs.shadow_index_templates = false;
            OA_CHECK(live::build(battle.inputs, ordinary, battle.error));
            battle.inputs.shadow_index_templates = true;
            OA_CHECK(live::build(battle.inputs, compact, battle.error));
            check_shadow_template_frame(ordinary, compact);
        };
        compare();
        compare();
        OA_CHECK(compact.body_lod_counts.body_lod_models == (scale < 1 ? 16 : 0));
        OA_CHECK(compact.body_lod_counts.full_mesh_models == (scale < 1 ? 0 : 16));
        OA_CHECK(compact.cards.shadow_indices.runs.size() == 1);
        OA_CHECK(!ordinary.cards.cast_indices.empty());
        OA_CHECK(ordinary.cards.cast_indices.size() % units.size() == 0);
        const auto corners_per_cast = ordinary.cards.cast_indices.size() / units.size();
        const auto pattern_bytes =
            corners_per_cast * oa::app::renderer_live_gpu::model_run_instances * sizeof(uint32_t);
        if (scale < 1) {
            OA_CHECK(pattern_bytes <= oa::app::renderer_live_gpu::shadow_template_admission_bytes);
            OA_CHECK(compact.cards.shadow_indices.runs[0].pattern != 0);
        } else {
            size_t source_corners{};
            for (size_t piece = 0; piece < fixture.source->objects.size(); ++piece)
                if ((fixture.instance.pieces()[piece].flags & cached) != 0)
                    for (const auto& face : fixture.source->objects[piece].primitives)
                        source_corners += (face.vertex_indices.size() - 2) * 3;
            OA_CHECK(source_corners == 2016 && corners_per_cast == source_corners);
            OA_CHECK(pattern_bytes > oa::app::renderer_live_gpu::shadow_template_admission_bytes);
            OA_CHECK(pattern_bytes <= oa::app::renderer_live_gpu::shadow_template_byte_limit);
            OA_CHECK(compact.cards.shadow_indices.runs[0].pattern == 0);
            OA_CHECK(compact.cards.shadow_indices.indices == ordinary.cards.cast_indices);
        }
        // A cut and its cached silhouette use different image-key rules.
        for (auto& unit : units)
            unit.flags = OA_UNIT_FLAG_BUILDING;
        fixture.state.shadow = oa::present::create_sprite(8, 8);
        fixture.state.image_unfinished = true;
        fixture.prepared.objects[0].primitives[0].color = drawing::image_key;
        fixture.prepared.objects[1].primitives[0].color = drawing::image_key;
        ordinary = {};
        compact = {};
        compare();
        compare();
        OA_CHECK(compact.cards.casts.size() == 32);
        for (const auto& run : compact.cards.shadow_indices.runs)
            OA_CHECK(run.pattern == 0);
        for (size_t at = 0; at + 1 < compact.cards.casts.size(); at += 2) {
            OA_CHECK(compact.cards.casts[at].kind == oa::app::renderer_live_gpu::shadow_cast_cut);
            OA_CHECK(
                compact.cards.casts[at + 1].kind == oa::app::renderer_live_gpu::shadow_cast_sheared
            );
            OA_CHECK(compact.cards.casts[at].depth == compact.cards.casts[at + 1].depth);
        }
    }
}

/// Material retries restore a warm pending run, even after it became dynamic.
void shadow_index_template_lifecycle() {
    auto cards = joined_card_fixture(16, 100, false);
    const auto original_words = cards.cast_indices;
    joined_compact_shadow_indices(cards, joined_shadow_patterns());
    live::remember_shadow_index_key(cards.shadow_indices, {23, 0, 1, false});
    live::mark_cards(cards);
    auto expected = cards;
    OA_CHECK(live::materialize_shadow_index_run(cards.shadow_indices, 0));
    OA_CHECK(cards.shadow_indices.indices == original_words);
    OA_CHECK(cards.shadow_indices.runs[0].pattern == 0);
    live::remember_shadow_index_key(cards.shadow_indices, {23, 0, 3, false});
    cards.casts.push_back(cards.casts.back());
    ++cards.cast_depths;
    cards.shadow_indices.runs.push_back({12, 16, 1, 48, 3, 0});
    cards.shadow_indices.indices.insert(cards.shadow_indices.indices.end(), {0, 1, 2});
    cards.shadow_indices.logical_indices += 3;
    live::unmark_cards(cards);
    // Retrying a material also discards the ordinary pose lookup epoch.
    OA_CHECK(cards.pose_epoch == expected.pose_epoch + 1);
    expected.pose_epoch = cards.pose_epoch;
    OA_CHECK(joined_card_records(cards) == joined_card_records(expected));
    OA_CHECK(cards.shadow_indices.runs[0].pattern == 1);
    OA_CHECK(cards.shadow_indices.indices.empty());
    OA_CHECK(live::materialize_shadow_index_run(cards.shadow_indices, 0));
    const auto already_dynamic = joined_card_records(cards);
    OA_CHECK(live::materialize_shadow_index_run(cards.shadow_indices, 0));
    OA_CHECK(joined_card_records(cards) == already_dynamic);
    OA_CHECK(!live::materialize_shadow_index_run(cards.shadow_indices, 1));
    OA_CHECK(joined_card_records(cards) == already_dynamic);

    auto refused = expected;
    const auto before = joined_card_records(refused);
    ground_allocations = refused_ground_allocations = 0;
    ground_allocation_to_refuse = 0;
    bool materialized{};
    watch_ground_allocations = true;
    try {
        materialized = live::materialize_shadow_index_run(refused.shadow_indices, 0);
    } catch (const std::bad_alloc&) {
    }
    watch_ground_allocations = false;
    ground_allocation_to_refuse = std::numeric_limits<size_t>::max();
    OA_CHECK(!materialized && refused_ground_allocations == 1);
    OA_CHECK(joined_card_records(refused) == before);
    live::clear_cards(cards);
    OA_CHECK(cards.shadow_indices.indices.empty() && cards.shadow_indices.runs.empty());
    OA_CHECK(
        cards.shadow_indices.logical_indices == 0 && cards.shadow_indices.candidate_count == 0
    );
}

/// The fixed candidate list deduplicates keys without allocating on worker threads.
void shadow_index_template_candidates() {
    live::ShadowIndexStream stream;
    stream.enabled = true;
    ground_allocations = refused_ground_allocations = 0;
    watch_ground_allocations = true;
    for (uint32_t at = 0; at < oa::app::renderer_live_gpu::shadow_template_pattern_limit + 3;
         ++at) {
        const live::ShadowIndexKey key{23, at, 1, false};
        live::remember_shadow_index_key(stream, key);
        live::remember_shadow_index_key(stream, key);
    }
    watch_ground_allocations = false;
    OA_CHECK(ground_allocations == 0);
    OA_CHECK(stream.candidate_count == oa::app::renderer_live_gpu::shadow_template_pattern_limit);
    for (uint32_t at = 0; at < stream.candidate_count; ++at)
        OA_CHECK((stream.candidates[at] == live::ShadowIndexKey{23, at, 1, false}));
}

/// Copied registries append under separate revisions without changing retained prefixes.
void shadow_index_template_registry_copies() {
    BodyLodFixture fixture;
    fixture.projection(2);
    OA_CHECK(live::build(fixture.battle.inputs, fixture.battle.scene, fixture.battle.error));
    auto& types = fixture.battle.scene.card_types;
    OA_CHECK(types.draw_types.size() == 1 && types.draw_types[0] >= 0);
    const auto type = static_cast<uint32_t>(types.draw_types[0]);
    const live::ShadowIndexKey base_key{types.revision, type, 1, false};
    live::admit_shadow_index_patterns(types, {&base_key, 1});
    const auto original = types.shadow_patterns;
    OA_CHECK(original && original->keys.size() == 1);
    if (!original)
        return;
    const auto original_indices = original->indices;
    OA_CHECK(original->patterns.size() == 1 && !original_indices.empty());
    auto left = types;
    auto right = types;
    live::prepare_shadow_index_patterns(left);
    live::prepare_shadow_index_patterns(right);
    OA_CHECK(left.shadow_patterns && right.shadow_patterns);
    if (!left.shadow_patterns || !right.shadow_patterns)
        return;
    OA_CHECK(left.shadow_patterns->revision != 0 && right.shadow_patterns->revision != 0);
    OA_CHECK(left.shadow_patterns->revision != original->revision);
    OA_CHECK(right.shadow_patterns->revision != original->revision);
    OA_CHECK(left.shadow_patterns->revision != right.shadow_patterns->revision);
    OA_CHECK(left.shadow_patterns->indices == original_indices);
    OA_CHECK(right.shadow_patterns->indices == original_indices);
    const live::ShadowIndexKey left_key{types.revision, type, 3, false};
    const live::ShadowIndexKey right_key{types.revision, type, 1, true};
    live::admit_shadow_index_patterns(left, {&left_key, 1});
    live::admit_shadow_index_patterns(right, {&right_key, 1});
    OA_CHECK((left.shadow_patterns->keys == std::vector<live::ShadowIndexKey>{base_key, left_key}));
    OA_CHECK(
        (right.shadow_patterns->keys == std::vector<live::ShadowIndexKey>{base_key, right_key})
    );
    OA_CHECK((original->keys == std::vector<live::ShadowIndexKey>{base_key}));
    OA_CHECK(original->indices == original_indices);
    OA_CHECK(left.shadow_patterns->indices.size() >= original_indices.size());
    OA_CHECK(right.shadow_patterns->indices.size() >= original_indices.size());
    if (left.shadow_patterns->indices.size() < original_indices.size() ||
        right.shadow_patterns->indices.size() < original_indices.size())
        return;
    OA_CHECK(
        std::equal(
            original_indices.begin(), original_indices.end(), left.shadow_patterns->indices.begin()
        )
    );
    OA_CHECK(
        std::equal(
            original_indices.begin(), original_indices.end(), right.shadow_patterns->indices.begin()
        )
    );
}

} // namespace
