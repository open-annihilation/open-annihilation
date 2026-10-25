// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/wait.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace oa::update_helper {
namespace {

constexpr int64_t wait_limit_ms = 60000;
constexpr int64_t wait_slice_ms = 100;

void set_error(std::string* reason, std::string message) {
    if (reason != nullptr)
        *reason = std::move(message);
}

int64_t real_now_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

WaitOutcome wait_with(int64_t pid, const WaitHooks& hooks, std::string* error) {
    const int64_t start = hooks.now_ms(hooks.context);
    while (true) {
        if (!hooks.alive(hooks.context, pid))
            return WaitOutcome::finished;
        const int64_t elapsed = hooks.now_ms(hooks.context) - start;
        if (elapsed >= wait_limit_ms) {
            set_error(error, std::string(still_running_text));
            return WaitOutcome::still_running;
        }
        const int64_t pause = std::min(wait_slice_ms, wait_limit_ms - elapsed);
        hooks.sleep_ms(hooks.context, pause);
    }
}

#if defined(_WIN32)
WaitOutcome wait_real(int64_t pid, std::string* error) {
    // A process id of 0 or less, or one that does not fit, is not running.
    if (pid <= 0 || static_cast<uint64_t>(pid) > MAXDWORD)
        return WaitOutcome::finished;
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr)
        return WaitOutcome::finished;
    const DWORD waited = WaitForSingleObject(handle, static_cast<DWORD>(wait_limit_ms));
    CloseHandle(handle);
    if (waited == WAIT_TIMEOUT) {
        set_error(error, std::string(still_running_text));
        return WaitOutcome::still_running;
    }
    return WaitOutcome::finished;
}
#else
bool real_alive(int64_t pid) {
    // kill with a process id of 0 or less signals a group. Never do that.
    if (pid <= 0)
        return false;
    if (pid > static_cast<int64_t>(std::numeric_limits<pid_t>::max()))
        return false;
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
        return true;
    return errno == EPERM;
}

WaitOutcome wait_real(int64_t pid, std::string* error) {
    WaitHooks hooks;
    const int64_t origin = real_now_ms();
    hooks.context = nullptr;

    // The real clock is closed over by the functions below through origin.
    struct Clock {
        int64_t origin;
    };

    Clock clock{origin};
    hooks.context = &clock;
    hooks.now_ms = [](void* context) {
        (void)context;
        return real_now_ms();
    };
    hooks.sleep_ms = [](void*, int64_t milliseconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    };
    hooks.alive = [](void*, int64_t process) { return real_alive(process); };
    (void)clock;
    return wait_with(pid, hooks, error);
}
#endif

} // namespace

WaitOutcome wait_for_process(int64_t pid, const WaitHooks* hooks, std::string* error) {
    if (pid == 0)
        return WaitOutcome::finished;
    if (hooks != nullptr && hooks->now_ms != nullptr && hooks->sleep_ms != nullptr &&
        hooks->alive != nullptr)
        return wait_with(pid, *hooks, error);
    return wait_real(pid, error);
}

} // namespace oa::update_helper
