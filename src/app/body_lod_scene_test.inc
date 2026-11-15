// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

namespace {

/// Builds articulated opaque bodies with optional deliberately dense source faces.
struct BodyLodFixture {
    Fixture battle{};
    std::shared_ptr<oa::formats::objects3d::Model> source{};
    oa::present::model::PreparedModel prepared{};
    oa::sim::model_runtime::Instance instance{};
    oa::present::model::ModelState state{};
    oa::Unit unit{};

    explicit BodyLodFixture(std::size_t piece_count = 2) {
        namespace drawing = oa::present::model;
        namespace runtime = oa::sim::model_runtime;
        constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
        source = std::make_shared<oa::formats::objects3d::Model>();
        source->objects = {coarse_cube(5 * fixed), coarse_cube(3 * fixed)};
        source->objects[0].first_child = 1;
        source->objects[0].offset_from_parent.y = 12 * fixed;
        source->objects[1].parent = 0;
        source->objects[1].offset_from_parent = {3 * fixed, -8 * fixed, 0};
        source->objects.resize(piece_count, source->objects[1]);
        for (std::size_t piece = 1; piece + 1 < piece_count; ++piece)
            source->objects[piece].next_sibling = static_cast<uint32_t>(piece + 1);
        // Many source faces make the original model exceed a crowded frame's
        // allowance. They do not change its silhouette or the reduced hull.
        prepared.objects.resize(source->objects.size());
        for (std::size_t piece = 0; piece < source->objects.size(); ++piece) {
            auto& object = source->objects[piece];
            const auto faces = object.primitives;
            object.primitives.clear();
            for (uint32_t repeat = 0; repeat < 4; ++repeat)
                object.primitives.insert(object.primitives.end(), faces.begin(), faces.end());
            for (uint32_t face = 0; face < object.primitives.size(); ++face) {
                auto& material = prepared.objects[piece].primitives.emplace_back();
                material.source_index = face;
                material.flags = drawing::primitive_colored;
                material.color = alternate_index;
            }
        }
        instance = runtime::make_instance(source);
        instance.rebuild_transforms({});
        instance.pieces()[0].flags |= static_cast<uint16_t>(runtime::PieceFlag::cached);
        state.image = oa::present::create_sprite(8, 8);
        unit.id = 7;
        unit.position = {16 * fixed, 0, 16 * fixed};
        battle.draws.models.resize(1);
        auto& draw = battle.draws.models[0];
        draw.model.unit = &unit;
        draw.model.instance = &instance;
        draw.model.prepared = &prepared;
        draw.model.state = &state;
        draw.plan.linked.drawn = true;
        draw.plan.model.from_image = true;
        battle.draws.shadow_level = drawing::shadow_full_level;
        battle.inputs.models_on_card = true;
        battle.inputs.unit_marker_pixels = 5;
        battle.inputs.body_lod_enabled = true;
        battle.inputs.graphics_flags =
            drawing::graphics_shadows | drawing::graphics_vehicle_shadows;
        battle.inputs.sight.mapping = battle.inputs.sight.line_of_sight = false;
        battle.inputs.clip_width = battle.inputs.clip_height = 128;
        projection(0.5F);
    }

    /// Changes the projection without changing any simulation field or pose.
    void projection(float scale) {
        battle.inputs.clip_projection = {
            scale, 0, 0, 32, 0, -scale * 0.5F, scale, 32, 0, -0.001F, 0, 0.5F
        };
    }
};

/// Keeps textured body materials and their live model ownership in one fixture.
struct TexturedBodyLodFixture : BodyLodFixture {
    std::array<std::array<uint8_t, 4>, 3> pixels{};
    oa::present::model::TextureSequence texture{};
    oa::formats::gaf::Sequence sequence{};

    /// @param team choose each owner's mapped colour instead of the running frame
    explicit TexturedBodyLodFixture(bool team = false) {
        namespace drawing = oa::present::model;
        prepared.model = source.get();
        prepared.owner = source;
        texture.frames.resize(pixels.size());
        for (std::size_t frame = 0; frame < texture.frames.size(); ++frame) {
            pixels[frame].fill(static_cast<uint8_t>(alternate_index + frame));
            auto& sprite = texture.frames[frame];
            sprite.width = sprite.height = 2;
            sprite.encoding = OA_SPRITE_RAW;
            sprite.data = pixels[frame].data();
        }
        for (std::size_t piece = 0; piece < source->objects.size(); ++piece) {
            // Textured faces have four corners; the remaining coloured faces
            // keep a visible original mesh when a texture is unreadable.
            source->objects[piece].primitives[0].vertex_indices = {0, 4, 6, 2};
            auto& primitive = prepared.objects[piece].primitives[0];
            primitive.flags = drawing::primitive_animated | (team ? drawing::primitive_team : 0);
            primitive.texture = &texture;
            primitive.cursor.sequence = &sequence;
            primitive.cursor.frame_index = 1;
        }
        instance.pieces()[1].flags &=
            ~static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::cached);
    }
};

/// A new build rechecks changed texture frames instead of retaining prior readiness.
void body_lod_material_changes() {
    TexturedBodyLodFixture fixture;
    auto& battle = fixture.battle;
    const auto ready = [&](bool expected) {
        OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
        OA_CHECK(battle.error.empty() && battle.scene.unit_detail.size() == 1);
        if (battle.scene.unit_detail.size() == 1)
            OA_CHECK(battle.scene.unit_detail[0].body_available == expected);
        OA_CHECK(battle.scene.body_lod_counts.body_lod_models == (expected ? 1U : 0U));
        OA_CHECK(battle.scene.body_lod_counts.full_mesh_models == (expected ? 0U : 1U));
        OA_CHECK(
            battle.scene.body_lod_counts.distance_markers == 0 &&
            battle.scene.body_lod_counts.budget_markers == 0
        );
    };
    ready(true);
    // Frame storage and primitive bindings remain stable across all builds.
    auto& moving = fixture.prepared.objects[1].primitives[0];
    moving.cursor.frame_index = 2;
    ready(true);
    auto& frame = fixture.texture.frames[2];
    frame.data = nullptr;
    ready(false);
    frame.data = fixture.pixels[2].data();
    ready(true);
    frame.encoding = OA_SPRITE_ROW_RLE;
    ready(false);
    frame.encoding = OA_SPRITE_RAW;
    ready(true);
    moving.cursor.sequence = nullptr;
    ready(false);
    moving.cursor.sequence = &fixture.sequence;
    moving.cursor.frame_index = 99;
    ready(false);
    moving.cursor.frame_index = 2;
    ready(true);
    // A drawn zero-sized RAW material still follows the original failure
    // path. Restoring it must restore physical bodies on the next build.
    for (const bool width : {true, false}) {
        (width ? frame.width : frame.height) = 0;
        OA_CHECK(!live::build(battle.inputs, battle.scene, battle.error));
        OA_CHECK(battle.error.find("invalid material") != std::string::npos);
        OA_CHECK(battle.scene.body_lod_counts.body_lod_models == 0);
        (width ? frame.width : frame.height) = 2;
        ready(true);
    }
    // Readiness checks both slots even when this cached piece draws only its
    // first frame. An unused invalid running slot therefore keeps the full mesh.
    fixture.instance.pieces()[1].flags |=
        static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::cached);
    frame.width = 0;
    ready(false);
    frame.width = 2;
    ready(true);
}

/// Distinct owners use their actual mapped team frames on every build.
void body_lod_material_teams() {
    TexturedBodyLodFixture fixture(true);
    auto& battle = fixture.battle;
    constexpr uint32_t population = 6;
    std::array<oa::Unit, population> units{};
    battle.draws.models.resize(population, battle.draws.models[0]);
    for (uint32_t index = 0; index < population; ++index) {
        units[index] = fixture.unit;
        units[index].id = static_cast<uint16_t>(index + 1);
        units[index].owner_index = static_cast<uint8_t>(index % 3);
        battle.draws.models[index].model.unit = &units[index];
    }
    fixture.texture.frames[2].data = nullptr;
    battle.inputs.team_colors[0] = battle.inputs.team_colors[2] = 1;
    battle.inputs.team_colors[1] = 2;
    const auto check = [&](uint32_t expected) {
        OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
        OA_CHECK(battle.scene.body_lod_counts.body_lod_models == expected);
        OA_CHECK(battle.scene.body_lod_counts.full_mesh_models == population - expected);
        OA_CHECK(battle.scene.unit_detail.size() == population);
        for (uint32_t index = 0;
             index < std::min<std::size_t>(population, battle.scene.unit_detail.size());
             ++index) {
            const auto team = battle.inputs.team_colors[units[index].owner_index];
            OA_CHECK(battle.scene.unit_detail[index].body_available == (team != 2));
        }
    };
    check(4);
    battle.inputs.team_colors[0] = battle.inputs.team_colors[2] = 2;
    battle.inputs.team_colors[1] = 1;
    check(2);
    fixture.texture.frames[2].data = fixture.pixels[2].data();
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.body_lod_counts.body_lod_models == population);
}

/// Matching team colours cannot share readiness between different model types.
void body_lod_material_types() {
    TexturedBodyLodFixture first(true), second(true);
    auto& battle = first.battle;
    first.unit.id = 1;
    second.unit.id = 2;
    battle.draws.models.push_back(second.battle.draws.models[0]);
    second.texture.frames[0].data = nullptr;
    const auto check = [&](bool first_ready) {
        OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
        OA_CHECK(battle.scene.body_lod_counts.body_lod_models == 1);
        OA_CHECK(battle.scene.body_lod_counts.full_mesh_models == 1);
        OA_CHECK(battle.scene.unit_detail.size() == 2);
        if (battle.scene.unit_detail.size() == 2) {
            OA_CHECK(battle.scene.unit_detail[0].body_available == first_ready);
            OA_CHECK(battle.scene.unit_detail[1].body_available != first_ready);
        }
    };
    check(true);
    second.texture.frames[0].data = second.pixels[0].data();
    first.texture.frames[0].data = nullptr;
    check(false);
}

/// Material memo controls retain exact physical bodies and counters across bands.
void body_lod_material_bands() {
    TexturedBodyLodFixture fixture(true);
    auto& battle = fixture.battle;
    constexpr uint32_t population = 1024;
    std::vector<oa::Unit> units(population, fixture.unit);
    battle.draws.models.resize(population, battle.draws.models[0]);
    for (uint32_t index = 0; index < population; ++index) {
        units[index].id = static_cast<uint16_t>(index + 1);
        units[index].owner_index = static_cast<uint8_t>(index % 3);
        battle.draws.models[index].model.unit = &units[index];
    }
    fixture.texture.frames[2].data = nullptr;
    battle.inputs.team_colors[0] = battle.inputs.team_colors[2] = 1;
    battle.inputs.team_colors[1] = 2;
    std::array<live::Scene, 3> scenes;
    const auto ownership = [&](bool owned) {
        fixture.prepared.model = owned ? fixture.source.get() : nullptr;
        if (owned)
            fixture.prepared.owner = fixture.source;
        else
            fixture.prepared.owner.reset();
    };
    const auto compare = [&](const live::Scene& actual) {
        const auto& expected = scenes[0];
        const auto left = card_frame(expected), right = card_frame(actual);
        OA_CHECK(left.units == right.units && left.vertices.size() == right.vertices.size());
        if (left.vertices.size() == right.vertices.size() && !left.vertices.empty())
            OA_CHECK(
                std::memcmp(
                    left.vertices.data(),
                    right.vertices.data(),
                    left.vertices.size() * sizeof(left.vertices[0])
                ) == 0
            );
        const auto& a = expected.body_lod_counts;
        const auto& b = actual.body_lod_counts;
        OA_CHECK(
            std::tie(
                a.full_mesh_models,
                a.body_lod_models,
                a.distance_markers,
                a.budget_markers,
                a.full_mesh_vertices,
                a.body_lod_vertices,
                a.shadow_vertices
            ) ==
            std::tie(
                b.full_mesh_models,
                b.body_lod_models,
                b.distance_markers,
                b.budget_markers,
                b.full_mesh_vertices,
                b.body_lod_vertices,
                b.shadow_vertices
            )
        );
        OA_CHECK(expected.unit_detail.size() == actual.unit_detail.size());
        for (std::size_t index = 0;
             index < std::min(expected.unit_detail.size(), actual.unit_detail.size());
             ++index)
            OA_CHECK(
                expected.unit_detail[index].body_available ==
                actual.unit_detail[index].body_available
            );
    };
    OA_CHECK(live::build(battle.inputs, scenes[0], battle.error));
    OA_CHECK(scenes[0].body_lod_counts.full_mesh_models == population / 3);
    OA_CHECK(scenes[0].body_lod_counts.body_lod_models == population - population / 3);
    ownership(false);
    OA_CHECK(live::build(battle.inputs, scenes[1], battle.error));
    compare(scenes[1]);
    ownership(true);
    oa::platform::job_pool::Pool four(4);
    battle.inputs.pool = &four;
    OA_CHECK(live::build(battle.inputs, scenes[2], battle.error));
    compare(scenes[2]);
    // A palette edit also exercises material re-packing after a warm frame.
    const auto old_atlas = scenes[0].texture_rgba;
    battle.palette[alternate_index * oa::palette_entry_bytes] ^= uint8_t{17};
    battle.inputs.pool = nullptr;
    OA_CHECK(live::build(battle.inputs, scenes[0], battle.error));
    ownership(false);
    OA_CHECK(live::build(battle.inputs, scenes[1], battle.error));
    compare(scenes[1]);
    OA_CHECK(scenes[0].texture_rgba != old_atlas);
}

/// Physical hulls follow moving visible pieces and retain actual GPU pick geometry.
void animated_body_lod_scene() {
    body_lod_material_changes();
    body_lod_material_teams();
    body_lod_material_types();
    body_lod_material_bands();
    namespace runtime = oa::sim::model_runtime;
    BodyLodFixture fixture;
    auto& battle = fixture.battle;
    live::Scene full;
    battle.inputs.body_lod_enabled = false;
    OA_CHECK(live::build(battle.inputs, full, battle.error));
    OA_CHECK(full.body_lod_counts.full_mesh_models == 1);
    battle.inputs.body_lod_enabled = true;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.body_lod_counts.body_lod_models == 1);
    OA_CHECK(battle.scene.body_lod_counts.full_mesh_models == 0);
    OA_CHECK(battle.scene.body_lod_counts.distance_markers == 0);
    OA_CHECK(battle.scene.body_lod_counts.budget_markers == 0);
    OA_CHECK(
        battle.scene.body_lod_counts.body_lod_vertices < full.body_lod_counts.full_mesh_vertices
    );
    OA_CHECK(battle.scene.cards.positions.size() < full.cards.positions.size());
    OA_CHECK(battle.scene.cards.units.size() == 1 && battle.scene.cards.units[0].body_lod);
    OA_CHECK(!battle.scene.cards.cast_indices.empty());
    OA_CHECK(
        battle.scene.body_lod_counts.shadow_vertices == battle.scene.cards.cast_indices.size()
    );
    const auto first = battle.scene.cards.positions;
    const auto ordinary_points = fixture.instance.pieces()[1].transformed_vertices;
    battle.draws.models[0].shown_vertices.resize(fixture.instance.pieces().size());
    for (std::size_t piece = 0; piece < fixture.instance.pieces().size(); ++piece)
        battle.draws.models[0].shown_vertices[piece] =
            fixture.instance.pieces()[piece].transformed_vertices;
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    for (auto& point : battle.draws.models[0].shown_vertices[1])
        point.x += fixed;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.body_lod_counts.body_lod_models == 1);
    OA_CHECK(battle.scene.cards.positions != first);
    OA_CHECK(fixture.instance.pieces()[1].transformed_vertices.size() == ordinary_points.size());
    for (std::size_t point = 0; point < ordinary_points.size(); ++point) {
        const auto& actual = fixture.instance.pieces()[1].transformed_vertices[point];
        OA_CHECK(
            actual.x == ordinary_points[point].x && actual.y == ordinary_points[point].y &&
            actual.z == ordinary_points[point].z
        );
    }
    const auto visible_indices = battle.scene.cards.indices.size();
    fixture.instance.pieces()[1].flags &= ~static_cast<uint16_t>(runtime::PieceFlag::visible);
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.cards.indices.size() < visible_indices);
    fixture.instance.pieces()[1].flags |= static_cast<uint16_t>(runtime::PieceFlag::visible);
    battle.draws.models[0].shown_vertices.clear();
    fixture.projection(2);
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.body_lod_counts.full_mesh_models == 1);
    OA_CHECK(battle.scene.body_lod_counts.body_lod_models == 0);
    OA_CHECK(battle.scene.cards.units.size() == 1 && !battle.scene.cards.units[0].body_lod);
    fixture.projection(0.1F);
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.body_lod_counts.distance_markers == 1);
    OA_CHECK(battle.scene.body_lod_counts.body_lod_models == 0);
    fixture.projection(0.5F);
    fixture.unit.state_flags |= oa::present::model::unit_state_cloaked;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(!battle.scene.unit_detail[0].body_available);
    OA_CHECK(battle.scene.body_lod_counts.body_lod_models == 0);
}

/// Crowded physical bodies reserve their low geometry before admitting near meshes.
void crowded_animated_body_lod() {
    constexpr uint32_t population = 20000;
    BodyLodFixture fixture;
    auto& battle = fixture.battle;
    fixture.projection(2);
    std::vector<oa::Unit> units(population, fixture.unit);
    battle.draws.models.resize(population, battle.draws.models[0]);
    for (uint32_t index = 0; index < population; ++index) {
        units[index].id = static_cast<uint16_t>(index + 1);
        battle.draws.models[index].model.unit = &units[index];
    }
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    const auto& counts = battle.scene.body_lod_counts;
    OA_CHECK(counts.body_lod_models > 0);
    OA_CHECK(counts.full_mesh_models + counts.body_lod_models == population);
    OA_CHECK(counts.distance_markers == 0 && counts.budget_markers == 0);
    OA_CHECK(battle.scene.cards.units.size() == population);
    OA_CHECK(battle.scene.cards.markers.empty());
    OA_CHECK(
        battle.scene.vertices.size() + battle.scene.cards.vertices <=
        oa::app::renderer_live_gpu::largest_vertex_count
    );
    OA_CHECK(
        battle.scene.cards.cast_indices.size() <= oa::app::renderer_live_gpu::largest_vertex_count
    );
    OA_CHECK(counts.full_mesh_vertices + counts.body_lod_vertices == battle.scene.cards.vertices);
}

/// Masked pieces still consume compact positions; reserve those before drawing.
void crowded_body_position_budget() {
    constexpr uint32_t population = 20000;
    BodyLodFixture fixture(64);
    auto& battle = fixture.battle;
    std::vector<oa::Unit> units(population, fixture.unit);
    battle.draws.models.resize(population, battle.draws.models[0]);
    for (uint32_t index = 0; index < population; ++index) {
        units[index].id = static_cast<uint16_t>(index + 1);
        battle.draws.models[index].model.unit = &units[index];
    }
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    const auto& counts = battle.scene.body_lod_counts;
    OA_CHECK(counts.body_lod_models > 0 && counts.budget_markers > 0);
    OA_CHECK(counts.full_mesh_models == 0 && counts.distance_markers == 0);
    OA_CHECK(counts.body_lod_models + counts.budget_markers == population);
    std::size_t reserved{};
    for (std::size_t index = 0; index < population; ++index) {
        const auto& detail = battle.scene.unit_detail[index];
        if (detail.marker)
            continue;
        OA_CHECK(detail.body_lod == 1);
        const auto type = battle.scene.card_types.body_draw_types_low[index];
        OA_CHECK(type >= 0);
        reserved +=
            battle.scene.card_types.types[static_cast<std::size_t>(type)].piece_vertices.back();
    }
    OA_CHECK(reserved <= oa::app::renderer_live_gpu::largest_vertex_count);
    OA_CHECK(battle.scene.cards.positions.size() / 3 <= reserved);
}

} // namespace
