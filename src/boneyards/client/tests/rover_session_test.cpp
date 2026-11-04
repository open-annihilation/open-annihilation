// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Central Command session against a fake Rover table: activation, the
// login stages and their texts, the dossier, disconnects, logout and close,
// the pump gate, native file placement and creation, server lists written
// for a host, the selected server read from oa-rover itself and
// the auto-update stand-in, the war-map requests and feeds (slots 34-36,
// 38, 39 and 72), the locator (slot 65), the chat room requests and feeds
// (slots 9 to 13, 41 and 69), the profile requests (slot 63), the battle
// requests and feeds (slots 15 to 23) through oa-rover's own handlers; then a
// whole login through the Boneyards screens' event sink. A confirmed login
// keeps the session live until logout finishes or the link drops.
#include "oa/boneyards/maia_update.hpp"
#include "oa/boneyards/rover_calls.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/rover_string.hpp"
#include "oa/boneyards/screens/navigator.hpp"
#include "oa/boneyards/screens/profile.hpp"
#include "oa/rover/platform.h"
#include "record.h"
#include "requests.h"
#include "servers.h"
#include "state.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace oa::boneyards;

int g_failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

using PumpAction = void (*)();

struct FakeRover {
    bool attach_ok = true;
    bool activate_ok = true;
    bool login_ok = true;
    int32_t attaches = 0;
    int32_t activates = 0;
    int32_t deactivates = 0;
    int32_t detaches = 0;
    int32_t resets = 0;
    int32_t logouts = 0;
    int32_t logout_user = 0;
    RoverLogoutFn logged_out = nullptr;
    std::string login_server;
    std::string login_user;
    std::string login_password;
    RoverLoginProgressFn progress = nullptr;
    RoverSystemAlertFn alert = nullptr;
    RoverDisconnectFn disconnect = nullptr;
    uint32_t profile_user = 0;
    RoverProfileFn profile = nullptr;
    RoverLadderProfileFn ladder = nullptr;
    std::vector<uint32_t> profile_users;
    PumpAction on_pump = nullptr;
    int32_t pumps = 0;
    // The war-map slots.
    std::vector<std::string> war_calls;
    std::string map_path;
    RoverSectorReader* reader = nullptr;
    int32_t read_width = 0;
    int32_t read_height = 0;
    RoverSectorPlanet* read_planets = nullptr;
    int32_t read_planet_count = 0;
    RoverSectorConnector* read_connectors = nullptr;
    int32_t read_connector_count = 0;
    RoverSectorReader* closed = nullptr;
    uint32_t census_area = 0;
    RoverCensusFn census = nullptr;
    RoverActivityFn activity = nullptr;
    RoverAllegianceFn allegiances = nullptr;
    std::string locate_name;
    RoverLocateFn located = nullptr;
    // The chat room slots: each call as "slot:arguments", what they answer
    // and the callbacks they were handed.
    std::vector<std::string> room_calls;
    bool room_ok = true;
    RoverRoomListFn room_list = nullptr;
    RoverRoomEnteredFn entered = nullptr;
    RoverRoomInfoFn room_info = nullptr;
    RoverRoomUpdateFn room_update = nullptr;
    RoverChatMsgFn chat_msg = nullptr;
    RoverRoomLeftFn left = nullptr;
    RoverRoomCreatedFn created = nullptr;
};

FakeRover g_fake;
RoverInterface g_table;

bool ROVER_CALL fake_attach() {
    ++g_fake.attaches;
    return g_fake.attach_ok;
}

void ROVER_CALL fake_detach() {
    ++g_fake.detaches;
}

bool ROVER_CALL fake_activate() {
    ++g_fake.activates;
    return g_fake.activate_ok;
}

void ROVER_CALL fake_deactivate() {
    ++g_fake.deactivates;
}

void ROVER_CALL fake_pump_events() {
    ++g_fake.pumps;
    PumpAction action = g_fake.on_pump;
    g_fake.on_pump = nullptr;
    if (action != nullptr)
        action();
}

bool ROVER_CALL fake_reset_session() {
    ++g_fake.resets;
    return true;
}

bool ROVER_CALL fake_start_login(
    const char* server_name,
    const char* username,
    const char* password,
    RoverLoginProgressFn progress,
    RoverSystemAlertFn system_alert,
    RoverDisconnectFn disconnect
) {
    g_fake.login_server = server_name;
    g_fake.login_user = username;
    g_fake.login_password = password;
    g_fake.progress = progress;
    g_fake.alert = system_alert;
    g_fake.disconnect = disconnect;
    return g_fake.login_ok;
}

bool ROVER_CALL fake_send_logout(int32_t user_id, RoverLogoutFn logged_out) {
    ++g_fake.logouts;
    g_fake.logout_user = user_id;
    g_fake.logged_out = logged_out;
    return true;
}

void ROVER_CALL
fake_request_get_profile(uint32_t user_id, RoverProfileFn profile, RoverLadderProfileFn ladder) {
    g_fake.profile_user = user_id;
    g_fake.profile = profile;
    g_fake.ladder = ladder;
    g_fake.profile_users.push_back(user_id);
}

void ROVER_CALL fake_find_current_area(const char** out) {
    *out = "CorePrime Alpha Test";
}

void ROVER_CALL fake_no_current_area(const char** out) {
    *out = nullptr;
}

// The fake dossier bag answers these fields.
struct DossierField {
    const char* key;
    const char* text;
    int32_t number;
};

constexpr DossierField kDossier[] = {
    {"USER_STATUS", nullptr, 5},
    {"LOGINNAME", "Kestrel", 0},
    {"ALLEGIANCE", "core", 0},
};

const DossierField* dossier_field(const RoverString* key) {
    for (const DossierField& field : kDossier)
        if (std::strcmp(field.key, rover_string_text(key)) == 0)
            return &field;
    return nullptr;
}

bool ROVER_CALL fake_get_field_int(RoverPropertyBag*, const RoverString* key, int32_t* out) {
    const DossierField* field = dossier_field(key);
    if (field == nullptr || field->text != nullptr)
        return false;
    *out = field->number;
    return true;
}

bool ROVER_CALL fake_get_field_string(RoverPropertyBag*, const RoverString* key, RoverString* out) {
    const DossierField* field = dossier_field(key);
    if (field == nullptr || field->text == nullptr)
        return false;
    rover_string_assign(out, field->text, static_cast<uint32_t>(std::strlen(field->text)));
    return true;
}

// A stand-in reader the fake slots hand out.
int g_reader_storage = 0;

const char* ROVER_CALL fake_match_area_name() {
    g_fake.war_calls.emplace_back("72");
    return "alpha";
}

RoverSectorReader* ROVER_CALL fake_open_sector_map(
    const char* path,
    int32_t* sector_id,
    char* sector_name,
    int32_t* width,
    int32_t* height,
    int32_t* planets,
    int32_t* connectors
) {
    g_fake.war_calls.emplace_back("34");
    g_fake.map_path = path;
    *sector_id = 45;
    std::snprintf(sector_name, 0x33, "New Hub");
    *width = 2000;
    *height = 1400;
    *planets = 25;
    *connectors = 32;
    return reinterpret_cast<RoverSectorReader*>(&g_reader_storage);
}

bool ROVER_CALL fake_read_sector_planets(
    RoverSectorReader* reader,
    int32_t width,
    int32_t height,
    RoverSectorPlanet* planets,
    int32_t planet_count,
    RoverSectorConnector* connectors,
    int32_t connector_count
) {
    g_fake.war_calls.emplace_back("35");
    g_fake.reader = reader;
    g_fake.read_width = width;
    g_fake.read_height = height;
    g_fake.read_planets = planets;
    g_fake.read_planet_count = planet_count;
    g_fake.read_connectors = connectors;
    g_fake.read_connector_count = connector_count;
    planets[0].id = 1803;
    std::snprintf(planets[0].desc, sizeof planets[0].desc, "Fuel");
    planets[0].watching = 2;
    connectors[0].planet2_id = 1804;
    return true;
}

void ROVER_CALL fake_close_sector_map(RoverSectorReader* reader) {
    g_fake.war_calls.emplace_back("36");
    g_fake.closed = reader;
}

void ROVER_CALL fake_request_census(uint32_t area, RoverCensusFn census, RoverActivityFn activity) {
    g_fake.war_calls.emplace_back("38");
    g_fake.census_area = area;
    g_fake.census = census;
    g_fake.activity = activity;
}

void ROVER_CALL fake_request_allegiance(RoverAllegianceFn allegiances) {
    g_fake.war_calls.emplace_back("39");
    g_fake.allegiances = allegiances;
}

void ROVER_CALL fake_locate_player(const char* name, RoverLocateFn located) {
    g_fake.locate_name = name;
    g_fake.located = located;
}

std::string text_or_null(const char* text) {
    return text != nullptr ? text : "(null)";
}

bool ROVER_CALL fake_get_rooms(int32_t area_id, RoverRoomListFn room_list) {
    g_fake.room_calls.push_back("9:" + std::to_string(area_id));
    g_fake.room_list = room_list;
    return g_fake.room_ok;
}

void keep_room_handlers(
    RoverRoomEnteredFn entered,
    RoverRoomInfoFn room_info,
    RoverRoomUpdateFn room_list,
    RoverChatMsgFn chat_msg
) {
    g_fake.entered = entered;
    g_fake.room_info = room_info;
    g_fake.room_update = room_list;
    g_fake.chat_msg = chat_msg;
}

bool ROVER_CALL fake_enter_room(
    int32_t room_id,
    int32_t area_id,
    const char* password,
    RoverRoomEnteredFn entered,
    RoverRoomInfoFn room_info,
    RoverRoomUpdateFn room_list,
    RoverChatMsgFn chat_msg
) {
    g_fake.room_calls.push_back(
        "10:" + std::to_string(room_id) + "," + std::to_string(area_id) + "," +
        text_or_null(password)
    );
    keep_room_handlers(entered, room_info, room_list, chat_msg);
    return g_fake.room_ok;
}

bool ROVER_CALL fake_switch_room(
    int32_t room_id,
    int32_t area_id,
    const char* password,
    int32_t old_room_id,
    int32_t old_area_id,
    RoverRoomEnteredFn entered,
    RoverRoomInfoFn room_info,
    RoverRoomUpdateFn room_list,
    RoverChatMsgFn chat_msg
) {
    g_fake.room_calls.push_back(
        "11:" + std::to_string(room_id) + "," + std::to_string(area_id) + "," +
        text_or_null(password) + "," + std::to_string(old_room_id) + "," +
        std::to_string(old_area_id)
    );
    keep_room_handlers(entered, room_info, room_list, chat_msg);
    return g_fake.room_ok;
}

bool ROVER_CALL fake_leave_room(int32_t room_id, int32_t area_id, RoverRoomLeftFn left) {
    g_fake.room_calls.push_back("12:" + std::to_string(room_id) + "," + std::to_string(area_id));
    g_fake.left = left;
    return g_fake.room_ok;
}

bool ROVER_CALL
fake_send_chat(int32_t room_id, int32_t area_id, int32_t target_user_id, const char* message) {
    g_fake.room_calls.push_back(
        "13:" + std::to_string(room_id) + "," + std::to_string(area_id) + "," +
        std::to_string(target_user_id) + "," + message
    );
    return g_fake.room_ok;
}

void ROVER_CALL fake_send_page_by_name(const char* user_name, const char* message) {
    g_fake.room_calls.push_back(std::string("41:") + user_name + "," + message);
}

void ROVER_CALL fake_create_room(
    uint32_t area_id,
    const char* room_name,
    const char* description,
    const char* password,
    RoverRoomCreatedFn created
) {
    g_fake.room_calls.push_back(
        "69:" + std::to_string(area_id) + "," + room_name + "," + description + "," +
        text_or_null(password)
    );
    g_fake.created = created;
}

void ROVER_CALL fake_get_interface(const RoverInterface** out) {
    *out = &g_table;
}

// What the session reported.
struct Reports {
    std::vector<std::string> statuses;
    std::vector<std::string> notices;
    std::vector<std::string> battle_lines;
    std::vector<SessionLevel> levels;
    int32_t resets = 0;
    int32_t alert_kind = -1;
    std::string alert_text;
    std::string allegiances;
    std::string activity;
    std::vector<CensusReport> census;
    std::vector<std::string> room_events; // each chat room event, fields in order
    uint32_t alert_sender = 0;
    std::string alert_type;
    bool in_battle = false;
};

Reports g_reports;

void record_status(void*, const char* text) {
    g_reports.statuses.emplace_back(text);
}

void record_reset(void*) {
    ++g_reports.resets;
}

void record_session(void*, const SessionRecord* session) {
    g_reports.levels.push_back(session->level);
}

void record_notice(void*, const char* text) {
    g_reports.notices.emplace_back(text);
}

void record_alert(void*, const AlertReport* alert) {
    g_reports.alert_kind = alert->kind;
    g_reports.alert_text = alert->text;
    g_reports.alert_sender = alert->sender_id;
    g_reports.alert_type = alert->notification_type;
}

bool report_battle(void*) {
    return g_reports.in_battle;
}

void record_battle_line(void*, const char* text) {
    g_reports.battle_lines.emplace_back(text);
}

void record_allegiances(void*, const char* text) {
    g_reports.allegiances = text;
}

void record_activity(void*, const char* text) {
    g_reports.activity = text;
}

void record_census(void*, const CensusReport* report) {
    g_reports.census.push_back(*report);
}

void record_room_listed(
    void*,
    int32_t change,
    uint32_t area_id,
    uint32_t room_id,
    const char* name,
    const char* description,
    bool needs_password,
    int32_t users,
    int32_t type,
    const char* tag,
    bool last,
    bool,
    int32_t
) {
    g_reports.room_events.push_back(
        "listed " + std::to_string(change) + "," + std::to_string(area_id) + "," +
        std::to_string(room_id) + "," + name + "," + description + "," +
        std::to_string(needs_password) + "," + std::to_string(users) + "," + std::to_string(type) +
        "," + tag + "," + std::to_string(last)
    );
}

void record_room_updated(
    void*,
    int32_t change,
    uint32_t area_id,
    uint32_t room_id,
    const char* name,
    const char* description,
    bool needs_password,
    int32_t users,
    int32_t type,
    const char* tag
) {
    g_reports.room_events.push_back(
        "updated " + std::to_string(change) + "," + std::to_string(area_id) + "," +
        std::to_string(room_id) + "," + name + "," + description + "," +
        std::to_string(needs_password) + "," + std::to_string(users) + "," + std::to_string(type) +
        "," + tag
    );
}

void record_room_entered(void*, int32_t result) {
    g_reports.room_events.push_back("entered " + std::to_string(result));
}

void record_room_member(
    void*,
    int32_t change,
    uint32_t user_id,
    const char* name,
    int32_t latency,
    int32_t icon,
    int32_t rank,
    const char* allegiance
) {
    g_reports.room_events.push_back(
        "member " + std::to_string(change) + "," + std::to_string(user_id) + "," + name + "," +
        std::to_string(latency) + "," + std::to_string(icon) + "," + std::to_string(rank) + "," +
        allegiance
    );
}

void record_chat_message(void*, bool from_server, uint32_t user_id, const char* text) {
    g_reports.room_events.push_back(
        "chat " + std::to_string(from_server) + "," + std::to_string(user_id) + "," + text
    );
}

void record_room_left(void*, bool failed) {
    g_reports.room_events.push_back("left " + std::to_string(failed));
}

void record_room_created(void*, uint32_t area_id, uint32_t room_id, const char* name) {
    g_reports.room_events.push_back(
        "created " + std::to_string(area_id) + "," + std::to_string(room_id) + "," + name
    );
}

BoneyardsService g_service;

void reset_all() {
    rover_module_unload();
    *rover_session() = RoverSession{};
    g_fake = FakeRover{};
    g_reports = Reports{};
    const RoverInterface* real = nullptr;
    rover_get_interface(&real);
    g_table = *real;
    g_table.attach = fake_attach;
    g_table.detach = fake_detach;
    g_table.activate = fake_activate;
    g_table.deactivate = fake_deactivate;
    g_table.pump_events = fake_pump_events;
    g_table.reset_session = fake_reset_session;
    g_table.start_login = fake_start_login;
    g_table.send_logout = fake_send_logout;
    g_table.request_get_profile = fake_request_get_profile;
    g_table.find_current_area = fake_find_current_area;
    g_table.get_field_int = fake_get_field_int;
    g_table.get_field_string = fake_get_field_string;
    g_table.match_area_name = fake_match_area_name;
    g_table.open_sector_map = fake_open_sector_map;
    g_table.read_sector_planets = fake_read_sector_planets;
    g_table.close_sector_map = fake_close_sector_map;
    g_table.request_census = fake_request_census;
    g_table.request_allegiance = fake_request_allegiance;
    g_table.locate_player = fake_locate_player;
    g_table.get_rooms = fake_get_rooms;
    g_table.enter_room = fake_enter_room;
    g_table.switch_room = fake_switch_room;
    g_table.leave_room = fake_leave_room;
    g_table.send_chat = fake_send_chat;
    g_table.send_page_by_name = fake_send_page_by_name;
    g_table.create_room = fake_create_room;
    rover_module_set_source(fake_get_interface);
    g_service = rover_session_service();
    ServiceEvents events{};
    events.login_status = record_status;
    events.login_reset = record_reset;
    events.session_changed = record_session;
    events.notice = record_notice;
    events.system_alert = record_alert;
    events.battle_active = report_battle;
    events.battle_message = record_battle_line;
    events.allegiances = record_allegiances;
    events.activity = record_activity;
    events.census = record_census;
    events.room_listed = record_room_listed;
    events.room_updated = record_room_updated;
    events.room_entered = record_room_entered;
    events.room_member = record_room_member;
    events.chat_message = record_chat_message;
    events.room_left = record_room_left;
    events.room_created = record_room_created;
    g_service.set_events(g_service.user, &events);
}

SessionLevel level() {
    return rover_session()->record.level;
}

void pump(PumpAction action) {
    g_fake.on_pump = action;
    g_service.pump(g_service.user);
}

void login(const char* user, const char* password) {
    const LoginRequest request{user, password, false, nullptr};
    CHECK(g_service.login(g_service.user, &request) == ServiceResult::pending);
}

void test_activation() {
    reset_all();
    rover_module_set_source(nullptr);
    CHECK(g_service.activate(g_service.user) == ActivationResult::module_missing);
    reset_all();
    g_fake.attach_ok = false;
    CHECK(g_service.activate(g_service.user) == ActivationResult::attach_failed);
    reset_all();
    g_fake.activate_ok = false;
    CHECK(g_service.activate(g_service.user) == ActivationResult::activate_failed);
    CHECK(level() == SessionLevel::offline && rover_session()->pump_enabled == 0);

    reset_all();
    pump(nullptr);
    CHECK(g_fake.pumps == 0); // no pumping before activation
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    CHECK(level() == SessionLevel::client_active && rover_session()->pump_enabled == 1);
    CHECK(g_reports.levels.size() == 1 && g_reports.levels[0] == SessionLevel::client_active);
    pump(nullptr);
    CHECK(g_fake.pumps == 1);
    CHECK(!g_service.connected(g_service.user));
}

void confirm_contact() {
    g_fake.progress(4, 0, 0, 0, nullptr);
    g_fake.progress(7, 0, 0, 0, nullptr);
    g_fake.progress(8, 0, 42, 7, nullptr);
}

void deliver_dossier() {
    g_fake.profile(true, reinterpret_cast<RoverPropertyBag*>(&g_fake));
}

void test_member_login() {
    reset_all();
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    login("kestrel", "secret");
    CHECK(g_fake.login_server.empty()); // slot 7 receives the row text, with no fallback
    CHECK(g_fake.login_user == "kestrel" && g_fake.login_password == "secret");
    CHECK(std::strcmp(rover_session()->record.user_name, "kestrel") == 0);
    pump(confirm_contact);
    CHECK(
        g_reports.statuses.size() == 2 && g_reports.statuses[0] == "Contact Established" &&
        g_reports.statuses[1] == "Verifying Clearance"
    );
    CHECK(level() == SessionLevel::logged_in && g_service.connected(g_service.user));
    CHECK(rover_session()->record.user_id == 42 && rover_session()->record.area_id == 7);
    CHECK(g_fake.profile_user == 42 && g_fake.profile != nullptr);
    pump(deliver_dossier);
    CHECK(level() == SessionLevel::dossier_loaded);
    CHECK(g_reports.levels.back() == SessionLevel::dossier_loaded);
    CHECK(std::strcmp(rover_session()->record.user_name, "Kestrel") == 0);
    CHECK(
        rover_session()->record.allegiance == 'C' &&
        rover_session()->record.matchmaking_enabled == 1
    );
    CHECK(g_reports.resets == 0 && g_fake.resets == 0);

    // Leaving while logged in logs out first; the confirmation releases Rover.
    g_service.close(g_service.user);
    CHECK(g_fake.logouts == 1 && g_fake.logout_user == 42 && g_fake.deactivates == 0);
    CHECK(rover_module_state()->bound);
    pump([] { g_fake.logged_out(); });
    CHECK(g_fake.deactivates == 1 && g_fake.detaches == 1);
    CHECK(!rover_module_state()->bound); // the unload waited for the pump to return
    CHECK(level() == SessionLevel::offline && rover_session()->pump_enabled == 0);
    CHECK(g_reports.levels.back() == SessionLevel::offline);
}

void test_guest_and_explicit_server() {
    reset_all();
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    const LoginRequest request{"bob", "", false, "Local Yard"};
    CHECK(g_service.login(g_service.user, &request) == ServiceResult::pending);
    CHECK(g_fake.login_server == "Local Yard" && g_fake.login_user == "bob");
    CHECK(g_fake.login_password.empty());
    CHECK(std::strcmp(rover_session()->record.user_name, "?_bob") == 0);
    g_fake.login_ok = false;
    CHECK(g_service.login(g_service.user, &request) == ServiceResult::pending);
    CHECK(g_fake.login_server == "Local Yard"); // the login ignores slot 7's bool
}

void fail_password() {
    g_fake.progress(8, 4, 0, 0, nullptr);
}

void fail_unknown() {
    g_fake.progress(8, 9, 0, 0, nullptr);
}

void fail_contact_silently() {
    g_fake.progress(4, 1, 0, 0, nullptr);
}

void fail_contact_with_reason() {
    g_fake.progress(4, 1, 0, 0, "Yard full");
}

void fail_relay() {
    g_fake.progress(7, 1, 0, 0, nullptr);
}

void test_login_failures() {
    reset_all();
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    login("kestrel", "wrong");
    pump(fail_password);
    CHECK(
        g_reports.statuses.size() == 1 &&
        g_reports.statuses[0] ==
            "The password you provided was not correct for the username.  Check the values and "
            "try again."
    );
    CHECK(g_reports.resets == 1 && g_fake.resets == 1 && level() == SessionLevel::client_active);

    g_reports.statuses.clear();
    pump(fail_unknown);
    CHECK(
        g_reports.statuses.size() == 1 &&
        g_reports.statuses[0] == "Unable to login to Boneyards, try again later."
    );

    g_reports.statuses.clear();
    pump(fail_contact_silently);
    CHECK(
        g_reports.statuses.size() == 2 && g_reports.statuses[0] == "Server Not responding" &&
        g_reports.statuses[1] == "Try a different server or come back later"
    );

    g_reports.statuses.clear();
    pump(fail_contact_with_reason);
    CHECK(g_reports.statuses.size() == 1 && g_reports.statuses[0] == "Yard full");

    g_reports.statuses.clear();
    pump(fail_relay);
    CHECK(g_reports.statuses.size() == 2 && g_reports.statuses[0] == "Transmision Relay Failure");
    CHECK(g_reports.resets == 5 && g_fake.resets == 5);

    // Other stages are not reported.
    g_reports.statuses.clear();
    g_fake.progress(1, 0, 0, 0, nullptr);
    CHECK(g_reports.statuses.empty() && g_reports.resets == 5);
}

void test_dossier_refused() {
    reset_all();
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    login("kestrel", "secret");
    pump(confirm_contact);
    pump([] { g_fake.profile(false, nullptr); });
    CHECK(g_reports.statuses.back() == "Unable to access dossier, Login Aborted");
    CHECK(g_fake.logouts == 1 && g_fake.logout_user == 42 && level() == SessionLevel::logged_in);
    pump([] { g_fake.logged_out(); });
    CHECK(g_reports.resets == 1 && g_fake.resets == 1 && level() == SessionLevel::client_active);
}

void test_cancel() {
    reset_all();
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    login("kestrel", "secret");
    g_service.cancel_login(g_service.user);
    CHECK(g_reports.resets == 1 && g_fake.resets == 1 && g_fake.logouts == 0);
    login("kestrel", "secret");
    pump(confirm_contact);
    g_service.cancel_login(g_service.user);
    CHECK(g_fake.logouts == 1 && g_reports.resets == 1);
    pump([] { g_fake.logged_out(); });
    CHECK(g_reports.resets == 2 && level() == SessionLevel::client_active);
}

// The extension holds the inactive-loop request while this is true.
void test_live_holds_until_logout_or_disconnect() {
    reset_all();
    CHECK(!rover_session_live(nullptr));
    CHECK(!rover_session_live(rover_session()));
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    CHECK(!rover_session_live(rover_session()));

    login("kestrel", "secret");
    CHECK(!rover_session_live(rover_session()));
    pump(confirm_contact);
    CHECK(rover_session_live(rover_session()));
    pump(deliver_dossier);
    CHECK(rover_session_live(rover_session()));

    // Logout is sent, and the session stays live until that finishes.
    g_service.logout(g_service.user);
    CHECK(g_fake.logouts == 1 && rover_session_live(rover_session()));
    pump([] { g_fake.logged_out(); });
    CHECK(level() == SessionLevel::client_active && !rover_session_live(rover_session()));

    // A login that never confirms does not hold.
    login("kestrel", "wrong");
    pump(fail_password);
    CHECK(level() == SessionLevel::client_active && !rover_session_live(rover_session()));

    // A dropped link releases. A later close stays released.
    login("kestrel", "secret");
    pump(confirm_contact);
    pump(deliver_dossier);
    CHECK(rover_session_live(rover_session()));
    pump([] { g_fake.disconnect(0, 0, 0); });
    CHECK(level() == SessionLevel::disconnected && !rover_session_live(rover_session()));
    g_service.close(g_service.user);
    CHECK(level() == SessionLevel::offline && !rover_session_live(rover_session()));
}

// A yard master the resolve step named, then a connect that is still failing, is a message box
// that names the host, and LOGIN can be pressed again.
void test_yard_master_unreachable() {
    reset_all();
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    login("kestrel", "secret");
    g_fake.progress(1, 1, 0, 0, "nope.invalid");
    g_fake.disconnect(1, 1, 0);
    CHECK(g_reports.notices.size() == 1);
    CHECK(
        g_reports.notices[0] ==
        "Could not reach nope.invalid. Check your connection, or try again later."
    );
    CHECK(g_reports.resets == 1 && level() == SessionLevel::client_active);

    // A connecting failure that never named a host keeps the original wording.
    g_reports.notices.clear();
    g_reports.resets = 0;
    login("kestrel", "secret");
    g_fake.disconnect(1, 1, 0);
    CHECK(
        g_reports.notices.size() == 1 &&
        g_reports.notices[0] == "Unable to connect to\nlogin server.\n\nPlease try again later."
    );
    CHECK(g_reports.resets == 1);
}

void test_disconnects_and_alerts() {
    reset_all();
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    login("kestrel", "secret");
    pump(confirm_contact);
    pump(deliver_dossier);
    pump([] { g_fake.disconnect(0, 0, 0); });
    CHECK(
        level() == SessionLevel::disconnected && rover_session()->record.matchmaking_enabled == 0
    );
    CHECK(
        g_reports.notices.size() == 1 &&
        g_reports.notices[0] == "An unexpected network\ndisconnect has occurred."
    );
    pump([] { g_fake.disconnect(1, 1, 0); });
    CHECK(
        g_reports.notices.back() == "Unable to connect to\nlogin server.\n\nPlease try again later."
    );
    pump([] { g_fake.disconnect(0, 1, 0); });
    CHECK(
        g_reports.notices.back() ==
        "Unable to connect to\nBoneyards server.\n\nPlease try again later."
    );
    CHECK(level() == SessionLevel::disconnected);

    g_reports.in_battle = true;
    const std::size_t notices = g_reports.notices.size();
    pump([] { g_fake.disconnect(0, 0, 0); });
    CHECK(g_reports.notices.size() == notices && g_reports.battle_lines.size() == 3);
    CHECK(g_reports.battle_lines[0] == "Boneyards: WARNING! DISCONNECTED FROM THE BONEYARDS!");

    // Only a page and a notification read their bag.
    pump([] { g_fake.alert(1, "Server going down", &g_fake); });
    CHECK(
        g_reports.alert_kind == 1 && g_reports.alert_text == "Server going down" &&
        g_reports.alert_sender == 0 && g_reports.alert_type.empty()
    );
    pump([] {
        RoverPropertyBag* bag = g_table.create_property_bag();
        RoverString key{};
        rover_string_assign(&key, "user_id", 7);
        g_table.set_field_uint(bag, &key, 0x80000009U);
        rover_string_release(&key, true);
        g_fake.alert(2, "<Page from guest> hi", bag);
        g_table.destroy_property_bag(bag);
    });
    CHECK(
        g_reports.alert_kind == 2 && g_reports.alert_text == "<Page from guest> hi" &&
        g_reports.alert_sender == 0x80000009U && g_reports.alert_type.empty()
    );
    pump([] {
        RoverPropertyBag* bag = g_table.create_property_bag();
        g_fake.alert(2, "<Page> no sender", bag);
        g_table.destroy_property_bag(bag);
    });
    CHECK(g_reports.alert_kind == 2 && g_reports.alert_sender == 0);

    // Closing a disconnected session releases Rover at once.
    g_service.close(g_service.user);
    CHECK(g_fake.logouts == 0 && g_fake.deactivates == 1 && g_fake.detaches == 1);
    CHECK(!rover_module_state()->bound && level() == SessionLevel::offline);
}

void test_file_placement() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "oa-boneyards-client-test";
    fs::remove_all(root);
    fs::create_directories(root / "Boneyards");
    std::ofstream(root / "Boneyards" / "SERV_SAV.BYD") << "installed";
    std::ofstream(root / "override.byd") << "override";
    auto read_back = [](const char* path) {
        const RoverPlatform* platform = rover_session_platform();
        RoverFile* file = platform->file_open(path);
        if (file == nullptr)
            return std::string("(missing)");
        char buffer[32] = {};
        const int32_t length = platform->file_read(file, buffer, sizeof buffer - 1);
        platform->file_close(file);
        return std::string(buffer, length > 0 ? static_cast<std::size_t>(length) : 0);
    };
    CHECK(!rover_session_configured() && rover_session_platform() == nullptr);
    rover_session_configure(root.string().c_str(), "");
    CHECK(rover_session_configured());
    CHECK(read_back("./boneyards/serv_sav.byd") == "installed");
    CHECK(read_back("./boneyards/server.byd") == "(missing)");
    rover_session_configure(root.string().c_str(), (root / "override.byd").string().c_str());
    CHECK(read_back("./boneyards/serv_sav.byd") == "override");
    CHECK(read_back(".\\Boneyards\\Serv_Sav.byd") == "override");
    rover_use_platform(nullptr);
    fs::remove_all(root);
}

std::string file_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool create_through_rover(const char* path, const char* text) {
    const RoverPlatform* platform = rover_session_platform();
    RoverFile* file = platform->file_create(path);
    if (file == nullptr)
        return false;
    const int32_t length = static_cast<int32_t>(std::strlen(text));
    const bool written = platform->file_write(file, text, length) == length;
    platform->file_close(file);
    return written;
}

void write_player_copy(const std::filesystem::path& path, const char* text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

std::string read_through_rover(const char* path) {
    const RoverPlatform* platform = rover_session_platform();
    RoverFile* file = platform->file_open(path);
    if (file == nullptr)
        return "(missing)";
    char buffer[256] = {};
    const int32_t length = platform->file_read(file, buffer, sizeof buffer - 1);
    platform->file_close(file);
    return std::string(buffer, length > 0 ? static_cast<std::size_t>(length) : 0);
}

// The files Rover creates go under the player's boneyards directory. The
// game folder is left as it was, including while no player folder is
// chosen. A substituted server list is what this run reads; the rewrite
// lands in the player's folder.
void test_file_creation() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "oa-boneyards-create-test";
    fs::remove_all(root);
    fs::create_directories(root / "Boneyards");
    const fs::path player = root / "player";
    fs::create_directories(player);
    const fs::path override_list = root / "override.byd";
    std::ofstream(override_list) << "override";
    std::ofstream(root / "Boneyards" / "serv_sav.byd") << "installed";
    std::ofstream(root / "Boneyards" / "server.byd") << "game-1998";
    const fs::path expansion = root / "expansion";
    fs::create_directories(expansion / "boneyards" / "Help");
    std::ofstream(expansion / "boneyards" / "Help" / "Help.def") << "help-shipped";
    std::ofstream(expansion / "boneyards" / "Server.byd")
        << "bypacific\tboneyards.net\t2\tUSA, West Coast\t\r\n";

    rover_session_configure(root.string().c_str(), "");
    CHECK(!create_through_rover("./boneyards/serv_sav.byd", "saved"));
    CHECK(!create_through_rover("http.log", "log"));
    CHECK(file_text((root / "Boneyards" / "serv_sav.byd").string()) == "installed");
    CHECK(!fs::exists(root / "http.log"));
    CHECK(!fs::exists(root / "boneyards" / "http.log"));

    rover_session_set_player_folder(player.string().c_str());
    rover_session_set_expansion_folder(expansion.string().c_str());
    CHECK(read_through_rover("./boneyards/serv_sav.byd") == "installed");
    CHECK(read_through_rover("./boneyards/Help/Help.def") == "help-shipped");
    write_player_copy(player / "boneyards" / "Help" / "Help.def", "player-help");
    CHECK(read_through_rover("./boneyards/Help/Help.def") == "player-help");
    fs::remove(player / "boneyards" / "Help" / "Help.def");
    CHECK(read_through_rover("./boneyards/Help/Help.def") == "help-shipped");
    CHECK(read_through_rover("./boneyards/server.byd") == "(missing)");
    CHECK(read_through_rover(".\\Boneyards\\Server.BYD") == "(missing)");
    write_player_copy(player / "boneyards" / "server.byd", "player-shipped");
    CHECK(read_through_rover("./boneyards/server.byd") == "player-shipped");
    fs::remove(player / "boneyards" / "server.byd");
    CHECK(read_through_rover("./boneyards/server.byd") == "(missing)");

    CHECK(create_through_rover("./boneyards/serv_sav.byd", "saved"));
    CHECK(file_text((player / "Boneyards" / "serv_sav.byd").string()) == "saved");
    CHECK(read_through_rover("./boneyards/serv_sav.byd") == "saved");
    CHECK(file_text((root / "Boneyards" / "serv_sav.byd").string()) == "installed");
    CHECK(create_through_rover("http.log", "log"));
    CHECK(file_text((player / "Boneyards" / "http.log").string()) == "log");
    CHECK(!fs::exists(root / "http.log"));
    const fs::path absolute = root / "absolute.txt";
    CHECK(create_through_rover(absolute.string().c_str(), "absolute"));
    CHECK(file_text(absolute.string()) == "absolute");

    rover_session_configure(root.string().c_str(), override_list.string().c_str());
    rover_session_set_player_folder(player.string().c_str());
    CHECK(read_through_rover("./boneyards/serv_sav.byd") == "override");
    CHECK(create_through_rover("./boneyards/serv_sav.byd", "rewritten"));
    CHECK(file_text(override_list.string()) == "override");
    CHECK(file_text((player / "Boneyards" / "serv_sav.byd").string()) == "rewritten");
    CHECK(file_text((root / "Boneyards" / "serv_sav.byd").string()) == "installed");
    rover_session_configure(root.string().c_str(), "");
    rover_session_set_player_folder(player.string().c_str());
    CHECK(read_through_rover("./boneyards/serv_sav.byd") == "rewritten");
    rover_use_platform(nullptr);
    fs::remove_all(root);
}

// oa-rover itself over the install's serv_sav.byd: the selected server is
// its 'D' entry, and the fetch worker's http.log is created under the
// player's boneyards directory.
void test_selected_server_from_rover() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "oa-boneyards-selected-test";
    fs::remove_all(root);
    fs::create_directories(root / "Boneyards");
    const fs::path player = root / "player";
    fs::create_directories(player);
    std::ofstream(root / "Boneyards" / "serv_sav.byd", std::ios::binary)
        << "alpha\tclusters.coreprime.net\t3\tCorePrime Alpha Test\tD\r\n";
    const bool stray_log = fs::exists("http.log");
    reset_all();
    rover_module_set_source(rover_get_interface);
    rover_session_configure(root.string().c_str(), "");
    rover_session_set_player_folder(player.string().c_str());
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    CHECK(std::strcmp(rover_session_selected_server(), "CorePrime Alpha Test") == 0);
    g_service.close(g_service.user);
    CHECK(!rover_module_state()->bound);
    CHECK(fs::exists(player / "Boneyards" / "http.log"));
    CHECK(!fs::exists(root / "http.log"));
    CHECK(stray_log || !fs::exists("http.log"));
    CHECK(
        file_text((root / "Boneyards" / "serv_sav.byd").string()) ==
        "alpha\tclusters.coreprime.net\t3\tCorePrime Alpha Test\tD\r\n"
    );
    rover_use_platform(nullptr);
    fs::remove_all(root);
}

void query_player_folder(void* context, char* out, std::size_t capacity) {
    const auto* folder = static_cast<const std::string*>(context);
    if (out == nullptr || capacity == 0)
        return;
    out[0] = '\0';
    if (folder == nullptr || folder->size() + 1 > capacity)
        return;
    std::memcpy(out, folder->data(), folder->size() + 1);
}

std::map<std::string, std::string> tree_files(const std::filesystem::path& root) {
    std::map<std::string, std::string> files;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(root, error), last; !error && it != last;
         it.increment(error)) {
        if (!it->is_regular_file())
            continue;
        const std::filesystem::path relative = std::filesystem::relative(it->path(), root, error);
        if (error)
            break;
        files[relative.generic_string()] = file_text(it->path().string());
    }
    return files;
}

// Drops write permission from a tree, and puts it back on destruction.
struct WriteLock {
    std::filesystem::path root;
    bool held = false;

    void lock() {
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(root, error), last;
             !error && it != last;
             it.increment(error))
            std::filesystem::permissions(
                it->path(),
                std::filesystem::perms::owner_write,
                std::filesystem::perm_options::remove,
                error
            );
        std::filesystem::permissions(
            root, std::filesystem::perms::owner_write, std::filesystem::perm_options::remove, error
        );
        held = true;
    }

    void unlock() {
        if (!held)
            return;
        std::error_code error;
        std::filesystem::permissions(
            root, std::filesystem::perms::owner_write, std::filesystem::perm_options::add, error
        );
        for (std::filesystem::recursive_directory_iterator it(root, error), last;
             !error && it != last;
             it.increment(error))
            std::filesystem::permissions(
                it->path(),
                std::filesystem::perms::owner_write,
                std::filesystem::perm_options::add,
                error
            );
        held = false;
    }

    ~WriteLock() { unlock(); }
};

// A session against a read-only game folder writes the fetch log, the saved
// server list and the people lists under the player's folder, and reads the
// game folder's copy when the player has none.
void test_read_only_game_folder() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "oa-boneyards-readonly-test";
    fs::remove_all(root);
    const fs::path install = root / "install";
    const fs::path player = root / "player";
    fs::create_directories(install / "Boneyards" / "peoplelists");
    fs::create_directories(player);
    const std::string saved = "alpha\texample.net\t0\tRead Only Yard\tD\r\n";
    const std::string shipped = "ship\texample.net\t0\tShipped Yard\t\r\n";
    std::ofstream(install / "Boneyards" / "serv_sav.byd", std::ios::binary) << saved;
    std::ofstream(install / "Boneyards" / "server.byd", std::ios::binary) << shipped;
    std::ofstream(install / "Boneyards" / "peoplelists" / "Silenced.lst", std::ios::binary)
        << "4\r\n";
    const std::map<std::string, std::string> before = tree_files(install);

    WriteLock lock;
    lock.root = install;
    lock.lock();
    std::string player_text = player.string();
    reset_all();
    rover_module_set_source(rover_get_interface);
    rover_session_configure(install.string().c_str(), "");
    rover_session_set_player_folder_query(query_player_folder, &player_text);

    CHECK(read_through_rover("./boneyards/serv_sav.byd") == saved);
    CHECK(read_through_rover("./boneyards/server.byd") == "(missing)");
    PeopleLists lists{};
    people_lists_init(&lists);
    CHECK(people_list_contains(&lists, kSilencedList, 4));
    CHECK(!people_list_contains(&lists, kSilencedList, 8));
    write_player_copy(player / "boneyards" / "server.byd", "player-shipped");
    CHECK(read_through_rover("./boneyards/server.byd") == "player-shipped");
    fs::remove(player / "boneyards" / "server.byd");
    CHECK(read_through_rover("./boneyards/server.byd") == "(missing)");

    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    CHECK(std::strcmp(rover_session_selected_server(), "Read Only Yard") == 0);
    g_service.close(g_service.user);
    CHECK(fs::exists(player / "Boneyards" / "http.log"));
    CHECK(!fs::exists(install / "http.log"));
    CHECK(!fs::exists(install / "boneyards" / "http.log"));

    people_list_insert(&lists, kSilencedList, 8);
    people_lists_release(&lists);
    CHECK(
        file_text((player / "Boneyards" / "peoplelists" / "Silenced.lst").string()) == "4\r\n8\r\n"
    );

    RoverServerList servers{};
    rover_servers_add(servers, "beta", "example.net", 0, "Learned Yard", false, true);
    CHECK(!rover_servers_write(servers, "./boneyards/serv_sav.byd"));
    rover_servers_clear(servers);
    CHECK(
        read_through_rover("./boneyards/serv_sav.byd") ==
        "beta\texample.net\t0\tLearned Yard\tD\r\n"
    );
    CHECK(
        file_text((player / "Boneyards" / "serv_sav.byd").string()) ==
        "beta\texample.net\t0\tLearned Yard\tD\r\n"
    );

    lock.unlock();
    CHECK(tree_files(install) == before);
    rover_use_platform(nullptr);
    fs::remove_all(root);
}

void remember_listed_server(void* context, const char* location) {
    static_cast<std::vector<std::string>*>(context)->emplace_back(
        location != nullptr ? location : ""
    );
}

// The expansion's 1998 server.byd is not a server list. With no player
// copy and no saved list, Rover attaches on the baked Alpha entry. The
// player's own server.byd and serv_sav.byd still win.
void test_expansion_server_list_uses_baked_alpha() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "oa-boneyards-baked-alpha-test";
    fs::remove_all(root);
    const fs::path expansion = root / "expansion";
    const fs::path player = root / "player";
    fs::create_directories(expansion / "boneyards");
    fs::create_directories(player);
    std::ofstream(expansion / "boneyards" / "server.byd", std::ios::binary)
        << "bypacific\tboneyards.net\t2\tUSA, West Coast\t\r\n";

    reset_all();
    rover_module_set_source(rover_get_interface);
    rover_session_configure((root / "install").string().c_str(), "");
    rover_session_set_player_folder(player.string().c_str());
    rover_session_set_expansion_folder(expansion.string().c_str());
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    std::vector<std::string> locations;
    g_service.list_servers(g_service.user, remember_listed_server, &locations);
    CHECK(locations.size() == 1 && locations[0] == "CorePrime (Alpha)");
    CHECK(std::strcmp(rover_session_selected_server(), "CorePrime (Alpha)") == 0);
    CHECK(
        g_rover_state != nullptr && g_rover_state->servers.count == 1 &&
        std::strcmp(g_rover_state->servers.entries[0].name, "alpha") == 0 &&
        std::strcmp(g_rover_state->servers.entries[0].domain, "clusters.coreprime.net") == 0 &&
        g_rover_state->servers.entries[0].cluster_size == 2 &&
        g_rover_state->servers.entries[0].selected
    );
    g_service.close(g_service.user);

    write_player_copy(
        player / "Boneyards" / "server.byd", "mine\texample.net\t0\tPlayer Yard\tD\r\n"
    );
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    locations.clear();
    g_service.list_servers(g_service.user, remember_listed_server, &locations);
    CHECK(locations.size() == 1 && locations[0] == "Player Yard");
    CHECK(std::strcmp(rover_session_selected_server(), "Player Yard") == 0);
    g_service.close(g_service.user);

    write_player_copy(
        player / "Boneyards" / "serv_sav.byd", "saved\texample.net\t0\tSaved Yard\tD\r\n"
    );
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    locations.clear();
    g_service.list_servers(g_service.user, remember_listed_server, &locations);
    CHECK(locations.size() == 1 && locations[0] == "Saved Yard");
    CHECK(std::strcmp(rover_session_selected_server(), "Saved Yard") == 0);
    g_service.close(g_service.user);
    rover_use_platform(nullptr);
    fs::remove_all(root);
}

// A server named by host becomes a one-entry list that selects it, and the
// login dialog's default entry is that host while Rover names none.
void test_host_server_list() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "oa-boneyards-host-list-test";
    fs::remove_all(root);
    fs::create_directories(root);
    const std::string existing = (root / "servers.byd").string();
    std::ofstream(existing) << "listed";
    std::string error;
    CHECK(rover_session_server_list(existing, &error) == existing && error.empty());
    CHECK(rover_session_server_list((root / "missing.byd").string(), &error).empty());
    CHECK(error.find("server list not found") == 0);
    error.clear();
    CHECK(rover_session_server_list("servers.BYD", &error).empty() && !error.empty());
    error.clear();
    CHECK(rover_session_server_list("127.0.0.1:9111", &error).empty());
    CHECK(error.find("port 9110") != std::string::npos);
    error.clear();
    CHECK(rover_session_server_list("bad host", &error).empty() && !error.empty());

    error.clear();
    const std::string list = rover_session_server_list("127.0.0.1:9110", &error);
    CHECK(!list.empty() && error.empty());
    CHECK(file_text(list) == "127.0.0.1\t \t0\tCommand Line Server\tD\r\n");
    CHECK(rover_session_server_list("yard.example.net", &error) != list);

    reset_all();
    g_table.find_current_area = fake_no_current_area;
    rover_session_configure(root.string().c_str(), existing.c_str());
    CHECK(!rover_session_uses_host_list());
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    CHECK(std::strcmp(rover_session_selected_server(), "") == 0);
    const std::string yard = rover_session_server_list("yard.example.net", &error);
    rover_session_configure(root.string().c_str(), yard.c_str());
    CHECK(rover_session_uses_host_list());
    login("kestrel", "secret");
    CHECK(g_fake.login_server.empty()); // slot 7 receives the selected row, never a fallback
    g_table.find_current_area = fake_find_current_area;
    CHECK(std::strcmp(rover_session_selected_server(), "CorePrime Alpha Test") == 0);
    rover_use_platform(nullptr);
    fs::remove(list);
    fs::remove(yard);
    fs::remove_all(root);
}

void test_auto_update_stand_in() {
    MaiaUpdateConsole console{};
    console.completed = 1;
    console.start_result = -1;
    maia_update_init(&console);
    // Opening the update console sets an 8-cycle login countdown and a 0 start result.
    CHECK(console.login_countdown == 8 && console.start_result == 0 && console.completed == 0);
    maia_update_open(&console);
    CHECK(console.start_result == kMaiaStartOk && console.login_countdown == kMaiaLoginDelay);
    int32_t idles = 1;
    while (!maia_update_idle(&console) && idles < 100)
        ++idles;
    CHECK(console.completed == 1 && idles == kMaiaLoginDelay);
}

// The war-map entries call their slots with the caller's arguments; the
// feed callbacks they hand Rover raise the events with the fields in order.
void test_war_map_entries() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "oa-boneyards-war-map-test";
    fs::remove_all(root);
    fs::create_directories(root / "Boneyards");
    reset_all();
    rover_session_configure(root.string().c_str(), "");
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    CHECK(std::strcmp(g_service.area_name(g_service.user), "alpha") == 0);
    SectorMapHeader header{};
    SectorMapReader* reader =
        g_service.open_sector_map(g_service.user, "Boneyards\\alpha_map.gwm", &header);
    CHECK(reader == reinterpret_cast<SectorMapReader*>(&g_reader_storage));
    // The caller's path is handed to the slot; the placed platform resolves it.
    CHECK(g_fake.map_path == "Boneyards\\alpha_map.gwm");
    CHECK(header.sector_id == 45 && std::strcmp(header.sector_name, "New Hub") == 0);
    CHECK(header.width == 2000 && header.height == 1400);
    CHECK(header.planet_count == 25 && header.connector_count == 32);
    static SectorPlanetRecord planets[2];
    ConnectorRecord connectors[1]{};
    CHECK(g_service.read_sector_map(g_service.user, reader, 2000, 1400, planets, 2, connectors, 1));
    CHECK(g_fake.reader == reinterpret_cast<RoverSectorReader*>(&g_reader_storage));
    CHECK(g_fake.read_width == 2000 && g_fake.read_height == 1400);
    CHECK(g_fake.read_planets == reinterpret_cast<RoverSectorPlanet*>(planets));
    CHECK(g_fake.read_planet_count == 2 && g_fake.read_connector_count == 1);
    CHECK(g_fake.read_connectors == reinterpret_cast<RoverSectorConnector*>(connectors));
    CHECK(planets[0].id == 1803 && std::strcmp(planets[0].desc, "Fuel") == 0);
    CHECK(planets[0].watching == 2 && connectors[0].planet2 == 1804);
    g_service.close_sector_map(g_service.user, reader);
    CHECK(g_fake.closed == reinterpret_cast<RoverSectorReader*>(&g_reader_storage));

    g_service.request_allegiance(g_service.user);
    g_service.request_census(g_service.user, 0);
    g_service.request_census(g_service.user, 1801);
    CHECK(g_fake.census_area == 1801);
    CHECK((g_fake.war_calls == std::vector<std::string>{"72", "34", "35", "36", "39", "38", "38"}));
    g_fake.allegiances("CCCCIIIAAACCCCCCAAAAAAAAC");
    CHECK(g_reports.allegiances == "CCCCIIIAAACCCCCCAAAAAAAAC");
    g_fake.activity("0123");
    CHECK(g_reports.activity == "0123");
    g_fake.census(1801, 3, 2, 1, 4, 6, 5);
    CHECK(g_reports.census.size() == 1);
    const CensusReport& report = g_reports.census[0];
    CHECK(report.area_id == 1801 && report.population == 3 && report.activity == 2);
    CHECK(report.in_progress == 1 && report.forming == 4);
    CHECK(report.core_wins == 6 && report.arm_wins == 5);

    // Without a Rover table the wrappers answer nothing.
    rover_module_unload();
    CHECK(g_service.area_name(g_service.user) == nullptr);
    CHECK(g_service.open_sector_map(g_service.user, "x.gwm", &header) == nullptr);
    CHECK(
        !g_service.read_sector_map(g_service.user, reader, 2000, 1400, planets, 2, connectors, 1)
    );
    rover_use_platform(nullptr);
    fs::remove_all(root);
}

// The offline service answers no server name and no war map, and ignores
// the feed requests.
void test_offline_war_map() {
    const BoneyardsService offline = boneyards_offline_service(nullptr);
    CHECK(offline.area_name(offline.user) == nullptr);
    SectorMapHeader header{};
    header.width = 7;
    CHECK(offline.open_sector_map(offline.user, "Boneyards\\alpha_map.gwm", &header) == nullptr);
    CHECK(header.width == 7);
    static SectorPlanetRecord planets[1];
    ConnectorRecord connectors[1]{};
    CHECK(!offline.read_sector_map(offline.user, nullptr, 2000, 1400, planets, 1, connectors, 1));
    offline.close_sector_map(offline.user, nullptr);
    offline.request_allegiance(offline.user);
    offline.request_census(offline.user, 0);
}

void record_locate(void* context, const LocateReport* report) {
    static_cast<std::vector<LocateReport>*>(context)->push_back(*report);
}

// Stores one typed field in a bag through Rover's own slots 49-51.
void put_uint(RoverPropertyBag* bag, const char* key, uint32_t value) {
    RoverString name{};
    rover_string_assign(&name, key, static_cast<uint32_t>(std::strlen(key)));
    g_table.set_field_uint(bag, &name, value);
    rover_string_release(&name, true);
}

void put_int(RoverPropertyBag* bag, const char* key, int32_t value) {
    RoverString name{};
    rover_string_assign(&name, key, static_cast<uint32_t>(std::strlen(key)));
    g_table.set_field_int(bag, &name, value);
    rover_string_release(&name, true);
}

void put_text(RoverPropertyBag* bag, const char* key, const char* text) {
    RoverString value{};
    rover_string_assign(&value, text, static_cast<uint32_t>(std::strlen(text)));
    CHECK(rover_call_set_bag_string(bag, key, &value));
    rover_string_release(&value, true);
}

// +locate: slot 65 gets the name and Rover's locator callback, and the
// answer reaches the console with the fields the game reads from its bag.
void test_locate_user() {
    reset_all();
    const RoverInterface* real = nullptr;
    rover_get_interface(&real);
    g_table.get_field_int = real->get_field_int;
    g_table.get_field_string = real->get_field_string;
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    std::vector<LocateReport> located;
    CHECK(
        g_service.locate_user(g_service.user, "ghost", record_locate, &located) ==
        ServiceResult::pending
    );
    CHECK(g_fake.locate_name == "ghost" && g_fake.located != nullptr);

    RoverPropertyBag* bag = rover_call_create_property_bag();
    put_uint(bag, "area_id", 1801);
    put_uint(bag, "room_id", 5);
    put_text(bag, "game_id", "g-77");
    put_int(bag, "pass_req", 1);
    put_text(bag, "game_name", "Duel");
    put_text(bag, "area_name", "Hub");
    put_text(bag, "room_name", "Frequency 5");
    put_text(bag, "server_name", "Alpha");
    put_text(bag, "command", "user_located");
    g_fake.located(3, 42, "Ghost", 7, bag);
    CHECK(located.size() == 1);
    const LocateReport& report = located[0];
    CHECK(report.status == 3 && std::strcmp(report.user_name, "Ghost") == 0);
    CHECK(report.area_id == 1801 && report.room_id == 5 && report.pass_required == 1);
    CHECK(std::strcmp(report.game_id, "g-77") == 0 && std::strcmp(report.game_name, "Duel") == 0);
    CHECK(std::strcmp(report.area_name, "Hub") == 0);
    CHECK(std::strcmp(report.room_name, "Frequency 5") == 0);
    CHECK(std::strcmp(report.server_name, "Alpha") == 0);
    rover_call_destroy_property_bag(bag);

    // Fields of another type or missing read as 0 and empty.
    RoverPropertyBag* sparse = rover_call_create_property_bag();
    put_int(sparse, "area_id", 1801);
    put_uint(sparse, "pass_req", 1);
    g_fake.located(5, 0, "nobody", -1, sparse);
    CHECK(located.size() == 2);
    const LocateReport& missing = located[1];
    CHECK(missing.status == 5 && std::strcmp(missing.user_name, "nobody") == 0);
    CHECK(missing.area_id == 0 && missing.room_id == 0 && missing.pass_required == 0);
    CHECK(missing.game_id[0] == '\0' && missing.server_name[0] == '\0');
    rover_call_destroy_property_bag(sparse);

    const BoneyardsService offline = boneyards_offline_service(nullptr);
    CHECK(
        offline.locate_user(offline.user, "ghost", record_locate, &located) ==
        ServiceResult::not_connected
    );
    rover_module_unload();
}

// What a profile request's callbacks read from the fields they were handed:
// the user id, login name and icon of a profile (and a text it wrote back),
// the user id and rank of a ladder standing.
struct ProfileTaker {
    std::vector<std::string> events;
};

std::string field_text(const screens::ProfileFields* f, const char* key) {
    char text[0x40] = "-";
    f->get_string(f->user, key, text, sizeof text);
    return text;
}

std::string field_user(const screens::ProfileFields* f) {
    uint32_t user_id = 0;
    return f->get_uint(f->user, "user_id", &user_id) ? std::to_string(user_id) : "-";
}

void take_profile(void* context, bool ok, const screens::ProfileFields* f) {
    int32_t icon = -1;
    f->get_int(f->user, "ICON", &icon);
    f->set_string(f->user, "NUMPLAYERS_TEXT", "Doesn't Matter");
    static_cast<ProfileTaker*>(context)->events.push_back(
        std::string(ok ? "profile " : "failed ") + field_user(f) + " " +
        field_text(f, "LOGINNAME") + " " + std::to_string(icon) + " " +
        field_text(f, "NUMPLAYERS_TEXT")
    );
}

void take_ladder(void* context, int32_t ladder_id, int32_t subid, const screens::ProfileFields* f) {
    static_cast<ProfileTaker*>(context)->events.push_back(
        "ladder " + std::to_string(ladder_id) + "/" + std::to_string(subid) + " " + field_user(f) +
        " " + field_text(f, "RANK")
    );
}

RoverPropertyBag* profile_bag(uint32_t user_id, const char* name, int32_t icon) {
    RoverPropertyBag* bag = rover_call_create_property_bag();
    put_uint(bag, "user_id", user_id);
    put_text(bag, "LOGINNAME", name);
    put_int(bag, "ICON", icon);
    return bag;
}

// The player pane and the Profile console: slot 63 gets each user id with
// Rover's two profile callbacks; replies reach the request whose user id
// their bag holds, through fields read and written in that bag, while more
// than one is out; a reply without a user id belongs to the latest request,
// and one for a request already answered is dropped. The player's own
// profile refreshes the session's dossier fields.
void test_profile_requests() {
    reset_all();
    const RoverInterface* real = nullptr;
    rover_get_interface(&real);
    g_table.get_field_int = real->get_field_int;
    g_table.get_field_string = real->get_field_string;
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    ProfileTaker pane;
    ProfileTaker console;
    CHECK(
        g_service.request_profile(g_service.user, 42, take_profile, take_ladder, &pane) ==
        ServiceResult::pending
    );
    CHECK(g_fake.profile_user == 42 && g_fake.profile != nullptr && g_fake.ladder != nullptr);
    const RoverProfileFn rover_profile = g_fake.profile;
    const RoverLadderProfileFn rover_ladder = g_fake.ladder;
    CHECK(
        g_service.request_profile(g_service.user, 7, take_profile, take_ladder, &console) ==
        ServiceResult::pending
    );
    CHECK(g_fake.profile_users.size() == 2 && g_fake.profile_user == 7);
    CHECK(g_fake.profile == rover_profile && g_fake.ladder == rover_ladder);

    RoverPropertyBag* duel = rover_call_create_property_bag();
    put_uint(duel, "user_id", 42);
    put_text(duel, "RANK", "12");
    rover_ladder(1, 0, duel);
    RoverPropertyBag* own = profile_bag(7, "Console", 3);
    rover_profile(true, own);
    RoverPropertyBag* other = profile_bag(42, "Sentinel", 18);
    rover_profile(true, other);
    CHECK(pane.events.size() == 2 && console.events.size() == 1);
    if (pane.events.size() == 2 && console.events.size() == 1) {
        CHECK(pane.events[0] == "ladder 1/0 42 12");
        CHECK(pane.events[1] == "profile 42 Sentinel 18 Doesn't Matter");
        CHECK(console.events[0] == "profile 7 Console 3 Doesn't Matter");
    }
    // Answered: another reply for 42 reaches no one.
    rover_profile(true, other);
    rover_ladder(1, 0, duel);
    CHECK(pane.events.size() == 2 && console.events.size() == 1);
    rover_call_destroy_property_bag(duel);
    rover_call_destroy_property_bag(own);
    rover_call_destroy_property_bag(other);

    // A failed reply with no fields goes to the latest request.
    g_service.request_profile(g_service.user, 42, take_profile, take_ladder, &pane);
    g_service.request_profile(g_service.user, 7, take_profile, take_ladder, &console);
    RoverPropertyBag* empty = rover_call_create_property_bag();
    rover_profile(false, empty);
    CHECK(console.events.size() == 2 && console.events.back() == "failed - - -1 Doesn't Matter");
    CHECK(pane.events.size() == 2);
    rover_call_destroy_property_bag(empty);

    // The player's own profile refreshes the session as the dossier does.
    rover_session()->record.user_id = 42;
    rover_session()->record.allegiance = 'U';
    const std::size_t published = g_reports.levels.size();
    RoverPropertyBag* self = profile_bag(42, "Sentinel", 18);
    put_text(self, "ALLEGIANCE", "arm");
    put_int(self, "USER_STATUS", 5);
    rover_profile(true, self);
    CHECK(pane.events.size() == 3);
    CHECK(
        rover_session()->record.allegiance == 'A' &&
        rover_session()->record.matchmaking_enabled == 1
    );
    CHECK(std::strcmp(rover_session()->record.user_name, "Sentinel") == 0);
    CHECK(g_reports.levels.size() == published + 1);
    rover_call_destroy_property_bag(self);

    const BoneyardsService offline = boneyards_offline_service(nullptr);
    CHECK(
        offline.request_profile(offline.user, 42, take_profile, take_ladder, &pane) ==
        ServiceResult::not_connected
    );
    rover_module_unload();
}

// The chat room entries call slots 9 to 13, 41 and 69 with Rover's argument
// order (room before area) and hand it the same four room callbacks for an
// entry and a switch; the callbacks raise the room events with the fields
// in order; a refused request fails.
void test_chat_room_entries() {
    reset_all();
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    CHECK(g_service.request_room_list(g_service.user, 1801) == ServiceResult::pending);
    CHECK(g_service.join_room(g_service.user, 1801, 13, nullptr, "Calm") == ServiceResult::pending);
    const RoverRoomEnteredFn entered = g_fake.entered;
    const RoverRoomInfoFn room_info = g_fake.room_info;
    const RoverRoomUpdateFn room_update = g_fake.room_update;
    const RoverChatMsgFn chat_msg = g_fake.chat_msg;
    CHECK(
        entered != nullptr && room_info != nullptr && room_update != nullptr && chat_msg != nullptr
    );
    CHECK(
        g_service.switch_room(g_service.user, 1802, 14, "pw", 1801, 13, "Far") ==
        ServiceResult::pending
    );
    CHECK(g_fake.entered == entered && g_fake.room_info == room_info);
    CHECK(g_fake.room_update == room_update && g_fake.chat_msg == chat_msg);
    CHECK(g_service.leave_room(g_service.user, 1802, 14) == ServiceResult::pending);
    CHECK(
        g_service.send_chat(g_service.user, 14, 1802, 9, "<Whispered> hi") == ServiceResult::pending
    );
    CHECK(g_service.page_user(g_service.user, "zed", "hello") == ServiceResult::pending);
    CHECK(
        g_service.create_room(g_service.user, 1802, "Mine", "a room", "sesame") ==
        ServiceResult::pending
    );
    CHECK(
        (g_fake.room_calls == std::vector<std::string>{
                                  "9:1801",
                                  "10:13,1801,(null)",
                                  "11:14,1802,pw,13,1801",
                                  "12:14,1802",
                                  "13:14,1802,9,<Whispered> hi",
                                  "41:zed,hello",
                                  "69:1802,Mine,a room,sesame"
                              })
    );

    g_fake.room_list(0, 1801, 13, "Calm", "quiet", false, 2, 1, "tag", false);
    g_fake.room_list(1, 1801, 12, "Gone", "", true, 0, 2, "", true);
    room_update(0, 1802, 15, "New", "fresh", true, 1, 2, "t2");
    entered(0);
    room_info(0, 9, "Dan", 2, 3, 12, "C");
    room_info(1, 0x80000004, "?_guest", 3, 1, 0, "");
    chat_msg(0, 9, "hi all");
    chat_msg(1, 0, "Server restarting");
    g_fake.left(true);
    uint32_t created_area = 1802;
    uint32_t created_room = 16;
    g_fake.created(&created_area, &created_room, "Mine");
    CHECK(
        (g_reports.room_events == std::vector<std::string>{
                                      "listed 0,1801,13,Calm,quiet,0,2,1,tag,0",
                                      "listed 1,1801,12,Gone,,1,0,2,,1",
                                      "updated 0,1802,15,New,fresh,1,1,2,t2",
                                      "entered 0",
                                      "member 0,9,Dan,2,3,12,C",
                                      "member 1,2147483652,?_guest,3,1,0,",
                                      "chat 0,9,hi all",
                                      "chat 1,0,Server restarting",
                                      "left 1",
                                      "created 1802,16,Mine"
                                  })
    );

    // A request Rover refuses fails; without a table every request fails.
    g_fake.room_ok = false;
    CHECK(g_service.request_room_list(g_service.user, 1801) == ServiceResult::failed);
    CHECK(g_service.join_room(g_service.user, 1801, 13, nullptr, "Calm") == ServiceResult::failed);
    CHECK(
        g_service.switch_room(g_service.user, 1801, 13, nullptr, 1802, 14, "Calm") ==
        ServiceResult::failed
    );
    CHECK(g_service.leave_room(g_service.user, 1801, 13) == ServiceResult::failed);
    CHECK(g_service.send_chat(g_service.user, 13, 1801, 0, "x") == ServiceResult::failed);
    rover_module_unload();
    const std::size_t calls = g_fake.room_calls.size();
    CHECK(g_service.request_room_list(g_service.user, 1801) == ServiceResult::failed);
    CHECK(g_service.page_user(g_service.user, "zed", "hello") == ServiceResult::failed);
    CHECK(g_service.create_room(g_service.user, 1801, "Mine", "", "") == ServiceResult::failed);
    CHECK(g_fake.room_calls.size() == calls);
}

// The battle events as the screens receive them, each with its fields in
// order.
std::vector<std::string> g_battle_events;
std::vector<uint32_t> g_launch_areas;

std::string join_fields(std::initializer_list<std::string> fields) {
    std::string out;
    for (const std::string& field : fields)
        out += (out.empty() ? "" : ",") + field;
    return out;
}

void listen_to_battles() {
    ServiceEvents events{};
    events.battles_done = [](void*, bool failed) {
        g_battle_events.push_back("done " + std::to_string(failed));
    };
    events.battle_info = [](void*,
                            int32_t change,
                            const char* game_id,
                            const char* name,
                            int32_t type,
                            int32_t forming,
                            int32_t host_code) {
        g_battle_events.push_back(
            "info " + join_fields(
                          {std::to_string(change),
                           game_id,
                           name,
                           std::to_string(type),
                           std::to_string(forming),
                           std::to_string(host_code)}
                      )
        );
    };
    events.intel = [](void*, const char* game_id, const char* text) {
        g_battle_events.push_back("intel " + join_fields({text_or_null(game_id), text}));
    };
    events.battle_intel = [](void*, const BattleIntel* i) {
        g_battle_events.push_back(
            "battle_intel " + join_fields(
                                  {i->game_id,
                                   std::to_string(i->user_id),
                                   i->user_name,
                                   std::to_string(i->has_password),
                                   i->mission,
                                   std::to_string(i->skill),
                                   std::to_string(i->players),
                                   std::to_string(i->on_death),
                                   std::to_string(i->failed)}
                              )
        );
    };
    events.battle_report = [](void*, const BattleReport* r) {
        g_battle_events.push_back(
            "report " + join_fields(
                            {r->game_id,
                             std::to_string(r->user_id),
                             std::to_string(r->icon),
                             r->user_name,
                             std::to_string(r->kills),
                             std::to_string(r->losses),
                             std::to_string(r->energy),
                             std::to_string(r->metal),
                             std::to_string(r->score),
                             r->message}
                        )
        );
    };
    events.launch = [](void*, const LaunchReport* l) {
        g_launch_areas.push_back(l != nullptr ? l->area_id : 0);
        g_battle_events.push_back(
            "launch " + join_fields(
                            {l->game_id,
                             l->session,
                             l->address,
                             l->password,
                             std::to_string(l->creator),
                             std::to_string(l->lock_options),
                             l->mission,
                             std::to_string(l->playerlimit),
                             std::to_string(l->deathopt),
                             std::to_string(l->energy),
                             std::to_string(l->metal),
                             std::to_string(l->location),
                             std::to_string(l->los),
                             std::to_string(l->mapping),
                             std::to_string(l->maxunits),
                             std::to_string(l->watching),
                             std::to_string(l->cheats),
                             std::to_string(l->tournament),
                             l->allegiance,
                             std::to_string(l->team),
                             std::to_string(l->result)}
                        )
        );
    };
    events.debriefing = [](void*, uint32_t area_id, uint32_t room_id, bool first) {
        g_battle_events.push_back(
            "debriefing " +
            join_fields({std::to_string(area_id), std::to_string(room_id), std::to_string(first)})
        );
    };
    g_service.set_events(g_service.user, &events);
}

// The next request oa-rover queued for the boneyard, as the wire carries it.
std::string sent() {
    RoverRecord* record = rover_queue_pop(g_rover_state->outbound[0]);
    char text[kRoverRecordTextLimit + 1] = {};
    int32_t length = 0;
    if (record != nullptr)
        rover_record_format(*record, text, sizeof text - 1, &length);
    text[length] = '\0';
    rover_record_destroy(&record);
    return text;
}

// A reply to the client: one Rover entry of the given command whose fields
// the caller sets with the record encoder.
struct Reply {
    RoverRecord* record = nullptr;
    RoverEntry* entry = nullptr;

    explicit Reply(const char* command) {
        entry = rover_add_request(&record, RoverService::Rover, command);
        if (entry != nullptr)
            rover_entry_set_uint(*entry, "area_id", 0x709);
    }

    Reply& text(const char* key, const char* value) {
        rover_entry_set_string(*entry, key, value);
        return *this;
    }

    Reply& number(const char* key, int32_t value) {
        rover_entry_set_int(*entry, key, value);
        return *this;
    }

    Reply& id(const char* key, uint32_t value) {
        rover_entry_set_uint(*entry, key, value);
        return *this;
    }
};

// Formats the reply, reads it back as the link does and pumps it through the
// session.
void deliver(Reply& reply) {
    CHECK(reply.entry != nullptr);
    char text[kRoverRecordTextLimit + 1] = {};
    int32_t length = 0;
    CHECK(
        rover_record_format(*reply.record, text, sizeof text - 1, &length) == RoverRecordStatus::Ok
    );
    rover_record_destroy(&reply.record);
    RoverRecord* pending = nullptr;
    RoverRecord* received = nullptr;
    int32_t consumed = 0;
    CHECK(
        rover_record_receive(&pending, text, length, &consumed, &received) == RoverRecordStatus::Ok
    );
    rover_queue_push(g_rover_state->inbound, received);
    g_service.pump(g_service.user);
}

constexpr const char* kGame = "9bfaa0d0f7b93f12";

// The battle entries call slots 15, 17, 19, 21 and 23 of oa-rover with
// the game's argument order (the Host panel's null description and keyword);
// the replies for Slag (area 0x709), built with the record
// encoder and decoded by oa-rover's handlers, raise the battle events with
// their fields in order.
void test_battle_entries() {
    reset_all();
    const RoverInterface* real = nullptr;
    rover_get_interface(&real);
    g_table.attach = real->attach;
    g_table.detach = real->detach;
    g_table.pump_events = real->pump_events;
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    CHECK(g_rover_state != nullptr);
    if (g_rover_state == nullptr)
        return;
    g_battle_events.clear();
    g_launch_areas.clear();
    listen_to_battles();

    CHECK(g_service.enter_battles(g_service.user, 0x709) == ServiceResult::pending);
    CHECK(sent() == "{[GameManager<U:area_id=709><S:command=enter_battles>]}");
    Reply entered("battles_entered");
    deliver(entered.number("result", 0));
    Reply forming_row("battle_info");
    deliver(forming_row.text("game_id", kGame)
                .text("name", "test")
                .number("type", 4)
                .number("forming", 1));
    Reply removed("battle_info");
    deliver(removed.text("game_id", "a")
                .text("name", "b")
                .number("type", 1)
                .text("host", "Core")
                .text("change", "delete"));
    Reply line("intel_msg");
    deliver(line.text("message", "Game over"));
    Reply about("intel_msg");
    deliver(about.text("game_id", "a").text("message", "a joined"));

    CHECK(g_service.get_battle(g_service.user, 0x709, kGame) == ServiceResult::pending);
    CHECK(
        sent() == "{[GameManager<U:area_id=709><S:command=get_battle>"
                  "<S:game_id=9bfaa0d0f7b93f12>]}"
    );
    Reply intel("battle_intel");
    deliver(intel.text("game_id", kGame)
                .id("user_id", 1)
                .text("username", "kestrel")
                .text("mission", "Acid Pools")
                .number("skill", 0)
                .number("players", 2)
                .number("on_death", 2)
                .number("result", 0));
    Reply score("battle_report");
    deliver(score.text("game_id", kGame).id("user_id", 1).text("username", "kestrel"));
    Reply said("battle_report");
    deliver(said.text("game_id", kGame)
                .id("user_id", 2)
                .text("username", "second")
                .number("icon", 3)
                .number("kills", 9)
                .number("score", 40)
                .text("message", "gg"));

    CHECK(g_service.leave_battles(g_service.user, 0x709) == ServiceResult::pending);
    CHECK(sent() == "{[GameManager<U:area_id=709><S:command=leave_battles>]}");
    Reply left("battles_left");
    deliver(left.number("result", 1));

    CHECK(
        (g_battle_events == std::vector<std::string>{
                                "done 0",
                                "info 0,9bfaa0d0f7b93f12,test,4,1,2",
                                "info 1,a,b,1,0,1",
                                "intel (null),Game over",
                                "intel a,a joined",
                                "battle_intel 9bfaa0d0f7b93f12,1,kestrel,0,Acid Pools,0,0,2,0",
                                "report 9bfaa0d0f7b93f12,1,0,kestrel,0,0,0,0,0,",
                                "report 9bfaa0d0f7b93f12,2,3,second,9,0,0,0,40,gg",
                                "done 1"
                            })
    );

    // Hosting: START's request, then launch_game and both debriefings.
    g_battle_events.clear();
    const HostGameRequest host_request{0x709, 4, "test", nullptr, 0, 0, 2};
    CHECK(g_service.host_game(g_service.user, &host_request) == ServiceResult::pending);
    CHECK(
        sent() == "{[GameManager<U:area_id=709><S:command=host_game><S:name=test><I:on_death=2>"
                  "<I:players=2><I:type=4>]}"
    );
    Reply launch("launch_game");
    deliver(launch.text("game_id", kGame)
                .text("address", "10.66.5.217")
                .text("password", "YFCPDVPKETZYKXA1")
                .text("session", kGame)
                .number("creator", 1)
                .number("lockOptions", 1)
                .number("result", 0)
                .text("mission", "Acid Pools")
                .number("playerlimit", 2)
                .number("deathopt", 2)
                .number("energy", 1000)
                .number("metal", 1000)
                .number("location", 1)
                .number("los", 0)
                .number("mapping", 2)
                .number("maxunits", 250)
                .number("watching", 0)
                .number("cheats", 0)
                .number("tournament", 1)
                .text("allegiance", "A")
                .text("relay", "virginia.relay.coreprime.net%663479")
                .text("overlay_ip", "10.66.5.217")
                .number("key_id", 1));
    Reply debriefing("debriefing");
    deliver(debriefing.text("game_id", kGame).id("room_id", 3));
    Reply again("debriefing");
    deliver(again.text("game_id", kGame).id("room_id", 3));
    CHECK(
        (g_battle_events ==
         std::vector<std::string>{
             "launch 9bfaa0d0f7b93f12,9bfaa0d0f7b93f12,10.66.5.217,YFCPDVPKETZYKXA1,1,1,"
             "Acid Pools,2,2,1000,1000,1,0,2,250,0,0,1,A,0,0",
             "debriefing 1801,3,1",
             "debriefing 1801,3,0"
         })
    );
    // The reply's <U:area_id=709> is hexadecimal: the planet is 1801.
    CHECK(g_launch_areas.size() == 1 && g_launch_areas[0] == 1801);

    // The selectors keep their places; a join names the game and password.
    const HostGameRequest chosen{0x709, 1, "sel", "pw", 2, 3, 3};
    CHECK(g_service.host_game(g_service.user, &chosen) == ServiceResult::pending);
    CHECK(
        sent() == "{[GameManager<U:area_id=709><S:command=host_game><S:name=sel><I:on_death=3>"
                  "<S:password=pw><I:players=1><I:skill=2><I:type=1>]}"
    );
    g_battle_events.clear();
    CHECK(g_service.join_game(g_service.user, 0x709, kGame, "pw") == ServiceResult::pending);
    CHECK(
        sent() == "{[GameManager<U:area_id=709><S:command=join_game>"
                  "<S:game_id=9bfaa0d0f7b93f12><S:password=pw>]}"
    );
    Reply refused("launch_game");
    deliver(refused.text("game_id", kGame)
                .text("address", "1.2.3.4")
                .text("password", "x")
                .number("result", 3));
    CHECK(
        g_battle_events.size() == 1 && g_battle_events[0].rfind("launch ", 0) == 0 &&
        g_battle_events[0].substr(g_battle_events[0].size() - 2) == ",3"
    );
    CHECK(g_launch_areas.size() == 2 && g_launch_areas[1] == 1801);

    real->detach();
    rover_module_unload();
    const std::size_t events = g_battle_events.size();
    CHECK(g_service.enter_battles(g_service.user, 0x709) == ServiceResult::failed);
    CHECK(g_service.get_battle(g_service.user, 0x709, kGame) == ServiceResult::failed);
    CHECK(g_service.join_game(g_service.user, 0x709, kGame, nullptr) == ServiceResult::failed);
    CHECK(g_battle_events.size() == events);
    const BoneyardsService offline = boneyards_offline_service(nullptr);
    CHECK(offline.enter_battles(offline.user, 0x709) == ServiceResult::not_connected);
    CHECK(offline.host_game(offline.user, &host_request) == ServiceResult::not_connected);
}

// The Profile console's bag: its fields go to oa-rover's slot 64 as one
// set_profile record for the user, and profile_updated answers the change
// through the service's callback; a bag without fields sends nothing.
std::vector<bool> g_profile_answers;

void add_profile_field(ProfileEdits* edits, const char* key, const char* value) {
    ProfileEditField& field = edits->fields[edits->count++];
    field.key = key;
    std::snprintf(field.value, sizeof field.value, "%s", value);
}

void test_profile_submission() {
    reset_all();
    const RoverInterface* real = nullptr;
    rover_get_interface(&real);
    g_table.attach = real->attach;
    g_table.detach = real->detach;
    g_table.pump_events = real->pump_events;
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    CHECK(g_rover_state != nullptr);
    if (g_rover_state == nullptr)
        return;
    g_profile_answers.clear();
    const auto answer = [](void* context, bool ok) {
        static_cast<std::vector<bool>*>(context)->push_back(ok);
    };
    ProfileEdits edits{};
    add_profile_field(&edits, "PASSWORD", "secret");
    add_profile_field(&edits, "TAG", " ");
    add_profile_field(&edits, "NUMPLAYERS", "1");
    CHECK(
        g_service.submit_profile(g_service.user, 0x51, &edits, answer, &g_profile_answers) ==
        ServiceResult::pending
    );
    const std::string record = sent();
    CHECK(record.rfind("{[UserManager<", 0) == 0);
    for (const char* field :
         {"<S:command=set_profile>",
          "<U:user_id=51>",
          "<S:PASSWORD=secret>",
          "<S:TAG= >",
          "<S:NUMPLAYERS=1>"})
        CHECK(record.find(field) != std::string::npos);
    CHECK(sent().empty());
    Reply updated("profile_updated");
    deliver(updated.id("user_id", 0x51).number("result", 0));
    CHECK((g_profile_answers == std::vector<bool>{true}));

    // A refused change answers false.
    ProfileEdits one{};
    add_profile_field(&one, "FAVORITEMAP", "Tundra");
    CHECK(
        g_service.submit_profile(g_service.user, 0x51, &one, answer, &g_profile_answers) ==
        ServiceResult::pending
    );
    CHECK(sent().find("<S:FAVORITEMAP=Tundra>") != std::string::npos);
    Reply refused("profile_updated");
    deliver(refused.id("user_id", 0x51).number("result", 2));
    CHECK((g_profile_answers == std::vector<bool>{true, false}));

    const ProfileEdits empty{};
    CHECK(
        g_service.submit_profile(g_service.user, 0x51, &empty, answer, &g_profile_answers) ==
        ServiceResult::failed
    );
    CHECK(sent().empty() && g_profile_answers.size() == 2);
    real->detach();
    rover_module_unload();
    CHECK(
        g_service.submit_profile(g_service.user, 0x51, &one, answer, &g_profile_answers) ==
        ServiceResult::failed
    );
}

// A login driven through the screens: LOGIN on the dialog, the progress
// lines in its status, and once the dossier arrives the planetary state with
// no screen opened.
// Slot 40 and slot 32 as the fake table saw them, and the arrivals the
// session reported.
struct ContentCalls {
    std::vector<int32_t> server_types;
    int32_t type_answer = 0;
    std::string url;
    std::string destination;
    RoverHttpCompleteFn complete = nullptr;
    RoverHttpProgressFn progress = nullptr;
    std::vector<std::string> arrivals;
};

ContentCalls g_content;

void ROVER_CALL fake_get_file_server(int32_t server_type, RoverFileServer* out) {
    g_content.server_types.push_back(server_type);
    out->type_index = g_content.type_answer;
    std::snprintf(out->host, sizeof out->host, "ladders.example");
    out->port = 8080;
}

bool ROVER_CALL fake_fetch_http_file(
    const char* url, const char* dest, RoverHttpCompleteFn complete, RoverHttpProgressFn progress
) {
    g_content.url = url;
    g_content.destination = dest;
    g_content.complete = complete;
    g_content.progress = progress;
    return true;
}

// The contents consoles' requests: slot 40 is asked for the console's kind
// (the record's type preset to it, as the game does) and an invalid
// answer means no server; slot 32 saves into a new empty file named 0BY...
// in the temporary directory, without progress, and its completion reaches
// the file_arrived event. Without a table nothing is asked or made.
void test_content_fetches() {
    reset_all();
    g_content = ContentCalls{};
    char host[0x100] = {};
    uint16_t port = 0;
    char dest[0x104] = "unset";
    CHECK(!g_service.content_server(g_service.user, kContentServerNews, host, sizeof host, &port));
    CHECK(
        g_service.fetch_file(g_service.user, "http://a/b", dest, sizeof dest) ==
        ServiceResult::not_connected
    );
    CHECK(dest[0] == '\0' && g_content.server_types.empty() && g_content.url.empty());

    g_table.get_file_server = fake_get_file_server;
    g_table.fetch_http_file = fake_fetch_http_file;
    ServiceEvents events{};
    events.file_arrived = [](void*, const char* url, const char* path, int32_t status) {
        g_content.arrivals.push_back(std::string(url) + "|" + path + "|" + std::to_string(status));
    };
    g_service.set_events(g_service.user, &events);
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    g_content.type_answer = kContentServerLadders;
    CHECK(
        g_service.content_server(g_service.user, kContentServerLadders, host, sizeof host, &port)
    );
    CHECK(g_content.server_types.size() == 1 && g_content.server_types[0] == kContentServerLadders);
    CHECK(std::strcmp(host, "ladders.example") == 0 && port == 8080);
    g_content.type_answer = OA_ROVER_FILE_SERVER_INVALID;
    CHECK(!g_service.content_server(g_service.user, kContentServerNews, host, sizeof host, &port));
    CHECK(g_content.server_types.size() == 2 && g_content.server_types[1] == kContentServerNews);

    const char* url = "http://ladders.example:8080/Boneyards/TA/Ladders/DUEL1.htm";
    CHECK(g_service.fetch_file(g_service.user, url, dest, sizeof dest) == ServiceResult::pending);
    const std::filesystem::path file(dest);
    CHECK(std::filesystem::equivalent(file.parent_path(), std::filesystem::temp_directory_path()));
    CHECK(file.filename().string().rfind("0BY", 0) == 0 && file.extension() == ".TMP");
    CHECK(std::filesystem::exists(file) && std::filesystem::file_size(file) == 0);
    CHECK(g_content.url == url && g_content.destination == dest);
    CHECK(g_content.complete != nullptr && g_content.progress == nullptr);
    char second[0x104] = {};
    CHECK(
        g_service.fetch_file(g_service.user, url, second, sizeof second) == ServiceResult::pending
    );
    CHECK(std::strcmp(dest, second) != 0);
    if (g_content.complete != nullptr)
        g_content.complete(url, dest, 0);
    CHECK(
        g_content.arrivals.size() == 1 &&
        g_content.arrivals[0] == std::string(url) + "|" + dest + "|0"
    );
    std::filesystem::remove(file);
    std::filesystem::remove(second);
    g_service.set_events(g_service.user, nullptr);
}

void test_login_through_screens() {
    reset_all();
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    auto* nav = new screens::BoneyardsScreens{};
    screens::boneyards_screens_init(nav, &g_service, screens::SettingsStore{});
    ServiceEvents events = screens::boneyards_screens_events(nav);
    nav->service.set_events(nav->service.user, &events);
    CHECK(screens::boneyards_open(nav, screens::ByScreen::login) == screens::ByScreen::login);
    std::snprintf(nav->login.name, sizeof nav->login.name, "kestrel");
    std::snprintf(nav->login.password, sizeof nav->login.password, "secret");
    screens::login_on_control(&nav->login, screens::LoginControl::login_button);
    CHECK(nav->login.state == screens::LoginState::connecting);
    CHECK(g_fake.login_user == "kestrel" && g_fake.login_password == "secret");
    pump([] { g_fake.progress(4, 0, 0, 0, nullptr); });
    CHECK(std::strcmp(nav->login.status, "Contact Established") == 0);
    pump([] {
        g_fake.progress(7, 0, 0, 0, nullptr);
        g_fake.progress(8, 0, 42, 7, nullptr);
    });
    CHECK(nav->current == screens::ByScreen::login && nav->session.self_user_id == 42);
    pump(deliver_dossier);
    CHECK(nav->current == screens::ByScreen::none);
    CHECK(nav->login.state == screens::LoginState::planetary && nav->session.area_id == 7);

    // A refused login leaves the dialog idle with LOGIN enabled.
    reset_all();
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    screens::boneyards_screens_init(nav, &g_service, screens::SettingsStore{});
    events = screens::boneyards_screens_events(nav);
    nav->service.set_events(nav->service.user, &events);
    screens::boneyards_open(nav, screens::ByScreen::login);
    std::snprintf(nav->login.name, sizeof nav->login.name, "kestrel");
    std::snprintf(nav->login.password, sizeof nav->login.password, "wrong");
    std::snprintf(nav->login.server, sizeof nav->login.server, "Local Yard");
    screens::login_on_control(&nav->login, screens::LoginControl::login_button);
    CHECK(g_fake.login_server == "Local Yard"); // the dialog's list selection
    pump(fail_password);
    CHECK(nav->login.state == screens::LoginState::idle && nav->login.login_value == 2);
    CHECK(std::strstr(nav->login.status, "coreprime.net/forgot") != nullptr);
    pump([] { g_fake.alert(1, "Maintenance at noon", nullptr); });
    CHECK(std::strcmp(nav->last_message, "Maintenance at noon") == 0);
    screens::boneyards_screens_release(nav);
    delete nav;
}

// U+6771 and U+6771 U+4EAC.
constexpr const char* kEast = "\xE6\x9D\xB1";
constexpr const char* kTokyo = "\xE6\x9D\xB1\xE4\xBA\xAC";

std::string repeat_unit(const char* unit, int32_t count) {
    std::string out;
    for (int32_t i = 0; i < count; ++i)
        out += unit;
    return out;
}

struct Utf8Bag {
    char clan[5];
    char login[96];
    bool found;
};

void take_utf8_bag(void* context, bool, const screens::ProfileFields* fields) {
    auto* sample = static_cast<Utf8Bag*>(context);
    sample->found = fields->get_string(fields->user, "CLAN", sample->clan, sizeof sample->clan);
    fields->get_string(fields->user, "LOGINNAME", sample->login, sizeof sample->login);
}

// A session name and password keep 15 characters, a guest keeps "?_" plus
// the name inside that same limit, and a profile bag copies on a character
// boundary. The dossier is not delivered here: it would replace the name.
void test_utf8_session_text() {
    reset_all();
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    const std::string fifteen = repeat_unit(kEast, 15);
    const std::string sixteen = repeat_unit(kEast, 16);
    login(fifteen.c_str(), "secret");
    CHECK(g_fake.login_user == fifteen);
    CHECK(std::strcmp(rover_session()->record.user_name, fifteen.c_str()) == 0);
    CHECK(utf8::characters(rover_session()->record.user_name) == 15);

    const std::string typed(20, 'p');
    login(sixteen.c_str(), typed.c_str());
    CHECK(g_fake.login_user == fifteen && g_fake.login_password == typed.substr(0, 15));
    CHECK(std::strcmp(rover_session()->record.user_name, fifteen.c_str()) == 0);
    CHECK(std::strcmp(rover_session()->password, typed.substr(0, 15).c_str()) == 0);

    const std::string guest_name(20, 'a');
    const LoginRequest guest{guest_name.c_str(), "", false, nullptr};
    CHECK(g_service.login(g_service.user, &guest) == ServiceResult::pending);
    CHECK(g_fake.login_user == guest_name.substr(0, 15));
    CHECK(std::strcmp(rover_session()->record.user_name, "?_aaaaaaaaaaaaa") == 0);
    CHECK(utf8::characters(rover_session()->record.user_name) == 15);

    const LoginRequest guest_east{sixteen.c_str(), "", false, nullptr};
    CHECK(g_service.login(g_service.user, &guest_east) == ServiceResult::pending);
    CHECK(g_fake.login_user == fifteen);
    CHECK(
        std::strcmp(rover_session()->record.user_name, ("?_" + repeat_unit(kEast, 13)).c_str()) == 0
    );
    CHECK(utf8::characters(rover_session()->record.user_name) == 15);

    reset_all();
    const RoverInterface* real = nullptr;
    rover_get_interface(&real);
    g_table.get_field_int = real->get_field_int;
    g_table.get_field_string = real->get_field_string;
    CHECK(g_service.activate(g_service.user) == ActivationResult::ok);
    Utf8Bag sample{};
    CHECK(
        g_service.request_profile(g_service.user, 42, take_utf8_bag, nullptr, &sample) ==
        ServiceResult::pending
    );
    CHECK(g_fake.profile != nullptr);
    rover_session()->record.user_id = 42;
    RoverPropertyBag* self = profile_bag(42, sixteen.c_str(), 1);
    put_text(self, "CLAN", kTokyo);
    g_fake.profile(true, self);
    CHECK(sample.found);
    CHECK(std::strcmp(sample.clan, kEast) == 0);
    CHECK(std::strcmp(sample.login, sixteen.c_str()) == 0);
    CHECK(std::strcmp(rover_session()->record.user_name, fifteen.c_str()) == 0);
    CHECK(utf8::characters(rover_session()->record.user_name) == 15);
    rover_call_destroy_property_bag(self);
    rover_module_unload();
}

} // namespace

int main() {
    test_activation();
    test_member_login();
    test_guest_and_explicit_server();
    test_login_failures();
    test_dossier_refused();
    test_cancel();
    test_live_holds_until_logout_or_disconnect();
    test_yard_master_unreachable();
    test_disconnects_and_alerts();
    test_file_placement();
    test_host_server_list();
    test_file_creation();
    test_expansion_server_list_uses_baked_alpha();
    test_selected_server_from_rover();
    test_read_only_game_folder();
    test_auto_update_stand_in();
    test_war_map_entries();
    test_offline_war_map();
    test_locate_user();
    test_profile_requests();
    test_chat_room_entries();
    test_content_fetches();
    test_battle_entries();
    test_profile_submission();
    test_login_through_screens();
    test_utf8_session_text();
    if (g_failures != 0) {
        std::fprintf(stderr, "rover session: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("rover session: ok");
    return EXIT_SUCCESS;
}
