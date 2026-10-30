// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// In-game options panel, exit menu, confirmations, restart and settings sheet.
#include "oa/ui/frontend/ingame_menu.hpp"

#include "oa/base/text/line_break.hpp"
#include "oa/data/match_rules/difficulty_names.hpp"
#include "oa/ui/campaign/frontend_host.hpp"
#include "oa/ui/campaign/single_player.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace oa::ui::frontend {

namespace {

constexpr std::string_view kOptionsSound = "Options";
constexpr std::string_view kExitSound = "Exit";
constexpr std::string_view kBigButtonSound = "BigButton";
constexpr int32_t kDiscMessageWidth = 200;
constexpr int32_t kSettingsLabelX = 0x12;
constexpr int32_t kSettingsLabelWidth = 0x6e;
constexpr int32_t kSettingsValueX = 0x8c;
constexpr int32_t kSettingsValueWidth = 0x78;
constexpr uint16_t kSettingsFirstRow = 0x5a;
constexpr uint16_t kSettingsRowStep = 0x12;

void play(const IngameContext& context, std::string_view name) {
    if (context.host.play_sound != nullptr)
        context.host.play_sound(context.host.context, name.data());
}

std::string_view return_label_text(const IngameContext& context) noexcept {
    return {
        context.return_label.data(),
        ::strnlen(context.return_label.data(), context.return_label.size())
    };
}

bool is_multiplayer(const IngameContext& context) noexcept {
    return context.session == SessionKind::multiplayer;
}

/// Captions YESORNO's CHOICE1 "Yes" and CHOICE2 "No", in that order, in the language shown.
///
/// Each button then takes the first letter of its caption that no other
/// button's quick key holds, as 3.1c assigns them: Y and N in English, J and
/// N in German.
///
/// @param[in,out] panel the loaded YESORNO panel
void set_choice_labels(Panel& panel) {
    panel_set_text(panel, "CHOICE1", "Yes");
    panel_set_text(panel, "CHOICE2", "No");
}

// Checks the disc a campaign or skirmish restart/load needs; false after
// showing the insert-disc message.
bool require_disc(IngameContext& context, SessionKind kind) {
    const bool present =
        context.host.disc_present == nullptr || context.host.disc_present(context.host.context);
    if (present)
        return true;
    if (context.host.show_message != nullptr) {
        const char* text = kind == SessionKind::campaign
                               ? "Please insert the Campaign CD (Disc 2) and try again"
                               : "Please insert the Multiplayer CD (Disc 1) and try again";
        context.host.show_message(context.host.context, text, kDiscMessageWidth);
    }
    return false;
}

// The campaign package's button helpers act on this panel's records.
campaign::FrontendHost panel_host(Panel& panel) {
    campaign::FrontendHost host{};
    host.context = &panel;
    host.select_group = [](void* context, const char* name) {
        (void)panel_set_group_value(*static_cast<Panel*>(context), name, 1);
    };
    host.mark_dirty = [](void* context) { static_cast<Panel*>(context)->dirty = true; };
    host.set_stage = [](void* context, const char* name, uint8_t stage) {
        (void)panel_set_stage(*static_cast<Panel*>(context), name, stage);
    };
    return host;
}

void add_entry(
    GameSettingsSheet& sheet,
    const char* text,
    int32_t x,
    uint16_t y,
    int32_t width,
    TranslateFn translate,
    void* translate_context
) {
    if (sheet.count >= sheet.entries.size())
        return;
    auto& entry = sheet.entries[sheet.count++];
    const char* shown = text;
    if (translate != nullptr) {
        if (const char* replacement = translate(translate_context, text))
            shown = replacement;
    }
    std::snprintf(entry.text.data(), entry.text.size(), "%s", shown);
    entry.x = static_cast<int16_t>(x);
    entry.y = y;
    entry.width = width;
}

} // namespace

// ---------------------------------------------------------------------------
// ARMOPT

void ingame_enter_options(Panel& panel, IngameContext& context) noexcept {
    const bool no_saved_games = is_multiplayer(context) || !context.saved_games_offered;
    panel_set_grayed(panel, "SAVEGAME", no_saved_games);
    panel_set_grayed(panel, "LOADGAME", no_saved_games);
    if (context.session == SessionKind::multiplayer || context.session == SessionKind::skirmish)
        panel_set_text(panel, "MISSION", "Settings");
    panel.dirty = true;
    if (!is_multiplayer(context))
        context.hold_game = true;
    if (context.host.set_cd_playback != nullptr)
        context.host.set_cd_playback(context.host.context, true);
}

IngameAction ingame_on_options_click(Panel& panel, IngameContext& context) noexcept {
    if (panel.selected == kNoSelection) {
        if (context.host.release_lightbar != nullptr)
            context.host.release_lightbar(context.host.context);
        if (context.in_game && !is_multiplayer(context))
            context.hold_game = false;
        context.realtime_panels = false;
        if (context.host.set_cd_playback != nullptr)
            context.host.set_cd_playback(context.host.context, false);
        return IngameAction::closed;
    }

    struct Route {
        std::string_view name;
        IngameAction action;
    };

    static constexpr Route routes[] = {
        {"LOADGAME", IngameAction::open_load_game},
        {"SAVEGAME", IngameAction::open_save_game},
        {"PREFS", IngameAction::open_options},
        {"HELP", IngameAction::open_help},
        {"MISSION", IngameAction::open_briefing},
        {"EXIT", IngameAction::open_exit_menu},
        {"OK", IngameAction::none},
    };
    for (const auto& route : routes) {
        if (!panel_selected_is(panel, route.name))
            continue;
        play(context, kOptionsSound);
        if (route.action == IngameAction::open_briefing && context.session != SessionKind::campaign)
            return IngameAction::open_game_settings;
        return route.action;
    }
    panel_clear_selection(panel);
    return IngameAction::none;
}

// ---------------------------------------------------------------------------
// EXITMENU and its confirmation

void ingame_enter_exit_menu(Panel& panel, IngameContext& context) noexcept {
    if (context.session == SessionKind::campaign || context.session == SessionKind::skirmish) {
        panel_set_active(panel, "RESTART", 1);
        panel_set_text(panel, "RESTART", "Restart");
    } else if (
        const auto label = return_label_text(context); context.spectating || !label.empty()
    ) {
        const bool has_label = !label.empty() && label.size() < kMaxReturnLabelLength + 1;
        const char* target = nullptr;
        if (label.empty()) {
            panel_set_active(panel, "MAINMENU", 0);
            target = "EXITGAME";
        } else {
            panel_set_active(panel, "EXITGAME", 0);
            target = "MAINMENU";
        }
        if (has_label)
            panel_set_text(panel, target, label);
    }
    panel.dirty = true;
}

IngameAction ingame_on_exit_menu_click(Panel& panel, IngameContext& context) noexcept {
    if (panel.selected == kNoSelection)
        return IngameAction::none;
    play(context, kOptionsSound);
    if (panel_selected_is(panel, "MAINMENU")) {
        context.exit_kind = ExitKind::main_menu;
        return IngameAction::open_exit_confirm;
    }
    if (panel_selected_is(panel, "EXITGAME")) {
        context.exit_kind = ExitKind::leave_game;
        return IngameAction::open_exit_confirm;
    }
    if (panel_selected_is(panel, "CANCEL"))
        return IngameAction::closed;
    if (panel_selected_is(panel, "RESTART"))
        return IngameAction::open_restart;
    panel_clear_selection(panel);
    return IngameAction::none;
}

void ingame_enter_exit_confirm(Panel& panel, const IngameContext& context) noexcept {
    set_choice_labels(panel);
    char title[0x81] = {};
    bool has_title = true;
    if (context.exit_kind == ExitKind::main_menu) {
        const auto label = return_label_text(context);
        if (!label.empty() && label.size() <= kMaxReturnLabelLength)
            std::snprintf(
                title,
                sizeof title,
                "Surrender this battle and return to %.*s?",
                static_cast<int>(label.size()),
                label.data()
            );
        else
            std::snprintf(
                title, sizeof title, "%s", "Surrender this battle and return to main menu?"
            );
    } else if (context.exit_kind == ExitKind::leave_game) {
        const std::string_view leave = context.spectating                ? "Exit the Battle"
                                       : !context.leave_question.empty() ? context.leave_question
                                                                         : kLeaveGameTitle;
        std::snprintf(title, sizeof title, "%.*s", static_cast<int>(leave.size()), leave.data());
    } else {
        has_title = false;
    }
    if (has_title)
        panel_set_text(panel, "TITLE", title);
    // kExitConfirmDefault answers Escape; ingame_exit_confirm_accept()'s
    // choice answers Enter and takes the focus.
    panel.selected = kNoSelection;
    panel.dirty = true;
}

std::string_view ingame_exit_confirm_accept(bool close_request) noexcept {
    return close_request ? kCloseConfirmAccept : kExitConfirmDefault;
}

void ingame_open_leave_confirm(Panel& panel, IngameContext& context) noexcept {
    context.exit_kind = ExitKind::leave_game;
    ingame_enter_exit_confirm(panel, context);
}

IngameAction ingame_on_exit_confirm_click(Panel& panel, IngameContext& context) noexcept {
    if (panel.selected == kNoSelection)
        return IngameAction::none;
    play(context, kExitSound);
    if (panel_selected_is(panel, "CHOICE1")) {
        if (context.exit_kind == ExitKind::main_menu ||
            context.exit_kind == ExitKind::labelled_return)
            return IngameAction::return_to_main_menu;
        if (context.exit_kind == ExitKind::leave_game) {
            context.quit_flags |= quit_flag::leave_application;
            return IngameAction::leave_game;
        }
        return IngameAction::none;
    }
    if (panel_selected_is(panel, "CHOICE2"))
        return IngameAction::closed;
    panel_clear_selection(panel);
    return IngameAction::none;
}

// ---------------------------------------------------------------------------
// RESTART

void ingame_enter_restart(Panel& panel, IngameContext& context, std::string_view mission) noexcept {
    char name[0x100] = {};
    // The name's whole characters that fit.
    std::snprintf(
        name,
        sizeof name,
        "%.*s",
        static_cast<int>(oa::base::text::whole_character_bytes(mission, sizeof name - 1)),
        mission.data()
    );
    char wrapped[0x200] = {};
    const auto* field = panel_control(panel, "MISSIONNAME");
    const int32_t width = field != nullptr ? field->width : 0;
    if (context.host.wrap_text != nullptr)
        context.host.wrap_text(context.host.context, name, width, wrapped, sizeof wrapped);
    else
        std::snprintf(wrapped, sizeof wrapped, "%s", name);
    // Split at '\n', skipping empty lines: the first two lines are shown.
    std::string_view rest(wrapped);
    const auto next_line = [&rest]() -> std::string_view {
        const auto start = rest.find_first_not_of('\n');
        if (start == std::string_view::npos) {
            rest = {};
            return {};
        }
        rest.remove_prefix(start);
        const auto end = rest.find('\n');
        const auto line = rest.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        return line;
    };
    panel_set_text(panel, "MISSIONNAME", next_line());
    if (const auto second = next_line(); !second.empty())
        panel_set_text(panel, "MISSIONNAME1", second);
    if (context.preferences != nullptr) {
        const auto host = panel_host(panel);
        const auto difficulty = static_cast<int32_t>(context.preferences->difficulty);
        campaign::show_difficulty(difficulty, &host, context.difficulty_names);
    }
    panel.dirty = true;
}

IngameAction ingame_on_restart_click(Panel& panel, IngameContext& context) noexcept {
    if (panel.selected == kNoSelection)
        return IngameAction::none;
    play(context, kOptionsSound);
    if (panel_selected_is(panel, "RESTART")) {
        if (context.session != SessionKind::campaign && context.session != SessionKind::skirmish)
            return IngameAction::none;
        if (!require_disc(context, context.session)) {
            panel_clear_selection(panel);
            return IngameAction::none;
        }
        if (context.host.refresh_archives != nullptr)
            context.host.refresh_archives(context.host.context);
        const auto stage = panel_stage(panel, "Difficulty");
        if (context.preferences != nullptr)
            context.preferences->difficulty = stage;
        context.restart_requested = true;
        return IngameAction::restart_mission;
    }
    if (panel_selected_is(panel, "Difficulty")) {
        play(context, kOptionsSound);
    } else if (panel_selected_is(panel, "CANCEL")) {
        return IngameAction::closed;
    }
    panel_clear_selection(panel);
    return IngameAction::none;
}

RestartPath ingame_run_restart(
    const IngameContext& context, ui::frontend_state::State& app, const RestartHost& host
) noexcept {
    if (!context.restart_requested)
        return RestartPath::none;
    const auto end_session = [&host] {
        if (host.end_session != nullptr)
            host.end_session(host.context);
    };
    auto path = RestartPath::skirmish;
    bool in_game = true;
    if (context.session == SessionKind::campaign) {
        path = RestartPath::campaign;
        end_session();
        // Read after the teardown and before the reload, which binds mission 0.
        const int32_t mission =
            host.bound_mission != nullptr ? host.bound_mission(host.context) : 0;
        if (host.reload_campaign != nullptr)
            host.reload_campaign(host.context);
        in_game = host.bind_mission != nullptr && host.bind_mission(host.context, mission);
        if (in_game)
            app.session_flags |= ui::frontend_state::flags::single_player;
    } else {
        const uint16_t players = app.player_count;
        end_session();
        app.player_count = players;
        if (host.select_skirmish_map != nullptr)
            host.select_skirmish_map(host.context);
        if (host.apply_roster != nullptr)
            host.apply_roster(host.context);
    }
    if (host.enter_frontend != nullptr)
        host.enter_frontend(host.context, in_game);
    return path;
}

// ---------------------------------------------------------------------------
// Continue watching

void ingame_enter_continue_watching(Panel& panel) noexcept {
    set_choice_labels(panel);
    panel_set_text(panel, "TITLE", "You're out!  Continue Watching?");
    panel.dirty = true;
}

IngameAction ingame_on_continue_watching_click(Panel& panel, IngameContext& context) noexcept {
    if (panel.selected == kNoSelection)
        return IngameAction::none;
    play(context, kBigButtonSound);
    if (panel_selected_is(panel, "CHOICE1")) {
        context.quit_flags &= static_cast<uint8_t>(~quit_flag::out_of_game);
        return IngameAction::keep_watching;
    }
    if (panel_selected_is(panel, "CHOICE2")) {
        context.quit_flags |= quit_flag::leave_application;
        context.quit_flags &= static_cast<uint8_t>(~quit_flag::out_of_game);
        return IngameAction::leave_game;
    }
    panel_clear_selection(panel);
    return IngameAction::none;
}

// ---------------------------------------------------------------------------
// GAMEOPTIONS

void ingame_build_game_settings(
    const GameSettingsView& view,
    GameSettingsSheet& sheet,
    TranslateFn translate,
    void* translate_context
) noexcept {
    static constexpr const char* commander_rules[] = {"Game Continues", "Game Ends", "Deathmatch"};
    static constexpr const char* locations[] = {"Random", "Fixed"};
    static constexpr const char* mapping[] = {"Mapped", "Unmapped"};
    static constexpr const char* allowed[] = {"Disallowed", "Allowed"};
    static constexpr const char* sight[] = {"True", "Circular", "Permanent"};
    static constexpr const char* difficulty[] = {"Easy", "Medium", "Hard"};
    const auto pick = [](const char* const* table, std::size_t size, uint32_t index) {
        return index < size ? table[index] : "";
    };
    sheet = GameSettingsSheet{};
    const auto label = [&](const char* text, uint16_t y) {
        add_entry(
            sheet, text, kSettingsLabelX, y, kSettingsLabelWidth, translate, translate_context
        );
    };
    const auto value = [&](const char* text, uint16_t y) {
        add_entry(
            sheet, text, kSettingsValueX, y, kSettingsValueWidth, translate, translate_context
        );
    };
    const auto number = [&](uint32_t amount, uint16_t y) {
        char text[16];
        std::snprintf(text, sizeof text, "%d", static_cast<int32_t>(amount));
        add_entry(sheet, text, kSettingsValueX, y, kSettingsValueWidth, nullptr, nullptr);
    };
    uint16_t y = kSettingsFirstRow;
    label("Commander Death:", y);
    value(pick(commander_rules, 3, view.commander_rule), y);
    y = static_cast<uint16_t>(y + kSettingsRowStep);
    label("Starting Locations:", y);
    value(pick(locations, 2, view.fixed_locations), y);
    y = static_cast<uint16_t>(y + kSettingsRowStep);
    label("Mapping Mode:", y);
    value(mapping[view.mapping_flags & 1U], y);
    y = static_cast<uint16_t>(y + kSettingsRowStep);
    label("Line of Sight:", y);
    uint32_t sight_index = 2;
    if ((view.mapping_flags & 2U) != 0)
        sight_index = ((~view.mapping_flags) & 4U) >> 2;
    value(sight[sight_index], y);
    y = static_cast<uint16_t>(y + kSettingsRowStep);
    if (view.session == SessionKind::multiplayer) {
        label("Cheat Codes:", y);
        value(allowed[view.cheats_allowed ? 1 : 0], y);
        y = static_cast<uint16_t>(y + kSettingsRowStep);
        label("Watching:", y);
        value(allowed[view.watching_allowed ? 1 : 0], y);
    } else {
        label("Difficulty:", y);
        value(
            pick(
                difficulty,
                3,
                static_cast<uint32_t>(data::match_rules::difficulty_name_index(
                    view.difficulty_names, static_cast<int32_t>(view.difficulty)
                ))
            ),
            y
        );
    }
    y = static_cast<uint16_t>(y + kSettingsRowStep);
    char map[kControlTextBytes];
    std::snprintf(
        map, sizeof map, "%.*s", static_cast<int>(view.map_name.size()), view.map_name.data()
    );
    label("Map:", y);
    value(map, y);
    y = static_cast<uint16_t>(y + kSettingsRowStep);
    label("Starting Metal:", y);
    number(view.starting_metal, y);
    y = static_cast<uint16_t>(y + kSettingsRowStep);
    label("Starting Energy:", y);
    number(view.starting_energy, y);
    y = static_cast<uint16_t>(y + kSettingsRowStep);
    label("Max Units:", y);
    number(view.max_units, y);
}

IngameAction ingame_on_game_settings_click(Panel& panel, IngameContext& context) noexcept {
    if (panel.selected == kNoSelection)
        return IngameAction::none;
    if (panel_selected_is(panel, "OK")) {
        play(context, kOptionsSound);
        return IngameAction::closed;
    }
    panel_clear_selection(panel);
    return IngameAction::none;
}

} // namespace oa::ui::frontend
