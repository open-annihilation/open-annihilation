// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend/ingame_menu.hpp"
#include "test_support.hpp"
#include "oa/base/text.hpp"
#include "oa/data/languages/translation.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace oa::ui::frontend::test {
namespace {

struct Calls {
    std::vector<std::string> sounds;
    std::vector<std::string> messages;
    bool disc = true;
    int cd_on = 0;
    int cd_off = 0;
};

IngameContext make_context(Calls& calls, SessionKind session) {
    IngameContext context;
    context.session = session;
    context.host.context = &calls;
    context.host.play_sound = [](void* c, const char* name) {
        static_cast<Calls*>(c)->sounds.emplace_back(name);
    };
    context.host.set_cd_playback = [](void* c, bool on) {
        ++(on ? static_cast<Calls*>(c)->cd_on : static_cast<Calls*>(c)->cd_off);
    };
    context.host.disc_present = [](void* c) { return static_cast<Calls*>(c)->disc; };
    context.host.show_message = [](void* c, const char* text, int32_t) {
        static_cast<Calls*>(c)->messages.emplace_back(text);
    };
    return context;
}

bool load_panel(Panel& panel, const char* name) {
    const auto layout = load_gui(name);
    if (!layout)
        return false;
    panel_load_layout(panel, *layout);
    return true;
}

OA_GAME_DATA_TEST(armopt_setup_by_session) {
    Panel panel;
    if (!load_panel(panel, "armopt.gui"))
        return;
    Calls calls;
    auto context = make_context(calls, SessionKind::multiplayer);
    ingame_enter_options(panel, context);
    OA_CHECK(panel_control(panel, "SAVEGAME")->grayed == 1);
    OA_CHECK(panel_control(panel, "LOADGAME")->grayed == 1);
    OA_CHECK(text_of(panel, "MISSION") == "Settings");
    OA_CHECK(!context.hold_game);
    OA_CHECK(calls.cd_on == 1);

    Panel campaign;
    load_panel(campaign, "armopt.gui");
    auto single = make_context(calls, SessionKind::campaign);
    ingame_enter_options(campaign, single);
    OA_CHECK(panel_control(campaign, "SAVEGAME")->grayed == 0);
    OA_CHECK(single.hold_game);

    // Game data without the save and load dialog grays both buttons.
    Panel without_dialog;
    load_panel(without_dialog, "armopt.gui");
    auto no_dialog = make_context(calls, SessionKind::campaign);
    no_dialog.saved_games_offered = false;
    ingame_enter_options(without_dialog, no_dialog);
    OA_CHECK(panel_control(without_dialog, "SAVEGAME")->grayed == 1);
    OA_CHECK(panel_control(without_dialog, "LOADGAME")->grayed == 1);
}

OA_GAME_DATA_TEST(armopt_clicks_route_to_dialogs) {
    Panel panel;
    if (!load_panel(panel, "armopt.gui"))
        return;
    Calls calls;
    auto context = make_context(calls, SessionKind::campaign);
    context.in_game = true;

    const struct {
        const char* name;
        IngameAction action;
    } cases[] = {
        {"LOADGAME", IngameAction::open_load_game},
        {"SAVEGAME", IngameAction::open_save_game},
        {"PREFS", IngameAction::open_options},
        {"HELP", IngameAction::open_help},
        {"MISSION", IngameAction::open_briefing},
        {"EXIT", IngameAction::open_exit_menu},
        {"OK", IngameAction::none},
    };

    for (const auto& item : cases) {
        select(panel, item.name);
        OA_CHECK(ingame_on_options_click(panel, context) == item.action);
    }
    context.session = SessionKind::skirmish;
    select(panel, "MISSION");
    OA_CHECK(ingame_on_options_click(panel, context) == IngameAction::open_game_settings);
    context.hold_game = true;
    panel.selected = kNoSelection;
    OA_CHECK(ingame_on_options_click(panel, context) == IngameAction::closed);
    OA_CHECK(!context.hold_game && calls.cd_off == 1);
}

OA_GAME_DATA_TEST(exit_menu_and_confirmation) {
    Panel panel;
    if (!load_panel(panel, "exitmenu.gui"))
        return;
    Calls calls;
    auto context = make_context(calls, SessionKind::skirmish);
    ingame_enter_exit_menu(panel, context);
    OA_CHECK(panel_control(panel, "RESTART")->active == 1);
    // The file gives the exit menu's buttons no quick keys; Restart takes
    // its first letter as its caption is set.
    OA_CHECK(panel_control(panel, "RESTART")->quick_key == 'R');
    OA_CHECK(panel_control(panel, "MAINMENU")->quick_key == 0);
    select(panel, "EXITGAME");
    OA_CHECK(ingame_on_exit_menu_click(panel, context) == IngameAction::open_exit_confirm);
    OA_CHECK(context.exit_kind == ExitKind::leave_game);

    Panel confirm;
    if (!load_panel(confirm, "yesorno.gui"))
        return;
    ingame_enter_exit_confirm(confirm, context);
    OA_CHECK(text_of(confirm, "TITLE") == "Surrender this battle and exit to the system?");
    // A profile's question is asked in its place, as written.
    context.leave_question = "Abandon this made-up war?";
    ingame_enter_exit_confirm(confirm, context);
    OA_CHECK(text_of(confirm, "TITLE") == "Abandon this made-up war?");
    context.leave_question.clear();
    ingame_enter_exit_confirm(confirm, context);
    OA_CHECK(text_of(confirm, "CHOICE1") == "Yes");
    OA_CHECK(panel_control(confirm, "CHOICE1")->quick_key == 'Y');
    OA_CHECK(panel_control(confirm, "CHOICE2")->quick_key == 'N');
    select(confirm, "CHOICE1");
    OA_CHECK(ingame_on_exit_confirm_click(confirm, context) == IngameAction::leave_game);
    OA_CHECK((context.quit_flags & quit_flag::leave_application) != 0);
    OA_CHECK(calls.sounds.back() == "Exit");

    context.exit_kind = ExitKind::main_menu;
    oa::base::text::copy_terminated(context.return_label, "Portal");
    ingame_enter_exit_confirm(confirm, context);
    OA_CHECK(text_of(confirm, "TITLE") == "Surrender this battle and return to Portal?");
    select(confirm, "CHOICE1");
    OA_CHECK(ingame_on_exit_confirm_click(confirm, context) == IngameAction::return_to_main_menu);
    select(confirm, "CHOICE2");
    OA_CHECK(ingame_on_exit_confirm_click(confirm, context) == IngameAction::closed);
    // Asked from the menu, Enter and Escape answer No, as CHOICE2 does.
    OA_CHECK(kExitConfirmDefault == "CHOICE2");
    OA_CHECK(ingame_exit_confirm_accept(false) == kExitConfirmDefault);
    OA_CHECK(text_of(confirm, kExitConfirmDefault) == "No");
    select(confirm, kExitConfirmDefault);
    OA_CHECK(ingame_on_exit_confirm_click(confirm, context) == IngameAction::closed);

    // Asked by a close request, Enter answers Yes and leaves the game;
    // Escape still answers No.
    context.quit_flags = 0;
    ingame_open_leave_confirm(confirm, context);
    OA_CHECK(context.exit_kind == ExitKind::leave_game);
    OA_CHECK(kCloseConfirmAccept == "CHOICE1");
    OA_CHECK(ingame_exit_confirm_accept(true) == kCloseConfirmAccept);
    OA_CHECK(text_of(confirm, ingame_exit_confirm_accept(true)) == "Yes");
    select(confirm, ingame_exit_confirm_accept(true));
    OA_CHECK(ingame_on_exit_confirm_click(confirm, context) == IngameAction::leave_game);
    OA_CHECK((context.quit_flags & quit_flag::leave_application) != 0);
    context.quit_flags = 0;
    select(confirm, kExitConfirmDefault);
    OA_CHECK(ingame_on_exit_confirm_click(confirm, context) == IngameAction::closed);
    OA_CHECK((context.quit_flags & quit_flag::leave_application) == 0);
}

OA_GAME_DATA_TEST(exit_menu_return_label_replaces_button) {
    Panel panel;
    if (!load_panel(panel, "exitmenu.gui"))
        return;
    Calls calls;
    auto context = make_context(calls, SessionKind::multiplayer);
    oa::base::text::copy_terminated(context.return_label, "Harbour");
    ingame_enter_exit_menu(panel, context);
    OA_CHECK(panel_control(panel, "EXITGAME")->active == 0);
    OA_CHECK(text_of(panel, "MAINMENU") == "Harbour");
    OA_CHECK(panel_control(panel, "MAINMENU")->quick_key == 'H');
}

OA_GAME_DATA_TEST(restart_dialog_disc_gate_and_difficulty) {
    Panel panel;
    if (!load_panel(panel, "restart.gui"))
        return;
    Calls calls;
    auto context = make_context(calls, SessionKind::campaign);
    context.host.wrap_text = [](void*, const char* text, int32_t, char* out, std::size_t cap) {
        std::snprintf(out, cap, "\n%s", text);
        for (char* p = out; *p; ++p)
            if (*p == ' ')
                *p = '\n';
        return std::strlen(out);
    };
    prefs::Preferences preferences{};
    preferences.difficulty = 1;
    context.preferences = &preferences;
    ingame_enter_restart(panel, context, "Mission Eleven");
    OA_CHECK(text_of(panel, "MISSIONNAME") == "Mission");
    OA_CHECK(text_of(panel, "MISSIONNAME1") == "Eleven");
    // The stored difficulty goes into the button's stage byte.
    OA_CHECK(panel_stage(panel, "Difficulty") == 1);
    OA_CHECK(panel_control(panel, "Difficulty")->active == 1);
    panel_set_stage(panel, "Difficulty", 2);
    calls.disc = false;
    select(panel, "RESTART");
    OA_CHECK(ingame_on_restart_click(panel, context) == IngameAction::none);
    OA_CHECK(
        !calls.messages.empty() && calls.messages.back().find("Campaign CD") != std::string::npos
    );
    calls.disc = true;
    select(panel, "RESTART");
    OA_CHECK(ingame_on_restart_click(panel, context) == IngameAction::restart_mission);
    OA_CHECK(preferences.difficulty == 2 && context.restart_requested);
}

OA_GAME_DATA_TEST(restart_dialog_other_controls) {
    Panel panel;
    if (!load_panel(panel, "restart.gui"))
        return;
    Calls calls;
    auto context = make_context(calls, SessionKind::skirmish);
    prefs::Preferences preferences{};
    preferences.difficulty = 7; // outside 0..2: the stage is left alone
    context.preferences = &preferences;
    panel_set_stage(panel, "Difficulty", 1);
    ingame_enter_restart(panel, context, "Coast To Coast");
    OA_CHECK(panel_stage(panel, "Difficulty") == 1);
    OA_CHECK(text_of(panel, "MISSIONNAME") == "Coast To Coast");

    // Difficulty plays the options sound a second time and drops the selection.
    select(panel, "Difficulty");
    OA_CHECK(ingame_on_restart_click(panel, context) == IngameAction::none);
    OA_CHECK((calls.sounds == std::vector<std::string>{"Options", "Options"}));
    OA_CHECK(panel.selected == kNoSelection);
    select(panel, "CANCEL");
    OA_CHECK(ingame_on_restart_click(panel, context) == IngameAction::closed);

    // A skirmish restart asks for the multiplayer disc.
    calls.disc = false;
    select(panel, "RESTART");
    OA_CHECK(ingame_on_restart_click(panel, context) == IngameAction::none);
    OA_CHECK(
        !calls.messages.empty() && calls.messages.back().find("Multiplayer CD") != std::string::npos
    );
    OA_CHECK(!context.restart_requested && panel.selected == kNoSelection);

    // Outside a campaign or skirmish RESTART returns with the selection kept.
    context.session = SessionKind::multiplayer;
    calls.disc = true;
    select(panel, "RESTART");
    OA_CHECK(ingame_on_restart_click(panel, context) == IngameAction::none);
    OA_CHECK(!context.restart_requested && panel.selected == panel_find(panel, "RESTART"));
}

struct RestartCalls {
    std::vector<std::string> log;
    ui::frontend_state::State* app = nullptr;
    int32_t bound = 0;
    bool bind_ok = true;
};

RestartHost recording_restart_host(RestartCalls& calls) {
    RestartHost host;
    host.context = &calls;
    host.bound_mission = [](void* c) {
        auto& calls = *static_cast<RestartCalls*>(c);
        calls.log.emplace_back("bound");
        return calls.bound;
    };
    // The teardown resets the player count the skirmish branch restores.
    host.end_session = [](void* c) {
        auto& calls = *static_cast<RestartCalls*>(c);
        calls.log.emplace_back("end");
        calls.app->player_count = 0;
    };
    host.reload_campaign = [](void* c) {
        static_cast<RestartCalls*>(c)->log.emplace_back("reload");
    };
    host.bind_mission = [](void* c, int32_t index) {
        auto& calls = *static_cast<RestartCalls*>(c);
        calls.log.push_back("bind " + std::to_string(index));
        return calls.bind_ok;
    };
    host.select_skirmish_map = [](void* c) {
        auto& calls = *static_cast<RestartCalls*>(c);
        calls.log.push_back("map " + std::to_string(calls.app->player_count));
    };
    host.apply_roster = [](void* c) { static_cast<RestartCalls*>(c)->log.emplace_back("roster"); };
    host.enter_frontend = [](void* c, bool in_game) {
        static_cast<RestartCalls*>(c)->log.push_back(in_game ? "frontend game" : "frontend menus");
    };
    return host;
}

OA_TEST(restart_reloads_the_bound_mission_or_map) {
    namespace flags = ui::frontend_state::flags;
    Calls calls;
    ui::frontend_state::State app{};
    RestartCalls restart;
    restart.app = &app;
    const auto host = recording_restart_host(restart);
    const auto single_player = [&app] { return (app.session_flags & flags::single_player) != 0; };

    // Without a restart request nothing runs.
    auto context = make_context(calls, SessionKind::campaign);
    OA_CHECK(ingame_run_restart(context, app, host) == RestartPath::none);
    OA_CHECK(restart.log.empty() && app.session_flags == 0);

    // Campaign: the bound mission is read after the teardown, the file reloads
    // under its own name, the mission rebinds, Game.session_flags bit 3 is
    // raised and mode 2 loads the game.
    context.restart_requested = true;
    restart.bound = 3;
    OA_CHECK(ingame_run_restart(context, app, host) == RestartPath::campaign);
    OA_CHECK((
        restart.log == std::vector<std::string>{"end", "bound", "reload", "bind 3", "frontend game"}
    ));
    OA_CHECK(app.session_flags == flags::single_player);

    // A failed rebind skips Game.session_flags bits 3 and 2: mode 2 stays in the menus.
    restart.log.clear();
    restart.bind_ok = false;
    app.session_flags = 0;
    OA_CHECK(ingame_run_restart(context, app, host) == RestartPath::campaign);
    OA_CHECK(restart.log.back() == "frontend menus");
    OA_CHECK(!single_player());

    // Skirmish: Game.player_count survives the teardown into the map selection, and
    // the single-player flag is left as it was.
    restart.log.clear();
    context.session = SessionKind::skirmish;
    app.player_count = 4;
    OA_CHECK(ingame_run_restart(context, app, host) == RestartPath::skirmish);
    OA_CHECK((restart.log == std::vector<std::string>{"end", "map 4", "roster", "frontend game"}));
    OA_CHECK(app.player_count == 4 && !single_player());

    // Every state other than campaign takes the skirmish branch.
    restart.log.clear();
    context.session = SessionKind::multiplayer;
    OA_CHECK(ingame_run_restart(context, app, host) == RestartPath::skirmish);
    OA_CHECK(restart.log.front() == "end" && restart.log[1] == "map 4" && !single_player());
}

/// Answers the German Yes and No as gamedata\translate.tdf gives them.
///
/// @param text the text
/// @return its German, or null for any other text
const char* german_yes_no(void*, const char* text) {
    if (std::strcmp(text, "Yes") == 0)
        return "Ja";
    if (std::strcmp(text, "No") == 0)
        return "Nein";
    return nullptr;
}

OA_GAME_DATA_TEST(yes_or_no_quick_keys_follow_the_language) {
    Panel confirm;
    if (!load_panel(confirm, "yesorno.gui"))
        return;
    Calls calls;
    auto context = make_context(calls, SessionKind::skirmish);
    context.exit_kind = ExitKind::leave_game;
    oa::data::languages::set_translation_hooks({nullptr, german_yes_no, nullptr});
    ingame_enter_exit_confirm(confirm, context);
    Panel watching;
    load_panel(watching, "yesorno.gui");
    ingame_enter_continue_watching(watching);
    oa::data::languages::set_translation_hooks({});
    // As in 3.1c, each button takes the first letter of its caption the
    // other's key does not hold: J for Ja and N for Nein.
    for (const Panel* panel : {&confirm, &watching}) {
        OA_CHECK(text_of(*panel, "CHOICE1") == "Ja");
        OA_CHECK(text_of(*panel, "CHOICE2") == "Nein");
        OA_CHECK(panel_control(*panel, "CHOICE1")->quick_key == 'J');
        OA_CHECK(panel_control(*panel, "CHOICE2")->quick_key == 'N');
    }
    // With no translation the keys are the English Y and N.
    ingame_enter_continue_watching(watching);
    OA_CHECK(text_of(watching, "CHOICE1") == "Yes");
    OA_CHECK(panel_control(watching, "CHOICE1")->quick_key == 'Y');
    OA_CHECK(panel_control(watching, "CHOICE2")->quick_key == 'N');
}

/// Answers the French of the texts the in-game menus set, as
/// gamedata\translate.tdf gives them in the game's code page.
///
/// @param text the text
/// @return its French, or null for any other text
const char* french_menu_texts(void*, const char* text) {
    static constexpr std::pair<const char*, const char*> kFrench[] = {
        {"Restart", "Red\xE9marrer"},
        {"Settings", "Jeu"},
        {"Surrender this battle and return to main menu?",
         "Abandonner cette bataille et retourner au menu principal?"},
        {"Exit the Battle", "Quitter la bataille"},
    };
    for (const auto& [english, french] : kFrench)
        if (std::strcmp(text, english) == 0)
            return french;
    return nullptr;
}

OA_GAME_DATA_TEST(texts_the_menus_set_follow_the_language) {
    Panel options;
    Panel exit_menu;
    Panel confirm;
    if (!load_panel(options, "armopt.gui") || !load_panel(exit_menu, "exitmenu.gui") ||
        !load_panel(confirm, "yesorno.gui"))
        return;
    Calls calls;
    auto context = make_context(calls, SessionKind::skirmish);
    oa::data::languages::set_translation_hooks({nullptr, french_menu_texts, nullptr});
    ingame_enter_options(options, context);
    ingame_enter_exit_menu(exit_menu, context);
    context.exit_kind = ExitKind::main_menu;
    ingame_enter_exit_confirm(confirm, context);
    Panel spectator;
    load_panel(spectator, "yesorno.gui");
    context.exit_kind = ExitKind::leave_game;
    context.spectating = true;
    ingame_enter_exit_confirm(spectator, context);
    oa::data::languages::set_translation_hooks({});
    // Each shows as the game data translates it; Restart's caption still
    // gives its button the quick key R.
    OA_CHECK(text_of(options, "MISSION") == "Jeu");
    OA_CHECK(text_of(exit_menu, "RESTART") == "Red\xE9marrer");
    OA_CHECK(panel_control(exit_menu, "RESTART")->quick_key == 'R');
    OA_CHECK(
        text_of(confirm, "TITLE") == "Abandonner cette bataille et retourner au menu principal?"
    );
    OA_CHECK(text_of(spectator, "TITLE") == "Quitter la bataille");
    // A text without a translation shows as it is.
    OA_CHECK(text_of(exit_menu, "MAINMENU") == "Exit to Menu");
    // With no translation the texts are the English ones.
    ingame_enter_exit_menu(exit_menu, context);
    OA_CHECK(text_of(exit_menu, "RESTART") == "Restart");
}

OA_GAME_DATA_TEST(continue_watching_prompt) {
    Panel panel;
    if (!load_panel(panel, "yesorno.gui"))
        return;
    Calls calls;
    auto context = make_context(calls, SessionKind::multiplayer);
    context.quit_flags = quit_flag::out_of_game;
    ingame_enter_continue_watching(panel);
    OA_CHECK(text_of(panel, "TITLE") == "You're out!  Continue Watching?");
    select(panel, "CHOICE1");
    OA_CHECK(ingame_on_continue_watching_click(panel, context) == IngameAction::keep_watching);
    OA_CHECK(context.quit_flags == 0);
    select(panel, "CHOICE2");
    OA_CHECK(ingame_on_continue_watching_click(panel, context) == IngameAction::leave_game);
    OA_CHECK(context.quit_flags == quit_flag::leave_application);
    OA_CHECK(calls.sounds.back() == "BigButton");
}

OA_TEST(game_settings_rows) {
    GameSettingsView view;
    view.session = SessionKind::skirmish;
    view.commander_rule = 1;
    view.fixed_locations = 0;
    view.mapping_flags = 2 | 4; // LOS on, circular
    view.difficulty = 2;
    view.map_name = "Coast To Coast";
    view.starting_metal = 1000;
    view.starting_energy = 1000;
    view.max_units = 250;
    GameSettingsSheet sheet;
    ingame_build_game_settings(view, sheet);
    OA_CHECK(sheet.count == 18);
    const auto text = [&](std::size_t i) { return std::string(sheet.entries[i].text.data()); };
    OA_CHECK(text(0) == "Commander Death:" && text(1) == "Game Ends");
    OA_CHECK(text(3) == "Random");
    OA_CHECK(text(5) == "Mapped");
    OA_CHECK(text(7) == "True"); // LOS on, circular bit set
    OA_CHECK(text(8) == "Difficulty:" && text(9) == "Hard");
    OA_CHECK(text(11) == "Coast To Coast");
    OA_CHECK(text(17) == "250");
    OA_CHECK(sheet.entries[1].x == 0x8c && sheet.entries[1].y == 0x5a);
    OA_CHECK(sheet.entries[17].y == 0x5a + 8 * 0x12);

    view.session = SessionKind::multiplayer;
    view.cheats_allowed = true;
    view.mapping_flags = 0;
    ingame_build_game_settings(view, sheet);
    OA_CHECK(sheet.count == 20);
    OA_CHECK(text(7) == "Permanent");
    OA_CHECK(text(8) == "Cheat Codes:" && text(9) == "Allowed");
    OA_CHECK(text(11) == "Disallowed");

    // ai.difficulty-names: the row shows the name the difficulty carries.
    using Names = data::match_rules::AiDifficultyNamesNames;
    view.session = SessionKind::skirmish;
    view.difficulty_names.names = {Names::hard, Names::medium, Names::easy};
    view.difficulty = 0;
    ingame_build_game_settings(view, sheet);
    OA_CHECK(text(8) == "Difficulty:" && text(9) == "Hard");
    view.difficulty = 2;
    ingame_build_game_settings(view, sheet);
    OA_CHECK(text(9) == "Easy");
}

} // namespace
} // namespace oa::ui::frontend::test
