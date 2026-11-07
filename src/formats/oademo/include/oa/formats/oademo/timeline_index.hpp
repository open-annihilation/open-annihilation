// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The index of a replay: `timeline.json`, which a `.oademo` may carry and
// which the index pass (`--replay-index`) writes beside any recording. It
// holds the replay's length and how it ended, the markers a timeline bar
// shows, each player's totals every ten seconds of game time, the battle's
// intensity and the result. Every number is a whole number counted from the
// recording, so the same recording always gives the same text.

#include "oa/formats/oademo/oademo.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::oademo {

/// The version of `timeline.json` this reader and writer use, written as
/// `oademo_timeline`.
inline constexpr int timeline_version = 1;

/// Ticks in one second of game time.
inline constexpr uint32_t game_ticks_per_second = 30;

/// Seconds of game time between two samples of the totals, and in each
/// stretch of battle intensity.
inline constexpr uint32_t index_period_seconds = 10;

/// Ticks between two samples of the totals, and in each stretch of battle
/// intensity.
inline constexpr uint32_t index_period_ticks = game_ticks_per_second * index_period_seconds;

/// The most markers of one kind an index keeps. Later ones are left out and
/// counted in `markers_omitted`, so a file stays small however many lines
/// of chat a game held.
inline constexpr std::size_t most_markers_of_a_kind = 1000;

/// How a replay ended.
enum class IndexEnd : uint8_t {
    finished, ///< the game reached its end: a side won, or the players' scores came in
    left,     ///< a player left or dropped, and the recording ends with the game undecided
    cut,      ///< the recording stops with the game undecided, or the replay could not go on
};

/// Returns the word `game.ended` and `ended.how` use for an end.
///
/// @param end how the replay ended
/// @return `finished`, `left` or `cut`
[[nodiscard]] std::string_view index_end_name(IndexEnd end) noexcept;

/// How the replay of the recording went, as the engine judged it.
struct IndexVerdict {
    /// Everything recorded was replayed.
    bool finished{};
    /// No record failed and no tick failed.
    bool clean{};
    /// The recording's periodic records came at the spacing it states.
    bool paced{};
    /// The installed unit definitions differ from the recording's.
    bool content_differs{};
    /// Records and ticks that failed.
    uint32_t errors{};
    /// The tick the first error was seen on; 0 for none.
    uint32_t first_error_tick{};
    /// The last error's text, as the engine words it; empty for none.
    std::string last_error{};
};

/// What a marker marks.
enum class MarkerKind : uint8_t {
    start,           ///< the first tick
    end,             ///< the last tick
    commander_death, ///< a player's commander died: `player` is the dead player's, `other` the killer's
    player_left,     ///< a player left the game
    player_dropped,  ///< a player's connection was lost
    player_defeated, ///< a player was removed as defeated
    alliance,        ///< `player` allied with `other` (`flag`) or broke the alliance
    pause,           ///< `player` paused (`flag`) or resumed the game
    chat,            ///< `player` said `text`
};

/// Returns the word `kind` is written as.
///
/// @param kind the kind
/// @return such as `commander_death`
[[nodiscard]] std::string_view marker_kind_name(MarkerKind kind) noexcept;

/// One marker of the timeline.
struct IndexMarker {
    /// The game tick it happened on.
    uint32_t tick{};
    /// What it marks.
    MarkerKind kind{MarkerKind::start};
    /// The player's number in the recording; 0 for none.
    int player{};
    /// The other player's number; 0 for none.
    int other{};
    /// A pause marker: the game paused. An alliance marker: the players allied.
    bool flag{};
    /// A chat marker: the line, as UTF-8.
    std::string text{};
};

/// One player's totals at one moment.
struct IndexTotals {
    /// Units the player destroyed.
    uint32_t kills{};
    /// Units the player lost.
    uint32_t losses{};
    /// Commanders the player lost.
    uint32_t commanders_lost{};
    /// Units the player has.
    uint32_t units{};
    /// Metal stored, in whole units.
    int32_t metal{};
    /// Energy stored, in whole units.
    int32_t energy{};
};

/// A player of the replay, and where the player's totals ended.
struct IndexPlayer {
    /// The player's number in the recording.
    int number{};
    /// The player's name, as UTF-8.
    std::string name{};
    /// The player only watched.
    bool watcher{};
    /// The totals on the last tick.
    IndexTotals final{};
};

/// One player's totals once every `period_ticks`, from `first_tick`.
struct IndexSeries {
    /// The player's number in the recording.
    int player{};
    /// Each sample's kills.
    std::vector<uint32_t> kills{};
    /// Each sample's losses.
    std::vector<uint32_t> losses{};
    /// Each sample's commanders lost.
    std::vector<uint32_t> commanders_lost{};
    /// Each sample's units.
    std::vector<uint32_t> units{};
    /// Each sample's metal stored.
    std::vector<int32_t> metal{};
    /// Each sample's energy stored.
    std::vector<int32_t> energy{};
};

/// How the result was found.
enum class ResultBy : uint8_t {
    none,       ///< the replay decides no result
    commanders, ///< the team whose commanders were left when the others were out
    mission,    ///< a campaign mission's own end, for the player who recorded it
};

/// Returns the word `by` is written as.
///
/// @param by how the result was found
/// @return `none`, `commanders` or `mission`
[[nodiscard]] std::string_view result_by_name(ResultBy by) noexcept;

/// Who won.
struct IndexResult {
    /// The winners' numbers in the recording; empty when no result is known.
    std::vector<int> winners{};
    /// How the result was found.
    ResultBy by{ResultBy::none};
};

/// A replay's index.
struct TimelineIndex {
    /// The version, written as `oademo_timeline`. The reader accepts 1.
    int version{timeline_version};
    /// The last tick: the replay's length in ticks.
    uint32_t length_ticks{};
    /// The recording's length on the recorder's clock, in milliseconds, when known.
    std::optional<uint64_t> length_ms{};
    /// How the replay ended.
    IndexEnd ended{IndexEnd::cut};
    /// The tick it ended on.
    uint32_t ended_tick{};
    /// How the replay of the recording went.
    IndexVerdict verdict{};
    /// Who won.
    IndexResult result{};
    /// The players, in the recording's order.
    std::vector<IndexPlayer> players{};
    /// The markers, by tick and then in the order the recording holds them.
    std::vector<IndexMarker> markers{};
    /// Markers left out because a kind held more than `most_markers_of_a_kind`.
    uint32_t markers_omitted{};
    /// The tick the first sample and the first stretch of intensity start on.
    uint32_t first_tick{};
    /// Ticks between two samples, and in each stretch of intensity.
    uint32_t period_ticks{index_period_ticks};
    /// Each player's totals who played, one sample every `period_ticks`.
    std::vector<IndexSeries> totals{};
    /// Health points taken off units in each stretch of `period_ticks`.
    std::vector<uint64_t> damage{};
    /// Members this version does not interpret.
    std::vector<JsonExtra> extra{};
};

/// Writes an index as compact JSON.
///
/// Members are written in a fixed order, then any member this version does
/// not interpret, so the same index gives the same text.
///
/// @param index the index
/// @return the `timeline.json` text
[[nodiscard]] std::string timeline_index_text(const TimelineIndex& index);

/// Reads an index.
///
/// A marker of a kind this version does not know is left out.
///
/// @param text the `timeline.json` text
/// @return the index, or nothing when the text is not JSON, names a version
///         other than 1, or lacks a member or holds a wrong one
[[nodiscard]] std::optional<TimelineIndex> parse_timeline_index(std::string_view text);

} // namespace oa::formats::oademo
