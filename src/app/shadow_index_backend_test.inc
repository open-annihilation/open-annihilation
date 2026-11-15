// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

/// A small mixed shadow stream and its independently expanded ordinary form.
struct ShadowTemplateFixture {
    static constexpr uint32_t edge = 64;
    static constexpr uint32_t corners_per_cast = 3;
    static constexpr uint32_t hot_casts = shadow_template_minimum_casts;
    static constexpr uint32_t first_type_corner = 3;
    static constexpr uint32_t first_cast_depth = 65000;
    static constexpr float depth_divisor = 65535;
    ModelFrame ordinary{};
    std::vector<Vertex> ground{
        {0, 0, 0, 0.5F, 0.5F},
        {edge, 0, 0, 0.5F, 0.5F},
        {0, 0, edge, 0.5F, 0.5F},
        {edge, 0, 0, 0.5F, 0.5F},
        {edge, 0, edge, 0.5F, 0.5F},
        {0, 0, edge, 0.5F, 0.5F}
    };
    std::vector<uint8_t> atlas{200, 160, 120, 255};
    std::vector<ShadowIndexPattern> patterns{};
    std::vector<uint32_t> pattern_indices{};
    std::vector<ShadowIndexRun> runs{};
    std::vector<uint32_t> dynamic_indices{};
    uint64_t revision{51};
    uint64_t type_revision{37};
    uint64_t patterns_type_revision{37};

    /// Places a dynamic picture before the hot bands and a cut/sheared pair after them.
    explicit ShadowTemplateFixture(uint32_t hot_bands = 1) {
        ordinary.corners = {0, 0, 1, 0, 2, 0, 0, 0, 1, 0, 2, 0};
        ordinary.positions = {0, 4, 0, 5, 4, 0, 0, 4, 5};
        ordinary.piece_frames = {0};
        ordinary.textures = {0, 0, 1, 1};
        const uint32_t tail = 1 + hot_casts * hot_bands;
        const uint32_t count = tail + 3;
        for (uint32_t cast = 0; cast < count; ++cast) {
            const float x = 3.25F + float(cast % 8) * 5.125F;
            const float z = 10.5F + float(cast / 8) * 5.25F;
            ordinary.instances.push_back(
                {first_type_corner, corners_per_cast, 0, 3, 0, 1, 0, 1, 1, {x, 0, z}}
            );
            ordinary.casts.push_back(
                {cast, shadow_cast_picture, 0, 0, -100, 0.5F,
                 float(first_cast_depth - cast) / depth_divisor, x, z}
            );
            for (uint32_t corner = 0; corner < corners_per_cast; ++corner)
                ordinary.cast_indices.push_back((cast << model_corner_bits) | corner);
        }
        ordinary.casts[tail].kind = shadow_cast_cut;
        ordinary.casts[tail].light = 1;
        ordinary.casts[tail + 1] = ordinary.casts[tail];
        ordinary.casts[tail + 1].kind = shadow_cast_sheared;
        ordinary.casts[tail + 1].light = 0.5F;
        ordinary.casts[tail + 2].least_height = 100;
        ordinary.cast_runs.push_back({0, 0, count * corners_per_cast, 0});
        ordinary.shadow_map = {0, 0, 1, edge, edge};
        ordinary.shadow_vertex = ordinary.shadow_ground = ground.size();
        runs.push_back({0, 0, 1, 0, corners_per_cast, 0});
        dynamic_indices = {0, 1, 2};
        for (uint32_t band = 0; band < hot_bands; ++band) {
            add_pattern();
            runs.push_back(
                {0, 1 + band * hot_casts, hot_casts, 0,
                 hot_casts * corners_per_cast, band + 1}
            );
        }
        runs.push_back({0, tail, 3, corners_per_cast, 3 * corners_per_cast, 0});
        for (uint32_t relative = 0; relative < 3; ++relative)
            for (uint32_t corner = 0; corner < corners_per_cast; ++corner)
                dynamic_indices.push_back((relative << model_corner_bits) | corner);
    }

    /// Appends one complete immutable template without changing its earlier prefix.
    void add_pattern() {
        const auto first = static_cast<uint32_t>(pattern_indices.size());
        patterns.push_back({first, corners_per_cast, first_type_corner, corners_per_cast, 1});
        for (uint32_t relative = 0; relative < model_run_instances; ++relative)
            for (uint32_t corner = 0; corner < corners_per_cast; ++corner)
                pattern_indices.push_back((relative << model_corner_bits) | corner);
    }

    /// Borrows either the original expanded indices or the ordered compact stream.
    [[nodiscard]] ModelDraws draws(bool compact) const {
        auto result = ordinary.draws(type_revision);
        if (compact) {
            result.cast_indices = {};
            result.cast_runs = {};
            result.shadow_index_encoding = ShadowIndexEncoding::compact;
            result.shadow_index_runs = runs;
            result.shadow_dynamic_indices = dynamic_indices;
            result.shadow_patterns = patterns;
            result.shadow_pattern_indices = pattern_indices;
            result.shadow_patterns_revision = revision;
            result.shadow_patterns_type_revision = patterns_type_revision;
            result.logical_shadow_index_count = ordinary.cast_indices.size();
        }
        return result;
    }
};

/// Draws supplied shadow packets over the fixture's unchanged ground.
Result shadow_template_draws(
    Context& context, const ShadowTemplateFixture& fixture, const ModelDraws& draws,
    std::vector<uint8_t>& pixels, std::string& error, uint32_t supersample = 1
) {
    return render(
        context, fixture.ground, fixture.atlas, 1, 1, 1,
        ShadowTemplateFixture::edge, ShadowTemplateFixture::edge,
        classic_projection, pixels, error, supersample, nullptr, nullptr, nullptr, {},
        nullptr, &draws
    );
}

/// Draws the same ground and shadows through either index representation.
Result shadow_template_frame(
    Context& context, const ShadowTemplateFixture& fixture, bool compact,
    std::vector<uint8_t>& pixels, std::string& error, uint32_t supersample = 1
) {
    return shadow_template_draws(
        context, fixture, fixture.draws(compact), pixels, error, supersample
    );
}

/// Refuses unsafe compact ranges and bindings before opening a graphics device.
void shadow_index_stream_validation() {
    Context context;
    context.device_unavailable = true;
    std::vector<uint8_t> pixels;
    std::string error;
    const ShadowTemplateFixture valid;
    const auto result = [&](const ShadowTemplateFixture& fixture) {
        return shadow_template_frame(context, fixture, true, pixels, error);
    };
    OA_CHECK(result(valid) == Result::unavailable);
    const auto refuses = [&](const auto& change) {
        auto broken = valid;
        ++broken.revision;
        change(broken);
        pixels.assign(channels, full_channel);
        OA_CHECK(result(broken) == Result::failed);
        OA_CHECK(pixels.empty());
        OA_CHECK(!error.empty());
        OA_CHECK(context.shadow_index_counts.logical_indices == 0);
    };
    refuses([](auto& fixture) { fixture.revision = 0; });
    refuses([](auto& fixture) { ++fixture.type_revision; });
    refuses([](auto& fixture) { fixture.runs[0].first_cast = 1; });
    refuses([](auto& fixture) { fixture.runs[1].first_cast = 0; });
    refuses([](auto& fixture) { fixture.runs[1].cast_count = 0; });
    refuses([](auto& fixture) { fixture.runs[1].cast_count = model_run_instances + 1; });
    refuses([](auto& fixture) { fixture.runs[1].first_index = 1; });
    refuses([](auto& fixture) { --fixture.runs[1].index_count; });
    refuses([](auto& fixture) { fixture.runs[1].pattern = 2; });
    refuses([](auto& fixture) { fixture.runs.pop_back(); });
    refuses([](auto& fixture) { fixture.runs[0].first_index = 100; });
    refuses([](auto& fixture) { fixture.dynamic_indices[0] = 1U << model_corner_bits; });
    refuses([](auto& fixture) { fixture.dynamic_indices[0] = 3; });
    refuses([](auto& fixture) { fixture.patterns[0].first_index = 1; });
    refuses([](auto& fixture) { fixture.patterns[0].indices_per_cast = 2; });
    refuses([](auto& fixture) { fixture.patterns[0].type_corners = 0; });
    refuses([](auto& fixture) { fixture.patterns[0].type_corner_count = 2; });
    refuses([](auto& fixture) { fixture.patterns[0].selected_mask = 0; });
    refuses([](auto& fixture) { fixture.pattern_indices[0] = 3; });
    refuses([](auto& fixture) { fixture.pattern_indices[1] = 1U << model_corner_bits; });
    refuses([](auto& fixture) { fixture.pattern_indices.back() ^= 1; });
    refuses([](auto& fixture) { fixture.pattern_indices.pop_back(); });
    refuses([](auto& fixture) {
        while (fixture.patterns.size() <= shadow_template_pattern_limit)
            fixture.add_pattern();
    });
    refuses([](auto& fixture) {
        fixture.pattern_indices.resize(shadow_template_byte_limit / sizeof(uint32_t) + 1);
    });
    refuses([](auto& fixture) { fixture.ordinary.casts[1].instance = 100; });
    refuses([](auto& fixture) { fixture.ordinary.instances[1].corners = 0; });
    refuses([](auto& fixture) { fixture.ordinary.instances[1].piece_count = 65; });
    refuses([](auto& fixture) { fixture.ordinary.casts[1].light = 2; });
    refuses([](auto& fixture) {
        fixture.ordinary.instances[1].origin[0] = std::numeric_limits<float>::quiet_NaN();
    });
    const auto refuses_draws = [&](const auto& change) {
        auto draws = valid.draws(true);
        change(draws);
        pixels.assign(channels, full_channel);
        OA_CHECK(shadow_template_draws(context, valid, draws, pixels, error) == Result::failed);
        OA_CHECK(pixels.empty());
    };
    refuses_draws([](auto& draws) { ++draws.logical_shadow_index_count; });
    refuses_draws([](auto& draws) { draws.shadow_patterns_type_revision = 0; });
    refuses_draws([](auto& draws) {
        draws.shadow_index_encoding = static_cast<ShadowIndexEncoding>(255);
    });
    refuses_draws([&](auto& draws) { draws.cast_indices = valid.ordinary.cast_indices; });
    refuses_draws([&](auto& draws) { draws.cast_runs = valid.ordinary.cast_runs; });
    // A wholly hidden ordinary run still covers its casts, with zero indices.
    auto hidden_draws = valid.draws(true);
    const ShadowIndexRun hidden{
        0, 0, static_cast<uint32_t>(valid.ordinary.casts.size()), 0, 0, 0
    };
    hidden_draws.shadow_index_runs = std::span(&hidden, 1);
    hidden_draws.shadow_dynamic_indices = {};
    hidden_draws.shadow_patterns = {};
    hidden_draws.shadow_pattern_indices = {};
    hidden_draws.shadow_patterns_revision = hidden_draws.shadow_patterns_type_revision = 0;
    hidden_draws.logical_shadow_index_count = 0;
    OA_CHECK(shadow_template_draws(
        context, valid, hidden_draws, pixels, error
    ) == Result::unavailable);
    // A post-plan resource failure leaves residency intact but chooses dynamic
    // uploads. Final-path accounting must replace the earlier planned hit.
    std::array<bool, shadow_template_pattern_limit> resident{}, admitted{};
    resident[0] = true;
    const auto planned = shadow_index_detail::draw_counts(valid.draws(true), resident, admitted, true);
    OA_CHECK(planned.cache_hits == 1 && planned.cached_runs == 1);
    const auto retried = shadow_index_detail::draw_counts(valid.draws(true), resident, admitted, false);
    OA_CHECK(retried.cache_hits == 0 && retried.cache_misses == 1);
    OA_CHECK(retried.cached_runs == 0 && retried.cached_indices == 0);
    OA_CHECK(retried.fallback_runs == 1);
    OA_CHECK(retried.dynamic_indices == valid.ordinary.cast_indices.size());
}

/// Retained templates, bounded admissions and capability fallback preserve every pixel.
void shadow_index_stream_readback() {
    ShadowTemplateFixture fixture;
    Context legacy_context;
    Context context;
    Context fallback_context;
    fallback_context.shadow_templates_allowed = false;
    std::vector<uint8_t> expected, pixels, fallback;
    std::string error;
    constexpr uint64_t hot_indices =
        ShadowTemplateFixture::hot_casts * ShadowTemplateFixture::corners_per_cast;
    constexpr uint64_t template_words =
        model_run_instances * ShadowTemplateFixture::corners_per_cast;
    const auto expected_map = expected_shadow_map(fixture.ordinary, fixture.ordinary.shadow_map);
    OA_CHECK(*std::min_element(expected_map.begin(), expected_map.end()) < 1);
    for (uint32_t supersample : {1U, 2U}) {
        OA_CHECK(shadow_template_frame(
            legacy_context, fixture, false, expected, error, supersample
        ) == Result::success);
        constexpr uint32_t shadow_inside_x = 10, shadow_inside_y = 10;
        const auto shadow_pixel =
            (shadow_inside_y * ShadowTemplateFixture::edge + shadow_inside_x) * channels;
        OA_CHECK(expected.size() > shadow_pixel && expected[shadow_pixel] < fixture.atlas[0]);
        OA_CHECK(shadow_template_frame(
            context, fixture, true, pixels, error, supersample
        ) == Result::success);
        OA_CHECK(pixels == expected);
        const auto counts = context.shadow_index_counts;
        OA_CHECK(counts.logical_indices == fixture.ordinary.cast_indices.size());
        OA_CHECK(counts.cached_indices == hot_indices && counts.cached_runs == 1);
        OA_CHECK(counts.dynamic_indices == counts.logical_indices - hot_indices);
        OA_CHECK(counts.requested_runs == 1 && counts.fallback_runs == 0);
        if (supersample == 1) {
            OA_CHECK(counts.admitted_patterns == 1 && counts.cached_patterns == 1);
            OA_CHECK(counts.template_uploaded_indices == template_words);
        } else {
            OA_CHECK(counts.cache_hits == 1 && counts.admitted_patterns == 0);
            OA_CHECK(counts.template_uploaded_indices == 0);
        }
        OA_CHECK(shadow_template_frame(
            fallback_context, fixture, true, fallback, error, supersample
        ) == Result::success);
        OA_CHECK(fallback == expected);
        const auto fallback_counts = fallback_context.shadow_index_counts;
        OA_CHECK(fallback_counts.logical_indices == counts.logical_indices);
        OA_CHECK(fallback_counts.cached_indices == 0 && fallback_counts.cached_runs == 0);
        OA_CHECK(fallback_counts.dynamic_indices == counts.logical_indices);
        OA_CHECK(fallback_counts.fallback_runs == 1 && fallback_counts.admitted_patterns == 0);
        OA_CHECK(fallback_counts.template_uploaded_indices == 0);
    }
    // Capability rejection also reclassifies an already resident request as a
    // dynamic fallback; retained residency alone is not a successful cache hit.
    context.shadow_templates_allowed = false;
    OA_CHECK(shadow_template_frame(context, fixture, true, pixels, error) == Result::success);
    OA_CHECK(shadow_template_frame(legacy_context, fixture, false, expected, error) == Result::success);
    OA_CHECK(pixels == expected);
    OA_CHECK(context.shadow_index_counts.cache_hits == 0);
    OA_CHECK(context.shadow_index_counts.cache_misses == 1);
    OA_CHECK(context.shadow_index_counts.cached_runs == 0);
    OA_CHECK(context.shadow_index_counts.fallback_runs == 1);
    context.shadow_templates_allowed = true;
    // Appending descriptors preserves the resident prefix and its exact old pixels.
    fixture.add_pattern();
    fixture.runs[1].pattern = 2;
    OA_CHECK(shadow_template_frame(context, fixture, true, pixels, error) == Result::success);
    OA_CHECK(shadow_template_frame(legacy_context, fixture, false, expected, error) == Result::success);
    OA_CHECK(pixels == expected);
    OA_CHECK(context.shadow_index_counts.admitted_patterns == 1);
    OA_CHECK(context.shadow_index_counts.cached_patterns == 2);
    OA_CHECK(context.shadow_index_counts.template_uploaded_indices == template_words);
    // Replacing a store uses a new revision and admits its current pattern afresh.
    ++fixture.revision;
    OA_CHECK(shadow_template_frame(context, fixture, true, pixels, error) == Result::success);
    OA_CHECK(pixels == expected);
    OA_CHECK(context.shadow_index_counts.cache_hits == 0);
    OA_CHECK(context.shadow_index_counts.admitted_patterns == 1);
    OA_CHECK(shadow_template_frame(context, fixture, false, pixels, error) == Result::success);
    OA_CHECK(pixels == expected);
    OA_CHECK(context.shadow_index_counts.cached_indices == 0);
    OA_CHECK(context.shadow_index_counts.cached_patterns == 1);
    OA_CHECK(context.shadow_index_counts.cache_bytes == template_words * sizeof(uint32_t));

    // Both submission paths queue their uploads before calling external work.
    // A throwing caller leaves valid queued residency, which the next frame
    // can reuse while still publishing no counts for the interrupted attempt.
    struct CallbackStopped {};
    for (const bool keep : {false, true}) {
        Context submitted;
        submitted.keep_on_card = keep;
        bool stopped{};
        const auto draws = fixture.draws(true);
        try {
            (void)render(
                submitted, fixture.ground, fixture.atlas, 1, 1, 1,
                ShadowTemplateFixture::edge, ShadowTemplateFixture::edge,
                classic_projection, pixels, error, 1, nullptr, nullptr, nullptr,
                [] { throw CallbackStopped{}; }, nullptr, &draws
            );
        } catch (const CallbackStopped&) {
            stopped = true;
        }
        OA_CHECK(stopped);
        OA_CHECK(submitted.shadow_index_counts.logical_indices == 0);
        submitted.keep_on_card = false;
        OA_CHECK(shadow_template_frame(submitted, fixture, true, pixels, error) == Result::success);
        OA_CHECK(pixels == expected);
        OA_CHECK(submitted.shadow_index_counts.cache_hits == 1);
        OA_CHECK(submitted.shadow_index_counts.admitted_patterns == 0);
        OA_CHECK(submitted.shadow_index_counts.template_uploaded_indices == 0);
    }

    // Only two new patterns may enter per frame; the remaining run uses the
    // same dynamic fallback as capability rejection, then enters next frame.
    ShadowTemplateFixture budget(3);
    Context budget_context;
    OA_CHECK(shadow_template_frame(legacy_context, budget, false, expected, error) == Result::success);
    OA_CHECK(shadow_template_frame(budget_context, budget, true, pixels, error) == Result::success);
    OA_CHECK(pixels == expected);
    OA_CHECK(budget_context.shadow_index_counts.admitted_patterns == 2);
    OA_CHECK(budget_context.shadow_index_counts.cached_runs == 2);
    OA_CHECK(budget_context.shadow_index_counts.fallback_runs == 1);
    OA_CHECK(budget_context.shadow_index_counts.cached_indices == 2 * hot_indices);
    OA_CHECK(budget_context.shadow_index_counts.template_uploaded_indices == 2 * template_words);
    OA_CHECK(shadow_template_frame(budget_context, budget, true, pixels, error) == Result::success);
    OA_CHECK(pixels == expected);
    OA_CHECK(budget_context.shadow_index_counts.admitted_patterns == 1);
    OA_CHECK(budget_context.shadow_index_counts.cached_runs == 3);
    OA_CHECK(budget_context.shadow_index_counts.cache_hits == 2);
    OA_CHECK(budget_context.shadow_index_counts.fallback_runs == 0);

    // Two individually admissible templates together exceed the byte allowance,
    // so the second request remains dynamic despite the two-pattern allowance.
    ShadowTemplateFixture byte_budget(2);
    constexpr uint32_t repeated_indices = 600;
    byte_budget.pattern_indices.clear();
    for (auto& pattern : byte_budget.patterns) {
        pattern.first_index = static_cast<uint32_t>(byte_budget.pattern_indices.size());
        pattern.indices_per_cast = repeated_indices;
        for (uint32_t relative = 0; relative < model_run_instances; ++relative)
            for (uint32_t at = 0; at < repeated_indices; ++at)
                byte_budget.pattern_indices.push_back(
                    relative << model_corner_bits | at % ShadowTemplateFixture::corners_per_cast
                );
    }
    for (auto& run : byte_budget.runs)
        if (run.pattern != 0)
            run.index_count = repeated_indices * run.cast_count;
    byte_budget.ordinary.cast_indices.clear();
    for (uint32_t cast = 0; cast < byte_budget.ordinary.casts.size(); ++cast) {
        const auto count = cast > 0 && cast <= 2 * ShadowTemplateFixture::hot_casts
                               ? repeated_indices
                               : ShadowTemplateFixture::corners_per_cast;
        for (uint32_t at = 0; at < count; ++at)
            byte_budget.ordinary.cast_indices.push_back(
                cast << model_corner_bits | at % ShadowTemplateFixture::corners_per_cast
            );
    }
    byte_budget.ordinary.cast_runs[0].index_count =
        static_cast<uint32_t>(byte_budget.ordinary.cast_indices.size());
    Context byte_context;
    OA_CHECK(shadow_template_frame(legacy_context, byte_budget, false, expected, error) == Result::success);
    OA_CHECK(shadow_template_frame(byte_context, byte_budget, true, pixels, error) == Result::success);
    OA_CHECK(pixels == expected);
    OA_CHECK(byte_context.shadow_index_counts.admitted_patterns == 1);
    OA_CHECK(byte_context.shadow_index_counts.cached_runs == 1);
    OA_CHECK(byte_context.shadow_index_counts.fallback_runs == 1);
    OA_CHECK(byte_context.shadow_index_counts.template_uploaded_indices * sizeof(uint32_t) <=
             shadow_template_admission_bytes);
    OA_CHECK(shadow_template_frame(byte_context, byte_budget, true, pixels, error) == Result::success);
    OA_CHECK(pixels == expected);
    OA_CHECK(byte_context.shadow_index_counts.admitted_patterns == 1);
    OA_CHECK(byte_context.shadow_index_counts.cached_runs == 2);
    OA_CHECK(byte_context.shadow_index_counts.fallback_runs == 0);

    // Complete repetitions are independently checked when an immutable pattern
    // first enters. The unused last relative cast cannot hide malformed words.
    auto broken = fixture;
    ++broken.revision;
    broken.pattern_indices.back() ^= 1;
    Context rejected_context;
    pixels.assign(channels, full_channel);
    OA_CHECK(shadow_template_frame(
        rejected_context, broken, true, pixels, error
    ) == Result::failed);
    OA_CHECK(pixels.empty());
    OA_CHECK(rejected_context.shadow_index_counts.logical_indices == 0);

    // Pattern policy is rechecked against current instance/type bindings even
    // after a successful resident frame; a previous GPU admission is no bypass.
    broken = fixture;
    broken.ordinary.instances[1].corners = 0;
    pixels.assign(channels, full_channel);
    OA_CHECK(shadow_template_frame(context, broken, true, pixels, error) == Result::failed);
    OA_CHECK(pixels.empty());
    OA_CHECK(context.shadow_index_counts.logical_indices == 0);
}
