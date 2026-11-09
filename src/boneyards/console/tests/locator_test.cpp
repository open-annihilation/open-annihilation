// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Each locator search: the request it sends, the replies, and the row it
// shows. Server transfer logs out and then logs in, and will not move to the
// server already in use. The console's widgets sit on the locator design.
// Each search's face is then drawn as the game draws it, opened from the
// Main Frame on a profile that does not mount the mod, so the layout and the
// icons come through the client session's reader and the game's picture
// store. With --frames DIR after the mod folder, the faces are written there
// as locator-player.png, locator-planet.png, locator-battle.png,
// locator-frequency.png and locator-server.png, to compare with the design.
#include "oa/boneyards/console/feature_consoles.hpp"
#include "oa/boneyards/console/gui_screen.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/release.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/chat_room.hpp"
#include "oa/boneyards/screens/galaxy.hpp"
#include "oa/boneyards/screens/locator.hpp"

#include "battle_console.hpp"
#include "console_layer.hpp"
#include "console_state.hpp"
#include "widgets.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "oa/boneyards_test/installs.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/png.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {

int g_failures = 0;

void check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr char k_server_detail[] = "The servers this build knows, and any the yard master lists.\n"
                                   "Relocating logs you out of this server and logs you in to\n"
                                   "the one selected, with the same name and password.";

struct Tape {
    int32_t locates = 0;
    char locate_name[64]{};
    LocateCallback locate_callback = nullptr;
    void* locate_context = nullptr;

    uint32_t room_areas[8]{};
    int32_t room_calls = 0;
    int32_t fail_rooms = 0;

    int32_t battle_calls = 0;

    int32_t game_locates = 0;
    char game_name[64]{};
    GameLocateCallback game_callback = nullptr;
    void* game_context = nullptr;
    bool fail_games = false;

    uint32_t census_area = 0;
    int32_t census_calls = 0;

    int32_t joins = 0;
    uint32_t join_area = 0;
    char join_game[64]{};
    const char* join_password = nullptr;

    int32_t switches = 0;
    uint32_t switch_area = 0;
    uint32_t switch_room = 0;
    uint32_t old_area = 0;
    uint32_t old_room = 0;
    char switch_name[64]{};

    int32_t logouts = 0;
    int32_t logins = 0;
    bool login_register = true;
    char login_server[32]{};
    char login_user[32]{};
    char login_password[32]{};

    int32_t pages = 0;

    int32_t intel_calls = 0;
    uint32_t intel_area = 0;
    char intel_game[64]{};

    const char* servers[4]{};
    int32_t server_count = 0;
    const char* current = nullptr;

    int32_t chats = 0;
    uint32_t chat_area = 0;
    uint32_t chat_room = 0;
    char chat_page[128]{};
    int32_t planets_opened = 0;
    uint32_t planet_area = 0;
    int32_t battles_opened = 0;
    uint32_t battle_opened = 0;
    int32_t joins_opened = 0;
    uint32_t join_opened_area = 0;
    char join_opened_game[64]{};
    bool join_needs_password = false;
};

struct Rig {
    Tape tape;
    BoneyardsService service;
    screens::Locator locator;
};

ServiceResult tape_locate(void* user, const char* name, LocateCallback callback, void* context) {
    auto* tape = static_cast<Tape*>(user);
    ++tape->locates;
    std::snprintf(tape->locate_name, sizeof tape->locate_name, "%s", name != nullptr ? name : "");
    tape->locate_callback = callback;
    tape->locate_context = context;
    return ServiceResult::pending;
}

ServiceResult tape_rooms(void* user, uint32_t area_id) {
    auto* tape = static_cast<Tape*>(user);
    if (tape->fail_rooms > 0) {
        --tape->fail_rooms;
        return ServiceResult::failed;
    }
    if (tape->room_calls < 8)
        tape->room_areas[tape->room_calls] = area_id;
    ++tape->room_calls;
    return ServiceResult::pending;
}

ServiceResult tape_battles(void* user, uint32_t) {
    ++static_cast<Tape*>(user)->battle_calls;
    return ServiceResult::pending;
}

ServiceResult
tape_locate_game(void* user, const char* name, GameLocateCallback callback, void* context) {
    auto* tape = static_cast<Tape*>(user);
    ++tape->game_locates;
    std::snprintf(tape->game_name, sizeof tape->game_name, "%s", name != nullptr ? name : "");
    tape->game_callback = callback;
    tape->game_context = context;
    return tape->fail_games ? ServiceResult::failed : ServiceResult::pending;
}

ServiceResult tape_intel(void* user, uint32_t area_id, const char* game_id) {
    auto* tape = static_cast<Tape*>(user);
    ++tape->intel_calls;
    tape->intel_area = area_id;
    std::snprintf(
        tape->intel_game, sizeof tape->intel_game, "%s", game_id != nullptr ? game_id : ""
    );
    return ServiceResult::pending;
}

ServiceResult tape_switch(
    void* user,
    uint32_t area_id,
    uint32_t room_id,
    const char* password,
    uint32_t old_area_id,
    uint32_t old_room_id,
    const char* room_name
) {
    auto* tape = static_cast<Tape*>(user);
    ++tape->switches;
    tape->switch_area = area_id;
    tape->switch_room = room_id;
    tape->old_area = old_area_id;
    tape->old_room = old_room_id;
    std::snprintf(
        tape->switch_name, sizeof tape->switch_name, "%s", room_name != nullptr ? room_name : ""
    );
    CHECK(password == nullptr);
    return ServiceResult::ok;
}

void tape_census(void* user, uint32_t area_id) {
    auto* tape = static_cast<Tape*>(user);
    ++tape->census_calls;
    tape->census_area = area_id;
}

ServiceResult tape_join(void* user, uint32_t area_id, const char* game_id, const char* password) {
    auto* tape = static_cast<Tape*>(user);
    ++tape->joins;
    tape->join_area = area_id;
    tape->join_password = password;
    std::snprintf(tape->join_game, sizeof tape->join_game, "%s", game_id != nullptr ? game_id : "");
    return ServiceResult::pending;
}

void tape_logout(void* user) {
    ++static_cast<Tape*>(user)->logouts;
}

ServiceResult tape_login(void* user, const LoginRequest* request) {
    auto* tape = static_cast<Tape*>(user);
    ++tape->logins;
    tape->login_register = request != nullptr && request->register_new;
    const char* server = request != nullptr ? request->server_name : nullptr;
    const char* name = request != nullptr ? request->user_name : nullptr;
    const char* password = request != nullptr ? request->password : nullptr;
    std::snprintf(
        tape->login_server, sizeof tape->login_server, "%s", server != nullptr ? server : ""
    );
    std::snprintf(tape->login_user, sizeof tape->login_user, "%s", name != nullptr ? name : "");
    std::snprintf(
        tape->login_password, sizeof tape->login_password, "%s", password != nullptr ? password : ""
    );
    return ServiceResult::pending;
}

ServiceResult tape_page(void* user, const char*, const char*) {
    ++static_cast<Tape*>(user)->pages;
    return ServiceResult::ok;
}

void tape_servers(void* user, void (*entry)(void*, const char*), void* context) {
    auto* tape = static_cast<Tape*>(user);
    if (entry == nullptr)
        return;
    for (int32_t i = 0; i < tape->server_count; ++i)
        entry(context, tape->servers[i]);
}

const char* tape_current(void* user) {
    return static_cast<Tape*>(user)->current;
}

void on_chat(void* user, uint32_t area_id, uint32_t room_id, const char* page_line) {
    auto* tape = static_cast<Tape*>(user);
    ++tape->chats;
    tape->chat_area = area_id;
    tape->chat_room = room_id;
    std::snprintf(
        tape->chat_page, sizeof tape->chat_page, "%s", page_line != nullptr ? page_line : ""
    );
}

void on_planet(void* user, uint32_t area_id) {
    auto* tape = static_cast<Tape*>(user);
    ++tape->planets_opened;
    tape->planet_area = area_id;
}

void on_battle_open(void* user, uint32_t area_id) {
    auto* tape = static_cast<Tape*>(user);
    ++tape->battles_opened;
    tape->battle_opened = area_id;
}

void on_join_open(void* user, uint32_t area_id, const char* game_id, bool needs_password) {
    auto* tape = static_cast<Tape*>(user);
    ++tape->joins_opened;
    tape->join_needs_password = needs_password;
    tape->join_opened_area = area_id;
    std::snprintf(
        tape->join_opened_game,
        sizeof tape->join_opened_game,
        "%s",
        game_id != nullptr ? game_id : ""
    );
}

void rig_init(Rig* rig) {
    rig->tape = {};
    rig->service = {};
    rig->service.user = &rig->tape;
    rig->service.locate_user = tape_locate;
    rig->service.request_room_list = tape_rooms;
    rig->service.enter_battles = tape_battles;
    rig->service.locate_game = tape_locate_game;
    rig->service.get_battle = tape_intel;
    rig->service.switch_room = tape_switch;
    rig->service.request_census = tape_census;
    rig->service.join_game = tape_join;
    rig->service.logout = tape_logout;
    rig->service.login = tape_login;
    rig->service.page_user = tape_page;
    rig->service.list_servers = tape_servers;
    rig->service.current_server = tape_current;
    screens::locator_init(&rig->locator, &rig->service);
    rig->locator.user = &rig->tape;
    rig->locator.open_chat = on_chat;
    rig->locator.open_planet = on_planet;
    rig->locator.open_battle = on_battle_open;
    rig->locator.open_join = on_join_open;
}

void fill_name(char* out, std::size_t capacity, const char* text) {
    std::snprintf(out, capacity, "%s", text != nullptr ? text : "");
}

GameLocateReport
game_report(uint32_t area_id, const char* game_id, const char* name, bool forming, bool last) {
    GameLocateReport report{};
    report.area_id = area_id;
    fill_name(report.game_id, sizeof report.game_id, game_id);
    fill_name(report.name, sizeof report.name, name);
    report.type = 1;
    report.forming = forming;
    report.pass_required = false;
    report.seats_known = false;
    report.players = 0;
    report.playerlimit = 0;
    report.last = last;
    return report;
}

void deliver_game(Tape& tape, const GameLocateReport& report) {
    CHECK(tape.game_callback != nullptr);
    if (tape.game_callback != nullptr)
        tape.game_callback(tape.game_context, &report);
}

void pump_times(screens::Locator* locator, int32_t times) {
    for (int32_t i = 0; i < times; ++i)
        screens::locator_pump(locator);
}

void deliver(Tape& tape, const LocateReport& report) {
    CHECK(tape.locate_callback != nullptr);
    if (tape.locate_callback != nullptr)
        tape.locate_callback(tape.locate_context, &report);
}

LocateReport player_report(int32_t status, const char* name) {
    LocateReport report{};
    report.status = status;
    fill_name(report.user_name, sizeof report.user_name, name);
    return report;
}

const screens::LocatorResult* row_at(const screens::Locator* locator, int32_t index) {
    const screens::LocatorResult* row = screens::locator_result(locator, index);
    CHECK(row != nullptr);
    return row;
}

void test_player() {
    Rig rig;
    rig_init(&rig);
    screens::Locator* locator = &rig.locator;
    screens::locator_set_here(locator, 3, 8);
    screens::locator_set_query(locator, "ArmSniper");
    screens::locator_search(locator);
    CHECK(rig.tape.locates == 1);
    CHECK(std::strcmp(rig.tape.locate_name, "ArmSniper") == 0);
    CHECK(screens::locator_result_count(locator) == 0);

    screens::locator_set_query(locator, "ArmSniper_II");
    screens::locator_search(locator);
    CHECK(rig.tape.locates == 2);
    CHECK(std::strcmp(rig.tape.locate_name, "ArmSniper_II") == 0);

    LocateReport examining =
        player_report(static_cast<int32_t>(LocateStatus::examining_battles), "ArmSniper_II");
    fill_name(examining.area_name, sizeof examining.area_name, "Altair");
    examining.area_id = 7;
    deliver(rig.tape, examining);
    const screens::LocatorResult* shown = row_at(locator, 0);
    CHECK(screens::locator_result_count(locator) == 1);
    CHECK(shown != nullptr && std::strcmp(shown->name, "ArmSniper_II") == 0);
    CHECK(shown != nullptr && std::strcmp(shown->where, "Altair") == 0);
    CHECK(shown != nullptr && std::strcmp(shown->status, "examining battles") == 0);
    CHECK(shown != nullptr && !shown->bright);
    CHECK(
        shown != nullptr &&
        std::strcmp(
            shown->detail, "ArmSniper_II is currently examining battles on Planet Altair."
        ) == 0
    );
    CHECK(!screens::locator_relocate_enabled(locator));
    CHECK(screens::locator_extra_enabled(locator));

    LocateReport chatting =
        player_report(static_cast<int32_t>(LocateStatus::chatting), "ArmSniper");
    fill_name(chatting.area_name, sizeof chatting.area_name, "Fuel Dump");
    fill_name(chatting.room_name, sizeof chatting.room_name, "Tactics");
    chatting.area_id = 4;
    chatting.room_id = 9;
    deliver(rig.tape, chatting);
    shown = row_at(locator, 0);
    CHECK(screens::locator_result_count(locator) == 1);
    CHECK(shown != nullptr && std::strcmp(shown->name, "ArmSniper") == 0);
    CHECK(shown != nullptr && std::strcmp(shown->where, "Fuel Dump / Tactics") == 0);
    CHECK(shown != nullptr && std::strcmp(shown->status, "chatting") == 0);
    CHECK(shown != nullptr && shown->bright);
    CHECK(shown != nullptr && std::strcmp(shown->game_id, "Tactics") == 0);
    CHECK(shown != nullptr && shown->area_id == 4 && shown->room_id == 9);
    CHECK(
        shown != nullptr &&
        std::strcmp(
            shown->detail, "ArmSniper is currently chatting on Planet Fuel Dump Frequency Tactics."
        ) == 0
    );

    screens::locator_extra(locator);
    CHECK(rig.tape.pages == 0);
    CHECK(rig.tape.switches == 0);
    CHECK(rig.tape.chats == 1);
    CHECK(rig.tape.chat_area == 0 && rig.tape.chat_room == 0);
    CHECK(std::strcmp(rig.tape.chat_page, "+page ArmSniper ") == 0);

    CHECK(screens::locator_relocate_enabled(locator));
    screens::locator_relocate(locator);
    CHECK(rig.tape.switches == 1);
    CHECK(rig.tape.switch_area == 4 && rig.tape.switch_room == 9);
    CHECK(rig.tape.old_area == 3 && rig.tape.old_room == 8);
    CHECK(std::strcmp(rig.tape.switch_name, "Tactics") == 0);
    CHECK(rig.tape.chats == 2);
    CHECK(rig.tape.chat_area == 4 && rig.tape.chat_room == 9);
    CHECK(std::strcmp(rig.tape.chat_page, "") == 0);

    struct StatusCase {
        int32_t status;
        const char* where;
        const char* short_status;
        const char* detail;
        bool bright;
    };

    const StatusCase cases[] = {
        {static_cast<int32_t>(LocateStatus::in_battle),
         "Altair",
         "engaged in battle",
         "ArmSniper is currently engaged in Battle Rumble on Planet Altair.",
         false},
        {static_cast<int32_t>(LocateStatus::other_server),
         "Yard Beta",
         "other server",
         "ArmSniper is currently on Server Yard Beta.",
         false},
        {static_cast<int32_t>(LocateStatus::logged_out),
         "",
         "not logged in",
         "ArmSniper is not currently logged in.",
         false},
        {static_cast<int32_t>(LocateStatus::no_such_user),
         "",
         "no such player",
         "ArmSniper does not exist.  Check spelling and try again.",
         false},
        {static_cast<int32_t>(LocateStatus::viewing_pages),
         "",
         "viewing pages",
         "ArmSniper is currently viewing the Map, News, Help, Ladders, or Recon.",
         false},
        {99,
         "",
         "locator error",
         "There has been a locator system error.  Please try again later.",
         false},
    };
    for (const StatusCase& one : cases) {
        screens::locator_set_kind(locator, screens::locator_kind::player);
        screens::locator_set_query(locator, "ArmSniper");
        screens::locator_search(locator);
        LocateReport report = player_report(one.status, "ArmSniper");
        if (one.status == static_cast<int32_t>(LocateStatus::in_battle)) {
            fill_name(report.area_name, sizeof report.area_name, "Altair");
            fill_name(report.game_name, sizeof report.game_name, "Rumble");
        } else if (one.status == static_cast<int32_t>(LocateStatus::other_server)) {
            fill_name(report.server_name, sizeof report.server_name, "Yard Beta");
        }
        deliver(rig.tape, report);
        shown = row_at(locator, 0);
        CHECK(shown != nullptr && std::strcmp(shown->where, one.where) == 0);
        CHECK(shown != nullptr && std::strcmp(shown->status, one.short_status) == 0);
        CHECK(shown != nullptr && std::strcmp(shown->detail, one.detail) == 0);
        CHECK(shown != nullptr && shown->bright == one.bright);
    }

    screens::locator_set_kind(locator, screens::locator_kind::planet);
    const int32_t locates = rig.tape.locates;
    deliver(rig.tape, chatting);
    CHECK(screens::locator_result_count(locator) == 0);
    CHECK(rig.tape.locates == locates);

    // No name: the server answers no locate, so none is sent.
    screens::locator_set_kind(locator, screens::locator_kind::player);
    screens::locator_set_query(locator, "");
    screens::locator_search(locator);
    CHECK(rig.tape.locates == locates);
    CHECK(screens::locator_result_count(locator) == 0);
}

void test_planet() {
    Rig rig;
    rig_init(&rig);
    screens::LocatorPlanet planets[3]{};
    planets[0].id = 7;
    fill_name(planets[0].name, sizeof planets[0].name, "Altair");
    planets[0].side = 'C';
    fill_name(planets[0].ta_map, sizeof planets[0].ta_map, "Biggie Biggs");
    planets[0].map_source = static_cast<int32_t>(screens::MapSource::core_contingency);
    planets[0].in_progress = 1;
    planets[0].forming = 2;
    planets[1].id = 4;
    fill_name(planets[1].name, sizeof planets[1].name, "Fuel Dump");
    planets[1].side = 'a';
    fill_name(planets[1].ta_map, sizeof planets[1].ta_map, "Metal");
    planets[1].map_source = static_cast<int32_t>(screens::MapSource::ta_disk);
    planets[2].id = 9;
    fill_name(planets[2].name, sizeof planets[2].name, "Rim");
    planets[2].side = 'I';
    fill_name(planets[2].ta_map, sizeof planets[2].ta_map, "Dust");
    planets[2].map_source = static_cast<int32_t>(screens::MapSource::web_ta);

    screens::Locator* locator = &rig.locator;
    screens::locator_set_planets(locator, planets, 3);
    screens::locator_set_kind(locator, screens::locator_kind::planet);
    screens::locator_set_query(locator, "altair");
    const int32_t calls = rig.tape.locates + rig.tape.room_calls + rig.tape.battle_calls;
    screens::locator_search(locator);
    CHECK(rig.tape.locates + rig.tape.room_calls + rig.tape.battle_calls == calls);
    CHECK(screens::locator_result_count(locator) == 1);
    const screens::LocatorResult* shown = row_at(locator, 0);
    CHECK(shown != nullptr && std::strcmp(shown->name, "Altair") == 0);
    CHECK(shown != nullptr && std::strcmp(shown->where, "Core") == 0);
    CHECK(shown != nullptr && std::strcmp(shown->status, "1 battles, 2 forming") == 0);
    CHECK(shown != nullptr && shown->bright);
    CHECK(
        shown != nullptr &&
        std::strcmp(
            shown->detail, "Altair is held by the Core. TA map Biggie Biggs (Core Contingency)."
        ) == 0
    );
    CHECK(screens::locator_selected(locator) == 0);
    CHECK(screens::locator_relocate_enabled(locator));
    CHECK(!screens::locator_extra_enabled(locator));
    screens::locator_relocate(locator);
    CHECK(rig.tape.census_calls == 1 && rig.tape.census_area == 7);
    CHECK(rig.tape.planets_opened == 1 && rig.tape.planet_area == 7);
    CHECK(rig.tape.joins == 0 && rig.tape.switches == 0);

    screens::locator_set_query(locator, "");
    screens::locator_search(locator);
    CHECK(screens::locator_result_count(locator) == 3);
    shown = row_at(locator, 1);
    CHECK(shown != nullptr && std::strcmp(shown->name, "Fuel Dump") == 0);
    CHECK(shown != nullptr && std::strcmp(shown->where, "Arm") == 0);
    shown = row_at(locator, 2);
    CHECK(shown != nullptr && std::strcmp(shown->where, "Independent") == 0);
    CHECK(
        shown != nullptr &&
        std::strcmp(
            shown->detail, "Rim is held by the Independent. TA map Dust (a downloaded map)."
        ) == 0
    );
    CHECK(rig.tape.census_calls == 1);
}

void test_battle() {
    Rig rig;
    rig_init(&rig);
    screens::LocatorPlanet planets[2]{};
    planets[0].id = 4;
    fill_name(planets[0].name, sizeof planets[0].name, "Fuel Dump");
    planets[1].id = 7;
    fill_name(planets[1].name, sizeof planets[1].name, "Altair");
    screens::Locator* locator = &rig.locator;
    screens::locator_set_planets(locator, planets, 2);
    screens::locator_set_kind(locator, screens::locator_kind::battle);
    CHECK(screens::locator_shows_join(screens::locator_kind::battle));

    // One game locate searches every planet; no battle list is entered.
    screens::locator_set_query(locator, "Rumble");
    screens::locator_search(locator);
    CHECK(rig.tape.game_locates == 1);
    CHECK(std::strcmp(rig.tape.game_name, "Rumble") == 0);
    CHECK(rig.tape.battle_calls == 0 && rig.tape.room_calls == 0);
    CHECK(screens::locator_listening_battles(locator));
    CHECK(screens::locator_result_count(locator) == 0);

    deliver_game(rig.tape, game_report(4, "g1", "Rumble", true, false));
    CHECK(screens::locator_listening_battles(locator));
    CHECK(screens::locator_result_count(locator) == 1);
    CHECK(rig.tape.intel_calls == 0);
    const screens::LocatorResult* shown = row_at(locator, 0);
    CHECK(shown != nullptr && std::strcmp(shown->name, "Rumble") == 0);
    CHECK(shown != nullptr && std::strcmp(shown->where, "Fuel Dump") == 0);
    CHECK(shown != nullptr && std::strcmp(shown->status, "forming") == 0);
    CHECK(shown != nullptr && shown->bright);
    CHECK(shown != nullptr && shown->area_id == 4 && std::strcmp(shown->game_id, "g1") == 0);
    CHECK(shown != nullptr && std::strcmp(shown->detail, "Rumble on Fuel Dump.") == 0);

    GameLocateReport rubble = game_report(7, "g2", "Rumble in the Rubble", false, true);
    rubble.pass_required = true;
    deliver_game(rig.tape, rubble);
    CHECK(!screens::locator_listening_battles(locator));
    CHECK(screens::locator_result_count(locator) == 2);
    CHECK(screens::locator_selected(locator) == 0);
    CHECK(rig.tape.intel_calls == 1);
    CHECK(rig.tape.intel_area == 4);
    CHECK(std::strcmp(rig.tape.intel_game, "g1") == 0);
    CHECK(std::strcmp(screens::locator_detail(locator), "Rumble on Fuel Dump.") == 0);

    screens::locator_on_intel(locator, "g1", "Kbot_Kid", 1, false);
    CHECK(
        std::strcmp(
            screens::locator_detail(locator), "Rumble: Duel, Seasoned, hosted by Kbot_Kid."
        ) == 0
    );
    CHECK(!screens::locator_awaiting_intel(locator));

    screens::locator_select(locator, 1);
    shown = row_at(locator, 1);
    CHECK(shown != nullptr && std::strcmp(shown->status, "in progress") == 0);
    CHECK(shown != nullptr && !shown->bright);
    CHECK(shown != nullptr && shown->password);
    CHECK(shown != nullptr && std::strcmp(shown->where, "Altair") == 0);
    CHECK(rig.tape.intel_calls == 2);
    CHECK(std::strcmp(rig.tape.intel_game, "g2") == 0);
    CHECK(std::strcmp(screens::locator_detail(locator), "Rumble in the Rubble on Altair.") == 0);
    screens::locator_on_intel(locator, "g2", "Kbot_Kid", 1, true);
    CHECK(std::strcmp(screens::locator_detail(locator), "Rumble in the Rubble on Altair.") == 0);
    screens::locator_select(locator, 1);
    screens::locator_on_intel(locator, "other", "Kbot_Kid", 1, false);
    CHECK(std::strcmp(screens::locator_detail(locator), "Rumble in the Rubble on Altair.") == 0);

    screens::locator_relocate(locator);
    CHECK(rig.tape.joins == 0);
    CHECK(rig.tape.battles_opened == 1 && rig.tape.battle_opened == 7);
    CHECK(screens::locator_extra_enabled(locator));
    screens::locator_extra(locator);
    CHECK(rig.tape.joins == 0);
    CHECK(rig.tape.joins_opened == 1 && rig.tape.join_needs_password);
    CHECK(rig.tape.join_opened_area == 7);
    CHECK(std::strcmp(rig.tape.join_opened_game, "g2") == 0);

    // An answer after the search ended, or for another kind, is ignored.
    deliver_game(rig.tape, game_report(4, "g3", "Rumble Late", true, true));
    CHECK(screens::locator_result_count(locator) == 2);

    // Nothing matched: one closing answer with no game id.
    screens::locator_set_query(locator, "Nothing");
    screens::locator_search(locator);
    CHECK(rig.tape.game_locates == 2);
    deliver_game(rig.tape, game_report(0, "", "", false, true));
    CHECK(!screens::locator_listening_battles(locator));
    CHECK(screens::locator_result_count(locator) == 0);
    CHECK(screens::locator_selected(locator) == -1);

    // A planet the war map does not hold has no WHERE.
    screens::locator_set_query(locator, "Far");
    screens::locator_search(locator);
    deliver_game(rig.tape, game_report(99, "far", "Far Away", true, true));
    shown = row_at(locator, 0);
    CHECK(shown != nullptr && shown->where[0] == '\0');
    CHECK(shown != nullptr && std::strcmp(shown->detail, "Far Away.") == 0);

    // No name: the server answers no game locate, so none is sent.
    screens::locator_set_query(locator, "");
    screens::locator_search(locator);
    CHECK(rig.tape.game_locates == 3);
    CHECK(!screens::locator_listening_battles(locator));
    CHECK(screens::locator_result_count(locator) == 0);

    // No answer at all: the search ends after its wait and shows nothing.
    screens::locator_set_query(locator, "Rumble");
    screens::locator_search(locator);
    CHECK(rig.tape.game_locates == 4);
    pump_times(locator, screens::kLocatorAnswerWaitPulses - 1);
    CHECK(screens::locator_listening_battles(locator));
    screens::locator_pump(locator);
    CHECK(!screens::locator_listening_battles(locator));
    CHECK(screens::locator_result_count(locator) == 0);

    // A search the service refuses does not wait.
    rig.tape.fail_games = true;
    screens::locator_search(locator);
    CHECK(rig.tape.game_locates == 5);
    CHECK(!screens::locator_listening_battles(locator));
    rig.tape.fail_games = false;

    // Changing the search drops the one still answering.
    screens::locator_search(locator);
    screens::locator_set_kind(locator, screens::locator_kind::planet);
    CHECK(!screens::locator_listening_battles(locator));
    deliver_game(rig.tape, game_report(4, "g1", "Rumble", true, true));
    CHECK(screens::locator_result_count(locator) == 0);

    // Both seat fields show n/m. One field, or a battle in progress, does not.
    screens::locator_set_kind(locator, screens::locator_kind::battle);
    screens::locator_set_query(locator, "Seat");
    screens::locator_search(locator);
    GameLocateReport seated = game_report(4, "seat", "Seated", true, false);
    seated.seats_known = true;
    seated.players = 2;
    seated.playerlimit = 4;
    deliver_game(rig.tape, seated);
    GameLocateReport partial = game_report(7, "part", "Partial", true, false);
    partial.players = 1;
    deliver_game(rig.tape, partial);
    GameLocateReport playing = game_report(4, "play", "Playing", false, true);
    playing.seats_known = true;
    playing.players = 0;
    playing.playerlimit = 0;
    deliver_game(rig.tape, playing);
    shown = row_at(locator, 0);
    CHECK(shown != nullptr && std::strcmp(shown->status, "forming 2/4") == 0);
    CHECK(shown != nullptr && shown->bright);
    shown = row_at(locator, 1);
    CHECK(shown != nullptr && std::strcmp(shown->status, "forming") == 0);
    shown = row_at(locator, 2);
    CHECK(shown != nullptr && std::strcmp(shown->status, "in progress") == 0);
    CHECK(shown != nullptr && !shown->bright);

    // An open battle still joins at once.
    screens::locator_select(locator, 1);
    const int32_t joins = rig.tape.joins;
    screens::locator_extra(locator);
    CHECK(rig.tape.joins == joins + 1);
    CHECK(std::strcmp(rig.tape.join_game, "part") == 0);
    CHECK(rig.tape.join_password == nullptr);
    CHECK(!rig.tape.join_needs_password);
}

void test_frequency() {
    Rig rig;
    rig_init(&rig);
    screens::LocatorPlanet planets[2]{};
    planets[0].id = 1;
    fill_name(planets[0].name, sizeof planets[0].name, "Nowhere");
    planets[1].id = 4;
    fill_name(planets[1].name, sizeof planets[1].name, "Fuel Dump");
    screens::Locator* locator = &rig.locator;
    screens::locator_set_planets(locator, planets, 2);
    screens::locator_set_here(locator, 4, 2);
    screens::locator_set_kind(locator, screens::locator_kind::frequency);
    screens::locator_set_query(locator, "Tactics");
    screens::locator_search(locator);
    CHECK(rig.tape.room_calls == 1 && rig.tape.room_areas[0] == 1);
    screens::locator_on_room(
        locator, 0, 1, 3, "Lobby", false, 1, screens::room_type::planetary_default, true
    );
    CHECK(rig.tape.room_calls == 2 && rig.tape.room_areas[1] == 4);
    CHECK(screens::locator_result_count(locator) == 0);
    screens::locator_on_room(
        locator, 0, 4, 12, "Tactics", false, 3, screens::room_type::user_created, false
    );
    screens::locator_on_room(
        locator, 0, 4, 13, "Briefing", true, 1, screens::room_type::battle_debriefing, true
    );
    CHECK(!screens::locator_listening_rooms(locator));
    CHECK(screens::locator_result_count(locator) == 1);
    const screens::LocatorResult* shown = row_at(locator, 0);
    CHECK(shown != nullptr && std::strcmp(shown->name, "Tactics") == 0);
    CHECK(shown != nullptr && std::strcmp(shown->where, "Fuel Dump") == 0);
    CHECK(shown != nullptr && std::strcmp(shown->status, "3 commanders") == 0);
    CHECK(
        shown != nullptr &&
        std::strcmp(shown->detail, "Tactics on Fuel Dump: user-created, no password.") == 0
    );
    CHECK(screens::locator_relocate_enabled(locator));
    CHECK(!screens::locator_extra_enabled(locator));
    screens::locator_relocate(locator);
    CHECK(rig.tape.switches == 1);
    CHECK(rig.tape.switch_area == 4 && rig.tape.switch_room == 12);
    CHECK(rig.tape.old_area == 4 && rig.tape.old_room == 2);
    CHECK(std::strcmp(rig.tape.switch_name, "Tactics") == 0);
    CHECK(rig.tape.chats == 1);
    CHECK(rig.tape.chat_area == 4 && rig.tape.chat_room == 12);
    CHECK(std::strcmp(rig.tape.chat_page, "") == 0);

    screens::locator_set_query(locator, "");
    screens::locator_search(locator);
    screens::locator_on_room(
        locator, 0, 99, 1, "Foreign", false, 1, screens::room_type::user_created, true
    );
    CHECK(screens::locator_listening_rooms(locator));
    CHECK(rig.tape.room_calls == 3);
    screens::locator_on_room(
        locator, 0, 1, 8, "Briefing", true, 2, screens::room_type::battle_debriefing, true
    );
    CHECK(screens::locator_result_count(locator) == 1);
    shown = row_at(locator, 0);
    CHECK(
        shown != nullptr &&
        std::strcmp(shown->detail, "Briefing on Nowhere: system, password required.") == 0
    );

    // A planet's rows can take many pulses to come back over the network:
    // the search waits for its last row and does not ask the next planet.
    rig_init(&rig);
    locator = &rig.locator;
    screens::locator_set_planets(locator, planets, 2);
    screens::locator_set_kind(locator, screens::locator_kind::frequency);
    screens::locator_search(locator);
    CHECK(screens::locator_listening_rooms(locator));
    pump_times(locator, screens::kLocatorAnswerWaitPulses - 1);
    CHECK(rig.tape.room_calls == 1);
    screens::locator_on_room(
        locator, 0, 1, 5, "Late", false, 1, screens::room_type::user_created, false
    );
    CHECK(screens::locator_result_count(locator) == 1);
    // A row starts the wait again.
    pump_times(locator, screens::kLocatorAnswerWaitPulses - 1);
    CHECK(rig.tape.room_calls == 1);
    screens::locator_on_game_located(locator, nullptr);
    CHECK(screens::locator_listening_rooms(locator));
    screens::locator_on_room(
        locator, 0, 1, 6, "Later", false, 1, screens::room_type::user_created, true
    );
    CHECK(rig.tape.room_calls == 2 && rig.tape.room_areas[1] == 4);

    rig_init(&rig);
    locator = &rig.locator;
    screens::locator_set_planets(locator, planets, 2);
    rig.tape.fail_rooms = 1;
    screens::locator_set_kind(locator, screens::locator_kind::frequency);
    screens::locator_search(locator);
    CHECK(rig.tape.room_calls == 1 && rig.tape.room_areas[0] == 4);

    // A planet that never answers is passed over after the wait.
    rig_init(&rig);
    locator = &rig.locator;
    screens::locator_set_planets(locator, planets, 1);
    screens::locator_set_kind(locator, screens::locator_kind::frequency);
    screens::locator_search(locator);
    pump_times(locator, screens::kLocatorAnswerWaitPulses - 1);
    CHECK(screens::locator_listening_rooms(locator));
    CHECK(rig.tape.room_calls == 1);
    screens::locator_pump(locator);
    CHECK(!screens::locator_listening_rooms(locator));
    CHECK(screens::locator_result_count(locator) == 0);
    CHECK(rig.tape.room_calls == 1);
}

// A frequency every planet lists (the campaign's Sector Chat) is one row,
// entered from the planet the player is on. Each planet's own frequency of
// the same id but its own name keeps its row.
void test_shared_frequency() {
    Rig rig;
    rig_init(&rig);
    screens::LocatorPlanet planets[3]{};
    planets[0].id = 1;
    fill_name(planets[0].name, sizeof planets[0].name, "Altair");
    planets[1].id = 2;
    fill_name(planets[1].name, sizeof planets[1].name, "Fuel Dump");
    planets[2].id = 3;
    fill_name(planets[2].name, sizeof planets[2].name, "Rim");
    screens::Locator* locator = &rig.locator;
    screens::locator_set_planets(locator, planets, 3);
    screens::locator_set_here(locator, 2, 1);
    screens::locator_set_kind(locator, screens::locator_kind::frequency);
    screens::locator_set_query(locator, "");
    screens::locator_search(locator);
    for (uint32_t area = 1; area <= 3; ++area) {
        screens::locator_on_room(
            locator,
            0,
            area,
            60001,
            "Sector Chat",
            false,
            5,
            screens::room_type::user_created,
            false
        );
        char own[32];
        std::snprintf(own, sizeof own, "P1-%s", planets[area - 1].name);
        screens::locator_on_room(
            locator, 0, area, 1, own, false, 1, screens::room_type::planetary_default, true
        );
    }
    CHECK(!screens::locator_listening_rooms(locator));
    CHECK(rig.tape.room_calls == 3);
    CHECK(screens::locator_result_count(locator) == 4);
    const screens::LocatorResult* shared = row_at(locator, 0);
    CHECK(shared != nullptr && std::strcmp(shared->name, "Sector Chat") == 0);
    CHECK(shared != nullptr && shared->every_planet);
    CHECK(shared != nullptr && std::strcmp(shared->where, screens::kLocatorEveryPlanet) == 0);
    CHECK(
        shared != nullptr &&
        std::strcmp(shared->detail, "Sector Chat on every planet: user-created, no password.") == 0
    );
    const screens::LocatorResult* own = row_at(locator, 3);
    CHECK(own != nullptr && std::strcmp(own->name, "P1-Rim") == 0);
    CHECK(own != nullptr && !own->every_planet && std::strcmp(own->where, "Rim") == 0);
    CHECK(screens::locator_selected(locator) == 0);
    screens::locator_relocate(locator);
    CHECK(rig.tape.switches == 1);
    CHECK(rig.tape.switch_area == 2 && rig.tape.switch_room == 60001);
    CHECK(rig.tape.chat_area == 2 && rig.tape.chat_room == 60001);

    // Off any planet, it is entered from the first planet that listed it.
    screens::locator_set_here(locator, 0, 0);
    screens::locator_relocate(locator);
    CHECK(rig.tape.switches == 2);
    CHECK(rig.tape.switch_area == 1 && rig.tape.switch_room == 60001);

    // A search for it finds the one row.
    screens::locator_set_query(locator, "sector");
    screens::locator_search(locator);
    for (uint32_t area = 1; area <= 3; ++area) {
        screens::locator_on_room(
            locator,
            0,
            area,
            60001,
            "Sector Chat",
            false,
            5,
            screens::room_type::user_created,
            false
        );
        screens::locator_on_room(
            locator, 0, area, 1, "P1-Own", false, 1, screens::room_type::planetary_default, true
        );
    }
    CHECK(screens::locator_result_count(locator) == 1);
}

// global 1 is every planet on the first sighting. A stated value other than
// 1 is not folded, even when the id and name match. Omitting global keeps
// the fold. The value travels with the call and is not kept afterwards.
void test_stated_frequency() {
    Rig rig;
    rig_init(&rig);
    screens::LocatorPlanet planets[2]{};
    planets[0].id = 1;
    fill_name(planets[0].name, sizeof planets[0].name, "Altair");
    planets[1].id = 2;
    fill_name(planets[1].name, sizeof planets[1].name, "Fuel Dump");
    screens::Locator* locator = &rig.locator;
    screens::locator_set_planets(locator, planets, 2);
    screens::locator_set_kind(locator, screens::locator_kind::frequency);
    screens::locator_set_query(locator, "Rally");
    screens::locator_search(locator);
    screens::locator_on_room(
        locator, 0, 1, 60002, "Rally", false, 4, screens::room_type::user_created, true, true, 1
    );
    CHECK(screens::locator_result_count(locator) == 1);
    const screens::LocatorResult* rally = row_at(locator, 0);
    CHECK(rally != nullptr && rally->every_planet);
    CHECK(rally != nullptr && std::strcmp(rally->where, screens::kLocatorEveryPlanet) == 0);

    screens::locator_set_query(locator, "Local");
    screens::locator_search(locator);
    // The search asks one planet at a time and takes only that planet's
    // rooms. Altair's list has to end before Fuel Dump is asked. A stated
    // global of 0 is not the shared frequency, so each planet keeps a row.
    screens::locator_on_room(
        locator, 0, 1, 9, "Local", false, 2, screens::room_type::user_created, true, true, 0
    );
    screens::locator_on_room(
        locator, 0, 2, 9, "Local", false, 3, screens::room_type::user_created, true, true, 0
    );
    CHECK(screens::locator_result_count(locator) == 2);
    const screens::LocatorResult* first = row_at(locator, 0);
    const screens::LocatorResult* second = row_at(locator, 1);
    CHECK(first != nullptr && !first->every_planet && std::strcmp(first->where, "Altair") == 0);
    CHECK(
        second != nullptr && !second->every_planet && std::strcmp(second->where, "Fuel Dump") == 0
    );
}

const char* region_of(void*, const char* location) {
    if (location != nullptr && std::strcmp(location, "CorePrime Alpha") == 0)
        return "US East";
    return nullptr;
}

void test_server_region() {
    Rig rig;
    rig_init(&rig);
    rig.service.server_region = region_of;
    rig.tape.servers[0] = "CorePrime Alpha";
    rig.tape.servers[1] = "Older Yard";
    rig.tape.server_count = 2;
    rig.tape.current = "CorePrime Alpha";
    screens::Locator* locator = &rig.locator;
    screens::locator_set_kind(locator, screens::locator_kind::server);
    const screens::LocatorResult* alpha = row_at(locator, 0);
    const screens::LocatorResult* older = row_at(locator, 1);
    CHECK(alpha != nullptr && std::strcmp(alpha->where, "US East") == 0);
    CHECK(older != nullptr && older->where[0] == '\0');
}

void test_server() {
    Rig rig;
    rig_init(&rig);
    rig.tape.servers[0] = "CorePrime Alpha";
    rig.tape.servers[1] = "CorePrime Alpha Two";
    rig.tape.servers[2] = "Yard Beta";
    rig.tape.server_count = 3;
    rig.tape.current = "CorePrime Alpha";
    screens::Locator* locator = &rig.locator;
    screens::locator_set_identity(locator, "Pilot", "secret");
    screens::locator_set_kind(locator, screens::locator_kind::server);
    CHECK(!screens::locator_shows_field(screens::locator_kind::server));
    CHECK(screens::locator_field_label(screens::locator_kind::server) == nullptr);
    CHECK(screens::locator_result_count(locator) == 3);
    CHECK(screens::locator_selected(locator) == 0);
    const screens::LocatorResult* current = row_at(locator, 0);
    CHECK(current != nullptr && std::strcmp(current->name, "CorePrime Alpha") == 0);
    CHECK(current != nullptr && current->where[0] == '\0');
    CHECK(current != nullptr && std::strcmp(current->status, "current server") == 0);
    CHECK(current != nullptr && std::strcmp(current->detail, k_server_detail) == 0);
    const screens::LocatorResult* near = row_at(locator, 1);
    CHECK(near != nullptr && std::strcmp(near->name, "CorePrime Alpha Two") == 0);
    CHECK(near != nullptr && near->status[0] == '\0' && near->where[0] == '\0');
    const screens::LocatorResult* other = row_at(locator, 2);
    CHECK(other != nullptr && std::strcmp(other->name, "Yard Beta") == 0);
    CHECK(other != nullptr && other->where[0] == '\0' && other->status[0] == '\0');
    CHECK(std::strcmp(other->detail, k_server_detail) == 0);
    CHECK(!screens::locator_relocate_enabled(locator));
    CHECK(!screens::locator_extra_enabled(locator));
    screens::locator_relocate(locator);
    CHECK(rig.tape.logouts == 0 && rig.tape.logins == 0);

    screens::locator_select(locator, 2);
    CHECK(screens::locator_relocate_enabled(locator));
    screens::locator_relocate(locator);
    CHECK(rig.tape.logouts == 1);
    CHECK(rig.tape.logins == 0);
    CHECK(!screens::locator_relocate_enabled(locator));
    screens::locator_on_login_reset(locator);
    CHECK(rig.tape.logins == 1);
    CHECK(!rig.tape.login_register);
    CHECK(std::strcmp(rig.tape.login_server, "Yard Beta") == 0);
    CHECK(std::strcmp(rig.tape.login_user, "Pilot") == 0);
    CHECK(std::strcmp(rig.tape.login_password, "secret") == 0);
    CHECK(screens::locator_relocate_enabled(locator));
    screens::locator_on_login_reset(locator);
    CHECK(rig.tape.logins == 1);

    rig.service.logout = nullptr;
    screens::locator_relocate(locator);
    CHECK(rig.tape.logouts == 1);
    CHECK(locator->transfer_pending);
}

struct Placed {
    const char* name;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

constexpr Placed k_placed[] = {
    {"Locator", 23, 126, 498, 313},  {"Title", 23, 132, 124, 16},
    {"icon_0", 26, 150, 11, 11},     {"icon_1", 26, 167, 11, 11},
    {"icon_2", 26, 184, 11, 11},     {"icon_3", 26, 201, 11, 11},
    {"icon_4", 26, 218, 11, 11},     {"row_0", 23, 147, 107, 17},
    {"row_1", 23, 164, 107, 17},     {"row_2", 23, 181, 107, 17},
    {"row_3", 23, 198, 107, 17},     {"row_4", 23, 215, 107, 17},
    {"kind_0", 40, 151, 100, 12},    {"kind_1", 40, 168, 100, 12},
    {"kind_2", 40, 185, 100, 12},    {"kind_3", 40, 202, 100, 12},
    {"kind_4", 40, 219, 100, 12},    {"Help0", 29, 248, 168, 12},
    {"Help1", 29, 260, 168, 12},     {"Help2", 29, 272, 168, 12},
    {"Help3", 29, 284, 168, 12},     {"Field", 150, 140, 80, 16},
    {"NameBox", 236, 136, 150, 21},  {"Name", 236, 136, 150, 21},
    {"Search", 396, 136, 99, 21},    {"Panel", 158, 166, 350, 150},
    {"HdrName", 166, 170, 40, 12},   {"HdrWhere", 300, 170, 80, 12},
    {"HdrStatus", 430, 170, 70, 12}, {"Rule", 159, 182, 348, 1},
    {"Results", 159, 184, 348, 130}, {"Divider", 146, 129, 1, 307},
    {"Detail0", 160, 324, 340, 12},  {"Detail1", 160, 336, 340, 12},
    {"Detail2", 160, 348, 340, 12},  {"Relocate", 160, 392, 99, 21},
    {"Page", 270, 392, 99, 21},
};

void expect_box(
    const char* name,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    int32_t ax,
    int32_t ay,
    int32_t aw,
    int32_t ah
) {
    const bool match = x == ax && y == ay && width == aw && height == ah;
    if (!match) {
        std::fprintf(
            stderr,
            "%s at %d,%d %dx%d, expected %d,%d %dx%d\n",
            name,
            x,
            y,
            width,
            height,
            ax,
            ay,
            aw,
            ah
        );
    }
    CHECK(match);
}

void expect_control(const GuiScreen* screen, const Placed& placed) {
    const GuiControl* control = gui_screen_find(screen, placed.name);
    CHECK(control != nullptr);
    if (control == nullptr) {
        std::fprintf(stderr, "missing control %s\n", placed.name);
        return;
    }
    expect_box(
        placed.name,
        control->x,
        control->y,
        control->width,
        control->height,
        placed.x,
        placed.y,
        placed.width,
        placed.height
    );
}

void expect_child(Window* root, const Placed& placed) {
    Window* window = window_find_named(root, placed.name, nullptr);
    CHECK(window != nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "missing window %s\n", placed.name);
        return;
    }
    expect_box(
        placed.name,
        window->x,
        window->y,
        window->width,
        window->height,
        placed.x,
        placed.y,
        placed.width,
        placed.height
    );
}

const char* caption_of(Window* window) {
    if (window == nullptr || window->ops == nullptr || window->ops->state_text == nullptr)
        return "";
    const char* text = window->ops->state_text(window, -1);
    return text != nullptr ? text : "";
}

struct FileSource {
    oa::HpiArchive* archive = nullptr;
    std::filesystem::path fixtures;
};

bool read_disk(const std::filesystem::path& path, std::vector<uint8_t>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    if (length < 0)
        return false;
    in.seekg(0);
    out->resize(static_cast<std::size_t>(length));
    if (length > 0)
        in.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(length));
    return static_cast<bool>(in);
}

bool read_source(void* context, const char* path, std::vector<uint8_t>* out) {
    if (context == nullptr || path == nullptr || out == nullptr)
        return false;
    auto* source = static_cast<FileSource*>(context);
    const std::filesystem::path on_disk = source->fixtures / path;
    std::error_code error;
    if (std::filesystem::is_regular_file(on_disk, error))
        return read_disk(on_disk, out);
    oa::base::bytes::Decoded<std::vector<uint8_t>> read = source->archive->read(path);
    if (!read.ok() || !read.value.has_value())
        return false;
    *out = std::move(*read.value);
    return true;
}

struct InstalledAssets {
    oa::AssetStore* assets = nullptr;
    oa::Palette palette{};
};

uint8_t* read_asset(void* user, const char* path, size_t* size) {
    auto* installed = static_cast<InstalledAssets*>(user);
    std::vector<uint8_t> bytes;
    try {
        bytes = installed->assets->read(path).bytes;
    } catch (...) {
        return nullptr;
    }
    auto* out = static_cast<uint8_t*>(std::malloc(bytes.size() + 1));
    if (out == nullptr)
        return nullptr;
    std::memcpy(out, bytes.data(), bytes.size());
    *size = bytes.size();
    return out;
}

void clear_overlay(Surface* surface) {
    for (int32_t y = 0; y < surface->height; ++y) {
        std::memset(
            surface->pixels + static_cast<std::size_t>(y * surface->pitch),
            k_overlay_transparent,
            static_cast<std::size_t>(surface->width)
        );
    }
}

std::vector<uint8_t> copy_overlay(const Surface* surface) {
    std::vector<uint8_t> pixels(
        static_cast<std::size_t>(surface->width) * static_cast<std::size_t>(surface->height)
    );
    for (int32_t y = 0; y < surface->height; ++y) {
        std::memcpy(
            pixels.data() + static_cast<std::size_t>(y * surface->width),
            surface->pixels + static_cast<std::size_t>(y * surface->pitch),
            static_cast<std::size_t>(surface->width)
        );
    }
    return pixels;
}

void clear_color(Window* window) {
    if (window->color >= 0)
        color_release(window->color);
    window->color = k_no_handle;
}

void test_pixels(Desktop* desktop, Surface* overlay) {
    GuiLoadError error{};
    GuiScreen* screen = gui_screen_load("gui/locator.gui", &error);
    if (screen == nullptr)
        std::fprintf(stderr, "locator gui: %s\n", error.message);
    CHECK(screen != nullptr);
    if (screen == nullptr)
        return;
    CHECK(gui_screen_count(screen) == static_cast<int32_t>(sizeof k_placed / sizeof k_placed[0]));
    for (const Placed& placed : k_placed)
        expect_control(screen, placed);
    const GuiControl* title = gui_screen_find(screen, "Title");
    CHECK(title != nullptr && std::strcmp(title->caption, "LOCATOR") == 0);
    const GuiControl* field = gui_screen_find(screen, "Field");
    CHECK(field != nullptr && std::strcmp(field->caption, "Player Name") == 0);
    const GuiControl* search = gui_screen_find(screen, "Search");
    CHECK(search != nullptr && std::strcmp(search->caption, "SEARCH") == 0);
    const GuiControl* page = gui_screen_find(screen, "Page");
    CHECK(page != nullptr && std::strcmp(page->caption, "PAGE") == 0);
    const GuiControl* kind = gui_screen_find(screen, "kind_0");
    CHECK(kind != nullptr && std::strcmp(kind->caption, "Locate Player") == 0);
    CHECK(gui_screen_root(screen)->color == k_no_handle);
    gui_screen_destroy(screen);

    Window* frame = oa::boneyards::console::panel_create();
    window_create(frame, "Frame", 0, 0, 640, 480, nullptr, true);
    clear_color(frame);
    Window* console = locator_console_create(nullptr, frame, 23, 126, 498, 313);
    CHECK(console != nullptr);
    if (console == nullptr)
        return;
    console->visible = 1;
    expect_box(
        "Locator", console->x, console->y, console->width, console->height, 23, 126, 498, 313
    );
    for (const Placed& placed : k_placed) {
        if (std::strcmp(placed.name, "Locator") == 0)
            continue;
        expect_child(console, placed);
    }
    Window* page_button = window_find_named(console, "Page", nullptr);
    CHECK(page_button != nullptr && page_button->visible != 0);
    CHECK(std::strcmp(caption_of(page_button), "PAGE") == 0);
    Window* relocate = window_find_named(console, "Relocate", nullptr);
    CHECK(relocate != nullptr && relocate->states.current == button_state::disabled);
    Window* search_button = window_find_named(console, "Search", nullptr);
    CHECK(search_button != nullptr && search_button->states.current == button_state::unpressed);

    Rig rig;
    rig_init(&rig);
    screens::Locator* model = locator_console_state(console);
    CHECK(model != nullptr);
    if (model == nullptr)
        return;
    model->service = &rig.service;
    Window* name = window_find_named(console, "Name", nullptr);
    CHECK(name != nullptr);
    if (name != nullptr && name->ops != nullptr && name->ops->set_caption != nullptr)
        name->ops->set_caption(name, "ArmSniper", text_style::left);
    // Return in the field runs SEARCH; the field hands the key to the console.
    Message enter{};
    enter.code = MessageCode::key_down;
    enter.key = key::enter;
    CHECK(name != nullptr && name->ops->key_down(name, &enter));
    CHECK(rig.tape.locates == 1);
    CHECK(std::strcmp(rig.tape.locate_name, "ArmSniper") == 0);
    Notify activated{};
    activated.code = NotifyCode::activated;
    activated.source = search_button;
    console->ops->on_notify(console, &activated);
    CHECK(rig.tape.locates == 2);
    CHECK(std::strcmp(rig.tape.locate_name, "ArmSniper") == 0);
    LocateReport chatting =
        player_report(static_cast<int32_t>(LocateStatus::chatting), "ArmSniper");
    fill_name(chatting.area_name, sizeof chatting.area_name, "Fuel Dump");
    fill_name(chatting.room_name, sizeof chatting.room_name, "Tactics");
    chatting.area_id = 4;
    chatting.room_id = 9;
    deliver(rig.tape, chatting);
    console->ops->pulse(console);
    Window* shown = window_find_named(console, "rname0", nullptr);
    CHECK(std::strcmp(caption_of(shown), "ArmSniper") == 0);
    CHECK(
        std::strcmp(
            caption_of(window_find_named(console, "rwhere0", nullptr)), "Fuel Dump / Tactics"
        ) == 0
    );
    CHECK(
        std::strcmp(caption_of(window_find_named(console, "rstatus0", nullptr)), "chatting") == 0
    );
    CHECK(relocate->states.current == button_state::unpressed);
    CHECK(page_button->states.current == button_state::unpressed);

    // The five find rows, Name and Search, then the result, Relocate and Page.
    // Return in Name already ran above, before this walk.
    console->region = 1;
    Window* row0 = window_find_named(console, "row_0", nullptr);
    desktop_set_focus(desktop, row0);
    std::vector<std::string> stops;
    stops.emplace_back("row_0");
    for (int32_t step = 0; step < 24; ++step) {
        Message tab{};
        tab.code = MessageCode::key_down;
        tab.key = key::tab;
        desktop_route(desktop, &tab);
        Window* focus = desktop->focus;
        if (focus == row0)
            break;
        stops.emplace_back(focus != nullptr && focus->name != nullptr ? focus->name : "");
    }
    int32_t previous = -1;
    for (const char* stop :
         {"row_0",
          "row_1",
          "row_2",
          "row_3",
          "row_4",
          "Name",
          "Search",
          "rhit0",
          "Relocate",
          "Page"}) {
        int32_t found = -1;
        for (int32_t i = 0; i < static_cast<int32_t>(stops.size()); ++i) {
            if (stops[static_cast<std::size_t>(i)] == stop) {
                found = i;
                break;
            }
        }
        CHECK(found > previous);
        previous = found;
    }

    // Return on a result rebuilds the row. The keyboard stays on that row,
    // so the next Tab reaches Relocate and Return relocates.
    Window* hit = window_find_named(console, "rhit0", nullptr);
    desktop_set_focus(desktop, hit);
    desktop_route(desktop, &enter);
    console->ops->pulse(console);
    Window* kept = desktop->focus;
    CHECK(kept != nullptr && kept->name != nullptr && std::strcmp(kept->name, "rhit0") == 0);
    Message tab_on{};
    tab_on.code = MessageCode::key_down;
    tab_on.key = key::tab;
    desktop_route(desktop, &tab_on);
    CHECK(desktop->focus == relocate);
    const int32_t switches = rig.tape.switches;
    desktop_route(desktop, &enter);
    CHECK(rig.tape.switches == switches + 1);

    desktop_set_focus(desktop, nullptr);
    clear_overlay(overlay);
    desktop_draw(desktop);
    const std::vector<uint8_t> drawn = copy_overlay(overlay);
    // The large function button is a frame. Its top and bottom rails are
    // painted, and its face is clear so the console shows through.
    const auto pixel = [&](int32_t x, int32_t y) {
        return drawn[static_cast<std::size_t>(y * overlay->width + x)];
    };
    int32_t top_rail = 0;
    int32_t bottom_rail = 0;
    for (int32_t x = 396; x < 495; ++x) {
        if (pixel(x, 136) != k_overlay_transparent)
            ++top_rail;
        if (pixel(x, 156) != k_overlay_transparent)
            ++bottom_rail;
    }
    CHECK(top_rail > 90);
    CHECK(bottom_rail > 90);
    CHECK(pixel(396, 146) != k_overlay_transparent);
    CHECK(pixel(494, 146) != k_overlay_transparent);
    CHECK(pixel(445, 146) == k_overlay_transparent);
    CHECK(pixel(390, 146) == k_overlay_transparent);
    int32_t above = 0;
    for (int32_t x = 0; x < overlay->width; ++x) {
        if (drawn[static_cast<std::size_t>(120 * overlay->width + x)] != k_overlay_transparent)
            ++above;
    }
    CHECK(above == 0);
    CHECK(drawn[static_cast<std::size_t>(10 * overlay->width + 10)] == k_overlay_transparent);

    model->listening_rooms = true;
    console->visible = 0;
    console->ops->pulse(console);
    CHECK(!screens::locator_listening_rooms(model));
    CHECK(screens::locator_result_count(model) == 1);
    CHECK(window_find_named(console, "rname0", nullptr) != nullptr);
}

// ---- The faces, drawn as the game draws them --------------------------

// The install's own files, without the mod: the layout and the new art come
// through the client session's reader, as on a profile that does not mount
// coreprime.ccx.
struct Data {
    oa::AssetStore store{oa::boneyards_test::by_install()};
    oa::Palette palette{};
};

bool read_store(void* user, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<Data*>(user)->store.read(path).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

StatusWidget* g_widget = nullptr;
ServiceEvents g_events{};
// The folder --frames names, where each face is written as a picture to
// compare with the design. Empty writes none.
std::filesystem::path g_frames;

bool face_connected(void*) {
    return true;
}

ActivationResult face_activate(void*) {
    return ActivationResult::ok;
}

void keep_events(void*, const ServiceEvents* events) {
    g_events = events != nullptr ? *events : ServiceEvents{};
}

void face_census(void*, uint32_t) {
}

Message pointer(MessageCode code, int32_t x, int32_t y, uint32_t buttons) {
    Message msg{};
    msg.code = code;
    msg.x = x;
    msg.y = y;
    msg.buttons = buttons;
    return msg;
}

void click_window(const Window* w) {
    if (w == nullptr)
        return;
    const int32_t x = w->x + w->width / 2;
    const int32_t y = w->y + w->height / 2;
    boneyards_status_widget_event(g_widget, pointer(MessageCode::left_down, x, y, button_left));
    boneyards_status_widget_event(g_widget, pointer(MessageCode::left_up, x, y, 0));
}

void press(int32_t code) {
    Message msg{};
    msg.code = MessageCode::key_down;
    msg.key = code;
    boneyards_status_widget_event(g_widget, msg);
    msg.code = MessageCode::key_up;
    boneyards_status_widget_event(g_widget, msg);
}

void tick(int32_t cycles) {
    for (int32_t i = 0; i < cycles; ++i)
        boneyards_status_widget_tick(g_widget);
}

void finish_slide(const MainFrame* frame) {
    for (int32_t i = 0; frame->console->sliding != 0 && i < 100; ++i)
        tick(1);
}

Window* face_named(Window* root, const char* name) {
    Window* found = root != nullptr ? window_find_named(root, name, nullptr) : nullptr;
    if (found == nullptr) {
        std::fprintf(stderr, "FAIL missing window %s\n", name);
        ++g_failures;
    }
    return found;
}

// Puts `text` in the name field, as a search typed before SEARCH.
void fill_field(Window* console, const char* text) {
    Window* name = face_named(console, "Name");
    ScopedServices scope(g_widget);
    if (name != nullptr && name->ops != nullptr && name->ops->set_caption != nullptr)
        name->ops->set_caption(name, text, text_style::left);
}

// Draws the frame with nothing focused, as the design shows the console.
std::vector<uint8_t> frame_pixels() {
    desktop_set_focus(&g_widget->desktop, nullptr);
    std::vector<uint8_t> pixels(640 * 480, 0);
    Surface target{};
    target.width = 640;
    target.height = 480;
    target.pitch = 640;
    target.pixels = pixels.data();
    target.clip = oa::Rect32{0, 0, 639, 479};
    boneyards_status_widget_draw(g_widget, &target);
    return pixels;
}

// Writes a drawn frame as an indexed PNG in the widget's palette.
void write_picture(const std::vector<uint8_t>& drawn, const char* name) {
    if (g_frames.empty() || g_widget == nullptr || g_widget->config.palette == nullptr)
        return;
    std::vector<oa::formats::png::Rgb> colors(static_cast<std::size_t>(OA_PALETTE_COLORS));
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i) {
        const oa::PaletteEntry& entry = g_widget->config.palette->entries[i];
        colors[static_cast<std::size_t>(i)] = oa::formats::png::Rgb{entry.r, entry.g, entry.b};
    }
    oa::formats::png::Image image;
    image.header.width = 640;
    image.header.height = 480;
    image.header.bit_depth = 8;
    image.header.color_type = oa::formats::png::ColorType::palette;
    image.palette = colors;
    image.rows = drawn;
    std::vector<uint8_t> bytes;
    CHECK(oa::formats::png::write(image, &bytes));
    std::error_code error;
    std::filesystem::create_directories(g_frames, error);
    std::ofstream out(g_frames / name, std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    CHECK(out.good());
}

uint8_t at(const std::vector<uint8_t>& drawn, int32_t x, int32_t y) {
    return drawn[static_cast<std::size_t>(y) * 640u + static_cast<std::size_t>(x)];
}

// Counts the pixels of a one-pixel line, from (x0, y0) to (x1, y1) along a
// row or a column, that are not `color`.
int32_t off_line(
    const std::vector<uint8_t>& drawn, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color
) {
    int32_t off = 0;
    for (int32_t y = y0; y <= y1; ++y) {
        for (int32_t x = x0; x <= x1; ++x)
            off += at(drawn, x, y) != color ? 1 : 0;
    }
    return off;
}

// Counts the pixels of a rectangle's one-pixel outline that are not `color`.
int32_t off_outline(
    const std::vector<uint8_t>& drawn, int32_t x, int32_t y, int32_t w, int32_t h, uint8_t color
) {
    return off_line(drawn, x, y, x + w - 1, y, color) +
           off_line(drawn, x, y + h - 1, x + w - 1, y + h - 1, color) +
           off_line(drawn, x, y, x, y + h - 1, color) +
           off_line(drawn, x + w - 1, y, x + w - 1, y + h - 1, color);
}

void expect_none_off(int32_t off, const char* what) {
    if (off != 0) {
        std::fprintf(stderr, "FAIL %s: %d pixels off\n", what, off);
        ++g_failures;
    }
}

// Opaque pixels of `image` drawn at (x, y), origin included. Colour 9 is
// the key.
void expect_picture(
    const std::vector<uint8_t>& drawn, Handle image, int32_t x, int32_t y, const char* what
) {
    Surface surface{};
    if (image < 0 || !image_surface(image, &surface) || surface.pixels == nullptr) {
        std::fprintf(stderr, "FAIL %s: picture missing\n", what);
        ++g_failures;
        return;
    }
    int32_t opaque = 0;
    int32_t bad = 0;
    for (int32_t row = 0; row < surface.height; ++row) {
        const uint8_t* in = surface.pixels + static_cast<std::ptrdiff_t>(row) * surface.pitch;
        for (int32_t col = 0; col < surface.width; ++col) {
            if (in[col] == k_overlay_transparent)
                continue;
            ++opaque;
            const int32_t sx = x - surface.origin_x + col;
            const int32_t sy = y - surface.origin_y + row;
            const bool inside = sx >= 0 && sy >= 0 && sx < 640 && sy < 480;
            if (!inside || at(drawn, sx, sy) != in[col])
                ++bad;
        }
    }
    if (opaque == 0 || bad != 0) {
        std::fprintf(stderr, "FAIL %s: opaque %d mismatches %d\n", what, opaque, bad);
        ++g_failures;
    }
}

// The icon frame drawn for a search, lit when it is the one shown.
void expect_icon(const std::vector<uint8_t>& drawn, int32_t frame, int32_t x, int32_t y) {
    ScopedServices scope(g_widget);
    const Handle icon = image_load("BY_New.gaf", "Locator_Icons", frame, k_overlay_transparent);
    char what[48];
    std::snprintf(what, sizeof what, "Locator icon %d at %d,%d", frame, x, y);
    expect_picture(drawn, icon, x, y, what);
    if (icon >= 0)
        image_release(icon);
}

// The console glass at a screen pixel: the Green_Screen border where it is
// opaque, otherwise the green console layer's remapped war map.
int expected_glass(const Surface& border, int32_t x, int32_t y) {
    const ConsoleLayer* layer =
        g_widget->system.main_frame != nullptr ? g_widget->system.main_frame->console : nullptr;
    const int32_t col = x - main_frame_layout::console_x;
    const int32_t row = y - main_frame_layout::console_y;
    if (layer == nullptr || layer->green == nullptr || col < 0 || row < 0 || col >= border.width ||
        row >= border.height)
        return -1;
    const uint8_t edge = border.pixels[static_cast<std::ptrdiff_t>(row) * border.pitch + col];
    if (edge != k_overlay_transparent)
        return edge;
    const Surface& glass = layer->green->surface;
    if (col >= glass.width || row >= glass.height)
        return -1;
    return glass.pixels[static_cast<std::ptrdiff_t>(row) * glass.pitch + col];
}

// The Locator draws no field of its own: inside the results' outline, below
// the rows, and inside the name field, the console glass shows.
void expect_glass(
    const std::vector<uint8_t>& drawn,
    int32_t x0,
    int32_t y0,
    int32_t x1,
    int32_t y1,
    const char* what
) {
    ScopedServices scope(g_widget);
    const Handle green = image_load("Widgets.gaf", "Green_Screen", 0, k_overlay_transparent);
    Surface border{};
    if (green < 0 || !image_surface(green, &border) || border.pixels == nullptr) {
        std::fprintf(stderr, "FAIL Green_Screen missing\n");
        ++g_failures;
        if (green >= 0)
            image_release(green);
        return;
    }
    int32_t bad = 0;
    for (int32_t y = y0; y < y1; ++y) {
        for (int32_t x = x0; x < x1; ++x) {
            const int want = expected_glass(border, x, y);
            if (want < 0 || at(drawn, x, y) != want)
                ++bad;
        }
    }
    if (bad != 0) {
        std::fprintf(stderr, "FAIL %s: %d pixels are not the console glass\n", what, bad);
        ++g_failures;
    }
    image_release(green);
}

// The frame every face shares: the divider and the rule in palette 93, the
// results outlined in palette 90, with the glass inside, the name field
// outlined when the search has one, and the five search icons, the shown
// one lit.
void expect_face(const std::vector<uint8_t>& drawn, int32_t kind, bool field) {
    expect_none_off(off_line(drawn, 146, 129, 146, 435, 93), "divider");
    expect_none_off(off_line(drawn, 159, 182, 506, 182, 93), "rule under the headings");
    expect_none_off(off_outline(drawn, 158, 166, 350, 150, 90), "results outline");
    expect_glass(drawn, 170, 260, 500, 314, "inside the results");
    if (field) {
        expect_none_off(off_outline(drawn, 236, 136, 150, 21, 90), "name field outline");
    } else {
        CHECK(off_outline(drawn, 236, 136, 150, 21, 90) > 0);
        expect_glass(drawn, 236, 136, 386, 157, "where the name field was");
    }
    for (int32_t i = 0; i < screens::locator_kind::count; ++i)
        expect_icon(drawn, i * 2 + (i == kind ? 1 : 0), 26, 150 + 17 * i);
}

// The selected row lies on a band of palette 44, the width of the list.
void expect_band(const std::vector<uint8_t>& drawn, int32_t row) {
    const int32_t top = 184 + 15 * row;
    expect_none_off(off_line(drawn, 159, top, 162, top + 14, 44), "selected band's left end");
    // Above the text: a long status runs to the list's edge.
    expect_none_off(off_line(drawn, 505, top, 506, top + 2, 44), "selected band's right end");
}

// Three planets of the war map, in id order, as the mock names them.
bool load_planets(screens::GalaxyView* view) {
    auto planet = [](int32_t id, const char* name, int32_t x, char side) {
        screens::PlanetRecord r{};
        r.id = id;
        r.name = name;
        r.x = x;
        r.y = 100;
        r.sequence = "Planet";
        r.side = side;
        r.ta_map = "Coast to Coast";
        r.ta_mapsource = "TA";
        r.ta_maptype = "Green";
        return r;
    };
    const screens::PlanetRecord planets[3] = {
        planet(1799, "Altair", 10, 'C'),
        planet(1803, "Fuel Dump", 100, 'A'),
        planet(1810, "Lava Alley", 190, 'A'),
    };
    const screens::ConnectorRecord links[2] = {{1799, 1803}, {1803, 1810}};
    const screens::SpriteFrame frames[3] = {{16, 16, 8, 8}, {16, 16, 8, 8}, {16, 16, 8, 8}};
    const std::size_t counts[3] = {1, 1, 1};
    const screens::PlanetSprites sprites{frames, counts};
    return screens::galaxy_load(view, 2400, 1600, planets, 3, links, 2, &sprites);
}

void test_player_face(Window* console, Tape& tape) {
    Window* name = face_named(console, "Name");
    CHECK(name != nullptr && name->font == g_widget->system.hat_fonts.small);
    CHECK(name != nullptr && reinterpret_cast<EditBox*>(name)->padding == 6);
    // The player face takes typing without a click on the name.
    press('Q');
    CHECK(
        name != nullptr && name->focused != 0 &&
        std::strcmp(reinterpret_cast<EditBox*>(name)->text, "Q") == 0
    );
    fill_field(console, "");
    // Typed, then Return runs the search.
    click_window(name);
    CHECK(name != nullptr && name->focused != 0);
    for (const char* c = "ArmSniper"; *c != '\0'; ++c)
        press(*c);
    press(key::enter);
    CHECK(tape.locates == 1);
    CHECK(std::strcmp(tape.locate_name, "ArmSniper") == 0);
    LocateReport chatting =
        player_report(static_cast<int32_t>(LocateStatus::chatting), "ArmSniper");
    fill_name(chatting.area_name, sizeof chatting.area_name, "Fuel Dump");
    fill_name(chatting.room_name, sizeof chatting.room_name, "Tactics");
    chatting.area_id = 1803;
    chatting.room_id = 9;
    deliver(tape, chatting);
    tick(1);
    const std::vector<uint8_t> drawn = frame_pixels();
    write_picture(drawn, "locator-player.png");
    expect_face(drawn, screens::locator_kind::player, true);
    expect_band(drawn, 0);
    expect_icon(drawn, 1, 163, 186);
    Window* page = face_named(console, "Page");
    CHECK(page != nullptr && page->visible != 0 && page->states.current == button_state::unpressed);
    Window* relocate = face_named(console, "Relocate");
    CHECK(relocate != nullptr && relocate->states.current == button_state::unpressed);
}

void expect_name_took(Window* console, char letter) {
    Window* name = face_named(console, "Name");
    const char* before = name != nullptr ? reinterpret_cast<EditBox*>(name)->text : "";
    const int32_t length = before != nullptr ? static_cast<int32_t>(std::strlen(before)) : 0;
    press(letter);
    const char* after = name != nullptr ? reinterpret_cast<EditBox*>(name)->text : "";
    CHECK(
        name != nullptr && name->focused != 0 && after != nullptr &&
        static_cast<int32_t>(std::strlen(after)) == length + 1 && after[length] == letter
    );
}

void test_planet_face(Window* console) {
    click_window(face_named(console, "row_1"));
    expect_name_took(console, 'P');
    fill_field(console, "Altair");
    click_window(face_named(console, "Search"));
    tick(1);
    const screens::Locator* model = locator_console_state(console);
    CHECK(model != nullptr && screens::locator_result_count(model) == 1);
    const std::vector<uint8_t> drawn = frame_pixels();
    write_picture(drawn, "locator-planet.png");
    expect_face(drawn, screens::locator_kind::planet, true);
    expect_band(drawn, 0);
    expect_icon(drawn, 3, 163, 186);
    Window* page = face_named(console, "Page");
    CHECK(page != nullptr && page->visible == 0);
}

void test_battle_face(Window* console, Tape& tape) {
    click_window(face_named(console, "row_2"));
    expect_name_took(console, 'B');
    fill_field(console, "Rumble");
    click_window(face_named(console, "Search"));
    CHECK(tape.game_locates == 1 && std::strcmp(tape.game_name, "Rumble") == 0);
    CHECK(tape.battle_calls == 0);
    deliver_game(tape, game_report(1810, "rr1", "Rumble in the Rubble", true, false));
    deliver_game(tape, game_report(1803, "rj2", "Rumble Jungle", false, true));
    CHECK(tape.intel_calls == 1 && std::strcmp(tape.intel_game, "rr1") == 0);
    BattleIntel intel{};
    intel.game_id = "rr1";
    intel.user_name = "Kbot_Kid";
    intel.skill = 1;
    CHECK(g_events.battle_intel != nullptr);
    if (g_events.battle_intel != nullptr) {
        ScopedServices scope(g_widget);
        g_events.battle_intel(g_events.user, &intel);
    }
    tick(1);
    const screens::Locator* model = locator_console_state(console);
    CHECK(model != nullptr && screens::locator_result_count(model) == 2);
    CHECK(
        model != nullptr && std::strcmp(
                                screens::locator_detail(model),
                                "Rumble in the Rubble: Duel, Seasoned, hosted by Kbot_Kid."
                            ) == 0
    );
    const std::vector<uint8_t> drawn = frame_pixels();
    write_picture(drawn, "locator-battle.png");
    expect_face(drawn, screens::locator_kind::battle, true);
    expect_band(drawn, 0);
    expect_icon(drawn, 5, 163, 186);
    expect_icon(drawn, 4, 163, 201);
    Window* join = face_named(console, "Page");
    CHECK(join != nullptr && join->visible != 0 && join->states.current == button_state::unpressed);
}

// The planets are asked in turn, each answering through the client's room
// list feed as the status widget hands it to the open Locator.
void test_frequency_face(Window* console, Tape& tape) {
    click_window(face_named(console, "row_3"));
    expect_name_took(console, 'F');
    fill_field(console, "Tactics");
    click_window(face_named(console, "Search"));

    struct Row {
        uint32_t area;
        uint32_t room;
        const char* name;
        int32_t users;
        int32_t type;
    };

    const Row rows[] = {
        {1799, 60001, "Sector Chat", 11, screens::room_type::user_created},
        {1799, 1, "P1-Altair", 2, screens::room_type::planetary_default},
        {1799, 61, "Tactics Talk", 3, screens::room_type::user_created},
        {1803, 60001, "Sector Chat", 11, screens::room_type::user_created},
        {1803, 1, "P1-Fuel Dump", 4, screens::room_type::planetary_default},
        {1803, 9, "Tactics", 8, screens::room_type::user_created},
        {1810, 60001, "Sector Chat", 11, screens::room_type::user_created},
        {1810, 1, "P1-Lava Alley", 1, screens::room_type::planetary_default},
    };
    const uint32_t areas[] = {1799, 1803, 1810};
    CHECK(g_events.room_listed != nullptr);
    for (int32_t a = 0; a < 3; ++a) {
        CHECK(tape.room_calls == a + 1 && tape.room_areas[a] == areas[a]);
        const Row* last = nullptr;
        for (const Row& row : rows)
            last = row.area == areas[a] ? &row : last;
        for (const Row& row : rows) {
            if (row.area != areas[a] || g_events.room_listed == nullptr)
                continue;
            ScopedServices scope(g_widget);
            g_events.room_listed(
                g_events.user,
                0,
                row.area,
                row.room,
                row.name,
                "",
                false,
                row.users,
                row.type,
                "",
                &row == last,
                false,
                0
            );
        }
    }
    tick(1);
    const screens::Locator* model = locator_console_state(console);
    CHECK(model != nullptr && !screens::locator_listening_rooms(model));
    CHECK(model != nullptr && screens::locator_result_count(model) == 2);
    const std::vector<uint8_t> drawn = frame_pixels();
    write_picture(drawn, "locator-frequency.png");
    expect_face(drawn, screens::locator_kind::frequency, true);
    expect_band(drawn, 0);
    expect_icon(drawn, 7, 163, 186);
    Window* extra = face_named(console, "Page");
    CHECK(extra != nullptr && extra->visible == 0);

    // A search for the Sector Chat lists it once, on every planet.
    fill_field(console, "Sector");
    click_window(face_named(console, "Search"));
    for (int32_t a = 0; a < 3; ++a) {
        const Row* last = nullptr;
        for (const Row& row : rows)
            last = row.area == areas[a] ? &row : last;
        for (const Row& row : rows) {
            if (row.area != areas[a] || g_events.room_listed == nullptr)
                continue;
            ScopedServices scope(g_widget);
            g_events.room_listed(
                g_events.user,
                0,
                row.area,
                row.room,
                row.name,
                "",
                false,
                row.users,
                row.type,
                "",
                &row == last,
                false,
                0
            );
        }
    }
    tick(1);
    CHECK(model != nullptr && screens::locator_result_count(model) == 1);
    const screens::LocatorResult* shared = screens::locator_result(model, 0);
    CHECK(shared != nullptr && std::strcmp(shared->name, "Sector Chat") == 0);
    CHECK(shared != nullptr && std::strcmp(shared->where, screens::kLocatorEveryPlanet) == 0);
}

void test_server_face(Window* console) {
    click_window(face_named(console, "row_4"));
    Window* name = face_named(console, "Name");
    const std::string before = name != nullptr && reinterpret_cast<EditBox*>(name)->text != nullptr
                                   ? reinterpret_cast<EditBox*>(name)->text
                                   : "";
    press('Z');
    const char* after = name != nullptr ? reinterpret_cast<EditBox*>(name)->text : "";
    CHECK(after != nullptr && std::strcmp(after, before.c_str()) == 0);
    CHECK(g_widget->desktop.focus != name);
    tick(1);
    const screens::Locator* model = locator_console_state(console);
    CHECK(model != nullptr && screens::locator_result_count(model) == 2);
    const std::vector<uint8_t> drawn = frame_pixels();
    write_picture(drawn, "locator-server.png");
    expect_face(drawn, screens::locator_kind::server, false);
    expect_band(drawn, 0);
    expect_icon(drawn, 9, 163, 186);
    for (const char* hidden : {"Field", "NameBox", "Name", "Search", "Page"}) {
        Window* w = face_named(console, hidden);
        CHECK(w != nullptr && w->visible == 0);
    }
    Window* relocate = face_named(console, "Relocate");
    CHECK(relocate != nullptr && relocate->states.current == button_state::disabled);
}

void test_password_face(Window* console, Tape& tape, MainFrame* frame) {
    // Server Transfer has hidden SEARCH. The battle face has to be showing
    // before that button is clicked, or the click lands on whatever is
    // underneath and the passworded battle never arrives.
    click_window(face_named(console, "row_2"));
    tick(1);
    Window* search = face_named(console, "Search");
    CHECK(search != nullptr && search->visible != 0);
    fill_field(console, "Lock");
    const int32_t locates = tape.game_locates;
    click_window(search);
    CHECK(tape.game_locates == locates + 1);
    const screens::Locator* listening = locator_console_state(console);
    CHECK(listening != nullptr && screens::locator_listening_battles(listening));
    GameLocateReport locked = game_report(1803, "lock1", "Locked", false, true);
    locked.pass_required = true;
    locked.seats_known = true;
    locked.players = 2;
    locked.playerlimit = 4;
    deliver_game(tape, locked);
    tick(1);
    const screens::Locator* model = locator_console_state(console);
    const screens::LocatorResult* row =
        model != nullptr ? screens::locator_result(model, 0) : nullptr;
    CHECK(row != nullptr);
    if (row != nullptr) {
        CHECK(std::strcmp(row->status, "in progress") == 0);
        CHECK(row->password);
    }
    const int32_t joins = tape.joins;
    Window* page = face_named(console, "Page");
    CHECK(page != nullptr && page->visible != 0);
    click_window(page);
    finish_slide(frame);
    tick(1);
    CHECK(tape.joins == joins);
    CHECK(frame->console_screen == console_screen::battle);
    InfoPane* pane = frame->panes[static_cast<int32_t>(PaneKind::join_summary)];
    CHECK(pane != nullptr && pane->base.visible != 0);
    auto* join = pane != nullptr ? reinterpret_cast<JoinSummaryPane*>(pane) : nullptr;
    CHECK(join != nullptr && join->password != nullptr && join->password->base.visible != 0);
    if (join != nullptr && join->password != nullptr) {
        CHECK(join->password_label != nullptr && join->password_label->visible != 0);
        press('k');
        CHECK(std::strcmp(single_edit_text(join->password), "k") == 0);
        single_edit_set_text(join->password, "");
    }
}

void test_faces(Data* data) {
    Tape tape{};
    tape.servers[0] = "CorePrime (Alpha)";
    tape.servers[1] = "Yard Beta";
    tape.server_count = 2;
    tape.current = "CorePrime (Alpha)";
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_store};
    config.palette = &data->palette;
    config.service.user = &tape;
    config.service.network_available = face_connected;
    config.service.activate = face_activate;
    config.service.connected = face_connected;
    config.service.set_events = keep_events;
    config.service.request_census = face_census;
    config.service.locate_user = tape_locate;
    config.service.locate_game = tape_locate_game;
    config.service.request_room_list = tape_rooms;
    config.service.enter_battles = tape_battles;
    config.service.get_battle = tape_intel;
    config.service.switch_room = tape_switch;
    config.service.join_game = tape_join;
    config.service.list_servers = tape_servers;
    config.service.current_server = tape_current;
    g_widget = boneyards_status_widget_create(config);
    CHECK(g_widget != nullptr);
    if (g_widget == nullptr)
        return;
    click_window(boneyards_status_widget_part(g_widget, OverlayPart::enter));
    MainFrame* frame = g_widget->system.main_frame;
    CHECK(frame != nullptr);
    if (frame == nullptr) {
        boneyards_status_widget_destroy(g_widget);
        g_widget = nullptr;
        return;
    }
    for (int32_t i = 0; frame->console_screen != console_screen::login && i < 160; ++i)
        tick(1);
    CHECK(load_planets(&g_widget->navigation->galaxy));
    screens::galaxy_apply_allegiances(&g_widget->navigation->galaxy, "CAA");
    {
        ScopedServices scope(g_widget);
        galaxy_panel_sync_hotspots(frame);
        main_frame_close_screen(frame);
    }
    finish_slide(frame);
    click_window(frame->map_panel->hotspots[1]);
    g_widget->system.matchmaking_enabled =
        account_sees_unfinished(kAccountStatusBetaTester) ? 1 : 0;
    main_frame_enable_buttons(frame);
    click_window(frame->console_buttons[static_cast<int32_t>(ConsoleButton::locator)]);
    finish_slide(frame);
    Window* console = frame->active_screen;
    CHECK(console != nullptr && window_is_named(console, k_locator_console_name));
    CHECK(locator_console_state(console) != nullptr);
    if (console != nullptr && locator_console_state(console) != nullptr) {
        test_player_face(console, tape);
        test_planet_face(console);
        test_battle_face(console, tape);
        test_frequency_face(console, tape);
        test_server_face(console);
        test_password_face(console, tape, frame);
    }
    boneyards_status_widget_destroy(g_widget);
    g_widget = nullptr;
    g_events = ServiceEvents{};
}

} // namespace

int main(int argc, char** argv) {
    test_player();
    test_planet();
    test_battle();
    test_frequency();
    test_shared_frequency();
    test_stated_frequency();
    test_server_region();
    test_server();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the locator console drawn from the install");
    const std::filesystem::path coreprime = oa::boneyards_test::coreprime_archive(argc, argv);
    if (coreprime.empty()) {
        std::fprintf(stderr, "the mod folder is required\n");
        return 1;
    }
    oa::base::bytes::Decoded<oa::HpiArchive> opened = oa::open_hpi_file(coreprime);
    if (!opened.ok() || !opened.value.has_value()) {
        std::fprintf(stderr, "coreprime.ccx could not be opened\n");
        return 1;
    }
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0)
            g_frames = std::filesystem::path(argv[i + 1]);
    }
    FileSource source;
    source.archive = &*opened.value;
    source.fixtures = OA_GUI_FIXTURES;
    rover_session_set_virtual_files(read_source, &source);

    oa::AssetStore assets(root);
    assets.mount(root / "by.ccx");
    assets.mount(root / "totala1.hpi");
    assets.mount(coreprime);
    InstalledAssets installed;
    installed.assets = &assets;
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int i = 0; i < OA_PALETTE_COLORS; ++i) {
        installed.palette.entries[i] = oa::PaletteEntry{
            raw[static_cast<std::size_t>(i) * 4],
            raw[static_cast<std::size_t>(i) * 4 + 1],
            raw[static_cast<std::size_t>(i) * 4 + 2],
            0
        };
    }
    ToolkitConfig config{};
    config.width = 640;
    config.height = 480;
    config.palette = &installed.palette;
    config.files = ToolkitFiles{&installed, read_asset};
    Toolkit* toolkit = toolkit_create(&config);
    CHECK(toolkit != nullptr);
    if (toolkit == nullptr) {
        rover_session_set_virtual_files(nullptr, nullptr);
        return 1;
    }
    const Services previous = services_install(toolkit_services(toolkit));
    test_pixels(toolkit_desktop(toolkit), toolkit_surface(toolkit));
    services_install(previous);
    toolkit_destroy(toolkit);
    {
        Data data;
        (void)data.store.discover("31");
        data.palette = installed.palette;
        test_faces(&data);
    }
    rover_session_set_virtual_files(nullptr, nullptr);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("locator tests passed\n");
    return 0;
}
