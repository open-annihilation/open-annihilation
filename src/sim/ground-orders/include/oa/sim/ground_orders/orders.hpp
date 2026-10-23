// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/data/match_rules.hpp"
#include "oa/sim/ground_orders/goals.hpp"
#include "oa/sim/unit_spawn/legacy_views.hpp"
#include "oa/sim/unit_movement/movement.hpp"
#include "oa/sim/ground_orders/navigator.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace oa::sim::ground_orders {
class MovementMap;
struct OccupancyRectangle;
// Mission kinds: each mission name's index in the case-insensitively sorted
// mission list.
inline constexpr uint8_t move_ground_kind = 26, standby_kind = 41,
                         vtol_land_if_can_kind = 52, // VTOL_LandIfCan
    vtol_landing_kind = 53,                          // VTOL_Landing
    vtol_move_kind = 55,                             // VTOL_Move
    vtol_standby_kind = 64;                          // VTOL_Standby
// OrderState.command_flags: play the command acknowledgement sound once.
inline constexpr uint8_t order_announce_flag = 0x20;

// The mission fields of an order that simulation_state::Order does not hold.
struct OrderState {
    Point destination{};        // signed 16.16
    int32_t tolerance{};        // arrival tolerance, world units; Move_Ground adds 4
    int16_t anchor_x{};         // high word of the saved X
    int16_t anchor_z{};         // high word of the saved Z
    uint8_t command_flags{};    // order_announce_flag and the order's other command bits
    std::unique_ptr<Goal> goal; // the order's goal; navigation needs its address to stay put
};

// The path-search controller's active record. The controller owns at most one
// in-progress search and identifies cancellation by its navigator.
struct SearchRecord {
    sim::simulation_state::Unit* unit{};
    Navigation* navigation{};
    Goal* goal{};
    MovementMap* movement_map{};
};

class SearchController {
  public:

    /// Returns the active search record.
    [[nodiscard]] const SearchRecord& active() const noexcept { return active_; }

    /// Returns whether no search is in progress (the record has no unit).
    [[nodiscard]] bool idle() const noexcept { return active_.unit == nullptr; }

    /// Starts a search job.
    ///
    /// @param record unit, navigator, goal and movement map of the job
    /// @return false, with nothing started, when a field is missing or another job is active
    [[nodiscard]] bool begin(SearchRecord record) noexcept;
    /// Drops the active job when it belongs to a navigator.
    ///
    /// A cancel for another navigator is ignored; a matching cancel clears the whole
    /// record.
    ///
    /// @param navigation navigator whose job is cancelled
    void cancel(Navigation& navigation) noexcept;
    /// Finishes the active job: releases the movement map and clears the job's unit and map.
    ///
    /// @param rectangle the searching unit's footprint, released from the movement map
    /// @param changed_tick tick the footprint changed at
    /// @quirk The navigator and goal are left as they were; idleness is decided by the unit alone.
    void complete(OccupancyRectangle rectangle, uint32_t changed_tick);

  private:

    SearchRecord active_{};
};

struct UnitView {
    sim::simulation_state::Unit& state;
    sim::unit_movement::Unit& geometry;
    sim::unit_movement::Movement& movement;
    Navigation& navigation;
    bool attached{};                      // Unit.attach_parent is set
    std::array<uint8_t, 3>& weapon_flags; // UnitWeapon.flags of each weapon slot
};

struct Host {
    virtual ~Host() = default;
    /// The rules the match plays by (Match::rules_view); unset, 3.1c's.
    data::match_rules::MatchRulesView rules{};
    /// Cancels the world search job a navigator owns; routing decisions stay in this component.
    ///
    /// @param navigation navigator whose job is cancelled
    virtual void cancel_search(Navigation& navigation) = 0;
    /// Plays one of the unit's sound categories.
    ///
    /// @param unit sounding unit
    /// @param category sound category (5 command acknowledgement, 6 arrival)
    virtual void play_sound(sim::simulation_state::Unit& unit, uint32_t category) = 0;
    /// Wakes a weapon slot of the unit.
    ///
    /// @param unit armed unit
    /// @param slot weapon slot, 0..2
    virtual void wake_weapon(sim::simulation_state::Unit& unit, uint32_t slot) = 0;
    /// Searches for a target for a standing unit.
    ///
    /// @param unit searching unit
    /// @return the target, or null
    virtual sim::simulation_state::Unit* find_target(sim::simulation_state::Unit& unit) = 0;
    /// Issues an attack order on a target.
    ///
    /// @param unit attacking unit
    /// @param target unit to attack
    /// @return true when the attack was issued
    virtual bool
    issue_attack(sim::simulation_state::Unit& unit, sim::simulation_state::Unit& target) = 0;
    /// Draws from the shared deterministic stream.
    ///
    /// @param exclusive_limit exclusive upper bound
    /// @return a value in 0..exclusive_limit-1
    virtual uint32_t random(uint32_t exclusive_limit) = 0;
};

/// Tears down a local navigator with its unit's movement object.
///
/// The path search job it owns is dropped before the navigator goes away.
///
/// @param[in,out] controller search controller
/// @param navigation navigator going away
void release_navigation(SearchController& controller, Navigation& navigation) noexcept;
/// Replaces the navigator goal and seeds a direct two-point route when no usable route exists.
///
/// Cancels the navigator's search job, flags the replaced goal's order and marks
/// a search pending. An existing route ending in the goal, or ending less than
/// half the unit's distance from it, is kept; otherwise a unit with a primary
/// non-retry order gets the route from its integer position to the goal
/// position. The search timestamp resets when at least 10 ticks old. A
/// navigator holding more points than its capacity is left as it is.
///
/// @param u unit, geometry, movement and navigator
/// @param goal new goal, or null to clear it
/// @param tick current game tick
/// @param host search cancellation
void install_goal(UnitView u, Goal* goal, uint32_t tick, Host& host);
/// Installs a search result, or reports a failed search on the goal's order.
///
/// An empty result raises path_failed_event unless the unit already satisfies
/// the goal. Either way the pending bit clears and the route is marked changed.
///
/// @param u unit geometry and navigator
/// @param points route points in integer world X/Z; truncated to the navigator capacity
/// @return true when path_failed_event was raised, false otherwise
bool accept_path(UnitView u, std::span<const RoutePoint> points);
/// Drops route points from the front of the route.
///
/// Clears route_present when fewer than two points remain. A count past the
/// points held, or a navigator holding more points than its capacity, drops
/// nothing.
///
/// @param[in,out] n navigator
/// @param count points to drop
void advance_path(Navigation& n, uint32_t count);
/// Runs arrival, waypoint consumption and search requests for one tick.
///
/// Arrival raises arrived_event and detaches the goal; a waypoint within about
/// five world units is consumed; a blocked move or an exhausted route marks a
/// search pending. A navigator holding more points than its capacity is left
/// as it is.
///
/// @param u unit geometry, movement and navigator
/// @param tick current game tick
/// @param host search cancellation for the detached goal
void tick_navigation(UnitView u, uint32_t tick, Host& host);
/// Marks the navigator's search timestamp once its pending request has aged 60 ticks.
///
/// @param[in,out] navigation navigator
/// @param tick current game tick
/// @return false while throttled or not pending
bool search_ready(Navigation& navigation, uint32_t tick) noexcept;
/// Returns the navigator's next three steering points.
///
/// Missing trailing points repeat the last one.
///
/// @param n navigator
/// @return signed 16.16 points with Y zero; all three at the origin for an empty
///         route or one holding more points than its capacity
[[nodiscard]] std::array<Point, 3> steering_points(const Navigation& n) noexcept;
/// Snaps X and Z onto the centre of the footprint cell they fall in.
///
/// @param[in,out] destination signed 16.16 position; Y is untouched
/// @param footprint_x footprint width in cells
/// @param footprint_z footprint depth in cells
void snap_to_footprint_centre(
    Point& destination, int16_t footprint_x, int16_t footprint_z
) noexcept;
/// Runs the Move_Ground mission handler.
///
/// Phase 0 plays the pending command acknowledgement (sound category 5),
/// builds a circle goal at the destination with tolerance + 4 (not for
/// aircraft, which keep their air goal), installs it and waits for arrival,
/// path failure or goal replacement. Phase 1 completes on arrival (sound
/// category 6). The handler does not advance the phase itself.
///
/// @param u unit, geometry, movement and navigator
/// @param[in,out] order the mission order: phase, raised and awaited events
/// @param[in,out] extra destination, tolerance, flags and the owned goal
/// @param events events that woke the order
/// @param tick current game tick
/// @param host sounds and search cancellation
/// @return the scheduler result: 1 waiting (phase 0), 5 arrived, 9 retry on another event, 7 for an attached unit or an unknown phase
uint32_t move_ground(
    UnitView u,
    sim::simulation_state::Order& order,
    OrderState& extra,
    uint32_t events,
    uint32_t tick,
    Host& host
);
/// Runs the Standby mission handler.
///
/// Phase 0 wakes each armed, sleeping weapon slot and waits one tick. Phase 1
/// attacks a found target when the unit's attack mode is seek, otherwise waits
/// 30..59 ticks.
///
/// @param u unit state and weapon flags
/// @param[in,out] order the mission order: phase and awaited events
/// @param tick current game tick
/// @param host weapon wake-up, target search, attack and random stream
/// @return the scheduler result: 1 waiting (phase 0), 5 attacking, 2 waiting again, 7 without an object or in an unknown phase
uint32_t standby(UnitView u, sim::simulation_state::Order& order, uint32_t tick, Host& host);
/// How the ground steering treats a unit faster than any stock unit, one that
/// steps more than 6 pixels a tick.
enum class FastUnitSteering : uint8_t {
    /// As 3.1c steers every unit. A match without mutators steers so, since a
    /// mod's units, fast ones included, must move as 3.1c moves them for its
    /// network games with 3.1c and its recordings.
    as_3_1c,
    /// Held to its route, as a match that applied mutators steers it: it never
    /// steps past a waypoint, comes on no faster than its braking can stop
    /// it, and slows while it turns.
    held_to_route,
};

/// Drives a ground unit along the navigator route: turns toward and accelerates along it.
///
/// The second route point is pulled back along the first segment to 80 world
/// units ahead; the unit turns toward it within its turn rate and accelerates
/// when both the turn and the stop fit before the route, otherwise decelerates.
/// Without a route it only decelerates; an empty or over-full route counts as
/// none. A zero maximum turn, or a deceleration whose double wraps to zero,
/// leaves no turn or stop distance to measure: the unit turns, then
/// decelerates. No search result is fabricated.
///
/// @param[in,out] unit unit whose heading changes
/// @param[in,out] movement movement object whose speed and velocity change
/// @param n navigator holding the route
/// @param acceleration signed 16.16 speed gain per tick
/// @param deceleration signed 16.16 speed loss per tick
/// @param sea_level map sea level in whole world units
/// @param fast_units how a unit faster than any stock unit is steered; as 3.1c when left out
void steer_ground(
    sim::unit_movement::Unit& unit,
    sim::unit_movement::Movement& movement,
    const Navigation& n,
    Fixed acceleration,
    Fixed deceleration,
    uint8_t sea_level,
    FastUnitSteering fast_units = FastUnitSteering::as_3_1c
);
/// Drives a ground unit along a mirrored navigator's shared route head.
///
/// @param[in,out] unit unit whose heading changes
/// @param[in,out] movement movement object whose speed and velocity change
/// @param n mirrored navigator holding the route head
/// @param acceleration signed 16.16 speed gain per tick
/// @param deceleration signed 16.16 speed loss per tick
/// @param sea_level map sea level in whole world units
/// @param fast_units how a unit faster than any stock unit is steered; as 3.1c when left out
void steer_ground(
    sim::unit_movement::Unit& unit,
    sim::unit_movement::Movement& movement,
    const MirroredNavigation& n,
    Fixed acceleration,
    Fixed deceleration,
    uint8_t sea_level,
    FastUnitSteering fast_units = FastUnitSteering::as_3_1c
);
// Default when OTA gravity is unset.
inline constexpr Fixed default_gravity = 0x1fdb;
} // namespace oa::sim::ground_orders
