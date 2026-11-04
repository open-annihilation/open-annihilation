// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What a request to close the window runs over a game's life with the
// Boneyards registered: the Boneyards box "Exit to the system?", which the
// Boneyards answers first while it has it installed and the box opens,
// else network play's installed close handler. Each case plays the game's
// modes, matches, the Boneyards coming up and going down and a Boneyards
// launch into both, and checks which of the two answers. A request to a
// box that cannot open runs the Boneyards' answer to the request with the
// console's own box, which no interface is up to open. The test reaches
// network play through its public headers alone; which handler network play
// has installed in each case is checked by netgame-close-handlers.
#include "boneyards_exit_prompt.hpp"
#include "boneyards_launch.hpp"

#include "oa/app/netgame/extension_api.hpp"
#include "oa/ui/frontend_state/dispatcher.hpp"

#include <cstdio>

namespace oa::app {
// The Boneyards console's "Exit to the system?" box (console_screens.cpp), which
// opens only while the Boneyards' interface is up.
bool boneyards_console_ask_exit();
} // namespace oa::app

namespace {

namespace mode_id = oa::ui::frontend_state::mode_id;
using namespace oa::app;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// Who answers a request to close the window.
enum class Answer : uint8_t {
    exit_prompt,  // the Boneyards box "Exit to the system?"
    network_play, // network play's installed close handler
};

// The game as a frame, a request or a mode set sees it.
struct Seen {
    int32_t frontend_mode{};
    bool match_runs{};   // a match runs or loads
    bool boneyards_up{}; // the Boneyards is up
    bool launched{};     // a launch is active
};

// The Boneyards box as the running game keeps it.
struct Game {
    ExitPrompt prompt{};
    bool match_started{}; // the Boneyards heard the running match start
};

ExitPromptObservation boneyards(const Seen& now) {
    ExitPromptObservation observed{};
    observed.frontend_mode = now.frontend_mode;
    observed.boneyards_up = now.boneyards_up;
    observed.launched = now.launched;
    return observed;
}

// A match that runs has started: the Boneyards hears it once
// (Extension::match_game).
void follow_match(Game& game, const Seen& now) {
    if (now.match_runs && !game.match_started) {
        game.match_started = true;
        exit_prompt_match_started(game.prompt);
    }
}

// A frame, or a request before it is answered.
void observe(Game& game, const Seen& now) {
    follow_match(game, now);
    exit_prompt_observe(game.prompt, boneyards(now));
}

// An application mode the frontend sets.
void mode_set(Game& game, int32_t mode, const Seen& now) {
    follow_match(game, now);
    exit_prompt_mode_set(game.prompt, mode, boneyards(now));
}

// A match that finished, was left or was torn down.
void match_ended(Game& game) {
    game.match_started = false;
    exit_prompt_match_ended(game.prompt);
}

// The answer while the Boneyards' interface is up, so the box opens: the
// Boneyards box while it is installed, as the Boneyards' close_requested
// gives it; else the request falls to network play's installed handler.
Answer answer(const Game& game) {
    return game.prompt.installed ? Answer::exit_prompt : Answer::network_play;
}

bool test_battle_ids(uint32_t* area_id, const char** game_id) {
    *area_id = 709;
    *game_id = "0c0ffee0d15ea5e0";
    return true;
}

// A Boneyards launch applied into the game.
void launch_into(Game& game) {
    BoneyardsLaunch boneyards{};
    boneyards.battle_ids = test_battle_ids;
    boneyards.exit_prompt = &game.prompt;
    bind_boneyards_launch(boneyards);
    oa::boneyards::LaunchReport report{};
    report.game_id = "0c0ffee0d15ea5e0";
    report.address = "127.0.0.1";
    on_boneyards_launch(nullptr, &report, "Ghost");
    bind_boneyards_launch({});
}

// The answer each application mode set gives: the Boneyards box while the
// Boneyards is up outside a battle it launched, whatever the mode; else
// network play's handler.
void test_answer_per_mode() {
    constexpr int32_t kModes = 8; // 0 start, 1 return, 2 frontend, 3 multiplayer idle,
                                  // 4 skirmish setup, 5 load, 6 match, 7 end of game
    for (int32_t mode = 0; mode < kModes; ++mode) {
        Seen now{};
        now.frontend_mode = mode;
        Game down{};
        mode_set(down, mode, now);
        check(
            answer(down) == Answer::network_play,
            "each mode set leaves the request to network play while the Boneyards is down"
        );
        now.boneyards_up = true;
        Game up{};
        mode_set(up, mode, now);
        check(
            answer(up) == Answer::exit_prompt,
            "each mode set gives the Boneyards box while the Boneyards is up"
        );
        // Entering the mode from another with the Boneyards down removes the
        // box.
        Game entering{};
        entering.prompt.mode = mode == 0 ? 1 : 0;
        entering.prompt.installed = true;
        entering.prompt.ready = true;
        now.boneyards_up = false;
        observe(entering, now);
        check(answer(entering) == Answer::network_play, "entering a mode removes the box");
    }
}

// What installs the box, and what keeps it, over a game's life.
void test_answers_kept() {
    Game game{};
    check(answer(game) == Answer::network_play, "the game starts with network play's handler");
    Seen now{};
    now.frontend_mode = mode_id::frontend;
    observe(game, now);
    check(answer(game) == Answer::network_play, "the main menu is network play's");

    // Enter Boneyards: the Boneyards box. It stays after EXIT, on the main
    // menu, until the game next sets a mode.
    now.boneyards_up = true;
    observe(game, now);
    check(answer(game) == Answer::exit_prompt, "the Boneyards asks Exit to the system?");
    now.boneyards_up = false;
    observe(game, now);
    check(
        answer(game) == Answer::exit_prompt,
        "the main menu after EXIT still asks Exit to the system?"
    );
    now.frontend_mode = mode_id::loading_return;
    observe(game, now);
    check(answer(game) == Answer::network_play, "the next mode removes the box");

    // A match played on this machine: network play answers from its first
    // frame, while a page opens over it and after its end.
    now.frontend_mode = mode_id::frontend;
    observe(game, now);
    now.match_runs = true;
    observe(game, now);
    check(answer(game) == Answer::network_play, "a match is network play's");
    now.match_runs = false;
    observe(game, now);
    check(answer(game) == Answer::network_play, "a page over the match is network play's");
    match_ended(game);
    observe(game, now);
    check(answer(game) == Answer::network_play, "the finished match is network play's");

    // A launch from the Boneyards: applying it removes the box, and network
    // play answers through the multiplayer screens, the launched battle, its
    // end of game and the return to the main menu; the return to the
    // Boneyards asks Exit to the system? again.
    now.boneyards_up = true;
    observe(game, now);
    check(answer(game) == Answer::exit_prompt, "back in the Boneyards");
    launch_into(game);
    check(answer(game) == Answer::network_play, "the launch removes the box");
    check(oa::app::netgame::extension_api::launch_active(), "the launch is active");
    now.launched = true;
    observe(game, now);
    check(answer(game) == Answer::network_play, "the multiplayer screens are network play's");
    now.match_runs = true;
    observe(game, now);
    check(answer(game) == Answer::network_play, "the launched battle is network play's");
    now.match_runs = false;
    match_ended(game);
    observe(game, now);
    check(answer(game) == Answer::network_play, "its end of game is network play's");
    now.frontend_mode = mode_id::loading_return;
    observe(game, now);
    check(answer(game) == Answer::network_play, "the return to the main menu is network play's");
    oa::app::netgame::extension_api::end_launch();
    now.launched = false;
    observe(game, now);
    check(
        answer(game) == Answer::exit_prompt, "the return to the Boneyards asks Exit to the system?"
    );
}

// Each application mode the game sets gives its answer, the mode it
// already runs included; seeing the same mode at a frame changes nothing.
void test_answers_mode_set_again() {
    Game game{};
    Seen now{};
    now.frontend_mode = mode_id::frontend;
    observe(game, now);

    // After the Boneyards' EXIT the main menu asks Exit to the system?, and
    // frames that see the frontend's mode unchanged keep it.
    now.boneyards_up = true;
    observe(game, now);
    now.boneyards_up = false;
    observe(game, now);
    observe(game, now);
    check(answer(game) == Answer::exit_prompt, "frames in the same mode keep Exit to the system?");
    // A briefing's return to the single-player menu sets the frontend's
    // mode again: network play's handler.
    mode_set(game, mode_id::frontend, now);
    check(
        answer(game) == Answer::network_play && game.prompt.mode == mode_id::frontend,
        "the frontend's mode set again removes the box"
    );

    // While the Boneyards is up outside a battle it launched, any mode set,
    // the same one again included, gives the Boneyards box.
    now.boneyards_up = true;
    observe(game, now);
    game.prompt.installed = false;
    mode_set(game, mode_id::frontend, now);
    check(
        answer(game) == Answer::exit_prompt, "a mode set while the Boneyards is up gives its box"
    );
    now.boneyards_up = false;

    // A match keeps its mode while the end-of-game screen sets its own; the
    // finished match then follows the end-of-game screen's mode.
    mode_set(game, mode_id::frontend, now);
    now.match_runs = true;
    observe(game, now);
    check(answer(game) == Answer::network_play, "the match is network play's");
    now.match_runs = false;
    now.frontend_mode = mode_id::end_game;
    mode_set(game, mode_id::end_game, now);
    check(
        answer(game) == Answer::network_play && game.prompt.mode == mode_id::in_match,
        "the end-of-game screen's mode is network play's and the match keeps its mode"
    );
    match_ended(game);
    observe(game, now);
    check(
        answer(game) == Answer::network_play && game.prompt.mode == mode_id::end_game,
        "the finished match follows the end-of-game screen's mode"
    );
}

// A match that starts after the Boneyards' EXIT, with no mode set between
// (MULTI and a battle hosted from its screens): its first frame removes the
// box, so network play answers.
void test_match_after_exit() {
    Game game{};
    Seen now{};
    now.frontend_mode = mode_id::frontend;
    observe(game, now);
    now.boneyards_up = true;
    observe(game, now);
    now.boneyards_up = false;
    observe(game, now);
    check(answer(game) == Answer::exit_prompt, "the main menu after EXIT asks Exit to the system?");
    now.match_runs = true;
    observe(game, now);
    check(answer(game) == Answer::network_play, "the battle is network play's");
    now.match_runs = false;
    match_ended(game);
    observe(game, now);
    check(answer(game) == Answer::network_play, "its end is network play's");
}

// The boxes a request opened while the Boneyards' interface is up.
int g_boxes_opened = 0;

// Opens the box, as the console does while the Boneyards' interface is up.
bool box_opens() {
    ++g_boxes_opened;
    return true;
}

// The box installed but unable to open (the Boneyards' interface is down, as
// once the main menu after EXIT has been left for another screen that sets
// no mode): the Boneyards' answer to the request, with the console's own box,
// declines, so the request falls to network play's installed handler, and
// the box stays installed for the next request, which opens it once it can.
void test_box_installed_cannot_open() {
    Game game{};
    Seen now{};
    now.frontend_mode = mode_id::frontend;
    observe(game, now);
    now.boneyards_up = true;
    observe(game, now);
    now.boneyards_up = false;
    observe(game, now);
    check(game.prompt.installed, "the main menu after EXIT keeps the box installed");
    check(
        !exit_prompt_close_requested(
            game.prompt, boneyards(now), oa::app::boneyards_console_ask_exit
        ),
        "a box that cannot open leaves the request to network play's handler"
    );
    check(game.prompt.installed, "the box stays installed for the next request");
    g_boxes_opened = 0;
    check(
        exit_prompt_close_requested(game.prompt, boneyards(now), box_opens) && g_boxes_opened == 1,
        "the next request opens it once it can"
    );
    mode_set(game, mode_id::frontend, now);
    g_boxes_opened = 0;
    check(
        !exit_prompt_close_requested(game.prompt, boneyards(now), box_opens) && g_boxes_opened == 0,
        "a mode set removes it: the request asks for no box"
    );
}

} // namespace

int main() {
    test_answer_per_mode();
    test_answers_kept();
    test_answers_mode_set_again();
    test_match_after_exit();
    test_box_installed_cannot_open();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("Boneyards exit prompt tests passed\n");
    return 0;
}
