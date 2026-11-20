// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// English sentences a player can see when a replay cannot be read. oa-tool
// joins this list with the other modules' lists. The reader returns these
// sentences itself; it does not look them up. The sentences `write` returns
// come after the list: a tool or a writer's own code reaches them, and no
// player is shown one.

#include <string_view>

namespace oa::formats::oademo {

/// The file is not a zip archive, or its first entry is not the stored `oademo`.
inline constexpr std::string_view not_a_replay = "This file is not a replay.";

/// The opening entry names a version other than 1, or the entries that follow
/// it are not the ones this version holds.
inline constexpr std::string_view not_this_version =
    "This replay is not a version this game reads.";

/// The archive holds neither recording, or both.
inline constexpr std::string_view not_one_recording = "This replay does not hold one recording.";

/// The recording's digest or its byte count does not match the description.
inline constexpr std::string_view recording_mismatch = "This replay's recording does not match.";

/// The recording is longer than the recording format allows.
inline constexpr std::string_view recording_too_long =
    "This replay's recording is too long to play.";

/// The description is missing or is not the JSON this version reads.
inline constexpr std::string_view details_unreadable = "This replay's details cannot be read.";

/// The recording's frame is damaged.
inline constexpr std::string_view recording_unreadable = "This replay's recording cannot be read.";

/// English texts this module passes to the interface catalogue
/// (oa/data/languages/interface_text.hpp). oa-tool sorts the modules'
/// lists together and keeps one of each text.
inline constexpr std::string_view interface_words[] = {
    not_a_replay,
    not_this_version,
    not_one_recording,
    details_unreadable,
    recording_unreadable,
    recording_mismatch,
    recording_too_long,
};

/// The timeline is not one JSON value. `write` returns it.
inline constexpr std::string_view timeline_unwritable = "The timeline is not one JSON value.";

/// The compression level is outside what a replay takes. `write` returns it.
inline constexpr std::string_view level_refused =
    "The compression level is not one this replay accepts.";

/// The recording could not be compressed or stored. `write` returns it.
inline constexpr std::string_view recording_unwritable =
    "This replay's recording cannot be written.";

/// The archive could not be put together. `write` returns it.
inline constexpr std::string_view replay_unwritable = "This replay could not be written.";

/// The recording is not a TA Demo recording. `write` returns it.
inline constexpr std::string_view recording_not_a_recording =
    "This replay's recording is not a TA Demo recording.";

/// The recording holds a sector of network addresses, which a replay never
/// holds. `write` returns it.
inline constexpr std::string_view recording_holds_addresses =
    "This replay's recording holds network addresses, and a replay holds none.";

} // namespace oa::formats::oademo
