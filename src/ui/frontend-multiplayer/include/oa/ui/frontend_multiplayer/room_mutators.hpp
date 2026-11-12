// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether the battle room may start, from the mutators each machine reports.
#pragma once

#include "oa/netgame/presence.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::frontend_multiplayer {

struct Lobby;

/// Whether one machine has one mutator of the room's list.
///
/// The values are the two-bit codes of a presence record's mutator states.
enum class MutatorHave : uint8_t {
    has,           ///< installed at the list's SHA-256 and version
    missing,       ///< not installed
    other_release, ///< another release is installed
    unknown,       ///< the list is not whole yet, or the record does not say
};

/// The name a room line shows for one mutator.
///
/// The installed name when this machine has one, else the catalogue's name,
/// else the id. An empty string is not a name.
///
/// @param installed the name of the copy installed here; empty when there is none
/// @param catalogue the catalogue's name; empty when none is known
/// @param id the mutator's id
/// @return the name to show
[[nodiscard]] std::string
mutator_shown_name(std::string_view installed, std::string_view catalogue, std::string_view id);

/// Why the host's START waits on mutators, or none when it does not.
enum class MutatorsHold : uint8_t {
    none,           ///< nothing about mutators holds START
    host_not_built, ///< the host's layer is not built yet
    host_failed,    ///< the host's build stopped with an error
    classic_player, ///< a seated 3.1c human or watcher, while mutators are on
    no_support,     ///< an OA player sent no mutator field
    receiving,      ///< an OA player is still taking the list, or names another one
    lacking,        ///< an OA player does not have a mutator of the list
    failed,         ///< an OA player's build stopped with an error
    building,       ///< an OA player has the list and has not built yet
    other_result,   ///< an OA player built a different layer
    other_sim_hash, ///< an OA player plays another sim hash
};

/// What the battle room last did with one mutator list.
enum class RoomMutatorPhase : uint8_t {
    none,      ///< this room plays no mutators
    receiving, ///< the host's list has not all arrived
    lacking,   ///< a mutator of the list is not installed here
    built,     ///< the list was built
    failed,    ///< the build stopped
};

/// Fills the mutators field one room phase sends, with no mutators in it.
///
/// Built carries `folded` and `index`. Any other phase leaves the layer
/// digest zero. A code list shorter than `total` marks the rest unknown.
///
/// @param phase the phase
/// @param digest the loadout digest the phase is for
/// @param folded the mutator digest, read only while built
/// @param index the mutator the phase names, or presence_mutator_index_none
/// @param total how many mutators the loadout names
/// @param held how many leading mutators the sender holds
/// @param codes one have-code per position; empty marks every position unknown
/// @param[out] out the field
/// @return false for none, or when total is outside 1 to 32
[[nodiscard]] bool lobby_room_mutator_field(
    RoomMutatorPhase phase,
    const netgame::PresenceDigest& digest,
    const netgame::PresenceDigest& folded,
    uint8_t index,
    uint8_t total,
    uint8_t held,
    std::span<const uint8_t> codes,
    netgame::PresenceMutators* out
);

/// Tells whether a slot is an Open Annihilation human or watcher of another machine.
///
/// Remote, playing, and carrying the engine signature. A 3.1c player and this
/// machine's own players are not.
///
/// @param lobby Lobby state.
/// @param player_slot Player slot.
/// @return True when that slot is another machine's Open Annihilation player.
[[nodiscard]] bool lobby_remote_oa_player(Lobby& lobby, int32_t player_slot) noexcept;

/// Tells whether this machine hosts.
///
/// @param lobby Lobby state.
/// @return True when the local player's block says host.
[[nodiscard]] bool lobby_hosts(Lobby& lobby) noexcept;

/// Why START waits, and the player and mutator it names.
struct MutatorsBlock {
    MutatorsHold hold{}; ///< none when START is not waiting on mutators
    int32_t slot{-1};    ///< the player it names; -1 when it names none
    /// The mutator it names, or presence_mutator_index_none.
    uint8_t position{netgame::presence_mutator_index_none};
};

/// Returns whether one slot has one mutator of the room's list.
///
/// This machine's players are read from its own record. A record with no
/// mutator field, or a position past that field's total, is unknown.
/// Allocates nothing.
///
/// @param lobby Lobby state.
/// @param slot Player slot, 0..9.
/// @param position The mutator's position, 0 to 31.
/// @return The two-bit code, or unknown when the record does not say.
[[nodiscard]] MutatorHave lobby_mutator_have(Lobby& lobby, int32_t slot, uint8_t position) noexcept;

/// Returns the first reason the host's START waits on mutators.
///
/// None when no record carries a mutator field. When the host's record has
/// none and an Open Annihilation peer's still does, START waits while that
/// peer drops it. Otherwise the host's own build, then each seated 3.1c
/// human or watcher in slot order, then each Open Annihilation human or
/// watcher of another machine: no mutator field, another list or a list
/// still arriving, a mutator they do not have, a failed build, a build not
/// finished, another layer, another sim hash. A layer digest that differs
/// names the earlier of the two dropped positions when those positions
/// differ. Reads the fixed part of each mutator field and the sim hash
/// only. Allocates nothing.
///
/// @param lobby Lobby state.
/// @return The first hold, or none.
[[nodiscard]] MutatorsBlock lobby_mutators_block(Lobby& lobby) noexcept;

/// Tells whether mutators allow the host to start.
///
/// @param lobby Lobby state.
/// @return True when nothing about mutators holds START.
[[nodiscard]] bool lobby_mutators_allow_start(Lobby& lobby) noexcept;

/// Remembers that START was pressed and mutators refused it.
///
/// The machine that pressed START asks "The game did not start" from this
/// record. A later press replaces it. The record is the block START saw.
///
/// @param block why START waits, and the player and mutator it names
void lobby_note_refused_start(const MutatorsBlock& block);

/// Returns the refused start, once.
///
/// @return the block, or nothing when START was not refused for mutators
[[nodiscard]] std::optional<MutatorsBlock> lobby_take_refused_start();

/// Slots of seated 3.1c humans and watchers, in slot order.
///
/// A player counts when its own setup block has arrived and lacks the
/// engine signature. The picker reads this before the host turns a list on.
/// START waits for these players (`classic_player`) only while the host's
/// record carries mutators.
///
/// @param lobby Lobby state.
/// @return The slots.
[[nodiscard]] std::vector<int32_t> lobby_classic_players(Lobby& lobby);

/// Posts one local line per seated 3.1c player when mutators turn on.
///
/// Looks at the host's record. The first time it carries a mutator field,
/// each machine posts "*** {player} plays Total Annihilation 3.1c, which
/// cannot play with mutators." for every seated 3.1c human or watcher, in
/// its own language, and not again until that field is gone and comes back.
/// A room without the field posts nothing and arms the next change. The
/// lines are not sent.
///
/// @param[in,out] lobby Lobby state.
void lobby_note_classic_room(Lobby& lobby) noexcept;

/// Posts one 3.1c line about a slot, split between words so each part fits.
///
/// Looks `words` up in the interface catalogue, fills `{player}` with the
/// slot's name, read as UTF-8, and posts each part of the sentence: with
/// Lobby::chat_measure bound, the lines fit_chat_line makes for the chat
/// list; each part within a stored chat line, and in the code page while
/// Unicode chat is off. The parts are not sent.
///
/// @param[in,out] lobby Lobby state.
/// @param words The English sentence, with `{player}`.
/// @param player_slot The player the line names.
void lobby_post_classic_line(Lobby& lobby, const char* words, int32_t player_slot) noexcept;

/// Posts one line already in the language shown, split as
/// lobby_post_classic_line splits its lines. The parts are not sent.
///
/// @param[in,out] lobby Lobby state.
/// @param line The line, UTF-8, every token filled.
void lobby_post_room_line(Lobby& lobby, std::string line) noexcept;

/// Tells whether a row's rules dot is on for mutators.
///
/// The host's own row does not, and neither does a computer player. A 3.1c
/// player holds START as `classic_player` while mutators are on, but it has
/// no dot. Another slot's problem does not hide this one. Allocates nothing.
///
/// @param lobby Lobby state.
/// @param slot Player slot, 0..9; any other gives false.
/// @return True when this slot's mutators put a dot on its row.
[[nodiscard]] bool lobby_mutators_differ_from_host(Lobby& lobby, int32_t slot) noexcept;

/// Logs a change of the host's mutator hold, including the first one seen.
///
/// The host logs `multiplayer: mutators hold START: <hold> slot <n>
/// position <p>` or `multiplayer: mutators allow START`. Another machine
/// logs nothing. The last hold is kept on the lobby, not in a function
/// local, so a test can reset it with the lobby.
///
/// @param[in,out] lobby Lobby state.
void lobby_log_mutators_hold(Lobby& lobby) noexcept;

} // namespace oa::ui::frontend_multiplayer
