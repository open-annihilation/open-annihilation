// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Host services behind the application loop: held-key queries, window
// activation, the pause of a game played alone while the window is away,
// closing the application and the developer memory report.
#include "oa/app/runtime.hpp"
#include "device_state.hpp"
#include "engine_settings_state.hpp"
#include "match_clock.hpp"
#include "oa/platform/memory_status.hpp"
#include "oa/ui/console/game_fields.hpp"
#include <SDL3/SDL.h>
#include <cstdint>
#include <cstdio>
#include <tuple>

namespace oa::app {
namespace {

namespace gui_input = oa::ui::gui_input;

// Text of one memory report (the report fits in well under 400 bytes).
constexpr std::size_t kMemoryReportCapacity = 512;

// Key-state word (bit 15 while held) from the keys held (device_state.hpp);
// either key of a left/right modifier pair holds it.
uint16_t sdl_async_key_state(void*, gui_input::VirtualKey key) {
    const auto held = [](SDL_Scancode code) { return device_state::key_held(code); };
    bool down = false;
    switch (key) {
    case gui_input::VirtualKey::left:
        down = held(SDL_SCANCODE_LEFT);
        break;
    case gui_input::VirtualKey::up:
        down = held(SDL_SCANCODE_UP);
        break;
    case gui_input::VirtualKey::right:
        down = held(SDL_SCANCODE_RIGHT);
        break;
    case gui_input::VirtualKey::down:
        down = held(SDL_SCANCODE_DOWN);
        break;
    case gui_input::VirtualKey::space:
        down = held(SDL_SCANCODE_SPACE);
        break;
    case gui_input::VirtualKey::shift:
        down = held(SDL_SCANCODE_LSHIFT) || held(SDL_SCANCODE_RSHIFT);
        break;
    case gui_input::VirtualKey::control:
        down = held(SDL_SCANCODE_LCTRL) || held(SDL_SCANCODE_RCTRL);
        break;
    case gui_input::VirtualKey::alt:
        down = held(SDL_SCANCODE_LALT) || held(SDL_SCANCODE_RALT);
        break;
    }
    return down ? gui_input::kAsyncKeyHeld : 0;
}

} // namespace

bool Runtime::control_key_down(gui_input::ControlKey key) const {
    if ((key == gui_input::ControlKey::shift && shift_held_by_check_) ||
        (key == gui_input::ControlKey::space && space_held_by_check_))
        return true;
    return gui_input::control_key_down(static_cast<int32_t>(key), sdl_async_key_state, nullptr);
}

void Runtime::note_window_activation(const SDL_Event& event) {
    if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED)
        application_active_ = true;
    else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
        application_active_ = false;
    else if (
        !application_active_ && sdl_.window != nullptr &&
        (SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_INPUT_FOCUS) != 0
    )
        // The loading pump and the input drain discard events, a focus gain
        // among them; the window's own state settles it.
        application_active_ = true;
    // An inactive application draws no cursor of its own (pointer_shows_cursor),
    // so the system's pointer shows over its window until it is active again.
    // Every event applies the rule, not only a change of focus, so that a
    // pointer another part of the system showed is hidden again.
    // Native cursor rectangles can change without changing SDL's cached
    // visibility. Reassert the hidden cursor when the pointer or focus moves.
    const bool redraw =
        event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_WINDOW_MOUSE_ENTER ||
        event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
    apply_system_pointer(redraw);
}

bool Runtime::pointer_shows_cursor() const {
    return !system_pointer_wanted();
}

bool Runtime::system_pointer_wanted() const {
    return !cursors_loaded_ || !application_active_ || system_pointer_screen_open_ ||
           (sdl_.window != nullptr &&
            (SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_MOUSE_FOCUS) == 0);
}

void Runtime::apply_system_pointer(bool redraw) {
    if (sdl_.window == nullptr)
        return;
    const bool wanted = system_pointer_wanted();
    if (SDL_CursorVisible() != wanted) {
        if (wanted)
            SDL_ShowCursor();
        else
            SDL_HideCursor();
    } else if (redraw && !wanted) {
        // A null cursor has SDL set the current one again, hidden as it is.
        std::ignore = SDL_SetCursor(nullptr);
    }
}

bool Runtime::keeps_running_inactive() const {
    constexpr uint8_t kLiveGame = 0x01; // the Game block's live-game bit
    return (current_extension_state() & extension_state::multiplayer) != 0 ||
           (match_ && (match_->state().game.session_flags & kLiveGame) != 0);
}

void keep_running_while_inactive(Runtime& runtime, bool hold) {
    runtime.keep_running_while_inactive_ = hold;
}

AwayAction Runtime::away_action() const {
    using oa::ui::engine_settings::FocusLost;
    const uint32_t extension = current_extension_state();
    const bool keep_playing = engine_settings_ != nullptr &&
                              engine_settings_->current.focus_lost == FocusLost::keep_playing;
    AwayFacts facts;
    facts.pause_chosen = !keep_playing;
    facts.match_shown = match_running();
    facts.finished = match_finished_;
    facts.network = keeps_running_inactive() || (extension & extension_state::shared_match) != 0;
    facts.replay = (extension & extension_state::replay) != 0;
    facts.scripted = options_.frame_limit.has_value() || options_.remote_controlled ||
                     director_ != nullptr || !options_.bench_file.empty() ||
                     options_.showcase != Showcase::none;
    return oa::app::away_action(facts);
}

bool Runtime::window_away() const {
    return !application_active_ || away_pause_.minimised || away_pause_.background;
}

void Runtime::note_window_away(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_WINDOW_MINIMIZED:
        away_pause_.minimised = true;
        start_away_pause();
        return;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
        away_pause_.minimised = false;
        return;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        start_away_pause();
        return;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        // A window with the focus is back, whatever else said it was away.
        away_pause_.minimised = false;
        away_pause_.background = false;
        return;
    default:
        return;
    }
}

void Runtime::start_away_pause() {
    if (away_action() != AwayAction::pause)
        return;
    // Away again before the pause ended: it waits for the window again.
    if (away_pause_holds()) {
        away_pause_.resume_ns.reset();
        return;
    }
    // A pause the player chose stays the player's: the pause bit set by the
    // Pause key or anything else, or an in-game menu or dialog holding the
    // match. The team menu and its panels hold none.
    auto& game = match_->state().game;
    if ((game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0 ||
        (match_paused_ && !team_panel_open()))
        return;
    game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags | oa::ui::console::kSimRunPaused);
    away_pause_.held = true;
    away_pause_.resume_ns.reset();
}

void Runtime::settle_away_pause() {
    if (!away_pause_.held)
        return;
    if (!away_pause_holds()) {
        away_pause_.held = false;
        away_pause_.resume_ns.reset();
        return;
    }
    if (window_away()) {
        away_pause_.resume_ns.reset();
        return;
    }
    if (!away_pause_.resume_ns) {
        away_pause_.resume_ns = frame_time_ns_ + away_resume_ms * 1'000'000ULL;
        return;
    }
    if (frame_time_ns_ >= *away_pause_.resume_ns)
        release_away_pause();
}

bool Runtime::away_pause_holds() const {
    return away_pause_.held && match_ &&
           (match_->state().game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0;
}

void Runtime::release_away_pause() {
    if (away_pause_holds()) {
        auto& game = match_->state().game;
        game.sim_run_flags =
            static_cast<uint16_t>(game.sim_run_flags & ~oa::ui::console::kSimRunPaused);
    }
    away_pause_.held = false;
    away_pause_.resume_ns.reset();
}

bool Runtime::keeps_playing_away() const {
    return !away_pause_.background && away_action() == AwayAction::keep_playing;
}

void Runtime::quit_application(const char* message) {
    exit_requested_ = true;
    if (message == nullptr || *message == '\0')
        return;
    if (sdl_.window != nullptr) {
        // The box needs the pointer, which full screen keeps on the window.
        release_pointer(sdl_.window);
        // A message the box cannot show goes to standard error, as it does
        // without a window.
        if (!SDL_ShowSimpleMessageBox(0, SDL_GetWindowTitle(sdl_.window), message, sdl_.window))
            std::fprintf(stderr, "%s\n", message);
    } else
        std::fprintf(stderr, "%s\n", message);
}

void Runtime::print_memory_status() {
    char text[kMemoryReportCapacity];
    if (oa::platform::format_memory_status(
            memory_report_, oa::platform::sample_process_memory, nullptr, text, sizeof text
        ) == 0)
        return;
    std::fputs(text, stdout);
    std::fflush(stdout);
}

} // namespace oa::app
