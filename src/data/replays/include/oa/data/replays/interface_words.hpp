// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// English sentences a player can see when the Library cannot read a replay
// file. oa-tool joins this list with the other modules' lists. The module
// stores these sentences itself in a replay's `damage`; it does not look them
// up. A `.oademo` the container refuses carries the container's own sentence
// (oa/formats/oademo/interface_words.hpp).

#include <string_view>

namespace oa::data::replays {

/// A `.tad` that does not parse as a TA Demo recording.
inline constexpr std::string_view tad_unreadable = "This recording cannot be read.";

/// A file that cannot be opened or read to its end.
inline constexpr std::string_view file_unreadable = "This file cannot be read.";

/// A file larger than most_file_bytes, which is never read.
inline constexpr std::string_view file_too_large = "This file is too large to be a replay.";

/// A `.tad` whose bytes are not a TA Demo recording.
inline constexpr std::string_view tad_not_a_recording = "This file is not a TA Demo recording";

/// Recordings/Imported could not be made, or the copy could not be written.
inline constexpr std::string_view replay_not_copied =
    "This replay could not be copied into Recordings.";

/// A TA Demo recording whose version this game does not play. `{version}` is the number.
inline constexpr std::string_view tad_unsupported_version =
    "This recording is in version {version} of the format, which Open Annihilation does not play";

/// The notice when a replay opened from outside is copied into Replays. `{title}` is the map.
inline constexpr std::string_view added_to_replays = "Added {title} to Replays";

/// English texts this module passes to the interface catalogue
/// (oa/data/languages/interface_text.hpp). oa-tool sorts the modules'
/// lists together and keeps one of each text.
inline constexpr std::string_view interface_words[] = {
    tad_unreadable,
    file_unreadable,
    file_too_large,
    tad_not_a_recording,
    tad_unsupported_version,
    replay_not_copied,
    added_to_replays,
};

} // namespace oa::data::replays
