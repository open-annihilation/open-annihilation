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
inline constexpr std::size_t maximum_units_per_side = 10'000;
/// Bounds placement candidates so blocked terrain can be skipped before spawning.
inline constexpr std::size_t maximum_layout_candidates = 1'000'000;
inline constexpr uint32_t default_spacing = 32; ///< map pixels between unit centres
inline constexpr uint32_t default_margin = 64;  ///< map pixels kept clear at map edges
inline constexpr uint32_t default_gap = 96;     ///< map pixels between the two fronts
inline constexpr uint32_t arrival_spacing = 16; ///< map pixels between arrival candidates
inline constexpr uint32_t shared_goal_region_pixels = 512;

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

/// Counts candidate positions per half-map, bounded by maximum_layout_candidates.
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
/// @param per_side candidate positions in each army, at most maximum_layout_candidates
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

/// Orders arrival candidates in the opposite half-map by shared goal region.
///
/// Complete 512-pixel regions precede partial edge regions, so a clear
/// patch holds 1,000 distinct one-cell arrivals. Positions in one region
/// are contiguous; the caller may skip blocked points in that region.
/// No point's terrain or footprint is checked here.
///
/// @param width map width in whole pixels, at most 32767
/// @param height map height in whole pixels, at most 32767
/// @param per_side arrival candidates per side, at most maximum_layout_candidates
/// @return opposite-side candidate patches, or a refusal when they cannot fit
[[nodiscard]] LayoutResult
plan_group_arrivals(uint32_t width, uint32_t height, std::size_t per_side);

/// Holds the occupied size of an arriving unit, in 16-pixel map cells.
struct Footprint {
    uint16_t x{1};
    uint16_t z{1};
};

/// Checks the actual arriving type's terrain and static obstacles.
struct ArrivalAccess {
    void* context{};
    bool (*can_stand)(void*, uint8_t side, std::size_t unit, Point){};
};

/// Selects terrain-valid, nonoverlapping terminal footprints from candidates.
///
/// Call before placing the additional armies, or use a static-obstacle-only
/// predicate. An opposing army occupying a destination now is not a static
/// obstacle to its later use. Candidate positions and footprints follow the
/// game's centre-to-cell rounding. Every side receives exactly the requested
/// number of points, or neither receives a layout.
///
/// @param width map width in whole pixels, at most 32767
/// @param height map height in whole pixels, at most 32767
/// @param candidates opposite-side arrival candidates in visit order
/// @param footprints actual arriving footprints, commander included, by unit order
/// @param access predicate for the corresponding actual type, or null to test geometry only
/// @return distinct terminal sites, or a refusal without a partial layout
[[nodiscard]] LayoutResult select_arrivals(
    uint32_t width,
    uint32_t height,
    const Layout& candidates,
    const std::array<std::span<const Footprint>, 2>& footprints,
    ArrivalAccess access = {}
);

/// Tracks one assigned goal outside the timed simulation step.
struct ProgressState {
    Point start{}, previous{}, goal{};
    uint64_t initial_distance{}, best_distance{};
    uint64_t stationary_ticks{}, stationary_run{}, longest_stall{};
    uint32_t progress_distance{arrival_spacing};
    uint32_t arrival_distance{arrival_spacing};
    bool moved{}, arrived{}, live{true};
};

/// Holds counts for the near, middle and far thirds of initial trip distance.
struct DistanceThird {
    uint64_t goal_units{}, progressed_units{}, arrived_units{};
};

/// Holds per-army progress; stationary samples do not assert a collision cause.
struct ProgressSummary {
    uint64_t goal_units{}, live_units{}, moved_units{}, progressed_units{}, arrived_units{};
    uint64_t stationary_unit_ticks{}, longest_stall_ticks{};
    uint64_t initial_goal_distance_pixels{}, final_goal_distance_pixels{};
    std::array<DistanceThird, 3> distance_thirds{};
};

/// Begins whole-pixel Manhattan-distance progress sampling.
///
/// @param start position when the command was assigned
/// @param goal assigned terminal position
/// @param progress_distance minimum movement or improvement that counts, in pixels
/// @param arrival_distance goal distance that counts as arrival, in pixels
/// @return initialized sampling state; zero thresholds are held at one pixel
[[nodiscard]] ProgressState begin_progress(
    Point start,
    Point goal,
    uint32_t progress_distance = arrival_spacing,
    uint32_t arrival_distance = arrival_spacing
);

/// Samples one completed tick without allocating or changing game state.
///
/// @param[in,out] state sampling state
/// @param position current whole-pixel position
/// @param live whether the assigned unit still exists
void sample_progress(ProgressState& state, Point position, bool live);

/// Summarizes counts and trip-distance thirds without changing the samples.
///
/// @param states one state per originally assigned unit
/// @return counts over setup, warmup and measured samples supplied by the caller
[[nodiscard]] ProgressSummary summarize_progress(std::span<const ProgressState> states);

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

/// Holds the measured contribution of one simulation phase.
struct PhaseTiming {
    std::string name{};
    Timing ticks{};
};

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
    uint64_t moved_unit_ticks{};
    uint64_t damaged_unit_ticks{};
    uint64_t projectile_high_water{};
    uint64_t projectile_capacity{};
    uint64_t projectile_allocation_failures{};
    uint64_t projection_preparations{};
    uint64_t projected_slots{};
    uint64_t bucket_queries{};
    uint64_t bucket_candidates{};
    uint64_t shared_route_requests{}, shared_route_hits{}, shared_route_fields{};
    uint64_t shared_route_cells{}, shared_route_waits{}, shared_route_invalidations{};
    uint64_t full_route_searches{};
    uint64_t local_flow_probes{}, local_flow_moving_blockers{}, local_flow_steering_adjustments{};
    uint64_t local_flow_yielded_ticks{}, local_flow_blocked_ticks{}, local_flow_deferred_replans{};
    uint64_t target_queries{}, target_candidates{}, target_full_scans{};
    bool goal_metrics_available{};
    std::array<ProgressSummary, 2> goal_progress{};
    std::string routing_metrics_scope{"cumulative_setup_warmup_and_measured"};
    bool activity_observed{};
    std::string activity_sampling_scope{"after_each_successful_warmup_and_measured_tick"};
    bool enhanced_mode{};
    uint32_t enhanced_version{};
    std::string simulation_hash{};
    std::string world_digest{};
    std::string renderer{"none"};
    Timing ticks{};
    std::vector<PhaseTiming> simulation_phases{};
};

/// Writes a UTF-8 JSON object with stable keys and locale-independent numbers.
///
/// @param report benchmark facts with UTF-8 text fields
/// @return the schema-versioned JSON object, ending with a newline
[[nodiscard]] std::string report_json(const Report& report);

} // namespace oa::app::battle_benchmark
