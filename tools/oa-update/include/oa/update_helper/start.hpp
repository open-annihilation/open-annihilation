// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Starting the game again, and running a journal step that has to wait.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Process;

namespace oa::update_helper {

/// Starts a program and keeps the handle, so a watch can see it end.
///
/// The arguments are the ones given and, when `add_updated_from` is set,
/// `--updated-from` and `from_version`. The program is a direct child, not a
/// background process: on POSIX a background process is started by a
/// short-lived child and SDL never learns the program's own pid, so a watch
/// would wait for a process it cannot see. Its standard streams are
/// discarded. The caller destroys the handle with SDL_DestroyProcess.
/// Destroying it does not kill the program.
///
/// @param program the program
/// @param arguments its arguments
/// @param add_updated_from true to add the flag
/// @param from_version the version the flag names
/// @param[out] error why it did not start; may be null
/// @return the process, or null when it did not start
[[nodiscard]] SDL_Process* start_process(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments,
    bool add_updated_from,
    std::string_view from_version,
    std::string* error
);

/// Starts a program and does not wait for it.
///
/// The program is detached. On success the arguments are the ones given,
/// and, when `add_updated_from` is set, `--updated-from` and `from_version`.
///
/// @param program the program
/// @param arguments its arguments
/// @param add_updated_from true to add the flag
/// @param from_version the version the flag names
/// @param[out] error why it did not start; may be null
/// @return true when it started
[[nodiscard]] bool start_detached(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments,
    bool add_updated_from,
    std::string_view from_version,
    std::string* error
);

/// Starts a program and waits until it has finished.
///
/// A non-zero status is a failure. Tests pass their own runner and do not
/// call this.
///
/// @param program the program
/// @param arguments its arguments
/// @param[out] error why it failed; may be null
/// @return true when it finished with status 0
[[nodiscard]] bool run_and_wait(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments,
    std::string* error
);

} // namespace oa::update_helper
