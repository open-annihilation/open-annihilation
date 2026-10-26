// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Windows install routes. The decisions, the operation lists and the
// WININIT.INI text are pure and built everywhere. Replacing the files
// runs on Windows only.
#pragma once

#include "oa/app/update/plan.hpp"
#include "oa/update_helper/journal.hpp"
#include "oa/update_helper/log.hpp"
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
/// are also in the install, except `Uninstall.exe`, `previous` and the
/// scratch file. When it is not a folder, the names are the install's top
/// entries except `previous` and the scratch file: the downloaded file is
/// the setup program, which has no zip listing. The last step runs the
/// setup. The caller runs that step itself and does not journal it.
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
/// @param plan the plan
/// @param staged the staging folder, holding one top folder
/// @param[out] why why no operations were produced; may be null; cleared on success
/// @return the operations; empty when `why` is set, or when there was nothing to move
[[nodiscard]] std::vector<Operation> windows_folder_operations(
    const oa::app::update::Plan& plan, const std::filesystem::path& staged, std::string* why
);

/// Puts kept program files back onto an install. `preferences.conf` is left.
///
/// A file is copied over the one in the install. A directory is copied over
/// entry by entry. Nothing the kept copy does not name is removed.
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
/// `open-annihilation.exe` changed. Declining the prompt changes nothing.
/// A folder is swapped in place. Elevation starts a second copy and this
/// copy starts the game. Windows 9x writes `WININIT.INI` and starts nothing.
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
