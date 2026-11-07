// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A replay opened from outside Recordings: copied into Recordings/Imported,
// or selected when that recording is already there. A `.tad` is written with
// sector 6 left out and nothing else changed. A `.oademo` whose recording
// holds sector 6 is rewritten without it. The identity is the SHA-256 of the
// stream as kept.
#pragma once

#include <filesystem>
#include <string>

namespace oa::data::replays {

/// What became of one replay opened from outside Recordings.
struct ImportResult {
    /// The file was a replay this game keeps, whether or not a copy was written.
    bool ok{false};
    /// A file was written. False when the same identity was already under Recordings.
    bool copied{false};
    /// The replay, relative to Recordings, '/' between folders. Empty when `ok` is false.
    std::string path{};
    /// The identity, lower-case hexadecimal. Empty when `ok` is false.
    std::string sha256{};
    /// The map's name, or the file's name when the recording names none.
    std::string title{};
    /// The sentence a player sees, in English, with `{version}` filled. Empty when `ok`.
    std::string error{};
    /// The catalogue sentence, unfilled. An `.oademo` the container refuses carries the
    /// container's own sentence. Empty when `ok`.
    std::string sentence{};
    /// The format version when `sentence` names `{version}`. Empty otherwise.
    std::string version{};
};

/// Copies a `.oademo` or a `.tad` into `Recordings/Imported`, or selects the replay
/// already there with the same identity.
///
/// A `.oademo` is recognised and then read, and a refusal is that read's sentence.
/// A `.tad` that is not a TA Demo recording, or whose version this game does not
/// play, is refused with the sentence that says so. Any other file that does not
/// parse is refused with `tad_unreadable`, `file_unreadable` or `file_too_large`.
///
/// @param recordings the Recordings folder; `Imported` is made under it
/// @param source the file that was opened
/// @return what became of it; never throws
[[nodiscard]] ImportResult
import_replay(const std::filesystem::path& recordings, const std::filesystem::path& source);

} // namespace oa::data::replays
