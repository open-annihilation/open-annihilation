// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-boneyards-launch and --check-boneyards-features, played from an
// overlay ticked on every screen of the running game. Test scaffolding: the
// Boneyards, the multiplayer screens and the battle it drives are implemented
// elsewhere.
#include "boneyards_launch_check.hpp"
#include "boneyards_launch.hpp"

#include "battle_console.hpp"
#include "console_state.hpp"

#include "oa/app/app.hpp"
#include "oa/app/check_host.hpp"
#include "oa/boneyards/console/feature_consoles.hpp"
#include "oa/boneyards/console/registration.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/rover_replay.hpp"
#include "oa/boneyards/screens/locator.hpp"
#include "oa/boneyards/screens/login.hpp"
#include "oa/boneyards/screens/quickplay.hpp"
#include "oa/boneyards/screens/registration.hpp"
#include "oa/core/game_state.h"
#include "oa/app/netgame/extension_api.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/panel.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/platform/system.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace oa::app {

namespace {

namespace by = oa::boneyards;
namespace by_console = oa::boneyards::console;
namespace by_screens = oa::boneyards::screens;
namespace mp = oa::ui::frontend_multiplayer;
namespace exit_code = launch_check_exit;

// What each side types at the login and says in the battle.
constexpr const char* kGuestName[] = {"oa_host", "oa_joiner"};
constexpr const char* kChatLine[] = {"host here, good luck", "joiner here, have fun"};
constexpr const char* kHostedLine = "boneyards launch check: the battle room is hosted";
// The game the host names on the Host pane, and the game type it picks.
constexpr const char* kHostedName = "Launch Check";
constexpr const char* kHostedType = "Galactic War";
constexpr int32_t kPlayersInBattle = 2;
// What the replays' launch names, as the records and screens show it.
constexpr const char* kLaunchMission = "Coast To Coast";
constexpr const char* kAreaField = "<U:area_id=709>";
constexpr const char* kLaunchLabel = "Boneyards";
// Longest a stage may run, in frames: two minutes at the game's full rate.
// The same count takes longer when a frame takes longer, so a slow run still
// reaches the screen. OA_TEST_TIMEOUT_SCALE, a positive number, multiplies
// the count, as the engine's headless checks multiply their run limit.
constexpr uint32_t kStageLimitFrames = 120 * kDefaultMaxFramesPerSecond;
// The pause between repeated presses.
constexpr uint32_t kRepeatFrames = 60;
// Frames between the reports of what a stage waits for.
constexpr uint32_t kReportFrames = 600;
// The login console settles this many frames before the name is typed, and
// Enter follows as many frames later; a login that did not start is typed
// again this often, after Backspaces enough to clear the name field.
constexpr uint32_t kLoginSettleFrames = 30;
constexpr uint32_t kLoginRetypeFrames = 240;
constexpr uint32_t kNameClearKeys = 0x20;
// Frames the battle runs before the chat line, and the joiner's commander
// self-destructs after the chat line has gone.
constexpr uint32_t kChatAfterFrames = 90;
constexpr uint32_t kDestructAfterFrames = 60;
// The centre of the end-of-game screen's MainMenu button (ENDMSN.GUI, 120 by
// 20 at x 460) on the 640x480 canvas, where a battle that cannot go on puts
// it: 416 down.
constexpr float kMainMenuX = 460 + 60;
constexpr float kMainMenuY = 416 + 10;
constexpr int32_t kLoggedInLevel = static_cast<int32_t>(by::SessionLevel::logged_in);
constexpr int32_t kDossierLevel = static_cast<int32_t>(by::SessionLevel::dossier_loaded);
constexpr ScreenId kMatchScreen = screen_id(Screen::match);

enum class Stage : uint8_t {
    main_menu,           // the main menu and its Boneyards strip: Enter Boneyards (B)
    login,               // the login console: the guest name, Enter
    galactic_map,        // the dossier and the loaded war map
    launch,              // the battle list, then HOST and START, or the battle and JOIN
    battle_room,         // both players in the battle room; READY
    start,               // the host's START; the joiner waits for the battle
    battle,              // the chat line; the joiner's self-destruct
    end_screen,          // through the end-of-game screen to MainMenu
    return_to_boneyards, // the Boneyards up again, in the debriefing room
    done,
};

constexpr const char* kStageNames[] = {
    "main menu",
    "login",
    "galactic map",
    "launch",
    "battle room",
    "start",
    "battle",
    "end screen",
    "return",
    "done",
};

constexpr int kStageExit[] = {
    exit_code::no_main_menu,
    exit_code::no_login,
    exit_code::no_galactic_map,
    exit_code::no_launch,
    exit_code::no_battle_room,
    exit_code::not_started,
    exit_code::no_match,
    exit_code::no_end,
    exit_code::no_return,
    exit_code::passed,
};

// How far the launch stage has driven the battle screens. `sent` waits for the battle room.
enum class Drive : uint8_t {
    battle_button,
    host_button,
    host_name,
    host_type,
    host_start,
    join_row,
    join_button,
    sent,
};

struct LaunchCheck {
    LaunchCheckRole role{};
    Stage stage{};
    Drive drive{};
    // Frames the current screen step has been ready to press, and whether it has had a target.
    uint32_t step_frames{};
    bool step_armed{};
    // Frames the current stage may run before the check fails it.
    uint32_t stage_limit{kStageLimitFrames};
    uint32_t stage_frames{};
    uint32_t battle_frames{};
    uint32_t login_frames{};
    bool said{};
    bool destructed{};
    bool ready_pressed{};
    bool reset_modifiers{};
    bool finished{};
    // Frames network play's launch was active on, and whether it has once
    // disagreed with the Boneyards' launched battle.
    uint32_t launch_frames{};
    bool launch_disagreed{};
    // What a stage found wrong on the way; the check fails at the end.
    std::vector<std::string> problems;
};

LaunchCheck g_check;

/// Returns the index of the side the check plays: 0 for the host, 1 for the joiner.
std::size_t side(const LaunchCheck& check) {
    return check.role == LaunchCheckRole::host ? 0 : 1;
}

/// Prints a line of the check's progress to standard output.
void report(const std::string& line) {
    std::printf("boneyards launch check: %s\n", line.c_str());
    std::fflush(stdout);
}

// ---- input through SDL's queue ---------------------------------------------

/// Returns the game's window, or null before it is open.
SDL_Window* game_window() {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    SDL_Window* window = windows != nullptr && count > 0 ? windows[0] : nullptr;
    SDL_free(windows);
    return window;
}

/// Presses and releases a key; the modifiers are held from now until the
/// next tick, when the pressed key has been handled.
void press_key(SDL_Keycode key, SDL_Keymod modifiers = SDL_KMOD_NONE) {
    SDL_Window* window = game_window();
    SDL_SetModState(modifiers);
    g_check.reset_modifiers = modifiers != SDL_KMOD_NONE;
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = window != nullptr ? SDL_GetWindowID(window) : 0;
    event.key.key = key;
    event.key.mod = modifiers;
    event.key.down = true;
    SDL_PushEvent(&event);
    event.type = SDL_EVENT_KEY_UP;
    event.key.down = false;
    SDL_PushEvent(&event);
}

/// Types text; `text` must stay valid until the event has been handled.
void type_text(const char* text) {
    SDL_Window* window = game_window();
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.windowID = window != nullptr ? SDL_GetWindowID(window) : 0;
    event.text.text = text;
    SDL_PushEvent(&event);
}

/// Clicks the left button at a point of the 640x480 canvas.
void click_canvas(float x, float y) {
    SDL_Window* window = game_window();
    SDL_Renderer* renderer = window != nullptr ? SDL_GetRenderer(window) : nullptr;
    float window_x = x;
    float window_y = y;
    if (renderer != nullptr)
        (void)SDL_RenderCoordinatesToWindow(renderer, x, y, &window_x, &window_y);
    const SDL_WindowID id = window != nullptr ? SDL_GetWindowID(window) : 0;
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.windowID = id;
    event.motion.x = window_x;
    event.motion.y = window_y;
    SDL_PushEvent(&event);
    for (const bool down : {true, false}) {
        event = SDL_Event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.windowID = id;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = down;
        event.button.clicks = 1;
        event.button.x = window_x;
        event.button.y = window_y;
        SDL_PushEvent(&event);
    }
}

/// Notes a problem the check reports and fails with at the end.
void expect(bool holds, const std::string& what) {
    if (holds)
        return;
    report("wrong: " + what);
    g_check.problems.push_back(what);
}

// ---- the records Rover queued for the Boneyards ----------------------------

/// Returns the text of every record Rover has queued for the Boneyards so far.
std::vector<std::string> sent_records() {
    std::vector<std::string> records;
    by::ReplaySent sent{};
    for (std::size_t i = 0; by::rover_replay_sent(i, &sent); ++i)
        if (sent.request == by::ReplayRequest::record)
            records.emplace_back(sent.text);
    return records;
}

/// Tells whether a record's text holds a field.
bool has(const std::string& record, const std::string& field) {
    return record.find(field) != std::string::npos;
}

/// Counts the records of a command that hold every one of the fields.
std::size_t count_records(
    const std::vector<std::string>& records,
    const std::string& command,
    const std::vector<std::string>& fields = {}
) {
    std::size_t count = 0;
    for (const std::string& record : records) {
        bool matches = has(record, "<S:command=" + command + ">");
        for (const std::string& field : fields)
            matches = matches && has(record, field);
        count += matches ? 1 : 0;
    }
    return count;
}

/// Returns the side's chat line as the score report message carries it:
/// "<name> text", with '<' and '>' as %4 and %5.
std::string reported_chat(const LaunchCheck& check) {
    return std::string("<S:message=%4") + kGuestName[side(check)] + "%5 " + kChatLine[side(check)] +
           ">";
}

/// Returns the field naming the launched battle's game id.
std::string game_field() {
    return std::string("<S:game_id=") + kLaunchCheckGame + ">";
}

/// Tells whether a score report of the battle carries the side's chat line.
bool chat_reported(const LaunchCheck& check) {
    return count_records(
               sent_records(), "score_report", {kAreaField, game_field(), reported_chat(check)}
           ) != 0;
}

/// Tells whether a score report went out while the battle ran: one with no message
/// that is not the last.
bool interval_reported() {
    for (const std::string& record : sent_records())
        if (has(record, "<S:command=score_report>") && !has(record, "<S:message=") &&
            !has(record, "<I:last=1>"))
            return true;
    return false;
}

/// Checks what the records must hold once the side is back in the Boneyards.
void check_records(const LaunchCheck& check) {
    const std::vector<std::string> records = sent_records();
    for (const std::string& record : records)
        report("sent " + record);
    const std::string area = kAreaField;
    const std::string game = game_field();
    if (check.role == LaunchCheckRole::host) {
        expect(
            count_records(
                records, "host_game", {area, std::string("<S:name=") + kHostedName + ">"}
            ) == 1,
            "one host_game for the hosted battle"
        );
        expect(
            count_records(
                records,
                "game_settings",
                {area, game, std::string("<S:mission=") + kLaunchMission + ">"}
            ) == 1,
            "the battle's game_settings, once"
        );
        // The replay's launch locks the options and leaves watching as the
        // battle room opened it, disallowed, which game_settings leaves out.
        expect(
            count_records(records, "game_settings", {"<I:lockOptions=1>"}) == 1,
            "game_settings says the options are locked, as the launch locked them"
        );
        expect(
            count_records(records, "game_settings", {"<I:watching="}) == 0,
            "game_settings says watching is disallowed, as the launch left it"
        );
        expect(
            count_records(
                records, "player_info", {area, game, "<S:name=oa_host>", "<S:allegiance=Arm>"}
            ) == 1 &&
                count_records(
                    records,
                    "player_info",
                    {area, game, "<S:name=oa_joiner>", "<S:allegiance=Core>"}
                ) == 1,
            "a player_info for each player, with its side"
        );
        expect(interval_reported(), "a score_report while the battle ran");
        expect(
            count_records(records, "score_report", {area, game, "<I:last=1>", "<I:victory=1>"}) ==
                1,
            "the final score_report naming the host the winner"
        );
    } else {
        expect(
            count_records(records, "join_game", {area, game}) == 1, "one join_game for the battle"
        );
        expect(
            count_records(records, "game_settings") == 0 &&
                count_records(records, "player_info") == 0,
            "no settings or players from the joiner"
        );
        expect(
            count_records(records, "score_report", {"<I:last=1>"}) == 0,
            "no final score_report from the loser"
        );
    }
    expect(chat_reported(check), "the side's chat line as a score_report message");
    expect(count_records(records, "left_game", {area, game}) == 1, "one left_game on the return");
    // Login asks for the war map before a battle exists. That one record
    // carries the login's area and no game id; every later record names the battle.
    expect(
        count_records(records, "get_sector", {"<U:area_id=0>"}) == 1,
        "one get_sector for the logged-in area"
    );
    for (const std::string& record : records)
        if (!has(record, "<S:command=host_game>") && !has(record, "<S:command=get_sector>"))
            expect(has(record, area) && has(record, game), "a record names the battle: " + record);
}

// ---- stages ------------------------------------------------------------------

/// Ends the check with an exit code, printing that it passed or why it failed.
void finish(ScreenContext* ctx, int code, const std::string& why) {
    g_check.finished = true;
    report(
        "direct packets " +
        std::to_string(oa::app::netgame::extension_api::overlay_direct_packets())
    );
    report(code == exit_code::passed ? std::string("passed") : "failed: " + why);
    if (ctx->services->quit != nullptr)
        ctx->services->quit(ctx->host, nullptr, code);
}

/// The positive factor OA_TEST_TIMEOUT_SCALE names, or 1 when it is unset,
/// empty, or not a positive number.
double timeout_scale() {
    const auto text_value = oa::platform::environment_value("OA_TEST_TIMEOUT_SCALE");
    const char* text = text_value ? text_value->c_str() : nullptr;
    if (text == nullptr)
        return 1.0;
    while (*text == ' ' || *text == '\t')
        ++text;
    if (*text == '\0')
        return 1.0;
    char* end = nullptr;
    const double factor = std::strtod(text, &end);
    while (*end == ' ' || *end == '\t')
        ++end;
    if (end == text || *end != '\0' || !(factor > 0.0))
        return 1.0;
    return factor;
}

/// Frames a stage may run: the frame limit times OA_TEST_TIMEOUT_SCALE.
uint32_t scaled_stage_limit() {
    const double scaled = static_cast<double>(kStageLimitFrames) * timeout_scale();
    if (!(scaled >= 1.0))
        return 1;
    if (scaled >= 4294967295.0)
        return 4294967295u;
    return static_cast<uint32_t>(scaled);
}

/// Moves the check to a stage, restarting the stage's frame count.
void enter(Stage stage) {
    g_check.stage = stage;
    g_check.stage_frames = 0;
    report(std::string("stage ") + kStageNames[static_cast<std::size_t>(stage)]);
}

/// Tells whether a stage repeats its press on this frame.
bool repeat_due() {
    return g_check.stage_frames % kRepeatFrames == 1;
}

/// Returns what the Boneyards status widget shows now.
by_console::MainFrameProbe probe_widget() {
    by_console::MainFrameProbe probe{};
    (void)by_console::boneyards_status_widget_probe(boneyards_console_status_widget(), &probe);
    return probe;
}

/// Clicks the centre of a control.
void click_rect(const by_console::ScreenRect& rect) {
    click_canvas(
        static_cast<float>(rect.x + rect.width / 2), static_cast<float>(rect.y + rect.height / 2)
    );
}

/// Moves the launch stage to its next screen step. The step's first press happens on the frame its
/// target is ready.
void advance(Drive drive) {
    g_check.drive = drive;
    g_check.step_frames = 0;
    g_check.step_armed = false;
}

/// Tells whether a ready target is pressed on this frame: at once when it first becomes ready, and
/// again every kRepeatFrames while it stays ready. A frame without it starts the wait over.
bool press_due(bool ready) {
    if (!ready) {
        g_check.step_armed = false;
        g_check.step_frames = 0;
        return false;
    }
    if (!g_check.step_armed) {
        g_check.step_armed = true;
        g_check.step_frames = 1;
        return true;
    }
    ++g_check.step_frames;
    return g_check.step_frames % kRepeatFrames == 1;
}

/// Tells whether the battle console is up and its list has finished arriving.
bool battle_list_ready(const by_console::MainFrameProbe& probe) {
    return probe.console_screen == by_console::console_screen::battle && probe.screen_shown &&
           !probe.layer_sliding && !probe.activity_light_running;
}

/// Presses Battle until the planet's battle list is open.
void tick_battle_button(const by_console::MainFrameProbe& probe) {
    if (battle_list_ready(probe)) {
        report("the battle list is open");
        advance(g_check.role == LaunchCheckRole::host ? Drive::host_button : Drive::join_row);
        return;
    }
    // Battle toggles the console shut once it is the open screen, so it is pressed only before
    // that, and not while the green layer is sliding or the button is disabled.
    if (probe.console_screen == by_console::console_screen::battle)
        return;
    const by_console::ScreenRect& button =
        probe.console_buttons[static_cast<int32_t>(by_console::ConsoleButton::battle)];
    const bool ready = !probe.layer_sliding && probe.battle_button != by::button_state::disabled &&
                       button.width != 0;
    if (!press_due(ready))
        return;
    click_rect(button);
    report("pressed Battle");
}

/// Presses HOST until the Host pane is open.
void tick_host_button(by_console::StatusWidget* widget) {
    by_console::BattleControlsProbe controls{};
    const bool shown = by_console::boneyards_status_widget_battle_controls(widget, &controls) &&
                       controls.host_pane_shown;
    if (shown) {
        report("the Host pane is open");
        advance(Drive::host_name);
        return;
    }
    const bool ready = controls.host_button_shown && controls.host_button.width != 0;
    if (!press_due(ready))
        return;
    click_rect(controls.host_button);
    report("pressed HOST");
}

/// Clicks the game-name field and types the battle's name until the field shows it.
void tick_host_name(by_console::StatusWidget* widget) {
    by_console::BattleControlsProbe controls{};
    if (!by_console::boneyards_status_widget_battle_controls(widget, &controls) ||
        !controls.host_pane_shown)
        return;
    if (std::strcmp(controls.name_text, kHostedName) == 0) {
        report("named the battle");
        advance(Drive::host_type);
        return;
    }
    if (!press_due(controls.name_entry.width != 0))
        return;
    click_rect(controls.name_entry);
    for (uint32_t i = 0; i < kNameClearKeys; ++i)
        press_key(SDLK_BACKSPACE);
    type_text(kHostedName);
    report("typed the battle's name");
}

/// Presses the Galactic War row until it is the selected game type.
void tick_host_type(by_console::StatusWidget* widget) {
    by_console::BattleTypeProbe type{};
    bool found = false;
    for (int32_t i = 0; by_console::boneyards_status_widget_battle_type(widget, i, &type); ++i) {
        if (std::strcmp(type.name, kHostedType) == 0) {
            found = true;
            break;
        }
    }
    if (found && type.selected) {
        report("picked Galactic War");
        advance(Drive::host_start);
        return;
    }
    if (!press_due(found && type.shown))
        return;
    click_canvas(static_cast<float>(type.x), static_cast<float>(type.y));
    report("pressed Galactic War");
}

/// Presses the Host pane's START once it is enabled.
void tick_host_start(by_console::StatusWidget* widget) {
    by_console::BattleControlsProbe controls{};
    if (!by_console::boneyards_status_widget_battle_controls(widget, &controls) ||
        !controls.host_pane_shown)
        return;
    const bool ready =
        controls.start_state == by::button_state::unpressed && controls.start_button.width != 0;
    if (!press_due(ready))
        return;
    click_rect(controls.start_button);
    report("pressed START");
    advance(Drive::sent);
}

/// Presses the forming battle until its Join summary is open.
void tick_join_row(by_console::StatusWidget* widget) {
    by_console::BattleControlsProbe controls{};
    const bool controls_open =
        by_console::boneyards_status_widget_battle_controls(widget, &controls);
    if (controls_open && controls.join_pane_shown) {
        report("the battle's summary is open");
        advance(Drive::join_button);
        return;
    }
    by_console::BattleRowProbe row{};
    bool found = false;
    for (int32_t i = 0; by_console::boneyards_status_widget_battle_row(widget, i, &row); ++i) {
        if (!row.category && std::strcmp(row.game_id, kLaunchCheckGame) == 0) {
            found = true;
            break;
        }
    }
    if (!press_due(found && row.shown))
        return;
    click_canvas(static_cast<float>(row.x), static_cast<float>(row.y));
    report("pressed the battle");
}

/// Presses JOIN on the battle's summary.
void tick_join_button(by_console::StatusWidget* widget) {
    by_console::BattleControlsProbe controls{};
    if (!by_console::boneyards_status_widget_battle_controls(widget, &controls) ||
        !controls.join_pane_shown) {
        advance(Drive::join_row);
        return;
    }
    const bool ready =
        controls.join_state != by::button_state::disabled && controls.join_button.width != 0;
    if (!press_due(ready))
        return;
    click_rect(controls.join_button);
    report("pressed JOIN");
    advance(Drive::sent);
}

/// Drives the battle list through HOST and START, or through the battle and JOIN.
void tick_battle_screens(by_console::StatusWidget* widget) {
    if (g_check.drive == Drive::sent)
        return;
    const by_console::MainFrameProbe probe = probe_widget();
    if (g_check.drive == Drive::battle_button) {
        tick_battle_button(probe);
        return;
    }
    if (!battle_list_ready(probe))
        return;
    switch (g_check.drive) {
    case Drive::host_button:
        tick_host_button(widget);
        break;
    case Drive::host_name:
        tick_host_name(widget);
        break;
    case Drive::host_type:
        tick_host_type(widget);
        break;
    case Drive::host_start:
        tick_host_start(widget);
        break;
    case Drive::join_row:
        tick_join_row(widget);
        break;
    case Drive::join_button:
        tick_join_button(widget);
        break;
    case Drive::battle_button:
    case Drive::sent:
        break;
    }
}

/// Checks the battle room against the launch and marks the side ready once both players are in.
void tick_battle_room(ScreenContext* ctx) {
    if (ctx->screen != mp::kScreenBattleroom)
        return;
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (lobby.game == nullptr)
        return;
    if (g_check.role == LaunchCheckRole::host && g_check.stage_frames == 1) {
        std::printf("%s\n", kHostedLine);
        std::fflush(stdout);
    }
    if (g_check.stage_frames == 1) {
        // The launch's values, as the battle room took them.
        const mp::PlayerSetupInfo& me = mp::local_info(lobby);
        const bool host = g_check.role == LaunchCheckRole::host;
        expect(((me.role & mp::kRoleHost) != 0) == host, "the side hosts only as the host");
        expect(me.side == (host ? 0 : 1), "the launch's allegiance is the side");
        expect(mp::lobby_launch_active(lobby), "the Boneyards launched the game");
        expect(mp::lobby_options_locked(lobby), "the launch locks the options");
        expect(
            mp::panel_text(mp::multiplayer_panel(), "PREVMENU") == kLaunchLabel,
            "PREVMENU reads the launcher's label"
        );
        const char* label = boneyards_return_label();
        expect(
            label != nullptr && std::strcmp(label, kLaunchLabel) == 0, "the online launch is active"
        );
        if (host)
            expect(
                std::string(me.map_name, ::strnlen(me.map_name, sizeof me.map_name)) ==
                    kLaunchMission,
                "the host plays the launch's map"
            );
    }
    if (mp::lobby_player_count(*lobby.game) < kPlayersInBattle)
        return;
    const int slot = lobby.game->local_player_index;
    char ready[0x10];
    std::snprintf(ready, sizeof ready, "READY%d", slot);
    if (!g_check.ready_pressed || repeat_due()) {
        const auto& me = mp::local_info(lobby);
        if ((me.options & mp::option::ready) == 0) {
            (void)mp::multiplayer_click(ctx, ready);
            report(std::string("pressed ") + ready);
        }
        g_check.ready_pressed = true;
    }
    if ((mp::local_info(lobby).options & mp::option::ready) != 0)
        enter(Stage::start);
}

/// Waits for the battle; the host presses START once it comes on.
void tick_start(ScreenContext* ctx) {
    if (ctx->screen == kMatchScreen && ctx->world != nullptr) {
        g_check.battle_frames = 0;
        enter(Stage::battle);
        return;
    }
    if (g_check.role != LaunchCheckRole::host || ctx->screen != mp::kScreenBattleroom ||
        !repeat_due())
        return;
    const mp::Control* start = mp::panel_control(mp::multiplayer_panel(), "START");
    if (start == nullptr || start->grayed || start->active == 0)
        return;
    if (mp::multiplayer_click(ctx, "START"))
        report("pressed START");
}

/// Sends the side's chat line in the battle; the joiner then self-destructs its commander.
void tick_battle(ScreenContext* ctx) {
    if (ctx->screen != kMatchScreen) {
        enter(Stage::end_screen);
        return;
    }
    if (ctx->world == nullptr)
        return;
    ++g_check.battle_frames;
    if (!g_check.said && g_check.battle_frames == kChatAfterFrames) {
        press_key(SDLK_RETURN);
        type_text(kChatLine[side(g_check)]);
        press_key(SDLK_RETURN);
        g_check.said = true;
        report(std::string("said \"") + kChatLine[side(g_check)] + "\"");
        return;
    }
    if (g_check.role != LaunchCheckRole::join || !g_check.said || g_check.destructed ||
        g_check.battle_frames < kChatAfterFrames + kDestructAfterFrames)
        return;
    // Ctrl+C selects the commander; Ctrl+D a tick later sets it to destruct.
    if (g_check.battle_frames == kChatAfterFrames + kDestructAfterFrames) {
        press_key(SDLK_C, SDL_KMOD_LCTRL);
        return;
    }
    press_key(SDLK_D, SDL_KMOD_LCTRL);
    g_check.destructed = true;
    report("self-destructed the commander");
}

/// Presses through the end-of-game screen: its picture waits for a key, and its
/// panel's MainMenu then leaves for the main menu, which returns to the Boneyards.
void tick_end_screen(ScreenContext* ctx) {
    if (ctx->screen == screen_id(Screen::main_menu)) {
        enter(Stage::return_to_boneyards);
        return;
    }
    if (g_check.stage_frames % kRepeatFrames == 1) {
        press_key(SDLK_SPACE);
        return;
    }
    if (g_check.stage_frames % kRepeatFrames == kRepeatFrames / 2) {
        click_canvas(kMainMenuX, kMainMenuY);
        report("pressed MainMenu on the end-of-game screen " + std::to_string(ctx->screen));
    }
}

/// Waits for the Boneyards' debriefing room, then checks the records and ends the check.
void tick_return(ScreenContext* ctx) {
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    const by_console::MainFrameProbe probe = probe_widget();
    if (!by_console::boneyards_status_widget_up(widget) ||
        !by_console::boneyards_status_widget_in_boneyards(widget) ||
        by_console::boneyards_status_widget_launched_game(widget) ||
        probe.chat_room_id != kLaunchCheckDebriefingRoom)
        return;
    report("back in the Boneyards, in debriefing room " + std::to_string(probe.chat_room_id));
    report(
        "network play's launch was active on " + std::to_string(g_check.launch_frames) +
        " frames, as the Boneyards' launched battle"
    );
    expect(g_check.launch_frames > 0, "network play's launch was never active");
    check_records(g_check);
    enter(Stage::done);
    finish(
        ctx,
        g_check.problems.empty() ? exit_code::passed : exit_code::wrong,
        std::to_string(g_check.problems.size()) + " thing(s) wrong"
    );
}

/// Plays the check's current stage on every frame, and fails a stage that runs past its frame limit.
void tick_check(ScreenContext* ctx, void*) {
    if (g_check.finished)
        return;
    if (g_check.reset_modifiers) {
        SDL_SetModState(SDL_KMOD_NONE);
        g_check.reset_modifiers = false;
    }
    ++g_check.stage_frames;
    if (g_check.stage_frames % kReportFrames == 0) {
        const by_console::MainFrameProbe probe = probe_widget();
        report(
            std::string("waiting in stage ") +
            kStageNames[static_cast<std::size_t>(g_check.stage)] + ": screen " +
            std::to_string(ctx->screen) + ", level " + std::to_string(probe.connection_level) +
            ", console " + std::to_string(probe.console_screen) +
            (probe.screen_shown ? " shown" : "") + ", planets " +
            std::to_string(probe.planet_count) + (probe.galaxy_loading ? " loading" : "") +
            (probe.layer_sliding ? " sliding" : "")
        );
    }
    if (g_check.stage_frames > g_check.stage_limit) {
        const auto index = static_cast<std::size_t>(g_check.stage);
        finish(
            ctx,
            kStageExit[index],
            std::string("timed out in stage ") + kStageNames[index] + " on screen " +
                std::to_string(ctx->screen) +
                (mp::multiplayer_message().empty()
                     ? std::string()
                     : ", message \"" + mp::multiplayer_message() + "\"")
        );
        return;
    }
    // Network play's launch is active exactly while the Boneyards counts its
    // battle as launched: from the launch reply to the return.
    const bool launch_active = oa::app::netgame::extension_api::launch_active();
    if (launch_active)
        ++g_check.launch_frames;
    if (launch_active != boneyards_launched_game() && !g_check.launch_disagreed) {
        g_check.launch_disagreed = true;
        expect(
            false,
            std::string("network play's launch is ") + (launch_active ? "active" : "not active") +
                " in stage " + kStageNames[static_cast<std::size_t>(g_check.stage)] +
                " while the Boneyards' battle is " + (launch_active ? "not launched" : "launched")
        );
    }
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    switch (g_check.stage) {
    case Stage::main_menu:
        if (ctx->screen == screen_id(Screen::main_menu) && widget != nullptr &&
            by_console::boneyards_status_widget_visible(widget) && repeat_due()) {
            press_key(SDLK_B);
            report("pressed Enter Boneyards");
        }
        if (by_console::boneyards_status_widget_in_boneyards(widget))
            enter(Stage::login);
        return;
    case Stage::login: {
        // Once the login console has settled: the name, then Enter once its
        // pulse has turned LOGIN on. A name that did not take is cleared and
        // typed again.
        const by_console::MainFrameProbe probe = probe_widget();
        if (probe.connection_level >= kLoggedInLevel) {
            report(std::string("logged in as ") + kGuestName[side(g_check)]);
            enter(Stage::galactic_map);
            return;
        }
        if (probe.console_screen != by_console::console_screen::login || !probe.screen_shown) {
            g_check.login_frames = 0;
            return;
        }
        ++g_check.login_frames;
        const uint32_t step = g_check.login_frames % kLoginRetypeFrames;
        if (step == kLoginSettleFrames) {
            for (uint32_t i = 0; i < kNameClearKeys; ++i)
                press_key(SDLK_BACKSPACE);
            type_text(kGuestName[side(g_check)]);
            press_key(SDLK_TAB);
        } else if (step == kLoginSettleFrames * 2) {
            press_key(SDLK_RETURN);
        }
        return;
    }
    case Stage::galactic_map: {
        // The battle's planet is pressed on the war map, which makes its
        // area the session's; its census answers the press.
        const by_console::MainFrameProbe probe = probe_widget();
        if (probe.connection_level != kDossierLevel || probe.layer_sliding ||
            probe.galaxy_loading || probe.planet_count == 0)
            return;
        if (probe.selected_area_id == kLaunchCheckArea && !probe.activity_light_running) {
            advance(Drive::battle_button);
            enter(Stage::launch);
            return;
        }
        int32_t x = 0;
        int32_t y = 0;
        if (repeat_due() && by_console::boneyards_status_widget_planet_hotspot(
                                widget, static_cast<int32_t>(kLaunchCheckArea), &x, &y
                            )) {
            click_canvas(static_cast<float>(x), static_cast<float>(y));
            report("pressed the battle's planet at " + std::to_string(x) + "," + std::to_string(y));
        }
        return;
    }
    case Stage::launch:
        // The battle list, then HOST and START or the battle and JOIN. The replay's launch
        // brings the battle room.
        if (by_console::boneyards_status_widget_launched_game(widget) &&
            ctx->screen == mp::kScreenBattleroom)
            enter(Stage::battle_room);
        else
            tick_battle_screens(widget);
        return;
    case Stage::battle_room:
        tick_battle_room(ctx);
        return;
    case Stage::start:
        tick_start(ctx);
        return;
    case Stage::battle:
        tick_battle(ctx);
        return;
    case Stage::end_screen:
        tick_end_screen(ctx);
        return;
    case Stage::return_to_boneyards:
        tick_return(ctx);
        return;
    case Stage::done:
        return;
    }
}

// ---- Phase 3 screens -------------------------------------------------------

namespace scr = oa::boneyards::screens;
namespace feat_exit = features_check_exit;

// REGISTER at (0x6f, 0x7e), in the login console at (0x17, 0x7e).
constexpr int32_t kRegisterClickX = 0x17 + 0x6f + 10;
constexpr int32_t kRegisterClickY = 0x7e + 0x7e + 8;
constexpr uint32_t kClickGap = 60;
constexpr uint32_t kSearchRetryFrames = 90;
// A search click that has not landed is tried again after this many frames.
constexpr uint32_t kClickSettleFrames = 15;

enum class FeatStep : uint8_t {
    main_menu,
    register_click,
    registration,
    form_user,
    form_pass,
    form_confirm,
    form_email,
    form_rules,
    form_send,
    form_box,
    registration_back,
    login,
    map,
    locator_open,
    locate_player,
    locate_planet,
    locate_battle,
    locate_frequency,
    locate_server,
    quickplay,
    quickplay_cancel,
    close_console,
    pick_planet,
    battle_panel,
    chat_open,
    chat_activate,
    chat_deactivate,
    player_battle,
    player_chat,
    done,
};

struct FeaturesRun {
    FeaturesCheckRole role{};
    FeatStep step{};
    uint32_t limit{kStageLimitFrames};
    uint32_t frames{};
    uint32_t login_frames{};
    uint32_t mark{};
    bool acted{};
    bool typed{};
    bool finished{};
    std::vector<std::string> problems;
};

FeaturesRun g_feat;

/// The account the replay confirms.
const char* account_name() {
    return g_feat.role == FeaturesCheckRole::beta ? "oa_beta" : "oa_player";
}

bool beta_account() {
    return g_feat.role == FeaturesCheckRole::beta;
}

const char* step_name(FeatStep step) {
    switch (step) {
    case FeatStep::main_menu:
        return "main menu";
    case FeatStep::register_click:
        return "register";
    case FeatStep::registration:
        return "registration page";
    case FeatStep::form_user:
        return "registration name";
    case FeatStep::form_pass:
        return "registration password";
    case FeatStep::form_confirm:
        return "registration confirm";
    case FeatStep::form_email:
        return "registration email";
    case FeatStep::form_rules:
        return "registration rules";
    case FeatStep::form_send:
        return "registration send";
    case FeatStep::form_box:
        return "registration answer";
    case FeatStep::registration_back:
        return "registration back";
    case FeatStep::login:
        return "login";
    case FeatStep::map:
        return "galactic map";
    case FeatStep::locator_open:
        return "locator";
    case FeatStep::locate_player:
        return "locator player";
    case FeatStep::locate_planet:
        return "locator planet";
    case FeatStep::locate_battle:
        return "locator battle";
    case FeatStep::locate_frequency:
        return "locator frequency";
    case FeatStep::locate_server:
        return "locator server";
    case FeatStep::quickplay:
        return "quickplay";
    case FeatStep::quickplay_cancel:
        return "quickplay cancel";
    case FeatStep::close_console:
        return "close console";
    case FeatStep::pick_planet:
        return "planet";
    case FeatStep::battle_panel:
        return "matchmaking panel";
    case FeatStep::chat_open:
        return "matchmaking";
    case FeatStep::chat_activate:
        return "matchmaking queue";
    case FeatStep::chat_deactivate:
        return "matchmaking leave";
    case FeatStep::player_battle:
        return "battle list";
    case FeatStep::player_chat:
        return "chat";
    case FeatStep::done:
        return "done";
    }
    return "features";
}

int step_exit(FeatStep step) {
    switch (step) {
    case FeatStep::main_menu:
        return feat_exit::no_main_menu;
    case FeatStep::register_click:
    case FeatStep::registration:
    case FeatStep::form_user:
    case FeatStep::form_pass:
    case FeatStep::form_confirm:
    case FeatStep::form_email:
    case FeatStep::form_rules:
    case FeatStep::form_send:
    case FeatStep::form_box:
    case FeatStep::registration_back:
        return feat_exit::no_registration;
    case FeatStep::login:
        return feat_exit::no_login;
    case FeatStep::map:
    case FeatStep::close_console:
    case FeatStep::pick_planet:
        return feat_exit::no_map;
    case FeatStep::locator_open:
    case FeatStep::locate_player:
    case FeatStep::locate_planet:
    case FeatStep::locate_battle:
    case FeatStep::locate_frequency:
    case FeatStep::locate_server:
        return feat_exit::no_locator;
    case FeatStep::quickplay:
    case FeatStep::quickplay_cancel:
        return feat_exit::no_quickplay;
    case FeatStep::battle_panel:
    case FeatStep::chat_open:
    case FeatStep::chat_activate:
    case FeatStep::chat_deactivate:
    case FeatStep::player_battle:
    case FeatStep::player_chat:
        return feat_exit::no_matchmaking;
    case FeatStep::done:
        return feat_exit::passed;
    }
    return feat_exit::wrong;
}

void feat_report(const std::string& line) {
    std::printf("boneyards features check: %s\n", line.c_str());
    std::fflush(stdout);
}

void feat_pass(const char* what) {
    feat_report(std::string("passed ") + what);
}

/// Remembers one problem. The same wording is kept once.
void note(bool holds, const std::string& what) {
    if (holds)
        return;
    for (const std::string& problem : g_feat.problems)
        if (problem == what)
            return;
    feat_report("wrong: " + what);
    g_feat.problems.push_back(what);
}

void feat_finish(ScreenContext* ctx, int code, const std::string& why) {
    if (g_feat.finished)
        return;
    g_feat.finished = true;
    feat_report(code == feat_exit::passed ? std::string("passed") : "failed: " + why);
    if (ctx->services->quit != nullptr)
        ctx->services->quit(ctx->host, nullptr, code);
}

void advance_feat(FeatStep step) {
    g_feat.step = step;
    g_feat.frames = 0;
    g_feat.login_frames = 0;
    g_feat.mark = 0;
    g_feat.acted = false;
    g_feat.typed = false;
    feat_report(std::string("stage ") + step_name(step));
}

bool due() {
    return g_feat.frames % kClickGap == 1;
}

bool shown(const by::Window* window) {
    return window != nullptr && by::window_is_shown(window);
}

void click_window(const by::Window* window) {
    if (window == nullptr || window->width <= 0 || window->height <= 0)
        return;
    click_canvas(
        static_cast<float>(window->x + window->width / 2),
        static_cast<float>(window->y + window->height / 2)
    );
}

by_console::MainFrame* feat_frame() {
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    return widget != nullptr ? widget->system.main_frame : nullptr;
}

by::Window* feat_screen() {
    by_console::MainFrame* frame = feat_frame();
    return frame != nullptr ? frame->active_screen : nullptr;
}

by::Window* registration_window() {
    by::Window* screen = feat_screen();
    return screen != nullptr ? by::window_find_named(screen, "Registration", nullptr) : nullptr;
}

by_console::RegistrationView* registration_view() {
    return by_console::registration_from_window(registration_window());
}

by::Window* form_window() {
    by_console::RegistrationView* view = registration_view();
    by::Window* form = view != nullptr ? by_console::registration_form_window(view) : nullptr;
    return shown(form) ? form : nullptr;
}

by::Window* child_named(by::Window* parent, const char* name) {
    return parent != nullptr ? by::window_find_named(parent, name, nullptr) : nullptr;
}

bool edit_text_is(const by::Window* field, const char* text) {
    if (field == nullptr || field->ops == nullptr || field->ops->is_class == nullptr ||
        !field->ops->is_class(by::class_name::edit_box))
        return false;
    const char* got = reinterpret_cast<const by::EditBox*>(field)->text;
    return got != nullptr && text != nullptr && std::strcmp(got, text) == 0;
}

bool single_text_is(const by::Window* field, const char* text) {
    if (field == nullptr || field->ops == nullptr || field->ops->is_class == nullptr ||
        !field->ops->is_class(by::class_name::single_edit))
        return false;
    const char* got = by::single_edit_text(reinterpret_cast<const by::SingleEdit*>(field));
    return got != nullptr && text != nullptr && std::strcmp(got, text) == 0;
}

/// Clicks an edit until it has the keyboard, then replaces its text.
void type_into(by::Window* field, const char* text) {
    if (field == nullptr)
        return;
    if (field->focused == 0) {
        g_feat.typed = false;
        if (due())
            click_window(field);
        return;
    }
    if (g_feat.typed) {
        if (g_feat.frames > g_feat.mark + 8)
            g_feat.typed = false;
        return;
    }
    for (uint32_t i = 0; i < kNameClearKeys; ++i)
        press_key(SDLK_BACKSPACE);
    type_text(text);
    g_feat.typed = true;
    g_feat.mark = g_feat.frames;
}

std::string message_text(by_console::StatusWidget* widget) {
    if (widget == nullptr)
        return {};
    by::MessageBox* box = by::message_box_find_open(&widget->desktop);
    if (box == nullptr)
        return {};
    const by::Window* label =
        by::window_find_named(&box->base, by::message_box_name::text, nullptr);
    by_console::ScopedServices scope(widget);
    if (label == nullptr || label->ops == nullptr || label->ops->state_text == nullptr)
        return {};
    const char* caption = label->ops->state_text(const_cast<by::Window*>(label), -1);
    return caption != nullptr ? caption : std::string();
}

bool console_up(int32_t screen) {
    const by_console::MainFrameProbe probe = probe_widget();
    return probe.console_screen == screen && probe.screen_shown && !probe.layer_sliding;
}

bool button_enabled(const by::Window* button) {
    return button != nullptr && button->states.current != by::button_state::disabled;
}

void press_probe_button(by_console::ConsoleButton which) {
    const by_console::MainFrameProbe probe = probe_widget();
    if (probe.layer_sliding || !due())
        return;
    by_console::MainFrame* frame = feat_frame();
    const by::Window* button =
        frame != nullptr ? frame->console_buttons[static_cast<int32_t>(which)] : nullptr;
    if (which == by_console::ConsoleButton::battle &&
        probe.battle_button == by::button_state::disabled)
        return;
    if (which == by_console::ConsoleButton::chat && probe.chat_button == by::button_state::disabled)
        return;
    if (which == by_console::ConsoleButton::release &&
        probe.release_button == by::button_state::disabled)
        return;
    if (!button_enabled(button) && which != by_console::ConsoleButton::battle &&
        which != by_console::ConsoleButton::chat && which != by_console::ConsoleButton::release)
        return;
    const by_console::ScreenRect& rect = probe.console_buttons[static_cast<int32_t>(which)];
    if (rect.width != 0)
        click_rect(rect);
}

void tick_form_field(const char* control, const char* text, FeatStep next) {
    by::Window* field = child_named(form_window(), control);
    if (field == nullptr)
        return;
    if (edit_text_is(field, text)) {
        advance_feat(next);
        return;
    }
    type_into(field, text);
}

bool row_named(const scr::Locator* locator, const char* name) {
    const int32_t count = scr::locator_result_count(locator);
    for (int32_t i = 0; i < count; ++i) {
        const scr::LocatorResult* row = scr::locator_result(locator, i);
        if (row != nullptr && std::strcmp(row->name, name) == 0)
            return true;
    }
    return false;
}

bool player_ready(const scr::Locator* locator) {
    const scr::LocatorResult* row = scr::locator_result(locator, 0);
    return row != nullptr && std::strcmp(row->status, "chatting") == 0 &&
           std::strstr(row->where, "Slag") != nullptr && std::strstr(row->where, "Main") != nullptr;
}

bool planet_ready(const scr::Locator* locator) {
    return row_named(locator, "Slag");
}

bool battle_ready(const scr::Locator* locator) {
    return !scr::locator_listening_battles(locator) && row_named(locator, "Rumble");
}

bool frequency_ready(const scr::Locator* locator) {
    return !scr::locator_listening_rooms(locator) && row_named(locator, "Tactics");
}

/// Clicks one Locator search and waits until its row is listed.
void tick_query(
    int32_t kind,
    const char* query,
    bool (*ready)(const scr::Locator*),
    const char* passed,
    FeatStep next
) {
    if (!console_up(by_console::console_screen::locator)) {
        press_probe_button(by_console::ConsoleButton::locator);
        return;
    }
    by::Window* console = feat_screen();
    scr::Locator* model = by_console::locator_console_state(console);
    if (model == nullptr)
        return;
    if (scr::locator_kind_of(model) != kind) {
        g_feat.acted = false;
        g_feat.typed = false;
        char kind_name[16];
        std::snprintf(kind_name, sizeof kind_name, "kind_%d", kind);
        if (due())
            click_window(child_named(console, kind_name));
        return;
    }
    by::Window* name = child_named(console, "Name");
    if (!edit_text_is(name, query)) {
        g_feat.acted = false;
        type_into(name, query);
        return;
    }
    if (!g_feat.acted) {
        if (due()) {
            click_window(child_named(console, "Search"));
            g_feat.acted = true;
            g_feat.mark = g_feat.frames;
        }
        return;
    }
    if (ready(model)) {
        feat_pass(passed);
        advance_feat(next);
        return;
    }
    const bool listening =
        scr::locator_listening_rooms(model) || scr::locator_listening_battles(model);
    if (!listening && g_feat.frames > g_feat.mark + kSearchRetryFrames)
        g_feat.acted = false;
}

void tick_server() {
    if (!console_up(by_console::console_screen::locator)) {
        press_probe_button(by_console::ConsoleButton::locator);
        return;
    }
    by::Window* console = feat_screen();
    scr::Locator* model = by_console::locator_console_state(console);
    if (model == nullptr)
        return;
    if (scr::locator_kind_of(model) != scr::locator_kind::server) {
        if (due())
            click_window(child_named(console, "kind_4"));
        return;
    }
    const int32_t count = scr::locator_result_count(model);
    if (count <= 0) {
        if (g_feat.frames > kClickGap) {
            note(false, "the server list is empty");
            advance_feat(FeatStep::quickplay);
        }
        return;
    }
    int32_t current = -1;
    for (int32_t i = 0; i < count; ++i) {
        const scr::LocatorResult* row = scr::locator_result(model, i);
        if (row != nullptr && row->current_server)
            current = i;
    }
    if (current < 0) {
        if (g_feat.frames > kClickGap) {
            note(false, "no server is marked as the one you are on");
            advance_feat(FeatStep::quickplay);
        }
        return;
    }
    if (!g_feat.acted) {
        char hit[16];
        std::snprintf(hit, sizeof hit, "rhit%d", current);
        by::Window* row = child_named(console, hit);
        if (row != nullptr && due()) {
            click_window(row);
            g_feat.acted = true;
            g_feat.mark = g_feat.frames;
        }
        return;
    }
    if (g_feat.frames <= g_feat.mark)
        return;
    note(!scr::locator_relocate_enabled(model), "Relocate is offered on the server you are on");
    const char* detail = scr::locator_detail(model);
    note(
        detail != nullptr && std::strstr(detail, "logs you out") != nullptr,
        "the server row does not say relocating logs you out"
    );
    feat_pass("locator server");
    advance_feat(FeatStep::quickplay);
}

void tick_registration_page() {
    by::Window* page = registration_window();
    by_console::RegistrationView* view = registration_view();
    if (!shown(page) || view == nullptr)
        return;
    const scr::RegistrationConsole* model = by_console::registration_model(view);
    if (!g_feat.acted) {
        note(
            model != nullptr && std::strcmp(model->address, scr::kRegistrationAddress) == 0,
            "the registration page names another address"
        );
        note(
            shown(child_named(page, "Qr")) || shown(child_named(page, "Url")),
            "the registration page shows no address"
        );
        const bool game = shown(child_named(page, "GameHot"));
        if (beta_account())
            note(game, "the in-game route is not offered");
        else {
            note(!game, "an ordinary account is offered the in-game route");
            note(
                by_console::registration_form_window(view) == nullptr,
                "an ordinary account is shown the registration form"
            );
        }
        g_feat.acted = true;
        feat_pass("registration page");
        if (!beta_account() || !game) {
            advance_feat(FeatStep::registration_back);
            return;
        }
    }
    if (form_window() != nullptr) {
        advance_feat(FeatStep::form_user);
        return;
    }
    if (due())
        click_window(child_named(page, "GameHot"));
}

void tick_form_rules() {
    by_console::RegistrationView* view = registration_view();
    scr::RegistrationForm* form = view != nullptr ? by_console::registration_form(view) : nullptr;
    if (form != nullptr && form->rules != 0) {
        advance_feat(FeatStep::form_send);
        return;
    }
    if (g_feat.acted && g_feat.frames > g_feat.mark + kClickSettleFrames)
        g_feat.acted = false;
    if (!g_feat.acted && due()) {
        click_window(child_named(form_window(), "RulesHot"));
        g_feat.acted = true;
        g_feat.mark = g_feat.frames;
    }
}

void tick_form_send(ScreenContext* ctx) {
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    by_console::RegistrationView* view = registration_view();
    scr::RegistrationForm* form = view != nullptr ? by_console::registration_form(view) : nullptr;
    if (by_console::boneyards_status_widget_message_open(widget) ||
        (form != nullptr && form->reply != scr::registration_reply::none)) {
        advance_feat(FeatStep::form_box);
        return;
    }
    if (!g_feat.acted) {
        if (due()) {
            click_window(child_named(form_window(), "Register"));
            g_feat.acted = true;
            g_feat.mark = g_feat.frames;
        }
        return;
    }
    if (g_feat.frames <= g_feat.mark + 10 || form == nullptr)
        return;
    if (form->check != scr::registration_check::ok &&
        form->reply == scr::registration_reply::none) {
        const char* status = form->status[0][0] != '\0' ? form->status[0] : "the form was not sent";
        feat_finish(ctx, feat_exit::wrong, status);
        return;
    }
    g_feat.acted = false;
}

void tick_form_box() {
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    if (!by_console::boneyards_status_widget_message_open(widget)) {
        if (!g_feat.acted)
            return;
        feat_pass("registration form");
        advance_feat(FeatStep::registration_back);
        return;
    }
    if (!g_feat.typed) {
        const std::string text = message_text(widget);
        note(
            text.find("does not take") != std::string::npos,
            "the form's answer does not say this server does not take it"
        );
        g_feat.typed = true;
    }
    int32_t x = 0;
    int32_t y = 0;
    if (due() && by_console::boneyards_status_widget_message_button(
                     widget, by::message_box_name::ok, &x, &y
                 )) {
        click_canvas(static_cast<float>(x), static_cast<float>(y));
        g_feat.acted = true;
    }
}

/// The login console's Username entry: the upper single-line field that shows
/// what was typed. The password field draws a mask instead.
by::Window* login_name_field() {
    by::Window* screen = feat_screen();
    if (screen == nullptr)
        return nullptr;
    by::Window* found = nullptr;
    for (int32_t i = 0; i < screen->children.count; ++i) {
        by::Window* child = screen->children.items[i];
        if (child == nullptr || child->ops == nullptr || child->ops->is_class == nullptr ||
            !child->ops->is_class(by::class_name::single_edit))
            continue;
        if (reinterpret_cast<const by::SingleEdit*>(child)->mask != 0)
            continue;
        if (found == nullptr || child->y < found->y)
            found = child;
    }
    return found;
}

/// The keyboard is on this control when the desktop's focus names it. A
/// control can show text that was saved earlier while the keyboard sits on
/// the password entry instead.
bool login_keyboard_on(const by::Window* field) {
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    return widget != nullptr && field != nullptr && widget->desktop.focus == field;
}

/// A function button of the login console, found by the words it shows.
/// Reading those words needs the console's text service.
by::Window* login_button(const char* caption) {
    by::Window* screen = feat_screen();
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    if (screen == nullptr || caption == nullptr || widget == nullptr)
        return nullptr;
    by_console::ScopedServices scope(widget);
    for (int32_t i = 0; i < screen->children.count; ++i) {
        by::Window* child = screen->children.items[i];
        if (child == nullptr || child->ops == nullptr || child->ops->state_text == nullptr)
            continue;
        const char* text = child->ops->state_text(child, -1);
        if (text != nullptr && std::strcmp(text, caption) == 0)
            return child;
    }
    return nullptr;
}

/// Guest Login is the selected account kind. The console opens on Registered
/// User, and LOGIN stays off until a password is typed or the check selects
/// Guest Login on the dialog.
bool guest_login_selected() {
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    return widget != nullptr && widget->navigation != nullptr &&
           widget->navigation->login.guest_value == by::button_state::pressed;
}

/// Selects Guest Login on the dialog. The radio is drawn disabled, so a
/// player cannot choose it. The check reaches the model directly.
void choose_guest_login() {
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    if (widget == nullptr || widget->navigation == nullptr)
        return;
    by_screens::login_on_control(&widget->navigation->login, by_screens::LoginControl::guest_radio);
}

/// Replaces the Username entry. Keystrokes land on whichever control has the
/// keyboard, so this waits until that is the name entry.
void type_account_name(by::Window* field) {
    if (!login_keyboard_on(field)) {
        g_feat.typed = false;
        if (due())
            click_window(field);
        return;
    }
    if (g_feat.typed) {
        if (g_feat.frames > g_feat.mark + 8)
            g_feat.typed = false;
        return;
    }
    for (uint32_t i = 0; i < kNameClearKeys; ++i)
        press_key(SDLK_BACKSPACE);
    type_text(account_name());
    g_feat.typed = true;
    g_feat.mark = g_feat.frames;
}

void tick_login() {
    const by_console::MainFrameProbe probe = probe_widget();
    if (probe.connection_level >= kLoggedInLevel) {
        feat_report(std::string("logged in as ") + account_name());
        advance_feat(FeatStep::map);
        return;
    }
    // A sliding console is not where its fields will be clicked. Registration
    // closing also hides the login console for a pass; wait until it is back.
    if (probe.console_screen != by_console::console_screen::login || !probe.screen_shown ||
        probe.layer_sliding) {
        g_feat.login_frames = 0;
        g_feat.typed = false;
        g_feat.acted = false;
        return;
    }
    by::Window* field = login_name_field();
    if (field == nullptr)
        return;
    ++g_feat.login_frames;
    // The login opens on Registered User, so LOGIN stays off until a password
    // is typed. Guest Login is drawn disabled. The check selects it on the
    // dialog, which a player cannot do, then presses LOGIN.
    if (!single_text_is(field, account_name())) {
        g_feat.acted = false;
        type_account_name(field);
        return;
    }
    if (!guest_login_selected()) {
        g_feat.acted = false;
        if (due())
            choose_guest_login();
        return;
    }
    by::Window* login = login_button("LOGIN");
    if (!button_enabled(login))
        return;
    if (!g_feat.acted) {
        if (due()) {
            click_window(login);
            g_feat.acted = true;
            g_feat.mark = g_feat.frames;
        }
        return;
    }
    if (g_feat.frames > g_feat.mark + kLoginRetypeFrames)
        g_feat.acted = false;
}

void tick_map() {
    const by_console::MainFrameProbe probe = probe_widget();
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    if (probe.connection_level < kDossierLevel || probe.layer_sliding || probe.layer_covering ||
        probe.galaxy_loading || probe.planet_count == 0 ||
        probe.console_screen != by_console::console_screen::none)
        return;
    note(
        widget != nullptr && widget->system.matchmaking_enabled == (beta_account() ? 1 : 0),
        beta_account() ? "a Beta Tester's session does not offer Matchmaking"
                       : "an ordinary account's session offers Matchmaking"
    );
    note(
        widget != nullptr && std::strcmp(widget->system.user_name, account_name()) == 0,
        "the login did not take the account"
    );
    feat_pass("galactic map");
    advance_feat(FeatStep::locator_open);
}

void tick_quickplay_button() {
    if (console_up(by_console::console_screen::quickplay))
        return;
    by_console::MainFrame* frame = feat_frame();
    by::Window* button = frame != nullptr ? frame->quickplay_button : nullptr;
    if (button_enabled(button) && !probe_widget().layer_sliding && due())
        click_window(button);
}

void tick_quickplay() {
    if (!console_up(by_console::console_screen::quickplay)) {
        tick_quickplay_button();
        return;
    }
    scr::QuickPlay* model = by_console::quickplay_console_model(feat_screen());
    if (model != nullptr && model->searching) {
        feat_pass("quickplay");
        advance_feat(FeatStep::quickplay_cancel);
        return;
    }
    by::Window* find = child_named(feat_screen(), "FIND BATTLE");
    if (shown(find) && due())
        click_window(find);
}

void tick_quickplay_cancel() {
    scr::QuickPlay* model = by_console::quickplay_console_model(feat_screen());
    if (g_feat.acted && model != nullptr && !model->searching) {
        advance_feat(FeatStep::close_console);
        return;
    }
    by::Window* cancel = child_named(feat_screen(), "CANCEL SEARCH");
    if (shown(cancel) && due()) {
        click_window(cancel);
        g_feat.acted = true;
        g_feat.mark = g_feat.frames;
    }
}

void tick_close_console() {
    const by_console::MainFrameProbe probe = probe_widget();
    if (probe.console_screen == by_console::console_screen::none && !probe.layer_sliding &&
        !probe.layer_covering) {
        advance_feat(FeatStep::pick_planet);
        return;
    }
    if (probe.console_screen != by_console::console_screen::none && !probe.layer_sliding)
        press_probe_button(by_console::ConsoleButton::release);
}

void tick_pick_planet() {
    const by_console::MainFrameProbe probe = probe_widget();
    if (probe.layer_sliding || probe.layer_covering || probe.galaxy_loading)
        return;
    if (probe.selected_area_id == kLaunchCheckArea && !probe.activity_light_running &&
        probe.battle_button == by::button_state::unpressed) {
        advance_feat(beta_account() ? FeatStep::battle_panel : FeatStep::player_battle);
        return;
    }
    int32_t x = 0;
    int32_t y = 0;
    if (due() &&
        by_console::boneyards_status_widget_planet_hotspot(
            boneyards_console_status_widget(), static_cast<int32_t>(kLaunchCheckArea), &x, &y
        ))
        click_canvas(static_cast<float>(x), static_cast<float>(y));
}

by_console::MatchControlPanel* match_panel() {
    by::Window* battle = feat_screen();
    if (battle == nullptr || !console_up(by_console::console_screen::battle))
        return nullptr;
    by::Window* panel = by::window_find_named(battle, "MatchControlPanel", nullptr);
    return panel != nullptr ? reinterpret_cast<by_console::MatchControlPanel*>(panel) : nullptr;
}

by_console::ChatView* chat_view() {
    by::Window* chat = feat_screen();
    if (chat == nullptr || !console_up(by_console::console_screen::chat))
        return nullptr;
    return reinterpret_cast<by_console::ChatView*>(chat);
}

void open_battle() {
    if (!console_up(by_console::console_screen::battle))
        press_probe_button(by_console::ConsoleButton::battle);
}

void open_chat() {
    if (!console_up(by_console::console_screen::chat))
        press_probe_button(by_console::ConsoleButton::chat);
}

void tick_battle_panel() {
    open_battle();
    by_console::MatchControlPanel* panel = match_panel();
    if (!console_up(by_console::console_screen::battle) || probe_widget().activity_light_running)
        return;
    if (panel == nullptr) {
        note(false, "the battle list has no matchmaking panel");
        advance_feat(FeatStep::chat_open);
        return;
    }
    if (shown(panel->activate)) {
        feat_pass("matchmaking panel");
        advance_feat(FeatStep::chat_open);
        return;
    }
    if (due())
        click_window(panel->minimize);
}

void tick_chat_open() {
    open_chat();
    by_console::ChatView* chat = chat_view();
    if (chat == nullptr)
        return;
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    by::Window* dialog = by_console::matchmaking_dialog_find(widget);
    if (shown(dialog) &&
        by_console::matchmaking_dialog_searching(dialog) == by_console::k_matchmaking_settings &&
        !by_console::matchmaking_dialog_queued(dialog)) {
        advance_feat(FeatStep::chat_activate);
        return;
    }
    if (chat->matchmaking == nullptr) {
        note(false, "Chat has no Matchmaking button");
        advance_feat(FeatStep::done);
        return;
    }
    if (due())
        click_window(chat->matchmaking);
}

void tick_chat_activate() {
    by::Window* dialog = by_console::matchmaking_dialog_find(boneyards_console_status_widget());
    if (shown(dialog) &&
        by_console::matchmaking_dialog_searching(dialog) == by_console::k_matchmaking_searching &&
        by_console::matchmaking_dialog_queued(dialog)) {
        feat_pass("matchmaking");
        advance_feat(FeatStep::chat_deactivate);
        return;
    }
    if (due())
        click_window(child_named(dialog, "Activate"));
}

void tick_chat_deactivate() {
    by::Window* dialog = by_console::matchmaking_dialog_find(boneyards_console_status_widget());
    if (shown(dialog) &&
        by_console::matchmaking_dialog_searching(dialog) == by_console::k_matchmaking_settings &&
        !by_console::matchmaking_dialog_queued(dialog) && g_feat.acted) {
        advance_feat(FeatStep::done);
        return;
    }
    if (due()) {
        click_window(child_named(dialog, "Deactivate"));
        g_feat.acted = true;
    }
}

void tick_player_battle() {
    open_battle();
    if (!console_up(by_console::console_screen::battle) || probe_widget().activity_light_running)
        return;
    note(match_panel() == nullptr, "an ordinary account is offered Matchmaking");
    note(
        by_console::matchmaking_dialog_find(boneyards_console_status_widget()) == nullptr,
        "an ordinary account opened Matchmaking"
    );
    feat_pass("battle list");
    advance_feat(FeatStep::player_chat);
}

void tick_player_chat() {
    open_chat();
    by_console::ChatView* chat = chat_view();
    if (chat == nullptr)
        return;
    note(chat->matchmaking == nullptr, "an ordinary account is offered Matchmaking from Chat");
    note(
        by_console::matchmaking_dialog_find(boneyards_console_status_widget()) == nullptr,
        "an ordinary account opened Matchmaking"
    );
    feat_pass("chat");
    advance_feat(FeatStep::done);
}

/// Plays one step of --check-boneyards-features. A step that runs past its frame limit fails with
/// that step's code. Problems found on a screen that is still usable are kept and fail the run at
/// the end.
void tick_features(ScreenContext* ctx, void*) {
    if (g_feat.finished)
        return;
    if (g_check.reset_modifiers) {
        SDL_SetModState(SDL_KMOD_NONE);
        g_check.reset_modifiers = false;
    }
    ++g_feat.frames;
    if (g_feat.frames % kReportFrames == 0)
        feat_report(std::string("waiting in ") + step_name(g_feat.step));
    if (g_feat.frames > g_feat.limit) {
        feat_finish(
            ctx, step_exit(g_feat.step), std::string("timed out in ") + step_name(g_feat.step)
        );
        return;
    }
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    switch (g_feat.step) {
    case FeatStep::main_menu:
        if (ctx->screen == screen_id(Screen::main_menu) && widget != nullptr &&
            by_console::boneyards_status_widget_visible(widget) && due())
            press_key(SDLK_B);
        if (by_console::boneyards_status_widget_in_boneyards(widget))
            advance_feat(FeatStep::register_click);
        return;
    case FeatStep::register_click: {
        const by_console::MainFrameProbe probe = probe_widget();
        if (probe.console_screen == by_console::console_screen::login && probe.screen_shown &&
            due())
            click_canvas(static_cast<float>(kRegisterClickX), static_cast<float>(kRegisterClickY));
        if (shown(registration_window()))
            advance_feat(FeatStep::registration);
        return;
    }
    case FeatStep::registration:
        tick_registration_page();
        return;
    case FeatStep::form_user:
        tick_form_field("User", "oaenlist", FeatStep::form_pass);
        return;
    case FeatStep::form_pass:
        tick_form_field("Pass", "yardpass", FeatStep::form_confirm);
        return;
    case FeatStep::form_confirm:
        tick_form_field("Confirm", "yardpass", FeatStep::form_email);
        return;
    case FeatStep::form_email:
        tick_form_field("Email", "oa@example.test", FeatStep::form_rules);
        return;
    case FeatStep::form_rules:
        tick_form_rules();
        return;
    case FeatStep::form_send:
        tick_form_send(ctx);
        return;
    case FeatStep::form_box:
        tick_form_box();
        return;
    case FeatStep::registration_back: {
        by::Window* page = registration_window();
        if (!shown(page)) {
            advance_feat(FeatStep::login);
            return;
        }
        if (due())
            click_window(child_named(page, "Back"));
        return;
    }
    case FeatStep::login:
        tick_login();
        return;
    case FeatStep::map:
        tick_map();
        return;
    case FeatStep::locator_open:
        if (console_up(by_console::console_screen::locator)) {
            feat_pass("locator");
            advance_feat(FeatStep::locate_player);
            return;
        }
        press_probe_button(by_console::ConsoleButton::locator);
        return;
    case FeatStep::locate_player:
        tick_query(
            scr::locator_kind::player,
            "Ghost_Commander",
            player_ready,
            "locator player",
            FeatStep::locate_planet
        );
        return;
    case FeatStep::locate_planet:
        tick_query(
            scr::locator_kind::planet,
            "Slag",
            planet_ready,
            "locator planet",
            FeatStep::locate_battle
        );
        return;
    case FeatStep::locate_battle:
        tick_query(
            scr::locator_kind::battle,
            "Rumble",
            battle_ready,
            "locator battle",
            FeatStep::locate_frequency
        );
        return;
    case FeatStep::locate_frequency:
        tick_query(
            scr::locator_kind::frequency,
            "Tactics",
            frequency_ready,
            "locator frequency",
            FeatStep::locate_server
        );
        return;
    case FeatStep::locate_server:
        tick_server();
        return;
    case FeatStep::quickplay:
        tick_quickplay();
        return;
    case FeatStep::quickplay_cancel:
        tick_quickplay_cancel();
        return;
    case FeatStep::close_console:
        tick_close_console();
        return;
    case FeatStep::pick_planet:
        tick_pick_planet();
        return;
    case FeatStep::battle_panel:
        tick_battle_panel();
        return;
    case FeatStep::chat_open:
        tick_chat_open();
        return;
    case FeatStep::chat_activate:
        tick_chat_activate();
        return;
    case FeatStep::chat_deactivate:
        tick_chat_deactivate();
        return;
    case FeatStep::player_battle:
        tick_player_battle();
        return;
    case FeatStep::player_chat:
        tick_player_chat();
        return;
    case FeatStep::done:
        feat_finish(
            ctx,
            g_feat.problems.empty() ? feat_exit::passed : feat_exit::wrong,
            std::to_string(g_feat.problems.size()) + " thing(s) wrong"
        );
        return;
    }
}

// ---- the status strip on the presented frame --------------------------------

constexpr int32_t kCanvasWidth = 640;
constexpr int32_t kCanvasHeight = 480;
constexpr int kOverlayFrameBudget = 600;

struct OverlayRun {
    bool armed = false;
    bool finished = false;
    bool reported = false;
    bool pending = false;
    int pending_code = 0;
    OverlayCheckTier tier = OverlayCheckTier::basic;
    const ScreenServices* services = nullptr;
    void* host = nullptr;
    bool have_pixel = false;
    int32_t pixel_x = 0;
    int32_t pixel_y = 0;
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    int frames = 0;
    char renderer[64] = {};
    std::vector<uint8_t> pass_a;
    std::vector<uint8_t> pass_b;
};

OverlayRun g_overlay;

const char* overlay_tier_name() {
    return g_overlay.tier == OverlayCheckTier::basic ? "basic" : "full";
}

void overlay_finish(int code, const char* why) {
    if (g_overlay.finished)
        return;
    if (!g_overlay.reported) {
        if (code == overlay_check_exit::passed)
            std::printf(
                "boneyards overlay check: %s: the strip is on the presented frame\n"
                "boneyards overlay check: renderer %s\n",
                overlay_tier_name(),
                g_overlay.renderer
            );
        else
            std::printf("boneyards overlay check: failed: %s\n", why != nullptr ? why : "");
        std::fflush(stdout);
        g_overlay.reported = true;
    }
    if (g_overlay.services == nullptr || g_overlay.services->quit == nullptr) {
        g_overlay.pending = true;
        g_overlay.pending_code = code;
        return;
    }
    g_overlay.finished = true;
    g_overlay.services->quit(g_overlay.host, nullptr, code);
}

void paint_indexed(std::vector<uint8_t>& pixels, by_console::StatusWidget* widget, uint8_t fill) {
    pixels.assign(static_cast<std::size_t>(kCanvasWidth) * kCanvasHeight, fill);
    oa::Surface surface{};
    surface.width = kCanvasWidth;
    surface.height = kCanvasHeight;
    surface.pitch = kCanvasWidth;
    surface.pixels = pixels.data();
    surface.clip = oa::Rect32{0, 0, kCanvasWidth - 1, kCanvasHeight - 1};
    by_console::boneyards_status_widget_draw(widget, &surface);
}

bool strip_color(
    int32_t x, int32_t y, const oa::Palette* palette, uint8_t* red, uint8_t* green, uint8_t* blue
) {
    if (x < 0 || y < by_console::overlay_layout::y || x >= kCanvasWidth || y >= kCanvasHeight)
        return false;
    const std::size_t index =
        static_cast<std::size_t>(y) * kCanvasWidth + static_cast<std::size_t>(x);
    const uint8_t color = g_overlay.pass_a[index];
    if (color != g_overlay.pass_b[index] || color == by_console::resource::console_key)
        return false;
    const oa::PaletteEntry& entry = palette->entries[color];
    *red = entry.r;
    *green = entry.g;
    *blue = entry.b;
    return true;
}

bool composed_is(
    const oa::ui::frontend_renderer::Surface* surface,
    int32_t x,
    int32_t y,
    uint8_t red,
    uint8_t green,
    uint8_t blue
) {
    if (surface == nullptr || surface->width != static_cast<uint32_t>(kCanvasWidth) ||
        surface->height != static_cast<uint32_t>(kCanvasHeight))
        return false;
    const std::size_t at =
        (static_cast<std::size_t>(y) * surface->width + static_cast<std::size_t>(x)) * 3U;
    return at + 2 < surface->rgb.size() && surface->rgb[at] == red &&
           surface->rgb[at + 1] == green && surface->rgb[at + 2] == blue;
}

bool take_strip_pixel(
    const oa::ui::frontend_renderer::Surface* surface,
    const oa::Palette* palette,
    int32_t x,
    int32_t y
) {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    if (!strip_color(x, y, palette, &red, &green, &blue) ||
        !composed_is(surface, x, y, red, green, blue))
        return false;
    g_overlay.pixel_x = x;
    g_overlay.pixel_y = y;
    g_overlay.red = red;
    g_overlay.green = green;
    g_overlay.blue = blue;
    g_overlay.have_pixel = true;
    return true;
}

/// Finds one opaque pixel of the status strip on the composed main menu.
void draw_overlay_check(ScreenContext* ctx, void* /*state*/) {
    if (ctx == nullptr)
        return;
    g_overlay.services = ctx->services;
    g_overlay.host = ctx->host;
    if (g_overlay.finished || g_overlay.reported)
        return;
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    if (widget == nullptr || !by_console::boneyards_status_widget_visible(widget) ||
        by_console::boneyards_status_widget_in_boneyards(widget))
        return;
    const oa::Palette* palette = widget->config.palette;
    if (palette == nullptr)
        return;
    paint_indexed(g_overlay.pass_a, widget, 0x00);
    paint_indexed(g_overlay.pass_b, widget, 0xFF);
    const int32_t enter_x =
        by_console::overlay_layout::enter_x + by_console::overlay_layout::enter_width / 2;
    const int32_t enter_y =
        by_console::overlay_layout::enter_y + by_console::overlay_layout::enter_height / 2;
    if (take_strip_pixel(ctx->surface, palette, enter_x, enter_y))
        return;
    for (int32_t y = by_console::overlay_layout::y; y < kCanvasHeight; ++y) {
        for (int32_t x = 0; x < kCanvasWidth; ++x) {
            if (take_strip_pixel(ctx->surface, palette, x, y))
                return;
        }
    }
}

bool presented_block_matches(const SDL_Surface* rgb, int origin_x, int origin_y, int scale) {
    if (rgb == nullptr || rgb->pixels == nullptr || scale <= 0)
        return false;
    const int x = origin_x + g_overlay.pixel_x * scale + scale / 2;
    const int y = origin_y + g_overlay.pixel_y * scale + scale / 2;
    if (x < 0 || y < 0 || x >= rgb->w || y >= rgb->h)
        return false;
    const auto* row =
        static_cast<const uint8_t*>(rgb->pixels) + static_cast<std::ptrdiff_t>(y) * rgb->pitch;
    const uint8_t* pixel = row + static_cast<std::ptrdiff_t>(x) * 3;
    return pixel[0] == g_overlay.red && pixel[1] == g_overlay.green && pixel[2] == g_overlay.blue;
}

void present_overlay(const CheckHost& host, const Options& options) {
    if (!g_overlay.armed || g_overlay.finished)
        return;
    if (g_overlay.reported) {
        if (g_overlay.pending && g_overlay.services != nullptr &&
            g_overlay.services->quit != nullptr) {
            g_overlay.finished = true;
            g_overlay.services->quit(g_overlay.host, nullptr, g_overlay.pending_code);
        }
        return;
    }
    ++g_overlay.frames;
    const uint32_t window_id = host.window_id != nullptr ? host.window_id(host.context) : 0;
    SDL_Window* window = window_id != 0 ? SDL_GetWindowFromID(window_id) : nullptr;
    SDL_Renderer* renderer = window != nullptr ? SDL_GetRenderer(window) : nullptr;
    const char* renderer_name = renderer != nullptr ? SDL_GetRendererName(renderer) : nullptr;
    if (renderer_name != nullptr && renderer_name[0] != '\0')
        std::snprintf(g_overlay.renderer, sizeof g_overlay.renderer, "%s", renderer_name);
    using Accel = oa::ui::engine_settings::HardwareAcceleration;
    const Accel asked_tier = g_overlay.tier == OverlayCheckTier::basic ? Accel::basic : Accel::full;
    if (!options.hardware_acceleration || *options.hardware_acceleration != asked_tier) {
        overlay_finish(
            overlay_check_exit::wrong_tier, "the run was not asked for this graphics tier"
        );
        return;
    }
    if (window == nullptr || renderer == nullptr || !g_overlay.have_pixel) {
        if (g_overlay.frames <= kOverlayFrameBudget)
            return;
        overlay_finish(
            window == nullptr || renderer == nullptr ? overlay_check_exit::no_window
                                                     : overlay_check_exit::no_strip,
            window == nullptr || renderer == nullptr
                ? "the main menu's window never opened"
                : "the status strip is not on the composed main menu"
        );
        return;
    }
    SDL_FRect letterbox{};
    if (!SDL_GetRenderLogicalPresentationRect(renderer, &letterbox) || !(letterbox.w > 0.0F) ||
        !(letterbox.h > 0.0F)) {
        if (g_overlay.frames <= kOverlayFrameBudget)
            return;
        overlay_finish(overlay_check_exit::bad_scale, "the presented frame has no letterbox");
        return;
    }
    const double width_scale = static_cast<double>(letterbox.w) / kCanvasWidth;
    const double height_scale = static_cast<double>(letterbox.h) / kCanvasHeight;
    const int scale = static_cast<int>(std::lround(width_scale));
    if (scale <= 0 || std::abs(width_scale - scale) > 0.01 ||
        std::abs(height_scale - scale) > 0.01) {
        char why[160];
        std::snprintf(
            why,
            sizeof why,
            "the presented scale is not a whole number (%.2f by %.2f)",
            width_scale,
            height_scale
        );
        overlay_finish(overlay_check_exit::bad_scale, why);
        return;
    }
    SDL_Surface* read = SDL_RenderReadPixels(renderer, nullptr);
    SDL_Surface* rgb = read != nullptr ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGB24) : nullptr;
    SDL_DestroySurface(read);
    if (rgb == nullptr) {
        if (g_overlay.frames <= kOverlayFrameBudget)
            return;
        overlay_finish(
            overlay_check_exit::presented_differs, "the presented frame could not be read"
        );
        return;
    }
    const int box_w = static_cast<int>(std::lround(letterbox.w));
    const int box_h = static_cast<int>(std::lround(letterbox.h));
    const int box_x = static_cast<int>(std::lround(letterbox.x));
    const int box_y = static_cast<int>(std::lround(letterbox.y));
    bool matched = rgb->w == box_w && rgb->h == box_h
                       ? presented_block_matches(rgb, 0, 0, scale)
                       : presented_block_matches(rgb, box_x, box_y, scale);
    if (!matched)
        matched = presented_block_matches(rgb, 0, 0, scale) ||
                  presented_block_matches(rgb, box_x, box_y, scale);
    if (!matched && box_w > 0 && rgb->w == box_w * 2 && rgb->h == box_h * 2)
        matched = presented_block_matches(rgb, 0, 0, scale * 2) ||
                  presented_block_matches(rgb, box_x * 2, box_y * 2, scale * 2);
    if (!matched && g_overlay.frames <= kOverlayFrameBudget) {
        SDL_DestroySurface(rgb);
        return;
    }
    if (!matched) {
        int window_w = 0;
        int window_h = 0;
        if (window != nullptr)
            SDL_GetWindowSizeInPixels(window, &window_w, &window_h);
        char why[240];
        std::snprintf(
            why,
            sizeof why,
            "the presented pixel is not the strip (window %dx%d, letterbox %.0fx%.0f at %.0f,%.0f, "
            "read %dx%d)",
            window_w,
            window_h,
            static_cast<double>(letterbox.w),
            static_cast<double>(letterbox.h),
            static_cast<double>(letterbox.x),
            static_cast<double>(letterbox.y),
            rgb->w,
            rgb->h
        );
        SDL_DestroySurface(rgb);
        overlay_finish(overlay_check_exit::presented_differs, why);
        return;
    }
    SDL_DestroySurface(rgb);
    overlay_finish(overlay_check_exit::passed, nullptr);
}

} // namespace

void register_boneyards_launch_check(ScreenRegistry* registry, LaunchCheckRole role) {
    g_check = LaunchCheck{};
    g_check.role = role;
    g_check.stage_limit = scaled_stage_limit();
    OverlayDesc desc{};
    desc.name = "boneyards-launch-check";
    desc.screen = kScreenAny;
    desc.tick = tick_check;
    (void)overlay_register(registry, &desc);
}

void register_boneyards_features_check(ScreenRegistry* registry, FeaturesCheckRole role) {
    g_feat = FeaturesRun{};
    g_feat.role = role;
    g_feat.limit = scaled_stage_limit();
    OverlayDesc desc{};
    desc.name = "boneyards-features-check";
    desc.screen = kScreenAny;
    desc.tick = tick_features;
    (void)overlay_register(registry, &desc);
}

void register_boneyards_overlay_check(ScreenRegistry* registry, OverlayCheckTier tier) {
    g_overlay = OverlayRun{};
    g_overlay.armed = true;
    g_overlay.tier = tier;
    OverlayDesc desc{};
    desc.name = "boneyards-overlay-check";
    desc.screen = oa::ui::frontend_multiplayer::kScreenMainMenu;
    desc.z = 1;
    desc.draw = draw_overlay_check;
    (void)overlay_register(registry, &desc);
}

void present_boneyards_overlay(const CheckHost& host, const Options& options) {
    present_overlay(host, options);
}

} // namespace oa::app
