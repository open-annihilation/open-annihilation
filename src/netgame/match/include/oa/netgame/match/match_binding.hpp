// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Binds a networked match to the offline match runtime: the replication
// callbacks call Match's public simulation entry points, the match's
// network hooks send through NetMatch (among them the deaths of units
// simulated here, the feature records, interceptions, carry links and
// finished units), and one call runs a networked tick in the game's order
// (tick, pump, simulate and send 0x2c, sharing, paced flush).

#include "oa/sim/match_runtime.hpp"
#include "oa/netgame/match/net_match.hpp"
#include "oa/netgame/match/sim_records.hpp"

#include <vector>

namespace oa::netgame::match {

// Verification observer: called after a detached full unit record has moved
// its unit, with the position the unit held before the record.
struct FullRecordProbe {
    void* context{};
    void (*placed)(void* context, const Unit& unit, const FixedVec3& before, const FixedVec3& to){};
};

struct MatchBinding {
    sim::match_runtime::Match* match{};
    NetMatch* net{};
    std::vector<MovementRecord> movement; // indexed by unit slot
    uint32_t created_remote{};
    uint32_t refused_creates{};
    uint32_t creates_past_table{}; // a definition index this unit table does not reach
    FullRecordProbe full_record_probe{};
    FeatureRecordLink
        features{}; // where the match's feature hooks send; filled by match_binding_install
};

/// Prepares a binding for a match without moving its unit ranges.
///
/// Sizes the per-unit movement records and clears the counters. The match's
/// unit ids and per-player views stay as they are, so a skirmish can send
/// through the binding without being rebound as a network game.
///
/// @param[out] binding Binding to prepare.
/// @param match Offline match runtime the binding reads; its ranges are not changed.
/// @param net Multiplayer match state the binding sends through.
void match_binding_prepare(MatchBinding* binding, sim::match_runtime::Match* match, NetMatch* net);

/// Prepares a binding for a match after match_launch_apply.
///
/// Calls match_binding_prepare, then assigns the multiplayer unit ranges and
/// rebinds the match's per-player views.
///
/// @param[out] binding Binding to initialise.
/// @param match Offline match runtime to drive; its per-player ranges are rebound.
/// @param net Multiplayer match state the binding sends through.
void match_binding_init(MatchBinding* binding, sim::match_runtime::Match* match, NetMatch* net);

/// Sends one local player's unit state (0x2c) and copies the sent route bits back.
///
/// The body of the binding's local_player_ticked hook. A recording that must
/// not move the match calls it and then puts the route bits and the player's
/// last tick back.
///
/// @param context The MatchBinding.
/// @param player The local player that ticked.
void match_binding_send_local_tick(void* context, Player& player);

/// Returns the replication callbacks over the bound match.
///
/// A received 0x14 creates the unit for its new owner with the build
/// progress, health, orientation and stockpiles the record carries
/// (Match::transfer_unit); the old unit dies when its owner's 0x0c arrives.
///
/// @param binding Binding passed back to every callback as its context.
/// @return Callbacks that apply received records through the match runtime.
[[nodiscard]] ReplicationSim match_binding_sim(MatchBinding* binding) noexcept;

/// Returns the NetMatch callbacks over the bound match and the canonical World.
///
/// Chat, notice and random callbacks are left for the caller to fill.
///
/// @param binding Binding passed back to every callback as its context.
/// @return Hooks with credit, debit, destroy_player_units, end_local_game and local_player_won set.
[[nodiscard]] NetMatchHooks match_binding_hooks(MatchBinding* binding) noexcept;

/// Installs the match's multiplayer hooks so its tick sends through NetMatch.
///
/// Besides the per-tick records, a unit simulated here that dies is shared
/// as a 0x0c record, weapon hits on features and feature changes (a
/// resurrected wreck among them) go out as 0x0f records through the
/// binding's FeatureRecordLink, a shot an interceptor set off here as two
/// 0x0e records, every carry link the match accepts as a 0x0a record, a
/// finished unit simulated here as a 0x12 record, and a unit simulated here
/// handed to a player another machine simulates as a 0x14 record from the
/// unit's owner (unit_transfer_record), ahead of its 0x0c.
///
/// @param[in,out] binding Binding whose match receives the hooks and whose feature link is filled.
void match_binding_install(MatchBinding* binding) noexcept;

/// Runs one multiplayer simulation tick in the game's order.
///
/// Advances Game.tick, pumps received records, simulates the tick, shares
/// economy every 30 ticks and runs the after-tick sends. While the pause bit
/// of Game.sim_run_flags is set it only pumps: the tick does not move. A
/// pause that arrives in the pump takes effect from the next call.
///
/// @param[in,out] binding Bound match to advance.
void match_binding_tick(MatchBinding* binding);

/// Runs the commander rule's sweep for a departing player (Match::destroy_player_units).
///
/// @param[in,out] binding Bound match.
/// @param slot Player slot, 0..9; others are ignored.
void match_binding_destroy_player_units(MatchBinding* binding, uint8_t slot) noexcept;

/// Ends the local player's game as a defeat (Match::end_local_game).
///
/// @param[in,out] binding Bound match.
void match_binding_end_local_game(MatchBinding* binding) noexcept;

/// Lets the bound match follow a player's alliance row (Match::follow_player_alliances).
///
/// @param[in,out] binding Bound match.
/// @param slot Player slot, 0..9; others are ignored.
void match_binding_follow_alliances(MatchBinding* binding, uint8_t slot) noexcept;

/// Gives one player every cell another has mapped (Match::share_mapped_area).
///
/// @param[in,out] binding Bound match.
/// @param from Player slot sharing its map, 0..9.
/// @param to Player slot receiving it, 0..9.
void match_binding_share_sight(MatchBinding* binding, uint8_t from, uint8_t to) noexcept;

/// Tells whether the bound match's local player has won.
///
/// @param binding Bound match.
/// @return True once Match::outcome() reports victory.
[[nodiscard]] bool match_binding_local_player_won(const MatchBinding* binding) noexcept;

/// Credits a resource to a player's economy staging block.
///
/// @param[in,out] world World holding the player's economy.
/// @param player Player slot, 0..9; others are ignored.
/// @param metal True for metal, false for energy.
/// @param amount Amount produced.
/// @quirk Easy and medium computer players receive the amount halved or scaled, as in 3.1c.
void credit_player_resource(World* world, uint8_t player, bool metal, float amount) noexcept;

/// Takes an amount from a player's store into its staging block when the store covers it.
///
/// @param[in,out] world World holding the player's economy.
/// @param player Player slot, 0..9.
/// @param metal True for metal, false for energy.
/// @param amount Amount requested.
/// @return True when the amount was paid; false when the store is short or the slot is invalid.
bool debit_player_resource(World* world, uint8_t player, bool metal, float amount) noexcept;

} // namespace oa::netgame::match
