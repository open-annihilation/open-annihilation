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

bool state_is_confirmed(const fs::path& state_path) {
    std::ifstream input(state_path, std::ios::binary);
    if (!input)
        return false;
    const std::vector<uint8_t> bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()
    );
    if (!input && !input.eof())
        return false;
    std::string error;
    const std::optional<update::State> state = update::read_state(bytes, &error);
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

} // namespace

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
    SDL_Process* restarted = nullptr;
    int restarted_pid = 0;
    if (again == nullptr || !again(context, &restarted, &restarted_pid, error))
        return WatchTwo::restart_failed;
    const WatchEnd second = watch_until_settled(restarted, restarted_pid, state_path);
    if (second == WatchEnd::confirmed)
        return WatchTwo::confirmed;
    return WatchTwo::failed_twice;
}

WatchEnd watch_until_settled(SDL_Process* process, int pid, const fs::path& state_path) {
    const bool have_process = process != nullptr;
    const bool have_pid = pid > 0;
    if (!have_process && !have_pid) {
        if (process != nullptr)
            SDL_DestroyProcess(process);
        return WatchEnd::ended;
    }
    for (;;) {
        if (state_is_confirmed(state_path)) {
            if (process != nullptr)
                SDL_DestroyProcess(process);
            return WatchEnd::confirmed;
        }
        const bool ended = process_ended(process) || (process == nullptr && pid_ended(pid));
        if (process != nullptr && ended) {
            SDL_DestroyProcess(process);
            return WatchEnd::ended;
        }
        if (process == nullptr && ended)
            return WatchEnd::ended;
        SDL_Delay(poll_ms);
    }
}

} // namespace oa::update_helper
