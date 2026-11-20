// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// A TA Demo recording described as a .oademo, the way `oa-tool replay pack`
// describes one: sides and colours from each player's lobby block, mutators
// and brains from their sectors, names turned from the game's code page into
// UTF-8. The recording is stored plain, with no network address. The app and
// the tool both call this, so a .tad wrapped as a .oademo matches a pack of
// the same file.

#include "oa/formats/oademo/oademo.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace oa::session::demo {

/// A recording described as a replay, or why it could not be.
struct ImportedRecording {
    /// The replay. Empty when `error` is set.
    formats::oademo::Replay replay{};
    /// Why the recording could not be described. Empty when it was.
    std::string error{};

    /// Tells whether the recording was described.
    ///
    /// @return true when `error` is empty
    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

/// Describes a TA Demo recording as a `.oademo` replay.
///
/// The recording is stored as plain packets, with no network address and
/// with Open Annihilation's own sectors kept. A player's side and colour
/// come from the lobby block, as playback binds them. A loadout this build
/// can read becomes manifest needs, with each package's digest. One it
/// cannot still names its mutators, and adds no need. Brains are named and
/// are not needs. `engine.needs` is the first release that reads a replay.
/// `engine.recorded_with` is `engine_version`. Names are not cut.
///
/// @param bytes a whole recording
/// @param engine_version the version of the build that describes it
/// @return the replay, or the recording format's sentence when it cannot be read
[[nodiscard]] ImportedRecording
describe_imported_recording(std::span<const uint8_t> bytes, std::string_view engine_version);

} // namespace oa::session::demo
