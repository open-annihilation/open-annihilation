// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-running-while-inactive: with the request held, the frame hook keeps
// being called while the window is inactive; once the request is released,
// the main loop waits for an event and the hook is not called again.
#include "oa/app/runtime.hpp"

#include "oa/app/extension.hpp"
#include "oa/base/threads.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace oa::app {
namespace {

// Frames the hook must be called for, while the window is inactive and the
// request is held, before the check releases the request.
constexpr uint32_t kFramesWhileHeld = 3;
// How long the loop is left inactive with the request released, so a loop
// that did not wait calls the frame hook again.
constexpr uint32_t kQuietMs = 1000;
// How long the check waits for those frames before it wakes the loop.
constexpr uint32_t kGiveUpMs = 30000;
constexpr uint32_t kStepMs = 50;

enum class Fault : uint8_t { none, no_frames, no_wake };

// The check's counts and the thread that wakes the loop. One run, one check.
struct InactiveLoopCheck {
    bool started{};
    bool holding{};
    bool count_frames{};
    bool woke{};
    uint32_t wake_event{};
    std::atomic<uint32_t> frames_while_held{};
    std::atomic<uint32_t> frames_after_release{};
    std::atomic<bool> released{};
    // While set, the check counts no frames and does not release. The run
    // that asked for the check clears it once its own work has finished.
    std::atomic<bool> defer_release{};
    std::atomic<uint8_t> fault{static_cast<uint8_t>(Fault::none)};
    // The engine's own thread, as on every system it supports.
    oa::base::threads::Thread thread{};

    ~InactiveLoopCheck() { oa::base::threads::join_thread(thread); }
};

/// Returns the check's state, made on first use.
///
/// @return the one state
InactiveLoopCheck& inactive_loop_check() {
    static InactiveLoopCheck check;
    return check;
}

/// Waits until the request is released, then leaves the loop quiet and wakes it.
///
/// The loop is blocked in its event wait by then, unless it failed to wait:
/// the quiet stretch is long enough for another frame in that case. The wake
/// is the check's own event, which ends the run.
///
/// @param argument the check's state, an InactiveLoopCheck
void watch_inactive_loop(void* argument) {
    InactiveLoopCheck& state = *static_cast<InactiveLoopCheck*>(argument);
    uint32_t waited = 0;
    while (waited < kGiveUpMs && !state.released.load()) {
        SDL_Delay(kStepMs);
        waited += kStepMs;
    }
    if (!state.released.load())
        state.fault.store(static_cast<uint8_t>(Fault::no_frames));
    else
        SDL_Delay(kQuietMs);
    SDL_Event wake{};
    wake.type = state.wake_event;
    if (!SDL_PushEvent(&wake))
        state.fault.store(static_cast<uint8_t>(Fault::no_wake));
}

} // namespace

void apply_inactive_loop_deferral(bool defer) {
    inactive_loop_check().defer_release.store(defer);
}

void Runtime::begin_inactive_loop_check() {
    InactiveLoopCheck& state = inactive_loop_check();
    // The check's window stays inactive whatever a later event reports, so
    // the loop's decision is the one the check is making.
    application_active_ = false;
    state.count_frames = true;
    if (state.started)
        return;
    state.started = true;
    if (sdl_.window == nullptr)
        throw std::runtime_error("inactive loop check: needs the window");
    if (extension_.frame == nullptr)
        throw std::runtime_error("inactive loop check: the frame hook is not filled");
    const uint32_t wake = SDL_RegisterEvents(1);
    if (wake == static_cast<uint32_t>(-1))
        throw std::runtime_error("inactive loop check: could not register a wake event");
    state.wake_event = wake;
    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    focus.window.windowID = SDL_GetWindowID(sdl_.window);
    bool running = true;
    dispatch_event(focus, running);
    application_active_ = false;
    keep_running_while_inactive(*this, true);
    state.holding = true;
    if (!oa::base::threads::start_thread(state.thread, watch_inactive_loop, &state))
        throw std::runtime_error("inactive loop check: could not start the thread that wakes it");
}

void Runtime::note_inactive_loop_frame() {
    InactiveLoopCheck& state = inactive_loop_check();
    // Held off until the run that asked for the check says its own work is
    // done. Counting here would release after three frames, before that work.
    if (state.defer_release.load())
        return;
    if (!state.count_frames || state.woke)
        return;
    if (state.holding) {
        const uint32_t frames = state.frames_while_held.fetch_add(1) + 1;
        if (frames >= kFramesWhileHeld) {
            keep_running_while_inactive(*this, false);
            state.holding = false;
            state.released.store(true);
        }
        return;
    }
    if (state.released.load())
        state.frames_after_release.fetch_add(1);
}

bool Runtime::take_inactive_loop_wake(const SDL_Event& event) {
    if (!options_.check_running_while_inactive)
        return false;
    InactiveLoopCheck& state = inactive_loop_check();
    if (state.wake_event == 0 || event.type != state.wake_event)
        return false;
    state.woke = true;
    state.count_frames = false;
    return true;
}

void Runtime::finish_inactive_loop_check() {
    InactiveLoopCheck& state = inactive_loop_check();
    oa::base::threads::join_thread(state.thread);
    const auto fail = [](const char* what) {
        throw std::runtime_error(std::string("inactive loop check: ") + what);
    };
    if (state.fault.load() == static_cast<uint8_t>(Fault::no_wake))
        fail("could not wake the loop");
    if (state.fault.load() == static_cast<uint8_t>(Fault::no_frames) ||
        state.frames_while_held.load() < kFramesWhileHeld)
        fail("the frame hook was not called while the window was inactive");
    if (state.frames_after_release.load() != 0)
        fail(
            "the loop kept calling the frame hook while the window was inactive, after the "
            "request was released"
        );
    if (!state.woke)
        fail("the loop did not return from its wait");
    std::printf(
        "inactive loop check: %u frames while inactive with the request held, then the loop "
        "waited\n",
        state.frames_while_held.load()
    );
}

} // namespace oa::app
