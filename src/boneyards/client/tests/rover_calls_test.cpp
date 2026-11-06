// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Rover table binding, its lock and call counting, the wrappers'
// forwarding, and the game's string object, against a fake table.
#include "oa/boneyards/rover_calls.hpp"
#include "oa/boneyards/rover_string.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

struct FakeRover {
    int32_t version_result = 1;
    int32_t depth_seen = -1;
    int32_t nested_depth_seen = -1;
    bool close_in_pump = false;
    bool nested_call_in_pump = false;
    bool bound_during_pump = false;
    int32_t pumps = 0;
    int32_t destroyed_bags = 0;
    const char* login_server = nullptr;
    const char* login_user = nullptr;
    const char* login_password = nullptr;
    RoverLoginProgressFn login_progress = nullptr;
    RoverSystemAlertFn login_alert = nullptr;
    RoverDisconnectFn login_disconnect = nullptr;
    int32_t host_skill = 0;
    RoverDebriefingFn host_debriefing = nullptr;
    int32_t score = 0;
    bool score_last = false;
    int32_t settings_tournament = 0;
    char key[64] = {};
    uint32_t key_length = 0;
    uint32_t key_capacity = 0;
    uint8_t key_share_count = 0xAA;
    int32_t chat_target = 0;
    int32_t create_depth = -1;
    uint32_t create_area = 0;
    const char* create_name = nullptr;
    const char* create_description = nullptr;
    const char* create_password = nullptr;
    RoverRoomCreatedFn create_callback = nullptr;
};

FakeRover g_fake;
RoverInterface g_table;

int32_t ROVER_CALL fake_check_version(int32_t, int32_t) {
    return g_fake.version_result;
}

void ROVER_CALL fake_pump_events() {
    ++g_fake.pumps;
    g_fake.depth_seen = rover_module_state()->call_depth;
    if (g_fake.nested_call_in_pump)
        rover_call_send_chat(1, 2, 3, "nested");
    if (g_fake.close_in_pump) {
        rover_module_close();
        g_fake.bound_during_pump = rover_module_state()->bound;
    }
}

bool ROVER_CALL fake_send_chat(int32_t, int32_t, int32_t target_user_id, const char*) {
    g_fake.chat_target = target_user_id;
    g_fake.nested_depth_seen = rover_module_state()->call_depth;
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
    g_fake.depth_seen = rover_module_state()->call_depth;
    g_fake.login_server = server_name;
    g_fake.login_user = username;
    g_fake.login_password = password;
    g_fake.login_progress = progress;
    g_fake.login_alert = system_alert;
    g_fake.login_disconnect = disconnect;
    return true;
}

bool ROVER_CALL fake_host_game(
    uint32_t,
    int32_t,
    const char*,
    const char*,
    const char*,
    const char*,
    int32_t skill_sel,
    int32_t,
    int32_t,
    RoverLaunchGameFn,
    RoverDebriefingFn debriefing
) {
    g_fake.host_skill = skill_sel;
    g_fake.host_debriefing = debriefing;
    return true;
}

bool ROVER_CALL fake_send_score_report(
    uint32_t,
    const char*,
    uint32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t score,
    bool,
    bool last
) {
    g_fake.score = score;
    g_fake.score_last = last;
    return true;
}

bool ROVER_CALL fake_send_game_settings(
    uint32_t,
    const char*,
    int32_t,
    const char*,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t tournament
) {
    g_fake.settings_tournament = tournament;
    return true;
}

void ROVER_CALL fake_create_room(
    uint32_t area_id,
    const char* room_name,
    const char* description,
    const char* password,
    RoverRoomCreatedFn created
) {
    g_fake.create_depth = rover_module_state()->call_depth;
    g_fake.create_area = area_id;
    g_fake.create_name = room_name;
    g_fake.create_description = description;
    g_fake.create_password = password;
    g_fake.create_callback = created;
}

void ROVER_CALL fake_destroy_property_bag(RoverPropertyBag*) {
    ++g_fake.destroyed_bags;
}

void remember_key(const RoverString* key) {
    std::snprintf(g_fake.key, sizeof g_fake.key, "%s", key->data);
    g_fake.key_length = key->length;
    g_fake.key_capacity = key->capacity;
    g_fake.key_share_count = reinterpret_cast<const uint8_t*>(key->data)[-1];
}

bool ROVER_CALL fake_get_field_int(RoverPropertyBag*, const RoverString* key, int32_t* out) {
    remember_key(key);
    *out = 5;
    return true;
}

bool ROVER_CALL fake_get_field_string(RoverPropertyBag*, const RoverString* key, RoverString* out) {
    remember_key(key);
    rover_string_assign(out, "Arm", 3);
    return true;
}

void ROVER_CALL fake_find_current_area(const char** out) {
    *out = "CorePrime Alpha Test";
}

void ROVER_CALL fake_get_interface(const RoverInterface** out) {
    *out = &g_table;
}

void reset_fake() {
    rover_module_unload();
    g_fake = FakeRover{};
    const RoverInterface* real = nullptr;
    rover_get_interface(&real);
    g_table = *real;
    g_table.check_version = fake_check_version;
    g_table.pump_events = fake_pump_events;
    g_table.send_chat = fake_send_chat;
    g_table.start_login = fake_start_login;
    g_table.host_game = fake_host_game;
    g_table.send_score_report = fake_send_score_report;
    g_table.send_game_settings = fake_send_game_settings;
    g_table.destroy_property_bag = fake_destroy_property_bag;
    g_table.create_room = fake_create_room;
    g_table.get_field_int = fake_get_field_int;
    g_table.get_field_string = fake_get_field_string;
    g_table.find_current_area = fake_find_current_area;
    rover_module_set_source(fake_get_interface);
}

void ROVER_CALL room_created(uint32_t*, uint32_t*, const char*) {
}

void test_unbound_answers() {
    reset_fake();
    rover_module_set_source(nullptr);
    CHECK(!rover_module_load());
    CHECK(!rover_module_state()->bound && rover_module_state()->table == nullptr);
    CHECK(!rover_call_attach());
    CHECK(rover_call_create_property_bag() == nullptr);
    CHECK(rover_call_match_area_name() == nullptr);
    const char* location = "untouched";
    rover_call_find_current_area(&location);
    CHECK(std::strcmp(location, "untouched") == 0);
    int32_t value = 0;
    CHECK(!rover_call_get_field_int(nullptr, "USER_STATUS", &value));
    rover_call_create_room(0x709, "Ops", nullptr, "", room_created);
    CHECK(g_fake.create_depth == -1); // unbound: the slot is not called
    CHECK(rover_module_state()->call_depth == 0);
}

void test_handshake() {
    reset_fake();
    g_fake.version_result = 0x100; // only the low byte is read
    CHECK(!rover_module_load());
    CHECK(!rover_module_state()->bound && rover_module_state()->table == nullptr);

    reset_fake();
    rover_module_set_source(rover_get_interface);
    CHECK(rover_module_load()); // oa-rover's own table passes its handshake
    rover_module_unload();

    reset_fake();
    CHECK(rover_module_load());
    CHECK(rover_module_state()->bound && rover_module_state()->table == &g_table);
    CHECK(rover_call_check_version(kRoverInterfaceVersion, kRoverBuildStamp));
    rover_module_unload();
    CHECK(!rover_module_state()->bound && rover_module_state()->table == nullptr);
}

void ROVER_CALL progress(int32_t, int32_t, int32_t, int32_t, const char*) {
}

void ROVER_CALL alert(int32_t, const char*, void*) {
}

void ROVER_CALL dropped(int32_t, char, int32_t) {
}

void ROVER_CALL debriefing(uint32_t, uint32_t, bool) {
}

void test_forwarding() {
    reset_fake();
    CHECK(rover_module_load());
    CHECK(rover_call_start_login("alpha", "kestrel", "pw", progress, alert, dropped));
    CHECK(g_fake.depth_seen == 1);
    CHECK(rover_module_state()->call_depth == 0);
    CHECK(std::strcmp(g_fake.login_server, "alpha") == 0);
    CHECK(std::strcmp(g_fake.login_user, "kestrel") == 0);
    CHECK(std::strcmp(g_fake.login_password, "pw") == 0);
    CHECK(
        g_fake.login_progress == progress && g_fake.login_alert == alert &&
        g_fake.login_disconnect == dropped
    );
    CHECK(rover_call_host_game(7, 1, "game", nullptr, "", "", 2, 3, 1, nullptr, debriefing));
    CHECK(g_fake.host_skill == 2 && g_fake.host_debriefing == debriefing);
    CHECK(rover_call_send_score_report(7, "g", 9, 1, 2, 3, 4, 5, 6, 7, 8, 1234, true, true));
    CHECK(g_fake.score == 1234 && g_fake.score_last);
    CHECK(rover_call_send_game_settings(7, "g", 1, "map", 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12));
    CHECK(g_fake.settings_tournament == 12);
    const char* location = nullptr;
    rover_call_find_current_area(&location);
    CHECK(location != nullptr && std::strcmp(location, "CorePrime Alpha Test") == 0);
    const char* name = "Proving Grounds";
    const char* description = "Duel practice";
    const char* password = "pw5";
    rover_call_create_room(0x709, name, description, password, room_created);
    CHECK(g_fake.create_depth == 1 && rover_module_state()->call_depth == 0);
    CHECK(
        g_fake.create_area == 0x709 && g_fake.create_name == name &&
        g_fake.create_description == description && g_fake.create_password == password &&
        g_fake.create_callback == room_created
    );

    rover_call_destroy_property_bag(nullptr);
    CHECK(g_fake.destroyed_bags == 0);
    rover_call_destroy_property_bag(reinterpret_cast<RoverPropertyBag*>(&g_fake));
    CHECK(g_fake.destroyed_bags == 1);
    rover_module_unload();
}

void test_keyed_bag_calls() {
    reset_fake();
    CHECK(rover_module_load());
    auto* bag = reinterpret_cast<RoverPropertyBag*>(&g_fake);
    int32_t status = 0;
    CHECK(rover_call_get_field_int(bag, "USER_STATUS", &status) && status == 5);
    CHECK(std::strcmp(g_fake.key, "USER_STATUS") == 0);
    CHECK(g_fake.key_length == 11 && g_fake.key_capacity == kRoverStringGranule);
    CHECK(g_fake.key_share_count == 0);
    RoverString value{};
    CHECK(rover_call_get_field_string(bag, "ALLEGIANCE", &value));
    CHECK(std::strcmp(g_fake.key, "ALLEGIANCE") == 0);
    CHECK(std::strcmp(rover_string_text(&value), "Arm") == 0 && value.length == 3);
    rover_string_release(&value, true);
    CHECK(value.data == nullptr && value.length == 0 && value.capacity == 0);
    rover_module_unload();
}

void test_nested_and_deferred_close() {
    reset_fake();
    CHECK(rover_module_load());
    g_fake.nested_call_in_pump = true;
    rover_call_pump_events();
    CHECK(g_fake.depth_seen == 1 && g_fake.nested_depth_seen == 2 && g_fake.chat_target == 3);
    CHECK(rover_module_state()->call_depth == 0 && rover_module_state()->bound);

    g_fake.nested_call_in_pump = false;
    g_fake.close_in_pump = true;
    rover_call_pump_events();
    CHECK(g_fake.bound_during_pump); // the close waits for the pump
    CHECK(!rover_module_state()->bound && rover_module_state()->table == nullptr);
    CHECK(rover_module_state()->unload_deferred == 0);
    const int32_t pumps = g_fake.pumps;
    rover_call_pump_events();
    CHECK(g_fake.pumps == pumps); // unbound: nothing is called

    reset_fake();
    CHECK(rover_module_load());
    rover_module_close();
    CHECK(!rover_module_state()->bound && rover_module_state()->unload_deferred == 0);
}

void test_string_object() {
    RoverString s{};
    rover_string_assign(&s, "abc", 3);
    CHECK(s.length == 3 && s.capacity == 0x1f && std::strcmp(s.data, "abc") == 0);
    CHECK(reinterpret_cast<uint8_t*>(s.data)[-1] == 0);
    char* small = s.data;
    rover_string_assign(&s, "0123456789", 10);
    CHECK(s.data == small && s.length == 10 && std::strcmp(s.data, "0123456789") == 0);
    const char* longer = "0123456789012345678901234567890123456789";
    rover_string_assign(&s, longer, 40);
    CHECK(s.capacity == 0x3f && s.length == 40 && std::strcmp(s.data, longer) == 0);
    rover_string_assign(&s, "xy", 2); // a big buffer is not kept by an assignment
    CHECK(s.capacity == 0x1f && s.length == 2 && std::strcmp(s.data, "xy") == 0);

    // A shared buffer: a release drops one share and keeps the bytes.
    reinterpret_cast<uint8_t*>(s.data)[-1] = 1;
    RoverString other = s;
    rover_string_release(&s, true);
    CHECK(s.data == nullptr && reinterpret_cast<uint8_t*>(other.data)[-1] == 0);
    CHECK(std::strcmp(other.data, "xy") == 0);

    // Keeping the contents while growing copies them into the new buffer.
    reinterpret_cast<uint8_t*>(other.data)[-1] = 1;
    RoverString copy = other;
    CHECK(rover_string_prepare(&copy, 0x40, false));
    CHECK(copy.data != other.data && copy.capacity == 0x5f && std::strcmp(copy.data, "xy") == 0);
    CHECK(reinterpret_cast<uint8_t*>(other.data)[-1] == 0);
    rover_string_release(&copy, true);

    // An empty assignment of a sole-owned string frees it.
    rover_string_assign(&other, "", 0);
    CHECK(other.data == nullptr && other.length == 0);
    CHECK(std::strcmp(rover_string_text(&other), "") == 0);
}

} // namespace

int main() {
    test_unbound_answers();
    test_handshake();
    test_forwarding();
    test_keyed_bag_calls();
    test_nested_and_deferred_close();
    test_string_object();
    if (g_failures != 0) {
        std::fprintf(stderr, "rover calls: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("rover calls: ok");
    return EXIT_SUCCESS;
}
