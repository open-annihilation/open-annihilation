// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The inactive-loop check's login. The window is held inactive for the whole
// run. Once the login console is up, the guest name is filled and LOGIN is
// clicked the way a program driving the game clicks, and the login must start
// and finish before the check is allowed to release.
#include "boneyards_options.hpp"
#include "console_state.hpp"

#include "oa/app/extension.hpp"
#include "oa/app/frame_coordinates.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/screens/login.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace oa::app {

// The Boneyards overlay's status widget (console_screens.cpp).
oa::boneyards::console::StatusWidget* boneyards_console_status_widget();

namespace {

namespace by = oa::boneyards;
namespace by_console = oa::boneyards::console;
namespace screens = oa::boneyards::screens;

// How long the login may take before the check says which step did not finish.
// The check's own watcher gives up later, with a less useful line.
constexpr uint64_t kLoginLimitMs = 25000;
// After the click is acted on, the loop still treats input as recent for
// 500 ms. The check's quiet stretch has to begin after that.
constexpr uint64_t kQuietAfterActedMs = 800;
// Frames after the click with the status still "Ready...".
constexpr uint32_t kFramesAfterClick = 180;
// The replay's guest. A saved name from an earlier run must not be what is sent.
constexpr const char* kGuestName = "oa_check";

enum class Phase : uint8_t { open, arm, watch, settle, done };

struct InactiveLogin {
    Phase phase = Phase::open;
    bool started = false;
    // ENTER BONEYARDS was asked for, and the Boneyards window then opened.
    bool entered = false;
    bool opened = false;
    // The name was written on an earlier frame. The console's pulse, which
    // copies the field into the dialog and enables LOGIN, runs before this
    // hook, so the click waits for the frame after the write.
    bool armed = false;
    bool acted = false;
    bool finished = false;
    bool released = false;
    uint64_t started_ms = 0;
    uint64_t acted_ms = 0;
    uint32_t frames_after_click = 0;
};

/// Returns the one login the check drives.
///
/// @return the login
InactiveLogin& inactive_login() {
    static InactiveLogin held;
    return held;
}

/// Fails the inactive-loop check.
///
/// @param what Failure text; thrown as std::runtime_error "inactive loop check: <what>".
[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string("inactive loop check: ") + what);
}

/// Fails when the login has taken longer than the check allows.
///
/// @param state the login
/// @param now_ms the clock now
void give_up(const InactiveLogin& state, uint64_t now_ms) {
    if (!state.started || now_ms - state.started_ms <= kLoginLimitMs)
        return;
    if (state.phase == Phase::open && !state.opened)
        fail("the Boneyards did not open");
    if (state.phase == Phase::open)
        fail("the login console did not open");
    if (state.phase == Phase::arm)
        fail("LOGIN stayed disabled while the window was inactive");
    fail("the login did not finish while the window was inactive");
}

/// Pushes a left click at a point of the frame, as a program driving the game does.
///
/// Motion, then the button down, then up. The three stay queued until the next
/// frame reads them.
///
/// @param frame_x column in the frame
/// @param frame_y row in the frame
void push_login_click(float frame_x, float frame_y) {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_Window* window = windows != nullptr && count > 0 ? windows[0] : nullptr;
    SDL_free(windows);
    if (window == nullptr)
        fail("needs the window to click LOGIN");
    SDL_Renderer* renderer = SDL_GetRenderer(window);
    float window_x = 0;
    float window_y = 0;
    if (renderer == nullptr || !frame_to_window(renderer, frame_x, frame_y, &window_x, &window_y))
        fail("LOGIN could not be placed in the window");
    const SDL_WindowID id = SDL_GetWindowID(window);
    const uint64_t now = SDL_GetTicksNS();
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.timestamp = now;
    event.motion.windowID = id;
    event.motion.which = 0;
    event.motion.x = window_x;
    event.motion.y = window_y;
    if (!SDL_PushEvent(&event))
        fail("the click did not reach the queue");
    for (const bool down : {true, false}) {
        event = SDL_Event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.timestamp = now;
        event.button.windowID = id;
        event.button.which = 0;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = down;
        event.button.clicks = 1;
        event.button.x = window_x;
        event.button.y = window_y;
        if (!SDL_PushEvent(&event))
            fail("the click did not reach the queue");
    }
}

/// Writes the guest name and selects Guest Login.
///
/// The dialog copies the field on its next pulse. A saved name selects a
/// member login, which leaves LOGIN off until a password is typed, so Guest
/// Login is selected on every run.
///
/// @param widget the overlay
void arm_guest(by_console::StatusWidget* widget) {
    by_console::MainFrame* frame = widget->system.main_frame;
    if (frame == nullptr || frame->active_screen == nullptr)
        fail("the login console is not open");
    by_console::ScopedServices scope(widget);
    by::Window* name = by::window_find_named(frame->active_screen, "Username", nullptr);
    if (name == nullptr || name->ops == nullptr || name->ops->set_caption == nullptr)
        fail("the Username field is not on the login");
    name->ops->set_caption(name, kGuestName, 0);
    screens::login_on_control(&widget->navigation->login, screens::LoginControl::guest_radio);
}

/// Tells whether LOGIN can be pressed.
///
/// @param widget the overlay
/// @return true when LOGIN is enabled
bool login_ready(by_console::StatusWidget* widget) {
    by_console::MainFrame* frame = widget->system.main_frame;
    if (frame == nullptr || frame->active_screen == nullptr || widget->navigation == nullptr)
        return false;
    const by::Window* login_button = by::window_find_named(frame->active_screen, "LOGIN", nullptr);
    if (login_button == nullptr)
        return false;
    const screens::LoginDialog& dialog = widget->navigation->login;
    return login_button->states.current != by::button_state::disabled && dialog.login_value != 0;
}

/// Clicks the centre of LOGIN.
///
/// @param widget the overlay
void click_login(by_console::StatusWidget* widget) {
    by_console::MainFrame* frame = widget->system.main_frame;
    const by::Window* login_button =
        frame != nullptr && frame->active_screen != nullptr
            ? by::window_find_named(frame->active_screen, "LOGIN", nullptr)
            : nullptr;
    if (login_button == nullptr)
        fail("LOGIN is not on the login");
    push_login_click(
        static_cast<float>(login_button->x + login_button->width / 2),
        static_cast<float>(login_button->y + login_button->height / 2)
    );
}

/// Watches the click until the dossier arrives, then holds the quiet stretch.
///
/// @param[in,out] state the login
/// @param widget the overlay
/// @param now_ms the clock now
void watch_login(InactiveLogin& state, by_console::StatusWidget* widget, uint64_t now_ms) {
    ++state.frames_after_click;
    const screens::LoginDialog& dialog = widget->navigation->login;
    by_console::MainFrameProbe probe{};
    const bool probed = by_console::boneyards_status_widget_probe(widget, &probe);
    const bool logged =
        probed && probe.connection_level >= static_cast<int32_t>(by::SessionLevel::logged_in);
    if (!state.acted && (dialog.state == screens::LoginState::connecting || logged)) {
        state.acted = true;
        state.acted_ms = now_ms;
        std::printf("inactive loop check: LOGIN acted on while the window was inactive\n");
        std::fflush(stdout);
    }
    if (!state.acted && dialog.state == screens::LoginState::idle &&
        std::strcmp(dialog.status, "Server Not responding") == 0)
        fail("the login failed while the window was inactive");
    if (!state.acted && state.frames_after_click >= kFramesAfterClick)
        fail("LOGIN was clicked while the window was inactive and the status stayed Ready...");
    const bool dossier =
        probed &&
        probe.connection_level >= static_cast<int32_t>(by::SessionLevel::dossier_loaded) &&
        std::strcmp(probe.user_name, kGuestName) == 0;
    if (!state.acted || !dossier)
        return;
    if (!state.finished) {
        state.finished = true;
        std::printf("inactive loop check: logged in while the window was inactive\n");
        std::fflush(stdout);
    }
    state.phase = Phase::settle;
}

} // namespace

void run_boneyards_inactive_check(Runtime& /*runtime*/) {
    InactiveLogin& state = inactive_login();
    if (state.released)
        return;
    const uint64_t now_ms = SDL_GetTicks();
    if (!state.started) {
        state.started = true;
        state.started_ms = now_ms;
        if (boneyards_options().boneyards_replay.empty())
            fail("needs a replay of the login");
    }
    give_up(state, now_ms);

    by_console::StatusWidget* widget = boneyards_console_status_widget();
    if (state.phase == Phase::open) {
        if (widget == nullptr)
            return;
        // ENTER BONEYARDS once the main menu's strip is up. Entering puts the
        // strip away, so from then on the Boneyards window is what shows.
        if (!state.entered) {
            if (!by_console::boneyards_status_widget_visible(widget))
                return;
            by_console::boneyards_status_widget_enter(widget);
            state.entered = true;
            return;
        }
        if (!by_console::boneyards_status_widget_in_boneyards(widget))
            return;
        if (!state.opened) {
            state.opened = true;
            std::printf(
                "inactive loop check: the Boneyards opened while the window was inactive\n"
            );
            std::fflush(stdout);
        }
        by_console::MainFrameProbe probe{};
        if (!by_console::boneyards_status_widget_probe(widget, &probe))
            return;
        if (probe.console_screen != by_console::console_screen::login || !probe.screen_shown ||
            probe.layer_sliding)
            return;
        state.phase = Phase::arm;
    }
    if (widget == nullptr || widget->navigation == nullptr)
        fail("the login console went away");

    if (state.phase == Phase::arm) {
        arm_guest(widget);
        if (!state.armed) {
            state.armed = true;
            return;
        }
        if (!login_ready(widget))
            return;
        click_login(widget);
        state.phase = Phase::watch;
        return;
    }
    if (state.phase == Phase::watch) {
        watch_login(state, widget, now_ms);
        return;
    }
    if (state.phase == Phase::settle && now_ms - state.acted_ms >= kQuietAfterActedMs) {
        defer_inactive_loop_release(false);
        state.released = true;
        state.phase = Phase::done;
    }
}

} // namespace oa::app
