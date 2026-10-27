// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/watch.hpp"

#include "oa/app/update/plan.hpp"

#include <SDL3/SDL.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>
#endif

namespace oa::update_helper {
namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;

constexpr uint32_t poll_ms = 250;
/// How long a start that cannot be observed may be waited on. A background
/// process on POSIX is not the program, and SDL does not keep its pid.
constexpr uint64_t unobservable_ms = 5000;

std::optional<update::State> read_state_file(const fs::path& state_path) {
    std::ifstream input(state_path, std::ios::binary);
    if (!input)
        return std::nullopt;
    const std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()
    );
    if (!input && !input.eof())
        return std::nullopt;
    std::string error;
    return update::read_state(bytes, &error);
}

bool state_is_confirmed(const fs::path& state_path) {
    const std::optional<update::State> state = read_state_file(state_path);
    return state.has_value() && state->confirmed;
}

bool process_ended(SDL_Process* process) {
    if (process == nullptr)
        return false;
    int status = 0;
    return SDL_WaitProcess(process, false, &status);
}

bool pid_ended(int pid) {
    if (pid <= 0)
        return false;
#if defined(_WIN32)
    (void)pid;
    return false;
#else
    if (::kill(pid, 0) == 0)
        return false;
    return errno == ESRCH;
#endif
}

/// A background process is started by a short-lived child on POSIX. SDL keeps
/// that child's pid, which has already exited, and never the program's, so
/// waiting on the handle does not see the program end.
bool background_process(SDL_Process* process) {
    if (process == nullptr)
        return false;
    const SDL_PropertiesID properties = SDL_GetProcessProperties(process);
    if (properties == 0)
        return false;
    return SDL_GetBooleanProperty(properties, SDL_PROP_PROCESS_BACKGROUND_BOOLEAN, false);
}

/// Tells whether the watch can see that the program is still running.
bool still_running(SDL_Process* process, int pid) {
    if (pid > 0)
        return !pid_ended(pid);
    if (process == nullptr || background_process(process))
        return false;
    return !process_ended(process);
}

void drop_process(SDL_Process* process) {
    if (process != nullptr)
        SDL_DestroyProcess(process);
}

} // namespace

WatchEnd ended_watch(const std::optional<update::State>& state) noexcept {
    switch (update::start_end(state)) {
    case update::StartEnd::reached_menu:
        return WatchEnd::confirmed;
    case update::StartEnd::quit:
        return WatchEnd::quit;
    case update::StartEnd::failed:
        return WatchEnd::ended;
    }
    return WatchEnd::ended;
}

WatchTwo watch_two_starts(
    SDL_Process* process,
    int pid,
    const fs::path& state_path,
    bool (*again)(void* context, SDL_Process** process, int* pid, std::string* error),
    void* context,
    std::string* error
) {
    const WatchEnd first = watch_until_settled(process, pid, state_path);
    if (first == WatchEnd::confirmed)
        return WatchTwo::confirmed;
    if (first == WatchEnd::quit)
        return WatchTwo::quit;
    if (first == WatchEnd::unobserved)
        return WatchTwo::unobserved;
    SDL_Process* restarted = nullptr;
    int restarted_pid = 0;
    if (again == nullptr || !again(context, &restarted, &restarted_pid, error))
        return WatchTwo::restart_failed;
    const WatchEnd second = watch_until_settled(restarted, restarted_pid, state_path);
    if (second == WatchEnd::confirmed)
        return WatchTwo::confirmed;
    if (second == WatchEnd::quit)
        return WatchTwo::quit;
    if (second == WatchEnd::unobserved)
        return WatchTwo::unobserved;
    return WatchTwo::failed_twice;
}

WatchEnd watch_until_settled(SDL_Process* process, int pid, const fs::path& state_path) {
    const bool have_process = process != nullptr;
    const bool have_pid = pid > 0;
    if (!have_process && !have_pid) {
        drop_process(process);
        return WatchEnd::unobserved;
    }
    const uint64_t began = SDL_GetTicks();
    for (;;) {
        if (state_is_confirmed(state_path)) {
            drop_process(process);
            return WatchEnd::confirmed;
        }
        const bool ended = process_ended(process) || (process == nullptr && pid_ended(pid));
        if (ended) {
            drop_process(process);
            return ended_watch(read_state_file(state_path));
        }
        // A start SDL cannot see is not a game that is still running. Stop
        // after a short wait so a child that has already gone cannot hold
        // the helper. A pid, or a process SDL can wait on, has no such limit.
        if (!still_running(process, pid) && SDL_GetTicks() - began >= unobservable_ms) {
            drop_process(process);
            return WatchEnd::unobserved;
        }
        SDL_Delay(poll_ms);
    }
}

} // namespace oa::update_helper
