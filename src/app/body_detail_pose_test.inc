// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

namespace {

/// Records actual packets, sampled picks and every per-unit body decision.
void body_detail_pose_signature(
    const char* name, const live::Scene& scene, const live::Inputs& in
) {
    InteriorCullSignature result;
    result.word(interior_cull_signature(scene, in));
    result.word(scene.unit_detail.size());
    for (const auto& detail : scene.unit_detail) {
        for (const auto value : std::array<uint64_t, 12>{
                 detail.surface,
                 detail.shadow,
                 detail.body_low_surface,
                 detail.body_low_shadow,
                 detail.body_medium_surface,
                 detail.body_medium_shadow,
                 detail.body_piece_mask,
                 detail.body_medium_piece_mask,
                 detail.body_lod,
                 detail.desired_body_lod,
                 detail.body_available,
                 detail.eligible
             })
            result.word(value);
        result.number(detail.pixels);
        result.word(detail.marker);
        result.word(detail.surface_outside);
        result.word(detail.shadow_outside);
    }
    std::printf(
        "BODY_DETAIL_POSE_SIGNATURE %s %016llx\n",
        name,
        static_cast<unsigned long long>(result.value)
    );
}

/// Keeps exact animated local poses while translating independently named units.
struct BodyDetailPoseFixture : BodyLodFixture {
    oa::UnitDef definition{};
    std::vector<oa::Unit> units{};
    std::vector<uint8_t> previous_history{};

    explicit BodyDetailPoseFixture(size_t population, size_t pieces = 8)
        : BodyLodFixture(pieces), units(population, unit) {
        namespace runtime = oa::sim::model_runtime;
        constexpr auto visible = static_cast<uint16_t>(runtime::PieceFlag::visible);
        constexpr auto cached = static_cast<uint16_t>(runtime::PieceFlag::cached);
        for (auto& piece : instance.pieces())
            piece.flags = visible | cached;
        instance.pieces().back().flags = visible;
        battle.inputs.graphics_flags = 0;
        battle.inputs.pool = nullptr;
        battle.draws.models.resize(population, battle.draws.models[0]);
        for (size_t at = 0; at < population; ++at) {
            units[at].id = static_cast<uint16_t>(at + 1);
            units[at].type_index = 1;
            units[at].position.x += static_cast<int32_t>(at % 12) * 65536;
            auto& draw = battle.draws.models[at];
            draw.model.unit = &units[at];
            draw.model.def = &definition;
        }
    }

    bool build(const char* name) {
        previous_history = battle.scene.body_detail_history;
        const bool built = live::build(battle.inputs, battle.scene, battle.error);
        OA_CHECK(built && battle.error.empty());
        if (built)
            body_detail_pose_signature(name, battle.scene, battle.inputs);
        return built;
    }
};

/// Each independent one-unit build has no earlier pose summary to reuse.
void body_detail_pose_single_references(const BodyDetailPoseFixture& fixture) {
    const auto decision = [](const live::UnitDetail& detail) {
        return std::tuple{
            detail.surface,
            detail.shadow,
            detail.pixels,
            detail.center,
            detail.eligible,
            detail.marker,
            detail.body_piece_mask,
            detail.body_medium_piece_mask,
            detail.body_low_surface,
            detail.body_medium_surface,
            detail.body_low_shadow,
            detail.body_medium_shadow,
            detail.body_available,
            detail.desired_body_lod,
            detail.body_lod
        };
    };
    for (size_t at = 0; at < fixture.battle.draws.models.size(); ++at) {
        oa::app::WorldDrawList one;
        one.models.push_back(fixture.battle.draws.models[at]);
        auto input = fixture.battle.inputs;
        input.draws = &one;
        live::Scene reference;
        reference.body_detail_history = fixture.previous_history;
        std::string error;
        OA_CHECK(live::build(input, reference, error));
        OA_CHECK(reference.unit_detail.size() == 1);
        if (reference.unit_detail.size() == 1)
            OA_CHECK(
                decision(reference.unit_detail[0]) == decision(fixture.battle.scene.unit_detail[at])
            );
    }
}

/// Exact local poses share geometry, while changed visible inputs remain separate.
void body_detail_pose_changes() {
    constexpr auto cached = static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::cached);
    constexpr auto visible = static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible);
    BodyDetailPoseFixture fixture(12);
    auto& battle = fixture.battle;
    if (!fixture.build("translated"))
        return;
    OA_CHECK(battle.scene.detail_poses.size() == 1 && battle.scene.detail_pose_hits == 11);
    OA_CHECK(battle.scene.body_lod_counts.body_lod_models == 12);
    const auto& geometry = battle.scene.detail_poses[0].body_geometry;
    OA_CHECK(geometry.measured && geometry.valid);
    OA_CHECK(geometry.low_piece_mask == 0x9f && geometry.medium_piece_mask == 0xff);
    OA_CHECK(geometry.low_surface != 0 && geometry.medium_surface > geometry.low_surface);
    body_detail_pose_single_references(fixture);

    auto& changed = battle.draws.models.back();
    for (const auto& piece : fixture.instance.pieces())
        changed.shown_vertices.push_back(piece.transformed_vertices);
    changed.shown_vertices[7][0].x += 2 * 65536;
    OA_CHECK(fixture.build("changed-local-point"));
    OA_CHECK(battle.scene.detail_poses.size() == 2);
    body_detail_pose_single_references(fixture);
    changed.shown_vertices.clear();
    auto alternate = fixture.instance;
    alternate.pieces()[7].flags |= cached;
    changed.model.instance = &alternate;
    OA_CHECK(fixture.build("changed-cached-flag"));
    OA_CHECK(battle.scene.detail_poses.size() == 2);
    OA_CHECK(battle.scene.unit_detail.front().body_piece_mask == 0x9f);
    OA_CHECK(battle.scene.unit_detail.back().body_piece_mask == 0x3f);
    body_detail_pose_single_references(fixture);
    alternate.pieces()[7].flags = 0;
    OA_CHECK(fixture.build("changed-visibility"));
    OA_CHECK(battle.scene.unit_detail.back().body_medium_piece_mask == 0x7f);
    body_detail_pose_single_references(fixture);
    auto reordered = fixture.instance;
    std::swap(reordered.pieces()[0].object_index, reordered.pieces()[1].object_index);
    changed.model.instance = &reordered;
    OA_CHECK(fixture.build("changed-piece-object-order"));
    OA_CHECK(battle.scene.detail_poses.size() == 2);
    OA_CHECK(
        battle.scene.card_types.body_draw_types_low.front() !=
        battle.scene.card_types.body_draw_types_low.back()
    );
    body_detail_pose_single_references(fixture);
    auto alternate_prepared = fixture.prepared;
    changed.model.instance = &fixture.instance;
    changed.model.prepared = &alternate_prepared;
    OA_CHECK(fixture.build("changed-prepared-identity"));
    OA_CHECK(battle.scene.detail_poses.size() == 2);
    body_detail_pose_single_references(fixture);
    BodyLodFixture other(8);
    for (auto& piece : other.instance.pieces())
        piece.flags = visible | cached;
    changed.model.instance = &other.instance;
    changed.model.prepared = &other.prepared;
    changed.model.state = &other.state;
    OA_CHECK(fixture.build("changed-model-identity"));
    OA_CHECK(battle.scene.detail_poses.size() == 2);
    OA_CHECK(battle.scene.unit_detail.back().body_piece_mask == 0x3f);
    body_detail_pose_single_references(fixture);
    changed.model.instance = &fixture.instance;
    changed.model.prepared = &fixture.prepared;
    changed.model.state = &fixture.state;
    fixture.projection(0.04F);
    OA_CHECK(fixture.build("changed-projection"));
    OA_CHECK(battle.scene.detail_poses.size() == 1);
    OA_CHECK(battle.scene.unit_detail.front().body_piece_mask == 1);
    OA_CHECK(battle.scene.unit_detail.front().body_medium_piece_mask == 1);
    fixture.projection(0.5F);
    OA_CHECK(fixture.build("restored-projection"));
    OA_CHECK(battle.scene.unit_detail.front().body_piece_mask == 0x9f);
    body_detail_pose_single_references(fixture);
}

/// Hidden point counts are checked per draw even when the exact visible pose matches.
void body_detail_pose_hidden_sizes() {
    BodyDetailPoseFixture fixture(3);
    auto& battle = fixture.battle;
    fixture.instance.pieces()[7].flags = 0;
    auto malformed = fixture.instance;
    malformed.pieces()[7].transformed_vertices.pop_back();
    battle.draws.models[1].model.instance = &malformed;
    if (!fixture.build("valid-invalid-valid-hidden-size"))
        return;
    OA_CHECK(battle.scene.detail_poses.size() == 1 && battle.scene.detail_pose_hits == 2);
    OA_CHECK(battle.scene.unit_detail[0].body_available);
    OA_CHECK(!battle.scene.unit_detail[1].body_available);
    OA_CHECK(battle.scene.unit_detail[2].body_available);
    OA_CHECK(
        battle.scene.unit_detail[0].body_piece_mask == battle.scene.unit_detail[2].body_piece_mask
    );
    OA_CHECK(battle.scene.body_lod_counts.body_lod_models == 2);
    OA_CHECK(battle.scene.body_lod_counts.full_mesh_models == 1);
    body_detail_pose_single_references(fixture);
}

/// Geometry sharing never shares per-unit history or nearby construction lighting.
void body_detail_pose_unit_gates() {
    BodyDetailPoseFixture fixture(2);
    auto& battle = fixture.battle;
    fixture.projection(12.3F / 18.0F);
    battle.scene.body_detail_history.resize(size_t{1} << 16);
    battle.scene.body_detail_history[1] = 1;
    battle.scene.body_detail_history[2] = 2;
    OA_CHECK(fixture.build("different-history"));
    OA_CHECK(battle.scene.detail_poses.size() == 1 && battle.scene.detail_pose_hits == 1);
    OA_CHECK(battle.scene.unit_detail[0].body_lod == 1);
    OA_CHECK(battle.scene.unit_detail[1].body_lod == 2);
    body_detail_pose_single_references(fixture);
    fixture.projection(0.5F);
    fixture.units[1].position.x = 96 * 65536;
    battle.inputs.nanolathe_lighting = true;
    battle.draws.spatial_particles.push_back({fixture.units[0].position, {80, 255, 80}, true});
    OA_CHECK(fixture.build("near-light-first"));
    OA_CHECK(!battle.scene.unit_detail[0].body_available);
    OA_CHECK(battle.scene.unit_detail[1].body_available);
    std::swap(fixture.units[0].position, fixture.units[1].position);
    OA_CHECK(fixture.build("near-light-after-shared-geometry"));
    OA_CHECK(battle.scene.detail_poses.size() == 1 && battle.scene.detail_pose_hits == 1);
    OA_CHECK(battle.scene.unit_detail[0].body_available);
    OA_CHECK(!battle.scene.unit_detail[1].body_available);
}

/// Current material frames are rechecked after each build clears its pose summaries.
void body_detail_pose_materials() {
    TexturedBodyLodFixture fixture;
    auto& battle = fixture.battle;
    std::array<oa::Unit, 3> units{fixture.unit, fixture.unit, fixture.unit};
    battle.draws.models.resize(units.size(), battle.draws.models[0]);
    for (size_t at = 0; at < units.size(); ++at) {
        units[at].id = static_cast<uint16_t>(at + 1);
        battle.draws.models[at].model.unit = &units[at];
    }
    auto& frame = fixture.texture.frames[1];
    for (const bool ready : {true, false, true}) {
        frame.data = ready ? fixture.pixels[1].data() : nullptr;
        OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
        body_detail_pose_signature(
            ready ? "material-ready" : "material-missing", battle.scene, battle.inputs
        );
        OA_CHECK(battle.scene.detail_poses.size() == 1 && battle.scene.detail_pose_hits == 2);
        for (const auto& detail : battle.scene.unit_detail)
            OA_CHECK(detail.body_available == ready);
    }
}

/// A later overflowing local projection keeps the earlier partial mask but no body.
void body_detail_pose_invalid_projection() {
    BodyDetailPoseFixture fixture(2, 2);
    auto& battle = fixture.battle;
    for (size_t piece = 0; piece < fixture.instance.pieces().size(); ++piece)
        for (auto& point : fixture.instance.pieces()[piece].transformed_vertices)
            point.x += static_cast<int32_t>(piece == 0 ? 25000 : 30000) * 65536;
    for (auto& unit : fixture.units)
        unit.position.x = -27500 * 65536;
    battle.inputs.clip_projection[0] = std::numeric_limits<float>::max() / 28000.0F;
    if (!fixture.build("later-invalid-local-projection"))
        return;
    OA_CHECK(battle.scene.detail_poses.size() == 1 && battle.scene.detail_pose_hits == 1);
    const auto& geometry = battle.scene.detail_poses[0].body_geometry;
    OA_CHECK(geometry.measured && !geometry.valid);
    OA_CHECK(geometry.low_piece_mask == 0 && geometry.medium_piece_mask == 1);
    for (const auto& detail : battle.scene.unit_detail) {
        OA_CHECK(!detail.body_available && detail.body_lod == 0);
        OA_CHECK(detail.body_piece_mask == 0 && detail.body_medium_piece_mask == 1);
    }
}

/// More unique presented poses than the bounded lookup still emit complete bodies.
void body_detail_pose_capacity() {
    constexpr size_t population = 1026;
    BodyDetailPoseFixture fixture(population, 2);
    auto& battle = fixture.battle;
    for (size_t at = 0; at < population; ++at) {
        auto& draw = battle.draws.models[at];
        for (const auto& piece : fixture.instance.pieces())
            draw.shown_vertices.push_back(piece.transformed_vertices);
        draw.shown_vertices[0][0].x += static_cast<int32_t>(at);
    }
    if (!fixture.build("bounded-1026-unique-poses"))
        return;
    OA_CHECK(!battle.scene.detail_poses.empty() && battle.scene.detail_poses.size() <= 1024);
    OA_CHECK(battle.scene.detail_pose_points.size() <= 131072);
    OA_CHECK(battle.scene.detail_pose_pieces.size() <= 32768);
    OA_CHECK(battle.scene.body_lod_counts.body_lod_models == population);
    OA_CHECK(battle.scene.cards.units.size() == population);
    OA_CHECK(battle.scene.body_lod_counts.distance_markers == 0);
    OA_CHECK(battle.scene.body_lod_counts.budget_markers == 0);
    for (const auto& detail : battle.scene.unit_detail)
        OA_CHECK(detail.body_available && detail.body_lod == 1 && detail.body_piece_mask == 3);
}

/// Covers reusable body geometry while all unit-specific admission remains independent.
void body_detail_pose_geometry() {
    body_detail_pose_changes();
    body_detail_pose_hidden_sizes();
    body_detail_pose_unit_gates();
    body_detail_pose_materials();
    body_detail_pose_invalid_projection();
    body_detail_pose_capacity();
}

} // namespace
