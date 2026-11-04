// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The handler a request to close the window runs in each application mode,
// as the game keeps it installed through its modes, its matches and a
// launch, and the disconnect reason a leave shows.
#include "close_handlers.hpp"
#include "launch_binding.hpp"

#include "oa/ui/frontend_state/dispatcher.hpp"

#include <cstdio>
#include <cstring>

namespace {

namespace nl = oa::app::netgame::launch;
using namespace oa::app;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// The disconnect reason a leave shows.
void test_leave_reason() {
    oa::Game game{};
    game.local_player_index = 1;
    check(leave_reason(game) == nullptr, "no reason, no text");
    game.players[1].reject_reason = 6;
    check(
        leave_reason(game) != nullptr &&
            std::strcmp(leave_reason(game), "You have lost connection with the game") == 0,
        "the local player's reason gives its text"
    );
    game.players[1].reject_reason = 2;
    check(leave_reason(game) == nullptr, "a player who left by itself is told nothing");
    game.players[1].reject_reason = 1;
    check(
        leave_reason(game) != nullptr &&
            std::strcmp(leave_reason(game), "You were rejected from the game") == 0,
        "a kick still shows"
    );
    game.local_player_index = 10;
    check(leave_reason(game) == nullptr, "no local player, no text");
}

// The handler setting each application mode installs: the exit confirmation
// in a match and the leave in every other mode.
void test_close_handler_per_mode() {
    namespace mode_id = oa::ui::frontend_state::mode_id;
    constexpr int32_t kModes = 8; // 0 start, 1 return, 2 frontend, 3 multiplayer idle,
                                  // 4 skirmish setup, 5 load, 6 match, 7 end of game
    for (int32_t mode = 0; mode < kModes; ++mode) {
        const CloseHandler expected =
            mode == mode_id::in_match ? CloseHandler::exit_confirm : CloseHandler::leave;
        check(
            app_mode_close_handler(mode) == expected,
            "each mode installs the leave, or the exit confirmation in a match"
        );
        // Entering the mode from another installs its handler.
        CloseHandlers handlers{};
        handlers.mode = mode == 0 ? 1 : 0;
        handlers.installed = CloseHandler::none;
        CloseObservation now{};
        now.frontend_mode = mode;
        close_handlers_observe(handlers, now);
        check(
            handlers.installed == expected && handlers.mode == mode,
            "entering a mode installs its handler"
        );
    }
}

// What installs the handler, and what keeps it, over a game's life.
void test_close_handlers_kept() {
    namespace mode_id = oa::ui::frontend_state::mode_id;
    CloseHandlers handlers{};
    check(handlers.installed == CloseHandler::leave, "the game starts with the leave");
    CloseObservation now{};
    now.frontend_mode = mode_id::frontend;
    close_handlers_observe(handlers, now);
    check(handlers.installed == CloseHandler::leave, "the main menu leaves");
    now.frontend_mode = mode_id::loading_return;
    close_handlers_observe(handlers, now);
    check(handlers.installed == CloseHandler::leave, "the next mode installs the leave");

    // A match played on this machine: the exit confirmation from its first
    // frame, kept while a page opens over it, then the end of game leaves.
    now.frontend_mode = mode_id::frontend;
    close_handlers_observe(handlers, now);
    now.match_runs = true;
    close_handlers_observe(handlers, now);
    check(
        handlers.installed == CloseHandler::exit_confirm && handlers.in_match,
        "a match asks for confirmation"
    );
    now.match_runs = false;
    close_handlers_observe(handlers, now);
    check(
        handlers.installed == CloseHandler::exit_confirm,
        "a page over the match keeps the confirmation"
    );
    close_handlers_match_ended(handlers);
    close_handlers_observe(handlers, now);
    check(
        handlers.installed == CloseHandler::leave && !handlers.in_match, "the finished match leaves"
    );

    // A launch: applying it installs the exit confirmation, which the
    // multiplayer screens keep until a mode is set; its battle asks for
    // confirmation, its end of game leaves, and so does the return.
    nl::LaunchBlock block{};
    uint8_t pending = 0;
    LaunchBinding launch{};
    launch.block = &block;
    launch.launch_pending = &pending;
    launch.close_handlers = &handlers;
    bind_launch(launch);
    apply_launch();
    check(handlers.installed == CloseHandler::exit_confirm, "the launch asks for confirmation");
    close_handlers_observe(handlers, now);
    check(
        handlers.installed == CloseHandler::exit_confirm,
        "the multiplayer screens keep the confirmation"
    );
    now.match_runs = true;
    close_handlers_observe(handlers, now);
    check(handlers.installed == CloseHandler::exit_confirm, "the launched battle confirms");
    now.match_runs = false;
    close_handlers_match_ended(handlers);
    close_handlers_observe(handlers, now);
    check(handlers.installed == CloseHandler::leave, "its end of game leaves");
    now.frontend_mode = mode_id::loading_return;
    close_handlers_observe(handlers, now);
    check(handlers.installed == CloseHandler::leave, "the return to the main menu leaves");
    end_launch();
    bind_launch({});
}

// Each application mode the game sets installs its handler, the mode it
// already runs included; seeing the same mode at a frame installs nothing.
void test_close_handlers_mode_set_again() {
    namespace mode_id = oa::ui::frontend_state::mode_id;
    CloseHandlers handlers{};
    CloseObservation now{};
    now.frontend_mode = mode_id::frontend;
    close_handlers_observe(handlers, now);

    // A launch's exit confirmation holds until the game sets a mode, the
    // one it runs included.
    handlers.installed = CloseHandler::exit_confirm;
    close_handlers_observe(handlers, now);
    check(handlers.installed == CloseHandler::exit_confirm, "frames keep the confirmation");
    close_handlers_mode_set(handlers, mode_id::frontend, now);
    check(
        handlers.installed == CloseHandler::leave && handlers.mode == mode_id::frontend,
        "the mode set again replaces the confirmation"
    );

    // A match keeps its mode while the end-of-game screen sets its own, whose
    // handler it installs; the finished match then leaves nothing to change.
    now.match_runs = true;
    close_handlers_observe(handlers, now);
    check(handlers.installed == CloseHandler::exit_confirm, "the match asks for confirmation");
    now.match_runs = false;
    now.frontend_mode = mode_id::end_game;
    close_handlers_mode_set(handlers, mode_id::end_game, now);
    check(
        handlers.installed == CloseHandler::leave && handlers.mode == mode_id::in_match,
        "the end-of-game screen's mode installs the leave and the match keeps its mode"
    );
    close_handlers_match_ended(handlers);
    close_handlers_observe(handlers, now);
    check(
        handlers.installed == CloseHandler::leave && handlers.mode == mode_id::end_game,
        "the finished match follows the end-of-game screen's mode"
    );
}

} // namespace

int main() {
    test_leave_reason();
    test_close_handler_per_mode();
    test_close_handlers_kept();
    test_close_handlers_mode_set_again();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("close handler tests passed\n");
    return 0;
}
