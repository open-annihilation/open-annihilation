// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One run of the helper: lock, wait, check, unpack, swap, start.
#pragma once

#include "oa/update_helper/verify.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace oa::update_helper {

/// Swapped the install and started the new version.
inline constexpr int exit_swapped = 0;
/// Refused the package. Nothing was changed. The old version was started
/// when it was not still running.
inline constexpr int exit_refused = 1;
/// The arguments or the plan cannot be used.
inline constexpr int exit_usage = 2;
/// A step failed, the finished steps were undone, and the old version was started.
inline constexpr int exit_undone = 3;

/// The sentence the window shows when a package cannot be checked.
inline constexpr std::string_view could_not_check =
    "The update could not be checked. Nothing was changed.";

/// Carries out the plan.
///
/// A plan that cannot be read returns exit_usage and starts nothing. A
/// readable plan that is refused starts the old version, without the update
/// flag, except when that version is still running.
///
/// @param plan_path the plan file
/// @param keys keys in place of the pinned ones; empty uses the pinned keys
/// @return an exit status
[[nodiscard]] int
run_plan(const std::filesystem::path& plan_path, const std::vector<FeedKey>& keys);

/// Undoes a journal left under `updates`.
///
/// @param updates the data folder's updates directory
/// @return exit_swapped when nothing is left to undo, exit_undone when it
///         could not be finished, exit_usage when the folder cannot be used
[[nodiscard]] int recover_updates(const std::filesystem::path& updates);

} // namespace oa::update_helper
