// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

namespace {

/// Offscreen surfaces keep their complete ground casters before Enhanced skips them.
void offscreen_body_surface_skip() {
    BodyLodFixture fixture;
    auto& battle = fixture.battle;
    battle.inputs.geometry_shadows_in_view = true;
    for (const float pan : {-512.0F, 512.0F}) {
        fixture.projection(0.5F);
        // Only the camera projection moves. The unit and its shadow stay
        // inside the original world crop and still require a full caster.
        battle.inputs.clip_projection[3] += pan;
        live::Scene ordinary, enhanced;
        battle.inputs.body_lod_enabled = false;
        OA_CHECK(live::build(battle.inputs, ordinary, battle.error));
        battle.inputs.body_lod_enabled = true;
        OA_CHECK(live::build(battle.inputs, enhanced, battle.error));
        OA_CHECK(ordinary.unit_detail.size() == 1 && enhanced.unit_detail.size() == 1);
        if (ordinary.unit_detail.size() != 1 || enhanced.unit_detail.size() != 1)
            continue;
        OA_CHECK(!ordinary.unit_detail[0].surface_outside);
        OA_CHECK(enhanced.unit_detail[0].surface_outside);
        OA_CHECK(ordinary.unit_detail[0].surface == 0 && enhanced.unit_detail[0].surface == 0);
        OA_CHECK(!ordinary.unit_detail[0].shadow_outside);
        OA_CHECK(!enhanced.unit_detail[0].shadow_outside);
        OA_CHECK(ordinary.shadow_count == 1 && enhanced.shadow_count == 1);
        OA_CHECK(ordinary.cards.casts.size() == 1 && enhanced.cards.casts.size() == 1);
        OA_CHECK(!ordinary.cards.cast_indices.empty());
        OA_CHECK(ordinary.cards.cast_indices == enhanced.cards.cast_indices);
        OA_CHECK(ordinary.cards.positions == enhanced.cards.positions);
        OA_CHECK(ordinary.cards.instances.size() == 1 && enhanced.cards.instances.size() == 1);
        if (ordinary.cards.casts.size() == 1 && enhanced.cards.casts.size() == 1) {
            const auto& expected = ordinary.cards.casts[0];
            const auto& actual = enhanced.cards.casts[0];
            OA_CHECK(expected.instance == actual.instance && expected.kind == actual.kind);
            OA_CHECK(expected.depth == actual.depth && expected.light == actual.light);
        }
        OA_CHECK(enhanced.cards.shadow_map.width != 0 && enhanced.cards.shadow_map.height != 0);
        OA_CHECK(ordinary.cards.units.empty() && enhanced.cards.units.empty());
        OA_CHECK(ordinary.cards.indices.empty() && enhanced.cards.indices.empty());
        OA_CHECK(ordinary.cards.markers.empty() && enhanced.cards.markers.empty());
        OA_CHECK(enhanced.model_count == 0 && enhanced.cards.vertices == 0);
        const auto& counts = enhanced.body_lod_counts;
        OA_CHECK(counts.full_mesh_models == 0 && counts.body_lod_models == 0);
        OA_CHECK(counts.full_mesh_vertices == 0 && counts.body_lod_vertices == 0);
        OA_CHECK(counts.distance_markers == 0 && counts.budget_markers == 0);
        OA_CHECK(counts.shadow_vertices == enhanced.cards.cast_indices.size());
    }

    // The same surface proof does not manufacture a cast when shadows are off.
    battle.inputs.graphics_flags = 0;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.unit_detail.size() == 1);
    if (battle.scene.unit_detail.size() == 1)
        OA_CHECK(battle.scene.unit_detail[0].surface_outside);
    OA_CHECK(battle.scene.shadow_count == 0 && battle.scene.cards.casts.empty());
    OA_CHECK(battle.scene.cards.cast_indices.empty() && battle.scene.cards.positions.empty());
    OA_CHECK(battle.scene.cards.units.empty() && battle.scene.cards.indices.empty());
    OA_CHECK(battle.scene.body_lod_counts.shadow_vertices == 0);

    // A floating model can have reflected geometry outside its body bounds;
    // it retains the ordinary bounds-and-reflection path even offscreen.
    oa::UnitDef definition{};
    definition.flags = OA_UNIT_DEF_FLAG_FLOATER;
    battle.draws.models[0].model.def = &definition;
    battle.inputs.water.detail = 1;
    battle.inputs.water.reflections = 1;
    OA_CHECK(live::build(battle.inputs, battle.scene, battle.error));
    OA_CHECK(battle.scene.unit_detail.size() == 1);
    if (battle.scene.unit_detail.size() == 1)
        OA_CHECK(!battle.scene.unit_detail[0].surface_outside);
}

} // namespace
