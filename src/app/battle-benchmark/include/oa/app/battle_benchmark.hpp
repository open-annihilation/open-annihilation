// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oa::app::battle_benchmark {

inline constexpr uint32_t report_schema_version = 1;
inline constexpr std::size_t maximum_units_per_side = 6553;
inline constexpr uint32_t default_spacing = 32; ///< map pixels between unit centres
inline constexpr uint32_t default_margin = 64;  ///< map pixels kept clear at map edges
inline constexpr uint32_t default_gap = 96;     ///< map pixels between the two fronts

/// Holds a unit centre in whole map pixels.
struct Point {
    int32_t x{};
    int32_t z{};
    /// Compares both whole-pixel coordinates.
    bool operator==(const Point&) const = default;
};

/// Holds equal armies, each ordered from its front towards its rear.
struct Layout {
    std::array<std::vector<Point>, 2> armies{};
};

struct LayoutResult {
    std::optional<Layout> layout{};
    std::string error{};
};

/// Counts the units each half-map can hold, bounded by maximum_units_per_side.
///
/// @param width map width in whole pixels, at most 32767
/// @param height map height in whole pixels, at most 32767
/// @param spacing positive distance between unit centres in map pixels
/// @param margin positive distance kept inside each map edge in map pixels
/// @param gap positive minimum distance between opposing fronts in map pixels
/// @return the smaller half's grid capacity, or zero when dimensions are invalid
[[nodiscard]] std::size_t maximum_layout_units(
    uint32_t width,
    uint32_t height,
    uint32_t spacing = default_spacing,
    uint32_t margin = default_margin,
    uint32_t gap = default_gap
);

/// Places two symmetric grids inside the map without testing terrain or occupancy.
///
/// @param width map width in whole pixels, at most 32767
/// @param height map height in whole pixels, at most 32767
/// @param per_side number of added units in each army, at most maximum_units_per_side
/// @param spacing positive distance between unit centres in map pixels
/// @param margin positive distance kept inside each map edge in map pixels
/// @param gap positive minimum distance between opposing fronts in map pixels
/// @return the complete layout, or a refusal without any points
[[nodiscard]] LayoutResult plan_layout(
    uint32_t width,
    uint32_t height,
    std::size_t per_side,
    uint32_t spacing = default_spacing,
    uint32_t margin = default_margin,
    uint32_t gap = default_gap
);

/// Holds nearest-rank timing percentiles, in nanoseconds.
struct Timing {
    uint64_t count{};
    uint64_t min_ns{};
    uint64_t p50_ns{};
    uint64_t p95_ns{};
    uint64_t p99_ns{};
    uint64_t max_ns{};
    double mean_ns{};
};

/// Summarizes samples without changing their order in the caller's storage.
///
/// @param samples elapsed nanoseconds of successful measured ticks
/// @return nearest-rank statistics, or all zeroes when there are no samples
[[nodiscard]] Timing summarize(std::span<const uint64_t> samples);

/// Holds one simulation-only benchmark's result and reproducibility facts.
struct Report {
    bool completed{};
    std::string error{};
    std::string engine_version{};
    std::string map{};
    std::string scenario{};
    uint32_t seed{};
    uint32_t unit_limit{};
    std::size_t requested_units_per_side{};
    std::size_t initial_units{};
    std::size_t final_units{};
    std::size_t warmup_ticks{};
    std::size_t measured_ticks{};
    std::size_t successful_warmup_ticks{};
    std::size_t successful_ticks{};
    bool enhanced_mode{};
    uint32_t enhanced_version{};
    std::string simulation_hash{};
    std::string world_digest{};
    std::string renderer{"none"};
    Timing ticks{};
};

/// Writes a UTF-8 JSON object with stable keys and locale-independent numbers.
///
/// @param report benchmark facts with UTF-8 text fields
/// @return the schema-versioned JSON object, ending with a newline
[[nodiscard]] std::string report_json(const Report& report);

} // namespace oa::app::battle_benchmark
