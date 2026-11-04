// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Chat through Rover. A server that does not take UTF-8 receives Windows-1252,
// with "?" for a character that code page cannot hold, and the same bytes come
// back as UTF-8. A server that takes UTF-8 keeps the text, including a name.

#include "../src/state.h"

#include "oa/rover/interface.h"
#include "oa/rover/platform.h"
#include "oa/rover/wire_text.h"
#include "oa/platform/system.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#define make_directory(path) _mkdir(path)
#define change_directory(path) _chdir(path)
#define remove_directory(path) _rmdir(path)
#define process_id() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_directory(path) mkdir(path, 0755)
#define change_directory(path) chdir(path)
#define remove_directory(path) rmdir(path)
#define process_id() getpid()
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
const RoverPlatform* g_native = nullptr;
RoverPlatform g_fake;

constexpr const char* kJose = "Jos\xc3\xa9";
constexpr const char* kJoseWire = "Jos\xe9";
constexpr const char* kSkoda = "\xc5\xa0koda";
constexpr const char* kSkodaWire = "\x8akoda";
constexpr const char* kGreek = "\xce\x95\xce\xbb";
constexpr const char* kCyrillic = "\xd0\xaf\xd1\x8e";
constexpr const char* kTokyo = "\xe6\x9d\xb1\xe4\xba\xac";

char g_chat[256];
char g_name[64];
int g_entered = -1;

void ROVER_CALL on_entered(int32_t result) {
    g_entered = result;
}

void ROVER_CALL on_member(
    int32_t, int32_t, const char* username, int32_t, int32_t, int32_t, const char* /*allegiance*/
) {
    snprintf(g_name, sizeof g_name, "%s", username != nullptr ? username : "");
}

void ROVER_CALL on_room_update(
    int32_t, int32_t, int32_t, const char*, const char*, bool, int32_t, int32_t, const char*
) {
}

void ROVER_CALL on_chat(int32_t, int32_t, const char* text) {
    snprintf(g_chat, sizeof g_chat, "%s", text != nullptr ? text : "");
}

void ROVER_CALL on_left(bool) {
}

RoverSocket fake_connect(const char*, uint16_t) {
    return 1;
}

int32_t fake_send(RoverSocket, const void*, int32_t length) {
    return length;
}

int32_t fake_recv(RoverSocket, void*, int32_t) {
    return 0;
}

void fake_close(RoverSocket) {
}

void deliver(const char* text) {
    RoverRecord* pending = nullptr;
    RoverRecord* record = nullptr;
    int32_t consumed = 0;
    const RoverRecordStatus status = rover_record_receive(
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

void check_sent(const char* expected, const char* what) {
    RoverRecord* record = rover_queue_pop(g_rover_state->outbound[0]);
    char text[kRoverRecordTextLimit + 1] = {};
    if (record != nullptr) {
        int32_t length = 0;
        rover_record_format(*record, text, static_cast<int32_t>(sizeof text - 1), &length);
        text[length] = '\0';
        rover_record_destroy(&record);
    }
    check(strcmp(text, expected) == 0, what);
    if (strcmp(text, expected) != 0)
        fprintf(stderr, "  sent:     %s\n  expected: %s\n", text, expected);
}

void expect_say(const char* wire_message, const char* what) {
    char expected[512];
    snprintf(
        expected,
        sizeof expected,
        "{[ChatManager<U:area_id=709><S:command=say><S:message=%s><U:room_id=5>]}",
        wire_message
    );
    check_sent(expected, what);
}

void say(const char* message, const char* wire_message, const char* what) {
    check(g_rover->send_chat(5, 0x709, 0, message), what);
    expect_say(wire_message, what);
}

void deliver_chat(const char* wire_message) {
    char text[512];
    snprintf(
        text,
        sizeof text,
        "{[Rover<S:command=chat_msg><U:area_id=709><U:room_id=5><U:user_id=2><S:message=%s>]}",
        wire_message
    );
    deliver(text);
}

void deliver_member(const char* wire_name) {
    char text[512];
    snprintf(
        text,
        sizeof text,
        "{[Rover<S:command=room_info><U:area_id=709><U:room_id=5><U:user_id=2>"
        "<S:username=%s><I:latency=1><I:icon=1><I:rank=1><S:allegiance=ARM>]}",
        wire_name
    );
    deliver(text);
}

void expect_player(const char* wire_name, const char* what) {
    char expected[512];
    snprintf(
        expected,
        sizeof expected,
        "{[GameManager<S:allegiance=ARM><U:area_id=709><S:command=player_info><U:dpid=2a>"
        "<S:game_id=game><S:name=%s><I:team=1>]}",
        wire_name
    );
    check_sent(expected, what);
}

void check_legacy_server() {
    rover_set_wire_utf8(false);
    check(!rover_wire_utf8(), "the server does not take UTF-8 until told");

    check(
        g_rover->enter_room(5, 0x709, nullptr, on_entered, on_member, on_room_update, on_chat),
        "enter the room"
    );
    check_sent(
        "{[ChatManager<U:area_id=709><S:command=enter_room><U:room_id=5>]}", "enter sends the room"
    );
    deliver("{[Rover<S:command=entered_room><U:area_id=709><U:room_id=5><I:result=0>]}");
    check(g_entered == 0, "the room was entered");

    say("Hello", "Hello", "Western chat stays as it is");
    say(kJose, kJoseWire, "a Western European name is Windows-1252");
    say(kSkoda, kSkodaWire, "a Central European name is Windows-1252");
    say(kGreek, "??", "Greek that the code page cannot hold becomes ?");
    say(kCyrillic, "??", "Cyrillic that the code page cannot hold becomes ?");
    say(kTokyo, "??", "CJK that the code page cannot hold becomes ?");

    deliver_chat(kJoseWire);
    check(strcmp(g_chat, kJose) == 0, "a Windows-1252 chat line is read as UTF-8");
    deliver_chat("??");
    check(strcmp(g_chat, "??") == 0, "a replaced character is read back as ?");
    deliver_member(kJoseWire);
    check(strcmp(g_name, kJose) == 0, "a Windows-1252 member name is read as UTF-8");

    check(g_rover->send_player_info(0x709, "game", 0x2a, kJose, "ARM", 1), "a player name is sent");
    expect_player(kJoseWire, "a player name is Windows-1252");
    check(g_rover->send_player_info(0x709, "game", 0x2a, kTokyo, "ARM", 1), "a CJK name is sent");
    expect_player("??", "a CJK player name becomes ?");
}

void check_utf8_server() {
    rover_set_wire_utf8(true);
    check(rover_wire_utf8(), "the server takes UTF-8");

    say(kJose, kJose, "a Western European name stays UTF-8");
    say(kSkoda, kSkoda, "a Central European name stays UTF-8");
    say(kGreek, kGreek, "Greek stays UTF-8");
    say(kCyrillic, kCyrillic, "Cyrillic stays UTF-8");
    say(kTokyo, kTokyo, "CJK stays UTF-8");

    deliver_chat(kTokyo);
    check(strcmp(g_chat, kTokyo) == 0, "a UTF-8 chat line is read as it is");
    deliver_chat(kCyrillic);
    check(strcmp(g_chat, kCyrillic) == 0, "a UTF-8 Cyrillic line is read as it is");
    deliver_member(kTokyo);
    check(strcmp(g_name, kTokyo) == 0, "a UTF-8 member name is read as it is");
    deliver_member(kSkoda);
    check(strcmp(g_name, kSkoda) == 0, "a UTF-8 Central European name is read as it is");

    check(g_rover->send_player_info(0x709, "game", 0x2a, kTokyo, "ARM", 1), "a UTF-8 name is sent");
    expect_player(kTokyo, "a player name stays UTF-8");

    rover_set_wire_utf8(false);
    check(!rover_wire_utf8(), "UTF-8 is not kept once the server does not take it");
}

} // namespace

int main() {
    const auto temporary_value = oa::platform::environment_value("TMPDIR");
    const char* temporary = temporary_value ? temporary_value->c_str() : nullptr;
    char directory[512];
    snprintf(
        directory,
        sizeof directory,
        "%s/rover-language-%d",
        temporary != nullptr && temporary[0] != '\0' ? temporary : ".",
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
    if (g_rover_state != nullptr) {
        check_legacy_server();
        check_utf8_server();
        g_rover->leave_room(5, 0x709, on_left);
        g_rover->detach();
    }
    rover_use_platform(nullptr);

    remove("http.log");
    if (change_directory("..") == 0)
        remove_directory(directory);

    if (failures != 0) {
        fprintf(stderr, "rover language: %d failure(s)\n", failures);
        return 1;
    }
    puts("rover language: ok");
    return 0;
}
