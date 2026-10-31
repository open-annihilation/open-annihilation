// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Windows install routes. The decisions, the operation lists and the
// WININIT.INI text are pure and built everywhere. Replacing the files
// runs on Windows only.
#pragma once

#include "oa/app/update/plan.hpp"
#include "oa/update_helper/journal.hpp"
#include "oa/update_helper/log.hpp"
#include "oa/update_helper/routes.hpp"
#include "oa/update_helper/verify.hpp"
#include "oa/update_helper/window.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::update_helper {

/// Sharing violation: a file is open in another program.
inline constexpr int file_held = 32;

/// The destination is already there.
inline constexpr int destination_exists = 183;

/// How many times a held file is tried.
inline constexpr int move_tries = 5;

/// The pause between those tries, in milliseconds.
inline constexpr int move_pause_ms = 500;

/// The sentence shown when the player declines the permission prompt.
inline constexpr std::string_view update_was_cancelled = "The update was cancelled.";

/// The sentence shown when Windows 9x still has to restart. `{version}` stays.
inline constexpr std::string_view restart_to_finish =
    "Restart Windows to finish installing {version}.";

/// True when `version` is a Windows 9x `GetVersion` value (the high bit is set).
///
/// @param version a `GetVersion` value; 0 is not Windows 9x
/// @return true for Windows 95, 98 and Me
[[nodiscard]] bool windows_9x(uint32_t version) noexcept;

/// True when `version` is Windows Vista or later.
///
/// The high bit is clear and the low byte, the major version, is at least 6.
/// Windows 9x and Windows XP are not. 0 is not.
///
/// @param version a `GetVersion` value
/// @return true for Vista, 7 and later
[[nodiscard]] bool windows_vista_or_later(uint32_t version) noexcept;

/// True when a folder swap must start a second, elevated helper.
///
/// Only when the folder cannot be written, and only on Vista and later.
/// Windows 9x and XP are never elevated. A writable folder is never elevated.
///
/// @param writable the install folder accepted a scratch file
/// @param version a `GetVersion` value
/// @return true when the swap asks for elevation
[[nodiscard]] bool folder_needs_elevation(bool writable, uint32_t version) noexcept;

/// The setup program's arguments. `/D=` is last and is not quoted.
///
/// Slashes become backslashes. A trailing separator is dropped, except on a
/// drive root such as `C:\`.
///
/// @param install_folder the install folder, as the plan stores it
/// @return `/S` and then `/D=` joined to that folder
[[nodiscard]] std::vector<std::string> setup_arguments(std::string_view install_folder);

/// One rename left for the next start of Windows.
struct PostponedRename {
    std::string from; ///< the file that is still in place
    std::string to;   ///< where a restart should move it
};

/// The `[rename]` section `WININIT.INI` carries.
///
/// Each line is the destination, `=`, the source, and CR LF. The key is the
/// destination.
///
/// @param rows the postponed renames, already in short names when Windows has them
/// @return the section text
[[nodiscard]] std::string wininit_rename_text(std::span<const PostponedRename> rows);

/// `restart_to_finish` with `{version}` replaced.
///
/// @param version the version being installed
/// @return the sentence
[[nodiscard]] std::string restart_line(std::string_view version);

/// Moves `from` onto `to`. Returns 0 when it moved, otherwise a system error.
using MoveFn =
    int (*)(void* context, const std::filesystem::path& from, const std::filesystem::path& to);

/// Waits between tries. Tests record the pause and return.
using PauseFn = void (*)(void* context, int milliseconds);

/// What one file's tries did.
struct MoveAttempt {
    bool moved{false};          ///< the file is at the destination
    bool postponed{false};      ///< Windows 9x will move it on restart
    int tries{0};               ///< how many times the move was called
    int error{0};               ///< the last error; 0 when it moved
    std::vector<int> pauses_ms; ///< the pauses that were asked for
};

/// Tries a move `move_tries` times, pausing `move_pause_ms` after a sharing
/// violation that is not the last try.
///
/// Any other error stops at once and is not postponed. After the last
/// sharing violation, Windows 9x postpones the rename and any other system
/// leaves it failed.
///
/// @param move the move; a null move fails at once
/// @param pause the wait; null does not wait
/// @param context passed to `move` and `pause`
/// @param on_windows_9x true to postpone a file that stays held
/// @param from the file to move
/// @param to where it should go
/// @return how the tries ended
[[nodiscard]] MoveAttempt move_with_retries(
    MoveFn move,
    PauseFn pause,
    void* context,
    bool on_windows_9x,
    const std::filesystem::path& from,
    const std::filesystem::path& to
);

/// A sharing violation on Windows 9x, or a destination that is still there
/// on Windows 9x, is postponed. Every other failure stays a failure.
///
/// @param attempt what the tries did
/// @param on_windows_9x true on Windows 9x
/// @return moved, postponed or failed
[[nodiscard]] MoveOutcome classify_move(const MoveAttempt& attempt, bool on_windows_9x) noexcept;

/// Copies the install's program files into `<updates>/previous` and records
/// the setup run.
///
/// When `staged` is one folder, the names are that folder's top entries that
/// are also in the install, except `Uninstall.exe`, `previous`, the scratch
/// file and this helper's own file (`oa-update.exe`). When it is not a
/// folder, the names are the install's top entries except `previous`, the
/// scratch file and that same helper file: the downloaded file is the setup
/// program, which has no zip listing. The helper is left out of the kept
/// set so an undo never has to replace the program that is running. The
/// last step runs the setup. The caller runs that step itself and does not
/// journal it.
///
/// @param plan the plan; its route is the installer
/// @param staged the staging folder, or an empty path when there is no zip
/// @param[out] why why no operations were produced; may be null; cleared on success
/// @return the operations; empty when `why` is set
[[nodiscard]] std::vector<Operation> windows_installer_operations(
    const oa::app::update::Plan& plan, const std::filesystem::path& staged, std::string* why
);

/// How a folder swap should run.
struct FolderPlan {
    bool elevate{false};                ///< start a second helper with `runas`
    std::string verb;                   ///< `runas` when elevating
    std::string program;                ///< that helper
    std::vector<std::string> arguments; ///< `--elevated-swap`, `--plan`, the plan
    std::vector<Operation> operations;  ///< the moves; empty when elevating
};

/// The folder swap: empty `<install>/previous`, move program files in, and
/// elevate only when the folder cannot be written on Vista or later.
///
/// Program files are the staged folder's top names. A file the package does
/// not carry stays. `previous` is not moved into itself.
///
/// @param plan the plan; its route is the folder
/// @param staged the staging folder, holding one top folder
/// @param writable the install folder accepted a scratch file
/// @param version a `GetVersion` value
/// @param helper this helper, started again when elevating
/// @param plan_file the plan file that second copy reads
/// @param[out] why why the swap was refused; may be null; cleared on success
/// @return the decision; operations are empty when `why` is set or when elevating
[[nodiscard]] FolderPlan plan_windows_folder(
    const oa::app::update::Plan& plan,
    const std::filesystem::path& staged,
    bool writable,
    uint32_t version,
    const std::filesystem::path& helper,
    const std::filesystem::path& plan_file,
    std::string* why
);

/// The moves of a folder swap, with no elevation.
///
/// The helper's own file is renamed with the other program files. The
/// process that applies the moves is a copy outside the install
/// (helper_aside_path), so the running file is not one of them. An undo
/// puts the old game, including that helper, back.
///
/// @param plan the plan
/// @param staged the staging folder, holding one top folder
/// @param[out] why why no operations were produced; may be null; cleared on success
/// @return the operations; empty when `why` is set, or when there was nothing to move
[[nodiscard]] std::vector<Operation> windows_folder_operations(
    const oa::app::update::Plan& plan, const std::filesystem::path& staged, std::string* why
);

/// True when `program` is this helper's file inside the install folder.
///
/// A folder swap renames that file. On Windows XP a running program cannot
/// be renamed, so the swap copies it out of the install and continues there.
///
/// @param program the running program
/// @param install the install folder
/// @return true for `oa-update` or `oa-update.exe` directly in `install`
[[nodiscard]] bool helper_image_blocks_swap(
    const std::filesystem::path& program, const std::filesystem::path& install
);

/// True when this process should hand the folder swap to a copy outside the
/// install.
///
/// A process that was already started as that copy (`helper_parent` is not
/// 0) carries on, so the hand-off happens once.
///
/// @param program the running program
/// @param install the install folder
/// @param helper_parent the process that copied this helper out; 0 for the first process
/// @return true when the caller should start the copy and leave
[[nodiscard]] bool folder_swap_must_step_aside(
    const std::filesystem::path& program,
    const std::filesystem::path& install,
    int64_t helper_parent
);

/// Where the copy runs.
///
/// `<updates>/oa-update-running.exe` is not a name the folder swap renames.
///
/// @param updates the data folder's updates directory
/// @return the copy's path
[[nodiscard]] std::filesystem::path helper_aside_path(const std::filesystem::path& updates);

/// The copy's arguments: the same plan and flags, plus `--helper-parent`.
///
/// @param plan_file the plan file
/// @param keys feed keys the copy must accept; empty for none
/// @param elevated_swap true to pass `--elevated-swap`
/// @param stage_only true to pass `--stage-only`
/// @param parent_pid the process the copy waits for
/// @return the arguments, without the program
[[nodiscard]] std::vector<std::string> helper_aside_arguments(
    const std::filesystem::path& plan_file,
    const std::vector<FeedKey>& keys,
    bool elevated_swap,
    bool stage_only,
    int64_t parent_pid
);

/// Copies the running helper to `to`, and each library beside the helper.
///
/// Windows loads a library from the program's own folder first. The copy
/// needs those libraries beside it, both so it can start and so the install's
/// libraries stay closed for the folder swap. The running file can be read.
/// An existing copy is replaced.
///
/// @param from the running helper
/// @param to the copy
/// @param[out] error why it could not; may be null
/// @return true when the copy is in place
[[nodiscard]] bool copy_helper_aside(
    const std::filesystem::path& from, const std::filesystem::path& to, std::string* error
);

/// This process's helper program. Empty when it cannot be named.
///
/// @return the program path
[[nodiscard]] std::filesystem::path running_helper_program();

/// This process's id.
///
/// @return the id; never 0
[[nodiscard]] int64_t helper_process_id() noexcept;

/// Moves `<install>/previous` back over the install, leaving preferences.conf.
///
/// @param state the record; its kept path is the previous folder
/// @param install the install folder
/// @param[out] why why no operations were produced; may be null
/// @return the operations
[[nodiscard]] std::vector<Operation> windows_folder_restore(
    const oa::app::update::State& state, const std::filesystem::path& install, std::string* why
);

/// Copies the kept installer files back and records the kept DisplayVersion.
///
/// The version step's program is `oa-display-version`. A test runs that step
/// itself. The helper's own runner writes the uninstall key on Windows and
/// succeeds without writing it elsewhere. `oa-update.exe` is not copied
/// back: that file is the program doing the undo.
///
/// @param state the record; its kept path is `<updates>/previous`
/// @param install the install folder
/// @param bad_version the version the step's undo records
/// @param[out] why why no operations were produced; may be null
/// @return the operations
[[nodiscard]] std::vector<Operation> windows_installer_restore(
    const oa::app::update::State& state,
    const std::filesystem::path& install,
    std::string_view bad_version,
    std::string* why
);

/// True when a Windows roll back has to run as the elevated copy.
///
/// Off Windows this is false. A folder that already accepts a file does not
/// elevate, and neither does Windows 9x.
///
/// @param install the install folder
/// @return true when the caller should relaunch with `--elevated-swap`
[[nodiscard]] bool windows_roll_back_elevate(const std::filesystem::path& install);

/// Starts this helper again with `--elevated-swap` and the same plan.
///
/// The elevated copy restores and does not start the game. Off Windows the
/// relaunch does not run.
///
/// @param plan_file the plan file
/// @return how the relaunch ended
[[nodiscard]] ElevatedRelaunch relaunch_elevated_plan(const std::filesystem::path& plan_file);

/// Writes DisplayVersion when `program` is `oa-display-version`.
///
/// @param program the journal step's program
/// @param arguments its arguments; the first is the version
/// @param[out] error why it failed; may be null
/// @return true when this was that program and the step can continue
[[nodiscard]] bool apply_display_version(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments,
    std::string* error
);

/// Puts kept program files back onto an install. `preferences.conf` is left,
/// and so is this helper's own file.
///
/// A file is copied over the one in the install. A directory is copied over
/// entry by entry. Nothing the kept copy does not name is removed. The
/// helper is not replaced, because the undo is that program.
///
/// @param previous the kept program files
/// @param install the install folder
/// @param[out] error why it could not; may be null
/// @return true when every kept program file is back
[[nodiscard]] bool restore_kept_copy(
    const std::filesystem::path& previous, const std::filesystem::path& install, std::string* error
);

/// What a leftover Windows swap means before this run undoes a journal.
enum class UpdatesGate : uint8_t {
    ready,          ///< a leftover journal may be undone
    restart,        ///< Windows 9x is waiting for a restart; the journal was dropped
    restore_failed, ///< the kept installer copy could not be put back; the journal stays
};

/// The result of looking for a swap a killed run left.
struct UpdatesGateResult {
    UpdatesGate gate{UpdatesGate::ready}; ///< what the caller should do
    std::string version;                  ///< the version a restart is finishing
    std::string detail;                   ///< why a restore failed; empty otherwise
};

/// Settles a marker a killed Windows swap left.
///
/// A reboot marker drops the journal and asks for a restart, so a deferred
/// Windows 9x swap is not undone. An installer marker puts the kept program
/// files back first; a failed restore leaves the journal. Postponed renames
/// without a reboot marker drop any `WININIT.INI` lines they added.
///
/// @param updates the data folder's updates directory
/// @return what the caller should do before undoing a journal
[[nodiscard]] UpdatesGateResult take_pending(const std::filesystem::path& updates);

/// Carries out a Windows route after the package has been checked.
///
/// The installer keeps program files under `<updates>/previous`, runs the
/// setup, and starts the game only when the setup exits 0 and
/// `open-annihilation.exe` changed. The helper's own file is not kept and
/// not put back. Declining the prompt changes nothing. A setup that fails
/// after files were changed puts the other files back and says so. A folder
/// is swapped in place. Elevation starts a second copy and this copy starts
/// the game. Windows 9x writes `WININIT.INI` and starts nothing.
/// `elevated_swap` never starts the game.
///
/// On a system that is not Windows, the route is refused.
///
/// @param plan the plan
/// @param verified the release the feed named
/// @param plan_path the plan file, for the elevated copy
/// @param stage the unpacked folder; ignored for an installer
/// @param elevated_swap true when this copy only swaps and exits
/// @param log the log
/// @param window the window
/// @param hooks the journal hooks; the note is kept
/// @return an exit status
[[nodiscard]] int finish_windows_route(
    const oa::app::update::Plan& plan,
    const Verified& verified,
    const std::filesystem::path& plan_path,
    const std::filesystem::path& stage,
    bool elevated_swap,
    Log& log,
    ProgressWindow& window,
    JournalHooks& hooks
);

} // namespace oa::update_helper
