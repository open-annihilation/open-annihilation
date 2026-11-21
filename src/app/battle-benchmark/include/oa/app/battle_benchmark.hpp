// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::battle_benchmark {

inline constexpr uint32_t report_schema_version = 1;
/// Nanoseconds allowed by the benchmark's 30-tick-per-second reference budget.
inline constexpr uint64_t tick_budget_ns = 33'333'333;
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

/// The final measured ticks over which a staged battle's stationary share is counted.
inline constexpr uint32_t stationary_window_ticks = 300;
/// Measured ticks between two ownership audits of a staged battle.
inline constexpr uint32_t audit_interval_ticks = 150;
/// Measured ticks between two samples of a staged battle's strike targets.
inline constexpr uint32_t target_sample_interval_ticks = 30;
/// The most crossing lines one staged battle counts.
inline constexpr std::size_t maximum_crossing_lines = 8;
/// The fewest pixels a unit must gain on its destination to count as advanced.
inline constexpr double advance_distance_pixels = 16.0;

/// A named measure line a staged battle counts crossers of, in whole map
/// pixels: a bridge, a choke or a river bank. It does nothing to the battle.
struct CrossingLine {
    std::string name{};
    Point from{};
    Point to{};
};

/// Reads a crossing line written NAME=X0,Z0,X1,Z1.
///
/// @param text the line; NAME is 1 to 32 letters, digits, '-' or '_'
/// @return the line, or none for a malformed text or a line of zero length
[[nodiscard]] std::optional<CrossingLine> parse_crossing_line(std::string_view text);

/// Tells which side of a crossing line, extended without end, a point lies on.
///
/// @param line the line
/// @param point the point
/// @return the sign of the cross product of (to - from) and (point - from): 1 on
///     one side, -1 on the other, 0 on the line
[[nodiscard]] int line_side(const CrossingLine& line, Point point) noexcept;

/// Tells whether a step crosses a crossing line's segment from one side to the other.
///
/// A step that only reaches the line, or leaves it, does not cross.
///
/// @param line the line
/// @param before where the step starts
/// @param after where the step ends
/// @return true when the step's two ends lie on opposite sides of the line and the
///     line's ends do not both lie on one side of the step
[[nodiscard]] bool crosses_line(const CrossingLine& line, Point before, Point after) noexcept;

/// Tracks one unit a stage placed, from the end of staging to the end of the run.
struct StagedUnit {
    uint8_t player{};
    Point start{};
    Point position{}; ///< last live position
    /// The point of the first order the unit held when staging ended, when that
    /// order names a point and no unit: its move or patrol destination.
    std::optional<Point> goal{};
    bool live{true};
    std::array<int8_t, maximum_crossing_lines> start_side{}; ///< line_side of `start`
    uint8_t crossed{}; ///< one bit a line: crossed away from its start side
    uint64_t window_live_ticks{};
    uint64_t window_stationary_ticks{};
};

/// Begins tracking a unit where staging left it.
///
/// @param player the unit's owner
/// @param start its position
/// @param goal its destination, if its first order names one
/// @param lines the battle's crossing lines, at most maximum_crossing_lines
/// @return the tracking state
[[nodiscard]] StagedUnit begin_staged_unit(
    uint8_t player, Point start, std::optional<Point> goal, std::span<const CrossingLine> lines
);

/// Samples a tracked unit after one completed tick.
///
/// Once a unit is gone it stays gone: a later unit in its slot is not it.
///
/// @param[in,out] unit the tracking state
/// @param lines the battle's crossing lines
/// @param position its position now, in whole map pixels
/// @param moved whether its exact position changed during the tick
/// @param live whether it is still the same live unit
/// @param in_window whether the tick is one of the final stationary_window_ticks
void sample_staged_unit(
    StagedUnit& unit,
    std::span<const CrossingLine> lines,
    Point position,
    bool moved,
    bool live,
    bool in_window
);

/// Strike targets of one player's units, sampled every target_sample_interval_ticks.
struct TargetSpread {
    uint64_t samples{};          ///< samples with at least one attacker
    uint64_t attackers{};        ///< attackers summed over those samples
    uint64_t distinct_targets{}; ///< distinct targets summed over those samples
    double largest_share{};      ///< share of attackers on the commonest target, summed
};

/// Adds one sample: the unit each of a player's attackers is ordered to attack.
///
/// @param[in,out] spread the player's samples so far
/// @param targets one entry an attacker; sorted in place
void add_target_sample(TargetSpread& spread, std::span<uint16_t> targets);

/// A mobile unit whose footprint an ownership audit checks.
struct AuditUnit {
    uint16_t id{};
    int32_t cell_x{}, cell_z{};           ///< footprint origin cell
    int32_t footprint_x{}, footprint_z{}; ///< footprint size in cells
    bool air{};                           ///< claims the air layer instead of the ground
};

/// What the ownership audits of a staged battle found.
struct OwnershipAudit {
    uint64_t audits{};
    uint64_t audited_units{};     ///< units checked, summed over audits
    uint64_t outside_units{};     ///< units skipped with a footprint past a map edge
    uint64_t ownership_errors{};  ///< footprint cells whose occupant word names another unit
    uint64_t units_with_errors{}; ///< units with at least one such cell, summed over audits
    uint64_t overlap_cells{};     ///< cells inside two ground units' footprints, summed
    std::optional<uint32_t> first_error_tick{};
};

/// Reads the occupant word of a map cell on the ground or air layer.
using OccupantReader = uint16_t (*)(const void* context, std::size_t cell, bool air);

/// Checks that every audited unit's footprint cells name it on its layer and
/// that no map cell lies inside two ground units' footprints.
///
/// A footprint with a cell past the map's last row or column is skipped, as
/// the game leaves such a unit off the map's cells.
///
/// @param[in,out] audit the findings so far
/// @param tick the measured tick
/// @param width map width in cells
/// @param height map height in cells
/// @param units the live, finished, unattached mobile units
/// @param context passed to `occupant`
/// @param occupant reads a cell's occupant word
/// @param[in,out] scratch reused between audits
void audit_ownership(
    OwnershipAudit& audit,
    uint32_t tick,
    uint32_t width,
    uint32_t height,
    std::span<const AuditUnit> units,
    const void* context,
    OccupantReader occupant,
    std::vector<uint8_t>& scratch
);

/// One player's results in a staged battle.
struct StagedPlayer {
    uint8_t player{};
    bool computer{}; ///< a computer player controls it
    uint64_t tracked_units{};
    uint64_t live_tracked_units{};
    uint64_t births{}; ///< units of this player that appeared during the run
    uint64_t deaths{}; ///< units of this player that died during the run, born ones too
    uint64_t goal_units{};
    double mean_progress_pixels{}; ///< over goal units: start distance less final distance
    uint64_t advanced_units{};     ///< goal units that gained advance_distance_pixels or more
    uint64_t window_live_unit_ticks{};
    uint64_t window_stationary_unit_ticks{};
    uint64_t stationary_throughout{}; ///< live through the window and never moved in it
    std::vector<uint64_t> crossers{}; ///< one count a crossing line
    TargetSpread strike_targets{};
};

/// Summarizes the tracked units of each player that has any.
///
/// @param units every tracked unit
/// @param line_count the battle's crossing lines
/// @param window_ticks the stationary window's measured ticks
/// @return one entry a player, by player index; births, deaths, computer and
///     strike targets left for the caller
[[nodiscard]] std::vector<StagedPlayer>
summarize_staged(std::span<const StagedUnit> units, std::size_t line_count, uint64_t window_ticks);

/// A staged battle's measured results.
struct StagedBattle {
    std::string stage{};
    std::vector<CrossingLine> lines{};
    uint32_t window_ticks{};
    std::vector<StagedPlayer> players{};
    OwnershipAudit audit{};
};

/// Holds nearest-rank timings, cumulative nanoseconds and reference-budget counts.
struct Timing {
    uint64_t count{};
    uint64_t min_ns{};
    uint64_t p50_ns{};
    uint64_t p95_ns{};
    uint64_t p99_ns{};
    uint64_t max_ns{};
    double mean_ns{};
    uint64_t total_ns{};
    uint64_t over_budget_count{};
    /// Signals a cumulative sum above uint64_t, represented by its maximum.
    bool total_ns_saturated{};
};

/// Summarizes samples without changing their order in the caller's storage.
///
/// @param samples elapsed nanoseconds of successful measured ticks
/// @return statistics and a saturating sum, or all zeroes when there are no samples
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
    /// The old preset number whose feature set matches, until R09.
    uint32_t enhanced_version{};
    /// The Enhanced Mode revision the match played. Zero is Off.
    uint32_t enhanced_revision{};
    /// Feature names in id order, snake_case, empty when the mode is off.
    std::vector<std::string> enhanced_features{};
    std::string simulation_hash{};
    std::string world_digest{};
    std::string renderer{"none"};
    Timing ticks{};
    std::optional<Timing> thread_cpu_ticks{};
    std::string thread_cpu_clock{"unsupported"};
    std::string thread_cpu_status{"unsupported"};
    std::vector<PhaseTiming> simulation_phases{};
    /// The stage scenario's results; none for the other scenarios.
    std::optional<StagedBattle> staged{};
};

/// Writes a UTF-8 JSON object with stable keys and locale-independent numbers.
///
/// @param report benchmark facts with UTF-8 text fields
/// @return the schema-versioned JSON object, ending with a newline
[[nodiscard]] std::string report_json(const Report& report);

} // namespace oa::app::battle_benchmark
