// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One run of the helper: lock, wait, check, unpack, swap, start.
#pragma once

#include "oa/app/update/plan.hpp"
#include "oa/update_helper/journal.hpp"
#include "oa/update_helper/verify.hpp"
#include "oa/update_helper/wait.hpp"

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
/// Only from the macOS copy with an administrator's rights: the bundle it
/// put in place could not be started by the player, so every step of the
/// journal was undone. The run that asked records player_cannot_run.
inline constexpr int exit_not_runnable = 4;

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

/// The sentence the window shows when an update stopped before anything
/// changed for a reason the player can try again.
inline constexpr std::string_view update_not_installed =
    "The update was not installed. Nothing was changed.";

/// What the start of a run found, before any file of the install moved.
enum class Opening : uint8_t {
    carry_on,        ///< nothing else runs from the install, and what a killed run left is settled
    game_running,    ///< the game that wrote the plan did not close
    copies_running,  ///< another program of the install did not close
    restart_windows, ///< Windows 9x finishes an earlier swap when it next starts
    restore_failed,  ///< the files kept before a setup that did not finish could not be put back
    undo_failed,     ///< a journal a killed run left could not be undone
};

/// What open_run found.
struct OpeningResult {
    Opening opening{Opening::carry_on}; ///< what the run does next
    std::string version;                ///< the version a restart finishes; empty otherwise
    std::string detail;                 ///< why the run stops, in English; empty when it carries on
};

class ProgressWindow;
struct PutBackHooks;

/// The waits a test injects, and the window. Null members are the real waits
/// and no window.
struct OpeningHooks {
    const WaitHooks* game{};     ///< the clock and the check for the game's own process
    const CopiesHooks* copies{}; ///< the clock, the process list and the line for other copies
    /// The Windows version, the write check and the copy with raised rights
    /// that puts a setup's kept files back.
    const PutBackHooks* put_back{};
    /// The window, which lets go of the installed font files while what a
    /// killed run left is put back.
    ProgressWindow* window{};
};

/// Waits for the game that wrote the plan, then for every other program of
/// the install, and only then settles what a killed run left: the restart
/// Windows 9x still owes, the files kept before a setup that did not finish,
/// and a journal. Nothing of the install moves until both waits have ended.
///
/// A copy started with `--elevated-swap` does not wait for other programs:
/// the run that started it has waited, and may itself run from the install.
///
/// @param plan the plan
/// @param elevated_swap true for the copy that only swaps and starts nothing
/// @param own_pid this helper's process, which is not another copy
/// @param hooks test waits and the window; default members for the real waits
/// @param journal the hooks a journal a killed run left is undone with
/// @return what the run does next
[[nodiscard]] OpeningResult open_run(
    const oa::app::update::Plan& plan,
    bool elevated_swap,
    int64_t own_pid,
    const OpeningHooks& hooks,
    const JournalHooks& journal
);

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

/// Puts back the files kept before a Windows setup that did not finish, for
/// the run that started this copy with raised rights (`--put-back`).
///
/// The copy puts the kept files back into the install the marker names and
/// removes the marker (put_back_marked). It writes no receipt and starts
/// nothing. The run that started it holds the lock, so a lock held by
/// another run does not stop it.
///
/// @param updates the data folder's updates directory
/// @return exit_swapped when nothing is left to put back, exit_undone when
///         the files are not all back, exit_usage when the folder cannot be used
[[nodiscard]] int put_back_updates(const std::filesystem::path& updates);

/// Undoes a journal left under `updates`.
///
/// @param updates the data folder's updates directory
/// @return exit_swapped when nothing is left to undo, exit_undone when it
///         could not be finished, exit_usage when the folder cannot be used
[[nodiscard]] int recover_updates(const std::filesystem::path& updates);

} // namespace oa::update_helper
