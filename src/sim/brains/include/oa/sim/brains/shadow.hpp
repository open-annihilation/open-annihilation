// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A brain running beside the built-in player: both call logs, the comparison,
// and the set-up record a seat keeps.
#pragma once

#include "oa/sim/brains/seat_calls.hpp"
#include "oa/sim/match_runtime/script.hpp"
#include "oa/sim/match_runtime/seat_hooks.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {
class Match;
}

namespace oa::sim::brains {

/// One line of a call log.
enum class LogKind : uint8_t {
    section,      ///< `section <name>`
    draw,         ///< `draw <bound> <value>`
    assign,       ///< `assign <slot> <squad>`
    move,         ///< `move <slot> <x> <y> <z> <queue>`
    patrol,       ///< `patrol <slot> <x> <y> <z> <queue>`
    attack,       ///< `attack <slot> <target>`
    build,        ///< `build <slot> <type> <x> <y> <z>`
    queue_order,  ///< `queue <slot> <type> <count>`
    on,           ///< `on <slot> <0|1>`
    attack_point, ///< `attack_point <slot> <x> <y> <z>`
    stance,       ///< `stance <slot> <move> <fire>`
    refused,      ///< `refused <name> <slot>`
    resync        ///< `resync <state>`
};

/// One recorded call. Coordinates are signed 16.16 integers.
struct LogEntry {
    LogKind kind{};
    /// Section name, or the refused order's name. Empty otherwise.
    char name[16]{};
    uint16_t slot{};
    /// Squad, target or type, where the line has one.
    uint16_t extra{};
    int32_t x{};
    int32_t y{};
    int32_t z{};
    /// Factory count, or the standing move when `kind` is `stance`.
    int32_t count{};
    uint32_t bound{};
    uint32_t value{};
    /// Stream state before a draw, or the state a `resync` names.
    uint32_t state_before{};
    /// Stream state after a draw.
    uint32_t state_after{};
    /// Queue flag, or 0 or 1 for `on`.
    uint8_t flag{};
    /// Standing fire when `kind` is `stance`.
    int32_t fire{};
};

/// A call log. It owns `items`. It cannot be copied or moved.
struct CallLog {
    LogEntry* items{};
    uint32_t count{};
    uint32_t capacity{};

    CallLog() = default;
    /// Frees `items`.
    ~CallLog();
    CallLog(const CallLog&) = delete;
    CallLog& operator=(const CallLog&) = delete;
    CallLog(CallLog&&) = delete;
    CallLog& operator=(CallLog&&) = delete;
};

/// Appends one entry, growing the buffer by doubling.
///
/// @param log the log
/// @param entry the entry
/// @return false when the buffer cannot grow
bool log_push(CallLog& log, const LogEntry& entry) noexcept;

/// Drops every entry. The buffer stays.
///
/// @param log the log
void log_clear(CallLog& log) noexcept;

/// The built-in player's host, forwarding each call and recording the ones
/// that act. It cannot be copied or moved: `host.context` points at it.
struct LoggingHost {
    /// Pass this to the built-in player. `context` is this object.
    sim::ai::ComputerHost host{};
    /// The host the calls are forwarded to.
    sim::ai::ComputerHost inner{};
    /// The log the calls are recorded in. Not owned.
    CallLog* log{};
    uint8_t player{};
    /// Stream state, or null when the host has no stream. A null reads as 0.
    uint32_t (*stream_state)(void* context){};
    void* stream_context{};
    /// True after this turn's log recorded `sort`, until the stances are noted.
    bool saw_sort{};

    LoggingHost() = default;
    LoggingHost(const LoggingHost&) = delete;
    LoggingHost& operator=(const LoggingHost&) = delete;
    LoggingHost(LoggingHost&&) = delete;
    LoggingHost& operator=(LoggingHost&&) = delete;
};

/// Points `session.host` at the forwarders.
///
/// `session` must outlive every call made through `session.host`.
///
/// @param session the logging host
/// @param inner the host to forward to
/// @param log the log
/// @param player the seat
/// @param stream_state reads the stream, or null
/// @param stream_context passed to `stream_state`
void logging_arm(
    LoggingHost& session,
    const sim::ai::ComputerHost& inner,
    CallLog& log,
    uint8_t player,
    uint32_t (*stream_state)(void* context),
    void* stream_context
) noexcept;

/// Records the standing move and fire of the seat's selectable units.
///
/// Called after the built-in turn. It records only when that turn logged
/// `sort`, and then only once.
///
/// @param session the logging host
void logging_note_stances(LoggingHost& session) noexcept;

/// The brain's calls: orders are recorded, reads of the seat come from a
/// snapshot, and every other read goes to `live`. It cannot be copied or
/// moved: `calls.host.context` points at it.
struct Recording {
    /// Pass the brain's Lua through this table.
    SeatCalls calls{};
    /// The match, or a test host. Reads that are not snapshotted go here.
    SeatCalls live{};
    /// The brain's log for this turn. Not owned.
    CallLog* brain{};
    /// The built-in player's log for this turn. Not owned. Null reads nothing.
    const CallLog* builtin{};
    /// The copy of the shared stream.
    match_runtime::SharedRandom copy{1};
    uint8_t player{};
    /// True while a turn's copy may be drawn.
    bool turn_ready{};
    /// Snapshot and section cursor. Owned. Null until `recording_bind`.
    struct RecordDetail* detail{};

    Recording() = default;
    /// Frees the snapshot.
    ~Recording();
    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;
    Recording(Recording&&) = delete;
    Recording& operator=(Recording&&) = delete;
};

/// Installs the recording table over `live`.
///
/// `rec` must outlive every call made through `rec.calls`.
///
/// @param rec the recording
/// @param live the match or the test host
/// @param player the seat
void recording_bind(Recording& rec, const SeatCalls& live, uint8_t player) noexcept;

/// Snapshots `live` and arms the stream copy for one turn.
///
/// @param rec the recording
/// @param brain the brain's log; it is not cleared
/// @param builtin the built-in player's log for this turn, or null
/// @param stream the stream state at the start of the turn
void recording_begin(
    Recording& rec, CallLog& brain, const CallLog* builtin, uint32_t stream
) noexcept;

/// Records `refused <name> <slot>` and does nothing else.
///
/// @param rec the recording
/// @param name the order's name
/// @param slot the unit
void recording_refuse(Recording& rec, const char* name, uint16_t slot) noexcept;

/// Where two logs of one tick first disagree.
struct TickDifference {
    /// True when a compared section disagrees.
    bool different{};
    /// The section name, or `schedule`. Empty when `different` is false.
    char section[16]{};
};

/// `start` in a section mask.
inline constexpr uint32_t shadow_bit_start = 1u << 0;
/// `schedule` in a section mask.
inline constexpr uint32_t shadow_bit_schedule = 1u << 1;
/// `sort` in a section mask.
inline constexpr uint32_t shadow_bit_sort = 1u << 2;
/// Every section a shadow can compare, including `start` and `schedule`.
inline constexpr uint32_t shadow_mask_all = (1u << 12) - 1u;

/// The bit of `taskN` for N in 1..9.
///
/// @param task the task, 1 to 9
/// @return the bit, or 0 when `task` is outside 1..9
[[nodiscard]] constexpr uint32_t shadow_bit_task(uint8_t task) noexcept {
    return task >= 1 && task <= 9 ? (1u << (2u + task)) : 0u;
}

/// Compares two logs of one tick.
///
/// `resync` lines are left out. Stance lines are compared on their own, and a
/// stance difference is named `sort`. A section whose bit is clear in
/// `mask` is left out.
///
/// @param builtin the built-in player's log
/// @param brain the brain's log
/// @param mask the sections to compare
/// @return the first difference, or none
[[nodiscard]] TickDifference
compare_logs(const CallLog& builtin, const CallLog& brain, uint32_t mask) noexcept;

/// Parses a comma-separated section list.
///
/// Empty or null means every section. A token that is not `start`, `schedule`,
/// `sort` or `task1` to `task9` is ignored.
///
/// @param list the list, or null
/// @return the mask
[[nodiscard]] uint32_t shadow_section_mask(const char* list) noexcept;

/// The name of a built-in section.
///
/// @param section `section_sort`, a squad index, or `section_start`
/// @return `sort`, `taskN`, `start`, or `section`
[[nodiscard]] const char* shadow_section_name(uint8_t section) noexcept;

/// Whether the brain's set-up draws matched the record.
enum class ShadowStart : uint8_t {
    not_compared, ///< `start` was left out of the section list
    same,         ///< the draws match
    differs       ///< a bound, a value or the count disagrees
};

/// What one shadowed seat has to report. Pointers address the seat's memory.
struct ShadowReport {
    ShadowStart start{ShadowStart::not_compared};
    /// Ticks whose logs were compared.
    uint32_t ticks{};
    /// Ticks that disagreed, including the first.
    uint32_t differ_count{};
    /// The first disagreeing tick. Meaningful when `differ_count` is not 0.
    uint32_t first_tick{};
    /// The first disagreeing section.
    char section[16]{};
    bool stopped{};
    uint32_t stopped_tick{};
    /// The stop message. Empty when the seat has not stopped.
    const char* message{};
    /// Rendered set-up logs. Null until the seat has a record.
    const char* start_builtin{};
    const char* start_brain{};
    /// The first disagreeing tick and the two before it, oldest first.
    const char* tick_builtin[3]{};
    const char* tick_brain[3]{};
    uint32_t tick_numbers[3]{};
    /// How many of `tick_builtin` are filled.
    uint8_t tick_files{};
};

/// Whether a shadow is seated for one player.
///
/// @param match the match
/// @param player player index, 0 to 9
/// @return true when that seat is a shadow
[[nodiscard]] bool brain_is_shadow(const match_runtime::Match& match, uint8_t player) noexcept;

/// The set-up record of a seated brain.
///
/// @param match the match
/// @param player player index, 0 to 9
/// @return the record, or null when that seat has none yet
[[nodiscard]] const match_runtime::SetUpRecord*
seat_setup_record(const match_runtime::Match& match, uint8_t player) noexcept;

/// The shadow report for one seat.
///
/// @param match the match
/// @param player player index, 0 to 9
/// @return the report, or null when that seat is not a shadow
[[nodiscard]] const ShadowReport*
shadow_report(const match_runtime::Match& match, uint8_t player) noexcept;

/// Plays the built-in turn and, until the brain stops, the brain's turn.
///
/// @param context the seat
/// @param match the match
/// @param player player index
/// @return true, so the match does not play the built-in turn again
bool shadow_play(void* context, match_runtime::Match& match, uint8_t player);

/// Keeps the set-up record and then calls the brain's `start`.
///
/// @param context the seat
/// @param match the match
/// @param player player index
/// @param record the set-up draws
void shadow_set_up(
    void* context,
    match_runtime::Match& match,
    uint8_t player,
    const match_runtime::SetUpRecord& record
);

/// Keeps the set-up record for a brain that plays the seat.
///
/// @param context the seat
/// @param match the match
/// @param player player index
/// @param record the set-up draws
void playing_set_up(
    void* context,
    match_runtime::Match& match,
    uint8_t player,
    const match_runtime::SetUpRecord& record
);

} // namespace oa::sim::brains
