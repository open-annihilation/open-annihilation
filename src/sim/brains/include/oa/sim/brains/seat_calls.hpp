// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One seat's calls into the match: the built-in player's host, plus the few
// calls it makes beside that host. A brain's Lua reaches the match only
// through this table.
#pragma once

#include "oa/sim/ai.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {
class Match;
}

namespace oa::sim::brains {

/// The calls one seat may make.
///
/// `host` functions are invoked as `host.fn(host.context, ...)`. Every other
/// function is invoked as `fn(&host, ...)`, so its context is the address of
/// this table's `host`. A null function is not called.
struct SeatCalls {
    /// The built-in player's boundary. `context` is whatever filled the host.
    oa::sim::ai::ComputerHost host{};

    /// The squad a unit stands in, or -1 when it has none.
    ///
    /// @param context the address of `host`
    /// @param slot unit slot
    /// @return `Unit.squad`
    int32_t (*unit_squad)(void* context, uint16_t slot){};

    /// The nearest enemy unit, as the built-in player's search ranks them.
    ///
    /// @param context the address of `host`
    /// @param player the seat
    /// @param x reference X, 16.16
    /// @param z reference Z, 16.16
    /// @param widened pass over widened occupancy and move-rate states
    /// @param skip_submerged pass over a fully submerged unit
    /// @param out_slot receives the unit slot when one is found
    /// @return true when a unit was found
    bool (*nearest_enemy)(
        void* context,
        uint8_t player,
        int32_t x,
        int32_t z,
        bool widened,
        bool skip_submerged,
        uint16_t* out_slot
    ){};

    /// Writes standing move and fire. A negative value leaves that field.
    ///
    /// @param context the address of `host`
    /// @param slot unit slot
    /// @param move 0 hold, 1 manoeuvre, 2 roam, or negative to leave it
    /// @param fire 0 hold, 1 return, 2 at will, or negative to leave it
    void (*write_stance)(void* context, uint16_t slot, int32_t move, int32_t fire){};

    /// Marks a section. In a game this does nothing.
    ///
    /// @param context the address of `host`
    /// @param name the section name; already checked
    void (*section)(void* context, const char* name){};

    /// Kind byte of the primary order's head, or 0 when the queue is empty.
    ///
    /// @param context the address of `host`
    /// @param slot unit slot
    /// @return the order kind
    uint8_t (*primary_kind)(void* context, uint16_t slot){};

    /// A fog-free site check at a facing other than south.
    ///
    /// @param context the address of `host`
    /// @param type unit type
    /// @param cell_x top-left cell x
    /// @param cell_z top-left cell z
    /// @param facing quarter turns from south, 1 to 3
    /// @param yard true for the yard, false for the site
    /// @return true when the site is clear
    bool (*site_facing)(
        void* context, uint16_t type, int32_t cell_x, int32_t cell_z, int32_t facing, bool yard
    ){};

    /// A site check that still has fog, through the placing player's test.
    ///
    /// @param context the address of `host`
    /// @param player the placing player
    /// @param type unit type
    /// @param cell_x top-left cell x
    /// @param cell_z top-left cell z
    /// @param facing quarter turns from south
    /// @param yard true for the yard; the caller has already tested the footprint
    /// @return true when the site is accepted
    bool (*site_fogged)(
        void* context,
        uint8_t player,
        uint16_t type,
        int32_t cell_x,
        int32_t cell_z,
        int32_t facing,
        bool yard
    ){};

    /// A queued attack on a unit. The unqueued attack is `host.order_attack`.
    ///
    /// @param context the address of `host`
    /// @param slot the attacker
    /// @param target the target slot
    /// @return true when the order was kept
    bool (*attack_queued)(void* context, uint16_t slot, uint16_t target){};

    /// A build whose facing is not south, or that is queued.
    ///
    /// @param context the address of `host`
    /// @param slot the builder
    /// @param type the building type
    /// @param at the site, 16.16
    /// @param queued true to queue it
    /// @param facing quarter turns from south
    /// @return true when the order was kept
    bool (*build_shaped)(
        void* context,
        uint16_t slot,
        uint16_t type,
        const oa::FixedVec3* at,
        bool queued,
        int32_t facing
    ){};

    /// Whether a weapon other than the first reaches a point.
    ///
    /// @param context the address of `host`
    /// @param unit the shooter
    /// @param at the point, 16.16
    /// @param weapon weapon slot, 0 to 2
    /// @return true when the weapon reaches
    bool (*weapon_reaches_slot)(
        void* context, uint16_t unit, const oa::FixedVec3* at, uint8_t weapon
    ){};

    /// Records an order this table does not play. Null on a seat that plays.
    ///
    /// @param context the address of `host`
    /// @param name the order's name
    /// @param slot the unit, or 0 for a line that is not a unit order
    void (*refuse)(void* context, const char* name, uint16_t slot){};

    /// The seat's built-in knowledge record, and the tick that record was last refreshed.
    ///
    /// Null when the player has no record. `me.knowledge` stays nil while this
    /// is null, and the call is not made through a null match: a match installs
    /// it only with the match as `host.context`, and a seat outside a match
    /// leaves it null unless a test supplies a record of its own.
    ///
    /// @param context the address of `host`
    /// @param player the seat
    /// @param refreshed_tick receives the sightings refresh tick when a record is returned
    /// @return the record, or null when the player has none
    const sim::ai::ComputerKnowledge* (*knowledge)(
        void* context, uint8_t player, uint32_t* refreshed_tick
    ){};

    /// The tick until which a capturing builder holds its build orders.
    ///
    /// 0 is a real tick: construction compares the match tick with `<`, so a
    /// hold of 0 never waits. Null when the seat has no match. `me.builds_held_until`
    /// stays nil while this is null, and the call is not made through a null
    /// match: a match installs it only with the match as `host.context`.
    ///
    /// @param context the address of `host`
    /// @param player the seat
    /// @return `commander_build_tick` for that player, or 0 when the player is out of range
    uint32_t (*builds_held_until)(void* context, uint8_t player){};

    /// Build-list runs for a seat that has no match: a count and that many type
    /// ids, from type 0. Null when the match holds the runs, and null outside a
    /// match that was not given a list.
    const uint16_t* build_runs{};
    /// The number of `uint16_t` values in `build_runs`.
    uint32_t build_run_length{};
    /// Entries a builder's list keeps, including the one the download menus add.
    /// 0 selects the match's own limit, or the longest list when `build_runs` is set.
    uint32_t build_kept{};
};

/// The call table of a match: `match_computer_host` and the calls beside it.
///
/// `section` does nothing. `refuse` is null.
///
/// @param match the match
/// @return the seat's calls
[[nodiscard]] SeatCalls seat_calls_for_match(match_runtime::Match& match);

/// The call table of a host that already speaks for a world, for tests.
///
/// Squad, stance and the nearest enemy read `host.world`. A kind read returns
/// 0. A facing or a fogged site uses the host's own site checks.
///
/// @param host a host whose `world` outlives the table
/// @return the seat's calls
[[nodiscard]] SeatCalls seat_calls_from_computer_host(const sim::ai::ComputerHost& host);

} // namespace oa::sim::brains
