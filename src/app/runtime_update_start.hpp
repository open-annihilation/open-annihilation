// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The count of starts that did not reach the main menu. A start of a newly
// installed version counts until it reaches the main menu; a start the
// player ends before then takes its count back, so only a crash, or an error
// that stops the game, counts towards putting the kept version back.
// runtime_update.cpp counts a start on macOS and Windows; the Linux launcher
// counts in `update-starts`. runtime_update_install.cpp takes a quit back.
#pragma once

#include "oa/app/app.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace oa::app {

/// The Linux launcher's count of unconfirmed starts, beside `versions/`.
///
/// The program sits in `versions/<version>` or behind the `current` link.
///
/// @return the file; empty off Linux, or when the program's folder cannot be named
[[nodiscard]] std::filesystem::path update_starts_file();

/// Takes back this start's count after the player ended it before the main menu.
///
/// Called as a run that was not a crash ends with status 0. On macOS and
/// Windows, a start the guard counted is uncounted in `state.json`, which
/// also records the quit for the helper watching the start. On Linux the
/// launcher's `update-starts` goes down by one, and `state.json` records the
/// quit. A version that already reached the main menu, and a run nothing
/// counted, are left as they are.
///
/// @param options the run's options, which name the data folder
void release_quit_start(const Options& options);

#if defined(_WIN32)
/// Starts the helper and does not wait for it.
///
/// The helper is not given this process's open files
/// (`update::helper_inherits_open_files`). On Windows an open file keeps its
/// folder from being renamed, and the fonts stay open until this process
/// leaves.
///
/// @param helper the helper program
/// @param arguments the helper's arguments, without the program
/// @param[out] why why it did not start; unchanged when it did
/// @return true when the helper was started
[[nodiscard]] bool start_helper_process(
    const std::filesystem::path& helper, const std::vector<std::string>& arguments, std::string& why
);
#endif

} // namespace oa::app
