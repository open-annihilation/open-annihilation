// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Timing ranks, map-contained armies and benchmark report facts.
#include "oa/app/battle_benchmark.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <locale>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

namespace benchmark = oa::app::battle_benchmark;

/// Checks exact nearest-rank percentiles and leaves input ordering unchanged.
void timings() {
    CHECK(benchmark::summarize({}).count == 0);
    const std::array<uint64_t, 4> samples{40, 10, 30, 20};
    const auto summary = benchmark::summarize(samples);
    CHECK(summary.count == 4);
    CHECK(summary.min_ns == 10 && summary.max_ns == 40);
    CHECK(summary.p50_ns == 20 && summary.p95_ns == 40 && summary.p99_ns == 40);
    CHECK(summary.mean_ns == 25);
    CHECK(samples.front() == 40 && samples.back() == 20);
    std::vector<uint64_t> hundred;
    for (uint64_t value = 100; value != 0; --value)
        hundred.push_back(value);
    const auto ranked = benchmark::summarize(hundred);
    CHECK(ranked.p50_ns == 50 && ranked.p95_ns == 95 && ranked.p99_ns == 99);
    const std::array<uint64_t, 1> one{std::numeric_limits<uint64_t>::max()};
    const auto single = benchmark::summarize(one);
    CHECK(single.min_ns == one[0] && single.p99_ns == one[0]);
    const std::array<uint64_t, 2> huge{one[0], one[0]};
    CHECK(benchmark::summarize(huge).mean_ns > 1.0e19);
}

/// Checks that two full armies fit inside margins with no duplicate points.
void layouts() {
    constexpr uint32_t width = 8192;
    constexpr uint32_t height = 8192;
    CHECK(benchmark::maximum_layout_units(width, height) > benchmark::maximum_units_per_side);
    CHECK(benchmark::maximum_layout_units(32767, 32767, 1) == benchmark::maximum_layout_candidates);
    CHECK(benchmark::maximum_layout_units(128, 128) == 0);
    CHECK(benchmark::maximum_layout_units(width, height, 0) == 0);
    CHECK(benchmark::maximum_layout_units(256, 256) == 5);
    CHECK(benchmark::plan_layout(256, 256, 5).layout.has_value());
    CHECK(!benchmark::plan_layout(256, 256, 6).layout);
    const auto result = benchmark::plan_layout(width, height, benchmark::maximum_units_per_side);
    CHECK(result.layout.has_value() && result.error.empty());
    if (!result.layout)
        return;
    std::set<std::pair<int32_t, int32_t>> occupied;
    for (std::size_t side = 0; side < result.layout->armies.size(); ++side) {
        const auto& army = result.layout->armies[side];
        CHECK(army.size() == benchmark::maximum_units_per_side);
        for (const auto point : army) {
            CHECK(point.x >= static_cast<int32_t>(benchmark::default_margin));
            CHECK(point.z >= static_cast<int32_t>(benchmark::default_margin));
            CHECK(point.x <= static_cast<int32_t>(width - benchmark::default_margin));
            CHECK(point.z <= static_cast<int32_t>(height - benchmark::default_margin));
            CHECK(occupied.emplace(point.x, point.z).second);
        }
    }
    for (std::size_t index = 0; index < result.layout->armies[0].size(); ++index) {
        const auto left = result.layout->armies[0][index];
        const auto right = result.layout->armies[1][index];
        CHECK(left.x + right.x == static_cast<int32_t>(width));
        CHECK(left.z == right.z);
        CHECK(right.x - left.x >= static_cast<int32_t>(benchmark::default_gap));
    }
    CHECK(benchmark::plan_layout(width, height, 0).layout.has_value());
    CHECK(benchmark::plan_layout(width, height, benchmark::maximum_units_per_side + 1).layout);
    CHECK(
        !benchmark::plan_layout(32767, 32767, benchmark::maximum_layout_candidates + 1, 1).layout
    );
    const auto all_candidates =
        benchmark::plan_layout(width, height, benchmark::maximum_layout_units(width, height));
    CHECK(all_candidates.layout.has_value());
    if (all_candidates.layout) {
        // Even discarding every second terrain candidate leaves room for a full army.
        CHECK(all_candidates.layout->armies[0].size() / 2 >= benchmark::maximum_units_per_side);
    }
    CHECK(!benchmark::plan_layout(128, 128, 1).layout);
    CHECK(!benchmark::plan_layout(256, 256, 100).layout);
    CHECK(!benchmark::plan_layout(width, height, 1, 0).layout);
    CHECK(!benchmark::plan_layout(width, height, 1, 32, 0).layout);
    CHECK(!benchmark::plan_layout(width, height, 1, 32, 64, 0).layout);
    CHECK(!benchmark::plan_layout(std::numeric_limits<uint32_t>::max(), height, 1).layout);
    CHECK(!benchmark::plan_layout(width, height, 1, 32, UINT32_MAX, UINT32_MAX).layout);
    const auto odd = benchmark::plan_layout(511, 513, 20, 32, 16, 33);
    CHECK(odd.layout.has_value());
    if (odd.layout)
        CHECK(
            odd.layout->armies[0] ==
            benchmark::plan_layout(511, 513, 20, 32, 16, 33).layout->armies[0]
        );
}

/// Checks shared destination patches, static rejection and real footprint reservations.
void group_arrivals() {
    constexpr uint32_t width = 8192, height = 8192;
    const auto small = benchmark::plan_group_arrivals(width, height, 1000);
    CHECK(small.layout.has_value());
    if (!small.layout)
        return;
    for (uint8_t side = 0; side < 2; ++side) {
        const auto& points = small.layout->armies[side];
        CHECK(points.size() == 1000);
        const auto region_x = points.front().x / benchmark::shared_goal_region_pixels;
        const auto region_z = points.front().z / benchmark::shared_goal_region_pixels;
        for (const auto point : points) {
            CHECK(point.x / benchmark::shared_goal_region_pixels == region_x);
            CHECK(point.z / benchmark::shared_goal_region_pixels == region_z);
            CHECK(
                side == 0 ? point.x > static_cast<int32_t>(width / 2)
                          : point.x < static_cast<int32_t>(width / 2)
            );
        }
    }
    CHECK(
        small.layout->armies == benchmark::plan_group_arrivals(width, height, 1000).layout->armies
    );
    CHECK(!benchmark::plan_group_arrivals(256, 256, 1000).layout);
    CHECK(!benchmark::plan_group_arrivals(UINT32_MAX, height, 1).layout);
    CHECK(!benchmark::plan_group_arrivals(width, height, benchmark::maximum_layout_candidates + 1)
               .layout);
    CHECK(benchmark::plan_group_arrivals(width, height, 0).layout.has_value());

    const auto available =
        benchmark::maximum_layout_units(width, height, benchmark::arrival_spacing);
    const auto candidates = benchmark::plan_group_arrivals(width, height, available);
    CHECK(candidates.layout.has_value());
    if (!candidates.layout)
        return;
    std::array<std::vector<benchmark::Footprint>, 2> footprints;
    for (auto& army : footprints) {
        army.resize(benchmark::maximum_units_per_side);
        army.front() = {2, 2}; // the commander occupies four cells rather than one
    }

    struct Terrain {
        std::size_t rejections{};

        static bool can_stand(void* context, uint8_t, std::size_t unit, benchmark::Point point) {
            auto& terrain = *static_cast<Terrain*>(context);
            const bool clear =
                (point.x / 16 + point.z / 16) % 7 != 0 && (unit != 0 || point.z % 32 == 0);
            terrain.rejections += !clear;
            return clear;
        }
    } terrain;

    const std::array<std::span<const benchmark::Footprint>, 2> spans{footprints[0], footprints[1]};
    const auto result = benchmark::select_arrivals(
        width, height, *candidates.layout, spans, {&terrain, Terrain::can_stand}
    );
    CHECK(result.layout.has_value());
    CHECK(terrain.rejections > 0);
    if (!result.layout)
        return;
    std::set<std::pair<int32_t, int32_t>> occupied;
    for (uint8_t side = 0; side < 2; ++side) {
        CHECK(result.layout->armies[side].size() == benchmark::maximum_units_per_side);
        for (std::size_t unit = 0; unit < footprints[side].size(); ++unit) {
            const auto point = result.layout->armies[side][unit];
            const auto footprint = footprints[side][unit];
            CHECK(Terrain::can_stand(&terrain, side, unit, point));
            const auto left = (point.x + 8 - footprint.x * 8) / 16;
            const auto top = (point.z + 8 - footprint.z * 8) / 16;
            for (int32_t z = top; z < top + footprint.z; ++z)
                for (int32_t x = left; x < left + footprint.x; ++x) {
                    CHECK(
                        x >= 0 && z >= 0 && x * 16 < static_cast<int32_t>(width) &&
                        z * 16 < static_cast<int32_t>(height)
                    );
                    CHECK(occupied.emplace(x, z).second);
                }
        }
    }
    CHECK(occupied.size() == benchmark::maximum_units_per_side * 2 + 6);

    benchmark::Layout duplicate;
    duplicate.armies[0] = {{64, 64}};
    duplicate.armies[1] = {{64, 64}};
    const std::array<benchmark::Footprint, 1> one{{{1, 1}}};
    const std::array<std::span<const benchmark::Footprint>, 2> ones{one, one};
    CHECK(!benchmark::select_arrivals(256, 256, duplicate, ones).layout);
    duplicate.armies[1] = {{-1, 64}, {256, 256}, {192, 192}};
    CHECK(benchmark::select_arrivals(256, 256, duplicate, ones).layout.has_value());
    const std::array<benchmark::Footprint, 1> zero{{{0, 1}}};
    CHECK(!benchmark::select_arrivals(256, 256, duplicate, {zero, one}).layout);
    CHECK(!benchmark::select_arrivals(UINT32_MAX, 256, duplicate, ones).layout);
    CHECK(!benchmark::select_arrivals(
               256,
               256,
               duplicate,
               ones,
               {nullptr, [](void*, uint8_t, std::size_t, benchmark::Point) { return false; }}
    ).layout);
    CHECK(benchmark::select_arrivals(256, 256, duplicate, {}).layout.has_value());
}

/// Checks that one jittering unit cannot stand in for an army making progress.
void progress() {
    auto still = benchmark::begin_progress({0, 0}, {100, 0});
    for (int tick = 0; tick < 4; ++tick)
        benchmark::sample_progress(still, {0, 0}, true);
    CHECK(still.stationary_ticks == 4 && still.longest_stall == 4);
    benchmark::sample_progress(still, {1, 0}, true);
    benchmark::sample_progress(still, {0, 0}, true);
    CHECK(
        !still.moved && benchmark::summarize_progress(std::span{&still, 1}).progressed_units == 0
    );
    benchmark::sample_progress(still, {-32, 0}, true); // a necessary detour can initially move away
    CHECK(still.moved && still.best_distance == 99);
    benchmark::sample_progress(still, {32, 0}, true);
    benchmark::sample_progress(still, {0, 0}, true);
    const auto detour = benchmark::summarize_progress(std::span{&still, 1});
    CHECK(detour.progressed_units == 1 && detour.final_goal_distance_pixels == 100);

    std::array<benchmark::ProgressState, 6> army;
    for (std::size_t index = 0; index < army.size(); ++index) {
        army[index] =
            benchmark::begin_progress({0, 0}, {static_cast<int32_t>((index + 1) * 30), 0});
        benchmark::sample_progress(army[index], {index % 2 == 0 ? 32 : 0, 0}, true);
    }
    const auto summary = benchmark::summarize_progress(army);
    CHECK(summary.goal_units == 6 && summary.live_units == 6);
    CHECK(summary.moved_units == 3 && summary.progressed_units == 3 && summary.arrived_units == 1);
    CHECK(summary.stationary_unit_ticks == 3 && summary.longest_stall_ticks == 1);
    CHECK(summary.initial_goal_distance_pixels == 630 && summary.final_goal_distance_pixels == 538);
    for (const auto& third : summary.distance_thirds)
        CHECK(third.goal_units == 2 && third.progressed_units == 1);
    CHECK(summary.distance_thirds[0].arrived_units == 1);
    benchmark::sample_progress(army[0], {0, 0}, false);
    CHECK(benchmark::summarize_progress(army).arrived_units == 0);
    CHECK(benchmark::summarize_progress({}).goal_units == 0);

    auto extreme = benchmark::begin_progress({INT32_MIN, INT32_MIN}, {INT32_MAX, INT32_MAX});
    CHECK(extreme.initial_distance == uint64_t{UINT32_MAX} * 2);
    benchmark::sample_progress(extreme, {INT32_MAX, INT32_MAX}, true);
    CHECK(extreme.arrived && extreme.moved);
}

/// Gives the global locale a decimal comma to check JSON number formatting.
class DecimalComma : public std::numpunct<char> {
  protected:

    /// Returns the locale's decimal separator.
    char do_decimal_point() const override { return ','; }
};

/// Checks escaped text, independent renderer facts and explicit failed-run fields.
void reports() {
    benchmark::Report report;
    report.completed = true;
    report.engine_version = "0.9.3";
    report.map = "Map \"name\"\\path\n\t";
    report.map.push_back('\0');
    report.map += "é";
    report.scenario = "march";
    report.seed = 123;
    report.unit_limit = 1500;
    report.requested_units_per_side = 1000;
    report.initial_units = 2000;
    report.final_units = 1999;
    report.warmup_ticks = 30;
    report.measured_ticks = 100;
    report.successful_warmup_ticks = 30;
    report.successful_ticks = 100;
    report.moved_unit_ticks = 40;
    report.damaged_unit_ticks = 3;
    report.projectile_high_water = 12;
    report.projectile_capacity = 32767;
    report.projectile_allocation_failures = 2;
    report.projection_preparations = 300;
    report.projected_slots = 6000000;
    report.bucket_queries = 2000;
    report.bucket_candidates = 4000;
    report.shared_route_requests = 48;
    report.shared_route_hits = 40;
    report.shared_route_fields = 4;
    report.full_route_searches = 3;
    report.target_queries = 100;
    report.target_candidates = 120;
    report.target_full_scans = 0;
    report.local_flow_blocked_ticks = 6;
    report.goal_metrics_available = true;
    report.goal_progress[0].goal_units = 24;
    report.goal_progress[0].progressed_units = 23;
    report.activity_observed = true;
    report.enhanced_mode = true;
    report.enhanced_version = 1;
    report.simulation_hash = "hash";
    report.world_digest = "digest";
    report.ticks.mean_ns = 1.5;
    report.simulation_phases.push_back(
        {"movement", benchmark::summarize(std::array<uint64_t, 2>{2, 4})}
    );
    report.simulation_phases.push_back(
        {"phase\"name", {.mean_ns = std::numeric_limits<double>::infinity()}}
    );
    const auto old_locale = std::locale();
    std::locale::global(std::locale(old_locale, new DecimalComma));
    const auto json = benchmark::report_json(report);
    std::locale::global(old_locale);
    CHECK(json.find("\"schema_version\": 1") != std::string::npos);
    CHECK(json.find("\"completed\": true") != std::string::npos);
    CHECK(json.find("Map \\\"name\\\"\\\\path\\u000a\\u0009\\u0000é") != std::string::npos);
    CHECK(json.find("\"renderer\": \"none\"") != std::string::npos);
    CHECK(json.find("\"mean\": 1.5") != std::string::npos);
    CHECK(json.find("\"simulation_phase_ns\": {") != std::string::npos);
    CHECK(json.find("\"movement\": {\"count\": 2, \"min\": 2") != std::string::npos);
    CHECK(json.find("\"phase\\\"name\":") != std::string::npos);
    CHECK(json.find("\"successful_ticks\": 100") != std::string::npos);
    CHECK(json.find("\"enhanced_version\": 1") != std::string::npos);
    CHECK(json.find("\"moved_unit_ticks\": 40") != std::string::npos);
    CHECK(json.find("\"damaged_unit_ticks\": 3") != std::string::npos);
    CHECK(json.find("\"projectile_high_water\": 12") != std::string::npos);
    CHECK(json.find("\"projectile_capacity\": 32767") != std::string::npos);
    CHECK(json.find("\"projectile_allocation_failures\": 2") != std::string::npos);
    CHECK(json.find("\"projection_preparations\": 300") != std::string::npos);
    CHECK(json.find("\"projected_slots\": 6000000") != std::string::npos);
    CHECK(json.find("\"bucket_queries\": 2000") != std::string::npos);
    CHECK(json.find("\"bucket_candidates\": 4000") != std::string::npos);
    CHECK(json.find("\"shared_route_requests\": 48") != std::string::npos);
    CHECK(json.find("\"shared_route_hits\": 40") != std::string::npos);
    CHECK(json.find("\"full_route_searches\": 3") != std::string::npos);
    CHECK(json.find("\"target_candidates\": 120") != std::string::npos);
    CHECK(json.find("\"local_flow_blocked_ticks\": 6") != std::string::npos);
    CHECK(json.find("\"goal_metrics_available\": true") != std::string::npos);
    CHECK(json.find("\"goal_progress\": [{\"goal_units\": 24") != std::string::npos);
    CHECK(json.find("cumulative_setup_warmup_and_measured") != std::string::npos);
    CHECK(json.find("\"activity_observed\": true") != std::string::npos);
    CHECK(json.find("after_each_successful_warmup_and_measured_tick") != std::string::npos);
    report.completed = false;
    report.error = "tick failed";
    report.successful_ticks = 5;
    report.ticks.mean_ns = std::numeric_limits<double>::infinity();
    const auto failed = benchmark::report_json(report);
    CHECK(failed.find("\"completed\": false") != std::string::npos);
    CHECK(failed.find("\"error\": \"tick failed\"") != std::string::npos);
    CHECK(failed.find("\"successful_ticks\": 5") != std::string::npos);
    CHECK(failed.find("\"mean\": null") != std::string::npos);
}

} // namespace

int main() {
    timings();
    layouts();
    group_arrivals();
    progress();
    reports();
    return failures == 0 ? 0 : 1;
}
