// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/core/world.h"
#include "oa/data/match_rules.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::sim::simulation_state {
// Unit, player and game state are the canonical oa/core records in oa::World.
// Order is a native view of the game's mission record.
struct Order {
    uint8_t kind{};         // mission kind; selects the handler Host::dispatch_mission runs
    uint8_t phase{};        // the handler's state; the scheduler restarts or advances it
    uint32_t wait_events{}; // events the order waits for; timer_event waits for wake_tick
    uint32_t wake_tick{};   // Game.tick the timer event fires at
    int16_t seen_x{};       // target cell the overlays last saw, map pixels
    int16_t seen_z{};
    uint8_t preserve_flags{}; // bit 2 keeps the order through a partial clear_orders
    uint8_t flags{};          // bit 0 detached, bit 2 secondary queue, bit 7 retry
    uint32_t issue_tick{};    // Game.tick at creation
    Order* next{};            // next order in the same queue
    uint32_t raised_events{}; // events raised on the order while it waited
};

// Type fields carried by sim::unit_spawn::Type for code not yet reading oa::UnitDef;
// sim::unit_spawn::load_unit_def copies them into the canonical record.
struct UnitType {
    uint32_t maximum_health{};      // UnitDef.max_damage
    int16_t heal_time{};            // UnitDef.heal_time; eligibility only
    uint8_t waterline_offset{};     // UnitDef.water_line
    uint8_t default_mission_type{}; // UnitDef.default_mission_type
    uint32_t flags{};               // UnitDef.flags
    uint32_t abilities{};           // UnitDef.abilities; bits 20..22 self-destruct countdown
};

// Order list heads of one unit slot. Order records are not canonical yet, so
// the heads live in this side table, indexed by unit slot, and
// Unit.primary_order/secondary_order stay zero.
struct OrderQueue {
    Order* primary{};
    Order* secondary{};
};

/// Tests whether a player record is live.
///
/// @param player player record
/// @return true when in use, with status local, computer or mirrored, and index not 10
[[nodiscard]] inline bool player_active(const oa::Player& player) noexcept {
    const auto status = player.status;
    return player.in_use != 0 && (status == 1 || status == 2 || status == 3) && player.index != 10;
}

/// Tests whether a player is still in the game.
///
/// @param player player record
/// @return true when active, with live units or none created yet
[[nodiscard]] inline bool player_participating(const oa::Player& player) noexcept {
    return player_active(player) && (player.unit_count != 0 || player.units_created == 0);
}

/// Tests whether a player slot below ten holds a live record.
///
/// @param index player slot
/// @param player the slot's record
/// @return true for a slot below 10 whose record is active
[[nodiscard]] inline bool player_slot_active(uint8_t index, const oa::Player& player) noexcept {
    return index < 10 && player_active(player);
}

/// Finds a unit's first order of a kind.
///
/// Orders of the mission kinds that run in the secondary queue, BuildWeapon (13)
/// and SelfDestruct (38), are looked for there; every other kind in the primary
/// queue.
///
/// @param queue the unit's order lists
/// @param kind mission kind
/// @return the first matching order, or null
[[nodiscard]] inline Order* find_order(const OrderQueue& queue, uint8_t kind) noexcept {
    const bool secondary = kind == 13 || kind == 38;
    for (auto* order = secondary ? queue.secondary : queue.primary; order != nullptr;
         order = order->next)
        if (order->kind == kind)
            return order;
    return nullptr;
}

/// Finds the smallest player mark in 1..10 that no active player holds in Player.machine_group.
///
/// @param world world whose players are checked
/// @return the mark, or 0 when every mark is taken
[[nodiscard]] uint8_t lowest_unused_player_mark(const oa::World& world) noexcept;

/// Which units nearest_candidate_unit passes over beyond 3.1c's tests; a default
/// filter passes over none.
struct CandidateFilter {
    /// Pass over a unit whose occupancy bits hold state 2 or 3, or whose move-rate
    /// bits hold tier 2 or 3, in place of 3.1c's occupancy state 2 alone.
    bool widened_flags{};
    /// Pass over a fully submerged unit: one whose last water state
    /// (Unit.last_occupy_code) is 3.
    bool skip_submerged{};
};

/// Finds the nearest candidate unit of the players a relation row admits.
///
/// A unit qualifies when it is live (OA_UNIT_FLAG_LIVE), its occupancy bits are not
/// 2, it is not flagged OA_UNIT_FLAG_NOT_SELECTABLE and it is not cloaked
/// (OA_UNIT_STATE_CLOAKED), and the filter does not pass over it. Rank is the sum of
/// the high halves of the signed X and Z squares; Y is ignored. The first strict
/// minimum wins.
///
/// @param world world whose players are searched
/// @param relation the reference player's Player.alliance table, indexed by each
///        candidate player's Player.index; a nonzero entry skips that player; must
///        cover every tested index
/// @param x reference point X, 16.16
/// @param z reference point Z, 16.16
/// @param filter the units passed over besides; none when left out
/// @return the nearest unit, or null
[[nodiscard]] oa::Unit* nearest_candidate_unit(
    oa::World& world,
    std::span<const uint8_t> relation,
    int32_t x,
    int32_t z,
    const CandidateFilter& filter = {}
) noexcept;

/// Averages a list of unit positions.
///
/// The average uses the signed high (whole-unit) words of the unit positions and is
/// shifted back to 16.16.
///
/// @param positions unit positions (Unit.position), 16.16 bit patterns
/// @param[out] out receives the centroid, 16.16; unchanged for an empty list
/// @return false for an empty list
[[nodiscard]] inline bool selection_centroid(
    std::span<const std::array<uint32_t, 3>> positions, std::array<int32_t, 3>& out
) noexcept {
    if (positions.empty())
        return false;
    int32_t sum_x = 0;
    int32_t sum_y = 0;
    int32_t sum_z = 0;
    for (const auto& position : positions) {
        sum_x += static_cast<int16_t>(position[0] >> 16);
        sum_y += static_cast<int16_t>(position[1] >> 16);
        sum_z += static_cast<int16_t>(position[2] >> 16);
    }
    const auto count = static_cast<int32_t>(positions.size());
    out[0] = (sum_x / count) << 16;
    out[1] = (sum_y / count) << 16;
    out[2] = (sum_z / count) << 16;
    return true;
}

/// Takes the centroid of the first non-empty of three selection lists, tried in order.
///
/// @param first_choice unit positions of the selection tried first
/// @param second_choice unit positions of the selection tried second
/// @param third_choice unit positions of the selection tried last
/// @param[out] out receives the centroid, 16.16; unchanged when all are empty
/// @return false only when all three lists are empty
[[nodiscard]] inline bool priority_selection_centroid(
    std::span<const std::array<uint32_t, 3>> first_choice,
    std::span<const std::array<uint32_t, 3>> second_choice,
    std::span<const std::array<uint32_t, 3>> third_choice,
    std::array<int32_t, 3>& out
) noexcept {
    if (selection_centroid(first_choice, out))
        return true;
    if (selection_centroid(second_choice, out))
        return true;
    return selection_centroid(third_choice, out);
}

/// Tests the viewpoint player's sight bit for a 16.16 point.
///
/// Cells are the signed high words: X >> 5 and (Z - (Y >> 1)) >> 5. The bit tested is
/// that of the viewpoint (Game.viewpoint_player), not of the queried player.
///
/// @param width sight grid width in cells (Player.sight_width)
/// @param height sight grid height in cells (Player.sight_height)
/// @param sight_words the shared sight grid; must cover every in-range cell
/// @param viewpoint_bit bit index of the viewpoint player
/// @param position point, 16.16 bit patterns
/// @return true when the cell is on the grid and its word has the bit
/// @quirk A negative cell fails the unsigned bounds test.
[[nodiscard]] inline bool viewpoint_sees_point(
    uint32_t width,
    uint32_t height,
    std::span<const uint16_t> sight_words,
    uint8_t viewpoint_bit,
    const std::array<uint32_t, 3>& position
) noexcept {
    const auto cell_x =
        static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(position[0] >> 16)) >> 5);
    const auto cell_z = static_cast<uint32_t>(
        (static_cast<int32_t>(static_cast<int16_t>(position[2] >> 16)) -
         (static_cast<int32_t>(static_cast<int16_t>(position[1] >> 16)) >> 1)) >>
        5
    );
    if (cell_x >= width || cell_z >= height)
        return false;
    const auto word = sight_words[static_cast<std::size_t>(width * cell_z + cell_x)];
    return (static_cast<uint32_t>(word) & (1u << (viewpoint_bit & 0x1fu))) != 0u;
}

/// Tests whether a 16.16 point is visible to the viewer.
///
/// With bit 1 of Game.visibility_flags set, the player's coverage grid
/// (Player.coverage_grid) decides: a nonzero byte is visible and an off-grid cell is
/// not. Any other flag value tests the viewpoint's sight bit instead
/// (viewpoint_sees_point).
///
/// @param world_sight_flags Game.visibility_flags
/// @param width grid width in cells (Player.sight_width)
/// @param height grid height in cells (Player.sight_height)
/// @param coverage the player's coverage grid; must cover every in-range cell
/// @param sight_words the shared sight grid; must cover every in-range cell
/// @param viewpoint_bit bit index of the viewpoint player
/// @param position point, 16.16 bit patterns
/// @return true when visible
/// @quirk An out-of-range cell on the coverage path is invisible and does not fall
///        through to the sight grid.
[[nodiscard]] inline bool point_visible(
    uint8_t world_sight_flags,
    uint32_t width,
    uint32_t height,
    std::span<const uint8_t> coverage,
    std::span<const uint16_t> sight_words,
    uint8_t viewpoint_bit,
    const std::array<uint32_t, 3>& position
) noexcept {
    if ((world_sight_flags & 2u) == 2u) {
        const auto cell_x = static_cast<uint32_t>(
            static_cast<int32_t>(static_cast<int16_t>(position[0] >> 16)) >> 5
        );
        const auto cell_z = static_cast<uint32_t>(
            (static_cast<int32_t>(static_cast<int16_t>(position[2] >> 16)) -
             (static_cast<int32_t>(static_cast<int16_t>(position[1] >> 16)) >> 1)) >>
            5
        );
        if (cell_x >= width || cell_z >= height)
            return false;
        return coverage[static_cast<std::size_t>(width * cell_z + cell_x)] != 0;
    }
    return viewpoint_sees_point(width, height, sight_words, viewpoint_bit, position);
}

/// Where the scheduler reports an order its handler ended, just before the
/// order is removed. A null `ended` reports nothing.
struct OrderEnds {
    void* context{}; ///< passed to ended
    /// Called before the ended order is removed; null reports nothing.
    ///
    /// @param context OrderEnds::context
    /// @param unit unit carrying the order
    /// @param order order about to be removed
    /// @param done true when the handler finished the order, false when it failed
    void (*ended)(void* context, const oa::Unit& unit, const Order& order, bool done){};
};

// Operations owned by other systems; all are mandatory, never default no-ops.
struct Host {
    virtual ~Host() = default;
    /// The rules the match plays by (Match::rules_view); unset, 3.1c's.
    data::match_rules::MatchRulesView rules{};
    /// Reports an order `primary_orders` is about to remove because its handler
    /// ended it. Null reports nothing.
    ///
    /// `primary_orders` calls it, before removing an order whose handler ended
    /// it, with `done` true for result 5, and false for 8, for a 9 that gives
    /// way to the next order, and for the head order of a result that clears
    /// the queue. It must not change the unit, the order or the queue.
    OrderEnds order_ends{};
    /// Runs one step of an order's mission handler.
    ///
    /// @param world world the unit lives in
    /// @param unit unit carrying the order
    /// @param order order to run; its phase is the handler's state
    /// @param events events that woke the order; 0 for secondary orders
    /// @return scheduler result: 0 restart at phase 0, 1 next phase, 2 or 4 keep, 3 wait
    ///         15 + random ticks; for a primary order 5 or 8 remove, 6 rotate to the
    ///         tail, 9 retry, other values clear every order; for a secondary order 6
    ///         or 7 remove and stop, other values remove
    virtual uint32_t
    dispatch_mission(oa::World& world, oa::Unit& unit, Order& order, uint32_t events) = 0;
    /// Draws from the synced random stream.
    ///
    /// @param exclusive_limit upper bound of the draw
    /// @return a value in [0, exclusive_limit)
    virtual uint32_t random_bounded(uint32_t exclusive_limit) = 0;
    /// Queues a locally simulated unit's default mission when it has no order.
    ///
    /// @param world world the unit lives in
    /// @param unit idle unit
    virtual void queue_default_mission(oa::World& world, oa::Unit& unit) = 0;
    /// Clears one weapon slot's target when a weapon event wakes an order.
    ///
    /// @param unit unit whose weapon is reset
    /// @param slot weapon slot 0..2
    virtual void clear_weapon_target(oa::Unit& unit, uint32_t slot) = 0;
    /// Frees an order already unlinked from its queue.
    ///
    /// @param unit unit that carried it
    /// @param order unlinked order
    virtual void destroy_order(oa::Unit& unit, Order& order) = 0;
    /// Runs the unit's COB script.
    ///
    /// @param unit unit with a script
    /// @param steps ticks to run, 1 per simulation tick
    virtual void tick_script(oa::Unit& unit, uint32_t steps) = 0;
    /// Applies scaled damage to a unit.
    ///
    /// @param unit damaged unit
    /// @param amount damage before scaling
    /// @param kind damage kind; 11 for environment damage below sea level
    virtual void apply_scaled_damage(oa::Unit& unit, int32_t amount, uint32_t kind) = 0;
    /// Regenerates a damaged unit's health when self_heal_due says so, at
    /// self_heal_rate.
    ///
    /// @param unit damaged unit
    virtual void regenerate_health(oa::Unit& unit) = 0;
    /// Runs one movement tick of a unit with a movement object.
    ///
    /// @param unit moving unit
    virtual void movement_tick(oa::Unit& unit) = 0;
    /// Returns the terrain height under a unit.
    ///
    /// @param unit unit to measure under
    /// @return height, whole units
    virtual int32_t terrain_height_under(oa::Unit& unit) = 0;
    /// Settles a unit on the ground under it.
    ///
    /// @param unit unit to settle
    virtual void settle_on_ground(oa::Unit& unit) = 0;
    /// Kills a unit flagged to die (OA_UNIT_FLAG_DEATH_PENDING).
    ///
    /// @param unit unit to kill
    /// @param kind the unit's damage kind
    virtual void kill_unit(oa::Unit& unit, uint8_t kind) = 0;
    /// Runs the per-unit step that comes before the unit tick (wind generators).
    ///
    /// @param unit unit about to tick
    virtual void update_wind_generator(oa::Unit& unit) = 0;
    /// Aims the weapons of a unit whose owner is simulated here.
    ///
    /// @param unit locally simulated unit
    virtual void tick_weapon_aim(oa::Unit& unit) = 0;
    /// Reports a locally simulated player's state after its units ticked in a live
    /// multiplayer game.
    ///
    /// @param player player whose units just ticked
    virtual void local_player_ticked(oa::Player& player) = 0;
    /// Tests whether a key is held.
    ///
    /// @param key virtual key code (0xf9 holds the periodic follow)
    /// @return true while held
    virtual bool is_key_down(uint32_t key) = 0;
    /// Moves the viewpoint to the next unit, every 90 ticks while enabled.
    virtual void select_next_viewpoint_unit() = 0;
    /// Follows the next selected unit, every 90 ticks while enabled.
    ///
    /// @param backward nonzero follows the previous selected unit instead; update_units
    ///        passes 0
    virtual void follow_next_selected(uint32_t backward) = 0;
};

// Words seed_placement_grids writes on a computer player's controller, not on the
// unit record.
struct PlacementGrids {
    int16_t step_x{};
    int16_t step_y{};
    int16_t phase_x{};
    int16_t phase_y{};
    int32_t base{}; // stored as a 32-bit word; only the low 16 bits are read back
    int16_t alt_step_x{};
    int16_t alt_step_y{};
    int16_t alt_phase_x{};
    int16_t alt_phase_y{};
    int32_t alt_base{}; // same low-word use as base
};

/// Seeds a computer player's two random building-placement grids.
///
/// The base words are set to 3 and 6. Each step is the low 16 bits of a random draw
/// plus the base word plus 8, and each phase is a random draw below the step, minus
/// half the step (rounded toward zero). The first grid's limits are 10 and 3, the
/// second's 0x14 and 3.
///
/// @param[out] grids the controller's placement-grid words
/// @param host synced random stream; eight draws in field order
/// @quirk The base add is 16-bit and the +8 32-bit; only the low word is stored.
inline void seed_placement_grids(PlacementGrids& grids, Host& host) {
    const auto step = [](uint32_t roll, int32_t base) noexcept {
        const auto sum = roll + static_cast<uint32_t>(static_cast<uint16_t>(base)) + 8u;
        return static_cast<int16_t>(static_cast<uint16_t>(sum));
    };
    const auto phase = [&host](int16_t span) noexcept {
        const auto roll = host.random_bounded(static_cast<uint32_t>(static_cast<int32_t>(span)));
        const auto half = static_cast<int32_t>(span) / 2;
        const auto bits = roll - static_cast<uint32_t>(half);
        return static_cast<int16_t>(static_cast<uint16_t>(bits));
    };
    grids.base = 3;
    grids.step_x = step(host.random_bounded(10), grids.base);
    grids.step_y = step(host.random_bounded(3), grids.base);
    grids.phase_x = phase(grids.step_x);
    grids.phase_y = phase(grids.step_y);
    grids.alt_base = 6;
    grids.alt_step_x = step(host.random_bounded(0x14), grids.alt_base);
    grids.alt_step_y = step(host.random_bounded(3), grids.alt_base);
    grids.alt_phase_x = phase(grids.alt_step_x);
    grids.alt_phase_y = phase(grids.alt_step_y);
}

inline constexpr std::size_t default_order_budget = 100000;

/// Why an order walk or a unit's tick stopped before its end.
///
/// The step stops where it finds the fault and leaves the rest of its work
/// undone; what it did before stays done.
enum class StepFault : uint8_t {
    none,               ///< the step ran to its end
    order_budget_spent, ///< a queue walk or a scheduler took more steps than its budget
    order_not_queued,   ///< the order to rotate is not in the primary queue
    untyped_unit,       ///< the unit has no type
    orders_short,       ///< the order lists do not cover the unit pool
};

/// Tests whether a unit's owner is present and simulated here (status 1 or 2).
///
/// @param world world the unit lives in
/// @param unit unit to test
/// @return true for a local or computer owner
[[nodiscard]] bool locally_simulated(const oa::World& world, const oa::Unit& unit) noexcept;
/// Tests whether a unit can stay selected.
///
/// A unit whose attach parent lies outside the unit pool cannot.
///
/// @param world world the unit lives in
/// @param unit unit to test
/// @return true with OA_UNIT_FLAG_SELECTABLE set, Unit.build_remaining zero or
///         unordered, Unit.capture_cooldown zero, and no attach parent or one flagged
///         OA_UNIT_FLAG_AIR_BASE
[[nodiscard]] bool unit_selectable(const oa::World& world, const oa::Unit& unit);
/// Unlinks an order from its queue and hands it to the host to destroy.
///
/// An order that was not the primary head is marked detached first; an order not in
/// its queue is left alone.
///
/// @param[in,out] queue the unit's order lists
/// @param unit unit carrying the order
/// @param[in,out] order order to remove
/// @param host destroys the order
/// @return order_budget_spent, with the queue unchanged, when the walk to the
///         order is longer than the order budget; else none
StepFault remove_order(OrderQueue& queue, oa::Unit& unit, Order& order, Host& host);
/// Moves a primary order to the tail of the primary queue.
///
/// A fault leaves the queue unchanged.
///
/// @param[in,out] queue the unit's order lists
/// @param[in,out] order order to move
/// @return order_not_queued when the order is not in the primary queue,
///         order_budget_spent when the queue is longer than the order budget,
///         else none
StepFault rotate_primary(OrderQueue& queue, Order& order);
/// Destroys a unit's orders.
///
/// @param[in,out] queue the unit's order lists
/// @param unit unit carrying the orders
/// @param all true destroys every primary and secondary order; false only the primary
///        orders without preserve bit 2
/// @param host destroys the orders
/// @return order_budget_spent when the queues are longer than the order budget,
///         the orders reached by then destroyed; else none
StepFault clear_orders(OrderQueue& queue, oa::Unit& unit, bool all, Host& host);
/// Runs the primary order scheduler until a handler yields.
///
/// An order runs once its wake tick has passed or an awaited event is raised; the
/// handler's result then restarts, advances, delays, removes, rotates or retries it.
/// A unit simulated here with no order queues its default mission.
///
/// @param world world the unit lives in; its tick wakes waiting orders
/// @param[in,out] queue the unit's order lists
/// @param[in,out] unit unit whose events are consumed
/// @param host mission handlers and random stream
/// @param budget most scheduler steps before giving up
/// @return order_budget_spent when the budget runs out, untyped_unit when a unit
///         simulated here with no order has no type, a fault of an order
///         removal or rotation, else none
StepFault primary_orders(
    oa::World& world,
    OrderQueue& queue,
    oa::Unit& unit,
    Host& host,
    std::size_t budget = default_order_budget
);
/// Runs every ready secondary order.
///
/// @param world world the unit lives in; its tick wakes waiting orders
/// @param[in,out] queue the unit's order lists
/// @param unit unit carrying the orders
/// @param host mission handlers and random stream
/// @param budget most scheduler steps before giving up
/// @return order_budget_spent when the budget runs out, a fault of an order
///         removal, else none
StepFault secondary_orders(
    oa::World& world,
    OrderQueue& queue,
    oa::Unit& unit,
    Host& host,
    std::size_t budget = default_order_budget
);
/// Corrects the height of a unit whose height is dirty or whose type floats at a waterline.
///
/// Clears the dirty flag, then places a unit with a movement object on the terrain,
/// at its waterline below sea level, or on the ground by the host.
///
/// @param world world the unit lives in
/// @param[in,out] unit unit to place
/// @param host terrain queries
/// @return untyped_unit, with nothing changed, for a unit without a type; else none
StepFault update_height(oa::World& world, oa::Unit& unit, Host& host);
/// Tells whether a unit regenerates health this tick, under the match's
/// repair.healtime-self-heal rule.
///
/// Its type must heal (a nonzero heal time) and its health, sign-extended and
/// compared without sign, must be below the maximum. In 3.1c (cadence
/// every-8-ticks) it then regenerates when tick & 7 is 0; with cadence
/// healtime-mask, when tick & (heal time & 0xff) is 0, so 1 regenerates every
/// 2 ticks, 3 every 4, 7 every 8 and 15 every 16. With skip-under-construction
/// an unfinished unit does not regenerate.
///
/// @param rule the match's self-heal rule
/// @param heal_time the type's heal time (UnitDef.heal_time)
/// @param health the unit's health
/// @param maximum_health the type's maximum health
/// @param build_remaining the unit's build fraction left (Unit.build_remaining)
/// @param tick the game tick
/// @return true when it regenerates
/// @quirk An unfinished unit is one whose build fraction is not +0.0: a fraction
///        of -0.0 counts as unfinished.
[[nodiscard]] bool self_heal_due(
    const data::match_rules::RepairHealtimeSelfHeal& rule,
    int16_t heal_time,
    int16_t health,
    uint32_t maximum_health,
    float build_remaining,
    uint32_t tick
) noexcept;
/// Returns the work one regeneration step passes to the repair step.
///
/// (heal time × 8 × work-multiplier) / 30, truncated, as a float. In 3.1c the heal
/// time is read without sign and the multiplier is 1; with cadence healtime-mask
/// the heal time is read with its sign, the quotient truncated toward zero.
///
/// @param rule the match's self-heal rule
/// @param heal_time the type's heal time (UnitDef.heal_time)
/// @return the work, worker time per step
[[nodiscard]] float
self_heal_rate(const data::match_rules::RepairHealtimeSelfHeal& rule, int16_t heal_time) noexcept;
/// Runs one unit's tick.
///
/// In order: script, counters, selectability, the health percentage (every 30 ticks),
/// then for a unit simulated here environment damage, regeneration, both order
/// schedulers and movement with height correction, and last the death of a unit
/// flagged to die.
///
/// The health percentage is health * 100 / the type's maximum health, clamped
/// to 0..100. A type with no maximum health (MaxDamage 0), for which 3.1c has
/// no reading, takes a quotient of 0: such a unit reads 0%.
///
/// @param world world the unit lives in
/// @param[in,out] queue the unit's order lists
/// @param[in,out] unit unit to tick
/// @param host operations owned by other systems
/// @return untyped_unit when the unit's type is read and it has none, a fault
///         of the order schedulers, else none
StepFault update_unit(oa::World& world, OrderQueue& queue, oa::Unit& unit, Host& host);
/// Ticks every active player's units and the periodic viewpoint follow.
///
/// Call once per simulation tick with the game tick already incremented.
///
/// @param[in,out] world players, units and game fields; the active unit count is rebuilt
/// @param orders order lists indexed by unit slot
/// @param host operations owned by other systems
/// @return orders_short, with nothing done, when `orders` does not cover the
///         unit pool; the first fault of a unit's tick, which ends that unit's
///         update while the other units are still updated; else none
StepFault update_units(oa::World& world, std::span<OrderQueue> orders, Host& host);
} // namespace oa::sim::simulation_state
