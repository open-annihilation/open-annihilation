// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One index pass for the Library and the playback bar. Both start it here,
// so the program, the low priority, the pass's own preferences and the mod
// folder cannot drift apart.
#pragma once

#include <SDL3/SDL_process.h>

#include <filesystem>
#include <string>

namespace oa::app {

namespace fs = std::filesystem;

/// What one index pass is asked to read.
struct IndexPassRequest {
    fs::path recording{};                ///< the replay file
    fs::path user_folder{};              ///< the player's folder the pass plays in
    fs::path game_dir{};                 ///< the game's folder; empty is left off
    fs::path mod_folder{};               ///< the mod; empty is left off
    fs::path map_pack{};                 ///< the kept map pack to mount; empty is left off
    fs::path scratch{};                  ///< the pass's own folder
    std::string output_name{"out.json"}; ///< the timeline file's name, in scratch
    bool pipe_stdout{false};             ///< the bar reads progress; the Library discards it
};

/// The process launch_index_pass started, and the files it writes.
struct IndexPassHandles {
    SDL_Process* process{nullptr}; ///< the process; null when it did not start
    int pid{0};                    ///< the id; 0 when the system named none
    fs::path output{};             ///< the timeline file
    fs::path errors{};             ///< standard error; empty when it went nowhere
    bool started{false};           ///< the process started
};

/// Starts one index pass at low priority.
///
/// The pass is this program, with a preferences file of its own in `scratch`
/// that asks for no update. Standard error goes to a file there. Standard
/// output is discarded unless `pipe_stdout` is set, and that pipe does not
/// block the caller. The pass is not a background process: that drops the
/// exit code.
///
/// @param request what to read
/// @return the process; `started` is false when it did not start
[[nodiscard]] IndexPassHandles launch_index_pass(const IndexPassRequest& request);

/// The process id SDL reports, or 0 when it names none.
///
/// @param process the process
/// @return the id
[[nodiscard]] int index_process_pid(SDL_Process* process);

/// The line a pass wrote when it could not read a replay.
///
/// Progress and the playback's own lines are left out.
///
/// @param file the pass's standard error
/// @return the reason; empty when the file gives none
[[nodiscard]] std::string index_pass_reason(const fs::path& file);

} // namespace oa::app
