// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle's recording on this computer, which Share… after a battle opens
// in the Library (runtime_library.cpp). Network play names its game's
// recording here as the recording starts, when it is refused, and when a
// finished game's file is renamed; a game that is not recorded names none.
// The header includes no other header of the engine's.
#pragma once

#include <filesystem>

namespace oa::app {

class Runtime;

/// Remembers the battle recording. A rename updates a Library that is still
/// waiting to select the earlier path. An empty path is remembered too, and
/// clears that wait when it still names the previous file.
///
/// @param runtime the running app
/// @param path the recording, as a full path; empty when this game is not recorded
void note_battle_recording(Runtime& runtime, std::filesystem::path path);

} // namespace oa::app
