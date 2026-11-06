// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Slot 40: the two download-server records the "file_server" handler keeps in
// the session block, index 0 for news and 1 for ladders.

#include "slots.h"
#include "state.h"

namespace {

constexpr int32_t kFileServerCount = 2;
// Host names are copied with this bound, one short of the field.
constexpr int32_t kHostCopyCapacity = 0xff;

bool is_server_index(int32_t index) {
    return index > -1 && index < kFileServerCount;
}

} // namespace

void ROVER_CALL rover_get_file_server(int32_t server_type, RoverFileServer* out_server) {
    if (!is_server_index(server_type) || !g_rover_state) {
        out_server->type_index = OA_ROVER_FILE_SERVER_INVALID;
        return;
    }
    const RoverFileServer& server = g_rover_state->file_servers[server_type];
    out_server->type_index = server.type_index;
    rover_copy_bounded(out_server->host, server.host, kHostCopyCapacity);
    out_server->port = server.port;
}
