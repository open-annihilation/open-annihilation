// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The app lifecycle: the system sending the game to the background, ending
// it and running short of memory, acted on as the event arrives, and the
// events kept from the screens (docs/touch-controls.md).
//
// Going to the background, a match played on this machine alone pauses
// while When the window loses focus says Pause single player, and goes on a
// moment after the game is back in the foreground (start_away_pause); with
// Keep playing it waits behind its in-game menu, as F2 leaves it, and the
// player resumes from the menu as ever. The settings are written either way.
// A shared match keeps running until the system suspends the game.
// Short of memory, the cached model images go; they are drawn again from
// their pieces when next needed.
#include "oa/app/runtime.hpp"
#include "match_clock.hpp"
#include <SDL3/SDL.h>
#include <exception>
#include <iostream>

namespace oa::app {
namespace {

/// Returns whether an event is one of the app lifecycle events.
///
/// @param type the event's type
/// @return true for termination, low memory and the four background and foreground events
bool lifecycle_event(uint32_t type) noexcept {
    switch (type) {
    case SDL_EVENT_TERMINATING:
    case SDL_EVENT_LOW_MEMORY:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        return true;
    default:
        return false;
    }
}

} // namespace

/// The lifecycle's helpers that reach the runtime's private members: static functions that take
/// Runtime&.
struct LifecycleAccess {
    /// Hands an app lifecycle event to the runtime as SDL queues it. The system may wait for
    /// the background events' return before it suspends the game, so the runtime acts at once.
    /// Only the main thread may touch the runtime: an event queued from another thread is not
    /// acted on (no platform the game is built for sends them so). Nothing thrown leaves the
    /// watch, which SDL calls.
    ///
    /// @param userdata the runtime
    /// @param event the event being queued
    /// @return true, so the event is queued as well
    static bool SDLCALL watch(void* userdata, SDL_Event* event) {
        if (userdata == nullptr || event == nullptr || !lifecycle_event(event->type) ||
            !SDL_IsMainThread())
            return true;
        attempt("the app lifecycle", [&] {
            static_cast<Runtime*>(userdata)->handle_lifecycle_event(*event);
        });
        return true;
    }

    /// Runs one step of the lifecycle's handling and reports on standard error what it throws,
    /// so that the next step still runs and nothing reaches SDL.
    ///
    /// @param what what the step looks after, for the report
    /// @param step the step
    template <typename Step>
    static void attempt(const char* what, Step&& step) noexcept {
        try {
            step();
        } catch (const std::exception& error) {
            std::cerr << "open-annihilation: " << what << ": " << error.what() << '\n';
        } catch (...) {
            std::cerr << "open-annihilation: " << what << ": an unknown error\n";
        }
    }

    /// Tells whether going to the background holds the match behind its in-game menu, as it
    /// does while When the window loses focus is Keep playing: the match screen shows a
    /// running, unfinished match played on this machine alone (keeps_running_inactive() is
    /// false) whose in-game menu is not already open (match_paused_), or over which only the
    /// team menu or one of its panels is open, which hold no match. The Pause key's pause is
    /// not looked at; the menu holds the match whether it is set or not.
    ///
    /// @param runtime the runtime
    /// @return true when show_match_pause_menu() is to hold the match
    [[nodiscard]] static bool holds_match_for_background(const Runtime& runtime) {
        return runtime.screen_ == Screen::match && runtime.match_ && !runtime.match_finished_ &&
               (!runtime.match_paused_ || runtime.team_panel_open()) &&
               !runtime.keeps_running_inactive();
    }
};

void Runtime::install_lifecycle_watch() {
    if (lifecycle_watch_installed_)
        return;
    if (!SDL_AddEventWatch(LifecycleAccess::watch, this))
        return;
    lifecycle_watch_installed_ = true;
}

void Runtime::remove_lifecycle_watch() {
    if (!lifecycle_watch_installed_)
        return;
    SDL_RemoveEventWatch(LifecycleAccess::watch, this);
    lifecycle_watch_installed_ = false;
}

void Runtime::handle_lifecycle_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        // The system may suspend the game from here on, and also sends
        // this for a moment away (a pulled-down panel, a call): a match
        // played alone pauses until a moment after the game is back, or
        // with Keep playing waits behind its in-game menu, which the player
        // resumes from; the settings are written in case the game does not
        // come back.
        away_pause_.background = true;
        if (away_action() == AwayAction::pause)
            LifecycleAccess::attempt("the pause", [this] { start_away_pause(); });
        else if (LifecycleAccess::holds_match_for_background(*this))
            LifecycleAccess::attempt("the in-game menu", [this] { show_match_pause_menu(); });
        LifecycleAccess::attempt("the preferences", [this] { flush_preferences(); });
        return;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        // Back in the foreground, a paused match goes on a moment later
        // (settle_away_pause); the in-game menu Keep playing opened is still
        // open over the held match, and its Resume resumes it as ever.
        away_pause_.background = false;
        return;
    case SDL_EVENT_DID_ENTER_BACKGROUND:
    case SDL_EVENT_TERMINATING:
        LifecycleAccess::attempt("the preferences", [this] { flush_preferences(); });
        return;
    case SDL_EVENT_LOW_MEMORY:
        // The images are a cache: each is drawn again from its model's
        // pieces at its next draw.
        LifecycleAccess::attempt("the model images", [this] { release_model_images(); });
        return;
    default:
        return;
    }
}

bool Runtime::take_lifecycle_event(const SDL_Event& event) {
    // The watch acted on each as SDL queued it; none reaches a screen.
    return lifecycle_event(event.type);
}

} // namespace oa::app
