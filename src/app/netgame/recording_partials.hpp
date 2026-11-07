// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The recordings of games not yet finished. A recording is streamed to
// Recordings/.replays/partial/<mod>/ as the game goes, with the files that
// say how to finish it; when the game ends it is finished into its place in
// Recordings, and one a crash left is finished by the next start. A game
// holds a lock on its files while it records, so that a start never finishes
// the recording of a game that is still being played, in this process or
// another.
#pragma once

#include "oa/app/package_install.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/session/demo/replay_record.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

/// How a game is recorded.
enum class RecordingLayout : uint8_t {
    none,     ///< the game is not recorded
    hack_tad, ///< the recorder hack's `.tad`
    replay,   ///< a `.oademo`
};

/// What decides how a game is recorded, and where.
struct RecordingPlan {
    /// The layout the game is written in.
    RecordingLayout layout{RecordingLayout::none};
    /// The command line named the file, so it is replaced instead of renamed.
    bool named{};
};

/// Decides whether a game is recorded, and in which layout.
///
/// A game is recorded when `--record-game` names a file (a `.oademo`, or the
/// recorder hack's layout when the name ends in `.tad`), when `--net-record`
/// names one (the hack's layout), when the recorder hack presents a recorder
/// (the hack's layout, the player's setting notwithstanding), or when the
/// player's setting keeps every game (a `.oademo`). Never in two layouts.
///
/// @param recorder_protocol the game presents a recorder, as the profile's recorder hack makes it
/// @param net_record the file --net-record names; empty for none
/// @param record_game the file --record-game names; empty for none
/// @param by_setting the player's setting keeps this game, which a run nobody plays
///        (Runtime::records_games_by_setting) never says
/// @return the layout, and whether the file was named
[[nodiscard]] RecordingPlan plan_recording(
    bool recorder_protocol,
    const std::filesystem::path& net_record,
    const std::filesystem::path& record_game,
    bool by_setting
);

/// What a recording's stream file name ends with, after the extension of its layout.
inline constexpr std::string_view partial_stream_suffix = ".part";
/// The extension a replay's stream carries before partial_stream_suffix, whatever the file it
/// becomes is called. A stream's layout is told by this and nothing else: a replay's
/// description file may be lost, and the stream must never be mistaken for the recorder
/// hack's.
inline constexpr std::string_view partial_replay_extension = ".oademo";
/// The extension the recorder hack's stream carries before partial_stream_suffix.
inline constexpr std::string_view partial_tad_extension = ".tad";
/// What the file that holds a replay's description and manifest ends with; the
/// recording's own name comes before it. Only a replay layout has one.
inline constexpr std::string_view partial_start_suffix = ".start";
/// What the file that holds the content note's text ends with.
inline constexpr std::string_view partial_note_suffix = ".note";
/// What the file a game holds a lock on while it records ends with.
inline constexpr std::string_view partial_lock_suffix = ".lock";
/// What a stream file that could not be finished is renamed to end with, so
/// that no start tries it again.
inline constexpr std::string_view partial_failed_suffix = ".failed";

/// The files of one recording being made, which share a name.
struct PartialFiles {
    /// The recording so far: a `.tad` stream.
    std::filesystem::path stream{};
    /// The replay's description and manifest as the game knew them at the start.
    std::filesystem::path start{};
    /// The content note's text.
    std::filesystem::path note{};
    /// The file the game locks while it records.
    std::filesystem::path lock_file{};
};

/// A recording being made on disk, and the lock that says its game is still on.
struct PartialRecording {
    /// Its files.
    PartialFiles files{};
    /// The hold on the lock file; released when this is destroyed.
    std::unique_ptr<oa::app::package_install::FileLock> lock{};
};

/// Tells which layout a stream file holds, by the extension before its partial_stream_suffix.
///
/// @param stream the stream file
/// @return replay for a name that ends `.oademo.part`, hack_tad for `.tad.part`, in any case of
///         letters, and none for any other name
[[nodiscard]] RecordingLayout partial_layout_of(const std::filesystem::path& stream);

/// Names the files of a recording by its stream file.
///
/// @param stream the stream file, whose name ends with partial_stream_suffix
/// @return the other files, named from the same stem
[[nodiscard]] PartialFiles partial_files_of(const std::filesystem::path& stream);

/// Makes the place for a recording being made and takes its lock.
///
/// The folder is Recordings/.replays/partial/<mod folder> in the player's own
/// folder. The files are named for the file the recording will become, with
/// the extension of the layout after it unless the name ends so already: a
/// replay's stream ends `.oademo.part`, the recorder hack's `.tad.part`, so
/// that a recording finished after a crash is known by its name. A name
/// already taken, by a recording left by a crash that has not been finished
/// yet, takes " (2)", and so on.
///
/// @param user_folder the player's own folder
/// @param mod_id the id of the mod's profile; empty without one
/// @param final_name the name the finished file will have
/// @param layout replay or hack_tad
/// @param[out] error why it could not be made; may be null
/// @return the recording's files with its lock held; null on a failure
[[nodiscard]] std::unique_ptr<PartialRecording> partial_make(
    const std::filesystem::path& user_folder,
    std::string_view mod_id,
    const std::filesystem::path& final_name,
    RecordingLayout layout,
    std::string* error
);

/// Writes a replay's description and manifest as they stand at the start of the game.
///
/// The file is a replay whose recording is the header alone (replay_begin),
/// so a crash leaves what finish_partial_recordings needs. It is written before the stream
/// is made: a game whose description cannot be kept is not recorded.
///
/// @param files the recording's files
/// @param start the replay replay_begin made
/// @return false when it could not be written
[[nodiscard]] bool
partial_write_start(const PartialFiles& files, const oa::formats::oademo::Replay& start);

/// Gives up a recording that could not be started: releases its lock and removes its files.
///
/// @param partial the recording's files, as partial_make made them
void partial_discard(PartialRecording& partial);

/// Keeps the content note beside the recording.
///
/// Nothing is written for an empty note.
///
/// @param files the recording's files
/// @param note the note, as `Runtime::content_note_text` gives it
/// @return false when a note was to be written and could not be
[[nodiscard]] bool partial_write_note(const PartialFiles& files, std::string_view note);

/// What finishing a recording does.
struct PartialFinish {
    /// The recording's files.
    PartialFiles files{};
    /// True for a `.oademo`; false for the recorder hack's `.tad`.
    bool replay_layout{};
    /// The replay as the game knew it at the start, for the replay layout.
    oa::formats::oademo::Replay start{};
    /// How the recording ended, for the replay layout.
    oa::session::demo::ReplayEnd end{};
    /// The file the recording becomes.
    std::filesystem::path destination{};
    /// Whether a file that is there is replaced, as a file the command line named is; when not,
    /// the recording takes the next free name instead (`FinishResult::placed`).
    bool replace{true};
    /// The recording was cut by a crash: a `.tad` is cut to its last whole packet.
    bool cut{};
};

/// Finishes a recording into its place.
///
/// The stream becomes the destination (finish_as_replay, finish_as_tad), the
/// content note, when there is one, becomes `<file>.oa-content` beside the file made, and
/// the other files are removed. Nothing is removed on a failure.
///
/// @param finish what to finish and how
/// @return whether the destination is in place, and how many packets it holds
[[nodiscard]] oa::session::demo::FinishResult finish_partial(const PartialFinish& finish);

/// What finishing the recordings a crash left did.
struct PartialsFinished {
    /// Recordings finished into Recordings.
    std::size_t finished{};
    /// Recordings dropped because they held no packet.
    std::size_t empty{};
    /// Recordings that could not be finished; their streams are renamed to end with
    /// partial_failed_suffix.
    std::size_t failed{};
    /// Recordings left as they were for a later start, because the disk refused a read or a write.
    std::size_t postponed{};
    /// One line for each, for the log.
    std::vector<std::string> lines{};
};

/// Finishes every recording a crash left in the player's own folder.
///
/// A recording whose lock is held is the recording of a game still being
/// played and is left alone. The others are finished as game ended
/// `cut`, in the layout their stream's name says (partial_layout_of): a
/// replay with `recorded.after_crash` true, or a `.tad` cut to its last whole
/// packet, each in Recordings/<mod folder>. A replay's stream whose
/// description is not there or cannot be read, and a stream whose name tells
/// no layout, is never put in Recordings: it is kept under the name ending
/// partial_failed_suffix. A stream that could not be read or written for a
/// reason of the disk is left as it is, for a later start. A folder with no
/// partial recordings is not touched.
///
/// @param user_folder the player's own folder
/// @return what was done
[[nodiscard]] PartialsFinished finish_partial_recordings(const std::filesystem::path& user_folder);

} // namespace oa::app
