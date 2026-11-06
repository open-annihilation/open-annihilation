// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/battle_benchmark.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string_view>
#include <utility>

namespace oa::app::battle_benchmark {
namespace {

/// Names the largest whole map coordinate positive 16.16 positions can hold.
constexpr uint32_t maximum_map_extent = 32767;
constexpr std::size_t percentile_denominator = 100;
constexpr std::size_t median_percentile = 50;
constexpr std::size_t tail_percentile = 95;
constexpr std::size_t slow_percentile = 99;
constexpr int double_decimal_digits = 17;

struct Grid {
    uint64_t front_left{};
    uint64_t front_right{};
    uint64_t rows{};
    uint64_t columns{};
};

/// Returns equal opposing grid dimensions, or none for invalid map dimensions.
std::optional<Grid>
grid_geometry(uint32_t width, uint32_t height, uint32_t spacing, uint32_t margin, uint32_t gap) {
    if (width > maximum_map_extent || height > maximum_map_extent || spacing == 0 || margin == 0 ||
        gap == 0)
        return {};
    const auto edge_room = static_cast<uint64_t>(margin) * 2;
    if (width <= edge_room + gap || height <= edge_room)
        return {};
    const auto centre_x = width / 2;
    const auto half_gap = (static_cast<uint64_t>(gap) + 1) / 2;
    if (centre_x < margin + half_gap)
        return {};
    const auto front_left = centre_x - half_gap;
    const auto front_right = centre_x + half_gap;
    const auto left_columns = (front_left - margin) / spacing + 1;
    const auto right_columns = (width - margin - front_right) / spacing + 1;
    return Grid{
        front_left,
        front_right,
        (height - edge_room) / spacing + 1,
        std::min(left_columns, right_columns)
    };
}

/// Returns a nearest-rank percentile without overflowing the sample count.
uint64_t percentile(const std::vector<uint64_t>& ordered, std::size_t percent) {
    const auto whole = ordered.size() / percentile_denominator * percent;
    const auto tail =
        (ordered.size() % percentile_denominator * percent + percentile_denominator - 1) /
        percentile_denominator;
    return ordered[whole + tail - 1];
}

/// Writes one JSON string, escaping quotes, reverse solidi and control bytes.
void json_string(std::ostream& out, std::string_view text) {
    constexpr char hex_digits[] = "0123456789abcdef";
    constexpr unsigned control_byte_end = 32;
    constexpr unsigned hexadecimal_mask = 15;
    constexpr unsigned hexadecimal_shift = 4;
    out << '"';
    for (const auto byte : text) {
        const auto value = static_cast<unsigned char>(byte);
        if (byte == '"' || byte == '\\')
            out << '\\' << byte;
        else if (value < control_byte_end)
            out << "\\u00" << hex_digits[value >> hexadecimal_shift]
                << hex_digits[value & hexadecimal_mask];
        else
            out << byte;
    }
    out << '"';
}

} // namespace

std::size_t maximum_layout_units(
    uint32_t width, uint32_t height, uint32_t spacing, uint32_t margin, uint32_t gap
) {
    const auto grid = grid_geometry(width, height, spacing, margin, gap);
    if (!grid)
        return 0;
    return static_cast<std::size_t>(
        std::min<uint64_t>(maximum_units_per_side, grid->columns * grid->rows)
    );
}

LayoutResult plan_layout(
    uint32_t width,
    uint32_t height,
    std::size_t per_side,
    uint32_t spacing,
    uint32_t margin,
    uint32_t gap
) {
    if (per_side > maximum_units_per_side)
        return {{}, "army exceeds the supported unit capacity"};
    if (width > maximum_map_extent || height > maximum_map_extent)
        return {{}, "map dimensions exceed positive 16.16 coordinates"};
    if (spacing == 0 || margin == 0 || gap == 0)
        return {{}, "spacing, edge margin and front gap must be positive"};
    const auto grid = grid_geometry(width, height, spacing, margin, gap);
    if (!grid)
        return {{}, "map is too small for the army margins and front gap"};
    if (per_side == 0)
        return {Layout{}, {}};
    if (per_side > grid->columns * grid->rows)
        return {{}, "map cannot hold the requested armies at the chosen spacing"};
    const auto rows = std::min<uint64_t>(per_side, grid->rows);
    const auto grid_height = (rows - 1) * spacing;
    const auto top = (height - grid_height) / 2;
    Layout layout;
    for (auto& army : layout.armies)
        army.reserve(per_side);
    for (std::size_t index = 0; index < per_side; ++index) {
        const auto distance = index / rows * spacing;
        const auto z = static_cast<int32_t>(top + index % rows * spacing);
        layout.armies[0].push_back({static_cast<int32_t>(grid->front_left - distance), z});
        layout.armies[1].push_back({static_cast<int32_t>(grid->front_right + distance), z});
    }
    return {std::move(layout), {}};
}

Timing summarize(std::span<const uint64_t> samples) {
    if (samples.empty())
        return {};
    std::vector<uint64_t> ordered(samples.begin(), samples.end());
    std::sort(ordered.begin(), ordered.end());
    double total = 0;
    for (const auto sample : ordered)
        total += static_cast<double>(sample);
    return {
        static_cast<uint64_t>(ordered.size()),
        ordered.front(),
        percentile(ordered, median_percentile),
        percentile(ordered, tail_percentile),
        percentile(ordered, slow_percentile),
        ordered.back(),
        total / static_cast<double>(ordered.size())
    };
}

std::string report_json(const Report& report) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(double_decimal_digits);
    const auto text = [&](std::string_view key, std::string_view value) {
        out << ",\n  ";
        json_string(out, key);
        out << ": ";
        json_string(out, value);
    };
    const auto number = [&](std::string_view key, uint64_t value) {
        out << ",\n  ";
        json_string(out, key);
        out << ": " << value;
    };
    out << "{\n  \"schema_version\": " << report_schema_version;
    out << ",\n  \"completed\": " << (report.completed ? "true" : "false");
    text("error", report.error);
    text("engine_version", report.engine_version);
    text("map", report.map);
    text("scenario", report.scenario);
    number("seed", report.seed);
    number("unit_limit", report.unit_limit);
    number("requested_units_per_side", report.requested_units_per_side);
    number("initial_units", report.initial_units);
    number("final_units", report.final_units);
    number("warmup_ticks", report.warmup_ticks);
    number("measured_ticks", report.measured_ticks);
    number("successful_warmup_ticks", report.successful_warmup_ticks);
    number("successful_ticks", report.successful_ticks);
    out << ",\n  \"enhanced_mode\": " << (report.enhanced_mode ? "true" : "false");
    number("enhanced_version", report.enhanced_version);
    text("simulation_hash", report.simulation_hash);
    text("world_digest", report.world_digest);
    text("renderer", report.renderer);
    out << ",\n  \"tick_ns\": {\n    \"count\": " << report.ticks.count
        << ",\n    \"min\": " << report.ticks.min_ns << ",\n    \"p50\": " << report.ticks.p50_ns
        << ",\n    \"p95\": " << report.ticks.p95_ns << ",\n    \"p99\": " << report.ticks.p99_ns
        << ",\n    \"max\": " << report.ticks.max_ns << ",\n    \"mean\": ";
    if (std::isfinite(report.ticks.mean_ns))
        out << report.ticks.mean_ns;
    else
        out << "null";
    out << "\n  }\n}\n";
    return out.str();
}

} // namespace oa::app::battle_benchmark
