// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle console's model against a recording service: the mode
// categories, the records the battle feeds keep, the panels a click opens,
// the Host panel's game types, and the requests the console sends.
#include "oa/boneyards/screens/battle_list.hpp"
#include "oa/boneyards/screens/navigator.hpp"

#include "check.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace oa::boneyards;
using namespace oa::boneyards::screens;

namespace {

// The battle_info row, battle_intel and first battle_report line of Slag
// (area 0x709) the Rover lobby test uses.
constexpr uint32_t kSlag = 0x709;
constexpr const char* kGame = "9bfaa0d0f7b93f12";

struct Recorder {
    std::vector<std::string> calls;
    std::vector<bool> lights;
    std::vector<std::string> intel_lines;
    std::vector<std::string> report_messages;
    int32_t host_opens = 0;
    ServiceResult result = ServiceResult::pending;
};

Recorder g;

ServiceResult record_enter(void*, uint32_t area_id) {
    g.calls.push_back("enter " + std::to_string(area_id));
    return g.result;
}

ServiceResult record_leave(void*, uint32_t area_id) {
    g.calls.push_back("leave " + std::to_string(area_id));
    return g.result;
}

ServiceResult record_get_battle(void*, uint32_t area_id, const char* game_id) {
    g.calls.push_back("get " + std::to_string(area_id) + " " + game_id);
    return g.result;
}

BoneyardsService recording_service() {
    BoneyardsService service = boneyards_offline_service(nullptr);
    service.enter_battles = record_enter;
    service.leave_battles = record_leave;
    service.get_battle = record_get_battle;
    return service;
}

BattleUi recording_ui() {
    BattleUi ui{};
    ui.activity = [](void*, bool running) { g.lights.push_back(running); };
    ui.open_host = [](void*) { ++g.host_opens; };
    ui.intel = [](void*, const char* text) { g.intel_lines.emplace_back(text); };
    ui.report_message = [](void*, const char* text) { g.report_messages.emplace_back(text); };
    return ui;
}

bool light_on() {
    return !g.lights.empty() && g.lights.back();
}

// The row showing `battle`, or row_count when none does.
std::size_t row_of(const BattleList* list, int32_t battle) {
    for (std::size_t i = 0; i < list->row_count; ++i)
        if (!list->rows[i].is_category && list->rows[i].battle == battle)
            return i;
    return list->row_count;
}

void add(
    BattleList* list,
    const char* game_id,
    const char* name,
    int32_t type,
    int32_t forming,
    int32_t host
) {
    battle_list_on_battle_info(list, kSlag, 0, game_id, name, type, forming, host);
}

void remove(BattleList* list, const char* game_id) {
    battle_list_on_battle_info(list, kSlag, 1, game_id, "", 0, 0, 2);
}

BattleList g_list;
BoneyardsService g_service;

BattleList* fresh_list() {
    g = Recorder{};
    g_service = recording_service();
    battle_list_init(&g_list, &g_service, recording_ui());
    return &g_list;
}

// battle_list_init builds one category per mode, each expanded, and requests
// nothing: the console's open slot enters.
void test_categories() {
    BattleList* list = fresh_list();
    CHECK(g.calls.empty());
    CHECK(list->category_count == 5 && list->row_count == 5);
    for (std::size_t i = 0; i < list->category_count; ++i)
        CHECK(
            list->categories[i].expanded && list->categories[i].battle_count == 0 &&
            list->rows[i].is_category
        );
    CHECK(list->categories[3].mode.mode == GameMode::galactic_war);
    CHECK(list->join_battle == kNoBattle && list->scoreboard_battle == kNoBattle);
}

// Opening enters the session area's battles (slot 15) after starting the
// light; the answer stops it; closing empties the categories and leaves
// (slot 17), again with the light running. A refused request stops it.
void test_open_and_close() {
    BattleList* list = fresh_list();
    battle_list_open(list, kSlag);
    CHECK(g.calls.size() == 1 && g.calls[0] == "enter 1801" && light_on());
    battle_list_on_done(list);
    CHECK(!light_on());
    add(list, kGame, "test", 4, 1, 2);
    battle_list_close(list, kSlag);
    CHECK(g.calls.size() == 2 && g.calls[1] == "leave 1801" && light_on());
    CHECK(list->categories[3].battle_count == 0 && list->rows_dirty);
    CHECK(battle_list_find(list, kGame) == kNoBattle); // the close freed it
    g.result = ServiceResult::not_connected;
    battle_list_open(list, kSlag);
    CHECK(!light_on());
}

// The forming row files a Galactic War record with the record defaults and
// the row's fields; a refresh keeps the one record; a row with change 1
// unfiles and frees it.
void test_battle_info() {
    BattleList* list = fresh_list();
    add(list, kGame, "test", 4, 1, 2);
    const int32_t battle = battle_list_find(list, kGame);
    const BattleEntry* b = battle_list_entry(list, battle);
    CHECK(b != nullptr);
    if (b == nullptr)
        return;
    CHECK_STR(b->game_id, kGame);
    CHECK_STR(b->name, "test");
    CHECK(b->type == 4 && b->forming == 1 && b->host_code == 2 && b->area_id == kSlag);
    CHECK(b->on_death == 2 && b->references == 1 && b->category == 3);
    CHECK(list->categories[3].battle_count == 1 && list->rows_dirty);
    battle_list_rebuild_rows(list);
    CHECK(list->row_count == 6 && row_of(list, battle) == 4);

    add(list, kGame, "renamed", 4, 0, 1);
    CHECK(battle_list_find(list, kGame) == battle && list->categories[3].battle_count == 1);
    CHECK_STR(b->name, "renamed");
    CHECK(b->forming == 0 && b->host_code == 1 && b->references == 1);

    add(list, "a", "b", 1, 0, 1);
    const int32_t duel = battle_list_find(list, "a");
    CHECK(duel != kNoBattle && list->categories[1].battle_count == 1);
    battle_list_rebuild_rows(list);
    CHECK(list->row_count == 7 && row_of(list, duel) == 2);

    remove(list, "a");
    CHECK(battle_list_find(list, "a") == kNoBattle && list->categories[1].battle_count == 0);
    battle_list_rebuild_rows(list);
    CHECK(list->row_count == 6 && row_of(list, battle) == 4);
    remove(list, "unknown"); // nothing to remove
    CHECK(list->categories[3].battle_count == 1);
    CHECK(g.calls.empty()); // no panel shows these battles
}

// A category row toggles its battles; a battle row asks for its intel
// (slot 19) with the light on, then shows it on the Join summary while
// forming and on the Scoreboard once in progress, closing the
// other panels; the panel holds the record.
void test_panel_choice() {
    BattleList* list = fresh_list();
    add(list, kGame, "test", 4, 1, 2);
    add(list, "a", "b", 4, 0, 1);
    const int32_t forming = battle_list_find(list, kGame);
    const int32_t running = battle_list_find(list, "a");
    battle_list_rebuild_rows(list);
    battle_list_on_control(list, BattleControl::battle_list, row_of(list, forming));
    CHECK(g.calls.size() == 1 && g.calls[0] == "get 1801 9bfaa0d0f7b93f12" && light_on());
    CHECK(list->join_visible && list->join_battle == forming && !list->scoreboard_visible);
    CHECK(battle_list_entry(list, forming)->references == 2);

    battle_list_on_control(list, BattleControl::battle_list, row_of(list, running));
    CHECK(g.calls.size() == 2 && g.calls[1] == "get 1801 a");
    CHECK(list->scoreboard_visible && list->scoreboard_battle == running);
    CHECK(!list->join_visible && list->join_battle == kNoBattle);
    CHECK(battle_list_entry(list, forming)->references == 1);
    CHECK(battle_list_entry(list, running)->references == 2);

    battle_list_on_control(list, BattleControl::battle_list, 3); // Galactic War category
    CHECK(!list->categories[3].expanded && list->rows_dirty && g.calls.size() == 2);
    battle_list_rebuild_rows(list);
    CHECK(list->row_count == 5);
    battle_list_on_control(list, BattleControl::battle_list, 3);
    battle_list_rebuild_rows(list);
    CHECK(list->row_count == 7);

    // HOST toggles the Host panel over the others.
    battle_list_on_control(list, BattleControl::host_button, 0);
    CHECK(list->host_visible && !list->scoreboard_visible && g.host_opens == 1);
    CHECK(battle_list_entry(list, running)->references == 1);
    battle_list_on_control(list, BattleControl::host_button, 0);
    CHECK(!list->host_visible && g.host_opens == 1);
}

// A battle update with the Join summary showing the battle: once in progress the
// battle moves to the Scoreboard and its intel is asked for again; removed,
// the panel closes and the record is freed. A Scoreboard keeps a removed
// battle's record until it closes.
void test_shown_battle_changes() {
    BattleList* list = fresh_list();
    add(list, kGame, "test", 4, 1, 2);
    const int32_t battle = battle_list_find(list, kGame);
    battle_list_rebuild_rows(list);
    battle_list_on_control(list, BattleControl::battle_list, row_of(list, battle));
    add(list, kGame, "test", 4, 1, 2);
    CHECK(list->join_visible && g.calls.size() == 2 && g.calls[1] == "get 1801 9bfaa0d0f7b93f12");
    add(list, kGame, "test", 4, 0, 2);
    CHECK(!list->join_visible && list->scoreboard_visible && list->scoreboard_battle == battle);
    CHECK(g.calls.size() == 3 && battle_list_entry(list, battle)->references == 2);

    remove(list, kGame);
    CHECK(list->scoreboard_visible && list->categories[3].battle_count == 0);
    const BattleEntry* kept = battle_list_entry(list, battle);
    CHECK(kept != nullptr && kept->references == 1 && kept->area_id == 0);
    battle_list_close_panels(list);
    CHECK(battle_list_find(list, kGame) == kNoBattle);

    add(list, "a", "b", 1, 1, 0);
    const int32_t duel = battle_list_find(list, "a");
    battle_list_rebuild_rows(list);
    battle_list_on_control(list, BattleControl::battle_list, row_of(list, duel));
    CHECK(list->join_visible && list->join_battle == duel);
    remove(list, "a");
    CHECK(!list->join_visible && list->join_battle == kNoBattle);
    CHECK(battle_list_find(list, "a") == kNoBattle);
}

// The intel stops the light and fills the record; a failed one fills
// nothing. The report hands a line with a message to the Scoreboard showing
// the battle, and fills score lines: rows by user id, the rank title before
// each name, a fifth player dropped.
void test_intel_and_report() {
    BattleList* list = fresh_list();
    add(list, kGame, "test", 4, 0, 2);
    const int32_t battle = battle_list_find(list, kGame);
    const BattleEntry* b = battle_list_entry(list, battle);
    g.lights.push_back(true);
    BattleIntel intel{kGame, 1, "kestrel", 0, "Acid Pools", 0, 0, 2, false};
    battle_list_on_battle_intel(list, &intel);
    CHECK(!light_on());
    CHECK(b->host_user_id == 1 && b->has_password == 0 && b->skill == 0 && b->players == 0);
    CHECK(b->on_death == 2);
    CHECK_STR(b->host_name, "kestrel");
    CHECK_STR(b->mission, "Acid Pools");
    BattleIntel failed{kGame, 9, "other", 1, "Gods of War", 2, 1, 3, true};
    battle_list_on_battle_intel(list, &failed);
    CHECK_STR(b->host_name, "kestrel");

    BattleReport first{kGame, 1, 0, "kestrel", 0, 0, 0, 0, 0, ""};
    BattleReport second{kGame, 2, 3, "second", 9, 2, 1500, 800, 40, ""};
    battle_list_on_battle_report(list, &first);
    battle_list_on_battle_report(list, &second);
    CHECK(b->report_count == 2);
    CHECK_STR(b->report_names, "kestrel, Core Sergeant second");
    CHECK(b->scores[0].user_id == 1 && b->scores[1].user_id == 2 && b->scores[1].icon == 3);
    CHECK_STR(b->scores[0].name, "kestrel");
    CHECK_STR(b->scores[0].kills, "0");
    CHECK_STR(b->scores[1].kills, "9");
    CHECK_STR(b->scores[1].losses, "2");
    CHECK_STR(b->scores[1].metal, "800");
    CHECK_STR(b->scores[1].energy, "1500");
    CHECK_STR(b->scores[1].score, "40");

    // The second line carries a message: not a score line.
    BattleReport message{kGame, 2, 3, "second", 9, 0, 0, 0, 40, "gg"};
    battle_list_on_battle_report(list, &message);
    CHECK(g.report_messages.empty() && b->report_count == 2);
    battle_list_rebuild_rows(list);
    battle_list_on_control(list, BattleControl::battle_list, row_of(list, battle));
    battle_list_on_battle_report(list, &message);
    CHECK(g.report_messages.size() == 1 && g.report_messages[0] == "gg" && b->report_count == 2);

    BattleReport third{kGame, 3, 0, "c", 1, 1, 1, 1, 1, ""};
    BattleReport fourth{kGame, 4, 0, "d", 1, 1, 1, 1, 1, ""};
    BattleReport fifth{kGame, 5, 0, "e", 1, 1, 1, 1, 1, ""};
    battle_list_on_battle_report(list, &third);
    battle_list_on_battle_report(list, &fourth);
    battle_list_on_battle_report(list, &fifth);
    CHECK(b->report_count == 4 && b->scores[3].user_id == 4);
    CHECK_STR(b->report_names, "kestrel, Core Sergeant second, c, d");
    battle_list_on_battle_report(list, &first); // the same player again
    CHECK(b->report_count == 5);
    CHECK_STR(b->report_names, "kestrel, Core Sergeant second, c, d, kestrel");

    battle_list_on_battle_intel(list, &intel);
    CHECK(b->report_count == 0 && b->report_names[0] == '\0' && b->scores[1].user_id == 2);

    battle_list_on_intel(list, "Game over");
    CHECK(g.intel_lines.size() == 1 && g.intel_lines[0] == "Game over");
}

// The list keeps every battle the feed reports, far past the 64 an early
// fixed table held: each is filed under its mode and listed, found by its
// id, and freed slots are reused.
void test_many_battles() {
    BattleList* list = fresh_list();
    constexpr int32_t kCount = 200;
    char id[16];
    for (int32_t i = 0; i < kCount; ++i) {
        std::snprintf(id, sizeof id, "g%03d", i);
        add(list, id, "many", i % 2 == 0 ? 0 : 4, 1, 2);
    }
    CHECK(list->categories[0].battle_count == kCount / 2);
    CHECK(list->categories[3].battle_count == kCount / 2);
    battle_list_rebuild_rows(list);
    CHECK(list->row_count == list->category_count + kCount);
    for (int32_t i = 0; i < kCount; i += 37) {
        std::snprintf(id, sizeof id, "g%03d", i);
        const int32_t battle = battle_list_find(list, id);
        CHECK(battle != kNoBattle && row_of(list, battle) < list->row_count);
        const BattleEntry* entry = battle_list_entry(list, battle);
        CHECK(entry != nullptr && std::strcmp(entry->game_id, id) == 0);
    }
    const int32_t freed = battle_list_find(list, "g150");
    remove(list, "g150");
    CHECK(battle_list_find(list, "g150") == kNoBattle && list->categories[0].battle_count == 99);
    add(list, "fresh", "again", 0, 1, 2);
    CHECK(battle_list_find(list, "fresh") == freed);
    battle_list_rebuild_rows(list);
    CHECK(list->row_count == list->category_count + kCount);
    battle_list_release(list);
    CHECK(list->battles == nullptr && list->rows == nullptr && list->battle_slots == 0);
}

// The navigator applies the battle feeds for the session's area, stops the
// light on battles_done, and readies the Host panel's types from the
// selected planet and the allegiance.
void test_through_navigator() {
    static BoneyardsScreens s;
    g = Recorder{};
    BoneyardsService service = recording_service();
    boneyards_screens_init(&s, &service, SettingsStore{});
    s.host = ScreensHost{nullptr, [](void*, bool running) { g.lights.push_back(running); }};
    SessionRecord session{};
    session.level = SessionLevel::dossier_loaded;
    session.user_id = 1;
    session.allegiance = 'A';
    session.area_id = kSlag;
    boneyards_on_session(&s, &session);
    CHECK(s.allegiance == 'A');
    CHECK(boneyards_open(&s, ByScreen::battle_list) == ByScreen::battle_list);
    battle_list_open(&s.battles, s.session.area_id);
    CHECK(g.calls.size() == 1 && g.calls[0] == "enter 1801" && light_on());
    ServiceEvents events = boneyards_screens_events(&s);
    events.battles_done(events.user, false);
    CHECK(!light_on());
    events.battle_info(events.user, 0, kGame, "test", 4, 1, 2);
    const BattleEntry* b = battle_list_entry(&s.battles, battle_list_find(&s.battles, kGame));
    CHECK(b != nullptr && b->area_id == kSlag && s.battles.categories[3].battle_count == 1);
    events.intel(events.user, nullptr, "Game over");
    const BattleIntel intel{kGame, 1, "kestrel", 0, "Acid Pools", 0, 0, 2, false};
    events.battle_intel(events.user, &intel);
    CHECK(b != nullptr && b->host_user_id == 1);
    const BattleReport report{kGame, 2, 3, "second", 9, 2, 1500, 800, 40, ""};
    events.battle_report(events.user, &report);
    CHECK(b != nullptr && b->report_count == 1);

    // Slag is contested ('I'): an Arm commander may host Galactic War.
    s.galaxy.planet_count = 1;
    s.galaxy.selected = 0;
    s.galaxy.planets[0].side = 'I';
    s.galaxy.planets[0].player_limit = 4;
    s.galaxy.planets[0].commander_death = 3;
    battle_list_on_control(&s.battles, BattleControl::host_button, 0);
    CHECK(s.battles.host_visible && s.setup.visible && s.setup.area_id == kSlag);
    CHECK(s.setup.type_count == 4 && s.setup.types[3].mode == GameMode::galactic_war);
    CHECK(s.setup.planet_players_limit == 4 && s.setup.planet_commander_death == 3);
    battle_list_on_control(&s.battles, BattleControl::host_button, 0);

    // An Arm planet offers its commanders War Training; a Core one does not.
    s.galaxy.planets[0].side = 'A';
    battle_list_on_control(&s.battles, BattleControl::host_button, 0);
    CHECK(s.setup.type_count == 4 && s.setup.types[3].mode == GameMode::war_training);
    battle_list_on_control(&s.battles, BattleControl::host_button, 0);
    s.galaxy.planets[0].side = 'C';
    battle_list_on_control(&s.battles, BattleControl::host_button, 0);
    CHECK(s.setup.type_count == 3);
    battle_list_on_control(&s.battles, BattleControl::host_button, 0);

    // Unaligned commanders get no Galactic War; nor does anyone without a planet.
    session.allegiance = 'U';
    boneyards_on_session(&s, &session);
    s.galaxy.planets[0].side = 'I';
    battle_list_on_control(&s.battles, BattleControl::host_button, 0);
    CHECK(s.setup.type_count == 3);
    battle_list_on_control(&s.battles, BattleControl::host_button, 0);
    session.allegiance = 'C';
    boneyards_on_session(&s, &session);
    s.galaxy.selected = kNoPlanet;
    battle_list_on_control(&s.battles, BattleControl::host_button, 0);
    CHECK(s.setup.type_count == 3 && !s.setup.has_planet);

    battle_list_close(&s.battles, s.session.area_id);
    CHECK(g.calls.size() == 2 && g.calls[1] == "leave 1801" && !s.battles.host_visible);
}

} // namespace

int main() {
    test_categories();
    test_many_battles();
    test_open_and_close();
    test_battle_info();
    test_panel_choice();
    test_shown_battle_changes();
    test_intel_and_report();
    test_through_navigator();
    return by_test::finish("boneyards battle list");
}
