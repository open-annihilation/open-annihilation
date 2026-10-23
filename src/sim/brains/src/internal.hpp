// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The seat a match keeps behind seat_state. Not part of the public header.
#pragma once

#include "oa/sim/brains/seat.hpp"

#include "oa/core/player.h"
#include "oa/data/brain_facts.hpp"
#include "oa/data/script_host/memory_sources.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

struct lua_State;

namespace oa::sim::brains {

namespace script_host = oa::data::script_host;

/// Most bytes of an entry path, not counting the terminating NUL.
inline constexpr std::size_t path_bytes = 256;
/// Most bytes of an option id, not counting the terminating NUL.
inline constexpr std::size_t option_id_bytes = 64;
/// Most bytes of an option's text, not counting the terminating NUL.
inline constexpr std::size_t option_text_bytes = 64;
/// Most options one seat copies. A manifest offers no more.
inline constexpr std::size_t most_options = 16;
/// Most bytes stored of a refusal or a fits reason, not counting the NUL.
inline constexpr std::size_t reason_limit = 255;

/// One option copied into the seat, so the script can read it after the call.
struct StoredOption {
    char id[option_id_bytes + 1]{};
    OptionKind kind{};
    bool toggle{};
    double number{};
    char text[option_text_bytes + 1]{};
};

/// One seated brain. The sources are declared before the sandbox, so the
/// sandbox is destroyed first and never reads the sources after they go.
struct Seat {
    uint8_t player{};
    char id[id_bytes + 1]{};
    char level[level_bytes + 1]{};
    char difficulty[8]{};
    bool full_awareness{};
    StoredOption options[most_options]{};
    uint8_t option_count{};
    bool has_fits{};
    bool has_start{};
    /// Empty registry reference until the entry's table is stored.
    int hooks_ref{-2};
    /// The match this seat plays in. Null for a load check.
    match_runtime::Match* match{};
    /// The facts `game` is built from. Null reads a default game.
    const oa::data::brain_facts::GameFacts* facts{};
    /// Registry reference of `game`'s hidden table. -2 is LUA_NOREF.
    int game_hidden_ref{-2};
    script_host::MemorySources sources{};
    std::optional<script_host::Sandbox> sandbox{};
    /// The match this seat plays, or null when a brain is checked with none.
    match_runtime::Match* match{};
    /// True when the seat was made after the built-in player's set-up had run.
    bool handed_over{};
    /// Assignments `me:assign` refused because the unit was not the seat's.
    uint32_t refused_assignments{};
};

/// The brains seated in one match, one slot per player.
struct Brains {
    /// Facts every seat made after they were stored reads. A default game until then.
    oa::data::brain_facts::GameFacts facts{};
    std::array<std::unique_ptr<Seat>, OA_PLAYER_COUNT> seats{};
};

/// Writes a refusal or a fits reason, cut to 255 bytes.
///
/// @param reason the outcome's text
/// @param text the sentence
void write_reason(std::array<char, 256>& reason, std::string_view text);

/// Tells whether the package texts, the entry and the networks are acceptable.
///
/// @param spec the package
/// @param outcome set when they are not
/// @return false when the package is refused
bool package_acceptable(const SeatSpec& spec, SeatOutcome& outcome);

/// Copies the spec into the seat.
///
/// @param seat the seat
/// @param spec the package, the level and the options
/// @return false when the sources cannot be stored
bool copy_spec_into(Seat& seat, const SeatSpec& spec);

/// Opens the sandbox, loads the entry and runs fits.
///
/// On success the sandbox stays open and start has not been called. On
/// failure `outcome` is set and the sandbox is closed. The seat's match is
/// not read.
///
/// @param seat the seat, with its facts already set
/// @param spec the package
/// @param outcome set when the load or fits fails
/// @return true when the brain fits
bool open_and_fit(Seat& seat, const SeatSpec& spec, SeatOutcome& outcome);

/// Opens the libraries a brain may call, in a fixed order.
///
/// Later tickets add one call here and nowhere else. Every installer has to
/// work when the seat has no match: this one reads only the texts copied
/// onto the seat.
///
/// @param state the sandbox state
/// @param seat the seat being opened
void install_libraries(lua_State* state, Seat& seat);

/// Returns the profile a brain runs under.
///
/// @param seat the seat, whose address the profile keeps for its install
/// @param bank the per-tick bank; the app passes the defaults
/// @return the brain profile
[[nodiscard]] script_host::Profile brain_profile(Seat& seat, const script_host::Bank& bank);

} // namespace oa::sim::brains
