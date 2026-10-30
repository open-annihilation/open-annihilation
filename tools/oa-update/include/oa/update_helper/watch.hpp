// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Watching the version the helper just started. The watch ends when that
// process does, or when state.json says the start reached the main menu.
// A start the player quit before the main menu is told apart from one that
// failed: the game marks its own quit in state.json as it leaves.
// It does not time out while the process is still running. A start that
// cannot be observed — no pid, and a background process whose pid SDL did
// not keep — ends after a short wait, so a child that is already gone
// cannot hold the helper. Such a start is not counted as failed: time alone
// never decides that a start failed.
#pragma once

#include "oa/app/update/plan.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

struct SDL_Process;

namespace oa::update_helper {

/// How a watch ended.
enum class WatchEnd : uint8_t {
    confirmed,  ///< state.json says the start reached the main menu
    quit,       ///< the process ended, and state.json says the player quit before the main menu
    ended,      ///< the process ended without either: a crash or an error
    unobserved, ///< the start could not be seen, so how it ended is not known
};

/// How a watch that saw the process end reads the state file then.
///
/// @param state the state file after the process ended, or nothing when it could not be read
/// @return confirmed, quit or ended
[[nodiscard]] WatchEnd ended_watch(const std::optional<oa::app::update::State>& state) noexcept;

/// Waits until `process` or `pid` has ended, or until `state_path` says confirmed.
///
/// Both are polled every 250 ms. A confirmed state ends the watch even while
/// the process is still running, and the process is not killed. When the
/// process ends, the state file says whether it reached the main menu, was
/// quit by the player before it, or neither. With neither a process nor a
/// pid, or a background process the watch cannot see, it is unobserved. Pass a
/// process from SDL, or a positive pid from Launch Services, or both. With
/// neither, the watch ends at once as `unobserved`. A background process with no
/// pid is not a process the watch can see; that watch ends after a few
/// seconds as `unobserved` when the state stays unconfirmed.
///
/// @param process the process SDL started; destroyed when the watch ends; may be null
/// @param pid a process id to poll; 0 polls nothing
/// @param state_path the state file
/// @return why the watch ended
[[nodiscard]] WatchEnd
watch_until_settled(SDL_Process* process, int pid, const std::filesystem::path& state_path);

/// How two watched starts ended.
enum class WatchTwo : uint8_t {
    confirmed,      ///< one of the starts reached the main menu
    quit,           ///< the player quit a start before the main menu; nothing failed
    unobserved,     ///< a start could not be seen; it is not counted as failed
    failed_twice,   ///< both ended without either
    restart_failed, ///< the second start could not be launched
};

/// Watches one start, and on a failed end watches the start `again` makes.
///
/// A start the player quit ends the watch at once as `quit`, and one that
/// could not be seen as `unobserved`; nothing is started again.
///
/// `again` receives the context and fills a new process or pid. A null
/// function, or one that returns false, is `restart_failed`. The processes
/// are destroyed here. The restored start is not watched: the caller does that
/// only when this returns `failed_twice` or `restart_failed`.
///
/// @param process the first process; may be null when `pid` is set
/// @param pid the first process id; 0 when `process` is set
/// @param state_path the state file
/// @param again starts the same version again; may be null
/// @param context passed to `again`
/// @param[out] error why the second start failed; may be null
/// @return how the watch ended
[[nodiscard]] WatchTwo watch_two_starts(
    SDL_Process* process,
    int pid,
    const std::filesystem::path& state_path,
    bool (*again)(void* context, SDL_Process** process, int* pid, std::string* error),
    void* context,
    std::string* error
);

} // namespace oa::update_helper
