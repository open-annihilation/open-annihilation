// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

/// Mixed local sequences, including empty casts, beside their ordinary expansion.
struct ProceduralShadowFixture {
    ShadowTemplateFixture surface{};
    ModelFrame ordinary{};
    std::vector<ShadowIndexPattern> patterns{};
    std::vector<uint32_t> pattern_words{}, dynamic_words{};
    std::vector<ShadowIndexCast> sources{};
    std::vector<ShadowIndexRun> runs{};
    std::array<std::vector<uint32_t>, 3> local{
        std::vector<uint32_t>{2, 0, 1},
        std::vector<uint32_t>{0, 1, 2, 3, 4, 5},
        std::vector<uint32_t>{5, 3, 4}
    };

    explicit ProceduralShadowFixture(
        uint32_t warm_casts = 16, uint32_t pattern_count = 2, uint32_t population = 24
    ) {
        constexpr std::array<float, 18> shape{
            -3, 4, -2, 3, 4, -2, -3, 4, 2, 3, 4, -2, 3, 4, 2, -3, 4, 2
        };
        for (uint32_t type = 0; type < pattern_count; ++type) {
            for (uint32_t corner = 0; corner < 6; ++corner) {
                ordinary.corners.push_back(type * 6 + corner);
                ordinary.corners.push_back(0);
            }
            for (size_t coordinate = 0; coordinate < shape.size(); ++coordinate)
                ordinary.positions.push_back(
                    coordinate % 3 == 1 ? shape[coordinate] + float(type) * 0.5F
                                        : shape[coordinate] * (1 - float(type) * 0.125F)
                );
        }
        if (population > 64)
            for (auto& coordinate : ordinary.positions)
                coordinate *= 0.25F;
        ordinary.piece_frames = {0};
        ordinary.textures = {0, 0, 1, 1};
        for (uint32_t pattern = 0; pattern < pattern_count; ++pattern) {
            const auto first = static_cast<uint32_t>(pattern_words.size());
            patterns.push_back(
                {first, static_cast<uint32_t>(local[pattern].size()), pattern * 6, 6, 1}
            );
            for (uint32_t cast = 0; cast < model_run_instances; ++cast)
                for (const auto corner : local[pattern])
                    pattern_words.push_back((cast << model_corner_bits) | corner);
        }
        uint32_t hot{};
        for (uint32_t cast = 0; cast < population; ++cast) {
            if (cast % model_run_instances == 0) {
                hot = 0;
                ordinary.cast_runs.push_back(
                    {9, static_cast<uint32_t>(ordinary.cast_indices.size()), 0, cast}
                );
                runs.push_back({9, cast, 0, 0, 0, 0});
            }
            const float x =
                population > 64 ? 3.0F + float(cast % 16) * 3.75F : 5.25F + float(cast % 8) * 7;
            const float z =
                population > 64 ? 5.25F + float(cast / 16) * 2.75F : 10.5F + float(cast / 8) * 7;
            const bool empty =
                cast % model_run_instances == 0 || cast % 8 == 0 || cast + 1 == population;
            const bool warm = !empty && hot < warm_casts;
            const auto pattern = warm ? hot++ % pattern_count : cast % pattern_count;
            ordinary.instances.push_back(
                {pattern * 6, 6, 0, pattern_count * 6, 0, 1, 0, 1, 1, {x, 0, z}}
            );
            ordinary.casts.push_back(
                {cast, shadow_cast_picture, 0, 0, -100, 0.5F, float(65000U - cast) / 65535.0F, x, z}
            );
            const auto& words = local[pattern];
            const auto count = empty ? 0U : static_cast<uint32_t>(words.size());
            sources.push_back(
                {warm ? 0U : static_cast<uint32_t>(dynamic_words.size()),
                 count,
                 warm ? pattern + 1 : 0}
            );
            if (!empty) {
                if (!warm)
                    dynamic_words.insert(dynamic_words.end(), words.begin(), words.end());
                for (const auto corner : words)
                    ordinary.cast_indices.push_back(
                        ((cast % model_run_instances) << model_corner_bits) | corner
                    );
            }
            ++runs.back().cast_count;
            runs.back().index_count += count;
            ordinary.cast_runs.back().index_count += count;
        }
        ordinary.shadow_map = {0, 0, 1, ShadowTemplateFixture::edge, ShadowTemplateFixture::edge};
        ordinary.shadow_vertex = ordinary.shadow_ground = surface.ground.size();
    }

    ModelDraws draws(bool procedural) const {
        auto result = ordinary.draws(37);
        if (procedural) {
            result.cast_indices = {};
            result.cast_runs = {};
            result.shadow_index_encoding = ShadowIndexEncoding::procedural;
            result.shadow_index_runs = runs;
            result.shadow_index_casts = sources;
            result.shadow_dynamic_indices = dynamic_words;
            result.shadow_patterns = patterns;
            result.shadow_pattern_indices = pattern_words;
            result.shadow_patterns_revision = 51;
            result.shadow_patterns_type_revision = 37;
            result.logical_shadow_index_count = ordinary.cast_indices.size();
        }
        return result;
    }

    uint64_t warm_words() const {
        uint64_t result{};
        for (const auto& source : sources)
            if (source.pattern != 0)
                result += source.index_count;
        return result;
    }
};

/// Uses one immutable world, atlas and projection for both source representations.
Result procedural_shadow_frame(
    Context& context,
    const ProceduralShadowFixture& fixture,
    bool procedural,
    std::vector<uint8_t>& pixels,
    std::string& error,
    uint32_t aa = 1,
    const std::array<float, 12>& projection = classic_projection
) {
    const auto draws = fixture.draws(procedural);
    return render(
        context,
        fixture.surface.ground,
        fixture.surface.atlas,
        1,
        1,
        1,
        ShadowTemplateFixture::edge,
        ShadowTemplateFixture::edge,
        projection,
        pixels,
        error,
        aa,
        nullptr,
        nullptr,
        nullptr,
        {},
        nullptr,
        &draws
    );
}

/// Invalid local spans or bindings fail before any graphics device is opened.
void shadow_index_procedural_validation() {
    Context context;
    context.device_unavailable = true;
    std::vector<uint8_t> pixels;
    std::string error;
    const ProceduralShadowFixture valid;
    const auto invoke = [&](const ProceduralShadowFixture& fixture) {
        return procedural_shadow_frame(context, fixture, true, pixels, error);
    };
    OA_CHECK(invoke(valid) == Result::unavailable);
    const auto refuses = [&](const auto& change) {
        auto broken = valid;
        change(broken);
        pixels.assign(channels, full_channel);
        OA_CHECK(invoke(broken) == Result::failed);
        OA_CHECK(pixels.empty() && !error.empty());
        OA_CHECK(context.shadow_index_counts.logical_indices == 0);
    };
    refuses([](auto& fixture) { fixture.sources.pop_back(); });
    refuses([](auto& fixture) { fixture.runs[0].first_index = 1; });
    refuses([](auto& fixture) { fixture.runs[0].pattern = 1; });
    refuses([](auto& fixture) { fixture.runs[0].cast_count = 257; });
    refuses([](auto& fixture) { ++fixture.runs[0].index_count; });
    refuses([](auto& fixture) { fixture.sources[1].pattern = 4; });
    refuses([](auto& fixture) { fixture.sources[1].first_index = 1; });
    refuses([](auto& fixture) { ++fixture.sources[1].index_count; });
    refuses([](auto& fixture) { fixture.sources.back().first_index += 1; });
    refuses([](auto& fixture) { fixture.dynamic_words[0] |= 1U << model_corner_bits; });
    refuses([](auto& fixture) { fixture.dynamic_words[0] = 6; });
    refuses([](auto& fixture) { fixture.patterns[0].selected_mask = 2; });
    refuses([](auto& fixture) { fixture.ordinary.instances[1].corners = 1; });
    refuses([](auto& fixture) { fixture.ordinary.casts[1].kind = shadow_cast_cut; });
    // Every cast may be empty; the original run still covers them all.
    auto empty = valid;
    empty.dynamic_words.clear();
    empty.ordinary.cast_indices.clear();
    empty.ordinary.cast_runs[0].index_count = empty.runs[0].index_count = 0;
    for (auto& source : empty.sources)
        source = {};
    OA_CHECK(invoke(empty) == Result::unavailable);

    // A small retained source still pays the complete logical geometry budget.
    constexpr uint32_t corners = 96;
    constexpr uint32_t casts_at_limit = 32768;
    OA_CHECK(size_t{casts_at_limit} * corners == largest_vertex_count);
    auto budget = valid;
    budget.dynamic_words.clear();
    budget.patterns = {{0, corners, 0, 6, 1}};
    budget.pattern_words.clear();
    for (uint32_t cast = 0; cast < model_run_instances; ++cast)
        for (uint32_t triangle = 0; triangle < corners / 3; ++triangle)
            for (const uint32_t corner : {2U, 0U, 1U})
                budget.pattern_words.push_back((cast << model_corner_bits) | corner);
    auto shadow = budget.ordinary.casts.front();
    shadow.instance = 0;
    budget.ordinary.casts.assign(casts_at_limit, shadow);
    budget.sources.assign(casts_at_limit, {0, corners, 1});
    budget.runs.clear();
    for (uint32_t first = 0; first < casts_at_limit; first += model_run_instances)
        budget.runs.push_back({0, first, model_run_instances, 0, corners * model_run_instances, 0});
    auto accepted = budget.draws(true);
    accepted.instances = accepted.instances.first(1);
    accepted.logical_shadow_index_count = largest_vertex_count;
    OA_CHECK(shadow_index_detail::valid_procedural_draws(accepted));
    OA_CHECK(budget.pattern_words.size() * sizeof(uint32_t) == 96 * 1024);
    budget.ordinary.casts.push_back(shadow);
    budget.sources.push_back({0, corners, 1});
    budget.runs.push_back({0, casts_at_limit, 1, 0, corners, 0});
    for (const auto declared : {largest_vertex_count + corners, largest_vertex_count}) {
        auto rejected = budget.draws(true);
        rejected.instances = rejected.instances.first(1);
        rejected.logical_shadow_index_count = declared;
        OA_CHECK(!shadow_index_detail::valid_procedural_draws(rejected));
        pixels.assign(channels, full_channel);
        context.shadow_index_counts.logical_indices = 1;
        OA_CHECK(
            shadow_template_draws(context, budget.surface, rejected, pixels, error) ==
            Result::failed
        );
        OA_CHECK(pixels.empty() && !error.empty());
        OA_CHECK(context.shadow_index_counts.logical_indices == 0);
    }
}

/// Variable local lengths and empty casts preserve every pixel at the 15/16 boundary.
void shadow_index_procedural_readback() {
    std::vector<uint8_t> pixels, expected;
    std::string error;
    for (const uint32_t warm : {0U, 15U, 16U}) {
        ProceduralShadowFixture fixture(warm);
        Context legacy, context;
        const auto map = expected_shadow_map(fixture.ordinary, fixture.ordinary.shadow_map);
        OA_CHECK(*std::min_element(map.begin(), map.end()) < 1);
        for (uint32_t aa : {1U, 2U}) {
            OA_CHECK(
                procedural_shadow_frame(legacy, fixture, false, expected, error, aa) ==
                Result::success
            );
            OA_CHECK(
                procedural_shadow_frame(context, fixture, true, pixels, error, aa) ==
                Result::success
            );
            OA_CHECK(pixels == expected);
            const auto& counts = context.shadow_index_counts;
            OA_CHECK(counts.logical_indices == fixture.ordinary.cast_indices.size());
            OA_CHECK(counts.dynamic_indices + counts.cached_indices == counts.logical_indices);
            if (warm < shadow_template_minimum_casts) {
                OA_CHECK(counts.cached_indices == 0 && counts.cached_runs == 0);
                OA_CHECK(counts.mapping_uploaded_words == 0);
            } else {
                OA_CHECK(counts.cached_indices == fixture.warm_words() && counts.cached_runs == 1);
                OA_CHECK(counts.dynamic_indices != 0);
                OA_CHECK(counts.mapping_uploaded_words == 4 * fixture.sources.size() + 4);
            }
        }
        for (const bool templates : {false, true}) {
            Context fallback;
            fallback.shadow_templates_allowed = templates;
            fallback.shadow_procedural_allowed = !templates;
            OA_CHECK(
                procedural_shadow_frame(fallback, fixture, true, pixels, error) == Result::success
            );
            OA_CHECK(
                procedural_shadow_frame(legacy, fixture, false, expected, error) == Result::success
            );
            OA_CHECK(pixels == expected);
            OA_CHECK(fallback.shadow_index_counts.cached_indices == 0);
            OA_CHECK(fallback.shadow_index_counts.mapping_uploaded_words == 0);
        }
    }
    // Requested casts alone cannot pass the threshold while a third pattern is cold.
    ProceduralShadowFixture three(16, 3);
    Context progress, legacy;
    OA_CHECK(procedural_shadow_frame(legacy, three, false, expected, error) == Result::success);
    OA_CHECK(procedural_shadow_frame(progress, three, true, pixels, error) == Result::success);
    OA_CHECK(pixels == expected);
    OA_CHECK(progress.shadow_index_counts.admitted_patterns == 2);
    OA_CHECK(progress.shadow_index_counts.cached_indices == 0);
    OA_CHECK(progress.shadow_index_counts.mapping_uploaded_words == 0);
    OA_CHECK(procedural_shadow_frame(progress, three, true, pixels, error) == Result::success);
    OA_CHECK(pixels == expected);
    OA_CHECK(progress.shadow_index_counts.admitted_patterns == 1);
    OA_CHECK(progress.shadow_index_counts.cached_indices == three.warm_words());
    // The second original run starts at cast256 and remains one unsplit draw.
    ProceduralShadowFixture boundary(16, 2, 300);
    Context boundary_context;
    auto turned = classic_projection;
    turned[0] = turned[6] = 0.85F;
    turned[2] = 0.2F;
    turned[3] = 2;
    turned[4] = -0.1F;
    turned[7] = 8;
    for (const auto& projection : {classic_projection, turned}) {
        OA_CHECK(
            procedural_shadow_frame(legacy, boundary, false, expected, error, 2, projection) ==
            Result::success
        );
        OA_CHECK(
            procedural_shadow_frame(
                boundary_context, boundary, true, pixels, error, 2, projection
            ) == Result::success
        );
        OA_CHECK(pixels == expected);
        OA_CHECK(boundary_context.shadow_index_counts.cached_runs == 2);
        OA_CHECK(boundary_context.shadow_index_counts.cached_indices == boundary.warm_words());
        OA_CHECK(
            boundary_context.shadow_index_counts.mapping_uploaded_words ==
            4 * boundary.sources.size() + 8
        );
    }

    // One frame switches from a procedural full run to a nonempty indexed tail.
    ProceduralShadowFixture mixed(16, 2, 257);
    const auto& tail = mixed.local[0];
    mixed.sources.back() = {
        static_cast<uint32_t>(mixed.dynamic_words.size()), static_cast<uint32_t>(tail.size()), 0
    };
    mixed.dynamic_words.insert(mixed.dynamic_words.end(), tail.begin(), tail.end());
    mixed.ordinary.cast_indices.insert(mixed.ordinary.cast_indices.end(), tail.begin(), tail.end());
    mixed.ordinary.cast_runs.back().index_count = static_cast<uint32_t>(tail.size());
    mixed.runs.back().index_count = static_cast<uint32_t>(tail.size());
    Context mixed_context;
    OA_CHECK(procedural_shadow_frame(legacy, mixed, false, expected, error) == Result::success);
    OA_CHECK(procedural_shadow_frame(mixed_context, mixed, true, pixels, error) == Result::success);
    OA_CHECK(pixels == expected);
    OA_CHECK(mixed_context.shadow_index_counts.cached_runs == 1);
    OA_CHECK(mixed_context.shadow_index_counts.cached_indices == mixed.warm_words());
    OA_CHECK(mixed_context.shadow_index_counts.dynamic_indices >= tail.size());
    OA_CHECK(
        mixed_context.shadow_index_counts.mapping_uploaded_words == 4 * model_run_instances + 4
    );

    struct Stopped {};

    Context interrupted;
    const ProceduralShadowFixture fixture;
    const auto draws = fixture.draws(true);
    bool stopped{};
    try {
        (void)render(
            interrupted,
            fixture.surface.ground,
            fixture.surface.atlas,
            1,
            1,
            1,
            ShadowTemplateFixture::edge,
            ShadowTemplateFixture::edge,
            classic_projection,
            pixels,
            error,
            1,
            nullptr,
            nullptr,
            nullptr,
            [] { throw Stopped{}; },
            nullptr,
            &draws
        );
    } catch (const Stopped&) {
        stopped = true;
    }
    OA_CHECK(stopped && interrupted.shadow_index_counts.logical_indices == 0);
    OA_CHECK(procedural_shadow_frame(interrupted, fixture, true, pixels, error) == Result::success);
    OA_CHECK(procedural_shadow_frame(legacy, fixture, false, expected, error) == Result::success);
    OA_CHECK(pixels == expected);
    OA_CHECK(interrupted.shadow_index_counts.cache_hits == 1);
    OA_CHECK(interrupted.shadow_index_counts.admitted_patterns == 0);
}
