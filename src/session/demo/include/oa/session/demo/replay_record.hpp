// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// A recording of a game, kept as a .oademo replay. The game describes what it
// knows when it starts (replay_begin: the map, the players, the content, the
// mutators and brains, the unit table's digest), streams the recording to a
// file as it goes (RecordingStream), and when the game ends, or a crash cut it
// short, finishes that stream into the replay (finish_as_replay) or, for the
// recorder hack's layout, into the .tad it already is (finish_as_tad). The
// replay holds no network address.

#include "oa/formats/oademo/oademo.hpp"
#include "oa/session/demo/recording.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::session::demo {

/// The first release that reads a `.oademo`, which a replay's manifest names
/// as the engine it needs (`engine.needs`), whichever build wrote it.
inline constexpr std::string_view first_release_reading_replays = "0.9.1";

/// What the game knows of one recorded player that the recording does not hold.
struct ReplayPlayerFacts {
    /// The player's name as UTF-8.
    std::string name{};
    /// human, computer or watcher.
    std::string kind{};
    /// True for a player on the machine that recorded.
    bool recorded_here{};
};

/// What the game knows of itself when a recording starts, and a recording does not hold.
struct ReplayFacts {
    /// The clock at the start, in UTC: `YYYY-MM-DDTHH:MM:SSZ`.
    std::string recorded_at{};
    /// The program that recorded, such as `open-annihilation 0.9.1`.
    std::string recorded_by{};
    /// The machine's platform, such as `macos-arm64`.
    std::string platform{};
    /// How the game was recorded: `multiplayer`, `skirmish` or `campaign`.
    std::string how{"multiplayer"};
    /// The version of the engine that recorded.
    std::string engine_version{};
    /// The kind of game: `multiplayer`, `skirmish`, `campaign`, or the kind an extension names.
    std::string game_kind{"multiplayer"};
    /// The object under `game` named for the kind, as compact JSON; empty for none.
    std::string game_kind_object{};
    /// The map's name as UTF-8.
    std::string map_name{};
    /// The base game's id.
    std::string base_game{"totala"};
    /// The base game's version.
    std::string base_version{"3.1c"};
    /// The expansions that change the rules, in order.
    std::vector<std::string> expansions{};
    /// The mod played, when there was one.
    std::optional<formats::oademo::ModRef> mod{};
    /// The mod and the map packs the game needs, as manifest entries (the
    /// mutators come from the loadout). An `oamod` entry gains the unit
    /// table's digest.
    std::vector<formats::oademo::Need> packages{};
    /// Each recorded player, in the recording's order. A player without an
    /// entry is a human whose name is the recording's.
    std::vector<ReplayPlayerFacts> players{};
};

/// How a recording ended.
struct ReplayEnd {
    /// finished, left or cut.
    std::string ended{};
    /// The game's length in ticks, when known.
    std::optional<uint64_t> length_ticks{};
    /// True when the recording was finished after a crash.
    bool after_crash{};
};

/// What finishing a file did.
struct FinishResult {
    /// True when the finished file is in place.
    bool ok{};
    /// Why not, as a sentence for a log; empty on success.
    std::string error{};
    /// How many packets the finished recording holds.
    uint64_t packets{};
    /// The file the recording became: the destination, or, where the destination was taken and
    /// nothing was to be replaced, the destination with " (2)" and so on before its extension.
    std::filesystem::path placed{};
    /// True when nothing was written because the recording held no packet:
    /// `ok` is false then, and `error` says so.
    bool empty{};
    /// True when the finish failed because the disk refused a read, a write or a move, and a
    /// later try may do better; false when the recording itself cannot be finished (its header is
    /// not whole, it does not parse, it holds a packet that is not plain).
    bool transient{};
};

/// Describes a recording that is about to start, as far as the game knows.
///
/// Gives the description and the manifest of a replay, with the recording the
/// header alone (recording_header), so the replay is a start file that a
/// crash leaves for the next start. The recording's identity, packet count,
/// length and end are set when it is finished.
///
/// @param setup the game being recorded; its loadout and brains give the
///        mutators and brains, its unit checks the unit table's digest
/// @param facts what the game knows besides
/// @return the replay; its recording is the header's bytes
[[nodiscard]] formats::oademo::Replay
replay_begin(const RecordingSetup& setup, const ReplayFacts& facts);

/// Finds the end of the last whole packet of a recording that may be cut.
///
/// Walks the chunks from the start: the header, the sector count, the
/// sectors, the players, their statuses and the unit checks, then each packet.
/// A chunk that is not all there is not counted.
///
/// @param stream the bytes of a recording, which may end part way through a chunk
/// @param[out] packets how many whole packets it holds; may be null
/// @return the length of the stream up to its last whole packet; 0 when the
///         part before the packets is not whole
[[nodiscard]] std::size_t whole_packets_end(std::span<const uint8_t> stream, uint64_t* packets);

/// Finishes a recording stream into a `.oademo`.
///
/// Reads the stream, cuts it to its last whole packet, checks that it parses
/// and that every packet is plain, sets the replay's recording, packet count,
/// length and end, and writes the replay at zstd level 3 (the level a game
/// ends at) to a file beside the stream that then takes its place. The
/// stream file is removed. Nothing is changed on a failure. A recording that
/// holds no packet is not written: `empty` is set.
///
/// @param stream_file the recording stream
/// @param replay the replay replay_begin made, or a copy of it
/// @param end how the recording ended
/// @param destination the file to make
/// @param replace true replaces a file that is there; false never does, and takes the next free
///        name instead (`placed` says which)
/// @return whether the file is in place
[[nodiscard]] FinishResult finish_as_replay(
    const std::filesystem::path& stream_file,
    formats::oademo::Replay replay,
    const ReplayEnd& end,
    const std::filesystem::path& destination,
    bool replace = true
);

/// Finishes a recording stream that is already a `.tad` by moving it into place.
///
/// A stream cut part way through a packet is shortened to its last whole
/// packet first. Any other stream is moved as it is. The stream file is gone
/// when this returns true.
///
/// @param stream_file the recording stream
/// @param destination the file to make
/// @param keep_empty whether a recording with no packet is still made; when
///        not, nothing is written and `empty` is set
/// @param replace true replaces a file that is there; false never does, and takes the next free
///        name instead (`placed` says which)
/// @return whether the file is in place
[[nodiscard]] FinishResult finish_as_tad(
    const std::filesystem::path& stream_file,
    const std::filesystem::path& destination,
    bool keep_empty,
    bool replace = true
);

} // namespace oa::session::demo
