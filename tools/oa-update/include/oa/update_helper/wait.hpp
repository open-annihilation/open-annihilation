// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Waiting until the game's process has left, and until no other copy of the
// game runs from the install folder. A process id of 0 waits for nothing.
// Each wait gives up after a minute.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::update_helper {

/// How long a wait lasts before it gives up, in milliseconds.
inline constexpr int64_t wait_limit_ms = 60000;

/// How often a wait looks again, in milliseconds.
inline constexpr int64_t wait_slice_ms = 100;

/// How the wait ended.
enum class WaitOutcome : uint8_t {
    finished,      ///< the process is gone, or there was nothing to wait for
    still_running, ///< it was still there when the minute ran out
};

/// The sentence the window shows when the game is still running.
inline constexpr std::string_view still_running_text = "Open Annihilation is still running.";

/// The window line while other copies of the game are still open.
inline constexpr std::string_view close_other_windows_text =
    "Close the other Open Annihilation windows.";

/// Why the update stopped when other copies of the game stayed open.
inline constexpr std::string_view other_windows_open_text =
    "Other Open Annihilation windows are still open.";

/// True where the system compares file names without regard to letter case.
#if defined(_WIN32) || defined(__APPLE__)
inline constexpr bool program_paths_ignore_case = true;
#else
inline constexpr bool program_paths_ignore_case = false;
#endif

/// A clock and an answer a test injects. Null hooks use the real clock and process.
struct WaitHooks {
    void* context{}; ///< passed back to the hooks
    /// Milliseconds since an arbitrary start.
    int64_t (*now_ms)(void* context){};
    /// Advance, or pause, by this many milliseconds.
    void (*sleep_ms)(void* context, int64_t milliseconds){};
    /// True while the process is still there. Not called for process 0.
    bool (*alive)(void* context, int64_t pid){};
};

/// Waits until `pid` is gone, or for a minute.
///
/// Process 0 returns finished and does not ask whether anything is alive.
/// A process that is still there sets `error` to still_running_text.
///
/// @param pid the process; 0 waits for nothing
/// @param hooks a test clock; null for the real wait
/// @param[out] error the sentence when the game is still running; may be null
/// @return how the wait ended
[[nodiscard]] WaitOutcome wait_for_process(int64_t pid, const WaitHooks* hooks, std::string* error);

/// One running process and the program it runs.
struct RunningProgram {
    int64_t pid{};       ///< the process id
    std::string program; ///< the program's full path, in UTF-8; empty when it could not be read
};

/// The processes among `running` whose program lies inside `install`,
/// except `own_pid`.
///
/// A program directly in the folder counts, and so does one in a folder
/// inside it, such as a macOS bundle's `Contents/MacOS`. Either slash
/// separates names, a trailing separator and a leading `\\?\` are ignored,
/// and with `ignore_case` ASCII letters match in either case. A program
/// whose path could not be read, and a process id of 0 or less, are left
/// out.
///
/// @param running the processes, as the system lists them
/// @param install the install folder, as the plan stores it
/// @param own_pid this helper's process, which is never counted
/// @param ignore_case true where file names ignore letter case
/// @return the process ids, in the order listed
[[nodiscard]] std::vector<int64_t> copies_in_install(
    std::span<const RunningProgram> running,
    std::string_view install,
    int64_t own_pid,
    bool ignore_case
);

/// The processes running on this machine with their programs.
///
/// On Windows the list comes from the system's process snapshot, with each
/// program's full path where the system gives it; on macOS and Linux from
/// the system's process table. A process whose program cannot be read has
/// an empty path. Empty where the system cannot list processes.
///
/// @return the processes
[[nodiscard]] std::vector<RunningProgram> running_programs();

/// A clock, a process list and a window line a test injects. Each null
/// member is the real one; a null `waiting` shows nothing.
struct CopiesHooks {
    void* context{}; ///< passed back to the hooks
    /// Milliseconds since an arbitrary start.
    int64_t (*now_ms)(void* context){};
    /// Advance, or pause, by this many milliseconds.
    void (*sleep_ms)(void* context, int64_t milliseconds){};
    /// The processes running now.
    std::vector<RunningProgram> (*running)(void* context){};
    /// Called on each look that still finds another copy, before the pause.
    void (*waiting)(void* context){};
};

/// Waits until no process other than this one runs a program from
/// `install`, or for a minute.
///
/// Every program of the install counts: another copy of the game, its
/// tools, a second helper. A copy that is still there sets `error` to
/// other_windows_open_text. When nothing runs from the install the wait
/// returns at once, without calling `waiting`.
///
/// @param install the install folder, as the plan stores it
/// @param own_pid this helper's process
/// @param hooks a test clock and process list; null for the real ones
/// @param[out] error the sentence when copies are still open; may be null
/// @return how the wait ended
[[nodiscard]] WaitOutcome wait_for_other_copies(
    std::string_view install, int64_t own_pid, const CopiesHooks* hooks, std::string* error
);

} // namespace oa::update_helper
