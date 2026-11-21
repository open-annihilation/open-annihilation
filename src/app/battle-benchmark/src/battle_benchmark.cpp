// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/battle_benchmark.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <locale>
#include <limits>
#include <numeric>
#include <sstream>
#include <string_view>
#include <tuple>
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
constexpr int32_t map_cell_pixels = 16;
constexpr int32_t half_map_cell_pixels = map_cell_pixels / 2;

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

/// Writes a timing object, including null for a non-finite mean.
void json_timing(std::ostream& out, const Timing& timing) {
    out << "{\"count\": " << timing.count << ", \"min\": " << timing.min_ns
        << ", \"p50\": " << timing.p50_ns << ", \"p95\": " << timing.p95_ns
        << ", \"p99\": " << timing.p99_ns << ", \"max\": " << timing.max_ns << ", \"mean\": ";
    if (std::isfinite(timing.mean_ns))
        out << timing.mean_ns;
    else
        out << "null";
    out << ", \"total\": " << timing.total_ns << ", \"over_budget\": " << timing.over_budget_count
        << ", \"total_saturated\": " << (timing.total_ns_saturated ? "true" : "false") << '}';
}

/// Measures whole-pixel Manhattan distance without overflowing signed coordinates.
uint64_t distance(Point a, Point b) {
    const auto x = static_cast<int64_t>(a.x) - b.x;
    const auto z = static_cast<int64_t>(a.z) - b.z;
    return static_cast<uint64_t>(x < 0 ? -x : x) + static_cast<uint64_t>(z < 0 ? -z : z);
}

/// Writes progress for one army, including its initial-distance thirds.
void json_progress(std::ostream& out, const ProgressSummary& progress) {
    out << "{\"goal_units\": " << progress.goal_units << ", \"live_units\": " << progress.live_units
        << ", \"moved_units\": " << progress.moved_units
        << ", \"progressed_units\": " << progress.progressed_units
        << ", \"arrived_units\": " << progress.arrived_units
        << ", \"stationary_unit_ticks\": " << progress.stationary_unit_ticks
        << ", \"longest_stall_ticks\": " << progress.longest_stall_ticks
        << ", \"initial_goal_distance_pixels\": " << progress.initial_goal_distance_pixels
        << ", \"final_goal_distance_pixels\": " << progress.final_goal_distance_pixels
        << ", \"distance_thirds\": [";
    bool first = true;
    for (const auto& third : progress.distance_thirds) {
        if (!first)
            out << ", ";
        out << "{\"goal_units\": " << third.goal_units
            << ", \"progressed_units\": " << third.progressed_units
            << ", \"arrived_units\": " << third.arrived_units << '}';
        first = false;
    }
    out << "]}";
}

/// Writes a finite number, or null.
void json_number(std::ostream& out, double value) {
    if (std::isfinite(value))
        out << value;
    else
        out << "null";
}

/// Writes a staged battle's lines, players and ownership audits.
void json_staged(std::ostream& out, const StagedBattle& staged) {
    out << "{\n    \"stage\": ";
    json_string(out, staged.stage);
    out << ",\n    \"window_ticks\": " << staged.window_ticks;
    out << ",\n    \"lines\": [";
    for (std::size_t index = 0; index < staged.lines.size(); ++index) {
        const auto& line = staged.lines[index];
        out << (index == 0 ? "" : ", ") << "{\"name\": ";
        json_string(out, line.name);
        out << ", \"from\": [" << line.from.x << ", " << line.from.z << "], \"to\": [" << line.to.x
            << ", " << line.to.z << "]}";
    }
    out << "],\n    \"players\": [";
    for (std::size_t index = 0; index < staged.players.size(); ++index) {
        const auto& player = staged.players[index];
        const auto& targets = player.strike_targets;
        out << (index == 0 ? "\n      " : ",\n      ") << "{\"player\": " << unsigned{player.player}
            << ", \"computer\": " << (player.computer ? "true" : "false")
            << ", \"tracked_units\": " << player.tracked_units
            << ", \"live_tracked_units\": " << player.live_tracked_units
            << ", \"births\": " << player.births << ", \"deaths\": " << player.deaths
            << ", \"goal_units\": " << player.goal_units << ", \"mean_progress_pixels\": ";
        json_number(out, player.mean_progress_pixels);
        out << ", \"advanced_units\": " << player.advanced_units
            << ", \"window_live_unit_ticks\": " << player.window_live_unit_ticks
            << ", \"window_stationary_unit_ticks\": " << player.window_stationary_unit_ticks
            << ", \"stationary_throughout\": " << player.stationary_throughout
            << ", \"crossers\": [";
        for (std::size_t line = 0; line < player.crossers.size(); ++line)
            out << (line == 0 ? "" : ", ") << player.crossers[line];
        out << "], \"strike_targets\": {\"samples\": " << targets.samples
            << ", \"attackers\": " << targets.attackers
            << ", \"distinct_targets\": " << targets.distinct_targets << ", \"largest_share\": ";
        json_number(out, targets.largest_share);
        out << "}}";
    }
    const auto& audit = staged.audit;
    out << (staged.players.empty() ? "]" : "\n    ]")
        << ",\n    \"ownership_audit\": {\"audits\": " << audit.audits
        << ", \"audited_units\": " << audit.audited_units
        << ", \"outside_units\": " << audit.outside_units
        << ", \"ownership_errors\": " << audit.ownership_errors
        << ", \"units_with_errors\": " << audit.units_with_errors
        << ", \"overlap_cells\": " << audit.overlap_cells << ", \"first_error_tick\": ";
    if (audit.first_error_tick)
        out << *audit.first_error_tick;
    else
        out << "null";
    out << "}\n  }";
}

} // namespace

std::size_t maximum_layout_units(
    uint32_t width, uint32_t height, uint32_t spacing, uint32_t margin, uint32_t gap
) {
    const auto grid = grid_geometry(width, height, spacing, margin, gap);
    if (!grid)
        return 0;
    return static_cast<std::size_t>(
        std::min<uint64_t>(maximum_layout_candidates, grid->columns * grid->rows)
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
    if (per_side > maximum_layout_candidates)
        return {{}, "placement exceeds the supported candidate capacity"};
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

LayoutResult plan_group_arrivals(uint32_t width, uint32_t height, std::size_t per_side) {
    if (per_side > maximum_layout_candidates)
        return {{}, "arrival placement exceeds the supported candidate capacity"};
    const auto available = maximum_layout_units(width, height, arrival_spacing);
    if (per_side > available)
        return {{}, "map cannot hold the arrival candidates"};
    if (per_side == 0)
        return plan_layout(width, height, 0, arrival_spacing);
    auto result = plan_layout(width, height, available, arrival_spacing);
    if (!result.layout)
        return result;
    std::swap(result.layout->armies[0], result.layout->armies[1]);
    const auto middle_region = static_cast<int32_t>(height / 2 / shared_goal_region_pixels);
    for (uint8_t side = 0; side < result.layout->armies.size(); ++side) {
        auto& points = result.layout->armies[side];
        const auto region_columns =
            (width + shared_goal_region_pixels - 1) / shared_goal_region_pixels;
        const auto region_rows =
            (height + shared_goal_region_pixels - 1) / shared_goal_region_pixels;
        std::vector<std::size_t> region_population(region_columns * region_rows);
        const auto region_index = [&](Point point) {
            return static_cast<std::size_t>(point.z / shared_goal_region_pixels) * region_columns +
                   point.x / shared_goal_region_pixels;
        };
        for (const auto point : points)
            ++region_population[region_index(point)];
        constexpr auto region_cells = shared_goal_region_pixels / arrival_spacing;
        constexpr auto full_region_population = region_cells * region_cells;
        const auto key = [&](Point point) {
            const auto region_x = point.x / static_cast<int32_t>(shared_goal_region_pixels);
            const auto region_z = point.z / static_cast<int32_t>(shared_goal_region_pixels);
            const auto from_middle = region_z - middle_region;
            return std::tuple{
                region_population[region_index(point)] == full_region_population ? 0 : 1,
                side == 0 ? region_x : -region_x,
                from_middle < 0 ? -from_middle : from_middle,
                region_z,
                side == 0 ? point.x : -point.x,
                point.z
            };
        };
        std::stable_sort(points.begin(), points.end(), [&](Point a, Point b) {
            return key(a) < key(b);
        });
        points.resize(per_side);
    }
    return result;
}

LayoutResult select_arrivals(
    uint32_t width,
    uint32_t height,
    const Layout& candidates,
    const std::array<std::span<const Footprint>, 2>& footprints,
    ArrivalAccess access
) {
    if (width == 0 || height == 0 || width > maximum_map_extent || height > maximum_map_extent)
        return {{}, "arrival map dimensions are invalid"};
    const auto columns = (static_cast<std::size_t>(width) + map_cell_pixels - 1) / map_cell_pixels;
    const auto rows = (static_cast<std::size_t>(height) + map_cell_pixels - 1) / map_cell_pixels;
    const auto cells = columns * rows;
    uint64_t needed_cells = 0;
    for (std::size_t side = 0; side < footprints.size(); ++side) {
        const auto army = footprints[side];
        if (candidates.armies[side].size() > maximum_layout_candidates)
            return {{}, "arrival candidates exceed the supported capacity"};
        if (army.size() > maximum_units_per_side)
            return {{}, "arrival army exceeds the supported population"};
        for (const auto footprint : army) {
            if (footprint.x == 0 || footprint.z == 0 || footprint.x > columns || footprint.z > rows)
                return {{}, "arrival footprint is invalid"};
            needed_cells += static_cast<uint64_t>(footprint.x) * footprint.z;
        }
    }
    if (needed_cells > cells)
        return {{}, "map cannot hold the arrival footprints"};
    std::vector<uint8_t> reserved(cells);
    Layout layout;
    for (uint8_t side = 0; side < layout.armies.size(); ++side) {
        auto& selected = layout.armies[side];
        selected.reserve(footprints[side].size());
        for (const auto point : candidates.armies[side]) {
            if (selected.size() == footprints[side].size())
                break;
            const auto footprint = footprints[side][selected.size()];
            const auto left_pixels = static_cast<int64_t>(point.x) + half_map_cell_pixels -
                                     static_cast<int64_t>(footprint.x) * half_map_cell_pixels;
            const auto top_pixels = static_cast<int64_t>(point.z) + half_map_cell_pixels -
                                    static_cast<int64_t>(footprint.z) * half_map_cell_pixels;
            if (point.x < 0 || point.z < 0 || static_cast<uint32_t>(point.x) >= width ||
                static_cast<uint32_t>(point.z) >= height || left_pixels < 0 || top_pixels < 0)
                continue;
            const auto left = static_cast<std::size_t>(left_pixels / map_cell_pixels);
            const auto top = static_cast<std::size_t>(top_pixels / map_cell_pixels);
            if ((left + footprint.x) * map_cell_pixels > width ||
                (top + footprint.z) * map_cell_pixels > height)
                continue;
            bool clear = true;
            for (std::size_t z = top; clear && z < top + footprint.z; ++z)
                for (std::size_t x = left; x < left + footprint.x; ++x)
                    if (reserved[z * columns + x] != 0) {
                        clear = false;
                        break;
                    }
            if (!clear || (access.can_stand != nullptr &&
                           !access.can_stand(access.context, side, selected.size(), point)))
                continue;
            for (std::size_t z = top; z < top + footprint.z; ++z)
                for (std::size_t x = left; x < left + footprint.x; ++x)
                    reserved[z * columns + x] = 1;
            selected.push_back(point);
        }
        if (selected.size() != footprints[side].size())
            return {
                {},
                "map has too few distinct clear arrival footprints for side " + std::to_string(side)
            };
    }
    return {std::move(layout), {}};
}

ProgressState
begin_progress(Point start, Point goal, uint32_t progress_distance, uint32_t arrival_distance) {
    ProgressState state;
    state.start = state.previous = start;
    state.goal = goal;
    state.initial_distance = state.best_distance = distance(start, goal);
    state.progress_distance = std::max<uint32_t>(1, progress_distance);
    state.arrival_distance = std::max<uint32_t>(1, arrival_distance);
    state.arrived = state.initial_distance <= state.arrival_distance;
    return state;
}

void sample_progress(ProgressState& state, Point position, bool live) {
    state.live = live;
    if (!live) {
        state.stationary_run = 0;
        return;
    }
    const auto remaining = distance(position, state.goal);
    state.best_distance = std::min(state.best_distance, remaining);
    state.arrived = remaining <= state.arrival_distance;
    state.moved |= distance(position, state.start) >= state.progress_distance;
    if (!state.arrived && position == state.previous) {
        ++state.stationary_ticks;
        ++state.stationary_run;
        state.longest_stall = std::max(state.longest_stall, state.stationary_run);
    } else
        state.stationary_run = 0;
    state.previous = position;
}

ProgressSummary summarize_progress(std::span<const ProgressState> states) {
    ProgressSummary summary;
    std::vector<std::size_t> order(states.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return states[a].initial_distance < states[b].initial_distance;
    });
    for (std::size_t rank = 0; rank < order.size(); ++rank) {
        const auto& state = states[order[rank]];
        const bool progressed =
            state.best_distance < state.initial_distance &&
            state.initial_distance - state.best_distance >= state.progress_distance;
        const bool arrived = state.live && state.arrived;
        ++summary.goal_units;
        summary.live_units += state.live;
        summary.moved_units += state.moved;
        summary.progressed_units += progressed;
        summary.arrived_units += arrived;
        summary.stationary_unit_ticks += state.stationary_ticks;
        summary.longest_stall_ticks = std::max(summary.longest_stall_ticks, state.longest_stall);
        summary.initial_goal_distance_pixels += state.initial_distance;
        summary.final_goal_distance_pixels += distance(state.previous, state.goal);
        auto& third = summary.distance_thirds[rank * summary.distance_thirds.size() / order.size()];
        ++third.goal_units;
        third.progressed_units += progressed;
        third.arrived_units += arrived;
    }
    return summary;
}

std::optional<CrossingLine> parse_crossing_line(std::string_view text) {
    constexpr std::size_t longest_name = 32;
    const auto equals = text.find('=');
    if (equals == std::string_view::npos || equals == 0 || equals > longest_name)
        return {};
    CrossingLine line;
    line.name = std::string(text.substr(0, equals));
    for (const auto letter : line.name)
        if (!(std::isalnum(static_cast<unsigned char>(letter)) != 0 || letter == '-' ||
              letter == '_'))
            return {};
    std::array<int32_t, 4> values{};
    auto rest = text.substr(equals + 1);
    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto comma = rest.find(',');
        if ((comma == std::string_view::npos) != (index + 1 == values.size()))
            return {};
        const auto word = rest.substr(0, comma);
        if (word.empty() || word.size() > 5)
            return {};
        int32_t value = 0;
        for (const auto digit : word) {
            if (digit < '0' || digit > '9')
                return {};
            value = value * 10 + (digit - '0');
        }
        if (value > static_cast<int32_t>(maximum_map_extent))
            return {};
        values[index] = value;
        if (comma != std::string_view::npos)
            rest = rest.substr(comma + 1);
    }
    line.from = {values[0], values[1]};
    line.to = {values[2], values[3]};
    if (line.from == line.to)
        return {};
    return line;
}

namespace {

/// Returns the sign of the cross product of (b - a) and (point - a).
int orientation(Point a, Point b, Point point) noexcept {
    const auto cross = (static_cast<int64_t>(b.x) - a.x) * (static_cast<int64_t>(point.z) - a.z) -
                       (static_cast<int64_t>(b.z) - a.z) * (static_cast<int64_t>(point.x) - a.x);
    return cross > 0 ? 1 : cross < 0 ? -1 : 0;
}

} // namespace

int line_side(const CrossingLine& line, Point point) noexcept {
    return orientation(line.from, line.to, point);
}

bool crosses_line(const CrossingLine& line, Point before, Point after) noexcept {
    const auto start = line_side(line, before);
    const auto end = line_side(line, after);
    if (start == 0 || end == 0 || start == end)
        return false;
    // The step reaches across the line; it crosses the segment unless both
    // of the segment's ends lie strictly on one side of the step.
    const auto first = orientation(before, after, line.from);
    const auto second = orientation(before, after, line.to);
    return first == 0 || second == 0 || first != second;
}

StagedUnit begin_staged_unit(
    uint8_t player, Point start, std::optional<Point> goal, std::span<const CrossingLine> lines
) {
    StagedUnit unit;
    unit.player = player;
    unit.start = unit.position = start;
    unit.goal = goal;
    for (std::size_t index = 0; index < lines.size() && index < maximum_crossing_lines; ++index)
        unit.start_side[index] = static_cast<int8_t>(line_side(lines[index], start));
    return unit;
}

void sample_staged_unit(
    StagedUnit& unit,
    std::span<const CrossingLine> lines,
    Point position,
    bool moved,
    bool live,
    bool in_window
) {
    if (!unit.live)
        return;
    if (!live) {
        unit.live = false;
        return;
    }
    for (std::size_t index = 0; index < lines.size() && index < maximum_crossing_lines; ++index) {
        const auto bit = static_cast<uint8_t>(1U << index);
        if ((unit.crossed & bit) != 0 || unit.start_side[index] == 0)
            continue;
        if (crosses_line(lines[index], unit.position, position) &&
            line_side(lines[index], position) == -unit.start_side[index])
            unit.crossed = static_cast<uint8_t>(unit.crossed | bit);
    }
    if (in_window) {
        ++unit.window_live_ticks;
        if (!moved)
            ++unit.window_stationary_ticks;
    }
    unit.position = position;
}

void add_target_sample(TargetSpread& spread, std::span<uint16_t> targets) {
    if (targets.empty())
        return;
    std::sort(targets.begin(), targets.end());
    uint64_t distinct = 0;
    uint64_t largest = 0;
    for (std::size_t index = 0; index < targets.size();) {
        auto next = index;
        while (next < targets.size() && targets[next] == targets[index])
            ++next;
        ++distinct;
        largest = std::max<uint64_t>(largest, next - index);
        index = next;
    }
    ++spread.samples;
    spread.attackers += targets.size();
    spread.distinct_targets += distinct;
    spread.largest_share += static_cast<double>(largest) / static_cast<double>(targets.size());
}

void audit_ownership(
    OwnershipAudit& audit,
    uint32_t tick,
    uint32_t width,
    uint32_t height,
    std::span<const AuditUnit> units,
    const void* context,
    OccupantReader occupant,
    std::vector<uint8_t>& scratch
) {
    ++audit.audits;
    scratch.assign(static_cast<std::size_t>(width) * height, 0);
    bool error_seen = false;
    for (const auto& unit : units) {
        if (unit.cell_x < 0 || unit.cell_z < 0 || unit.footprint_x < 0 || unit.footprint_z < 0 ||
            static_cast<int64_t>(unit.cell_x) + unit.footprint_x >= width ||
            static_cast<int64_t>(unit.cell_z) + unit.footprint_z >= height) {
            ++audit.outside_units;
            continue;
        }
        ++audit.audited_units;
        bool unit_error = false;
        for (int32_t row = 0; row < unit.footprint_z; ++row)
            for (int32_t column = 0; column < unit.footprint_x; ++column) {
                const auto cell = static_cast<std::size_t>(unit.cell_z + row) * width +
                                  static_cast<std::size_t>(unit.cell_x + column);
                if (occupant(context, cell, unit.air) != unit.id) {
                    ++audit.ownership_errors;
                    unit_error = true;
                }
                if (unit.air)
                    continue;
                if (scratch[cell] == 1)
                    ++audit.overlap_cells;
                if (scratch[cell] < 2)
                    ++scratch[cell];
            }
        audit.units_with_errors += unit_error;
        error_seen |= unit_error;
    }
    if (audit.overlap_cells != 0)
        error_seen = true;
    if (error_seen && !audit.first_error_tick)
        audit.first_error_tick = tick;
}

std::vector<StagedPlayer>
summarize_staged(std::span<const StagedUnit> units, std::size_t line_count, uint64_t window_ticks) {
    std::vector<StagedPlayer> players;
    std::vector<double> progress_sums;
    const auto entry = [&](uint8_t player) -> std::size_t {
        for (std::size_t index = 0; index < players.size(); ++index)
            if (players[index].player == player)
                return index;
        auto position = players.begin();
        while (position != players.end() && position->player < player)
            ++position;
        const auto index = static_cast<std::size_t>(position - players.begin());
        StagedPlayer added;
        added.player = player;
        added.crossers.assign(std::min(line_count, maximum_crossing_lines), 0);
        players.insert(position, added);
        progress_sums.insert(progress_sums.begin() + static_cast<std::ptrdiff_t>(index), 0.0);
        return index;
    };
    for (const auto& unit : units) {
        const auto index = entry(unit.player);
        auto& summary = players[index];
        ++summary.tracked_units;
        summary.live_tracked_units += unit.live;
        if (unit.goal) {
            const auto before = std::hypot(
                static_cast<double>(unit.start.x) - unit.goal->x,
                static_cast<double>(unit.start.z) - unit.goal->z
            );
            const auto after = std::hypot(
                static_cast<double>(unit.position.x) - unit.goal->x,
                static_cast<double>(unit.position.z) - unit.goal->z
            );
            ++summary.goal_units;
            progress_sums[index] += before - after;
            summary.advanced_units += before - after >= advance_distance_pixels;
        }
        summary.window_live_unit_ticks += unit.window_live_ticks;
        summary.window_stationary_unit_ticks += unit.window_stationary_ticks;
        summary.stationary_throughout += window_ticks != 0 &&
                                         unit.window_live_ticks == window_ticks &&
                                         unit.window_stationary_ticks == window_ticks;
        for (std::size_t line = 0; line < summary.crossers.size(); ++line)
            summary.crossers[line] += (unit.crossed >> line) & 1U;
    }
    for (std::size_t index = 0; index < players.size(); ++index)
        if (players[index].goal_units != 0)
            players[index].mean_progress_pixels =
                progress_sums[index] / static_cast<double>(players[index].goal_units);
    return players;
}

Timing summarize(std::span<const uint64_t> samples) {
    if (samples.empty())
        return {};
    std::vector<uint64_t> ordered(samples.begin(), samples.end());
    std::sort(ordered.begin(), ordered.end());
    double total = 0;
    uint64_t total_ns = 0, over_budget = 0;
    bool saturated = false;
    for (const auto sample : ordered) {
        total += static_cast<double>(sample);
        over_budget += sample > tick_budget_ns;
        if (sample > std::numeric_limits<uint64_t>::max() - total_ns) {
            total_ns = std::numeric_limits<uint64_t>::max();
            saturated = true;
        } else {
            total_ns += sample;
        }
    }
    return {
        static_cast<uint64_t>(ordered.size()),
        ordered.front(),
        percentile(ordered, median_percentile),
        percentile(ordered, tail_percentile),
        percentile(ordered, slow_percentile),
        ordered.back(),
        total / static_cast<double>(ordered.size()),
        total_ns,
        over_budget,
        saturated
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
    number("moved_unit_ticks", report.moved_unit_ticks);
    number("damaged_unit_ticks", report.damaged_unit_ticks);
    number("projectile_high_water", report.projectile_high_water);
    number("projectile_capacity", report.projectile_capacity);
    number("projectile_allocation_failures", report.projectile_allocation_failures);
    number("projection_preparations", report.projection_preparations);
    number("projected_slots", report.projected_slots);
    number("bucket_queries", report.bucket_queries);
    number("bucket_candidates", report.bucket_candidates);
    number("shared_route_requests", report.shared_route_requests);
    number("shared_route_hits", report.shared_route_hits);
    number("shared_route_fields", report.shared_route_fields);
    number("shared_route_cells", report.shared_route_cells);
    number("shared_route_waits", report.shared_route_waits);
    number("shared_route_invalidations", report.shared_route_invalidations);
    number("full_route_searches", report.full_route_searches);
    number("local_flow_probes", report.local_flow_probes);
    number("local_flow_moving_blockers", report.local_flow_moving_blockers);
    number("local_flow_steering_adjustments", report.local_flow_steering_adjustments);
    number("local_flow_yielded_ticks", report.local_flow_yielded_ticks);
    number("local_flow_blocked_ticks", report.local_flow_blocked_ticks);
    number("local_flow_deferred_replans", report.local_flow_deferred_replans);
    number("target_queries", report.target_queries);
    number("target_candidates", report.target_candidates);
    number("target_full_scans", report.target_full_scans);
    text("routing_metrics_scope", report.routing_metrics_scope);
    out << ",\n  \"goal_metrics_available\": "
        << (report.goal_metrics_available ? "true" : "false");
    out << ",\n  \"goal_progress\": [";
    json_progress(out, report.goal_progress[0]);
    out << ", ";
    json_progress(out, report.goal_progress[1]);
    out << ']';
    out << ",\n  \"activity_observed\": " << (report.activity_observed ? "true" : "false");
    text("activity_sampling_scope", report.activity_sampling_scope);
    out << ",\n  \"enhanced_mode\": " << (report.enhanced_mode ? "true" : "false");
    number("enhanced_version", report.enhanced_version);
    text("simulation_hash", report.simulation_hash);
    text("world_digest", report.world_digest);
    text("renderer", report.renderer);
    number("tick_budget_ns", tick_budget_ns);
    text("thread_cpu_clock", report.thread_cpu_clock);
    text("thread_cpu_status", report.thread_cpu_status);
    out << ",\n  \"thread_cpu_available\": "
        << (report.thread_cpu_ticks.has_value() ? "true" : "false");
    out << ",\n  \"tick_thread_cpu_ns\": ";
    if (report.thread_cpu_ticks)
        json_timing(out, *report.thread_cpu_ticks);
    else
        out << "null";
    out << ",\n  \"simulation_phase_ns\": {";
    bool first = true;
    for (const auto& phase : report.simulation_phases) {
        if (!first)
            out << ',';
        out << "\n    ";
        json_string(out, phase.name);
        out << ": ";
        json_timing(out, phase.ticks);
        first = false;
    }
    out << (first ? "}" : "\n  }");
    out << ",\n  \"staged_battle\": ";
    if (report.staged)
        json_staged(out, *report.staged);
    else
        out << "null";
    out << ",\n  \"tick_ns\": ";
    json_timing(out, report.ticks);
    out << "\n}\n";
    return out.str();
}

} // namespace oa::app::battle_benchmark
