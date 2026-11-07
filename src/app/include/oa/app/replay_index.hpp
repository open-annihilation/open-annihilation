// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The two parts of the index pass (--replay-index, docs/replays.md) that need
// no running match.
//
// A recording holds more than the replay shows: the chat lines the players
// typed, the pauses they called, the players who left, were dropped or were
// removed as defeated, the alliances they made, and the end-of-game records
// the machines exchanged for the score screen. read_stream_notes walks the
// recording's records once, without playing it, and notes each with the tick
// of the packet that carries it, as playback numbers the packets.
// build_index then combines those notes with the timeline the director's
// recorder made while the recording was replayed (the commander deaths, each
// player's totals and the damage dealt) into the index.
//
// Everything is a whole number or text taken from the recording, so the same
// recording always gives the same index.
#pragma once

#include "oa/formats/oademo/timeline_index.hpp"
#include "oa/media/director/timeline.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oa::app::replay_index {

/// What a note of the recording's stream is about.
enum class NoteKind : uint8_t {
    chat,         ///< a chat line: `player` said `text`
    pause,        ///< the game was paused (`value` 1) or resumed (`value` 0) by `player`
    rejected,     ///< a reject record named `other` with the reason in `value`
    disconnected, ///< a disconnect notice named `other`
    alliance,     ///< `player` set its alliance with `other` to `value`
    end_of_game,  ///< `player` sent its score for the end screen, asking for the others'
};

/// One record of the recording's stream that the index notes.
struct Note {
    /// The tick of the packet that holds the record, as playback numbers it.
    uint32_t tick{};
    /// What the record is.
    NoteKind kind{NoteKind::chat};
    /// The number of the player the packet is from.
    int player{};
    /// The number of the player the record names; 0 when it names none the recording holds.
    int other{};
    /// The record's value; see NoteKind.
    uint8_t value{};
    /// A chat line, as UTF-8.
    std::string text{};
};

/// A player of the recording.
struct StreamPlayer {
    /// The player's number in the recording.
    int number{};
    /// The player's name, as UTF-8.
    std::string name{};
    /// The player only watches.
    bool watcher{};
};

/// What a recording's own stream holds.
struct StreamNotes {
    /// The players, by player index: the order the replay seats them in.
    std::vector<StreamPlayer> players{};
    /// The notes, in the order the recording holds them.
    std::vector<Note> notes{};
};

/// Reads the notes of a recording.
///
/// A packet whose payload cannot be unpacked, or that holds no game record,
/// or that is from a player the recording does not list, is skipped, as
/// playback skips it. A private chat line is left out.
///
/// @param file a `.tad` or a `.oademo`
/// @param[out] error why the file was not read; untouched on success
/// @return the notes, or nothing when the file is neither a `.oademo` this
///         game reads nor a recording that parses
[[nodiscard]] std::optional<StreamNotes>
read_stream_notes(std::span<const uint8_t> file, std::string& error);

/// What the index is built from.
struct Source {
    /// The replayed game's timeline, finished.
    const oa::media::director::Timeline* timeline{};
    /// What the recording's stream holds.
    const StreamNotes* stream{};
    /// How every player stood on the last tick (director::read_player_stats).
    std::vector<oa::media::director::PlayerStats> final_stats{};
    /// The recording's length on the recorder's clock, in milliseconds; 0 when not known.
    uint64_t length_ms{};
    /// How `meta.json` says the game ended, when that word is `finished` or
    /// `left`. Nothing for a `.tad`, a description that was not read, and any
    /// other word. Used only when the replay reached the end of the recording
    /// and the commanders, a departure and the end screen did not decide it.
    std::optional<oa::formats::oademo::IndexEnd> recorded_end{};
};

/// Builds a replay's index.
///
/// The totals are the timeline's player stats on every
/// formats::oademo::index_period_ticks-th tick from the first; a player whose
/// slot has been emptied keeps its last totals, with no units. The damage in
/// each stretch of that many ticks is the health points taken off units by
/// every health event but healing and paralysis. The recording ended `cut`
/// when the replay could not go on to the end of what was recorded. Otherwise
/// `finished` when the commanders decided it or its end-of-game records came,
/// `left` when players left or dropped with the game undecided, and, when
/// none of those, `finished` or `left` as `Source::recorded_end` says. It is
/// `cut` when that is unset. `ended.tick` is the last tick.
///
/// @param source the timeline, the stream's notes, the last totals and the
///        recording's own end
/// @return the index
[[nodiscard]] oa::formats::oademo::TimelineIndex build_index(const Source& source);

} // namespace oa::app::replay_index
