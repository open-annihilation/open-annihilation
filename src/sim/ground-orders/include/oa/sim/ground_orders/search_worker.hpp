// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/ground_orders/movement_map.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/ground_orders/search_heap.hpp"
#include "oa/sim/ground_orders/search_trace.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace oa::sim::ground_orders {

// Step direction tables. Index is the predecessor-direction byte.
inline constexpr std::array<int8_t, 8> search_direction_x{0, -1, -1, -1, 0, 1, 1, 1};
inline constexpr std::array<int8_t, 8> search_direction_z{-1, -1, 0, 1, 1, 1, 0, -1};

// The path search's turn penalties, indexed by relative turn & 7.
inline constexpr std::array<uint8_t, 8> search_turn_penalty{0, 40, 60, 80, 100, 80, 60, 40};
inline constexpr uint8_t search_cardinal_cost = 0x10; // even (cardinal) directions
inline constexpr uint8_t search_diagonal_cost = 0x16; // odd (diagonal) directions
inline constexpr int32_t search_early_turn_penalty = 0x4b;
inline constexpr int16_t search_early_run_limit = 5;
inline constexpr int16_t search_difficult_extra = 0x1e;
inline constexpr int16_t search_start_run = 100;
inline constexpr int32_t search_slice_expansions = 100;    // job scan inner budget
inline constexpr int32_t search_initial_fan = 4;           // fan when a job starts
inline constexpr int32_t search_continue_fan = 2;          // fan after the first pop
inline constexpr int32_t search_heuristic_scale = 0x18000; // initial base heuristic weight
inline constexpr int32_t search_tick_credit = 0x535;       // initial per-tick node credit
inline constexpr int32_t search_scale_period = 0x96;       // heuristic refresh period
inline constexpr uint8_t search_goal_bit = 4;
inline constexpr uint8_t search_seed_bit = 8;
inline constexpr uint16_t search_heading_bias = 0x1000;
inline constexpr uint32_t search_heading_shift = 13;

enum class SearchCellVisit : uint8_t { unvisited = 0, open = 1, closed = 2, blocked = 3 };

enum class SearchAdvance : uint8_t { in_progress, succeeded, failed };

// The path search's cell grid. count is (width * height + 7) & ~7, the
// rounded cell count, not the raw product.
struct SearchCellGrid {
    std::vector<SearchMapCell> cells;
    int32_t width{};
    int32_t height{};
    uint32_t count{};

    /// Drops the previous cells and allocates the rounded count; none when it is zero.
    ///
    /// @param map_width map width in cells
    /// @param map_height map height in cells
    void allocate(int32_t map_width, int32_t map_height);
};

// Job inputs that are not on the unit record (cell, footprint and heading are
// read from the record itself).
struct SearchBegin {
    // The searching unit's GroundRuntime::occupancy_changed_tick, read as the
    // current tick while the map is prepared. Null reads as zero.
    uint32_t* unit_changed_tick{};
    // Every other unit with a movement object.
    std::span<const OccupancyChange> occupancy{};
    uint32_t current_tick{};
    // The job scan's player round-robin index (SearchPlayerJobState::round_robin),
    // reused as the sight-test shift. Isolated jobs supply the bit explicitly.
    uint8_t mask_bit{};
    int32_t heuristic_scale{search_heuristic_scale};
    // Game.map_width / map_height. Zero copies the movement-map dimensions.
    uint32_t world_width{};
    uint32_t world_height{};
    // Maximum charged seed-walk steps; zero retains the map-sized legacy bound.
    uint32_t seed_step_limit{};
};

/// Tests whether a wall-follow cell crossed the axis-aligned path from the start to the nearest goal cell.
///
/// Coordinates are mirrored so the goal lies at nonnegative offsets; the cell
/// crosses on the start row between the start and the goal column, or on the
/// goal column between the start and the goal row.
///
/// @param start_x wall-follow start column
/// @param start_z wall-follow start row
/// @param goal_x nearest goal column
/// @param goal_z nearest goal row
/// @param cell_x tested cell column
/// @param cell_z tested cell row
/// @return true when the cell lies on that path
[[nodiscard]] bool crossed_start_goal_axis(
    int32_t start_x, int32_t start_z, int32_t goal_x, int32_t goal_z, int32_t cell_x, int32_t cell_z
) noexcept;

/// Divides the tick credit by the live player count (Game.player_count).
///
/// @param tick_credit search budget per tick (SearchPlayerJobState::tick_credit)
/// @param player_count live players
/// @return each player's share, 0 without players
[[nodiscard]] int32_t search_player_credit(int32_t tick_credit, uint16_t player_count) noexcept;

/// Returns a player's heuristic scale after the refresh counter wraps at 150.
///
/// Players whose units searched rarely get a greedier (larger) heuristic: six
/// times the base below one pick per unit, three times below two.
///
/// @param usage_count jobs the player's units started since the last refresh
/// @param units_per_player unit slots per player
/// @param base base heuristic scale (16.16)
/// @return the scale; the base for zero units per player, which leaves no picks per unit
[[nodiscard]] int32_t
search_rescaled_heuristic(int32_t usage_count, uint16_t units_per_player, int32_t base) noexcept;

inline constexpr std::size_t search_player_slots = 10; // player slots the job scan walks

// Controller fields the job scan keeps across ticks. Cursors are indexes into
// each player's inclusive first_unit..last_unit span. construct_search stores
// the first unit and zeros the credits; player_heuristic starts at the base.
// job_picks, player_heuristic and refresh_tick persist across scans so
// successive scans stay deterministic.
struct SearchPlayerJobState {
    uint8_t round_robin{}; // the player whose credit the scan spends next
    std::array<uint32_t, search_player_slots> unit_cursor{};
    std::array<int32_t, search_player_slots> credit{};
    std::array<int32_t, search_player_slots> job_picks{};
    std::array<int32_t, search_player_slots> player_heuristic{};
    int32_t refresh_tick{};                         // not reset by construct_search
    int32_t tick_credit{search_tick_credit};        // search budget per tick, all players
    int32_t base_heuristic{search_heuristic_scale}; // heuristic scale before rescaling
    int32_t slice_cost{};                           // cost of the current round-robin step
    int32_t active_heuristic{};                     // heuristic scale of the running job

    /// Starts every player's heuristic at the base scale.
    SearchPlayerJobState();
};

/// Constructs the search controller state.
///
/// Empties the open set (free head -1), sizes the cell grid from the map, sets
/// the touched-map bits then clears them with the cells, and resets the credits
/// and the ten cursors to the first unit (index 0). Leaves job_picks and
/// refresh_tick untouched. A zero cell count, or one of 2^31 - 255 or more,
/// keeps no touched-map words.
///
/// @param[out] heap open-set heap to reset
/// @param[out] grid cell grid to allocate
/// @param[out] touched touched-map words
/// @param[in,out] jobs player job state
/// @param world_width map width in cells
/// @param world_height map height in cells
void construct_search(
    SearchHeap& heap,
    SearchCellGrid& grid,
    std::vector<uint32_t>& touched,
    SearchPlayerJobState& jobs,
    int32_t world_width,
    int32_t world_height
);

// Asynchronous expander that produces routes for one job at a time. Job
// selection and slicing belong to scan_player_jobs.
class SearchWorker {
  public:

    /// Starts a job.
    ///
    /// Prepares the movement map, marks goal cells, seeds the wall-follow threshold
    /// and pushes the start node. Cells the job's player has not seen classify as
    /// passable.
    ///
    /// @param record unit, navigator, goal and movement map
    /// @param begin occupancy inputs, sight bit, heuristic scale and map size
    /// @param sight_grid Game.sight_grid, one player bit per 32-unit cell
    /// @return in_progress, or failed when the job ends at once (start inside the goal, off the map, or unreachable);
    ///         failed too, with nothing but budget() changed (to zero), for a record with a missing field
    ///         or a map whose cell count does not fit in memory
    [[nodiscard]] SearchAdvance
    begin(SearchRecord record, const SearchBegin& begin, std::span<const uint16_t> sight_grid);
    /// Pops the best open node and expands its neighbours within the fan.
    ///
    /// @return 1 when the node is a goal cell (the finish is recorded), else 0
    [[nodiscard]] uint32_t expand_best();
    /// Runs up to 100 expansions of an isolated job; the scheduler slices itself.
    ///
    /// @return succeeded with the route published, failed when the open set is exhausted, else in_progress
    [[nodiscard]] SearchAdvance advance_slice();
    /// Returns a cell's movement-map class after the sight test.
    ///
    /// @param x cell column
    /// @param z cell row
    /// @return 0 off the map, 2 when the job's player has not seen the cell, else the movement-map class
    [[nodiscard]] uint32_t classify(int32_t x, int32_t z) const;
    /// Returns the goal cost scaled by the job's heuristic weight.
    ///
    /// @param x cell column
    /// @param z cell row
    /// @return goal_cost * scale >> 16
    [[nodiscard]] int32_t heuristic(int32_t x, int32_t z) const;

    /// Returns whether no unpopped open node remains.
    [[nodiscard]] bool exhausted() const noexcept { return heap_.exhausted(); }

    /// Rebuilds the route from the finish cell and stores it in route().
    void publish_route();

    /// Returns the last published route; empty after a failure.
    [[nodiscard]] const std::vector<RoutePoint>& route() const noexcept { return route_; }

    /// Returns the order events the job raised.
    [[nodiscard]] uint32_t order_flags() const noexcept { return order_flags_; }

    /// Returns the relative turns expanded either side of straight ahead.
    [[nodiscard]] int32_t fan() const noexcept { return fan_; }

    /// Sets the relative turns expanded either side of straight ahead.
    void set_fan(int32_t fan) noexcept { fan_ = fan; }

    /// Returns the wall-follow cost threshold for goal-bit marking.
    [[nodiscard]] int32_t threshold() const noexcept { return threshold_; }

    /// Returns the expansions or wall-follow steps spent so far.
    ///
    /// After begin it holds the wall-follow walk's steps, which the job scan
    /// charges with the job's start; advance_slice counts its expansions.
    [[nodiscard]] int32_t budget() const noexcept { return budget_; }

    /// Returns the job's start cell.
    [[nodiscard]] std::array<int16_t, 2> start_cell() const noexcept { return start_; }

    /// Returns the goal cell the job reached.
    [[nodiscard]] std::array<int16_t, 2> finish_cell() const noexcept { return finish_; }

    /// Returns the goal cell nearest the start.
    [[nodiscard]] std::array<int32_t, 2> nearest_goal() const noexcept { return nearest_; }

    /// Returns the open-set heap.
    [[nodiscard]] const SearchHeap& heap() const noexcept { return heap_; }

    /// Returns a search-map cell.
    ///
    /// @param x cell column
    /// @param z cell row
    /// @return the cell; an empty cell outside the allocated grid
    [[nodiscard]] SearchMapCell cell(uint32_t x, uint32_t z) const noexcept;

    /// Returns the search map width in cells.
    [[nodiscard]] uint32_t width() const noexcept { return width_; }

    /// Returns the search map height in cells.
    [[nodiscard]] uint32_t height() const noexcept { return height_; }

  private:

    /// Installs the turn penalty and the cardinal/diagonal move costs.
    void install_turn_tables();
    /// Marks the eight-cell group of a cell as touched.
    ///
    /// @param index row-major cell index
    void mark_dirty(uint32_t index);
    /// Clears the flags of every touched cell group and the touched bits.
    void clear_dirty();
    /// Marks a cell as a goal cell; cells off the map are ignored.
    ///
    /// @param x cell column
    /// @param z cell row
    void mark_goal(int32_t x, int32_t z);
    /// Opens or improves one neighbour of an expanded node.
    ///
    /// An unvisited cell is closed as blocked when it classifies below 1 (unless on
    /// the wall-follow seed), otherwise opened with its move cost, turn penalty,
    /// difficult-terrain extra and early-turn penalty, and marked as a goal cell
    /// when its heuristic is within the threshold. An open cell whose new cost is
    /// lower takes the new predecessor and moves up the heap.
    ///
    /// @param parent the expanded node's payload
    /// @param parent_direction the direction the search reached the parent from
    /// @param relative_turn turn from that direction, 0..7
    void expand_neighbor(
        const SearchHeap::Payload& parent, uint8_t parent_direction, uint32_t relative_turn
    );
    /// Walks from the start toward the nearest goal, following walls both ways, and marks the path as seed cells.
    ///
    /// @param step_limit maximum charged seed-walk steps, zero for the map-sized bound
    /// @param[out] capped whether the configured limit stopped the walk
    /// @return the lowest heuristic seen on the walk, 0 when it reached a goal cell
    int32_t seed_wall_follow(uint32_t step_limit, bool& capped);
    /// Clears the route for a failed job.
    void fail_empty();
    /// Returns a search-map cell without a bounds check.
    ///
    /// @param x cell column
    /// @param z cell row
    /// @return the cell
    [[nodiscard]] SearchMapCell& at(uint32_t x, uint32_t z);
    /// Returns a search-map cell without a bounds check.
    ///
    /// @param x cell column
    /// @param z cell row
    /// @return the cell
    [[nodiscard]] const SearchMapCell& at(uint32_t x, uint32_t z) const;
    /// Tests whether a cell lies on the search map.
    ///
    /// @param x cell column
    /// @param z cell row
    /// @return true inside the map
    [[nodiscard]] bool in_map(uint32_t x, uint32_t z) const noexcept;

    SearchRecord record_{};
    std::span<const uint16_t> sight_grid_{};
    SearchHeap heap_;
    std::vector<SearchMapCell> cells_;
    std::vector<uint32_t> dirty_;
    std::vector<RoutePoint> route_;
    std::array<uint8_t, 8> turn_penalty_{};
    std::array<uint8_t, 8> move_cost_{};
    std::array<int16_t, 2> start_{};
    std::array<int16_t, 2> finish_{};
    std::array<int32_t, 2> nearest_{};
    uint32_t width_{}, height_{}, world_width_{}, world_height_{};
    int32_t threshold_{};
    int32_t fan_{};
    int32_t budget_{};
    int32_t heuristic_scale_{search_heuristic_scale};
    uint32_t order_flags_{};
    uint8_t mask_bit_{};
};

// How the job scan reaches units' movement objects (Unit.movement side table).
struct SearchUnitAccess {
    void* context{};
    /// Returns the unit's local ground navigator when it has one and a movement class.
    ///
    /// @param context SearchUnitAccess::context
    /// @param unit candidate unit
    /// @return the navigator, or null for mirrored, air or class-less movement objects
    Navigation* (*navigator)(void* context, sim::simulation_state::Unit& unit){};
    /// Fills the job inputs and the movement class's map.
    ///
    /// @param context SearchUnitAccess::context
    /// @param unit searching unit
    /// @param[out] begin unit cell rectangle, heading, occupancy history and map size
    /// @param[out] map the unit's movement-class map
    /// @return false to skip the job
    bool (*prepare)(
        void* context, sim::simulation_state::Unit& unit, SearchBegin& begin, MovementMap*& map
    ){};
    /// Installs a finished route (empty on failure) on the unit's navigator.
    ///
    /// Called through accept_path with the unit's current geometry.
    ///
    /// @param context SearchUnitAccess::context
    /// @param unit searching unit
    /// @param navigation the unit's navigator
    /// @param route route points in integer world X/Z
    void (*publish)(
        void* context,
        sim::simulation_state::Unit& unit,
        Navigation& navigation,
        std::span<const RoutePoint> route
    ){};
    /// Reports the unit's current occupancy rectangle and change tick, used to release the map.
    ///
    /// @param context SearchUnitAccess::context
    /// @param unit searching unit
    /// @param[out] rectangle the unit's packed cell and footprint
    /// @param[out] changed_tick the movement object's occupancy_changed_tick
    void (*occupancy)(
        void* context,
        sim::simulation_state::Unit& unit,
        OccupancyRectangle& rectangle,
        uint32_t& changed_tick
    ){};
    /// Offers one unit before the ordinary cursor, or none when null.
    ///
    /// Called at most once per player per scan, on every fourth credited
    /// cursor pick. The returned index addresses the player's current unit
    /// span, which the callback must not change. An out-of-order choice does
    /// not move the ordinary cursor. The usual search readiness, preparation
    /// and work charges still apply.
    ///
    /// @param context SearchUnitAccess::context
    /// @param player the credited player slot
    /// @param tick current game tick
    /// @param[out] unit_index an index in the player's current unit span
    /// @return true to offer the indexed unit, false for the ordinary cursor
    bool (*preferred_unit)(void* context, uint8_t player, uint32_t tick, uint32_t& unit_index){};
};

// The path search: player job state, the active job record and its worker.
struct SearchScheduler {
    SearchPlayerJobState jobs;
    SearchController controller;
    SearchWorker worker;
    /// Runtime pick phase, independent of the heuristic's usage-count reset.
    std::array<uint8_t, search_player_slots> preferred_pick_phase{};
};

/// Spends the per-tick path search budget.
///
/// Refreshes per-player heuristic weights every 150 scans, credits each active
/// player an equal share of the tick budget, then round-robins players while
/// credit remains. With no job it steps the player's unit cursor and starts a
/// job for a unit whose navigator asks for a search (cost 1, +100 when a job
/// starts, +1 for each step of the job's wall-follow walk); with a job it
/// expands up to 100 nodes and publishes the route when a goal cell is
/// reached. The scan ends early when no player has credit left to pick, or the
/// picked player has no unit list, no units, or a cursor past its units; a job
/// the controller refuses to start costs its 100 all the same.
/// An optional preferred-unit callback may replace one of every four cursor
/// picks, at most once per player per scan, within the same work budget.
///
/// @param[in,out] scheduler job state, controller and worker
/// @param players player slots with their unit lists
/// @param player_count live players
/// @param units_per_player unit slots per player
/// @param tick current game tick
/// @param sight_grid Game.sight_grid
/// @param access unit movement-object callbacks
/// @return how many round-robin steps ran
int32_t scan_player_jobs(
    SearchScheduler& scheduler,
    std::span<sim::simulation_state::Player> players,
    uint16_t player_count,
    uint16_t units_per_player,
    uint32_t tick,
    std::span<const uint16_t> sight_grid,
    const SearchUnitAccess& access
);

} // namespace oa::sim::ground_orders
