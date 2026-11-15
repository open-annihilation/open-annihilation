// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdio>
#include <span>

namespace {

/// Serializes packet contents without addresses, padding or opaque resource revisions.
struct InteriorCullSignature {
    uint64_t value{14695981039346656037ULL};

    void word(uint64_t input) {
        constexpr uint64_t prime = 1099511628211ULL;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value ^= (input >> shift) & 255;
            value *= prime;
        }
    }

    void number(float input) { word(std::bit_cast<uint32_t>(input)); }

    template <typename T>
    void plain(std::span<const T> input) {
        word(input.size());
        for (const auto byte : std::as_bytes(input)) {
            value ^= std::to_integer<uint8_t>(byte);
            value *= 1099511628211ULL;
        }
    }

    template <typename T>
    void plain(const std::vector<T>& input) {
        plain(std::span<const T>{input});
    }

    void runs(const std::vector<oa::app::renderer_live_gpu::ModelRun>& input) {
        word(input.size());
        for (const auto& run : input) {
            word(run.vertex);
            word(run.first_index);
            word(run.index_count);
            word(run.first_instance);
        }
    }

    void ranges(const std::vector<live::UnitRange>& input) {
        word(input.size());
        for (const auto& range : input) {
            word(range.unit_id);
            word(range.first_vertex);
            word(range.vertex_count);
        }
    }
};

/// Covers ordered packet data, emitted surfaces, pick bounds and sampled command picks.
uint64_t interior_cull_signature(const live::Scene& scene, const live::Inputs& in) {
    namespace gpu = oa::app::renderer_live_gpu;
    static_assert(sizeof(gpu::Vertex) == 18 * sizeof(float));
    static_assert(sizeof(gpu::ModelInstance) == 13 * sizeof(uint32_t));
    static_assert(gpu::ModelInstance{}.root_prefix == 0);
    static_assert(sizeof(gpu::ShadowCast) == 9 * sizeof(uint32_t));
    static_assert(sizeof(gpu::MarkerInstance) == 16 * sizeof(float));
    InteriorCullSignature result;
    result.plain(scene.vertices);
    result.plain(scene.translucent_vertices);
    result.word(scene.card_ground.vertex_count);
    result.word(scene.card_ground.next_first);
    result.word(scene.card_ground.next_count);
    result.plain(scene.card_ground.next);
    result.ranges(scene.unit_ranges);
    result.ranges(scene.translucent_ranges);
    result.word(scene.terrain_vertex_count);
    result.word(scene.model_count);
    result.word(scene.shadow_count);
    result.word(scene.texture_width);
    result.word(scene.texture_height);
    result.plain(scene.texture_rgba);
    result.plain(scene.card_types.records);
    const auto& cards = scene.cards;
    result.plain(cards.instances);
    result.plain(cards.positions);
    result.plain(cards.piece_frames);
    result.plain(cards.textures);
    result.plain(cards.slot_states);
    result.word(cards.tables.size());
    for (const auto& table : cards.tables) {
        result.word(table.type);
        result.word(table.team);
        result.word(table.first);
    }
    result.plain(cards.indices);
    result.runs(cards.runs);
    result.plain(cards.casts);
    result.plain(cards.cast_indices);
    result.runs(cards.cast_runs);
    result.plain(cards.markers);
    result.word(cards.marker_runs.size());
    for (const auto& run : cards.marker_runs) {
        result.word(run.vertex);
        result.word(run.first_instance);
        result.word(run.instance_count);
    }
    for (const float value : cards.marker_uv)
        result.number(value);
    result.word(cards.vertices);
    result.word(cards.shadow_vertex);
    result.word(cards.shadow_ground);
    result.number(cards.shadow_map.left);
    result.number(cards.shadow_map.top);
    result.number(cards.shadow_map.scale);
    result.word(cards.shadow_map.width);
    result.word(cards.shadow_map.height);
    result.word(cards.units.size());
    for (const auto& unit : cards.units) {
        result.word(unit.unit_id);
        result.word(unit.vertex);
        result.word(unit.instance);
        result.word(unit.type);
        result.word(unit.first_index);
        result.word(unit.index_count);
        result.number(unit.left);
        result.number(unit.top);
        result.number(unit.right);
        result.number(unit.bottom);
        result.word(unit.bounded);
        result.word(unit.marker);
        result.word(unit.body_lod);
    }
    const auto& counts = scene.body_lod_counts;
    result.word(counts.full_mesh_models);
    result.word(counts.body_lod_models);
    result.word(counts.distance_markers);
    result.word(counts.budget_markers);
    result.word(counts.full_mesh_vertices);
    result.word(counts.body_lod_vertices);
    result.word(counts.shadow_vertices);
    const auto expanded = card_frame(scene);
    result.plain(expanded.vertices);
    result.plain(expanded.units);
    constexpr std::array<float, 7> sample{0, 16, 32, 48, 64, 96, 128};
    for (const auto y : sample)
        for (const auto x : sample) {
            uint16_t picked{};
            float nearest = -std::numeric_limits<float>::infinity();
            for (size_t first = 0; first + 2 < expanded.vertices.size(); first += 3) {
                if (expanded.units[first] == 0)
                    continue;
                std::array<std::array<float, 3>, 3> triangle{};
                for (size_t corner = 0; corner < triangle.size(); ++corner) {
                    const auto& point = expanded.vertices[first + corner];
                    for (size_t row = 0; row < triangle[corner].size(); ++row) {
                        const auto offset = row * 4;
                        triangle[corner][row] = in.clip_projection[offset] * point.x +
                                                in.clip_projection[offset + 1] * point.height +
                                                in.clip_projection[offset + 2] * point.z +
                                                in.clip_projection[offset + 3];
                    }
                }
                std::array<float, 3> weights{};
                float depth{};
                if (oa::app::renderer_live_camera::triangle_hit(triangle, x, y, weights, depth) &&
                    depth >= nearest) {
                    nearest = depth;
                    picked = expanded.units[first];
                }
            }
            result.word(picked);
            if (picked != 0)
                result.number(nearest);
        }
    return result.value;
}

/// Fixed source cases supply a same-policy corpus for a private full-scan reference.
void interior_model_cull_tests() {
    namespace runtime = oa::sim::model_runtime;
    constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
    enum class Expected { ordinary, body, full, reflection };
    const auto check = [](const char* name,
                          BodyLodFixture& fixture,
                          Expected expected = Expected::ordinary) -> uint64_t {
        auto& battle = fixture.battle;
        live::Scene first, repeated;
        const bool built = live::build(battle.inputs, first, battle.error);
        OA_CHECK(built);
        if (!built) {
            std::printf("INTERIOR_CULL_SIGNATURE %s refused\n", name);
            return 0;
        }
        if (expected == Expected::body) {
            OA_CHECK(first.body_lod_counts.body_lod_models != 0);
            OA_CHECK(!first.cards.units.empty() && !first.cards.cast_indices.empty());
        } else if (expected == Expected::full) {
            OA_CHECK(first.body_lod_counts.full_mesh_models != 0);
            OA_CHECK(first.body_lod_counts.body_lod_models == 0);
        } else if (expected == Expected::reflection) {
            OA_CHECK(
                std::any_of(first.vertices.begin(), first.vertices.end(), [](const auto& point) {
                    return point.terrain_size == -3 && point.opacity > 0;
                })
            );
        }
        OA_CHECK(live::build(battle.inputs, repeated, battle.error));
        const auto signature = interior_cull_signature(first, battle.inputs);
        OA_CHECK(interior_cull_signature(repeated, battle.inputs) == signature);
        std::printf(
            "INTERIOR_CULL_SIGNATURE %s %016llx models=%u bodies=%u shadows=%u\n",
            name,
            static_cast<unsigned long long>(signature),
            first.model_count,
            first.body_lod_counts.body_lod_models,
            first.shadow_count
        );
        return signature;
    };

    // Real moving local poses and presentation-only vertices keep identical policy.
    for (const bool shown : {false, true}) {
        BodyLodFixture fixture;
        fixture.instance.pieces()[1].rotation.xz = 9000;
        fixture.instance.pieces()[1].translation.x = fixed / 3;
        fixture.instance.pieces()[1].flags &= ~static_cast<uint16_t>(runtime::PieceFlag::cached);
        fixture.instance.rebuild_transforms({0, 2300, 0});
        if (shown) {
            auto& draw = fixture.battle.draws.models[0];
            draw.shown_vertices.resize(fixture.instance.pieces().size());
            for (size_t piece = 0; piece < draw.shown_vertices.size(); ++piece)
                draw.shown_vertices[piece] = fixture.instance.pieces()[piece].transformed_vertices;
            for (auto& point : draw.shown_vertices[1])
                point.x += 2 * fixed;
            draw.shown_position = fixture.unit.position;
        }
        check(shown ? "animated-shown-body" : "animated-raw-body", fixture, Expected::body);
        fixture.projection(4);
        check(shown ? "animated-shown-full" : "animated-raw-full", fixture, Expected::full);
        fixture.battle.inputs.models_on_card = false;
        const auto cpu =
            check(shown ? "animated-shown-cpu" : "animated-raw-cpu", fixture, Expected::full);
        fixture.battle.inputs.body_lod_enabled = false;
        const auto legacy =
            check(shown ? "legacy-shown-cpu" : "legacy-raw-cpu", fixture, Expected::full);
        OA_CHECK(cpu == legacy);
    }

    // The two screen axes can overlap only after several individually outside points.
    {
        BodyLodFixture fixture;
        fixture.battle.inputs.unit_marker_pixels = 0;
        auto& draw = fixture.battle.draws.models[0];
        draw.shown_vertices.resize(fixture.instance.pieces().size());
        for (size_t piece = 0; piece < draw.shown_vertices.size(); ++piece)
            draw.shown_vertices[piece] = fixture.instance.pieces()[piece].transformed_vertices;
        draw.shown_vertices[0][0] = {-300 * fixed, 0, -300 * fixed};
        draw.shown_vertices[0][1] = {300 * fixed, 0, 300 * fixed};
        check("split-axis-prefix", fixture);
        draw.shown_vertices[0][0] = {-300 * fixed, 0, 0};
        check("outside-then-inside", fixture);
    }

    // Keep the adaptive prepass off so the model's own exact edge scan is reached.
    for (const float pan : {-512.0F, 512.0F}) {
        BodyLodFixture fixture;
        fixture.battle.inputs.unit_marker_pixels = 0;
        fixture.battle.inputs.clip_projection[3] += pan;
        check(pan < 0 ? "outside-left" : "outside-right", fixture);
        fixture.projection(0.5F);
        fixture.battle.inputs.clip_projection[7] += pan;
        check(pan < 0 ? "outside-top" : "outside-bottom", fixture);
    }
    for (const float edge : {0.0F, 128.0F})
        for (const float direction : {-1.0F, 0.0F, 1.0F}) {
            BodyLodFixture fixture;
            fixture.battle.inputs.unit_marker_pixels = 0;
            const auto point = fixture.instance.pieces()[0].transformed_vertices.front();
            const float x = fixture.unit.position.x / float(fixed) + point.x / float(fixed);
            const float margin = 0.5F;
            const float target = edge == 0 ? -margin : edge + margin;
            const float translation = target - 0.5F * x;
            fixture.battle.inputs.clip_projection[3] =
                direction == 0 ? translation
                               : std::nextafter(
                                     translation,
                                     direction < 0 ? -std::numeric_limits<float>::infinity()
                                                   : std::numeric_limits<float>::infinity()
                                 );
            check(
                edge == 0 ? (direction < 0   ? "left-margin-below"
                             : direction > 0 ? "left-margin-above"
                                             : "left-margin-equal")
                          : (direction < 0   ? "right-margin-below"
                             : direction > 0 ? "right-margin-above"
                                             : "right-margin-equal"),
                fixture
            );
        }

    {
        BodyLodFixture fixture;
        fixture.battle.inputs.clip_width = fixture.battle.inputs.clip_height = 0;
        check("coarse-crop", fixture);
        fixture.unit.position.y = 300 * fixed;
        check("coarse-height-fallback", fixture);
    }
    {
        BodyLodFixture fixture;
        for (auto& piece : fixture.instance.pieces())
            piece.flags &= ~static_cast<uint16_t>(runtime::PieceFlag::visible);
        check("hidden-pieces", fixture);
        for (size_t piece = 0; piece < fixture.source->objects.size(); ++piece) {
            fixture.source->objects[piece].vertices.clear();
            fixture.source->objects[piece].primitives.clear();
            fixture.prepared.objects[piece].primitives.clear();
        }
        fixture.instance = runtime::make_instance(fixture.source);
        fixture.instance.rebuild_transforms({});
        fixture.state.image = {};
        fixture.battle.draws.models[0].plan.model.from_image = false;
        check("empty-points", fixture);
    }
    {
        BodyLodFixture fixture;
        oa::UnitDef definition{};
        definition.flags = OA_UNIT_DEF_FLAG_FLOATER;
        fixture.battle.draws.models[0].model.def = &definition;
        fixture.battle.inputs.sea_level = fixture.battle.inputs.water.sea_level = 24;
        fixture.battle.inputs.water.detail = fixture.battle.inputs.water.reflections = 1;
        fixture.battle.inputs.view_direction = {0, -1, -1};
        fixture.unit.position.y = 24 * fixed;
        fixture.projection(2);
        fixture.battle.inputs.clip_projection[7] = -32;
        check("reflection-only-ingress", fixture, Expected::reflection);
    }
    {
        BodyLodFixture fixture;
        fixture.battle.inputs.unit_marker_pixels = 0;
        fixture.battle.inputs.clip_projection[0] = std::numeric_limits<float>::max();
        fixture.battle.inputs.clip_projection[3] = 0;
        auto& draw = fixture.battle.draws.models[0];
        draw.shown_vertices.resize(fixture.instance.pieces().size());
        for (size_t piece = 0; piece < draw.shown_vertices.size(); ++piece)
            draw.shown_vertices[piece] = fixture.instance.pieces()[piece].transformed_vertices;
        draw.shown_vertices[0][0].x = -fixture.unit.position.x;
        check("later-projection-overflow", fixture);
        fixture.battle.inputs.clip_projection[0] = std::numeric_limits<float>::quiet_NaN();
        check("nonfinite-coarse-fallback", fixture);
    }
}

} // namespace
