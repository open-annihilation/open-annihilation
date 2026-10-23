// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The seat a match keeps behind seat_state. Not part of the public header.
#pragma once

#include "oa/sim/brains/chat.hpp"
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
#include <string_view>

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
/// Most timers one brain may have waiting.
inline constexpr uint16_t max_timers = 1024;
/// Registry reference that means a timer holds no function. Equal to LUA_NOREF.
inline constexpr int timer_ref_empty = -2;

/// One function waiting to run on a later turn.
struct WaitingTimer {
    /// Registry reference of the function, or `timer_ref_empty`.
    int function_ref{timer_ref_empty};
    /// The tick this timer next runs.
    int64_t due{};
    /// Ticks between runs. Zero means the timer runs once.
    int32_t period{};
    /// The id `brain.cancel` takes. It only grows.
    uint32_t sequence{};
    /// True while the timer is waiting.
    bool waiting{};
};

/// One seat's plain event buffer. The seat owns `items`.
struct EventBuffer {
    /// The records. Null until the first event grows the buffer.
    Event* items{};
    /// How many records are waiting.
    uint32_t count{};
    /// How many records `items` can hold.
    uint32_t capacity{};
};

/// Most chat lines waiting for one turn.
inline constexpr uint8_t max_chat_lines = 32;
/// Most bytes stored of one chat line.
inline constexpr std::size_t chat_line_bytes = 256;

/// One chat line waiting for a seat's turn. The words are not terminated.
struct ChatLine {
    uint8_t from{};
    uint8_t mode{};
    uint16_t length{};
    std::array<char, chat_line_bytes> words{};
};

/// Lines waiting for one turn. `count` is how many `lines` are filled.
struct ChatQueue {
    std::array<ChatLine, max_chat_lines> lines{};
    uint8_t count{};
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
    /// Chat lines heard since the last turn. A line heard during the turn
    /// lands here and waits until the next turn.
    ChatQueue chat_incoming{};
    /// Chat lines the turn in progress is delivering.
    ChatQueue chat_delivering{};
    /// Chat lines dropped because the incoming queue already held 32.
    uint32_t dropped_chat{};
    /// True after a banked call failed. Later turns do nothing.
    bool stopped{};
    /// True once a turn has stored alliance and participation.
    bool diplomacy_known{};
    /// Last delivered `Match::allied(seat, player)`, one entry per player.
    bool allied_with[OA_PLAYER_COUNT]{};
    /// Last delivered participation, one entry per player.
    bool participating[OA_PLAYER_COUNT]{};
    /// True when this seat's files match the Classic fingerprint.
    bool classic{};
    /// Timers waiting to run on a later turn.
    WaitingTimer timers[max_timers]{};
    /// The next timer id. It starts at 1 and only grows.
    uint32_t next_timer_sequence{1};

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
    /// How a seated brain says a line. A null `say` means chat is not connected.
    ChatHooks chat{};
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
/// failure `outcome` is set and the sandbox is closed. A null match seeds
/// the brain's stream from match seed 0.
///
/// @param seat the seat, with its facts already set
/// @param spec the package
/// @param outcome set when the load or fits fails
/// @return true when the brain fits
bool open_and_fit(Seat& seat, const SeatSpec& spec, SeatOutcome& outcome);

/// Opens the libraries a brain may call, in a fixed order.
///
/// Later changes add one call here and nowhere else. Every installer has to
/// work when the seat has no match.
///
/// @param state the sandbox state
/// @param seat the seat being opened
void install_libraries(lua_State* state, Seat& seat);

/// Adds `brain.say`. Works when the seat's match is null.
///
/// @param state the sandbox state
/// @param seat the seat being opened
void install_chat(lua_State* state, Seat& seat);

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

/// The seed of one brain's own xoshiro256** stream.
///
/// The first eight bytes, little-endian, of the SHA-256 of the match seed as
/// eight little-endian bytes, then the player index, the id, a zero byte and
/// the version. A missing match uses seed 0.
///
/// @param match_seed the match's seed, or 0 when there is no match
/// @param player the seat's player
/// @param id the brain's id
/// @param version the brain's version
/// @return the seed passed to the sandbox
[[nodiscard]] uint64_t brain_random_seed(
    uint32_t match_seed, uint8_t player, std::string_view id, std::string_view version
) noexcept;

/// Copies `math.random` onto `brain.random`, so both share one stream.
///
/// @param state the sandbox state
void install_random(lua_State* state);

/// Installs `brain.after`, `brain.every` and `brain.cancel`.
///
/// Works when the seat has no match: the timers are stored and nothing runs
/// them.
///
/// @param state the sandbox state
/// @param seat the seat being opened
void install_timers(lua_State* state, Seat& seat);

/// Runs the timers whose due tick has come, in due-tick then sequence order.
///
/// A null match runs nothing. Each run charges one element of work.
///
/// @param state the sandbox state
/// @param seat the seat whose turn is in progress
void run_due_timers(lua_State* state, Seat& seat);

/// Sets the seat's Classic flag from its files and the baked-in fingerprint.
///
/// A non-empty `classic_fingerprint_for_tests` replaces the baked-in value.
/// An empty fingerprint matches nothing.
///
/// @param seat the seat being opened
/// @param spec the package
void note_classic(Seat& seat, const SeatSpec& spec) noexcept;

/// Installs `brain.shared_random` when the seat is Classic and has a match.
///
/// @param state the sandbox state
/// @param seat the seat being opened
void install_shared_random(lua_State* state, Seat& seat);

} // namespace oa::sim::brains
