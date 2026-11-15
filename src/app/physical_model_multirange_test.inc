// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

namespace {

/// Places articulated GPU armies around actual serial model draws.
struct MultipleModelRangeFixture {
    static constexpr uint32_t region_size = 512;
    static constexpr uint32_t first_gap = region_size;
    static constexpr uint32_t second_gap = region_size * 2 + 1;
    static constexpr uint32_t suffix = region_size * 3 + 2;
    static constexpr uint32_t population = suffix + 1;
    static constexpr uint32_t candidate_slots = region_size * 3;
    BodyLodFixture bodies{};
    oa::UnitDef definition{};
    oa::present::model::PreparedModel unique_material{};
    std::vector<oa::Unit> units{};

    MultipleModelRangeFixture() {
        constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
        auto& battle = bodies.battle;
        bodies.instance.pieces()[1].flags &=
            ~static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::cached);
        bodies.state.shadow = oa::present::create_sprite(8, 8);
        unique_material = bodies.prepared;
        for (auto& object : unique_material.objects)
            for (auto& primitive : object.primitives)
                primitive.color = 3;
        battle.palette[3 * oa::palette_entry_bytes] = 230;
        battle.palette[3 * oa::palette_entry_bytes + 1] = 40;
        battle.palette[3 * oa::palette_entry_bytes + 2] = 170;
        units.assign(population, bodies.unit);
        battle.draws.models.resize(population, battle.draws.models[0]);
        for (uint32_t index = 0; index < population; ++index) {
            auto& unit = units[index];
            unit.id = static_cast<uint16_t>(index + 1);
            unit.type_index = 1;
            unit.owner_index = static_cast<uint8_t>(index % 2);
            unit.position = {
                (8 + int32_t(index % 12)) * fixed, 0, (8 + int32_t(index / 12 % 12)) * fixed
            };
            auto& draw = battle.draws.models[index];
            draw.model.unit = &unit;
            draw.model.def = &definition;
        }
        units[first_gap].state_flags |= oa::present::model::unit_state_cloaked;
        units[second_gap].flags |= OA_UNIT_FLAG_BUILDING;
        units[suffix].state_flags |= oa::present::model::unit_state_cloaked;
        battle.inputs.body_lod_enabled = true;
        battle.inputs.geometry_shadow_metadata = true;
        battle.inputs.geometry_shadows_in_view = true;
        battle.inputs.owner_colors[0] = {255, 128, 32};
        battle.inputs.owner_colors[1] = {32, 128, 255};
    }

    /// Makes one genuine adaptive marker without moving a canonical unit.
    void marker(uint32_t index) {
        auto& draw = bodies.battle.draws.models[index];
        units[index].state_flags &= ~oa::present::model::unit_state_cloaked;
        draw.shown_vertices.resize(bodies.instance.pieces().size());
        for (std::size_t piece = 0; piece < bodies.instance.pieces().size(); ++piece) {
            draw.shown_vertices[piece] = bodies.instance.pieces()[piece].transformed_vertices;
            for (auto& point : draw.shown_vertices[piece]) {
                point.x /= 64;
                point.y /= 64;
                point.z /= 64;
            }
        }
    }
};

/// Visits each shadow's ordinary local corners, independent of packet boundaries.
std::vector<std::array<float, 3>> multiple_range_shadow_points(const live::Scene& scene) {
    namespace gpu = oa::app::renderer_live_gpu;
    const auto draws = live::card_draws(scene.card_types, scene.cards);
    std::pair<std::vector<uint32_t>, std::vector<gpu::ModelRun>> expanded;
    if (draws.shadow_index_encoding == gpu::ShadowIndexEncoding::compact)
        expanded = expanded_shadow_templates(draws);
    else if (draws.shadow_index_encoding == gpu::ShadowIndexEncoding::procedural)
        expanded = procedural_shadow_expansion(draws);
    else {
        expanded.first = scene.cards.cast_indices;
        expanded.second = scene.cards.cast_runs;
    }
    constexpr uint32_t corner_mask = (uint32_t{1} << gpu::model_corner_bits) - 1;
    constexpr uint32_t point_mask = (uint32_t{1} << gpu::model_piece_shift) - 1;
    std::vector<std::array<float, 3>> result;
    result.reserve(expanded.first.size());
    for (const auto& run : expanded.second)
        for (std::size_t at = run.first_index; at < std::size_t{run.first_index} + run.index_count;
             ++at) {
            const auto word = expanded.first[at];
            const auto cast = run.first_instance + (word >> gpu::model_corner_bits);
            const auto& instance = scene.cards.instances[scene.cards.casts[cast].instance];
            const auto record = instance.corners + (word & corner_mask);
            const auto point = scene.card_types.records[std::size_t{record} * 2] & point_mask;
            const auto first = std::size_t{instance.positions + point} * 3;
            result.push_back(
                {instance.origin[0] + scene.cards.positions[first],
                 instance.origin[1] + scene.cards.positions[first + 1],
                 instance.origin[2] + scene.cards.positions[first + 2]}
            );
        }
    return result;
}

/// Samples command-picking triangles from the current physical surface and translucent ranges.
std::vector<uint16_t> multiple_range_picks(const live::Scene& scene, const live::Inputs& in) {
    auto frame = card_frame(scene);
    const auto opaque = frame.vertices.size();
    frame.vertices.insert(
        frame.vertices.end(), scene.translucent_vertices.begin(), scene.translucent_vertices.end()
    );
    frame.units.resize(frame.vertices.size());
    for (const auto& range : scene.translucent_ranges)
        std::fill_n(
            frame.units.begin() + static_cast<std::ptrdiff_t>(opaque + range.first_vertex),
            range.vertex_count,
            range.unit_id
        );
    std::vector<uint16_t> result;
    for (const float y : {32.0F, 36.0F, 40.0F, 44.0F, 48.0F})
        for (const float x : {32.0F, 36.0F, 40.0F, 44.0F, 48.0F}) {
            uint16_t picked{};
            float nearest = -std::numeric_limits<float>::infinity();
            for (std::size_t first = 0; first + 2 < frame.vertices.size(); first += 3) {
                if (frame.units[first] == 0)
                    continue;
                std::array<std::array<float, 3>, 3> triangle{};
                for (std::size_t corner = 0; corner < triangle.size(); ++corner) {
                    const auto& vertex = frame.vertices[first + corner];
                    for (std::size_t row = 0; row < triangle[corner].size(); ++row) {
                        const auto offset = row * 4;
                        triangle[corner][row] = in.clip_projection[offset] * vertex.x +
                                                in.clip_projection[offset + 1] * vertex.height +
                                                in.clip_projection[offset + 2] * vertex.z +
                                                in.clip_projection[offset + 3];
                    }
                }
                std::array<float, 3> weights{};
                float depth{};
                if (oa::app::renderer_live_camera::triangle_hit(triangle, x, y, weights, depth) &&
                    depth >= nearest) {
                    nearest = depth;
                    picked = frame.units[first];
                }
            }
            result.push_back(picked);
        }
    return result;
}

/// Compares physical payloads without requiring worker pose or run storage to be identical.
void check_multiple_range_frame(const live::Scene& expected, const live::Scene& actual) {
    OA_CHECK(
        expected.model_count == actual.model_count && expected.shadow_count == actual.shadow_count
    );
    OA_CHECK(expected.texture_rgba == actual.texture_rgba);
    OA_CHECK(expected.terrain_vertex_count == actual.terrain_vertex_count);
    OA_CHECK(expected.vertices.size() == actual.vertices.size());
    if (expected.vertices.size() == actual.vertices.size() && !expected.vertices.empty())
        OA_CHECK(
            std::memcmp(
                expected.vertices.data(),
                actual.vertices.data(),
                expected.vertices.size() * sizeof(expected.vertices[0])
            ) == 0
        );
    OA_CHECK(expected.translucent_vertices.size() == actual.translucent_vertices.size());
    if (expected.translucent_vertices.size() == actual.translucent_vertices.size() &&
        !expected.translucent_vertices.empty())
        OA_CHECK(
            std::memcmp(
                expected.translucent_vertices.data(),
                actual.translucent_vertices.data(),
                expected.translucent_vertices.size() * sizeof(expected.translucent_vertices[0])
            ) == 0
        );
    const auto ranges_equal = [](const auto& a, const auto& b) {
        return a.unit_id == b.unit_id && a.first_vertex == b.first_vertex &&
               a.vertex_count == b.vertex_count;
    };
    OA_CHECK(
        std::equal(
            expected.unit_ranges.begin(),
            expected.unit_ranges.end(),
            actual.unit_ranges.begin(),
            actual.unit_ranges.end(),
            ranges_equal
        )
    );
    OA_CHECK(
        std::equal(
            expected.translucent_ranges.begin(),
            expected.translucent_ranges.end(),
            actual.translucent_ranges.begin(),
            actual.translucent_ranges.end(),
            ranges_equal
        )
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
    OA_CHECK(expected.cards.units.size() == actual.cards.units.size());
    for (std::size_t index = 0;
         index < std::min(expected.cards.units.size(), actual.cards.units.size());
         ++index) {
        const auto& left = expected.cards.units[index];
        const auto& right = actual.cards.units[index];
        OA_CHECK(
            std::tie(
                left.unit_id,
                left.vertex,
                left.index_count,
                left.left,
                left.top,
                left.right,
                left.bottom,
                left.bounded,
                left.marker,
                left.body_lod
            ) ==
            std::tie(
                right.unit_id,
                right.vertex,
                right.index_count,
                right.left,
                right.top,
                right.right,
                right.bottom,
                right.bounded,
                right.marker,
                right.body_lod
            )
        );
        if (left.index_count != right.index_count)
            continue;
        for (uint32_t corner = 0; corner < left.index_count; ++corner) {
            const auto first = live::card_vertex(
                expected.card_types,
                expected.cards,
                left,
                expected.cards.indices[left.first_index + corner]
            );
            const auto second = live::card_vertex(
                actual.card_types,
                actual.cards,
                right,
                actual.cards.indices[right.first_index + corner]
            );
            OA_CHECK(std::memcmp(&first, &second, sizeof(first)) == 0);
        }
    }
    OA_CHECK(expected.cards.casts.size() == actual.cards.casts.size());
    for (std::size_t index = 0;
         index < std::min(expected.cards.casts.size(), actual.cards.casts.size());
         ++index) {
        const auto& left = expected.cards.casts[index];
        const auto& right = actual.cards.casts[index];
        OA_CHECK(
            std::tie(
                left.kind,
                left.base,
                left.ground_half,
                left.least_height,
                left.light,
                left.depth,
                left.across,
                left.down
            ) ==
            std::tie(
                right.kind,
                right.base,
                right.ground_half,
                right.least_height,
                right.light,
                right.depth,
                right.across,
                right.down
            )
        );
    }
    OA_CHECK(expected.cards.cast_depths == actual.cards.cast_depths);
    OA_CHECK(multiple_range_shadow_points(expected) == multiple_range_shadow_points(actual));
}

/// Ordered Enhanced gaps pack new colours; the false gate keeps ordinary retries.
void multiple_model_range_material_retries() {
    for (const bool ordered_packing : {false, true})
        for (const bool at_end : {false, true}) {
            MultipleModelRangeFixture fixture;
            auto& battle = fixture.bodies.battle;
            battle.inputs.body_lod_enabled = ordered_packing;
            const auto index =
                at_end ? MultipleModelRangeFixture::suffix : MultipleModelRangeFixture::first_gap;
            auto& gap = battle.draws.models[index];
            gap.model.prepared = &fixture.unique_material;
            gap.plan.linked.drawn = false;
            oa::platform::job_pool::Pool pool(4);
            live::Scene serial, parallel;
            const auto build = [&] {
                battle.inputs.pool = nullptr;
                OA_CHECK(live::build(battle.inputs, serial, battle.error));
                battle.inputs.pool = &pool;
                OA_CHECK(live::build(battle.inputs, parallel, battle.error));
                OA_CHECK(battle.error.empty());
                check_multiple_range_frame(serial, parallel);
            };
            build();
            build();
            OA_CHECK(parallel.model_work_counts.accepted_bands == 4);
            // Both GPU regions are now warm. Only this actual serial draw has
            // a new colour; no worker can request or pack its material.
            gap.plan.linked.drawn = true;
            build();
            const auto& work = parallel.model_work_counts;
            OA_CHECK(work.candidate_bands == 4);
            OA_CHECK(work.candidate_slots == MultipleModelRangeFixture::candidate_slots);
            const bool retained = at_end || ordered_packing;
            OA_CHECK(work.accepted_bands == (retained ? 4U : 0U));
            OA_CHECK(
                work.accepted_slots == (retained ? MultipleModelRangeFixture::candidate_slots : 0U)
            );
            OA_CHECK(work.fallback_frames == (retained ? 0U : 1U));
            if (ordered_packing) {
                OA_CHECK(parallel.passes.models.materials_packed > 0);
                OA_CHECK(parallel.passes.models.texels_packed > 0);
            }
            OA_CHECK(parallel.translucent_ranges.size() == serial.translucent_ranges.size());
            build();
            OA_CHECK(parallel.model_work_counts.accepted_bands == 4);
            OA_CHECK(
                parallel.model_work_counts.accepted_slots ==
                MultipleModelRangeFixture::candidate_slots
            );
            OA_CHECK(parallel.model_work_counts.fallback_frames == 0);
            OA_CHECK(parallel.passes.models.materials_packed == 0);
            OA_CHECK(parallel.passes.models.texels_packed == 0);
        }
}

/// A genuine textured serial gap preserves warm packets or the original atlas repack.
void multiple_model_range_textured_gap() {
    for (const bool force_repack : {false, true}) {
        MultipleModelRangeFixture fixture;
        auto& battle = fixture.bodies.battle;
        OA_CHECK(battle.inputs.body_lod_enabled);
        std::fill_n(battle.palette.begin() + 6 * oa::palette_entry_bytes, 3, uint8_t{117});
        auto& gap = battle.draws.models[MultipleModelRangeFixture::first_gap];
        gap.plan.linked.drawn = false;
        oa::platform::job_pool::Pool pool(4);
        live::Scene serial, parallel;
        const auto compare = [&] {
            battle.inputs.pool = nullptr;
            OA_CHECK(live::build(battle.inputs, serial, battle.error));
            battle.inputs.pool = &pool;
            OA_CHECK(live::build(battle.inputs, parallel, battle.error));
            OA_CHECK(battle.error.empty());
            check_multiple_range_frame(serial, parallel);
            OA_CHECK(
                multiple_range_picks(serial, battle.inputs) ==
                multiple_range_picks(parallel, battle.inputs)
            );
        };
        compare();
        compare();
        OA_CHECK(parallel.model_work_counts.accepted_bands == 4);

        std::array<uint8_t, 32 * 32> pixels;
        pixels.fill(6);
        oa::Sprite texture{};
        texture.width = texture.height = 32;
        texture.encoding = OA_SPRITE_RAW;
        texture.data = pixels.data();
        // Textured source faces need four corners. Clone only this serial
        // draw's source and pose; every worker retains its original source.
        auto source = std::make_shared<oa::formats::objects3d::Model>(*fixture.bodies.source);
        source->objects[0].primitives[0].vertex_indices = {0, 4, 6, 2};
        auto instance = oa::sim::model_runtime::make_instance(source);
        for (std::size_t piece = 0; piece < instance.pieces().size(); ++piece) {
            auto& out = instance.pieces()[piece];
            const auto& in = fixture.bodies.instance.pieces()[piece];
            out.translation = in.translation;
            out.rotation = in.rotation;
            out.flags = in.flags;
        }
        instance.rebuild_transforms({}, true);
        auto prepared = fixture.unique_material;
        prepared.model = source.get();
        prepared.owner = source;
        auto& face = prepared.objects[0].primitives[0];
        face.flags = 0;
        face.frame = &texture;
        face.texture = nullptr;
        gap.model.instance = &instance;
        gap.model.prepared = &prepared;
        gap.plan.linked.drawn = true;
        const auto serial_revision = serial.texture_revision;
        const auto parallel_revision = parallel.texture_revision;
        if (force_repack) {
            serial.next_x = serial.texture_width;
            serial.next_y = serial.texture_height;
            parallel.next_x = parallel.texture_width;
            parallel.next_y = parallel.texture_height;
        }
        compare();
        OA_CHECK(parallel.passes.models.materials_packed > 0);
        OA_CHECK(parallel.passes.models.texels_packed >= pixels.size());
        OA_CHECK(fixture.bodies.source->objects[0].primitives[0].vertex_indices.size() == 3);
        if (force_repack) {
            OA_CHECK(serial.atlas_update.previous_revision == serial_revision);
            OA_CHECK(parallel.atlas_update.previous_revision == parallel_revision);
            OA_CHECK(serial.atlas_update.first_row == 0 && parallel.atlas_update.first_row == 0);
            OA_CHECK(
                serial.atlas_update.end_row > 0 &&
                serial.atlas_update.end_row <= serial.texture_height
            );
            OA_CHECK(
                parallel.atlas_update.end_row > 0 &&
                parallel.atlas_update.end_row <= parallel.texture_height
            );
            OA_CHECK(serial.passes.models.peak == parallel.passes.models.peak);
            OA_CHECK(serial.passes.models.stopped == parallel.passes.models.stopped);
        } else {
            OA_CHECK(parallel.model_work_counts.accepted_bands == 4);
            OA_CHECK(
                parallel.model_work_counts.accepted_slots ==
                MultipleModelRangeFixture::candidate_slots
            );
            OA_CHECK(parallel.model_work_counts.fallback_frames == 0);
        }
        compare();
        OA_CHECK(parallel.model_work_counts.accepted_bands == 4);
        OA_CHECK(parallel.model_work_counts.fallback_frames == 0);
        OA_CHECK(parallel.passes.models.materials_packed == 0);
        OA_CHECK(parallel.passes.models.texels_packed == 0);
    }
}

/// A serial marker remains inside a genuinely saturated global surface allowance.
void multiple_model_range_geometry_budget() {
    constexpr uint32_t half = 3000;
    constexpr uint32_t population = half * 2 + 1;
    constexpr auto limit = oa::app::renderer_live_gpu::largest_vertex_count;
    BodyLodFixture fixture;
    auto& battle = fixture.battle;
    OA_CHECK(battle.inputs.body_lod_enabled);
    fixture.projection(2);
    oa::UnitDef definition{};
    std::vector<oa::Unit> units(population, fixture.unit);
    battle.draws.models.resize(population, battle.draws.models[0]);
    for (uint32_t index = 0; index < population; ++index) {
        units[index].id = static_cast<uint16_t>(index + 1);
        units[index].type_index = 1;
        battle.draws.models[index].model.unit = &units[index];
        battle.draws.models[index].model.def = &definition;
    }
    auto& marker = battle.draws.models[half];
    marker.shown_vertices.resize(fixture.instance.pieces().size());
    for (std::size_t piece = 0; piece < fixture.instance.pieces().size(); ++piece) {
        marker.shown_vertices[piece] = fixture.instance.pieces()[piece].transformed_vertices;
        for (auto& point : marker.shown_vertices[piece]) {
            point.x /= 64;
            point.y /= 64;
            point.z /= 64;
        }
    }
    battle.inputs.geometry_shadow_metadata = battle.inputs.geometry_shadows_in_view = true;
    oa::platform::job_pool::Pool pool(4);
    live::Scene serial, parallel;
    std::string serial_error, parallel_error;
    for (unsigned frame = 0; frame < 2; ++frame) {
        battle.inputs.pool = nullptr;
        const bool serial_built = live::build(battle.inputs, serial, serial_error);
        battle.inputs.pool = &pool;
        const bool parallel_built = live::build(battle.inputs, parallel, parallel_error);
        OA_CHECK(serial_built == parallel_built && serial_built);
        OA_CHECK(serial_error == parallel_error && serial_error.empty());
        OA_CHECK(serial.out_of_room == parallel.out_of_room && !serial.out_of_room);
        check_multiple_range_frame(serial, parallel);
        const auto held = serial.vertices.size() + serial.cards.vertices;
        OA_CHECK(held <= limit && held > limit - 1024);
        OA_CHECK(serial.body_lod_counts.distance_markers == 1);
        OA_CHECK(serial.body_lod_counts.budget_markers == 0);
        OA_CHECK(
            serial.body_lod_counts.full_mesh_models > 0 &&
            serial.body_lod_counts.body_lod_models > 0
        );
        OA_CHECK(
            serial.body_lod_counts.full_mesh_models + serial.body_lod_counts.body_lod_models ==
            half * 2
        );
        OA_CHECK(serial.passes.models.peak == parallel.passes.models.peak);
        OA_CHECK(serial.passes.models.stopped == parallel.passes.models.stopped);
        OA_CHECK(serial.passes.models.stopped == population);
        if (frame != 0) {
            OA_CHECK(parallel.model_work_counts.candidate_bands == 4);
            OA_CHECK(parallel.model_work_counts.candidate_slots == half * 2);
            OA_CHECK(parallel.model_work_counts.accepted_slots == half * 2);
            OA_CHECK(parallel.model_work_counts.fallback_frames == 0);
        }
    }
}

/// Three genuine GPU intervals retain every body around serial CPU and shadow draws.
void physical_model_multiple_ranges() {
    MultipleModelRangeFixture fixture;
    auto& battle = fixture.bodies.battle;
    oa::platform::job_pool::Pool pool(4);
    live::Scene serial, parallel;
    const auto compare = [&](uint32_t slots, bool accepted) {
        const auto canonical = fixture.units;
        battle.inputs.pool = nullptr;
        OA_CHECK(live::build(battle.inputs, serial, battle.error));
        battle.inputs.pool = &pool;
        OA_CHECK(live::build(battle.inputs, parallel, battle.error));
        OA_CHECK(battle.error.empty());
        check_multiple_range_frame(serial, parallel);
        const auto& work = parallel.model_work_counts;
        OA_CHECK(work.candidate_bands == 4 && work.candidate_slots == slots);
        OA_CHECK(work.candidate_slots > MultipleModelRangeFixture::region_size);
        OA_CHECK(work.accepted_bands == (accepted ? 4U : 0U));
        OA_CHECK(work.accepted_slots == (accepted ? slots : 0U));
        OA_CHECK(work.fallback_frames == (accepted ? 0U : 1U));
        OA_CHECK(
            serial.body_lod_counts.body_lod_models >= MultipleModelRangeFixture::candidate_slots
        );
        OA_CHECK(
            multiple_range_picks(serial, battle.inputs) ==
            multiple_range_picks(parallel, battle.inputs)
        );
        OA_CHECK(
            std::memcmp(
                canonical.data(), fixture.units.data(), canonical.size() * sizeof(canonical[0])
            ) == 0
        );
    };
    compare(MultipleModelRangeFixture::candidate_slots, false);
    compare(MultipleModelRangeFixture::candidate_slots, true);
    const auto picks = multiple_range_picks(serial, battle.inputs);
    OA_CHECK(std::any_of(picks.begin(), picks.end(), [](uint16_t id) { return id != 0; }));
    OA_CHECK(!serial.translucent_vertices.empty());
    OA_CHECK(
        std::any_of(serial.cards.casts.begin(), serial.cards.casts.end(), [](const auto& cast) {
            return cast.kind == oa::app::renderer_live_gpu::shadow_cast_cut;
        })
    );
    fixture.marker(MultipleModelRangeFixture::first_gap);
    // The new marker is a serial-gap atlas write after all worker packets join.
    compare(MultipleModelRangeFixture::candidate_slots, true);
    OA_CHECK(parallel.passes.models.materials_packed > 0);
    compare(MultipleModelRangeFixture::candidate_slots, true);
    OA_CHECK(serial.body_lod_counts.distance_markers == 1);
    OA_CHECK(
        std::any_of(serial.unit_ranges.begin(), serial.unit_ranges.end(), [&](const auto& range) {
            return range.unit_id == fixture.units[MultipleModelRangeFixture::first_gap].id &&
                   range.vertex_count == 6;
        })
    );
    // Change real posed points and separately supplied interpolated endpoints.
    const auto previous_positions = serial.cards.positions;
    fixture.bodies.instance.pieces()[1].translation.x += 65536 / 4;
    fixture.bodies.instance.rebuild_transforms();
    auto& animated = battle.draws.models[MultipleModelRangeFixture::second_gap + 2];
    animated.shown_vertices.resize(fixture.bodies.instance.pieces().size());
    for (std::size_t piece = 0; piece < fixture.bodies.instance.pieces().size(); ++piece)
        animated.shown_vertices[piece] =
            fixture.bodies.instance.pieces()[piece].transformed_vertices;
    for (auto& point : animated.shown_vertices[1])
        point.z += 65536 / 3;
    compare(MultipleModelRangeFixture::candidate_slots, true);
    OA_CHECK(serial.cards.positions != previous_positions);
    for (const bool procedural : {false, true}) {
        battle.inputs.shadow_index_templates = true;
        battle.inputs.shadow_index_procedural = procedural;
        compare(MultipleModelRangeFixture::candidate_slots, true);
        compare(MultipleModelRangeFixture::candidate_slots, true);
    }
    // Removing the actual gaps restores the original four-band single-region case.
    for (const auto index :
         {MultipleModelRangeFixture::first_gap,
          MultipleModelRangeFixture::second_gap,
          MultipleModelRangeFixture::suffix}) {
        fixture.units[index].flags = 0;
        fixture.units[index].state_flags = 0;
        battle.draws.models[index].shown_vertices.clear();
    }
    compare(MultipleModelRangeFixture::population, true);
    OA_CHECK(serial.body_lod_counts.distance_markers == 0);
    multiple_model_range_material_retries();
    multiple_model_range_textured_gap();
    multiple_model_range_geometry_budget();
}

} // namespace
