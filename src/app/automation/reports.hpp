// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the automation endpoint reports of a match and of the battle room,
// as the match and room requests answer (serve_reports.hpp): the match is
// read from its world, as the automation host gives it, and the battle room
// from the multiplayer screens' own lobby; both are only read.
#pragma once

#include "oa/formats/json.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa {
struct Game;
struct Player;
struct World;
} // namespace oa

namespace oa::ui::frontend_multiplayer {
struct Lobby;
}

namespace oa::app::automation {

// A source can include more than one of these headers, so each name is declared once.
#ifndef OA_APP_AUTOMATION_USING_JSON_WRITER
#define OA_APP_AUTOMATION_USING_JSON_WRITER
using oa::formats::json::JsonWriter;
#endif

/// Whose economy a match answer includes.
enum class EconomyView : uint8_t {
    player, ///< the local player, and each player allied with it
    all,    ///< every seated player
};

/// Writes a match's members of the match answer: whether a match runs, its
/// tick, pause and speed, its unit limit and clock while it runs, its
/// players, the local player, and its outcome.
///
/// Each player's economy is written for the local player and for a player
/// allied with it. `shown` all writes every seated player's economy. An
/// enemy's economy is null. The rates are the resource bar's per-second
/// figures. A call that omits `shown` keeps the player's view.
///
/// @param[in,out] json the answer's object, open
/// @param match the running match's world; null while no match runs
/// @param digest the match's saved-state digest, written as state_hash; nothing writes none
/// @param shown whose economy is written
void write_match(
    JsonWriter& json,
    const oa::World* match,
    std::optional<uint64_t> digest,
    EconomyView shown = EconomyView::player
);

/// Writes the map a running match plays: its name, width and height.
///
/// The name is the setup's map name, null when that field is empty.
///
/// @param[in,out] json the answer being written
/// @param match the running match
void write_map(JsonWriter& json, const oa::World& match);

/// Writes the rules a running match plays, into an object that is already open:
/// commander, line of sight, mapping, starting metal and energy, and the unit limit.
///
/// The words are the battle room's. The values are the session's, which is
/// what the match plays.
///
/// @param[in,out] json the rules object, open
/// @param match the running match
void write_rule_fields(JsonWriter& json, const oa::World& match);

/// Writes each seated player's side and team.
///
/// @param[in,out] json the answer being written
/// @param match the running match
void write_seated_sides(JsonWriter& json, const oa::World& match);

/// Writes the battle room's members of the room answer: whether the room
/// shows, the game's name, its host, its players, its map, the host's
/// options and the room's chat.
///
/// @param[in,out] json the answer's object, open
/// @param lobby the multiplayer screens' lobby, which is only read; null for none
/// @param in_room the battle room shows
void write_room(JsonWriter& json, oa::ui::frontend_multiplayer::Lobby* lobby, bool in_room);

/// Returns the text of one of the game's fixed text fields: its bytes up to
/// the first zero byte, as they are, in the game's code page. The JSON
/// writer writes text that is not UTF-8 as unicode_text (json.hpp) gives it.
///
/// @param bytes the field
/// @param size the field's size in bytes
/// @return the text
[[nodiscard]] std::string game_text(const char* bytes, size_t size);

/// Returns a player's name, as its record holds it.
///
/// @param player the player
/// @return the name, up to its end or the record's 30 bytes
[[nodiscard]] std::string_view player_name(const oa::Player& player) noexcept;

/// Tells whether a match's player record holds a player taking part: a
/// player of this machine, a computer player or one of another machine.
///
/// @param player the record
/// @return true for such a player
[[nodiscard]] bool player_seated(const oa::Player& player) noexcept;

/// Tells whether a match's local player has its outcome: won or lost.
///
/// @param game the match's Game block
/// @return true once the match is decided for the local player
[[nodiscard]] bool match_decided(const oa::Game& game) noexcept;

/// Returns the local player's outcome, once the match is decided for it.
///
/// @param game the match's Game block
/// @return "victory" or "defeat"; nothing while the match goes on
[[nodiscard]] std::optional<std::string_view> match_outcome(const oa::Game& game) noexcept;

/// Returns the slot of a match's winner, once the match is decided.
///
/// @param game the match's Game block
/// @return the local player when it won; otherwise the one player still
///         with units, when there is one; nothing else
[[nodiscard]] std::optional<uint8_t> match_winner(const oa::Game& game) noexcept;

/// One seat of a finished match, as every machine reports it.
///
/// The outcome is won, lost, drawn, left or watcher. It is computed from
/// the shared simulation, not from this machine's own victory or defeat.
struct SeatResult {
    uint8_t slot{};              ///< the seat, 0 to 9
    std::string name;            ///< the player's name
    std::optional<uint8_t> team; ///< the team, when the player has one
    uint8_t side{};              ///< the side's index, as the match answer's side
    std::string outcome;         ///< won, lost, drawn, left or watcher
    uint16_t units{};            ///< units still on the field
    int16_t kills{};             ///< units destroyed
    int16_t losses{};            ///< units lost
};

/// How a match ended for every seat, the same on every machine.
struct MatchResults {
    std::vector<SeatResult> seats;     ///< one entry per seat that took part
    std::optional<std::string> winner; ///< the one player still with units, or nothing
    uint32_t tick{};                   ///< the match's tick
};

/// Computes how the match ended for each seat, from the world alone.
///
/// A seat is listed when it is in play, when it left, or when it produced
/// units. A watcher is a watcher. Anyone who left is left. The rest are
/// grouped by team and by a mutual alliance: when one group still has units
/// its members won, including a teammate with none left, and the others
/// lost; when none or more than one group still has units, they drew. The
/// winner is the one of them who still has units, and nothing when several
/// do or none does.
///
/// @param world the match
/// @return the seats, the winner and the tick
[[nodiscard]] MatchResults match_results(const oa::World& world);

/// Writes a match's results, and its tick, into an open object.
///
/// `winner` is written only when `write_winner` is set. A game_over event
/// already has its own winner, this machine's, and keeps it.
///
/// @param[in,out] json the object, open
/// @param results the match's results
/// @param write_winner also write winner
void write_result_fields(JsonWriter& json, const MatchResults& results, bool write_winner);

} // namespace oa::app::automation
