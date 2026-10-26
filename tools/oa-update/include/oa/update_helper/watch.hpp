// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Watching the version the helper just started. The watch ends when that
// process does, or when state.json says the start reached the main menu.
// It does not time out while the process is still running.
#pragma once

#include <filesystem>

struct SDL_Process;

namespace oa::update_helper {

/// How a watch ended.
enum class WatchEnd : uint8_t {
    confirmed, ///< state.json says the start reached the main menu
    ended,     ///< the process ended without that
};

/// Waits until `process` or `pid` has ended, or until `state_path` says confirmed.
///
/// Both are polled every 250 ms. A confirmed state ends the watch even while
/// the process is still running, and the process is not killed. Pass a
/// process from SDL, or a positive pid from Launch Services, or both. With
/// neither, the watch ends at once as `ended`.
///
/// @param process the process SDL started; destroyed when the watch ends; may be null
/// @param pid a process id to poll; 0 polls nothing
/// @param state_path the state file
/// @return why the watch ended
[[nodiscard]] WatchEnd watch_until_settled(
    SDL_Process* process, int pid, const std::filesystem::path& state_path
);

/// How two watched starts ended.
enum class WatchTwo : uint8_t {
    confirmed,      ///< one of the starts reached the main menu
    failed_twice,   ///< both ended without that
    restart_failed, ///< the second start could not be launched
};

/// Watches one start, and on an unconfirmed end watches the start `again` makes.
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
