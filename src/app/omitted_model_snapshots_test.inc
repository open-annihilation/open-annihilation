// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

namespace {

/// Places omitted records before, between and after sixteen ordinary casters.
struct OmittedModelFixture {
    static constexpr uint32_t population = 32;
    BodyLodFixture bodies{};
    oa::UnitDef definition{};
    std::vector<oa::Unit> units{population, bodies.unit};

    OmittedModelFixture() {
        auto& battle = bodies.battle;
        battle.inputs.unit_marker_pixels = 0;
        battle.inputs.geometry_shadow_metadata = battle.inputs.geometry_shadows_in_view = true;
        bodies.instance.pieces()[1].flags &=
            ~static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::cached);
        battle.draws.models.resize(population, battle.draws.models[0]);
        for (uint32_t at = 0; at < population; ++at) {
            units[at].id = static_cast<uint16_t>(at + 1);
            units[at].type_index = 1;
            auto& draw = battle.draws.models[at];
            draw.model.unit = &units[at];
            draw.model.def = &definition;
            draw.plan.linked.drawn = (at >= 4 && at < 12) || (at >= 16 && at < 24);
        }
    }
};

/// The final omitted record snapshots the completed payload before any run flush.
void check_omitted_model_snapshot(const live::Scene& scene, std::size_t end) {
    const auto& pass = scene.passes.models;
    OA_CHECK(pass.stopped == end && !pass.failure);
    OA_CHECK(pass.draw_vertices == pass.vertices.size());
    OA_CHECK(pass.draw_translucent == pass.translucent_vertices.size());
    OA_CHECK(pass.draw_ranges == pass.unit_ranges.size());
    OA_CHECK(pass.cards.draw_instance == -1);
    auto remarked = pass.cards;
    live::mark_cards(remarked);
    OA_CHECK(procedural_shadow_records(pass.cards) == procedural_shadow_records(remarked));
}

/// Omitted tails retain complete snapshots, including a short warm shadow run.
void omitted_model_snapshots() {
    for (const bool enhanced : {false, true})
        for (unsigned encoding = 0; encoding < 3; ++encoding) {
            OmittedModelFixture fixture;
            auto& battle = fixture.bodies.battle;
            battle.inputs.body_lod_enabled = enhanced;
            battle.inputs.shadow_index_templates = encoding != 0;
            battle.inputs.shadow_index_procedural = encoding == 2;
            live::Scene scene;
            for (unsigned frame = 0; frame < 2; ++frame) {
                OA_CHECK(live::build(battle.inputs, scene, battle.error));
                OA_CHECK(battle.error.empty() && !scene.out_of_room);
                OA_CHECK(scene.cards.units.size() == 16 && scene.cards.casts.size() == 16);
                OA_CHECK(scene.body_lod_counts.full_mesh_models == 16);
                check_omitted_model_snapshot(scene, OmittedModelFixture::population);
            }
            if (encoding != 0) {
                const auto& stream = scene.passes.models.cards.shadow_indices;
                OA_CHECK(stream.runs.size() == 1);
                if (stream.runs.size() != 1)
                    continue;
                if (encoding == 1)
                    OA_CHECK(stream.runs[0].pattern != 0);
                else {
                    OA_CHECK(stream.casts.size() == 16);
                    OA_CHECK(std::all_of(stream.casts.begin(), stream.casts.end(), [](auto cast) {
                        return cast.pattern != 0;
                    }));
                }
                // A final omitted snapshot precedes the compact short-run flush.
                for (uint32_t at = 0; at < OmittedModelFixture::population; ++at)
                    battle.draws.models[at].plan.linked.drawn = at >= 4 && at < 7;
                OA_CHECK(live::build(battle.inputs, scene, battle.error));
                const auto& cards = scene.passes.models.cards;
                const auto& mark = cards.mark.shadow_indices;
                OA_CHECK(scene.cards.casts.size() == 3 && mark.last_run.cast_count == 3);
                OA_CHECK(mark.logical_indices == cards.shadow_indices.logical_indices);
                OA_CHECK(mark.runs == 1 && cards.shadow_indices.runs.size() == 1);
                const auto& pass = scene.passes.models;
                OA_CHECK(pass.stopped == OmittedModelFixture::population);
                OA_CHECK(pass.draw_vertices == pass.vertices.size());
                OA_CHECK(pass.draw_translucent == pass.translucent_vertices.size());
                OA_CHECK(pass.draw_ranges == pass.unit_ranges.size());
                if (cards.shadow_indices.runs.size() != 1)
                    continue;
                if (encoding == 1) {
                    OA_CHECK(mark.last_run.pattern != 0 && mark.indices == 0);
                    OA_CHECK(cards.shadow_indices.runs[0].pattern == 0);
                    OA_CHECK(cards.shadow_indices.indices.size() == mark.logical_indices);
                    auto restored = cards;
                    live::unmark_cards(restored);
                    OA_CHECK(restored.shadow_indices.indices.empty());
                    OA_CHECK(restored.shadow_indices.runs[0].pattern == mark.last_run.pattern);
                    OA_CHECK(restored.shadow_indices.runs[0].cast_count == 3);
                } else {
                    OA_CHECK(mark.casts == 3 && cards.shadow_indices.casts.size() == 3);
                    check_omitted_model_snapshot(scene, OmittedModelFixture::population);
                }
            }
            // Reusing a previously populated pass must still replace its final mark.
            for (auto& draw : battle.draws.models)
                draw.plan.linked.drawn = false;
            OA_CHECK(live::build(battle.inputs, scene, battle.error));
            OA_CHECK(battle.error.empty() && !scene.out_of_room);
            OA_CHECK(scene.cards.units.empty() && scene.cards.instances.empty());
            OA_CHECK(scene.cards.casts.empty() && live::shadow_index_count(scene.cards) == 0);
            OA_CHECK(scene.body_lod_counts.full_mesh_models == 0);
            OA_CHECK(scene.body_lod_counts.body_lod_models == 0);
            check_omitted_model_snapshot(scene, OmittedModelFixture::population);
        }
}

/// Omitted stretches preserve serial material retries between and after GPU intervals.
void omitted_model_material_retries() {
    for (const bool at_end : {false, true})
        for (const bool procedural : {false, true}) {
            MultipleModelRangeFixture fixture;
            auto& battle = fixture.bodies.battle;
            battle.inputs.shadow_index_templates = true;
            battle.inputs.shadow_index_procedural = procedural;
            const auto material =
                at_end ? MultipleModelRangeFixture::suffix : MultipleModelRangeFixture::first_gap;
            battle.draws.models[material].model.prepared = &fixture.unique_material;
            battle.draws.models[material].plan.linked.drawn = false;
            for (const auto gap :
                 {MultipleModelRangeFixture::first_gap, MultipleModelRangeFixture::suffix})
                for (uint32_t before = 1; before <= 3; ++before)
                    battle.draws.models[gap - before].plan.linked.drawn = false;
            // The final suffix is itself omitted when testing a middle pause.
            if (!at_end)
                battle.draws.models[MultipleModelRangeFixture::suffix].plan.linked.drawn = false;
            constexpr auto omitted = uint32_t{6};
            constexpr auto slots = MultipleModelRangeFixture::candidate_slots;
            oa::platform::job_pool::Pool pool(4);
            live::Scene serial, parallel;
            const auto build = [&] {
                battle.inputs.pool = nullptr;
                OA_CHECK(live::build(battle.inputs, serial, battle.error));
                battle.inputs.pool = &pool;
                OA_CHECK(live::build(battle.inputs, parallel, battle.error));
                OA_CHECK(battle.error.empty());
                check_multiple_range_frame(serial, parallel);
                OA_CHECK(serial.passes.models.stopped == MultipleModelRangeFixture::population);
                OA_CHECK(parallel.passes.models.stopped == MultipleModelRangeFixture::population);
                OA_CHECK(serial.body_lod_counts.body_lod_models >= slots - omitted);
            };
            build();
            build();
            OA_CHECK(parallel.model_work_counts.accepted_bands == 4);
            battle.draws.models[material].plan.linked.drawn = true;
            build();
            OA_CHECK(parallel.model_work_counts.accepted_bands == (at_end ? 4U : 0U));
            OA_CHECK(parallel.model_work_counts.fallback_frames == (at_end ? 0U : 1U));
            build();
            OA_CHECK(parallel.model_work_counts.accepted_bands == 4);
            OA_CHECK(parallel.model_work_counts.fallback_frames == 0);
        }
}

} // namespace
