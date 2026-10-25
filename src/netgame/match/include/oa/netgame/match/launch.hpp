// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Leaving the battle room for a multiplayer match: the loader copies the
// lobby's slot table and player-info blocks into the match and derives the
// game options from the host's block. The random seed is per machine.

#include "oa/core/world.h"
#include "oa/ui/frontend_multiplayer/lobby.hpp"

namespace oa::netgame::match {

/// Returns the seed argument the loader passes to the random seeder.
///
/// Each machine seeds from its own counter; nothing about it is exchanged.
///
/// @param performance_counter The local high-resolution performance counter.
/// @return The counter's high and low 32-bit halves added, wrapping.
[[nodiscard]] uint32_t launch_random_seed(uint64_t performance_counter) noexcept;

/// Returns the width of a unit-definition index in the 0x2c stream.
///
/// @param count Number of unit definitions.
/// @return The bit length of count; 0 for 0.
[[nodiscard]] int32_t unit_def_id_bits_for_count(uint32_t count) noexcept;

/// Finds the info block of the in-use slot flagged host (role bit 0).
///
/// @param world Match world whose player records are searched.
/// @return The block, or null when no in-use slot is the host.
[[nodiscard]] const PlayerSetupInfo* launch_host_info(World* world) noexcept;

/// Copies the lobby into the match World before its first tick.
///
/// Copies the ten Player records (slot, id, status, name, team, machine
/// group, alliances) and their info blocks, the local slot, the number of
/// seated players (Game.player_count, which the path search shares its
/// credit by), under an active launch the options lock the battle room set from it
/// (Game.setup_options bit 0, set or clear), and from the host's block the
/// commander mode (Game.session_rules) and the mapping/line-of-sight bits
/// (Game.visibility_flags); then sets the definition-index width, resets
/// Game.tick and sets the live-game flag.
/// Free slots get player id no_player_id so they sort last when unit ranges
/// are numbered.
///
/// @param lobby Battle room state being left; its game must name a local slot.
/// @param[in,out] world Match world; its unit pool must already be sized from the host's max_units.
/// @return False, changing nothing, when the lobby has no game or no local slot.
bool match_launch_apply(ui::frontend_multiplayer::Lobby& lobby, World* world) noexcept;

/// Applies the host's game options: the commander mode (Game.session_rules) and the
/// mapping/line-of-sight bits (Game.visibility_flags).
///
/// @param[in,out] world Match world whose Game is updated.
/// @param host The host's lobby info block.
void match_apply_host_options(World* world, const PlayerSetupInfo& host) noexcept;

/// Tells whether a slot's lobby block marks it as watching (no commander, no start position, never defeated).
///
/// @param world Match world, or null.
/// @param slot Player slot, 0..9.
/// @return True when the slot is in use and its info block has the watcher option.
[[nodiscard]] bool match_slot_watcher(const World* world, uint8_t slot) noexcept;

/// Finds the first slot this machine would place a commander for on a side the match does not have.
///
/// Slot order. A slot counts when it is in use, its player is local or a
/// computer, and it is not watching. Its info block's side is at or past
/// Game.side_count. Nothing is found when world is null, when the match's
/// side count is 0, or when no such slot is seated.
///
/// @param world Match world, or null.
/// @return The slot, or OA_PLAYER_COUNT when there is none.
[[nodiscard]] uint8_t match_first_side_past_table(const World* world) noexcept;

/// Lets a local watcher see the whole map.
///
/// Once the commanders stand, the launch turns the host's mapping and
/// line-of-sight rules off for a watching local slot and keeps the
/// line-of-sight type. Other machines keep the host's rules.
///
/// @param[in,out] world Match world whose Game.visibility_flags may change.
void match_apply_watcher_view(World* world) noexcept;

/// Shows the local view the whole map, as a watcher sees it: turns the
/// host's mapping and line-of-sight rules off and keeps the line-of-sight
/// type. match_apply_watcher_view does this for a watching local slot; a
/// replay that leaves the viewer no slot of its own does it for the viewer.
///
/// @param[in,out] world Match world whose Game.visibility_flags change.
void match_show_whole_map(World* world) noexcept;

/// Returns the player count the multiplayer map schema is chosen for.
///
/// @param game Game holding the host's slot table (Game.slot_table, record 0x26).
/// @return One past the highest slot the slot table fills, else Game.player_count.
[[nodiscard]] int32_t match_layout_player_count(const Game& game) noexcept;

/// Gives every player its units_per_player range in ascending player-id order.
///
/// Every machine thus numbers units alike whatever its slot order. Each
/// range's units get their owner and owner index and no squad; a player
/// whose range does not fit the unit pool gets none.
///
/// @param[in,out] world Match world whose players and units are updated.
void match_assign_unit_ranges(World* world) noexcept;

} // namespace oa::netgame::match
