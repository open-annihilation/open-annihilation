// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Waiting until the game's process has left. A process id of 0 waits for
// nothing. The wait gives up after a minute.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace oa::update_helper {

/// How the wait ended.
enum class WaitOutcome : uint8_t {
    finished,      ///< the process is gone, or there was nothing to wait for
    still_running, ///< it was still there when the minute ran out
};

/// The sentence the window shows when the game is still running.
inline constexpr std::string_view still_running_text = "Open Annihilation is still running.";

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

} // namespace oa::update_helper
