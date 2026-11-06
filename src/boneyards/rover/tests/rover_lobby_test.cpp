// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The lobby slots (9-32, 38, 39, 41, 42, 63-66 and 69) without a network: each
// request slot queues the record 3.1c sends, and each
// reply reaches the game's callback through the pump. HTTP fetches run on
// the fetch thread against a platform table that answers from memory.

#include "../src/requests.h"
#include "../src/state.h"
#include "../src/wire_text.h"
#include "oa/rover/interface.h"
#include "oa/rover/platform.h"
#include "oa/rover/strings.h"
#include "oa/rover/wire_text.h"
#include "oa/platform/system.hpp"
#include "oa/platform/files.hpp"

#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#include <windows.h>
#define make_directory(path) _mkdir(path)
#define change_directory(path) _chdir(path)
#define remove_directory(path) _rmdir(path)
#define process_id() _getpid()
#define sleep_milliseconds(ms) Sleep(ms)
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_directory(path) mkdir(path, 0755)
#define change_directory(path) chdir(path)
#define remove_directory(path) rmdir(path)
#define process_id() getpid()
#define sleep_milliseconds(ms) usleep((ms) * 1000)
#endif

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

const RoverInterface* g_rover = nullptr;

// Entries of a bag handed to a callback, which releases it on return.
int32_t bag_entry_count(RoverPropertyBag* bag) {
    int32_t count = 0;
    for (RoverBagEntry* entry = g_rover->get_first_bag_entry(bag); entry;
         entry = g_rover->get_next_bag_entry(bag))
        ++count;
    return count;
}

// Copies the text of a bag's string entry, or "" when there is none.
void read_bag_text(RoverPropertyBag* bag, const char* key, char* out, size_t capacity) {
    RoverString name{};
    RoverString value{};
    rover_string_assign_text(&name, key, static_cast<uint32_t>(strlen(key)));
    RoverBagEntry* entry = g_rover->find_bag_entry(bag, &name);
    bool found = entry && g_rover->get_entry_string(entry, &value);
    snprintf(out, capacity, "%s", found ? rover_string_text(&value) : "");
    rover_string_release(&value);
    rover_string_release(&name);
}

// A bag of the given entries, each typed by its letter: I signed, U
// unsigned (hexadecimal text) or S text; the caller destroys it.
RoverPropertyBag* make_bag(const char* const (*entries)[3], int32_t count) {
    RoverPropertyBag* bag = g_rover->create_property_bag();
    for (int32_t i = 0; i < count; ++i) {
        RoverString key{};
        RoverString value{};
        const char* text = entries[i][2];
        rover_string_assign_text(&key, entries[i][1], static_cast<uint32_t>(strlen(entries[i][1])));
        rover_string_assign_text(&value, text, static_cast<uint32_t>(strlen(text)));
        if (entries[i][0][0] == 'I')
            g_rover->set_field_int(bag, &key, static_cast<int32_t>(strtol(text, nullptr, 10)));
        else if (entries[i][0][0] == 'U')
            g_rover->set_field_uint(bag, &key, static_cast<uint32_t>(strtoul(text, nullptr, 16)));
        else
            g_rover->set_bag_string(bag, &key, &value);
        rover_string_release(&value);
        rover_string_release(&key);
    }
    return bag;
}

// Queues one received record and pumps it.
void deliver(const char* text) {
    RoverRecord* pending = nullptr;
    RoverRecord* record = nullptr;
    int32_t consumed = 0;
    RoverRecordStatus status = rover_record_receive(
        &pending, text, static_cast<int32_t>(strlen(text)), &consumed, &record
    );
    check(status == RoverRecordStatus::Ok, text);
    if (status != RoverRecordStatus::Ok) {
        rover_record_destroy(&pending);
        return;
    }
    rover_queue_push(g_rover_state->inbound, record);
    g_rover->pump_events();
}

// Checks the next record queued for boneyard link 0 or 1.
void check_sent_on(int32_t link, const char* expected, const char* what) {
    RoverRecord* record = rover_queue_pop(g_rover_state->outbound[link]);
    char text[kRoverRecordTextLimit + 1] = {};
    if (record) {
        int32_t length = 0;
        rover_record_format(*record, text, sizeof text - 1, &length);
        text[length] = '\0';
        rover_record_destroy(&record);
    }
    check(strcmp(text, expected) == 0, what);
    if (strcmp(text, expected) != 0)
        fprintf(stderr, "  sent:     %s\n  expected: %s\n", text, expected);
}

void check_sent(const char* expected, const char* what) {
    check_sent_on(0, expected, what);
}

void check_nothing_sent(const char* what) {
    RoverRecord* record = rover_queue_pop(g_rover_state->outbound[0]);
    check(record == nullptr, what);
    rover_record_destroy(&record);
}

// ---- the galactic war feeds and pages ---------------------------------------

struct Census {
    int calls;
    uint32_t area;
    int32_t values[6];
};

Census g_census{};
char g_allegiances[64];
int g_allegiance_calls = 0;
char g_activity[64];

void ROVER_CALL on_census(
    uint32_t area,
    int32_t pop,
    int32_t activity,
    int32_t progress,
    int32_t forming,
    int32_t core_wins,
    int32_t arm_wins
) {
    g_census =
        Census{g_census.calls + 1, area, {pop, activity, progress, forming, core_wins, arm_wins}};
}

void ROVER_CALL on_census_other(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {
}

void ROVER_CALL on_allegiances(const char* array) {
    snprintf(g_allegiances, sizeof g_allegiances, "%s", array);
    ++g_allegiance_calls;
}

void ROVER_CALL on_activity(const char* array) {
    snprintf(g_activity, sizeof g_activity, "%s", array);
}

void check_war_feeds() {
    RoverHandlerSet& handlers = g_rover_state->handlers;
    g_rover->request_allegiance(on_allegiances);
    check_sent("{[WarManager<S:command=get_allegiance>]}", "get_allegiance, as 3.1c sends it");
    g_rover->request_census(0, on_census, on_activity);
    check_sent("{[WarManager<S:command=get_census>]}", "get_census for every area sends no area");
    g_rover->request_census(0x709, on_census, on_activity);
    check_sent(
        "{[WarManager<I:area_id=1801><S:command=get_census>]}",
        "get_census for one area, as 3.1c sends it"
    );

    deliver("{[Rover<S:command=allegiances><S:array=CCCCIIIAAACCCCCCAAAAAAAAC>]}");
    check(strcmp(g_allegiances, "CCCCIIIAAACCCCCCAAAAAAAAC") == 0, "allegiances pass the array");
    deliver("{[Rover<S:command=allegiances>]}");
    check(g_allegiance_calls == 1, "allegiances without an array are dropped");
    deliver(
        "{[Rover<S:command=census><U:area_id=709><I:pop=3><I:activity=2><I:in_progress=1>"
        "<I:forming=1><I:arm_wins=5><I:core_wins=6>]}"
    );
    check(
        g_census.calls == 1 && g_census.area == 0x709 && g_census.values[0] == 3 &&
            g_census.values[1] == 2 && g_census.values[2] == 1 && g_census.values[3] == 1 &&
            g_census.values[4] == 6 && g_census.values[5] == 5,
        "census passes area, population, activity, games, forming, core and arm wins"
    );
    deliver("{[Rover<S:command=census><U:area_id=709><I:pop=3>]}");
    check(g_census.calls == 1, "a census missing fields is dropped");
    deliver("{[Rover<S:command=activity><S:array=0123>]}");
    check(strcmp(g_activity, "0123") == 0, "activity passes the array");
    check(
        rover_handler_is_armed(handlers, RoverHandlerId::Census) &&
            rover_handler_is_armed(handlers, RoverHandlerId::Allegiances),
        "the war feed handlers stay armed"
    );
    g_rover->request_census(0, on_census_other, on_activity);
    check_sent("{[WarManager<S:command=get_census>]}", "get_census again");
    check(handlers.lobby.census == on_census_other, "a new census callback replaces the old");

    g_rover->send_page_by_name("kestrel", "hi there");
    check_sent(
        "{[UserManager<S:command=page><S:message=hi there><S:name=kestrel>]}", "page by name"
    );
    g_rover->send_page_by_name("", "");
    check_sent("{[UserManager<S:command=page>]}", "page by name leaves out empty text");
    g_rover->send_page_by_id(0x2a, "hi");
    check_sent("{[UserManager<S:command=page><S:message=hi><U:user_id=2a>]}", "page by id");
}

// ---- the user locator ----------------------------------------------------------

struct Located {
    int calls;
    int32_t loc_code;
    uint32_t user_id;
    char name[32];
    int32_t user_icon;
    int32_t fields;
    char command[32];
    char area_name[32];
};

Located g_located{};

void ROVER_CALL on_located(
    int32_t loc_code,
    uint32_t user_id,
    const char* name,
    int32_t user_icon,
    RoverPropertyBag* fields
) {
    ++g_located.calls;
    g_located.loc_code = loc_code;
    g_located.user_id = user_id;
    snprintf(g_located.name, sizeof g_located.name, "%s", name);
    g_located.user_icon = user_icon;
    g_located.fields = bag_entry_count(fields);
    read_bag_text(fields, "command", g_located.command, sizeof g_located.command);
    read_bag_text(fields, "area_name", g_located.area_name, sizeof g_located.area_name);
}

void check_locator() {
    RoverHandlerSet& handlers = g_rover_state->handlers;
    g_rover->locate_player("Steve_Of_The_Core", on_located);
    check_sent(
        "{[UserManager<S:command=locate><S:name=Steve_Of_The_Core>]}",
        "locate asks the user manager about the whole name"
    );
    check(
        rover_handler_is_armed(handlers, RoverHandlerId::UserLocated) &&
            handlers.lobby.user_located.callback == on_located &&
            strcmp(handlers.lobby.user_located.name, "Steve_Of_The_Co") == 0,
        "the locator keeps the callback and the name's first 15 characters"
    );
    deliver(
        "{[Rover<S:command=user_located><I:loc_code=1><U:user_id=2a><S:name=other>"
        "<I:user_icon=3>]}"
    );
    check(
        g_located.calls == 0 && rover_handler_is_armed(handlers, RoverHandlerId::UserLocated),
        "an answer about another name is left alone"
    );
    deliver(
        "{[Rover<S:command=user_located><I:loc_code=1><U:user_id=2a><S:name=Steve_Of_The_Core>]}"
    );
    check(
        g_located.calls == 0 && rover_handler_is_armed(handlers, RoverHandlerId::UserLocated),
        "an answer without user_icon is taken and ignored"
    );
    deliver(
        "{[Rover<S:command=user_located><I:loc_code=1><U:user_id=2a><S:name=STEVE_OF_THE_COWS>"
        "<I:user_icon=3><S:area_name=Hub><U:area_id=709><S:room_name=Frequency 5>]}"
    );
    check(
        g_located.calls == 1 && g_located.loc_code == 1 && g_located.user_id == 0x2a &&
            strcmp(g_located.name, "STEVE_OF_THE_COWS") == 0 && g_located.user_icon == 3,
        "an answer matching the first 15 characters in any case reaches the game"
    );
    check(
        g_located.fields == 4 && strcmp(g_located.command, "user_located") == 0 &&
            strcmp(g_located.area_name, "Hub") == 0,
        "the answer's other fields, command included, come in a bag"
    );
    check(
        !rover_handler_is_armed(handlers, RoverHandlerId::UserLocated) &&
            handlers.lobby.user_located.callback == nullptr &&
            handlers.lobby.user_located.name[0] == '\0',
        "the locator answers once"
    );

    g_rover->locate_player("", on_located);
    check_sent("{[UserManager<S:command=locate>]}", "locate leaves out an empty name");
    g_rover->locate_player("ghost", on_located);
    check_sent("{[UserManager<S:command=locate><S:name=ghost>]}", "locate again");
    check(
        strcmp(handlers.lobby.user_located.name, "ghost") == 0,
        "a second request replaces the name awaited"
    );
    deliver(
        "{[Rover<S:command=user_located><I:user_icon=-2><S:loc_code=5><I:user_id=7>"
        "<S:name=Ghost>]}"
    );
    check(
        g_located.calls == 2 && g_located.loc_code == -1 && g_located.user_id == 0 &&
            strcmp(g_located.name, "Ghost") == 0 && g_located.user_icon == -2 &&
            g_located.fields == 1,
        "a code that is not an I field reads -1 and a user id that is not a U field 0"
    );
}

// ---- the game locator ----------------------------------------------------------

struct GameLocated {
    int calls;
    uint32_t area_id;
    const char* game_id_null;
    char game_id[40];
    char name[64];
    int32_t type;
    int32_t forming;
    int32_t pass_required;
    int32_t last;
};

GameLocated g_game{};

void ROVER_CALL on_game_located(
    uint32_t area_id,
    const char* game_id,
    const char* name,
    int32_t type,
    int32_t forming,
    int32_t pass_required,
    int32_t last
) {
    ++g_game.calls;
    g_game.area_id = area_id;
    g_game.game_id_null = game_id;
    snprintf(g_game.game_id, sizeof g_game.game_id, "%s", game_id ? game_id : "");
    snprintf(g_game.name, sizeof g_game.name, "%s", name ? name : "");
    g_game.type = type;
    g_game.forming = forming;
    g_game.pass_required = pass_required;
    g_game.last = last;
}

void ROVER_CALL
on_game_located_other(uint32_t, const char*, const char*, int32_t, int32_t, int32_t, int32_t) {
    g_game.calls += 100;
}

void check_game_locate() {
    RoverHandlerSet& handlers = g_rover_state->handlers;
    g_game = GameLocated{};
    g_rover->locate_target("Duel at dawn", on_game_located);
    check_sent(
        "{[GameManager<S:command=locate><S:name=Duel at dawn>]}",
        "locate asks the game manager about the whole name"
    );
    check(
        rover_handler_is_armed(handlers, RoverHandlerId::GameLocated) &&
            handlers.lobby.game_located.callback == on_game_located,
        "game locate arms game_located with the callback"
    );
    deliver("{[Rover<S:command=user_located><S:name=Duel at dawn>]}");
    check(g_game.calls == 0, "a user_located answer is not a game");
    deliver(
        "{[Rover<S:command=game_located><U:area_id=709><S:game_id=4f1c2d3e>"
        "<S:name=Duel at dawn><I:type=2><I:forming=2><I:pass_req=1><I:last=1>]}"
    );
    check(
        g_game.calls == 0 && rover_handler_is_armed(handlers, RoverHandlerId::GameLocated) &&
            handlers.lobby.game_located.callback == on_game_located,
        "a forming value other than 0 or 1 is taken and ignored, last included"
    );
    deliver(
        "{[Rover<S:command=game_located><U:area_id=709><S:game_id=4f1c2d3e>"
        "<S:name=Duel at dawn><I:type=2><I:forming=1><I:pass_req=3><I:last=0>]}"
    );
    check(
        g_game.calls == 1 && g_game.area_id == 0x709 && strcmp(g_game.game_id, "4f1c2d3e") == 0 &&
            strcmp(g_game.name, "Duel at dawn") == 0 && g_game.type == 2 && g_game.forming == 1 &&
            g_game.pass_required == 1 && g_game.last == 0,
        "a forming game is reported, and a non-zero pass_req is 1"
    );
    check(
        rover_handler_is_armed(handlers, RoverHandlerId::GameLocated),
        "a reply whose last is zero leaves the handler armed"
    );
    deliver("{[Rover<S:command=game_located><I:forming=0>]}");
    check(
        g_game.calls == 2 && g_game.area_id == 0 && g_game.game_id_null == nullptr &&
            strcmp(g_game.name, "") == 0 && g_game.type == 0 && g_game.forming == 0 &&
            g_game.pass_required == 0 && g_game.last == 0,
        "an answer without the fields reads 0, a null game id and an empty name"
    );
    deliver("{[Rover<S:command=game_located><S:forming=1><I:last=1>]}");
    check(
        g_game.calls == 2 && rover_handler_is_armed(handlers, RoverHandlerId::GameLocated),
        "a forming value that is not an I field is taken and ignored, last included"
    );
    deliver("{[Rover<S:command=game_located><I:area_id=5><I:forming=0><S:pass_req=1><I:last=0>]}");
    check(
        g_game.calls == 3 && g_game.area_id == 0 && g_game.pass_required == 1 &&
            rover_handler_is_armed(handlers, RoverHandlerId::GameLocated),
        "an area id that is not a U field reads 0, and a pass_req that is not an I field reads as "
        "required"
    );
    deliver("{[Rover<S:command=game_located><I:forming=0><I:last=1><I:pass_req=0>]}");
    check(
        g_game.calls == 4 && g_game.pass_required == 0 && g_game.last == 1 &&
            !rover_handler_is_armed(handlers, RoverHandlerId::GameLocated) &&
            handlers.lobby.game_located.callback == nullptr,
        "a reply marked last is reported and disarms the handler"
    );
    deliver("{[Rover<S:command=game_located><I:forming=1><I:last=0>]}");
    check(g_game.calls == 4, "a reply after the handler disarms is left alone");

    g_rover->locate_target("", on_game_located);
    check_sent("{[GameManager<S:command=locate>]}", "locate leaves out an empty name");
    g_rover->locate_target(nullptr, on_game_located_other);
    check_sent("{[GameManager<S:command=locate>]}", "locate leaves out a null name");
    check(
        handlers.lobby.game_located.callback == on_game_located_other,
        "a second request replaces the callback"
    );
    deliver("{[Rover<S:command=game_located><I:forming=1><I:last=1>]}");
    check(
        g_game.calls == 104 && !rover_handler_is_armed(handlers, RoverHandlerId::GameLocated),
        "the replacement callback takes the next answer"
    );
}

// One field of a user_located answer beyond the four the callback takes:
// I signed, U unsigned or S text.
struct LocatedField {
    char type;
    const char* key;
    const char* value;
};

constexpr int32_t kMaxLocatedFields = 5;

// A synthetic answer for each place a user can be, with the fields the game's
// locator callback reads for it.
struct LocateCase {
    const char* what;
    const char* asked;
    int32_t loc_code;
    uint32_t user_id;
    const char* name;
    int32_t user_icon;
    LocatedField fields[kMaxLocatedFields];
    int32_t field_count;
};

const LocateCase kLocateCases[] = {
    {"logged out", "sleeper", 0, 0x11, "Sleeper", 2, {}, 0},
    {"chatting",
     "talker",
     1,
     0x12,
     "Talker",
     3,
     {{'U', "area_id", "709"},
      {'U', "room_id", "5"},
      {'S', "area_name", "Hub"},
      {'S', "room_name", "Frequency 5"}},
     4},
    {"examining battles",
     "browser",
     2,
     0x13,
     "Browser",
     1,
     {{'U', "area_id", "704"}, {'S', "area_name", "Aegis"}},
     2},
    {"in a battle",
     "fighter",
     3,
     0x14,
     "Fighter",
     4,
     {{'U', "area_id", "709"},
      {'S', "game_id", "4f1c2d3e"},
      {'I', "pass_req", "1"},
      {'S', "game_name", "Duel at dawn"},
      {'S', "area_name", "Hub"}},
     5},
    {"on another server", "roamer", 4, 0x15, "Roamer", 0, {{'S', "server_name", "Beta"}}, 1},
    {"no such user", "nobody", 5, 0, "nobody", -1, {}, 0},
    {"viewing other screens", "reader", 6, 0x16, "Reader", 5, {}, 0},
};

const LocateCase* g_locate_case = nullptr;
int g_case_calls = 0;
bool g_case_fields_ok = false;

bool bag_has(RoverPropertyBag* bag, const LocatedField& field) {
    RoverString key{};
    rover_string_assign_text(&key, field.key, static_cast<uint32_t>(strlen(field.key)));
    bool same = false;
    if (field.type == 'I') {
        int32_t value = 0;
        same = g_rover->get_field_int(bag, &key, &value) &&
               value == static_cast<int32_t>(strtol(field.value, nullptr, 10));
    } else if (field.type == 'U') {
        uint32_t value = 0;
        same = g_rover->get_field_uint(bag, &key, &value) &&
               value == static_cast<uint32_t>(strtoul(field.value, nullptr, 16));
    } else {
        RoverString value{};
        same = g_rover->get_field_string(bag, &key, &value) &&
               strcmp(rover_string_text(&value), field.value) == 0;
        rover_string_release(&value);
    }
    rover_string_release(&key);
    return same;
}

void ROVER_CALL on_located_case(
    int32_t loc_code,
    uint32_t user_id,
    const char* name,
    int32_t user_icon,
    RoverPropertyBag* fields
) {
    ++g_case_calls;
    const LocateCase& c = *g_locate_case;
    char command[32];
    read_bag_text(fields, "command", command, sizeof command);
    bool ok = loc_code == c.loc_code && user_id == c.user_id && strcmp(name, c.name) == 0 &&
              user_icon == c.user_icon && strcmp(command, "user_located") == 0 &&
              bag_entry_count(fields) == c.field_count + 1;
    for (int32_t i = 0; i < c.field_count; ++i)
        ok = ok && bag_has(fields, c.fields[i]);
    g_case_fields_ok = ok;
}

// The answer as the locator sends it, built with the record encoder and
// read back through the record text the link carries.
void deliver_located(const LocateCase& c) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::Rover, "user_located");
    check(entry != nullptr, c.what);
    if (!entry)
        return;
    rover_entry_set_int(*entry, "loc_code", c.loc_code);
    rover_entry_set_uint(*entry, "user_id", c.user_id);
    rover_entry_set_string(*entry, "name", c.name);
    rover_entry_set_int(*entry, "user_icon", c.user_icon);
    for (int32_t i = 0; i < c.field_count; ++i) {
        const LocatedField& f = c.fields[i];
        if (f.type == 'I')
            rover_entry_set_int(*entry, f.key, static_cast<int32_t>(strtol(f.value, nullptr, 10)));
        else if (f.type == 'U')
            rover_entry_set_uint(
                *entry, f.key, static_cast<uint32_t>(strtoul(f.value, nullptr, 16))
            );
        else
            rover_entry_set_string(*entry, f.key, f.value);
    }
    char text[kRoverRecordTextLimit + 1] = {};
    int32_t length = 0;
    const RoverRecordStatus status =
        rover_record_format(*record, text, static_cast<int32_t>(sizeof text - 1), &length);
    rover_record_destroy(&record);
    check(status == RoverRecordStatus::Ok, c.what);
    text[length] = '\0';
    deliver(text);
}

void check_locate_codes() {
    RoverHandlerSet& handlers = g_rover_state->handlers;
    for (const LocateCase& c : kLocateCases) {
        g_locate_case = &c;
        g_case_calls = 0;
        g_case_fields_ok = false;
        g_rover->locate_player(c.asked, on_located_case);
        char sent[64];
        snprintf(sent, sizeof sent, "{[UserManager<S:command=locate><S:name=%s>]}", c.asked);
        check_sent(sent, c.what);
        deliver_located(c);
        check(g_case_calls == 1 && g_case_fields_ok, c.what);
        check(!rover_handler_is_armed(handlers, RoverHandlerId::UserLocated), c.what);
        if (g_case_calls != 1 || !g_case_fields_ok)
            fprintf(stderr, "  locate case: %s\n", c.what);
    }
    g_locate_case = nullptr;
}

// ---- profiles ------------------------------------------------------------------

int g_profiles = 0;
bool g_profile_ok = false;
RoverPropertyBag* g_profile_fields = nullptr;
int32_t g_profile_field_count = -1;
char g_profile_login[32];
int g_ladders = 0;
int32_t g_ladder_id = -1;
int32_t g_ladder_subid = -1;
int g_updates_done = 0;
bool g_update_ok = false;

void ROVER_CALL on_profile(bool ok, RoverPropertyBag* fields) {
    ++g_profiles;
    g_profile_ok = ok;
    g_profile_fields = fields;
    g_profile_field_count = bag_entry_count(fields);
    read_bag_text(fields, "LOGIN_NAME", g_profile_login, sizeof g_profile_login);
}

void ROVER_CALL on_ladder(int32_t ladder_id, int32_t ladder_subid, RoverPropertyBag*) {
    ++g_ladders;
    g_ladder_id = ladder_id;
    g_ladder_subid = ladder_subid;
}

void ROVER_CALL on_profile_updated(bool ok) {
    ++g_updates_done;
    g_update_ok = ok;
}

void check_profiles() {
    RoverHandlerSet& handlers = g_rover_state->handlers;
    g_rover->request_get_profile(2, on_profile, on_ladder);
    check_sent(
        "{[UserManager<S:command=get_profile><U:user_id=2>]}", "get_profile, as 3.1c sends it"
    );
    deliver("{[Rover<S:command=profile_info><U:user_id=3><I:last=1>]}");
    check(g_profiles == 0, "another user's profile is not taken");
    deliver(
        "{[Rover<S:command=ladder_profile><U:user_id=2><I:ladder_id=4><I:ladder_subid=1>"
        "<I:rank=7>]}"
    );
    check(g_ladders == 1 && g_ladder_id == 4 && g_ladder_subid == 1, "a ladder profile");
    deliver(
        "{[Rover<S:command=profile_info><U:user_id=2><S:name=second><S:ALLEGIANCE=CORE>"
        "<S:VISITCOUNT=78><S:BONETIME=1988><S:LOGIN_NAME=second><S:JOINDATE=Jun 25, 2026>"
        "<I:last=1>]}"
    );
    check(
        g_profiles == 1 && g_profile_ok && g_profile_fields != nullptr &&
            g_profile_field_count == 7 && strcmp(g_profile_login, "second") == 0,
        "a profile_info reply completes the profile with its fields in a bag"
    );
    check(
        !rover_handler_is_armed(handlers, RoverHandlerId::ProfileInfo), "the profile answers once"
    );
    g_rover->request_get_profile(2, on_profile, nullptr);
    check_sent("{[UserManager<S:command=get_profile><U:user_id=2>]}", "get_profile again");
    deliver("{[Rover<S:command=profile_info><U:user_id=2><I:last=0>]}");
    check(g_profiles == 1, "a profile continues until last");
    deliver("{[Rover<S:command=profile_info><U:user_id=2><I:last=-1>]}");
    check(g_profiles == 2 && !g_profile_ok, "last -1 reports the profile failed");

    g_rover->request_set_profile(2, nullptr, on_profile_updated);
    check_sent("{[UserManager<S:command=set_profile><U:user_id=2>]}", "set_profile");
    const char* const profile[][3] = {
        {"S", "ALLEGIANCE", "ARM"},
        {"I", "VISITCOUNT", "-78"},
        {"U", "zeta", "1f"},
        {"I", "user_id", "5"},
    };
    RoverPropertyBag* properties = make_bag(profile, 4);
    g_rover->request_set_profile(2, properties, on_profile_updated);
    g_rover->destroy_property_bag(properties);
    check_sent(
        "{[UserManager<S:ALLEGIANCE=ARM><I:VISITCOUNT=-78><S:command=set_profile>"
        "<U:user_id=2><U:zeta=1f>]}",
        "set_profile adds the bag's fields by type; its user_id does not replace the request's"
    );
    deliver("{[Rover<S:command=profile_updated><U:user_id=2><I:result=1>]}");
    check(g_updates_done == 1 && !g_update_ok, "profile_updated reports a failure");
}

// ---- rooms ---------------------------------------------------------------------

struct RoomRow {
    int calls;
    int32_t change;
    int32_t area;
    int32_t room;
    char name[32];
    char description[32];
    bool needs_pw;
    int32_t users;
    int32_t type;
    char tag[16];
    bool last;
};

RoomRow g_rows{};
RoomRow g_updates{};
int32_t g_entered = -1;
int g_entered_calls = 0;
int32_t g_member_change = -1;
int32_t g_member_id = -1;
char g_member_name[32];
int32_t g_member_latency = -1;
int32_t g_member_icon = -1;
int32_t g_member_rank = -1;
char g_member_allegiance[16];
int32_t g_chat_server = -1;
int32_t g_chat_user = -1;
char g_chat_text[64];
int g_left = -1;

void ROVER_CALL on_room_row(
    int32_t change,
    int32_t area,
    int32_t room,
    const char* name,
    const char* description,
    bool needs_pw,
    int32_t users,
    int32_t type,
    const char* tag,
    bool last
) {
    g_rows.calls++;
    g_rows.change = change;
    g_rows.area = area;
    g_rows.room = room;
    snprintf(g_rows.name, sizeof g_rows.name, "%s", name);
    snprintf(g_rows.description, sizeof g_rows.description, "%s", description);
    g_rows.needs_pw = needs_pw;
    g_rows.users = users;
    g_rows.type = type;
    snprintf(g_rows.tag, sizeof g_rows.tag, "%s", tag);
    g_rows.last = last;
}

void ROVER_CALL on_room_update(
    int32_t change,
    int32_t area,
    int32_t room,
    const char* name,
    const char*,
    bool,
    int32_t users,
    int32_t type,
    const char*
) {
    g_updates.calls++;
    g_updates.change = change;
    g_updates.area = area;
    g_updates.room = room;
    snprintf(g_updates.name, sizeof g_updates.name, "%s", name);
    g_updates.users = users;
    g_updates.type = type;
}

void ROVER_CALL on_entered(int32_t result) {
    g_entered = result;
    ++g_entered_calls;
}

void ROVER_CALL on_member(
    int32_t change,
    int32_t user,
    const char* name,
    int32_t latency,
    int32_t icon,
    int32_t rank,
    const char* allegiance
) {
    g_member_change = change;
    g_member_id = user;
    snprintf(g_member_name, sizeof g_member_name, "%s", name);
    g_member_latency = latency;
    g_member_icon = icon;
    g_member_rank = rank;
    snprintf(g_member_allegiance, sizeof g_member_allegiance, "%s", allegiance);
}

void ROVER_CALL on_chat(int32_t from_server, int32_t user, const char* text) {
    g_chat_server = from_server;
    g_chat_user = user;
    snprintf(g_chat_text, sizeof g_chat_text, "%s", text);
}

void ROVER_CALL on_left(bool failed) {
    g_left = failed ? 1 : 0;
}

// Link notices the link thread queues for the pump.
void deliver_connected(int32_t yard_number, bool ok) {
    char text[160];
    snprintf(
        text,
        sizeof text,
        "{[Rover<S:command=_connect_to_boneyard><S:state=connect><I:result=%d>"
        "<I:yard_number=%d>]}",
        ok ? 1 : 0,
        yard_number
    );
    deliver(text);
}

void deliver_closed(int32_t yard_number) {
    char text[96];
    snprintf(
        text, sizeof text, "{[Rover<S:command=_boneyard_closed><I:yard_number=%d>]}", yard_number
    );
    deliver(text);
}

void enter_room_9(int32_t link, const char* password) {
    g_rover->enter_room(9, 0x709, password, on_entered, on_member, on_room_update, on_chat);
    check_sent_on(
        link,
        password ? "{[ChatManager<U:area_id=709><S:command=enter_room><S:password=pw9>"
                   "<U:room_id=9>]}"
                 : "{[ChatManager<U:area_id=709><S:command=enter_room><U:room_id=9>]}",
        "enter room 9"
    );
}

// entered_room result 2 names the room's boneyard: the session moves there,
// logs in again with the magic and enters the room there before the game
// hears of it. Relogin confirmed first and the old link's close second, then
// the other way round; failures reach the game as result 1.
void check_boneyard_moves() {
    RoverHandlerSet& handlers = g_rover_state->handlers;
    RoverState& state = *g_rover_state;
    state.user_id = 0x2a;

    enter_room_9(0, nullptr);
    deliver(
        "{[Rover<S:command=entered_room><U:area_id=709><U:room_id=9><I:result=2><S:ip=10.0.0.2>"
        "<I:port=9111>]}"
    );
    int calls = g_entered_calls;
    check(g_entered == 1 && !state.move.active, "a reply that names no magic fails the enter");

    enter_room_9(0, "pw9");
    deliver(
        "{[Rover<S:command=entered_room><U:area_id=709><U:room_id=9><I:result=2><S:ip=10.0.0.2>"
        "<I:port=9111><U:magic=5eed>]}"
    );
    check(g_entered_calls == calls, "the game hears nothing while the session moves");
    check(
        state.move.active && state.move.magic == 0x5eed && state.move.room_id == 9 &&
            state.move.area_id == 0x709 && strcmp(state.move.password, "pw9") == 0,
        "the move keeps the room to enter again"
    );
    check(
        strcmp(state.links[1].address, "10.0.0.2") == 0 && state.links[1].port == 9111,
        "boneyard link 2 dials the named boneyard"
    );
    check(
        !rover_handler_is_armed(handlers, RoverHandlerId::EnteredRoom) &&
            rover_handler_is_armed(handlers, RoverHandlerId::ConnectToBoneyard) &&
            rover_handler_is_armed(handlers, RoverHandlerId::Yard2Closed),
        "the move waits for boneyard 2's connect and hears of its close"
    );

    g_rover->enter_room(0x10, 0x709, nullptr, on_entered, on_member, on_room_update, on_chat);
    check_sent("{[ChatManager<U:area_id=709><S:command=enter_room><U:room_id=10>]}", "enter 10");
    deliver(
        "{[Rover<S:command=entered_room><U:area_id=709><U:room_id=10><I:result=2>"
        "<S:ip=10.0.0.3><I:port=9111><U:magic=6>]}"
    );
    check(
        g_entered == 1 && g_entered_calls == calls + 1 && state.move.magic == 0x5eed,
        "no second move starts while one runs"
    );

    deliver_connected(2, true);
    check_sent_on(
        1,
        "{[CommManager<S:command=connect_type><S:compression_type=text>"
        "<S:requested_type=client><S:version=0.01>]}",
        "boneyard 2 is asked for a client link"
    );
    deliver("{[Rover<S:command=connect_info><S:seed=0f1e>]}");
    check_sent_on(
        1,
        "{[UserManager<S:command=relogin><U:magic=5eed><U:user_id=2a>]}",
        "relogin goes to boneyard 2 with the user id and the magic"
    );
    check(state.move.stage == 7, "the move reaches stage 7 once relogin is sent");
    deliver("{[Rover<S:command=relogin_confirmed><I:result=0>]}");
    check(
        !state.second_boneyard && state.move.half_done && g_entered_calls == calls + 1,
        "the switch waits for boneyard 1 to close"
    );
    deliver_closed(1);
    check(
        state.second_boneyard && !state.move.active &&
            rover_handler_is_armed(handlers, RoverHandlerId::EnteredRoom) &&
            !handlers.lobby.entered_room.switching,
        "boneyard 2 becomes the session's"
    );
    check_sent_on(
        1,
        "{[ChatManager<U:area_id=709><S:command=enter_room><S:password=pw9><U:room_id=9>]}",
        "the room is entered again on boneyard 2"
    );
    deliver("{[Rover<S:command=entered_room><U:area_id=709><U:room_id=9><I:result=0>]}");
    check(g_entered == 0 && g_entered_calls == calls + 2, "the game hears the enter on boneyard 2");

    enter_room_9(1, nullptr);
    deliver(
        "{[Rover<S:command=entered_room><U:area_id=709><U:room_id=9><I:result=2><S:ip=10.0.0.1>"
        "<I:port=9112><U:magic=77>]}"
    );
    check(
        strcmp(state.links[0].address, "10.0.0.1") == 0 && state.links[0].port == 9112 &&
            rover_handler_is_armed(handlers, RoverHandlerId::Yard1Closed),
        "from boneyard 2 the move dials boneyard link 1"
    );
    deliver_connected(1, true);
    check_sent_on(
        0,
        "{[CommManager<S:command=connect_type><S:compression_type=text>"
        "<S:requested_type=client><S:version=0.01>]}",
        "boneyard 1 is asked for a client link"
    );
    deliver("{[Rover<S:command=connect_info><S:seed=0f1e>]}");
    check_sent_on(0, "{[UserManager<S:command=relogin><U:magic=77><U:user_id=2a>]}", "relogin");
    deliver_closed(2);
    check(state.second_boneyard, "the switch waits for relogin_confirmed");
    deliver("{[Rover<S:command=relogin_confirmed><I:result=0>]}");
    check(!state.second_boneyard && !state.move.active, "boneyard 1 becomes the session's again");
    check_sent("{[ChatManager<U:area_id=709><S:command=enter_room><U:room_id=9>]}", "re-enter");
    deliver("{[Rover<S:command=entered_room><U:area_id=709><U:room_id=9><I:result=0>]}");
    check(g_entered == 0 && g_entered_calls == calls + 3, "the game hears the enter on boneyard 1");

    enter_room_9(0, nullptr);
    deliver(
        "{[Rover<S:command=entered_room><U:area_id=709><U:room_id=9><I:result=2><S:ip=10.0.0.2>"
        "<I:port=9111><U:magic=5eed>]}"
    );
    deliver_connected(2, false);
    check(
        g_entered == 1 && g_entered_calls == calls + 4 && !state.move.active &&
            !rover_handler_is_armed(handlers, RoverHandlerId::ConnectToBoneyard) &&
            !rover_handler_is_armed(handlers, RoverHandlerId::Yard2Closed) &&
            !state.second_boneyard,
        "a failed connect fails the move"
    );

    enter_room_9(0, nullptr);
    deliver(
        "{[Rover<S:command=entered_room><U:area_id=709><U:room_id=9><I:result=2><S:ip=10.0.0.2>"
        "<I:port=9111><U:magic=5eed>]}"
    );
    deliver_connected(2, true);
    check_sent_on(
        1,
        "{[CommManager<S:command=connect_type><S:compression_type=text>"
        "<S:requested_type=client><S:version=0.01>]}",
        "connect_type"
    );
    deliver("{[Rover<S:command=connect_info><S:seed=0f1e>]}");
    check_sent_on(1, "{[UserManager<S:command=relogin><U:magic=5eed><U:user_id=2a>]}", "relogin");
    deliver("{[Rover<S:command=relogin_confirmed><I:result=3>]}");
    check(
        g_entered == 1 && g_entered_calls == calls + 5 && !state.move.active &&
            !state.second_boneyard && handlers.yard_closed[1] == nullptr &&
            !rover_handler_is_armed(handlers, RoverHandlerId::Yard2Closed) &&
            rover_handler_is_armed(handlers, RoverHandlerId::Yard1Closed),
        "a refused relogin fails the move and keeps boneyard 1"
    );
}

void check_rooms() {
    RoverHandlerSet& handlers = g_rover_state->handlers;
    check(g_rover->get_rooms(0x709, on_room_row), "get_rooms reports true");
    check_sent("{[ChatManager<U:area_id=709><S:command=get_rooms>]}", "get_rooms");
    deliver(
        "{[Rover<S:command=room_list><U:area_id=709><U:room_id=3><S:name=Lobby><I:users=12>"
        "<S:description=Main room><I:needs_pw=1><I:type=2><S:tag=main><I:full=1>]}"
    );
    check(
        g_rows.calls == 1 && g_rows.change == 0 && g_rows.area == 0x709 && g_rows.room == 3 &&
            strcmp(g_rows.name, "Lobby") == 0 && strcmp(g_rows.description, "Main room") == 0 &&
            g_rows.needs_pw && g_rows.users == 12 && g_rows.type == 2 &&
            strcmp(g_rows.tag, "main") == 0 && !g_rows.last,
        "a room_list row of the asked area"
    );
    deliver(
        "{[Rover<S:command=room_list><U:area_id=709><U:room_id=4><S:name=Ops><I:users=1>"
        "<I:type=9><S:change=delete><I:full=1>]}"
    );
    check(
        g_rows.calls == 2 && g_rows.change == 1 && g_rows.type == 0 && !g_rows.needs_pw &&
            g_rows.description[0] == '\0',
        "a deleted row, an unknown type reported as 0"
    );
    deliver(
        "{[Rover<S:command=room_list><U:area_id=709><U:room_id=4><S:name=Ops><I:users=1>"
        "<S:change=renamed><I:full=1>]}"
    );
    check(g_rows.calls == 2, "an unknown change is dropped");
    deliver(
        "{[Rover<S:command=room_list><U:area_id=709><U:room_id=5><S:name=Last><I:users=0>"
        "<I:full=1><I:last=1>]}"
    );
    check(g_rows.calls == 3 && g_rows.last && g_rows.type == 1, "the last row, type 1 by default");
    check(
        !rover_handler_is_armed(handlers, RoverHandlerId::RoomListFinal),
        "the last row disarms the room list"
    );

    check(
        g_rover->enter_room(5, 0x709, "pw", on_entered, on_member, on_room_update, on_chat),
        "enter_room reports true"
    );
    check_sent(
        "{[ChatManager<U:area_id=709><S:command=enter_room><S:password=pw><U:room_id=5>]}",
        "enter_room with a password"
    );
    deliver("{[Rover<S:command=entered_room><U:area_id=709><U:room_id=6><I:result=0>]}");
    check(g_entered_calls == 0, "entered_room of another room is not taken");
    deliver("{[Rover<S:command=entered_room><U:area_id=709><U:room_id=5><I:result=0>]}");
    check(g_entered == 0 && g_entered_calls == 1, "entered_room reports the result");
    check(
        !rover_handler_is_armed(handlers, RoverHandlerId::EnteredRoom), "entered_room answers once"
    );
    deliver(
        "{[Rover<S:command=room_info><U:area_id=709><U:room_id=5><U:user_id=2><S:username=second>"
        "<I:latency=7><I:icon=4><I:rank=3><S:allegiance=CORE>]}"
    );
    check(
        g_member_change == 0 && g_member_id == 2 && strcmp(g_member_name, "second") == 0 &&
            g_member_latency == 1 && g_member_icon == 4 && g_member_rank == 3 &&
            strcmp(g_member_allegiance, "CORE") == 0,
        "room_info passes the member, an unknown latency as 1"
    );
    deliver("{[Rover<S:command=room_list><U:area_id=709><U:room_id=5><S:name=Lobby><I:users=13>]}");
    check(
        g_updates.calls == 1 && g_updates.users == 13 && g_updates.room == 5,
        "room_list without full goes to the entered room's handler"
    );
    deliver("{[Rover<S:command=chat_msg><U:area_id=709><U:room_id=5><U:user_id=2><S:message=hi>]}");
    check(
        g_chat_server == 0 && g_chat_user == 2 && strcmp(g_chat_text, "hi") == 0, "a member speaks"
    );
    deliver("{[Rover<S:command=chat_msg><U:area_id=709><U:room_id=5><S:message=notice>]}");
    check(
        g_chat_server == 1 && g_chat_user == 0 && strcmp(g_chat_text, "notice") == 0,
        "a line without a user comes from the server"
    );

    check(g_rover->send_chat(5, 0x709, 0, "hello"), "send_chat reports true");
    check_sent(
        "{[ChatManager<U:area_id=709><S:command=say><S:message=hello><U:room_id=5>]}",
        "say to the room"
    );
    g_rover->send_chat(5, 0x709, 7, "psst");
    check_sent(
        "{[ChatManager<U:area_id=709><S:command=say><S:message=psst><U:room_id=5><U:user_id=7>]}",
        "say to one member"
    );
    deliver(
        "{[Rover<S:command=chat_msg><U:area_id=709><U:room_id=5><U:user_id=2><S:message=caf\xe9>]}"
    );
    check(strcmp(g_chat_text, "caf\xc3\xa9") == 0, "a room line is read as Windows-1252");
    check(g_rover->send_chat(5, 0x709, 0, "caf\xc3\xa9\xe6\x9d\xb1"), "send_chat of a room line");
    check_sent(
        "{[ChatManager<U:area_id=709><S:command=say><S:message=caf\xe9?><U:room_id=5>]}",
        "a room line is written as Windows-1252"
    );
    rover_set_wire_utf8(true);
    g_rover->send_chat(5, 0x709, 0, "\xe6\x9d\xb1\xe4\xba\xac");
    check_sent(
        "{[ChatManager<U:area_id=709><S:command=say><S:message=\xe6\x9d\xb1\xe4\xba\xac><U:room_id="
        "5>]}",
        "a room line stays UTF-8 when the server accepts it"
    );
    rover_set_wire_utf8(false);

    // The drop-in library speaks Windows-1252 at the slot table. The same byte then leaves and
    // comes back unchanged, because the wire is Windows-1252 too. UTF-8 mode, above, turned "é"
    // into 0xE9 on the wire and 0xE9 from the wire back into UTF-8.
    rover_set_interface_windows1252(true);
    deliver(
        "{[Rover<S:command=chat_msg><U:area_id=709><U:room_id=5><U:user_id=2><S:message=\xe9>]}"
    );
    check(
        strcmp(g_chat_text, "\xe9") == 0, "a Windows-1252 room line is handed back as Windows-1252"
    );
    check(g_rover->send_chat(5, 0x709, 0, "\xe9"), "send_chat of a Windows-1252 room line");
    check_sent(
        "{[ChatManager<U:area_id=709><S:command=say><S:message=\xe9><U:room_id=5>]}",
        "a Windows-1252 room line reaches the wire unchanged"
    );
    g_rover->send_player_info(0x709, "game", 0x2a, "\xe9", "ARM", 1);
    check_sent(
        "{[GameManager<S:allegiance=ARM><U:area_id=709><S:command=player_info><U:dpid=2a>"
        "<S:game_id=game><S:name=\xe9><I:team=1>]}",
        "a Windows-1252 player name reaches the wire unchanged"
    );
    rover_set_interface_windows1252(false);

    check(
        g_rover->switch_room(
            8, 0x709, nullptr, 5, 0x709, on_entered, on_member, on_room_update, on_chat
        ),
        "switch_room reports true"
    );
    check_sent(
        "{[ChatManager<U:area_id=709><S:command=enter_room><U:room_id=8>]}",
        "switch_room enters without a password"
    );
    deliver("{[Rover<S:command=entered_room><U:area_id=709><U:room_id=8>]}");
    check(g_entered == 0 && g_entered_calls == 2, "the switch succeeded");
    check_sent(
        "{[ChatManager<U:area_id=709><S:command=leave_room><U:room_id=5>]}",
        "a successful switch leaves the old room"
    );
    check(
        handlers.lobby.room_info.room_id == 8 && handlers.lobby.chat_msg.room_id == 8,
        "the room handlers follow the new room"
    );
    check(
        handlers.lobby.entered_room.callback == nullptr &&
            handlers.lobby.entered_room.room_id == 0 && !handlers.lobby.entered_room.switching,
        "an answered enter clears its handler"
    );
    check_boneyard_moves();

    check(g_rover->leave_room(8, 0x709, on_left), "leave_room reports true");
    check_sent("{[ChatManager<U:area_id=709><S:command=leave_room><U:room_id=8>]}", "leave_room");
    check(
        !rover_handler_is_armed(handlers, RoverHandlerId::ChatMessage) &&
            !rover_handler_is_armed(handlers, RoverHandlerId::RoomInfo) &&
            !rover_handler_is_armed(handlers, RoverHandlerId::RoomList),
        "leaving drops the room handlers"
    );
    deliver("{[Rover<S:command=left_room><U:area_id=709><U:room_id=8><I:result=0>]}");
    check(g_left == 0, "left_room reports success");
    g_rover->leave_room(0, 0x709, on_left);
    check_nothing_sent("leaving room 0 sends nothing");
}

// ---- room creation -------------------------------------------------------------

struct RoomCreated {
    int calls;
    bool ids_given;
    uint32_t area_id;
    uint32_t room_id;
    char name[32];
};

RoomCreated g_created{};

void ROVER_CALL on_room_created(uint32_t* area_id, uint32_t* room_id, const char* name) {
    ++g_created.calls;
    g_created.ids_given = area_id != nullptr && room_id != nullptr;
    g_created.area_id = area_id ? *area_id : 0xffffffffU;
    g_created.room_id = room_id ? *room_id : 0xffffffffU;
    snprintf(g_created.name, sizeof g_created.name, "%s", name ? name : "(null)");
}

void ROVER_CALL on_room_created_other(uint32_t*, uint32_t*, const char*) {
}

// A room_created reply built with the record encoder and read back through
// the record text the link carries.
void deliver_room_created(uint32_t area_id, uint32_t room_id, const char* name) {
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::Rover, "room_created");
    check(entry != nullptr, "a room_created reply is encoded");
    if (!entry)
        return;
    rover_entry_set_uint(*entry, "area_id", area_id);
    rover_entry_set_uint(*entry, "room_id", room_id);
    rover_entry_set_string(*entry, "name", name);
    char text[kRoverRecordTextLimit + 1] = {};
    int32_t length = 0;
    const RoverRecordStatus status =
        rover_record_format(*record, text, static_cast<int32_t>(sizeof text - 1), &length);
    rover_record_destroy(&record);
    check(status == RoverRecordStatus::Ok, "a room_created reply is formatted");
    text[length] = '\0';
    deliver(text);
}

/// Checks that slot 69 queues create_room with the keys and types 3.1c sends (area_id U, name S,
/// then desc S and password S only when not empty) and arms handler 44, which takes any
/// room_created once.
void check_room_creation() {
    RoverHandlerSet& handlers = g_rover_state->handlers;
    RoverRoomCreatedHandler& pending = handlers.lobby.room_created;

    deliver_room_created(0x709, 12, "Unasked");
    check(g_created.calls == 0, "room_created before any creation is not taken");

    g_rover->create_room(0x709, "Proving Grounds", "Duel practice", "pw5", on_room_created_other);
    check_sent(
        "{[ChatManager<U:area_id=709><S:command=create_room><S:desc=Duel practice>"
        "<S:name=Proving Grounds><S:password=pw5>]}",
        "create_room with a description and a password"
    );
    check(
        rover_handler_is_armed(handlers, RoverHandlerId::RoomCreated) &&
            pending.callback == on_room_created_other && pending.area_id == 0x709 &&
            strcmp(pending.name, "Proving Grounds") == 0,
        "creation keeps the area, the name and the callback"
    );

    g_rover->create_room(0x704, "Frequency_For_The_Whole_Arm", "", nullptr, on_room_created);
    check_sent(
        "{[ChatManager<U:area_id=704><S:command=create_room><S:name=Frequency_For_The_Whole_Arm>]}",
        "create_room leaves out an empty description and no password"
    );
    check(
        rover_handler_is_armed(handlers, RoverHandlerId::RoomCreated) &&
            pending.callback == on_room_created && pending.area_id == 0x704 &&
            strcmp(pending.name, "Frequency_For_The_Whole") == 0,
        "a second creation replaces the first and keeps the name's first 23 characters"
    );
    g_rover->create_room(0x704, "Ops", nullptr, "", on_room_created);
    check_sent(
        "{[ChatManager<U:area_id=704><S:command=create_room><S:name=Ops>]}",
        "create_room leaves out no description and an empty password"
    );

    deliver("{[Rover<S:command=room_create><U:area_id=704><U:room_id=9><S:name=Ops>]}");
    check(
        g_created.calls == 0 && rover_handler_is_armed(handlers, RoverHandlerId::RoomCreated),
        "another command is left alone"
    );
    deliver_room_created(0x709, 0x2b, "Somewhere Else");
    check(
        g_created.calls == 1 && g_created.ids_given && g_created.area_id == 0x709 &&
            g_created.room_id == 0x2b && strcmp(g_created.name, "Somewhere Else") == 0,
        "room_created passes its area, room and name whatever was asked"
    );
    check(
        !rover_handler_is_armed(handlers, RoverHandlerId::RoomCreated) &&
            pending.callback == nullptr && pending.area_id == 0x704 &&
            strcmp(pending.name, "Ops") == 0,
        "room_created answers once and forgets only the callback"
    );
    deliver_room_created(0x709, 0x2c, "Again");
    check(g_created.calls == 1, "a second room_created is not taken");

    g_rover->create_room(0x709, "Taken", nullptr, nullptr, on_room_created);
    check_sent(
        "{[ChatManager<U:area_id=709><S:command=create_room><S:name=Taken>]}", "create again"
    );
    deliver_room_created(0, 0, "Taken");
    check(
        g_created.calls == 2 && g_created.ids_given && g_created.area_id == 0 &&
            g_created.room_id == 0 && strcmp(g_created.name, "Taken") == 0,
        "a refusal with zero ids passes zeros"
    );

    g_rover->create_room(0x709, "Bare", nullptr, nullptr, on_room_created);
    check_sent(
        "{[ChatManager<U:area_id=709><S:command=create_room><S:name=Bare>]}", "create a third time"
    );
    deliver("{[Rover<S:command=room_created><I:room_id=5>]}");
    check(
        g_created.calls == 3 && g_created.ids_given && g_created.area_id == 0 &&
            g_created.room_id == 0 && g_created.name[0] == '\0',
        "a reply without an area or name, or with a room id that is not a U field, passes 0 "
        "and empty text"
    );
    check(
        !rover_handler_is_armed(handlers, RoverHandlerId::RoomCreated),
        "even an empty reply answers the creation"
    );
}

// ---- battles -------------------------------------------------------------------

int g_battles_done = -1;
int g_battle_rows = 0;
int32_t g_battle_change = -1;
char g_battle_game[40];
char g_battle_name[32];
int32_t g_battle_type = -1;
int32_t g_battle_forming = -1;
int32_t g_battle_host = -1;
RoverPropertyBag* g_battle_fields = nullptr;
int32_t g_battle_field_count = -1;
char g_battle_map[32];
int32_t g_intel_no_game = -1;
char g_intel_text[64];
char g_intel_game[40];

struct Intel {
    int calls;
    char game[40];
    uint32_t user;
    char username[32];
    int32_t has_password;
    char mission[32];
    int32_t skill;
    int32_t players;
    int32_t on_death;
    bool failed;
};

Intel g_intel{};

struct Report {
    int calls;
    uint32_t user;
    int32_t icon;
    char username[32];
    int32_t kills;
    int32_t score;
    char message[32];
};

Report g_report{};

void ROVER_CALL on_battles_done(bool failed) {
    g_battles_done = failed ? 1 : 0;
}

void ROVER_CALL on_battle(
    int32_t change, const char* game, const char* name, int32_t type, int32_t forming, int32_t host
) {
    ++g_battle_rows;
    g_battle_change = change;
    snprintf(g_battle_game, sizeof g_battle_game, "%s", game);
    snprintf(g_battle_name, sizeof g_battle_name, "%s", name);
    g_battle_type = type;
    g_battle_forming = forming;
    g_battle_host = host;
}

void ROVER_CALL on_battle_fields(
    int32_t, const char* game, const char*, int32_t, int32_t, RoverPropertyBag* fields
) {
    snprintf(g_battle_game, sizeof g_battle_game, "%s", game);
    g_battle_fields = fields;
    g_battle_field_count = bag_entry_count(fields);
    read_bag_text(fields, "map", g_battle_map, sizeof g_battle_map);
}

void ROVER_CALL on_intel_message(int32_t no_game, const char* game, const char* text) {
    g_intel_no_game = no_game;
    snprintf(g_intel_game, sizeof g_intel_game, "%s", game ? game : "(null)");
    snprintf(g_intel_text, sizeof g_intel_text, "%s", text);
}

void ROVER_CALL on_intel(
    const char* game,
    uint32_t user,
    const char* username,
    int32_t has_password,
    const char* mission,
    int32_t skill,
    int32_t players,
    int32_t on_death,
    bool failed
) {
    g_intel.calls++;
    snprintf(g_intel.game, sizeof g_intel.game, "%s", game);
    g_intel.user = user;
    snprintf(g_intel.username, sizeof g_intel.username, "%s", username);
    g_intel.has_password = has_password;
    snprintf(g_intel.mission, sizeof g_intel.mission, "%s", mission);
    g_intel.skill = skill;
    g_intel.players = players;
    g_intel.on_death = on_death;
    g_intel.failed = failed;
}

void ROVER_CALL on_report(
    const char*,
    uint32_t user,
    int32_t icon,
    const char* username,
    int32_t kills,
    int32_t,
    int32_t,
    int32_t,
    int32_t score,
    const char* message
) {
    g_report.calls++;
    g_report.user = user;
    g_report.icon = icon;
    snprintf(g_report.username, sizeof g_report.username, "%s", username);
    g_report.kills = kills;
    g_report.score = score;
    snprintf(g_report.message, sizeof g_report.message, "%s", message);
}

constexpr char kGame[] = "9bfaa0d0f7b93f12";

void check_battles() {
    RoverHandlerSet& handlers = g_rover_state->handlers;
    check(
        g_rover->enter_battles(0x709, on_battles_done, on_battle, on_intel_message),
        "enter_battles reports true"
    );
    check_sent(
        "{[GameManager<U:area_id=709><S:command=enter_battles>]}", "enter_battles, as 3.1c sends it"
    );
    deliver("{[Rover<S:command=battles_entered><U:area_id=709><I:result=0>]}");
    check(g_battles_done == 0, "battles_entered reports success");
    deliver(
        "{[Rover<S:command=battle_info><U:area_id=709><S:game_id=9bfaa0d0f7b93f12><S:name=test>"
        "<I:type=4><I:forming=1>]}"
    );
    check(
        g_battle_rows == 1 && g_battle_change == 0 && strcmp(g_battle_game, kGame) == 0 &&
            strcmp(g_battle_name, "test") == 0 && g_battle_type == 4 && g_battle_forming == 1 &&
            g_battle_host == 2,
        "a forming battle_info row"
    );
    deliver(
        "{[Rover<S:command=battle_info><U:area_id=709><S:game_id=a><S:name=b><I:type=1>"
        "<S:host=Core><S:change=delete>]}"
    );
    check(
        g_battle_rows == 2 && g_battle_change == 1 && g_battle_forming == 0 && g_battle_host == 1,
        "a deleted Core-hosted battle that is not forming"
    );
    deliver(
        "{[Rover<S:command=battle_info><U:area_id=709><S:game_id=a><S:name=b><I:type=1>"
        "<I:forming=0>]}"
    );
    deliver(
        "{[Rover<S:command=battle_info><U:area_id=709><S:game_id=a><S:name=b><I:type=1>"
        "<S:host=X>]}"
    );
    check(g_battle_rows == 2, "a forming other than 1 or an unknown host drops the row");
    deliver("{[Rover<S:command=intel_msg><U:area_id=709><S:message=Game over>]}");
    check(
        g_intel_no_game == 1 && strcmp(g_intel_game, "(null)") == 0 &&
            strcmp(g_intel_text, "Game over") == 0,
        "intel_msg without a game"
    );

    check(g_rover->get_battle(0x709, kGame, on_intel, on_report), "get_battle reports true");
    check_sent(
        "{[GameManager<U:area_id=709><S:command=get_battle><S:game_id=9bfaa0d0f7b93f12>]}",
        "get_battle, as 3.1c sends it"
    );
    deliver(
        "{[Rover<S:command=battle_intel><U:area_id=709><S:game_id=9bfaa0d0f7b93f12><U:user_id=1>"
        "<S:username=kestrel><S:mission=Acid Pools><I:skill=0><I:players=2><I:on_death=2>"
        "<I:result=0>]}"
    );
    check(
        g_intel.calls == 1 && strcmp(g_intel.game, kGame) == 0 && g_intel.user == 1 &&
            strcmp(g_intel.username, "kestrel") == 0 && g_intel.has_password == 0 &&
            strcmp(g_intel.mission, "Acid Pools") == 0 && g_intel.skill == 0 &&
            g_intel.players == 0 && g_intel.on_death == 2 && !g_intel.failed,
        "battle_intel with players 2 read as selector 0"
    );
    check(
        !rover_handler_is_armed(handlers, RoverHandlerId::BattleIntel), "battle_intel answers once"
    );
    deliver(
        "{[Rover<S:command=battle_report><U:area_id=709><S:game_id=9bfaa0d0f7b93f12><U:user_id=1>"
        "<S:username=kestrel>]}"
    );
    deliver(
        "{[Rover<S:command=battle_report><U:area_id=709><S:game_id=9bfaa0d0f7b93f12><U:user_id=2>"
        "<S:username=second><I:icon=3><I:kills=9><I:score=40><S:message=gg>]}"
    );
    check(
        g_report.calls == 2 && g_report.user == 2 && g_report.icon == 3 &&
            strcmp(g_report.username, "second") == 0 && g_report.kills == 9 &&
            g_report.score == 40 && strcmp(g_report.message, "gg") == 0,
        "battle_report lines, the handler staying armed"
    );
    deliver(
        "{[Rover<S:command=battle_report><U:area_id=709><S:game_id=other><U:user_id=2>"
        "<S:username=x>]}"
    );
    check(g_report.calls == 2, "a report of another game is not taken");

    check(g_rover->leave_battles(0x709, on_battles_done), "leave_battles reports true");
    check_sent(
        "{[GameManager<U:area_id=709><S:command=leave_battles>]}", "leave_battles, as 3.1c sends it"
    );
    check(
        !rover_handler_is_armed(handlers, RoverHandlerId::BattleInfo) &&
            !rover_handler_is_armed(handlers, RoverHandlerId::IntelMessage) &&
            !rover_handler_is_armed(handlers, RoverHandlerId::BattleReport),
        "leaving drops the battle handlers"
    );
    g_battles_done = -1;
    deliver("{[Rover<S:command=battles_left><U:area_id=709><I:result=1>]}");
    check(g_battles_done == 1, "battles_left reports a failure");

    g_rover->enter_battles_kv(0x709, on_battles_done, on_battle_fields, on_intel_message);
    check_sent("{[GameManager<U:area_id=709><S:command=enter_battles>]}", "enter_battles_kv");
    deliver(
        "{[Rover<S:command=battle_info><U:area_id=709><S:game_id=kv><S:name=b><I:type=1>"
        "<S:map=Acid Pools>]}"
    );
    check(
        strcmp(g_battle_game, "kv") == 0 && g_battle_fields != nullptr &&
            g_battle_field_count == 1 && strcmp(g_battle_map, "Acid Pools") == 0,
        "the key/value battle_info passes its undecoded fields in a bag"
    );
    g_rover->leave_battles_kv(0x709, on_battles_done);
    check_sent("{[GameManager<U:area_id=709><S:command=leave_battles>]}", "leave_battles_kv");
    rover_handlers_disarm_all(handlers);
}

// ---- HTTP fetches through a platform that answers from memory -----------------

const RoverPlatform* g_native = nullptr;
RoverPlatform g_fake;
constexpr RoverSocket kFakeSocket = 0x5eed;

struct FakeServer {
    char host[64];
    uint16_t port;
    char request[1024];
    const char* response;
    int32_t first_piece; // size of the first receive; 10 bytes after it
    int32_t served;
    int connects;
};

FakeServer g_server{};

RoverSocket fake_connect(const char* host, uint16_t port) {
    snprintf(g_server.host, sizeof g_server.host, "%s", host);
    g_server.port = port;
    g_server.served = 0;
    g_server.request[0] = '\0';
    ++g_server.connects;
    return kFakeSocket;
}

int32_t fake_send(RoverSocket socket, const void* data, int32_t length) {
    if (socket != kFakeSocket)
        return g_native->tcp_send(socket, data, length);
    int32_t kept = length < 1023 ? length : 1023;
    memcpy(g_server.request, data, static_cast<size_t>(kept));
    g_server.request[kept] = '\0';
    return length;
}

// Serves the response in pieces: first_piece bytes, then 10 at a time.
int32_t fake_recv(RoverSocket socket, void* buffer, int32_t capacity) {
    if (socket != kFakeSocket)
        return g_native->tcp_recv(socket, buffer, capacity);
    int32_t left = static_cast<int32_t>(strlen(g_server.response)) - g_server.served;
    int32_t piece = g_server.served == 0 ? g_server.first_piece : 10;
    int32_t size = left < piece ? left : piece;
    if (size > capacity)
        size = capacity;
    memcpy(buffer, g_server.response + g_server.served, static_cast<size_t>(size));
    g_server.served += size;
    return size;
}

void fake_close(RoverSocket socket) {
    if (socket != kFakeSocket)
        g_native->tcp_close(socket);
}

std::atomic<int> g_fetches{0};
char g_fetched_url[128];
char g_fetched_file[64];
int32_t g_fetch_status = -1;
std::atomic<int32_t> g_percent{-1};

bool ROVER_CALL on_fetched(const char* url, const char* file, int32_t status) {
    snprintf(g_fetched_url, sizeof g_fetched_url, "%s", url);
    snprintf(g_fetched_file, sizeof g_fetched_file, "%s", file);
    g_fetch_status = status;
    ++g_fetches;
    return true;
}

void ROVER_CALL on_progress(int32_t percent) {
    g_percent = percent;
}

bool wait_for_fetches(int count) {
    for (int waited = 0; waited < 5000; waited += 5) {
        g_rover->pump_events();
        if (g_fetches >= count)
            return true;
        sleep_milliseconds(5);
    }
    return false;
}

bool file_equals(const char* path, const char* expected) {
    FILE* file = oa::platform::open_file(path, "rb");
    if (!file)
        return false;
    char content[512] = {};
    size_t length = fread(content, 1, sizeof content - 1, file);
    fclose(file);
    return length == strlen(expected) && memcmp(content, expected, length) == 0;
}

constexpr char kNewsBody[] = "[NEWS]\r\n{\r\ncommand=news;\r\n}\r\n";

/// Checks that fetch_http_file downloads a news file from a local server and reports its progress.
void check_http_fetch() {
    static char ok_response[256];
    snprintf(
        ok_response,
        sizeof ok_response,
        "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n%s",
        static_cast<int>(strlen(kNewsBody)),
        kNewsBody
    );
    g_server.response = ok_response;
    g_server.first_piece = 80;
    check(g_rover->activate(), "activate starts the fetch thread");
    check(
        g_rover->fetch_http_file(
            "http://files.example:8080/boneyards/ta/news/news.def",
            "news.def",
            on_fetched,
            on_progress
        ),
        "fetch_http_file reports true"
    );
    check(wait_for_fetches(1), "the fetch completes");
    check(
        strcmp(g_server.host, "files.example") == 0 && g_server.port == 8080,
        "the fetch connects to the URL's host and port"
    );
    check(
        strcmp(
            g_server.request,
            "GET /boneyards/ta/news/news.def HTTP/1.0\r\nAccept: */*\r\nUser-Agent: Mozilla/3.0 "
            "(compatible; getpage 1.0)\r\nHost: files.example:8080\r\n\r\n"
        ) == 0,
        "the request 3.1c sends"
    );
    check(
        strcmp(g_fetched_url, "http://files.example:8080/boneyards/ta/news/news.def") == 0 &&
            strcmp(g_fetched_file, "news.def") == 0 && g_fetch_status == 0,
        "the completion names the URL and file, status 0"
    );
    check(file_equals("news.def", kNewsBody), "the body is saved without the headers");
    // Progress counts from the receive after the one that held Content-Length:
    // 14 of the 29 body bytes arrive after the first 80-byte receive.
    check(g_percent == 14 * 100 / 29, "progress counts the receives after the headers");

    g_server.response = "HTTP/1.0 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found";
    g_rover->fetch_http_file("files.example/missing", "missing.def", on_fetched, nullptr);
    check(wait_for_fetches(2), "a failed fetch still completes");
    check(
        g_fetch_status == 0 && file_equals("missing.def", ""),
        "a 404 reads as status 0 with an empty file"
    );
    check(g_server.port == 8080, "a URL without a port keeps the last one");

    // Only the first receive is searched for the end of the headers; the
    // rest of headers that straddle it are saved as body.
    g_server.response = ok_response;
    g_server.first_piece = 30;
    g_percent = -1;
    g_rover->fetch_http_file(
        "http://files.example:8080/split.def", "split.def", on_fetched, on_progress
    );
    check(wait_for_fetches(3), "a fetch whose headers straddle two receives completes");
    check(
        file_equals("split.def", ok_response + 30) && g_percent == -1,
        "headers past the first receive are kept, and Content-Length there is not seen"
    );
    g_server.first_piece = 80;

    g_server.response = ok_response;
    int connects = g_server.connects;
    g_rover->fetch_http_file("ftp://files.example/news.def", "ftp.def", on_fetched, nullptr);
    g_rover->fetch_http_file("http://files.example:0/news.def", "zero.def", on_fetched, nullptr);
    g_rover->fetch_http_file(
        "http://files.example:40000/news.def", "high.def", on_fetched, nullptr
    );
    g_rover->fetch_http_file("http://files.example:81", "noslash.def", on_fetched, nullptr);
    g_rover->fetch_http_file("HTTP://files.example:82/last.def", "last.def", on_fetched, nullptr);
    check(wait_for_fetches(4), "the fetch after the refused URLs completes");
    check(
        g_server.connects == connects + 1 && strcmp(g_fetched_file, "last.def") == 0 &&
            g_server.port == 82,
        "other schemes, port 0, ports above 32767 and a port without a path are dropped"
    );
    g_rover->fetch_http_file("http://files.example", "root.def", on_fetched, nullptr);
    check(wait_for_fetches(5), "a fetch of a URL without a path completes");
    check(
        strcmp(
            g_server.request,
            "GET / HTTP/1.0\r\nAccept: */*\r\nUser-Agent: Mozilla/3.0 "
            "(compatible; getpage 1.0)\r\nHost: files.example:82\r\n\r\n"
        ) == 0,
        "a URL without a path asks for /"
    );
    g_rover->deactivate();
    remove("news.def");
    remove("missing.def");
    remove("split.def");
    remove("last.def");
    remove("root.def");
}

// Fills out with count copies of mark between head and tail.
void fill_between(char* out, const char* head, char mark, int32_t count, const char* tail) {
    int32_t at = static_cast<int32_t>(strlen(head));
    memcpy(out, head, static_cast<size_t>(at));
    memset(out + at, mark, static_cast<size_t>(count));
    memcpy(out + at + count, tail, strlen(tail) + 1);
}

void check_report_wire() {
    rover_set_wire_utf8(false);
    g_rover->send_score_report_message(0x709, "game", 0x2a, "caf\xc3\xa9\xe6\x9d\xb1");
    check_sent(
        "{[GameManager<U:area_id=709><S:command=score_report><U:dpid=2a><S:game_id=game>"
        "<S:message=caf\xe9?>]}",
        "a chat relay writes Windows-1252 and ? for what it cannot hold"
    );

    char long_line[301];
    memset(long_line, 'A', 300);
    long_line[300] = '\0';
    g_rover->send_score_report_message(0x709, "game", 0x2a, long_line);
    char expected[640];
    fill_between(
        expected,
        "{[GameManager<U:area_id=709><S:command=score_report><U:dpid=2a><S:game_id=game><S:"
        "message=",
        'A',
        255,
        ">]}"
    );
    check_sent(expected, "a chat relay cuts its message at 255 bytes");

    char boundary[260];
    memset(boundary, 'A', 253);
    memcpy(boundary + 253, "\xe6\x97\xa5", 4);
    g_rover->send_score_report_message(0x709, "game", 0x2a, boundary);
    fill_between(
        expected,
        "{[GameManager<U:area_id=709><S:command=score_report><U:dpid=2a><S:game_id=game><S:"
        "message=",
        'A',
        253,
        "?>]}"
    );
    check_sent(expected, "a chat relay cuts on a character boundary");

    char name[41];
    memset(name, 'N', 40);
    name[40] = '\0';
    char side[21];
    memset(side, 'S', 20);
    side[20] = '\0';
    g_rover->send_player_info(0x709, "game", 0x2a, name, side, 1);
    check_sent(
        "{[GameManager<S:allegiance=SSSSSSSSSSSSSSS><U:area_id=709><S:command=player_info>"
        "<U:dpid=2a><S:game_id=game><S:name=NNNNNNNNNNNNNNNNNNNNNNNNNNNNN><I:team=1>]}",
        "a player name cuts at 29 bytes and a side at 15"
    );

    char game_id[41];
    memset(game_id, 'G', 40);
    game_id[40] = '\0';
    char mission[131];
    memset(mission, 'M', 130);
    mission[130] = '\0';
    g_rover->send_game_settings(
        0x709, game_id, 0, mission, 2, 2, 1000, 1000, 2, 1, 1, 250, 1, 1, 0
    );
    char settings[400];
    int32_t at = snprintf(
        settings,
        sizeof settings,
        "{[GameManager<U:area_id=709><S:command=game_settings><S:game_id="
    );
    memset(settings + at, 'G', 36);
    at += 36;
    at += snprintf(settings + at, sizeof settings - static_cast<size_t>(at), "><S:mission=");
    memset(settings + at, 'M', 127);
    memcpy(settings + at + 127, ">]}", 4);
    check_sent(settings, "a mission cuts at 127 bytes and a game id at 36");

    g_rover->send_left_game(0x709, game_id);
    fill_between(
        expected, "{[GameManager<U:area_id=709><S:command=left_game><S:game_id=", 'G', 40, ">]}"
    );
    check_sent(expected, "leaving a game does not cut the game id");

    rover_set_wire_utf8(true);
    g_rover->send_score_report_message(0x709, "game", 0x2a, "\xe6\x9d\xb1\xe4\xba\xac");
    check_sent(
        "{[GameManager<U:area_id=709><S:command=score_report><U:dpid=2a><S:game_id=game>"
        "<S:message=\xe6\x9d\xb1\xe4\xba\xac>]}",
        "a chat relay stays UTF-8 when the server accepts it"
    );
    rover_set_wire_utf8(false);
}

} // namespace

int main() {
    const auto temporary_value = oa::platform::environment_value("TMPDIR");
    const char* temporary = temporary_value ? temporary_value->c_str() : nullptr;
    char directory[512];
    snprintf(
        directory,
        sizeof directory,
        "%s/rover-lobby-test-%d",
        temporary && *temporary ? temporary : ".",
        static_cast<int>(process_id())
    );
    make_directory(directory);
    if (change_directory(directory) != 0) {
        fprintf(stderr, "cannot prepare %s\n", directory);
        return 1;
    }

    g_native = rover_native_platform();
    g_fake = *g_native;
    g_fake.tcp_connect = fake_connect;
    g_fake.tcp_send = fake_send;
    g_fake.tcp_recv = fake_recv;
    g_fake.tcp_close = fake_close;
    rover_use_platform(&g_fake);

    rover_get_interface(&g_rover);
    check(g_rover->attach(), "attach");
    if (g_rover_state) {
        check_war_feeds();
        check_locator();
        check_locate_codes();
        check_game_locate();
        check_profiles();
        check_rooms();
        check_room_creation();
        check_battles();
        check_http_fetch();
        check_report_wire();
        check_nothing_sent("no request left unchecked");
        g_rover->detach();
    }
    rover_use_platform(nullptr);

    if (change_directory("..") == 0)
        remove_directory(directory);
    if (failures)
        return 1;
    puts("rover lobby: ok");
    return 0;
}
