// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One run of the helper: lock, wait, check, unpack, swap, start.
#pragma once

#include "oa/app/update/plan.hpp"
#include "oa/update_helper/journal.hpp"
#include "oa/update_helper/verify.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
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

/// The sentence the window shows when a step fails after files were changed
/// and those changes were undone.
inline constexpr std::string_view update_failed_put_back =
    "The update failed. The game was put back.";

/// The sentence the window shows when a step fails after files were changed
/// and those changes could not be undone.
inline constexpr std::string_view update_failed_not_put_back =
    "The update failed. The game could not be put back.";

/// The window line after an update has started changing files and then failed.
///
/// A journal still on disk means the undo did not finish, so the line does
/// not say the game was put back. No journal means the finished steps were
/// undone.
///
/// @param updates the data folder's updates directory
/// @return the sentence
[[nodiscard]] inline std::string_view
line_after_failed_change(const std::filesystem::path& updates) {
    std::error_code io;
    if (std::filesystem::is_regular_file(updates / "journal.json", io) && !io)
        return update_failed_not_put_back;
    return update_failed_put_back;
}

/// The sentence the window shows when a roll back fails and the steps it
/// had finished were undone, so the version that was running stays.
inline constexpr std::string_view roll_back_failed_kept =
    "The roll back failed. Nothing was changed.";

/// The sentence the window shows when a roll back fails and the steps it
/// had finished could not be undone.
inline constexpr std::string_view roll_back_failed_not_put_back =
    "The roll back failed. The game could not be put back.";

/// The window line after a roll back has started changing files and then
/// failed, read from the journal as line_after_failed_change reads it.
///
/// @param updates the data folder's updates directory
/// @return the sentence
[[nodiscard]] inline std::string_view
line_after_failed_roll_back(const std::filesystem::path& updates) {
    std::error_code io;
    if (std::filesystem::is_regular_file(updates / "journal.json", io) && !io)
        return roll_back_failed_not_put_back;
    return roll_back_failed_kept;
}

/// How the helper starts a version of the game.
enum class StartWay : uint8_t {
    program,     ///< the plan's program, started directly
    open_bundle, ///< the macOS bundle, opened through Launch Services as the Finder opens it
    refused,     ///< nothing: the game never starts with an administrator's rights
};

/// How the helper starts the game for `plan`, the new version or the one
/// that was running.
///
/// A macOS bundle is opened through Launch Services: a program macOS starts
/// directly is refused the player's Documents folder, and the game would
/// take the player's copy for missing. A process that holds an
/// administrator's rights starts nothing.
///
/// @param plan the plan
/// @param administrator this process holds an administrator's rights
/// @return the way
[[nodiscard]] StartWay start_way(const oa::app::update::Plan& plan, bool administrator) noexcept;

/// Replaces the kept preferences copy and records the kept version.
///
/// An existing `updates/previous/preferences.conf` is moved aside before the
/// new copy, and `state.json` is replaced. Undoing the journal puts the
/// earlier copy and the earlier record back. The route's own operations
/// move an older kept version aside the same way.
///
/// @param plan the plan; its preferences file is the new copy
/// @param installed_version the version being installed
/// @param installed_release that version's release number
/// @return the operations, in the order to append after the route's own
[[nodiscard]] std::vector<Operation> kept_record_operations(
    const oa::app::update::Plan& plan, std::string_view installed_version, int64_t installed_release
);

/// Carries out the plan.
///
/// A plan that cannot be read returns exit_usage and starts nothing. A
/// readable plan that is refused starts the old version, without the update
/// flag, except when that version is still running.
///
/// @param plan_path the plan file
/// @param keys keys in place of the pinned ones; empty uses the pinned keys
/// @param elevated_swap true when this process only swaps a folder and does not start the game
/// @param stage_only true to stage a Linux update and not start the game; a
///        self-check build's `--stage-only`. Ignored for any other route
/// @param helper_parent the process that copied this helper out of the install;
///        0 when this is that first process. The copy waits for it, then swaps
/// @return an exit status
[[nodiscard]] int run_plan(
    const std::filesystem::path& plan_path,
    const std::vector<FeedKey>& keys,
    bool elevated_swap,
    bool stage_only,
    int64_t helper_parent
);

/// Undoes a journal left under `updates`.
///
/// @param updates the data folder's updates directory
/// @return exit_swapped when nothing is left to undo, exit_undone when it
///         could not be finished, exit_usage when the folder cannot be used
[[nodiscard]] int recover_updates(const std::filesystem::path& updates);

} // namespace oa::update_helper
