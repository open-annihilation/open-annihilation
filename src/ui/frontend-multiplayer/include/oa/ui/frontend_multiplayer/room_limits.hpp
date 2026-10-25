// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether a battle room whose profile is past 3.1c's limits may start.
#pragma once

#include <cstdint>

namespace oa::ui::frontend_multiplayer {

struct Lobby;

/// Why START waits on a machine that cannot play this mod, or none.
enum class LimitsHold : uint8_t {
    none,           ///< nothing about the limits holds START
    classic_player, ///< a seated 3.1c human or watcher, while the room is OA-only
    older_build,    ///< a remote OA human or watcher whose record does not cover the room
};

/// The first limits hold, and the slot it names. Slot is -1 when nothing holds.
struct LimitsBlock {
    LimitsHold hold{LimitsHold::none};
    int32_t slot{-1};
};

/// Tells whether the room's profile can be played only on Open Annihilation.
///
/// Reads the profile's network rules, not the weapons or sides loaded.
///
/// @param lobby Lobby state.
/// @return True when weapon types pass 256 or sides pass five.
[[nodiscard]] bool lobby_room_oa_only(const Lobby& lobby) noexcept;

/// The line a room posts about a 3.1c player it cannot include.
///
/// The weapon-types line when the rules name more than 256 weapon types,
/// otherwise the five-sides line. Both raised: the weapon-types line.
///
/// @param lobby Lobby state.
/// @return The English catalogue line, with `{player}`.
[[nodiscard]] const char* lobby_limits_classic_line(const Lobby& lobby) noexcept;

/// The line a room posts about an Open Annihilation build that is too old.
///
/// @return The English catalogue line, with `{player}`.
[[nodiscard]] const char* lobby_limits_newer_line() noexcept;

/// The line the game list shows after this machine leaves a 3.1c host.
///
/// @return The English catalogue line. It has no `{player}`.
[[nodiscard]] const char* lobby_limits_leave_line() noexcept;

/// Tells whether a slot's presence record covers the room's limits.
///
/// A missing record, a record without the limits tag, or a tag naming fewer
/// weapon types or sides than the rules does not cover. Allocates nothing.
///
/// @param lobby Lobby state.
/// @param player_slot Player slot.
/// @return True when the tag names at least the rules' weapon types and sides.
[[nodiscard]] bool lobby_limits_covered(Lobby& lobby, int32_t player_slot) noexcept;

/// Tells whether a slot is a remote Open Annihilation human or watcher.
///
/// The block has to be that player's. A computer player is not one, and
/// neither is a block without the engine signature. Allocates nothing.
///
/// @param lobby Lobby state.
/// @param player_slot Player slot.
/// @return True when that seat is a remote OA human or watcher.
[[nodiscard]] bool lobby_limits_remote_oa(Lobby& lobby, int32_t player_slot) noexcept;

/// The first reason START waits on a machine this mod cannot include.
///
/// In slot order: a seated remote 3.1c human or watcher while the room is
/// OA-only, else a remote OA human or watcher whose record does not cover
/// the rules. A stock room holds nothing. Allocates nothing.
///
/// @param lobby Lobby state.
/// @return The hold, or none.
[[nodiscard]] LimitsBlock lobby_limits_block(Lobby& lobby) noexcept;

/// Tells whether START may proceed as far as these limits are concerned.
///
/// @param lobby Lobby state.
/// @return True when nothing about the limits holds START. Allocates nothing.
[[nodiscard]] bool lobby_limits_allow_start(Lobby& lobby) noexcept;

/// Posts one limits line on this machine and writes it whole to the log.
///
/// Looks `words` up in the interface catalogue, fills `{player}` when
/// `player_slot` names a seat, writes that one line to the log, and posts
/// it the way the room posts a 3.1c line. A negative slot writes the line
/// as it is and posts nothing: the sentence shown after leaving a 3.1c host.
/// The line is not sent.
///
/// @param lobby Lobby state.
/// @param words The English sentence.
/// @param player_slot The player the line names, or a negative value for none.
void lobby_post_limits_line(Lobby& lobby, const char* words, int32_t player_slot) noexcept;

} // namespace oa::ui::frontend_multiplayer
