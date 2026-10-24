// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// A live recording of a network game in the recorder's container
// (recorder.ta-demo-recorder), which a replayer and demo playback read:
// version 5, the recording machine as player 1, every player's status as
// the datagram of its setup block and team, the battle room's unit checks,
// and every record sent or received in order, each unit state stored
// without its tick behind a tick base.

#include "oa/core/player_setup.h"
#include "oa/formats/tad.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace oa::session::demo {

/// One player of the recorded game.
struct RecordingPlayer {
    uint32_t player_id{}; ///< its transport id; records are stored under its player number
    std::string name;
    PlayerSetupInfo info{}; ///< its setup block as last sent
    uint8_t team{};         ///< its team byte (0x24)
    std::string address;    ///< its network address as text; stored masked
};

/// The most loadout text one recording holds: a sector chunk less its length and type words.
inline constexpr std::size_t recording_loadout_most_bytes =
    formats::tad::limit::chunk_bytes - formats::tad::layout::chunk_length_bytes -
    formats::tad::layout::sector_type_bytes;

/// The most brains text one recording holds: a sector chunk less its length and type words.
inline constexpr std::size_t recording_brains_most_bytes =
    formats::tad::limit::chunk_bytes - formats::tad::layout::chunk_length_bytes -
    formats::tad::layout::sector_type_bytes;

/// What a recording starts with.
struct RecordingSetup {
    uint16_t max_units{};
    std::string map_name;
    std::string recorder_text; ///< sector 3: the recording program's line
    std::string date_text;     ///< sector 4
    /// The loadout of a game played with mutators, as text. Empty writes no
    /// sector. A text longer than recording_loadout_most_bytes is left out,
    /// and the recording is still written.
    std::string loadout_text;
    /// The brains that played the game's computer players, as text. Empty
    /// writes no sector. A text longer than recording_brains_most_bytes is
    /// left out, and the recording is still written.
    std::string brains_text;
    std::vector<RecordingPlayer> players; ///< the recording machine's player first; at most 10
    std::vector<std::array<uint8_t, formats::tad::layout::unit_check_record_bytes>> unit_checks;
    bool compress{}; ///< packets stored compressed (marker 4)
};

/// A recording being made.
struct DemoRecording {
    RecordingSetup setup;
    std::vector<std::vector<uint8_t>> packets; ///< each u16 delay, u8 sender, payload
    uint64_t last_ms{};
    bool started{};
    uint32_t dropped_records{}; ///< from senders the recording does not name
};

/// Starts a recording.
///
/// @param[out] recording the recording; any earlier one is dropped
/// @param setup the game being recorded
void recording_begin(DemoRecording* recording, RecordingSetup setup);

/// Adds one record sent or received.
///
/// A unit state (0x2c) is stored as a tick base and the record without its
/// tick, an empty one as the empty-tick marker. The delay is the time since
/// the previous record, in milliseconds.
///
/// @param[in,out] recording the recording
/// @param sender_id transport id of the player the record is from
/// @param now_ms wall clock in milliseconds
/// @param record the record, type byte first
void recording_add(
    DemoRecording* recording, uint32_t sender_id, uint64_t now_ms, std::span<const uint8_t> record
);

/// Writes the recording's container.
///
/// @param recording the recording
/// @return the file bytes, or the container's error
[[nodiscard]] formats::tad::WriteResult recording_write(const DemoRecording& recording);

/// Builds the file name an automatic recording takes: the date and time, then the map.
///
/// @param date_time the date and time as "YYYY-MM-DD HHMM"; empty leaves the map's name alone
/// @param map_name the map
/// @param extension the recorder's automatic extension, such as ".tad"
/// @return the name, without directories; characters a file name cannot hold become '_'
[[nodiscard]] std::string recording_file_name(
    std::string_view date_time, std::string_view map_name, std::string_view extension
);

} // namespace oa::session::demo
