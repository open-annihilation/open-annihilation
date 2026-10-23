// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The seat a match keeps behind seat_state. Not part of the public header.
#pragma once

#include "sight.hpp"

#include "oa/sim/brains/events.hpp"
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

/// One seat's plain event buffer. The seat owns `items`.
struct EventBuffer {
    /// The records. Null until the first event grows the buffer.
    Event* items{};
    /// How many records are waiting.
    uint32_t count{};
    /// How many records `items` can hold.
    uint32_t capacity{};
};

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
    /// True when the seat was made after the built-in player's set-up had run.
    bool handed_over{};
    /// Assignments `me:assign` refused because the unit was not the seat's.
    uint32_t refused_assignments{};
    /// Events recorded since the last turn. Hooks that fire during the turn
    /// land here and wait until the next turn.
    EventBuffer incoming{};
    /// Events the turn in progress is delivering.
    EventBuffer delivering{};
    /// Events dropped because the incoming buffer was full or could not grow.
    uint32_t dropped{};
    /// True after a banked call failed. Later turns do nothing.
    bool stopped{};
    /// True once a turn has stored alliance and participation.
    bool diplomacy_known{};
    /// Last delivered `Match::allied(seat, player)`, one entry per player.
    bool allied_with[OA_PLAYER_COUNT]{};
    /// Last delivered participation, one entry per player.
    bool participating[OA_PLAYER_COUNT]{};
    /// Radar contacts delivered last. The next picture is written into the work list.
    std::unique_ptr<ContactEntry[]> delivered_contacts;
    std::unique_ptr<ContactEntry[]> contact_work;
    /// How many contacts `delivered_contacts` holds.
    uint32_t delivered_contact_count{};
    /// Slots both lists can hold. Zero until the first picture is read.
    uint32_t contact_capacity{};
    /// `player_contacts_tick` of the delivered picture.
    uint32_t delivered_contacts_tick{};
    /// False until the first picture has been compared, even when that tick is 0.
    bool contacts_compared{};

    /// Releases the two event buffers.
    ~Seat() {
        delete[] incoming.items;
        delete[] delivering.items;
    }
};

/// The brains seated in one match, one slot per player.
struct Brains {
    /// Facts every seat made after they were stored reads. A default game until then.
    oa::data::brain_facts::GameFacts facts{};
    std::array<std::unique_ptr<Seat>, OA_PLAYER_COUNT> seats{};
    /// The match these seats play. Set when the first listener is registered.
    match_runtime::Match* match{};
    /// Listener id, or 0 when the match could not take another listener.
    uint32_t listener_id{};
    /// True once registration has been attempted. It is not tried again.
    bool listener_tried{};
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

/// Listens to the match's events once, when the first brain is seated.
///
/// The listener is not removed. It and the brains both end with the match.
///
/// @param brains the match's brains
/// @param match the match
void ensure_listener(Brains& brains, match_runtime::Match& match);

/// Delivers one seated turn and returns true.
///
/// Returns false when the seat has no running brain, so the built-in computer
/// player takes the turn. A stopped seat returns true and makes no call.
///
/// @param context the seat
/// @param match the match
/// @param player the seat's player
/// @return false when the built-in player should take the turn
bool deliver_turn(void* context, match_runtime::Match& match, uint8_t player);

} // namespace oa::sim::brains
