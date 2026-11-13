// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

namespace {

/// Requires identical shadow placement, instance naming and ordered source corners.
/// @param ordinary the complete reference emitter's retained payload
/// @param enhanced the optimized emitter's retained payload
void check_caster_index_payload(const live::Scene& ordinary, const live::Scene& enhanced) {
    OA_CHECK(enhanced.cards.cast_indices == ordinary.cards.cast_indices);
    OA_CHECK(enhanced.cards.casts.size() == ordinary.cards.casts.size());
    OA_CHECK(enhanced.cards.positions == ordinary.cards.positions);
    OA_CHECK(enhanced.cards.piece_frames == ordinary.cards.piece_frames);
    OA_CHECK(enhanced.cards.cast_depths == ordinary.cards.cast_depths);
    OA_CHECK(enhanced.shadow_count == ordinary.shadow_count);
    OA_CHECK(enhanced.cards.cast_runs.size() == ordinary.cards.cast_runs.size());
    for (std::size_t at = 0;
         at < std::min(ordinary.cards.casts.size(), enhanced.cards.casts.size());
         ++at) {
        const auto& expected = ordinary.cards.casts[at];
        const auto& actual = enhanced.cards.casts[at];
        OA_CHECK(
            std::tie(
                expected.instance,
                expected.kind,
                expected.base,
                expected.ground_half,
                expected.least_height,
                expected.light,
                expected.depth,
                expected.across,
                expected.down
            ) ==
            std::tie(
                actual.instance,
                actual.kind,
                actual.base,
                actual.ground_half,
                actual.least_height,
                actual.light,
                actual.depth,
                actual.across,
                actual.down
            )
        );
    }
    for (std::size_t at = 0;
         at < std::min(ordinary.cards.cast_runs.size(), enhanced.cards.cast_runs.size());
         ++at) {
        const auto& expected = ordinary.cards.cast_runs[at];
        const auto& actual = enhanced.cards.cast_runs[at];
        OA_CHECK(
            std::tie(
                expected.vertex, expected.first_index, expected.index_count, expected.first_instance
            ) ==
            std::tie(actual.vertex, actual.first_index, actual.index_count, actual.first_instance)
        );
    }
    OA_CHECK(enhanced.cards.instances.size() == ordinary.cards.instances.size());
    for (std::size_t at = 0;
         at < std::min(ordinary.cards.instances.size(), enhanced.cards.instances.size());
         ++at) {
        const auto& expected = ordinary.cards.instances[at];
        const auto& actual = enhanced.cards.instances[at];
        OA_CHECK(
            std::tie(
                expected.corners,
                expected.corner_count,
                expected.positions,
                expected.position_count,
                expected.pieces,
                expected.piece_count,
                expected.textures,
                expected.texture_count,
                expected.brightness,
                expected.origin
            ) ==
            std::tie(
                actual.corners,
                actual.corner_count,
                actual.positions,
                actual.position_count,
                actual.pieces,
                actual.piece_count,
                actual.textures,
                actual.texture_count,
                actual.brightness,
                actual.origin
            )
        );
    }
    OA_CHECK(enhanced.body_lod_counts.shadow_vertices == ordinary.body_lod_counts.shadow_vertices);
}

/// Compares complete caster payloads with physical detail selection disabled.
/// @param inputs the same immutable world and presentation for both emitters
/// @param index_count the source faces that the casting policy includes
/// @param cast_count the ordinary picture, cut and sheared records expected
/// @return the Enhanced frame for additional policy checks
live::Scene
caster_index_frame(const live::Inputs& inputs, std::size_t index_count, std::size_t cast_count) {
    OA_CHECK(inputs.unit_marker_pixels == 0);
    auto ordinary_inputs = inputs;
    ordinary_inputs.body_lod_enabled = false;
    auto enhanced_inputs = inputs;
    enhanced_inputs.body_lod_enabled = true;
    live::Scene ordinary, enhanced;
    std::string ordinary_error, enhanced_error;
    OA_CHECK(live::build(ordinary_inputs, ordinary, ordinary_error));
    OA_CHECK(live::build(enhanced_inputs, enhanced, enhanced_error));
    OA_CHECK(ordinary_error.empty() && enhanced_error.empty());
    OA_CHECK(enhanced.card_types.draw_types.size() == inputs.draws->models.size());
    for (const auto type_index : enhanced.card_types.draw_types) {
        OA_CHECK(type_index >= 0);
        if (type_index < 0)
            continue;
        const auto& type = enhanced.card_types.types[static_cast<std::size_t>(type_index)];
        OA_CHECK(type.shadow_runs_ready);
        OA_CHECK(type.piece_shadow_runs.size() == type.objects.size() + 1);
    }
    OA_CHECK(ordinary.cards.cast_indices.size() == index_count);
    OA_CHECK(ordinary.cards.casts.size() == cast_count);
    OA_CHECK(enhanced.body_lod_counts.body_lod_models == 0);
    check_caster_index_payload(ordinary, enhanced);
    return enhanced;
}

/// Cached, moving and hidden pieces keep the same ordered source corners.
void caster_index_piece_policy() {
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr auto visible = static_cast<uint16_t>(runtime::PieceFlag::visible);
    constexpr auto cached = static_cast<uint16_t>(runtime::PieceFlag::cached);
    BodyLodFixture fixture;
    auto& battle = fixture.battle;
    battle.inputs.unit_marker_pixels = 0;
    const auto corners = [&](std::size_t piece) {
        std::size_t count{};
        for (const auto& face : fixture.source->objects[piece].primitives)
            count += (face.vertex_indices.size() - 2) * 3;
        return count;
    };
    const auto root = corners(0), child = corners(1);
    fixture.instance.pieces()[0].flags = visible | cached;
    fixture.instance.pieces()[1].flags = visible;
    (void)caster_index_frame(battle.inputs, root, 1);
    fixture.instance.pieces()[1].flags = visible | cached;
    (void)caster_index_frame(battle.inputs, root + child, 1);
    fixture.instance.pieces()[0].flags = cached;
    (void)caster_index_frame(battle.inputs, child, 1);
    fixture.instance.pieces()[0].flags = visible;
    fixture.instance.pieces()[1].flags = 0;
    (void)caster_index_frame(battle.inputs, 0, 1);
    fixture.instance.pieces()[1].flags = visible;
    fixture.state.image_unfinished = true;
    (void)caster_index_frame(battle.inputs, root + child, 1);
    fixture.state.image_unfinished = false;
    fixture.unit.build_remaining = 0.5F;
    (void)caster_index_frame(battle.inputs, root + child, 1);
    fixture.unit.build_remaining = 0;
    fixture.instance.pieces()[0].flags = visible | cached;
    fixture.instance.pieces()[1].flags = visible | cached;
    for (const auto level : {drawing::shadow_full_level, drawing::shadow_full_level / 2}) {
        battle.draws.shadow_level = level;
        const auto frame = caster_index_frame(battle.inputs, root + child, 1);
        if (frame.cards.casts.size() == 1)
            OA_CHECK(
                frame.cards.casts[0].light == 1.0F - 0.5F * level / drawing::shadow_full_level
            );
    }
    // A missing surface texture never removes the same face from its shadow.
    fixture.prepared.objects[0].primitives[0].flags = 0;
    (void)caster_index_frame(battle.inputs, root + child, 1);
}

/// Building cuts omit clear walls while the cached sheared cast retains them.
void caster_index_building_walls() {
    namespace drawing = oa::present::model;
    namespace runtime = oa::sim::model_runtime;
    constexpr auto visible = static_cast<uint16_t>(runtime::PieceFlag::visible);
    constexpr auto cached = static_cast<uint16_t>(runtime::PieceFlag::cached);
    BodyLodFixture fixture;
    auto& battle = fixture.battle;
    battle.inputs.unit_marker_pixels = 0;
    fixture.unit.flags = OA_UNIT_FLAG_BUILDING;
    fixture.state.shadow = oa::present::create_sprite(8, 8);
    fixture.state.image_unfinished = true;
    fixture.instance.pieces()[0].flags = visible | cached;
    fixture.instance.pieces()[1].flags = visible;
    fixture.prepared.objects[0].primitives[0].color = drawing::image_key;
    fixture.prepared.objects[1].primitives[0].color = drawing::image_key;
    const auto root = fixture.source->objects[0].primitives.size() * 3;
    const auto child = fixture.source->objects[1].primitives.size() * 3;
    const auto frame = caster_index_frame(battle.inputs, root + child - 6 + root, 2);
    if (frame.cards.casts.size() == 2) {
        OA_CHECK(frame.cards.casts[0].kind == oa::app::renderer_live_gpu::shadow_cast_cut);
        OA_CHECK(frame.cards.casts[1].kind == oa::app::renderer_live_gpu::shadow_cast_sheared);
        OA_CHECK(frame.cards.casts[0].depth == frame.cards.casts[1].depth);
        OA_CHECK(frame.cards.casts[0].light == 1 && frame.cards.casts[1].light == 0.5F);
    }
}

/// The next shadow run restarts instance naming without changing corner order.
void caster_index_run_boundary() {
    constexpr uint32_t population = oa::app::renderer_live_gpu::model_run_instances + 4;
    ShadowViewFixture fixture(population);
    fixture.inputs.unit_marker_pixels = 0;
    fixture.instance.pieces()[0].flags =
        static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible) |
        static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::cached);
    const auto indices = std::size_t{population} * fixture.source->objects[0].primitives.size() * 6;
    const auto frame = caster_index_frame(fixture.inputs, indices, population);
    OA_CHECK(frame.cards.cast_runs.size() == 2);
    if (frame.cards.cast_runs.size() == 2) {
        OA_CHECK(frame.cards.cast_runs[0].first_instance == 0);
        OA_CHECK(
            frame.cards.cast_runs[1].first_instance ==
            oa::app::renderer_live_gpu::model_run_instances
        );
    }
}

/// Exhausted fast-path capacity retains the ordinary scene outcome and index stream.
void caster_index_capacity_fallback() {
    constexpr uint32_t population = 12000;
    constexpr auto capacity = oa::app::renderer_live_gpu::largest_vertex_count;
    BodyLodFixture fixture;
    auto& battle = fixture.battle;
    battle.inputs.unit_marker_pixels = 0;
    // Pan the camera only: every full caster still reaches the fixed world crop.
    battle.inputs.clip_projection[3] += 512;
    fixture.instance.pieces()[0].flags =
        static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible) |
        static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::cached);
    fixture.instance.pieces()[1].flags =
        static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible);
    std::vector<oa::Unit> units(population, fixture.unit);
    battle.draws.models.resize(population, battle.draws.models[0]);
    for (uint32_t index = 0; index < population; ++index) {
        units[index].id = static_cast<uint16_t>(index + 1);
        battle.draws.models[index].model.unit = &units[index];
    }
    const auto expected =
        std::size_t{population} * fixture.source->objects[0].primitives.size() * 3;
    OA_CHECK(expected > capacity);
    OA_CHECK(population < oa::app::renderer_live_gpu::largest_model_instance_count);
    live::Scene ordinary, enhanced;
    std::string ordinary_error, enhanced_error;
    battle.inputs.body_lod_enabled = false;
    const bool ordinary_built = live::build(battle.inputs, ordinary, ordinary_error);
    battle.inputs.body_lod_enabled = true;
    const bool enhanced_built = live::build(battle.inputs, enhanced, enhanced_error);
    OA_CHECK(ordinary_built == enhanced_built);
    OA_CHECK(ordinary_error == enhanced_error && ordinary.out_of_room == enhanced.out_of_room);
    OA_CHECK(enhanced.card_types.draw_types.size() == population);
    if (!enhanced.card_types.draw_types.empty()) {
        const auto type = enhanced.card_types.draw_types[0];
        OA_CHECK(type >= 0);
        if (type >= 0)
            OA_CHECK(enhanced.card_types.types[static_cast<std::size_t>(type)].shadow_runs_ready);
    }
    if (ordinary_built && enhanced_built) {
        OA_CHECK(ordinary.cards.cast_indices.size() == expected);
        OA_CHECK(enhanced.cards.cast_indices == ordinary.cards.cast_indices);
        OA_CHECK(enhanced.cards.positions == ordinary.cards.positions);
        OA_CHECK(ordinary.cards.casts.size() == population);
        OA_CHECK(enhanced.cards.casts.size() == ordinary.cards.casts.size());
        OA_CHECK(enhanced.cards.cast_depths == ordinary.cards.cast_depths);
        OA_CHECK(enhanced.shadow_count == ordinary.shadow_count);
        OA_CHECK(ordinary.cards.units.empty() && enhanced.cards.units.empty());
    } else if (!ordinary_built && !enhanced_built) {
        // A later geometry check may reject the same oversized legacy frame.
        OA_CHECK(ordinary.out_of_room && !ordinary_error.empty());
        OA_CHECK(ordinary_error.find("geometry budget") != std::string::npos);
        OA_CHECK(ordinary.cards.cast_indices.empty() && enhanced.cards.cast_indices.empty());
        OA_CHECK(
            ordinary.passes.models.cards.cast_indices == enhanced.passes.models.cards.cast_indices
        );
        OA_CHECK(ordinary.model_count == 0 && enhanced.model_count == 0);
        OA_CHECK(ordinary.shadow_count == 0 && enhanced.shadow_count == 0);
    }
}

/// Reduced bodies keep identical shadow streams when only the emitter cache differs.
void caster_index_coarse_equivalence() {
    namespace runtime = oa::sim::model_runtime;
    constexpr auto visible = static_cast<uint16_t>(runtime::PieceFlag::visible);
    constexpr auto cached = static_cast<uint16_t>(runtime::PieceFlag::cached);
    for (const auto [scale, expected] :
         std::array{std::pair{0.5F, uint8_t{1}}, std::pair{0.8F, uint8_t{2}}}) {
        BodyLodFixture fixture(8);
        auto& battle = fixture.battle;
        for (auto& piece : fixture.instance.pieces())
            piece.flags = visible | cached;
        fixture.instance.pieces()[1].flags = visible;
        fixture.projection(scale);
        live::Scene ordinary, enhanced;
        OA_CHECK(live::build(battle.inputs, ordinary, battle.error));
        // This optional cache changes only the index emitter. Both frames
        // retain Enhanced Mode and the same animated physical body quality.
        for (auto& type : ordinary.card_types.types)
            type.shadow_runs_ready = false;
        OA_CHECK(live::build(battle.inputs, ordinary, battle.error));
        OA_CHECK(live::build(battle.inputs, enhanced, battle.error));
        OA_CHECK(ordinary.unit_detail.size() == 1 && enhanced.unit_detail.size() == 1);
        if (ordinary.unit_detail.size() != 1 || enhanced.unit_detail.size() != 1)
            continue;
        OA_CHECK(ordinary.unit_detail[0].body_lod == expected);
        OA_CHECK(enhanced.unit_detail[0].body_lod == expected);
        OA_CHECK(
            ordinary.unit_detail[0].body_piece_mask == enhanced.unit_detail[0].body_piece_mask
        );
        OA_CHECK(
            ordinary.unit_detail[0].body_medium_piece_mask ==
            enhanced.unit_detail[0].body_medium_piece_mask
        );
        if (expected == 1)
            OA_CHECK(std::popcount(enhanced.unit_detail[0].body_piece_mask) == 6);
        else
            OA_CHECK(std::popcount(enhanced.unit_detail[0].body_medium_piece_mask) == 8);
        OA_CHECK(ordinary.body_lod_counts.body_lod_models == 1);
        OA_CHECK(enhanced.body_lod_counts.body_lod_models == 1);
        OA_CHECK(!ordinary.cards.cast_indices.empty());
        OA_CHECK(ordinary.cards.units.size() == 1 && enhanced.cards.units.size() == 1);
        if (ordinary.cards.units.size() == 1 && enhanced.cards.units.size() == 1) {
            const auto& reference = ordinary.cards.units[0];
            const auto& actual = enhanced.cards.units[0];
            OA_CHECK(reference.body_lod && actual.body_lod);
            OA_CHECK(!ordinary.card_types.types[reference.type].shadow_runs_ready);
            OA_CHECK(enhanced.card_types.types[actual.type].shadow_runs_ready);
        }
        check_caster_index_payload(ordinary, enhanced);
    }
}

} // namespace
