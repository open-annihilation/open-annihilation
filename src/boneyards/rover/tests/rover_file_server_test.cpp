// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Slot 40 over the file-server records of the session block.

#include "../src/state.h"
#include "oa/rover/interface.h"

#include <stdio.h>
#include <string.h>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void announce(RoverState& state, int32_t index, const char* host, uint16_t port) {
    RoverFileServer& server = state.file_servers[index];
    server.type_index = index;
    server.port = port;
    snprintf(server.host, sizeof server.host, "%s", host);
}

RoverState g_state{};

} // namespace

int main() {
    const RoverInterface* rover = nullptr;
    rover_get_interface(&rover);

    RoverFileServer server{};
    memset(&server, 0x5a, sizeof server);
    rover->get_file_server(0, &server);
    check(
        server.type_index == OA_ROVER_FILE_SERVER_INVALID && server.host[0] == 0x5a &&
            server.port == 0x5a5a,
        "a detached session only sets the invalid index"
    );

    g_rover_state = &g_state;
    for (RoverFileServer& record : g_state.file_servers)
        record.type_index = OA_ROVER_FILE_SERVER_INVALID;
    memset(&server, 0x5a, sizeof server);
    rover->get_file_server(1, &server);
    check(
        server.type_index == OA_ROVER_FILE_SERVER_INVALID && server.host[0] == 0 &&
            server.port == 0,
        "an unannounced server copies the session's invalid record"
    );

    announce(g_state, 0, "news.boneyards.net", 80);
    announce(g_state, 1, "ladder.boneyards.net", 8080);
    memset(&server, 0x5a, sizeof server);
    rover->get_file_server(1, &server);
    check(
        server.type_index == 1 && strcmp(server.host, "ladder.boneyards.net") == 0 &&
            server.port == 8080,
        "the ladder server"
    );
    check(server.host[21] == 0x5a, "the host copy stops at its terminator");
    rover->get_file_server(0, &server);
    check(
        server.type_index == 0 && strcmp(server.host, "news.boneyards.net") == 0 &&
            server.port == 80,
        "the news server"
    );

    const int32_t unknown_types[] = {-1, 2, 0x7fffffff};
    for (int32_t type : unknown_types) {
        memset(&server, 0x5a, sizeof server);
        rover->get_file_server(type, &server);
        check(
            server.type_index == OA_ROVER_FILE_SERVER_INVALID && server.host[0] == 0x5a &&
                server.port == 0x5a5a,
            "an unknown type only sets the invalid index"
        );
    }

    memset(g_state.file_servers[0].host, 'h', sizeof g_state.file_servers[0].host - 1);
    g_state.file_servers[0].host[sizeof g_state.file_servers[0].host - 1] = 0;
    g_state.file_servers[0].port = 1;
    rover->get_file_server(0, &server);
    check(strlen(server.host) == 254 && server.port == 1, "hosts copy at most 254 characters");

    g_rover_state = nullptr;
    if (failures)
        return 1;
    puts("rover file server: ok");
    return 0;
}
