// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The seat a match keeps behind seat_state. Not part of the public header.
#pragma once

#include "oa/sim/brains/chat.hpp"
#include "sight.hpp"

#include "oa/sim/brains/events.hpp"
#include "oa/sim/brains/logs.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/brains/seat_calls.hpp"

#include "reach.hpp"

#include "oa/core/player.h"
#include "oa/data/brain_facts.hpp"
#include "oa/data/script_host/memory_sources.hpp"
#include "oa/sim/match_runtime/script.hpp"
#include "oa/sim/match_runtime/seat_hooks.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

struct lua_State;

namespace oa::data::neural {
class NetworkSet;
}

namespace oa::sim::brains {

namespace script_host = oa::data::script_host;

struct CallLog;

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
/// Most orders one turn may pass to the match.
inline constexpr uint32_t max_orders_per_turn = 4096;

/// Orders given and the refusals, for one turn or for the match.
struct OrderTally {
    uint32_t given{};
    uint32_t not_own{};
    uint32_t dead{};
    uint32_t not_seen{};
    uint32_t not_buildable{};
    uint32_t over_limit{};
    uint32_t by_engine{};
};

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
    /// Networks built from the manifest, under each network's id.
    /// Null when the spec names none. Destroyed after the sandbox.
    std::unique_ptr<oa::data::neural::NetworkSet> networks;
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
    /// True after a failed turn was handed back. `stopped` has been called.
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
    /// True when this seat's files match the Classic fingerprint.
    bool classic{};
    /// Orders given and refused since this turn started. `start` is one turn.
    OrderTally turn_orders{};
    /// Orders given and refused since the seat was made.
    OrderTally match_orders{};
    /// Orders passed to the match since this turn started, whether or not it
    /// kept them. A refusal before the call does not count here.
    uint32_t orders_this_turn{};
    /// Timers waiting to run on a later turn.
    WaitingTimer timers[max_timers]{};
    /// The next timer id. It starts at 1 and only grows.
    uint32_t next_timer_sequence{1};
    /// Movement regions built when a script asks whether two points meet.
    /// One class is labelled at most once a tick.
    RegionCache regions{};
    /// True when this brain runs beside the built-in player.
    bool shadow{};
    /// The calls Lua reaches the match through. Empty until the seat is filled.
    SeatCalls calls{};
    /// The built-in player's set-up draws. Meaningful when `setup_ready`.
    match_runtime::SetUpRecord setup{};
    /// True once `setup` holds this seat's record.
    bool setup_ready{};
    /// A copy of the stream at the set-up record. `brain.setup_random` draws it.
    match_runtime::SharedRandom setup_copy{1};
    /// The shadow's logs and snapshot. Null when the seat is not a shadow.
    struct ShadowBody* shadow_body{};
    /// Sections to compare. Empty compares every section.
    char shadow_sections[256]{};
    /// The calls before a brain log wrapped them. Empty until the log arms.
    SeatCalls logged_inner{};
    /// True once `calls` forwards through the logging wrappers.
    bool calls_logged{};
    /// Text of the tick being logged. Null until a line needs room.
    char* log_text{};
    /// Bytes used in `log_text`, not counting the terminating NUL.
    uint32_t log_used{};
    /// Bytes `log_text` can hold, including the NUL.
    uint32_t log_capacity{};
    /// True while this tick's lines are being collected.
    bool log_open{};

    /// Defined with the load so `NetworkSet` can stay incomplete here.
    Seat();
    /// Releases the event buffers, the log text and the shadow body. The
    /// networks die with the seat.
    ~Seat();
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
    /// Hooks the app set. Empty until `set_report_hooks`.
    ReportHooks reports{};
    /// How a seated brain says a line. A null `say` means chat is not connected.
    ChatHooks chat{};
    /// Instructions each player's brain calls have used. This outlives the
    /// seat, so a hand-over or a closed seat keeps the total.
    std::array<uint64_t, OA_PLAYER_COUNT> instructions{};
    /// Where this match's brain log and samples go. Empty collects nothing.
    LogHooks logs{};
    /// True once `brain log 1` has been handed to `logs.lines`.
    bool log_banner_sent{};
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

/// Builds each manifest network into the seat, after the digest check.
///
/// An empty list leaves `seat.networks` null. Each file is read and its
/// memory is counted before it is built. A file, a build or a cap that fails
/// refuses the seat. Nothing here runs Lua or checks a digest again.
///
/// @param seat the seat being loaded
/// @param spec the package
/// @param outcome set when a network is refused or cannot be stored
/// @return false when the seat must not open
bool build_networks(Seat& seat, const SeatSpec& spec, SeatOutcome& outcome);

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
/// Installs `game.map`. With no match the field is nil.
///
/// @param state the sandbox state
/// @param seat the seat being opened
void install_map(lua_State* state, Seat& seat);

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
/// player takes the turn. A failed turn hands the seat back and still returns
/// true, so the built-in player starts on the following tick.
///
/// @param context the seat
/// @param match the match
/// @param player the seat's player
/// @return false when the built-in player should take the turn
bool deliver_turn(void* context, match_runtime::Match& match, uint8_t player);

/// Drains the sandbox's print lines into the match's report hooks.
///
/// A seat with no sandbox, or a match with no printed hook, forgets the
/// lines. The view passed to the hook lasts for that call.
///
/// @param seat the seat
void note_prints(Seat& seat);

/// Closes a failed seat and lets the built-in player take the next tick.
///
/// The first call drains printed lines, calls `stopped` once, clears the
/// seat's hooks and closes the sandbox. A later call only clears the hooks
/// and closes the sandbox. The failing turn's play still returns true.
///
/// @param seat the seat
/// @param error the script error; it must stay alive for this call
void hand_back(Seat& seat, const script_host::ScriptError& error);

/// Adds the sandbox's instructions from the call that just returned.
///
/// A seat with no match, or no sandbox, adds nothing. The count saturates
/// at the largest 64-bit value.
///
/// @param seat the seat whose call just returned
void note_call_instructions(Seat& seat) noexcept;

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

/// The hooks stored for the seat's match. Empty when the seat has no match.
///
/// @param seat the seat
/// @return the hooks, or empty hooks
[[nodiscard]] LogHooks seat_log_hooks(const Seat& seat) noexcept;

/// Points a playing seat's calls through the brain-log wrappers.
///
/// A shadow is left alone. A seat with no log, or with calls not yet filled,
/// is left alone and can be armed later. A second call does nothing.
///
/// @param seat the seat
void log_arm_seat(Seat& seat) noexcept;

/// Starts one tick of the brain log. A seat with no log collects nothing.
///
/// @param seat the seat
/// @param tick the tick number written on the heading
/// @param brain true for a brain, false for the built-in player
void log_begin_turn(Seat& seat, uint32_t tick, bool brain) noexcept;

/// Hands the tick's text to the log and clears the buffer.
///
/// @param seat the seat
void log_end_turn(Seat& seat) noexcept;

/// Writes `call <name>` and the numeric arguments, when a tick is open.
///
/// @param seat the seat
/// @param name the hook name
/// @param numbers the numeric arguments; null when `count` is 0
/// @param count how many numbers
void log_call(Seat& seat, const char* name, const int64_t* numbers, uint32_t count) noexcept;

/// Writes `own-draw <low> <high> <value>`.
///
/// @param seat the seat
/// @param low the low end of the draw
/// @param high the high end of the draw
/// @param value the number drawn
void log_own_integer(Seat& seat, int64_t low, int64_t high, int64_t value) noexcept;

/// Writes `own-draw float` and 16 hex digits of a double's bits.
///
/// @param seat the seat
/// @param bits the double's bits
void log_own_float(Seat& seat, uint64_t bits) noexcept;

/// Writes `order <name>` and the numeric arguments, when a tick is open.
///
/// @param seat the seat
/// @param name the order name
/// @param numbers the numeric arguments; null when `count` is 0
/// @param count how many numbers
void log_order(Seat& seat, const char* name, const int64_t* numbers, uint32_t count) noexcept;

/// Writes a standing `stance` line, overlaying one field on the unit's flags.
///
/// @param seat the seat
/// @param slot the unit
/// @param fire_field true to overlay fire, false to overlay move
/// @param value the field's new value
void log_standing_order(Seat& seat, uint16_t slot, bool fire_field, int32_t value) noexcept;

/// Hands a shadow turn's two call logs to the brain log, built-in then brain.
///
/// A match with no log does nothing. The caller still owns both logs.
///
/// @param seat the shadow seat
/// @param builtin the built-in player's calls
/// @param brain the brain's calls
/// @param tick the tick
void publish_logged_shadow_turn(
    Seat& seat, const CallLog& builtin, const CallLog& brain, uint32_t tick
) noexcept;

/// Installs one function as both `math.random` and `brain.random`.
///
/// Both draw the stream `math.random` had when the profile installed. A
/// script that replaces `math.random` still draws that stream through
/// `brain.random`. An open brain log records each draw.
///
/// @param state the sandbox state
/// @param seat the seat being opened
void install_random(lua_State* state, Seat& seat);

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

/// Installs `brain.shared_random` for a Classic seat, and for a shadow.
///
/// A Classic seat draws the match stream. A shadow draws its turn's copy,
/// and only while that copy is armed. Neither is installed without a match.
///
/// @param state the sandbox state
/// @param seat the seat being opened
void install_shared_random(lua_State* state, Seat& seat);

/// Installs `brain.section` for every seat, including one with no match.
///
/// @param state the sandbox state
/// @param seat the seat being opened
void install_section(lua_State* state, Seat& seat);

/// Installs `brain.setup_random` for a Classic seat and for a shadow.
///
/// Installed with no match as well: the call then raises that the seat is
/// not set up yet.
///
/// @param state the sandbox state
/// @param seat the seat being opened
void install_setup_random(lua_State* state, Seat& seat);

/// Writes `me.difficulty` in the hidden table `me` reads.
///
/// @param state the sandbox state
/// @param difficulty `easy`, `medium` or `hard`
void set_hidden_difficulty(lua_State* state, const char* difficulty);

/// Builds the shadow body and points the seat's calls at the recording table.
///
/// @param seat the shadow seat; its match, when set, supplies the live calls
/// @param mask sections to compare
void shadow_attach(Seat& seat, uint32_t mask) noexcept;

/// Drops the shadow body. A seat that is not a shadow is left as it is.
///
/// @param seat the seat
void shadow_release(Seat& seat) noexcept;

/// Whether the shadow's turn copy may be drawn.
///
/// @param seat the seat
/// @return true while its recording is armed for this turn
[[nodiscard]] bool shadow_stream_ready(const Seat& seat) noexcept;

/// Records one `brain.setup_random` draw on a shadow.
///
/// @param seat the shadow seat
/// @param bound the bound the brain passed
/// @param value the value the copy returned
void shadow_note_setup_draw(Seat& seat, uint32_t bound, uint32_t value) noexcept;

/// Delivers one shadow turn. A failure leaves the seat seated.
///
/// @param seat the shadow seat
/// @param error receives the script error when the call fails
/// @return false when the turn failed
bool deliver_shadow_turn(Seat& seat, script_host::ScriptError& error);

} // namespace oa::sim::brains
