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
    CHECK(benchmark::maximum_layout_units(width, height) == benchmark::maximum_units_per_side);
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
    CHECK(!benchmark::plan_layout(width, height, benchmark::maximum_units_per_side + 1).layout);
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
    report.enhanced_mode = true;
    report.enhanced_version = 1;
    report.simulation_hash = "hash";
    report.world_digest = "digest";
    report.ticks.mean_ns = 1.5;
    const auto old_locale = std::locale();
    std::locale::global(std::locale(old_locale, new DecimalComma));
    const auto json = benchmark::report_json(report);
    std::locale::global(old_locale);
    CHECK(json.find("\"schema_version\": 1") != std::string::npos);
    CHECK(json.find("\"completed\": true") != std::string::npos);
    CHECK(json.find("Map \\\"name\\\"\\\\path\\u000a\\u0009\\u0000é") != std::string::npos);
    CHECK(json.find("\"renderer\": \"none\"") != std::string::npos);
    CHECK(json.find("\"mean\": 1.5") != std::string::npos);
    CHECK(json.find("\"successful_ticks\": 100") != std::string::npos);
    CHECK(json.find("\"enhanced_version\": 1") != std::string::npos);
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
    reports();
    return failures == 0 ? 0 : 1;
}
