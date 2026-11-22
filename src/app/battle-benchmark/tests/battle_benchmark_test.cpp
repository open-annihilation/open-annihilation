// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Timing ranks, map-contained armies and benchmark report facts.
#include "oa/app/battle_benchmark.hpp"

#include <array>
#include <cstdio>
#include <limits>
#include <locale>
#include <optional>
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
    const auto empty = benchmark::summarize({});
    CHECK(empty.count == 0 && empty.total_ns == 0 && empty.over_budget_count == 0);
    CHECK(!empty.total_ns_saturated);
    const std::array<uint64_t, 4> samples{40, 10, 30, 20};
    const auto summary = benchmark::summarize(samples);
    CHECK(summary.count == 4);
    CHECK(summary.min_ns == 10 && summary.max_ns == 40);
    CHECK(summary.p50_ns == 20 && summary.p95_ns == 40 && summary.p99_ns == 40);
    CHECK(summary.mean_ns == 25);
    CHECK(summary.total_ns == 100 && summary.over_budget_count == 0);
    CHECK(!summary.total_ns_saturated);
    CHECK(samples.front() == 40 && samples.back() == 20);
    std::vector<uint64_t> hundred;
    for (uint64_t value = 100; value != 0; --value)
        hundred.push_back(value);
    const auto ranked = benchmark::summarize(hundred);
    CHECK(ranked.p50_ns == 50 && ranked.p95_ns == 95 && ranked.p99_ns == 99);
    const std::array<uint64_t, 1> one{std::numeric_limits<uint64_t>::max()};
    const auto single = benchmark::summarize(one);
    CHECK(single.min_ns == one[0] && single.p99_ns == one[0]);
    CHECK(single.total_ns == one[0] && !single.total_ns_saturated);
    CHECK(single.over_budget_count == 1);
    const std::array<uint64_t, 2> huge{one[0], one[0]};
    const auto saturated = benchmark::summarize(huge);
    CHECK(saturated.mean_ns > 1.0e19);
    CHECK(saturated.total_ns == one[0] && saturated.total_ns_saturated);
    CHECK(saturated.over_budget_count == 2);
    const auto with_zero = benchmark::summarize(std::array<uint64_t, 2>{one[0], 0});
    CHECK(with_zero.total_ns == one[0] && !with_zero.total_ns_saturated);
    const auto one_past = benchmark::summarize(std::array<uint64_t, 2>{one[0], 1});
    CHECK(one_past.total_ns == one[0] && one_past.total_ns_saturated);

    // A tick exactly at the budget is not long; zero is a valid CPU sample.
    constexpr auto budget = benchmark::tick_budget_ns;
    CHECK(budget == 33'333'333);
    const std::array<uint64_t, 5> boundary{budget + 1, 0, budget * 2, budget - 1, budget};
    const auto long_ticks = benchmark::summarize(boundary);
    CHECK(long_ticks.count == 5 && long_ticks.min_ns == 0 && long_ticks.max_ns == budget * 2);
    CHECK(long_ticks.p50_ns == budget && long_ticks.p95_ns == budget * 2);
    CHECK(long_ticks.total_ns == budget * 5 && !long_ticks.total_ns_saturated);
    CHECK(long_ticks.over_budget_count == 2);
    CHECK(boundary.front() == budget + 1 && boundary.back() == budget);
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
    report.enhanced_revision = 1;
    report.enhanced_features = {"capacity", "local_flow"};
    report.simulation_hash = "hash";
    report.world_digest = "digest";
    report.ticks.mean_ns = 1.5;
    report.thread_cpu_clock = "CLOCK_THREAD_CPUTIME_ID";
    report.thread_cpu_status = "available";
    report.thread_cpu_ticks = benchmark::summarize(
        std::array<uint64_t, 3>{0, benchmark::tick_budget_ns, benchmark::tick_budget_ns + 1}
    );
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
    CHECK(json.find("\"tick_budget_ns\": 33333333") != std::string::npos);
    CHECK(json.find("\"thread_cpu_clock\": \"CLOCK_THREAD_CPUTIME_ID\"") != std::string::npos);
    CHECK(json.find("\"thread_cpu_status\": \"available\"") != std::string::npos);
    CHECK(json.find("\"thread_cpu_available\": true") != std::string::npos);
    CHECK(json.find("\"tick_thread_cpu_ns\": {\"count\": 3, \"min\": 0") != std::string::npos);
    CHECK(json.find("\"total\": 66666667") != std::string::npos);
    CHECK(json.find("\"over_budget\": 1") != std::string::npos);
    CHECK(json.find("\"total_saturated\": false") != std::string::npos);
    CHECK(json.find("\"simulation_phase_ns\": {") != std::string::npos);
    CHECK(json.find("\"movement\": {\"count\": 2, \"min\": 2") != std::string::npos);
    CHECK(json.find("\"phase\\\"name\":") != std::string::npos);
    CHECK(json.find("\"successful_ticks\": 100") != std::string::npos);
    CHECK(json.find("\"enhanced_version\": 1") != std::string::npos);
    CHECK(json.find("\"enhanced_revision\": 1") != std::string::npos);
    CHECK(json.find("\"enhanced_features\": [\"capacity\", \"local_flow\"]") != std::string::npos);
    CHECK(json.find("\"enhanced_features_spec\": \"\"") != std::string::npos);
    const std::string chose_features = std::string("\"enhanced_features_") + "override\": false";
    CHECK(json.find(chose_features) != std::string::npos);
    CHECK(json.find("\"data_faults\": 0") != std::string::npos);
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
    report.thread_cpu_clock = "unsupported";
    report.thread_cpu_status = "unsupported";
    report.thread_cpu_ticks.reset();
    const auto unsupported = benchmark::report_json(report);
    CHECK(unsupported.find("\"thread_cpu_available\": false") != std::string::npos);
    CHECK(unsupported.find("\"thread_cpu_status\": \"unsupported\"") != std::string::npos);
    CHECK(unsupported.find("\"tick_thread_cpu_ns\": null") != std::string::npos);
    report.completed = false;
    report.error = "tick failed";
    report.successful_ticks = 5;
    report.ticks.mean_ns = std::numeric_limits<double>::infinity();
    report.thread_cpu_clock = "GetThreadTimes";
    report.thread_cpu_status = "read_failed";
    report.ticks.total_ns = std::numeric_limits<uint64_t>::max();
    report.ticks.total_ns_saturated = true;
    const auto failed = benchmark::report_json(report);
    CHECK(failed.find("\"completed\": false") != std::string::npos);
    CHECK(failed.find("\"error\": \"tick failed\"") != std::string::npos);
    CHECK(failed.find("\"successful_ticks\": 5") != std::string::npos);
    CHECK(failed.find("\"mean\": null") != std::string::npos);
    CHECK(failed.find("\"thread_cpu_clock\": \"GetThreadTimes\"") != std::string::npos);
    CHECK(failed.find("\"thread_cpu_status\": \"read_failed\"") != std::string::npos);
    CHECK(failed.find("\"tick_thread_cpu_ns\": null") != std::string::npos);
    CHECK(failed.find("\"total\": 18446744073709551615") != std::string::npos);
    CHECK(failed.find("\"total_saturated\": true") != std::string::npos);
}

/// Checks crossing lines: reading, sides, strict crossings and segment ends.
void crossing_lines() {
    const auto line = benchmark::parse_crossing_line("bridge-north=100,0,100,200");
    CHECK(line && line->name == "bridge-north");
    CHECK(
        line && line->from == (benchmark::Point{100, 0}) && line->to == (benchmark::Point{100, 200})
    );
    CHECK(!benchmark::parse_crossing_line("=1,2,3,4"));
    CHECK(!benchmark::parse_crossing_line("a b=1,2,3,4"));
    CHECK(!benchmark::parse_crossing_line("a=1,2,3"));
    CHECK(!benchmark::parse_crossing_line("a=1,2,3,4,5"));
    CHECK(!benchmark::parse_crossing_line("a=1,2,-3,4"));
    CHECK(!benchmark::parse_crossing_line("a=1,2,32768,4"));
    CHECK(!benchmark::parse_crossing_line("a=5,5,5,5"));
    CHECK(!benchmark::parse_crossing_line("a=1,,3,4"));
    CHECK(benchmark::parse_crossing_line("a=0,0,32767,32767").has_value());
    const auto& bridge = *line;
    CHECK(benchmark::line_side(bridge, {99, 50}) == -benchmark::line_side(bridge, {101, 50}));
    CHECK(benchmark::line_side(bridge, {99, 50}) != 0);
    CHECK(benchmark::line_side(bridge, {100, 500}) == 0);
    // Across the segment, past its end, onto and off the line.
    CHECK(benchmark::crosses_line(bridge, {98, 50}, {102, 50}));
    CHECK(benchmark::crosses_line(bridge, {102, 200}, {98, 200}));
    CHECK(!benchmark::crosses_line(bridge, {98, 250}, {102, 250}));
    CHECK(!benchmark::crosses_line(bridge, {98, 50}, {100, 50}));
    CHECK(!benchmark::crosses_line(bridge, {100, 50}, {102, 50}));
    CHECK(!benchmark::crosses_line(bridge, {98, 50}, {99, 60}));
    // A diagonal step whose path passes the segment's end exactly crosses.
    CHECK(benchmark::crosses_line(bridge, {99, 199}, {101, 201}));
}

/// Checks staged-unit tracking: crossings away from the start side, the
/// stationary window, retirement and per-player summaries.
void staged_units() {
    const std::array<benchmark::CrossingLine, 2> lines{
        benchmark::CrossingLine{"centre", {100, 0}, {100, 1000}},
        benchmark::CrossingLine{"far", {500, 0}, {500, 1000}}
    };
    auto runner = benchmark::begin_staged_unit(0, {90, 10}, benchmark::Point{190, 10}, lines);
    CHECK(runner.start_side[0] != 0 && runner.start_side[1] == runner.start_side[0]);
    benchmark::sample_staged_unit(runner, lines, {95, 10}, true, true, false);
    benchmark::sample_staged_unit(runner, lines, {105, 10}, true, true, false);
    CHECK(runner.crossed == 1);
    // Back and over again counts once.
    benchmark::sample_staged_unit(runner, lines, {95, 10}, true, true, false);
    benchmark::sample_staged_unit(runner, lines, {105, 10}, true, true, false);
    CHECK(runner.crossed == 1);
    benchmark::sample_staged_unit(runner, lines, {105, 10}, false, true, true);
    benchmark::sample_staged_unit(runner, lines, {110, 10}, true, true, true);
    CHECK(runner.window_live_ticks == 2 && runner.window_stationary_ticks == 1);
    // A unit starting on the far side of a line crosses it only towards the near side.
    auto returner = benchmark::begin_staged_unit(1, {120, 10}, std::nullopt, lines);
    benchmark::sample_staged_unit(returner, lines, {130, 10}, true, true, false);
    CHECK(returner.crossed == 0);
    benchmark::sample_staged_unit(returner, lines, {90, 10}, true, true, false);
    CHECK(returner.crossed == 1);
    // A still unit through the whole window, and one that dies in it.
    auto still = benchmark::begin_staged_unit(0, {10, 10}, benchmark::Point{10, 110}, lines);
    benchmark::sample_staged_unit(still, lines, {10, 10}, false, true, true);
    benchmark::sample_staged_unit(still, lines, {10, 10}, false, true, true);
    auto lost = benchmark::begin_staged_unit(0, {10, 500}, benchmark::Point{10, 400}, lines);
    benchmark::sample_staged_unit(lost, lines, {10, 480}, true, true, true);
    benchmark::sample_staged_unit(lost, lines, {10, 470}, true, false, true);
    benchmark::sample_staged_unit(lost, lines, {10, 300}, true, true, true);
    CHECK(!lost.live && lost.position == (benchmark::Point{10, 480}));
    CHECK(lost.window_live_ticks == 1);
    const std::array<benchmark::StagedUnit, 4> units{returner, runner, still, lost};
    const auto players = benchmark::summarize_staged(units, lines.size(), 2);
    CHECK(players.size() == 2 && players[0].player == 0 && players[1].player == 1);
    const auto& first = players[0];
    CHECK(first.tracked_units == 3 && first.live_tracked_units == 2 && first.goal_units == 3);
    // Progress: runner 100 -> 80, still 100 -> 100, lost 100 -> 80.
    CHECK(first.mean_progress_pixels > 13.33 && first.mean_progress_pixels < 13.34);
    CHECK(first.advanced_units == 2);
    CHECK(first.window_live_unit_ticks == 5 && first.window_stationary_unit_ticks == 3);
    CHECK(first.stationary_throughout == 1);
    CHECK(first.crossers.size() == 2 && first.crossers[0] == 1 && first.crossers[1] == 0);
    CHECK(players[1].goal_units == 0 && players[1].mean_progress_pixels == 0);
    CHECK(players[1].crossers[0] == 1);
    CHECK(first.window_engaged_stationary_unit_ticks == 0);
    CHECK(first.stationary_throughout_unengaged == 1);
}

/// Checks the engaged split of the stationary window: a unit standing while
/// engaged, one standing while engaged only once, an engaged moving unit, and
/// engagement outside the window or after death, which do not count.
void staged_engagement() {
    const std::array<benchmark::CrossingLine, 0> lines{};
    auto firing = benchmark::begin_staged_unit(1, {10, 10}, std::nullopt, lines);
    benchmark::sample_staged_unit(firing, lines, {10, 10}, false, true, false, true);
    benchmark::sample_staged_unit(firing, lines, {10, 10}, false, true, true, true);
    benchmark::sample_staged_unit(firing, lines, {10, 10}, false, true, true, true);
    CHECK(firing.window_engaged_ticks == 2 && firing.window_engaged_stationary_ticks == 2);
    auto waiting = benchmark::begin_staged_unit(1, {20, 10}, std::nullopt, lines);
    benchmark::sample_staged_unit(waiting, lines, {20, 10}, false, true, true, false);
    benchmark::sample_staged_unit(waiting, lines, {20, 10}, false, true, true, true);
    auto charging = benchmark::begin_staged_unit(1, {30, 10}, std::nullopt, lines);
    benchmark::sample_staged_unit(charging, lines, {31, 10}, true, true, true, true);
    benchmark::sample_staged_unit(charging, lines, {31, 10}, false, true, true, false);
    CHECK(charging.window_engaged_ticks == 1 && charging.window_engaged_stationary_ticks == 0);
    auto stuck = benchmark::begin_staged_unit(0, {40, 10}, std::nullopt, lines);
    benchmark::sample_staged_unit(stuck, lines, {40, 10}, false, true, true, false);
    benchmark::sample_staged_unit(stuck, lines, {40, 10}, false, true, true, false);
    benchmark::sample_staged_unit(stuck, lines, {40, 10}, false, false, true, true);
    CHECK(stuck.window_engaged_ticks == 0 && stuck.window_live_ticks == 2);
    const std::array<benchmark::StagedUnit, 4> units{firing, waiting, charging, stuck};
    const auto players = benchmark::summarize_staged(units, 0, 2);
    CHECK(players.size() == 2);
    const auto& human = players[0];
    const auto& computer = players[1];
    CHECK(human.window_live_unit_ticks == 2 && human.window_stationary_unit_ticks == 2);
    CHECK(human.window_engaged_stationary_unit_ticks == 0);
    CHECK(human.stationary_throughout == 1 && human.stationary_throughout_unengaged == 1);
    CHECK(computer.window_live_unit_ticks == 6 && computer.window_stationary_unit_ticks == 5);
    CHECK(computer.window_engaged_stationary_unit_ticks == 3);
    // Firing and waiting stood through the window; only waiting was ever
    // unengaged, and it was engaged once, so neither counts as unengaged.
    CHECK(computer.stationary_throughout == 2 && computer.stationary_throughout_unengaged == 0);
}

/// Checks strike-target spread samples.
void target_spread() {
    benchmark::TargetSpread spread;
    std::vector<uint16_t> none;
    benchmark::add_target_sample(spread, none);
    CHECK(spread.samples == 0);
    std::vector<uint16_t> targets{7, 3, 7, 7, 9};
    benchmark::add_target_sample(spread, targets);
    CHECK(spread.samples == 1 && spread.attackers == 5 && spread.distinct_targets == 3);
    CHECK(spread.largest_share > 0.599 && spread.largest_share < 0.601);
    std::vector<uint16_t> one{4};
    benchmark::add_target_sample(spread, one);
    CHECK(spread.samples == 2 && spread.attackers == 6 && spread.distinct_targets == 4);
    CHECK(spread.largest_share > 1.599 && spread.largest_share < 1.601);
}

/// Checks strike target churn between two samples: kept and switched
/// targets, a target that died or whose slot holds another unit, a replaced
/// attacker, attackers without a target in either sample and owners past the
/// counted players.
void target_churn() {
    constexpr uint8_t human = 0;
    constexpr uint8_t computer = 1;
    constexpr uint8_t uncounted = 2;
    std::vector<benchmark::StrikeSlot> earlier(16);
    std::vector<benchmark::StrikeSlot> later(16);
    const auto unit =
        [](
            uint8_t player, uint32_t generation, uint16_t target = 0, uint32_t target_generation = 0
        ) { return benchmark::StrikeSlot{true, player, generation, target, target_generation}; };
    // Targets: 5 and 6 live through both samples, 7's unit is replaced, 8 dies.
    earlier[5] = later[5] = unit(human, 3);
    earlier[6] = later[6] = unit(human, 4);
    earlier[7] = unit(human, 2);
    later[7] = unit(human, 9);
    earlier[8] = unit(human, 5);
    // Keeps its target, and switches it.
    earlier[1] = later[1] = unit(computer, 10, 5, 3);
    earlier[2] = unit(computer, 11, 5, 3);
    later[2] = unit(computer, 11, 6, 4);
    // Its target's slot holds another unit now, which it attacks.
    earlier[3] = unit(computer, 12, 7, 2);
    later[3] = unit(computer, 12, 7, 9);
    // Its target died.
    earlier[4] = unit(computer, 13, 8, 5);
    later[4] = unit(computer, 13, 5, 3);
    // A later unit in the attacker's slot is not it.
    earlier[9] = unit(computer, 14, 5, 3);
    later[9] = unit(computer, 15, 6, 4);
    // No attack in one of the samples.
    earlier[10] = unit(computer, 16);
    later[10] = unit(computer, 16, 5, 3);
    earlier[11] = unit(computer, 17, 5, 3);
    later[11] = unit(computer, 17);
    // Another player's attacker that switched, and an owner past the counted players.
    earlier[12] = unit(human, 18, 5, 3);
    later[12] = unit(human, 18, 6, 4);
    earlier[13] = later[13] = unit(uncounted, 19, 5, 3);
    later[13].target = 6;
    later[13].target_generation = 4;
    // The earlier target lies past the later sample's slots.
    earlier[14] = unit(computer, 20, 15, 1);
    later[14] = unit(computer, 20, 5, 3);
    later.resize(15);
    std::array<benchmark::TargetChurn, 2> churn{};
    benchmark::add_target_churn(churn, earlier, later);
    CHECK(churn[computer].compared_attackers == 2 && churn[computer].switched_attackers == 1);
    CHECK(churn[human].compared_attackers == 1 && churn[human].switched_attackers == 1);
    // Pairs add up: a sample against itself compares every attacker of a live
    // unit and switches none. An empty sample compares nothing.
    benchmark::add_target_churn(churn, later, later);
    CHECK(churn[computer].compared_attackers == 9 && churn[computer].switched_attackers == 1);
    CHECK(churn[human].compared_attackers == 2 && churn[human].switched_attackers == 1);
    benchmark::add_target_churn(churn, {}, later);
    benchmark::add_target_churn({}, earlier, later);
    CHECK(churn[computer].compared_attackers == 9 && churn[human].compared_attackers == 2);
}

/// Checks ownership audits: owned cells, a stolen cell, an overlap, an air
/// unit and a footprint past the map's edge.
void ownership_audits() {
    constexpr uint32_t width = 8;
    constexpr uint32_t height = 6;

    struct Plots {
        std::array<uint16_t, width * height> ground{};
        std::array<uint16_t, width * height> air{};
    } plots;

    const auto read = [](const void* context, std::size_t cell, bool air) -> uint16_t {
        const auto& words = *static_cast<const Plots*>(context);
        return air ? words.air[cell] : words.ground[cell];
    };
    const auto claim = [&](uint16_t id, int32_t x, int32_t z, int32_t size, bool air) {
        for (int32_t row = 0; row < size; ++row)
            for (int32_t column = 0; column < size; ++column)
                (air ? plots.air : plots.ground)
                    [static_cast<std::size_t>(z + row) * width +
                     static_cast<std::size_t>(x + column)] = id;
    };
    claim(1, 0, 0, 2, false);
    claim(2, 3, 0, 2, false);
    claim(3, 3, 0, 1, true);
    std::vector<benchmark::AuditUnit> units{
        {1, 0, 0, 2, 2, false},
        {2, 3, 0, 2, 2, false},
        {3, 3, 0, 1, 1, true},
        {4, 7, 5, 2, 2, false}
    };
    benchmark::OwnershipAudit audit;
    std::vector<uint8_t> scratch;
    benchmark::audit_ownership(audit, 0, width, height, units, &plots, read, scratch);
    CHECK(audit.audits == 1 && audit.audited_units == 3 && audit.outside_units == 1);
    CHECK(audit.ownership_errors == 0 && audit.overlap_cells == 0 && !audit.first_error_tick);
    // Unit 5 stands over unit 2's cell (4, 1); the plot names unit 2.
    units.push_back({5, 4, 1, 2, 2, false});
    plots.ground[2 * width + 4] = 5;
    plots.ground[2 * width + 5] = 5;
    plots.ground[1 * width + 5] = 5;
    benchmark::audit_ownership(audit, 150, width, height, units, &plots, read, scratch);
    CHECK(audit.audits == 2 && audit.audited_units == 7);
    CHECK(audit.ownership_errors == 1 && audit.units_with_errors == 1);
    CHECK(audit.overlap_cells == 1 && audit.first_error_tick == 150u);
    benchmark::audit_ownership(audit, 300, width, height, units, &plots, read, scratch);
    CHECK(audit.first_error_tick == 150u && audit.overlap_cells == 2);
    CHECK(audit.ownership_errors == 2);
}

/// Checks the footprint origins a ground step passes: within a cell, across a
/// column, past a corner on either side, exactly through a corner, backwards,
/// with a larger footprint, and the steps it refuses.
void ground_step_paths() {
    constexpr int32_t pixel = 65536;
    const auto step = [&](std::array<int32_t, 4> pixels, int32_t size) {
        benchmark::GroundStep made;
        made.from = {pixels[0] * pixel, pixels[1] * pixel};
        made.to = {pixels[2] * pixel, pixels[3] * pixel};
        made.footprint = {size, size};
        // The cell of a 16.16 position less the footprint's half-size plus half a cell.
        const auto origin = [&](int32_t at) {
            const int64_t cell = 16 * pixel;
            const int64_t biased = int64_t{at} + 8 * pixel - int64_t{size} * 8 * pixel;
            return static_cast<int32_t>(
                biased >= 0 ? biased / cell : -((-biased + cell - 1) / cell)
            );
        };
        made.from_cell = {origin(made.from[0]), origin(made.from[1])};
        made.to_cell = {origin(made.to[0]), origin(made.to[1])};
        return made;
    };
    using Cells = std::vector<benchmark::Cell>;
    Cells origins;
    CHECK(benchmark::ground_step_origins(step({2, 3, 12, 14}, 1), origins));
    CHECK((origins == Cells{{0, 0}}));
    CHECK(benchmark::ground_step_origins(step({8, 4, 20, 4}, 1), origins));
    CHECK((origins == Cells{{0, 0}, {1, 0}}));
    // Past the corner on the column side, then on the row side.
    CHECK(benchmark::ground_step_origins(step({8, 4, 20, 18}, 1), origins));
    CHECK((origins == Cells{{0, 0}, {1, 0}, {1, 1}}));
    CHECK(benchmark::ground_step_origins(step({4, 8, 18, 20}, 1), origins));
    CHECK((origins == Cells{{0, 0}, {0, 1}, {1, 1}}));
    // Exactly through the corner: both cells beside it.
    CHECK(benchmark::ground_step_origins(step({8, 8, 24, 24}, 1), origins));
    CHECK((origins == Cells{{0, 0}, {1, 0}, {0, 1}, {1, 1}}));
    CHECK(benchmark::ground_step_origins(step({20, 18, 8, 4}, 1), origins));
    CHECK((origins == Cells{{1, 1}, {1, 0}, {0, 0}}));
    CHECK(benchmark::ground_step_origins(step({-4, 8, -20, 8}, 1), origins));
    CHECK((origins == Cells{{-1, 0}, {-2, 0}}));
    // A two-cell footprint's origin is half a cell up and left of its centre's cell.
    CHECK(benchmark::ground_step_origins(step({16, 16, 40, 16}, 2), origins));
    CHECK((origins == Cells{{0, 0}, {1, 0}, {2, 0}}));
    auto wrong = step({8, 4, 20, 4}, 1);
    wrong.to_cell = {2, 0};
    CHECK(!benchmark::ground_step_origins(wrong, origins) && origins.empty());
    CHECK(!benchmark::ground_step_origins(step({8, 4, 8 + 16 * 128, 4}, 1), origins));
    CHECK(benchmark::ground_step_origins(step({8, 4, 8 + 16 * 127, 4}, 1), origins));
    CHECK(origins.size() == benchmark::swept_step_origin_limit);
    CHECK(!benchmark::ground_step_origins(step({8, 4, 20, 4}, 0), origins));
}

/// Checks the swept-step audit: a corner cell only a diagonal passes, the
/// exact corner, a unit's cell, a step's own start, and unchecked steps.
void swept_step_audits() {
    constexpr int32_t pixel = 65536;
    const auto step = [](std::array<int32_t, 4> pixels, benchmark::Cell from, benchmark::Cell to) {
        benchmark::GroundStep made;
        made.from = {pixels[0] * pixel, pixels[1] * pixel};
        made.to = {pixels[2] * pixel, pixels[3] * pixel};
        made.footprint = {1, 1};
        made.from_cell = from;
        made.to_cell = to;
        return made;
    };
    // Cell (1, 0) is a cliff and cell (0, 1) another unit; (0, 0) is blocked
    // too, but a step's own start is never read.
    const auto read = [](const void*, benchmark::Cell origin) {
        if (origin == benchmark::Cell{1, 0} || origin == benchmark::Cell{0, 0})
            return benchmark::OriginBlock::ground;
        if (origin == benchmark::Cell{0, 1})
            return benchmark::OriginBlock::unit;
        return benchmark::OriginBlock::clear;
    };
    benchmark::SweptStepAudit audit;
    std::vector<benchmark::Cell> scratch;
    benchmark::audit_ground_step(
        audit, 40, step({4, 30, 20, 30}, {0, 1}, {1, 1}), nullptr, read, scratch
    );
    CHECK(audit.steps == 1 && audit.misses == 0 && !audit.first_miss_tick);
    benchmark::audit_ground_step(
        audit, 41, step({8, 4, 20, 18}, {0, 0}, {1, 1}), nullptr, read, scratch
    );
    CHECK(audit.steps == 2 && audit.misses == 1 && audit.ground_misses == 1);
    CHECK(audit.first_miss_tick == 41u);
    benchmark::audit_ground_step(
        audit, 42, step({4, 8, 18, 20}, {0, 0}, {1, 1}), nullptr, read, scratch
    );
    CHECK(audit.misses == 2 && audit.unit_misses == 1);
    // The exact corner reads both sides; the first blocked one names the miss.
    benchmark::audit_ground_step(
        audit, 43, step({8, 8, 24, 24}, {0, 0}, {1, 1}), nullptr, read, scratch
    );
    CHECK(audit.misses == 3 && audit.ground_misses == 2 && audit.unit_misses == 1);
    benchmark::audit_ground_step(
        audit, 44, step({8, 4, 20, 4}, {0, 0}, {2, 0}), nullptr, read, scratch
    );
    CHECK(audit.steps == 5 && audit.unchecked_steps == 1 && audit.misses == 3);
    CHECK(audit.first_miss_tick == 41u);
}

/// Checks the staged battle's report block and its absence for other scenarios.
void staged_report() {
    benchmark::Report report;
    report.scenario = "combat";
    CHECK(benchmark::report_json(report).find("\"staged_battle\": null") != std::string::npos);
    report.scenario = "stage";
    benchmark::StagedBattle staged;
    staged.stage = "aligned\\patrol.stage";
    staged.window_ticks = benchmark::stationary_window_ticks;
    staged.lines.push_back({"north", {5120, 4800}, {5120, 5000}});
    benchmark::StagedPlayer player;
    player.player = 1;
    player.computer = true;
    player.tracked_units = 10;
    player.deaths = 2;
    player.mean_progress_pixels = 12.5;
    player.crossers = {4};
    player.strike_targets.samples = 2;
    player.strike_targets.largest_share = 0.75;
    player.strike_churn = {8, 3};
    player.window_live_unit_ticks = 400;
    player.window_stationary_unit_ticks = 120;
    player.window_engaged_stationary_unit_ticks = 70;
    player.stationary_throughout = 5;
    player.stationary_throughout_unengaged = 3;
    staged.players.push_back(player);
    staged.audit.audits = 61;
    staged.audit.first_error_tick = 450u;
    staged.swept_steps.steps = 90;
    staged.swept_steps.misses = 3;
    staged.swept_steps.ground_misses = 1;
    staged.swept_steps.unit_misses = 2;
    staged.swept_steps.first_miss_tick = 17u;
    report.staged = staged;
    const auto json = benchmark::report_json(report);
    CHECK(json.find("\"stage\": \"aligned\\\\patrol.stage\"") != std::string::npos);
    CHECK(json.find("\"window_ticks\": 300") != std::string::npos);
    CHECK(
        json.find("{\"name\": \"north\", \"from\": [5120, 4800], \"to\": [5120, 5000]}") !=
        std::string::npos
    );
    CHECK(
        json.find("{\"player\": 1, \"computer\": true, \"tracked_units\": 10") != std::string::npos
    );
    CHECK(json.find("\"deaths\": 2") != std::string::npos);
    CHECK(json.find("\"mean_progress_pixels\": 12.5") != std::string::npos);
    CHECK(json.find("\"crossers\": [4]") != std::string::npos);
    CHECK(
        json.find(
            "\"largest_share\": 0.75, \"compared_attackers\": 8, \"switched_attackers\": 3}"
        ) != std::string::npos
    );
    CHECK(json.find("\"audits\": 61") != std::string::npos);
    CHECK(json.find("\"first_error_tick\": 450") != std::string::npos);
    CHECK(
        json.find(
            "\"swept_steps\": {\"steps\": 90, \"unchecked_steps\": 0, \"misses\": 3, "
            "\"ground_misses\": 1, \"unit_misses\": 2, \"first_miss_tick\": 17}"
        ) != std::string::npos
    );
    CHECK(
        json.find(
            "\"extra\": {\"swept_misses\": 3, \"stationary_engaged_pct\": [0, 17.500], "
            "\"stationary_unengaged_pct\": [0, 12.500], \"stationary_unengaged_pct_all\": 12.500, "
            "\"stationary_throughout_unengaged\": [0, 3], "
            "\"stationary_throughout_unengaged_all\": 3, \"strike_target_churn_pct\": [0, 37.500]}"
        ) != std::string::npos
    );
    CHECK(
        json.find(
            "\"window_engaged_stationary_unit_ticks\": 70, \"stationary_throughout\": 5, "
            "\"stationary_throughout_unengaged\": 3"
        ) != std::string::npos
    );
    // Two-thirds rounds up at the third decimal; the pooled share counts both players.
    benchmark::StagedPlayer human;
    human.window_live_unit_ticks = 3;
    human.window_stationary_unit_ticks = 2;
    human.strike_churn = {3, 2};
    report.staged->players.insert(report.staged->players.begin(), human);
    const auto pooled = benchmark::report_json(report);
    CHECK(
        pooled.find(
            "\"stationary_engaged_pct\": [0.000, 17.500], \"stationary_unengaged_pct\": "
            "[66.667, 12.500], \"stationary_unengaged_pct_all\": 12.903"
        ) != std::string::npos
    );
    CHECK(pooled.find("\"strike_target_churn_pct\": [66.667, 37.500]}") != std::string::npos);
    report.staged->players.clear();
    CHECK(
        benchmark::report_json(report).find(
            "\"stationary_engaged_pct\": [], \"stationary_unengaged_pct\": [], "
            "\"stationary_unengaged_pct_all\": 0.000"
        ) != std::string::npos
    );
    CHECK(
        benchmark::report_json(report).find("\"strike_target_churn_pct\": []}") != std::string::npos
    );
    // A player with no compared attacker has no churn.
    report.staged->players.push_back({});
    CHECK(
        benchmark::report_json(report).find("\"strike_target_churn_pct\": [0.000]}") !=
        std::string::npos
    );
    report.staged->players.clear();
    report.staged->audit.first_error_tick.reset();
    report.staged->swept_steps = {};
    const auto cleared = benchmark::report_json(report);
    CHECK(cleared.find("\"first_error_tick\": null") != std::string::npos);
    CHECK(cleared.find("\"first_miss_tick\": null") != std::string::npos);
    CHECK(cleared.find("\"extra\": {\"swept_misses\": 0, ") != std::string::npos);
}

} // namespace

int main() {
    timings();
    layouts();
    group_arrivals();
    progress();
    reports();
    crossing_lines();
    staged_units();
    target_spread();
    target_churn();
    ownership_audits();
    ground_step_paths();
    swept_step_audits();
    staged_engagement();
    staged_report();
    return failures == 0 ? 0 : 1;
}
