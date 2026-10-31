// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// When the window loses focus on a skirmish against the computer, driven by
// the window's and the app lifecycle's events: Pause single player pauses
// while the window is away and goes on a moment after it is back, a pause
// the player chose stays, Keep playing never pauses, and a game a
// multiplayer session is open for never pauses.
#include "oa/app/runtime.hpp"

#include "match_clock.hpp"
#include "oa/app/frame_coordinates.hpp"
#include "oa/base/game_loop.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/frontend_state/dispatcher.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace oa::app {
namespace {

namespace settings = oa::ui::engine_settings;

// Each frame the check runs moves the frame clock on by this many
// milliseconds.
constexpr uint32_t kFrameMs = 50;
// Nanoseconds in a millisecond.
constexpr uint64_t kNsPerMs = 1'000'000;
// A stretch of frames long enough for a running skirmish to tick.
constexpr uint32_t kRunMs = 1000;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("away pause: " + what);
}

void require(bool ok, const std::string& what) {
    if (!ok)
        fail(what);
}

} // namespace

void Runtime::check_away_pause() {
    namespace console = oa::ui::console;
    const auto step = [](std::string_view name) {
        std::cout << "away pause: " << name << '\n' << std::flush;
    };
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the window");
    require(
        settings::EngineSettings{}.focus_lost == settings::FocusLost::pause_single_player,
        "When the window loses focus is not Pause single player by default"
    );
    const auto choose = [&](settings::FocusLost lost) {
        auto chosen = engine_settings();
        chosen.focus_lost = lost;
        chosen.window_edge = settings::WindowEdge::capture;
        apply_engine_settings(chosen);
    };
    choose(settings::FocusLost::pause_single_player);

    start_benchmark_skirmish();
    require(screen_ == Screen::match && match_ && !match_finished_, "the skirmish did not start");
    require(!match_paused_, "a menu is open over the skirmish");
    auto& game = match_->state().game;
    require((game.sim_run_flags & console::kSimRunPaused) == 0, "the skirmish starts paused");
    require(
        away_action() == AwayAction::pause,
        "a skirmish against the computer is not taken for a game played alone"
    );

    bool running = true;
    const SDL_WindowID window_id = SDL_GetWindowID(sdl_.window);
    const auto window_event = [&](SDL_EventType type) {
        SDL_Event event{};
        event.type = type;
        event.window.windowID = window_id;
        dispatch_event(event, running);
        require(running, "a window event ended the run");
    };
    const auto key = [&](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event event{};
        event.key.windowID = window_id;
        event.key.key = code;
        event.key.scancode = scancode;
        for (const bool down : {true, false}) {
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.down = down;
            dispatch_event(event, running);
            require(running, "a key ended the run");
        }
    };
    // The app lifecycle's events reach the watch as they are pushed, and
    // then the dispatcher, which takes them.
    const auto lifecycle = [&](SDL_EventType type) {
        SDL_Event event{};
        event.type = type;
        require(SDL_PushEvent(&event), SDL_GetError());
        SDL_Event queued{};
        while (SDL_PollEvent(&queued)) {
            dispatch_event(queued, running);
            require(running, "a lifecycle event ended the run");
        }
    };
    const auto paused = [&] { return (game.sim_run_flags & console::kSimRunPaused) != 0; };

    // The frames run on a clock of the check's own, from now, so the moment
    // the pause waits is counted exactly.
    const uint64_t clock_start_ns = frame_pacing::steady_now_ns();
    frame_run_clock_ns_ = clock_start_ns;
    match_timing_.previous_clock = oa::base::game_loop::scaled_clock(
        static_cast<uint32_t>(clock_start_ns / kNsPerMs), match_clock_scale()
    );
    match_timing_.remainder = 0.0F;
    // Runs frames for a time and returns the ticks the skirmish ran.
    const auto run_for = [&](uint32_t ms) {
        const uint32_t before = match_timing_.tick;
        for (uint32_t elapsed = 0; elapsed < ms; elapsed += kFrameMs) {
            frame_run_clock_ns_ = *frame_run_clock_ns_ + kFrameMs * kNsPerMs;
            idle_tick();
            require(!exit_requested_, "a frame ended the run");
        }
        return match_timing_.tick - before;
    };
    // The paused title's place: the battlefield's middle.
    renderer::Surface frame;
    const auto band = match_layout_.top;
    const CanvasRect title{
        (match_layout_.width + match_layout_.left) / 2 - band,
        match_layout_.height / 2 - band / 2,
        2 * band,
        band
    };
    const auto title_pixels = [&] {
        render_match_surface();
        compose_match_frame(frame);
        return copy_rect(frame, title);
    };
    // Checks the skirmish is held for a while, and returns once a moment
    // after the window is back, when it runs again.
    const auto comes_back = [&](std::string_view what) {
        require(paused() && away_pause_holds(), std::string(what) + " did not hold the pause");
        require(
            run_for(away_resume_ms - 2 * kFrameMs) == 0 && paused(),
            std::string(what) + ": the skirmish ran before the moment after the window came back"
        );
        std::ignore = run_for(3 * kFrameMs);
        require(
            !paused() && !away_pause_holds(),
            std::string(what) + ": the pause did not end a moment after the window came back"
        );
        require(
            run_for(kRunMs) > 0, std::string(what) + ": the skirmish did not run after the pause"
        );
    };
    require(run_for(kRunMs) > 0, "the skirmish does not run");

    step("losing the focus pauses the skirmish, with the paused title");
    const auto unpaused_title = title_pixels();
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
    require(paused() && away_pause_holds(), "losing the focus did not set the pause bit");
    require(!match_paused_, "losing the focus opened a menu");
    require(
        changed_pixels(unpaused_title, title_pixels()) >= kTextMinPixels,
        "the paused title did not show while the window was away"
    );
    require(run_for(kRunMs) == 0 && paused(), "the skirmish ran while the window was away");
    require(!keeps_playing_away(), "Pause single player kept the loop running while away");

    step("the focus back: still paused for a moment, then the skirmish goes on");
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    require(paused(), "the focus coming back resumed the skirmish at once");
    comes_back("the focus coming back");

    step("the click that brings the focus back gives no order while the pause holds");
    {
        window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
        window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
        require(battlefield_in_play(), "the pause the window's absence holds left no play");
        const float x = static_cast<float>(match_layout_.left) +
                        static_cast<float>(match_layout_.battlefield_width()) * 0.5F;
        const float y = static_cast<float>(match_layout_.top) +
                        static_cast<float>(match_layout_.battlefield_height()) * 0.5F;
        float window_x = 0.0F;
        float window_y = 0.0F;
        require(frame_to_window(sdl_.renderer, x, y, &window_x, &window_y), SDL_GetError());
        SDL_Event click{};
        click.button.windowID = window_id;
        click.button.button = SDL_BUTTON_LEFT;
        click.button.clicks = 1;
        click.button.x = window_x;
        click.button.y = window_y;
        const auto press = [&](bool down) {
            click.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            click.button.down = down;
            dispatch_event(click, running);
            require(running, "the click ended the run");
        };
        match_drag_.reset();
        press(true);
        require(!match_drag_, "the click that brought the focus back reached the battlefield");
        press(false);
        require(!match_drag_, "the focusing click's release reached the battlefield");
        comes_back("the focusing click");
        // The next click is the battlefield's again.
        press(true);
        require(match_drag_.has_value(), "a click after the pause did not reach the battlefield");
        press(false);
        match_drag_.reset();
    }

    step("a pause the player chose stays: the Pause key");
    key(SDLK_PAUSE, SDL_SCANCODE_PAUSE);
    require(paused() && !away_pause_holds(), "Pause did not pause the skirmish");
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
    require(paused() && !away_pause_holds(), "losing the focus took over the player's pause");
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    require(run_for(kRunMs) == 0 && paused(), "the focus coming back resumed the player's pause");
    key(SDLK_PAUSE, SDL_SCANCODE_PAUSE);
    require(!paused() && run_for(kRunMs) > 0, "Pause again did not resume the skirmish");

    step("a pause the player chose stays: the game menu");
    key(SDLK_F2, SDL_SCANCODE_F2);
    require(match_paused_ && !paused(), "F2 did not open the game menu");
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
    require(!paused() && !away_pause_holds(), "losing the focus over the game menu paused again");
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    require(
        run_for(kRunMs) == 0 && match_paused_ && !match_clock_steps(),
        "the focus coming back closed the game menu or ran the skirmish"
    );
    resume_match_pause();
    require(!match_paused_ && run_for(kRunMs) > 0, "Resume did not run the skirmish");

    step("Pause while the pause waits keeps the skirmish paused as the player's");
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    std::ignore = run_for(4 * kFrameMs);
    key(SDLK_PAUSE, SDL_SCANCODE_PAUSE);
    require(paused() && !away_pause_holds(), "Pause while the pause waited resumed the skirmish");
    require(run_for(kRunMs) == 0 && paused(), "the player's pause ended with the window's");
    key(SDLK_PAUSE, SDL_SCANCODE_PAUSE);
    require(!paused() && run_for(kRunMs) > 0, "Pause again did not resume the skirmish");

    step("Resume on the game menu while the pause waits runs the skirmish");
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    key(SDLK_F2, SDL_SCANCODE_F2);
    require(match_paused_ && paused(), "F2 did not open the game menu over the pause");
    resume_match_pause();
    require(
        !match_paused_ && !paused() && !away_pause_holds() && run_for(kRunMs) > 0,
        "Resume on the game menu left the skirmish paused"
    );

    step("away again before the moment ends: the pause waits again");
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    std::ignore = run_for(6 * kFrameMs);
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
    require(run_for(kRunMs) == 0 && paused(), "the pause ended while the window was away again");
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    comes_back("the focus coming back again");

    step("a minimised window pauses the skirmish until it is restored");
    window_event(SDL_EVENT_WINDOW_MINIMIZED);
    require(paused() && away_pause_holds(), "minimising the window did not pause the skirmish");
    require(run_for(kRunMs) == 0 && paused(), "the skirmish ran while the window was minimised");
    window_event(SDL_EVENT_WINDOW_RESTORED);
    comes_back("restoring the window");

    step("the application in the background pauses the skirmish and opens no menu");
    lifecycle(SDL_EVENT_WILL_ENTER_BACKGROUND);
    require(paused() && away_pause_holds(), "going to the background did not pause the skirmish");
    require(!match_paused_, "going to the background opened the game menu");
    require(run_for(kRunMs) == 0 && paused(), "the skirmish ran while the application was away");
    lifecycle(SDL_EVENT_DID_ENTER_FOREGROUND);
    comes_back("coming back to the foreground");

    step("Keep playing never pauses");
    choose(settings::FocusLost::keep_playing);
    require(
        away_action() == AwayAction::keep_playing && keeps_playing_away(),
        "Keep playing does not keep the loop running while the window is away"
    );
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
    require(!paused(), "losing the focus paused the skirmish under Keep playing");
    require(run_for(kRunMs) > 0, "the skirmish did not play on while the window was away");
    window_event(SDL_EVENT_WINDOW_MINIMIZED);
    require(!paused() && run_for(kRunMs) > 0, "a minimised window paused Keep playing");
    window_event(SDL_EVENT_WINDOW_RESTORED);
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    require(!paused() && run_for(kRunMs) > 0, "the focus coming back held Keep playing");
    // The system stops an application in the background: there the match
    // waits behind its game menu, as before the setting.
    lifecycle(SDL_EVENT_WILL_ENTER_BACKGROUND);
    require(
        match_paused_ && !paused() && !keeps_playing_away(),
        "going to the background did not hold Keep playing behind the game menu"
    );
    lifecycle(SDL_EVENT_DID_ENTER_FOREGROUND);
    require(match_paused_, "coming back to the foreground closed the game menu");
    resume_match_pause();
    require(!match_paused_ && run_for(kRunMs) > 0, "Resume did not run the skirmish");

    step("a game a multiplayer session is open for never pauses");
    choose(settings::FocusLost::pause_single_player);
    game.session_flags =
        static_cast<uint16_t>(game.session_flags | oa::ui::frontend_state::flags::live_game);
    require(
        keeps_running_inactive() && away_action() == AwayAction::none && !keeps_playing_away(),
        "the live-game bit did not make the skirmish a multiplayer game"
    );
    window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
    require(!paused() && !away_pause_holds(), "losing the focus paused a multiplayer game");
    window_event(SDL_EVENT_WINDOW_MINIMIZED);
    require(!paused(), "minimising the window paused a multiplayer game");
    window_event(SDL_EVENT_WINDOW_RESTORED);
    window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
    game.session_flags =
        static_cast<uint16_t>(game.session_flags & ~oa::ui::frontend_state::flags::live_game);
    require(!paused() && run_for(kRunMs) > 0, "the skirmish did not run after the session");

    frame_run_clock_ns_.reset();
    std::cout << "away pause check: Pause single player held the skirmish with the paused title "
                 "while the window was away, minimised or in the background and let it go "
                 "half a second after it came back, kept the player's Pause and game menu, and "
                 "gave the focusing click no order; Keep playing played on; a multiplayer game "
                 "never paused\n";
}

} // namespace oa::app
