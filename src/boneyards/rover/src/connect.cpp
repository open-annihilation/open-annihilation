// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The connect and login steps.

#include "connect.h"

#include "lobby.h"
#include "link.h"
#include "md5.h"
#include "protocol.h"
#include "requests.h"
#include "servers.h"
#include "state.h"
#include "wire_text.h"

#include <stdio.h>
#include <string.h>

namespace {

// Stages reported to the game's progress callback.
enum class RoverLoginStage : int32_t {
    YardMasterOpening = 2,
    YardMasterConnected = 3,
    Redirected = 4,
    BoneyardOpening = 5,
    BoneyardConnected = 6,
    LoginSent = 7,
    LoginConfirmed = 8,
};
constexpr int32_t kStageResolvingYardMaster = 1;
constexpr int32_t kResultOk = 0;
constexpr int32_t kResultFailed = 1;

// The yard master is the first member of its server's cluster.
constexpr int32_t kYardMasterClusterIndex = 1;
constexpr int32_t kHostNameCapacity = 0x100;
constexpr int32_t kAddressCapacity = 0x80;
constexpr char kAddressFormat[] = "%d.%d.%d.%d";

// Each part of the login digest keeps at most this many characters.
constexpr int32_t kDigestPartLimit = 15;
// A digest byte of 0 is sent as 1, so the digest stays a C string.
constexpr uint8_t kDigestZeroStandIn = 1;

// Disconnect callback's first argument.
constexpr int32_t kLostBoneyard = 0;
constexpr int32_t kLostYardMaster = 1;

// A move's stage once started; it then takes the login stage numbers.
constexpr int32_t kMoveStarted = 1;
// What the game's entered-room callback hears when a move fails.
constexpr int32_t kMoveEnterFailed = 1;

/// Passes a login step to the game's progress callback and records its stage.
///
/// @param stage login stage reached
/// @param result the step's result code
/// @param detail_a first detail value the callback hears
/// @param detail_b second detail value the callback hears
/// @param message text for the step, or null
void report(
    int32_t stage, int32_t result, int32_t detail_a, int32_t detail_b, const char* message
) {
    RoverState& state = *g_rover_state;
    state.progress(stage, result, detail_a, detail_b, rover_show(message));
    state.login_stage = stage;
}

// Stage of a link's connect notice: "open" means the connect started.
int32_t connect_stage(int32_t not_open, RoverLoginStage opening, RoverLoginStage connected) {
    if (not_open == 0)
        return static_cast<int32_t>(opening);
    if (not_open == 1)
        return static_cast<int32_t>(connected);
    return not_open;
}

// Result code of a link's connect notice, as the steps compute it.
int32_t connect_result(int32_t not_open, int32_t failed) {
    if (failed == 0)
        return kResultOk;
    if (failed == 1)
        return kResultFailed;
    return not_open;
}

void rearm(RoverHandlerSet& handlers, RoverHandlerId id) {
    if (rover_handler_is_armed(handlers, id))
        rover_handler_disarm(handlers, id);
    rover_handler_arm(handlers, id);
}

// Keeps the name and domain of the server this login dialled. The list still holds it; the yard
// master's own list may spell the same server's location differently, and the row is recognised
// by this name and domain.
void remember_dialled_server(RoverState& state) {
    state.dialled_name[0] = '\0';
    state.dialled_domain[0] = '\0';
    const RoverServerEntry* dialled = nullptr;
    for (int32_t i = 0; i < state.servers.count; ++i) {
        if (strcmp(state.servers.entries[i].location, state.server_name) == 0)
            dialled = &state.servers.entries[i];
    }
    if (!dialled)
        return;
    rover_copy_bounded(state.dialled_name, dialled->name, sizeof state.dialled_name);
    rover_copy_bounded(state.dialled_domain, dialled->domain, sizeof state.dialled_domain);
}

// True when this yard-master row is the server the login dialled.
bool is_dialled_server(const RoverState& state, const char* name, const char* domain) {
    if (state.dialled_name[0] == '\0' || !name || !domain)
        return false;
    return strcmp(name, state.dialled_name) == 0 && strcmp(domain, state.dialled_domain) == 0;
}

/// Writes a host's IPv4 address as a dotted quad: a numeric host as given, a name through a lookup.
///
/// @param[out] out buffer for the address text
/// @param capacity size of out in bytes
/// @param host host name or dotted quad
/// @return false when the name does not resolve, true otherwise
bool format_host_address(char* out, int32_t capacity, const char* host) {
    const RoverPlatform* platform = rover_platform();
    uint32_t address = 0;
    if (platform->ipv4_parse(host, &address)) {
        rover_copy_bounded(out, host, capacity);
        return true;
    }
    if (!platform->ipv4_lookup(host, &address))
        return false;
    int written = snprintf(
        out,
        static_cast<size_t>(capacity),
        kAddressFormat,
        (address >> 24) & 0xffu,
        (address >> 16) & 0xffu,
        (address >> 8) & 0xffu,
        address & 0xffu
    );
    if (written < 0)
        out[capacity - 1] = '\0';
    return true;
}

/// Reports a yard master link that closed while a login was running to the game's disconnect
/// callback.
///
/// The callback hears which link was lost (1), whether it was still connecting and its last socket
/// error. Nothing is reported while the workers stop or before a login started.
void on_yard_master_lost() {
    RoverState& state = *g_rover_state;
    if (state.stopping || !state.login_started)
        return;
    const RoverConnection& link = rover_link(state, RoverLinkId::YardMaster);
    state.disconnect(kLostYardMaster, link.connecting, link.last_error);
}

/// Reports a boneyard link that closed while a login was running to the game's disconnect callback.
///
/// The callback hears 0 for a boneyard and, for yard 1 or 2, whether that link was still connecting
/// and its last socket error (0 and 0 otherwise). Nothing is reported while the workers stop or
/// before a login started.
///
/// @param yard_number the closed link's yard, 1 or 2
void on_boneyard_lost(int32_t yard_number) {
    RoverState& state = *g_rover_state;
    if (state.stopping || !state.login_started)
        return;
    char connecting = 0;
    int32_t error = 0;
    if (yard_number == 1 || yard_number == 2) {
        const RoverConnection& link =
            rover_link(state, yard_number == 1 ? RoverLinkId::Boneyard1 : RoverLinkId::Boneyard2);
        connecting = link.connecting;
        error = link.last_error;
    }
    state.disconnect(kLostBoneyard, connecting, error);
}

// The yard master closing after it redirected the client needs no action.
void ignore_yard_master_closed() {
}

/// Keeps the host and port of the news (0) or ladder (1) file server for slot 40.
///
/// @param host host name, copied to at most 254 characters
/// @param port port number, kept as 16 bits
/// @param type_index 0 news, 1 ladders; any other index is ignored
void store_file_server(const char* host, int32_t port, int32_t type_index) {
    if (type_index < 0 || type_index >= 2)
        return;
    RoverFileServer& server = g_rover_state->file_servers[type_index];
    server.type_index = type_index;
    server.port = static_cast<uint16_t>(port);
    rover_copy_bounded(server.host, host, 0xff);
}

/// Ends the login when login_confirmed arrives: disarms its handler and reports stage 8.
///
/// @param result login result, 0 on success
/// @param user_id the user's id, reported as detail a
/// @param area_id the user's home area, reported as detail b
void on_login_confirmed(int32_t result, uint32_t user_id, uint32_t area_id) {
    RoverState& state = *g_rover_state;
    rover_handler_disarm(state.handlers, RoverHandlerId::LoginConfirmed);
    state.handlers.login_confirmed = nullptr;
    report(
        static_cast<int32_t>(RoverLoginStage::LoginConfirmed),
        result,
        static_cast<int32_t>(user_id),
        static_cast<int32_t>(area_id),
        nullptr
    );
}

/// Sends the login once connect_info arrives, arms login_confirmed and reports stage 7.
///
/// A registered login carries the password digest MD5(password . lower(username) . seed). The
/// username is the bytes the username field carries on the wire, ASCII letters folded to lower
/// case. Each part is at most 15 bytes, and each zero digest byte is sent as 1 so the digest stays
/// a C string. Without a password the login is anonymous.
///
/// @param role boneyard role from connect_info; not consulted
/// @param compression stream encoding from connect_info; not consulted
/// @param seed login seed from connect_info
void send_login(int32_t /*role*/, int32_t /*compression*/, const char* seed) {
    RoverState& state = *g_rover_state;
    rover_handler_disarm(state.handlers, RoverHandlerId::ConnectInfo);
    state.handlers.connect_info = nullptr;
    state.handlers.login_confirmed = on_login_confirmed;
    rover_handler_arm(state.handlers, RoverHandlerId::LoginConfirmed);

    RoverLoginKind kind = RoverLoginKind::Anonymous;
    char digest[kRoverMd5DigestSize + 1] = {};
    if (state.password[0] != '\0') {
        char text[kDigestPartLimit * 3 + 3] = {};
        rover_copy_bounded(text, state.password, kDigestPartLimit + 1);
        char name[kDigestPartLimit + 1] = {};
        rover_login_digest_username(name, static_cast<int32_t>(sizeof name), state.username);
        snprintf(
            text + strlen(text),
            static_cast<size_t>(kDigestPartLimit) + 1,
            "%.*s",
            static_cast<int>(kDigestPartLimit),
            name
        );
        text[kDigestPartLimit * 2] = '\0';
        snprintf(
            text + strlen(text),
            static_cast<size_t>(kDigestPartLimit) + 1,
            "%.*s",
            static_cast<int>(kDigestPartLimit),
            seed
        );
        text[kDigestPartLimit * 3] = '\0';
        RoverMd5 md5;
        rover_md5_init(md5);
        rover_md5_update(md5, text, static_cast<uint32_t>(strlen(text)));
        uint8_t hash[kRoverMd5DigestSize];
        rover_md5_final(md5, hash);
        for (int32_t i = 0; i < kRoverMd5DigestSize; ++i)
            digest[i] = static_cast<char>(hash[i] ? hash[i] : kDigestZeroStandIn);
        kind = RoverLoginKind::Registered;
    }
    RoverRecord* login = rover_request_login(kind, state.username, digest, nullptr);
    if (login)
        rover_queue_push(state.outbound[static_cast<int32_t>(rover_active_boneyard(state))], login);
    report(static_cast<int32_t>(RoverLoginStage::LoginSent), kResultOk, 0, 0, nullptr);
}

/// Advances the login on the boneyard link's connect notice and reports stage 5 or 6.
///
/// Once connected, asks for a client link with connect_type and arms connect_info to send the
/// login; a failed connect disarms the connect handler.
///
/// @param not_open 0 when the connect started ("open"), 1 when it finished
/// @param failed 1 when the link's result was kRoverLinkResultFailed, 0 otherwise
void on_boneyard_connect(int32_t not_open, int32_t failed) {
    RoverState& state = *g_rover_state;
    int32_t stage = connect_stage(
        not_open, RoverLoginStage::BoneyardOpening, RoverLoginStage::BoneyardConnected
    );
    int32_t result = connect_result(not_open, failed);
    if (result == kResultOk) {
        if (stage == static_cast<int32_t>(RoverLoginStage::BoneyardConnected)) {
            rover_handler_disarm(state.handlers, RoverHandlerId::ConnectToBoneyard);
            state.handlers.boneyard_connect = nullptr;
            state.handlers.connect_info = send_login;
            rover_handler_arm(state.handlers, RoverHandlerId::ConnectInfo);
            RoverRecord* request = rover_request_connect_type(
                RoverConnectRole::Client, RoverCompression::Text, nullptr
            );
            if (request)
                rover_queue_push(
                    state.outbound[static_cast<int32_t>(rover_active_boneyard(state))], request
                );
        }
    } else {
        rover_handler_disarm(state.handlers, RoverHandlerId::ConnectToBoneyard);
        state.handlers.boneyard_connect = nullptr;
    }
    report(stage, result, 0, 0, nullptr);
}

/// Acts on connect_to: closes the yard master, dials the boneyard it names and reports stage 4 with
/// the message.
///
/// A successful redirect arms the boneyard's close, system_alert, file_server, ad_server and
/// connect handlers.
/// The address must be numeric; no lookup is made. An address of more than 15 characters is cut to
/// 15, the most the link's address field holds.
///
/// @param result connect_to result; 0 dials the boneyard, anything else reports stage 4 as failed
/// @param address boneyard address as a dotted quad
/// @param port boneyard port
/// @param message text passed to the game with the stage
void on_redirect(int32_t result, const char* address, uint16_t port, const char* message) {
    RoverState& state = *g_rover_state;
    const RoverPlatform* platform = rover_platform();
    RoverHandlerSet& handlers = state.handlers;
    handlers.yardmaster_closed = ignore_yard_master_closed;
    rearm(handlers, RoverHandlerId::YardMasterClosed);
    rover_link_stop(rover_link(state, RoverLinkId::YardMaster));
    int32_t code = kResultFailed;
    if (result == 0) {
        RoverLinkId boneyard = rover_active_boneyard(state);
        int32_t yard_index = static_cast<int32_t>(boneyard);
        handlers.yard_closed[yard_index] = on_boneyard_lost;
        rearm(
            handlers, yard_index == 0 ? RoverHandlerId::Yard1Closed : RoverHandlerId::Yard2Closed
        );
        handlers.system_alert = state.system_alert;
        rearm(handlers, RoverHandlerId::SystemAlert);
        handlers.file_server = store_file_server;
        rearm(handlers, RoverHandlerId::FileServer);
        rearm(handlers, RoverHandlerId::AdServer);
        handlers.boneyard_connect = on_boneyard_connect;
        rearm(handlers, RoverHandlerId::ConnectToBoneyard);
        RoverConnection& link = rover_link(state, boneyard);
        rover_copy_bounded(link.address, address, sizeof link.address);
        link.port = port;
        platform->event_set(state.connect_event[yard_index]);
        code = kResultOk;
    }
    report(static_cast<int32_t>(RoverLoginStage::Redirected), code, 0, 0, message);
}

/// Saves one server of the yard master's list; on the last, writes the list to serv_sav.byd and
/// asks the yard master for a boneyard.
///
/// @param cluster_name server's cluster name
/// @param domain server's domain
/// @param cluster_size number of machines in the server's cluster
/// @param location server's location text; the row with the dialled server's name and domain is
///     marked selected, and the resolved server takes that location
/// @param last true for the final row of the list
void on_server_entry(
    const char* cluster_name,
    const char* domain,
    int32_t cluster_size,
    const char* location,
    bool last
) {
    RoverState& state = *g_rover_state;
    int32_t before = state.servers.count;
    bool selected = is_dialled_server(state, cluster_name, domain);
    rover_servers_add(state.servers, cluster_name, domain, cluster_size, location, false, selected);
    if (selected && state.servers.count == before + 1) {
        rover_copy_bounded(
            state.resolved_server,
            state.servers.entries[before].location,
            sizeof state.resolved_server
        );
    }
    if (!last)
        return;
    rover_servers_write(state.servers, kRoverSavedServerFile);
    rover_request_boneyard();
}

/// Advances the login on the yard master link's connect notice and reports stage 2 or 3.
///
/// Once connected, remembers the dialled server's name and domain and its location, drops the
/// unkept saved servers, arms server_list and asks for the yard master's list; a failed connect
/// disarms the connect handler. The list's row for that name and domain replaces the location.
///
/// @param not_open 0 when the connect started ("open"), 1 when it finished
/// @param failed 1 when the link's result was kRoverLinkResultFailed, 0 otherwise
void on_yard_master_connect(int32_t not_open, int32_t failed) {
    RoverState& state = *g_rover_state;
    int32_t stage = connect_stage(
        not_open, RoverLoginStage::YardMasterOpening, RoverLoginStage::YardMasterConnected
    );
    int32_t result = connect_result(not_open, failed);
    if (result == kResultOk) {
        if (stage == static_cast<int32_t>(RoverLoginStage::YardMasterConnected)) {
            rover_copy_bounded(
                state.resolved_server, state.server_name, sizeof state.resolved_server
            );
            remember_dialled_server(state);
            rover_handler_disarm(state.handlers, RoverHandlerId::ConnectToYardMaster);
            state.handlers.yardmaster_connect = nullptr;
            rover_servers_remove_unkept(state.servers);
            state.handlers.server_list = on_server_entry;
            rearm(state.handlers, RoverHandlerId::ServerList);
            RoverRecord* request = rover_request_get_servers(nullptr);
            if (request)
                rover_queue_push(
                    state.outbound[static_cast<int32_t>(RoverLinkId::YardMaster)], request
                );
        }
    } else {
        rover_handler_disarm(state.handlers, RoverHandlerId::ConnectToYardMaster);
        state.handlers.yardmaster_connect = nullptr;
    }
    report(stage, result, 0, 0, nullptr);
}

// Index of the boneyard link the session is not using.
int32_t other_boneyard(const RoverState& state) {
    return state.second_boneyard ? 0 : 1;
}

RoverHandlerId yard_closed_handler(int32_t yard_index) {
    return yard_index == 0 ? RoverHandlerId::Yard1Closed : RoverHandlerId::Yard2Closed;
}

void queue_on_other_boneyard(RoverState& state, RoverRecord* record) {
    if (record)
        rover_queue_push(state.outbound[other_boneyard(state)], record);
}

void end_failed_move(RoverState& state) {
    state.move.active = false;
    if (state.move.entered)
        state.move.entered(kMoveEnterFailed);
}

/// Makes the other boneyard the session's and enters the room there again, the game's entered-room
/// callback taking the result.
void enter_room_on_other_boneyard() {
    RoverState& state = *g_rover_state;
    RoverBoneyardMove& move = state.move;
    move.active = false;
    move.stage = static_cast<int32_t>(RoverLoginStage::LoginConfirmed);
    state.second_boneyard = !state.second_boneyard;
    RoverEnteredRoomHandler& enter = state.handlers.lobby.entered_room;
    enter.room_id = move.room_id;
    enter.area_id = move.area_id;
    enter.callback = move.entered;
    rover_copy_bounded(enter.password, move.password, sizeof enter.password);
    enter.switching = false;
    rover_handler_arm(state.handlers, RoverHandlerId::EnteredRoom);
    rover_send_to_boneyard(rover_request_enter_room(
        static_cast<uint32_t>(move.area_id), static_cast<uint32_t>(move.room_id), move.password
    ));
}

/// Handles the old boneyard's link closing during a move.
///
/// The session switches when relogin_confirmed has come already; otherwise the close is noted. The
/// handler stays armed after the switch, and a later close of that link tells the game its enter
/// failed.
///
/// @param yard_number the closed link's yard, 1 or 2
void on_move_old_link_closed(int32_t yard_number) {
    RoverState& state = *g_rover_state;
    if (!state.move.half_done) {
        state.move.half_done = true;
        return;
    }
    if (yard_number == static_cast<int32_t>(rover_active_boneyard(state)) + 1)
        enter_room_on_other_boneyard();
    else
        end_failed_move(state);
}

/// Handles relogin_confirmed during a move.
///
/// A success closes the old boneyard's link, or switches when it has closed already; a refusal
/// fails the move and the old boneyard's close is again reported as a lost link.
///
/// @param result relogin result, 0 on success
void on_move_relogin_confirmed(int32_t result) {
    RoverState& state = *g_rover_state;
    RoverHandlerSet& handlers = state.handlers;
    rover_handler_disarm(handlers, RoverHandlerId::ReloginConfirmed);
    handlers.relogin_confirmed = nullptr;
    state.move.stage = static_cast<int32_t>(RoverLoginStage::LoginConfirmed);
    if (result == kResultOk) {
        if (!state.move.half_done) {
            state.move.half_done = true;
            rover_link_stop(rover_link(state, rover_active_boneyard(state)));
        } else {
            enter_room_on_other_boneyard();
        }
        return;
    }
    int32_t other = other_boneyard(state);
    int32_t current = 1 - other;
    rover_handler_disarm(handlers, yard_closed_handler(other));
    handlers.yard_closed[other] = nullptr;
    handlers.yard_closed[current] = on_boneyard_lost;
    rearm(handlers, yard_closed_handler(current));
    end_failed_move(state);
}

/// Sends relogin with the user id and the magic to the other boneyard once its connect_info
/// arrives, then waits for the confirmation and the old link's close.
///
/// @param role boneyard role from connect_info; not consulted
/// @param compression stream encoding from connect_info; not consulted
/// @param seed login seed from connect_info; not used by relogin
void on_move_connect_info(int32_t /*role*/, int32_t /*compression*/, const char* /*seed*/) {
    RoverState& state = *g_rover_state;
    RoverHandlerSet& handlers = state.handlers;
    rover_handler_disarm(handlers, RoverHandlerId::ConnectInfo);
    handlers.connect_info = nullptr;
    handlers.relogin_confirmed = on_move_relogin_confirmed;
    rover_handler_arm(handlers, RoverHandlerId::ReloginConfirmed);
    int32_t current = 1 - other_boneyard(state);
    handlers.yard_closed[current] = on_move_old_link_closed;
    rearm(handlers, yard_closed_handler(current));
    queue_on_other_boneyard(state, rover_request_relogin(state.user_id, state.move.magic, nullptr));
    state.move.half_done = false;
    state.move.stage = static_cast<int32_t>(RoverLoginStage::LoginSent);
}

/// Advances a move on the other boneyard link's connect notice.
///
/// Once connected, asks it for a client link with connect_type and arms connect_info; a failed
/// connect fails the move.
///
/// @param not_open 0 when the connect started ("open"), 1 when it finished
/// @param failed 1 when the link's result was kRoverLinkResultFailed, 0 otherwise
void on_move_connect(int32_t not_open, int32_t failed) {
    RoverState& state = *g_rover_state;
    RoverHandlerSet& handlers = state.handlers;
    int32_t stage = connect_stage(
        not_open, RoverLoginStage::BoneyardOpening, RoverLoginStage::BoneyardConnected
    );
    if (connect_result(not_open, failed) == kResultOk) {
        if (stage == static_cast<int32_t>(RoverLoginStage::BoneyardConnected)) {
            rover_handler_disarm(handlers, RoverHandlerId::ConnectToBoneyard);
            handlers.boneyard_connect = nullptr;
            handlers.connect_info = on_move_connect_info;
            rover_handler_arm(handlers, RoverHandlerId::ConnectInfo);
            RoverRecord* request = rover_request_connect_type(
                RoverConnectRole::Client, RoverCompression::Text, nullptr
            );
            queue_on_other_boneyard(state, request);
        }
    } else {
        rover_handler_disarm(handlers, RoverHandlerId::ConnectToBoneyard);
        handlers.boneyard_connect = nullptr;
        int32_t other = other_boneyard(state);
        rover_handler_disarm(handlers, yard_closed_handler(other));
        handlers.yard_closed[other] = nullptr;
        end_failed_move(state);
    }
    state.move.stage = stage;
}

} // namespace

bool rover_move_to_other_boneyard(const RoverEnteredRoomHandler& handler, const RoverEntry& entry) {
    RoverState& state = *g_rover_state;
    RoverBoneyardMove& move = state.move;
    if (move.active)
        return false;
    const char* address = nullptr;
    int32_t port = 0;
    uint32_t magic = 0;
    rover_entry_get_string(entry, kRoverKeyAddress, &address, nullptr);
    rover_entry_get_int(entry, kRoverKeyPort, &port, 0);
    rover_entry_get_uint(entry, kRoverKeyMagic, &magic, 0);
    if (!address || port == 0 || magic == 0)
        return false;
    move.active = true;
    move.room_id = handler.room_id;
    move.area_id = handler.area_id;
    rover_copy_bounded(move.password, handler.password, sizeof move.password);
    rover_copy_bounded(move.address, address, sizeof move.address);
    move.port = port;
    move.magic = magic;
    move.entered = handler.callback;
    move.stage = kMoveStarted;

    int32_t other = other_boneyard(state);
    RoverConnection& link = state.links[other];
    rover_copy_bounded(link.address, address, sizeof link.address);
    link.port = static_cast<uint16_t>(port);
    rover_platform()->event_set(state.connect_event[other]);
    RoverHandlerSet& handlers = state.handlers;
    handlers.yard_closed[other] = on_boneyard_lost;
    rearm(handlers, yard_closed_handler(other));
    handlers.boneyard_connect = on_move_connect;
    rover_handler_arm(handlers, RoverHandlerId::ConnectToBoneyard);
    return true;
}

void rover_connect_yard_master() {
    RoverState& state = *g_rover_state;
    const RoverPlatform* platform = rover_platform();
    state.resolved_server[0] = '\0';
    state.dialled_name[0] = '\0';
    state.dialled_domain[0] = '\0';
    state.handlers.yardmaster_closed = on_yard_master_lost;
    rearm(state.handlers, RoverHandlerId::YardMasterClosed);
    state.handlers.yardmaster_connect = on_yard_master_connect;
    rover_handler_arm(state.handlers, RoverHandlerId::ConnectToYardMaster);
    char host[kHostNameCapacity] = {};
    rover_servers_host_name(
        state.servers, host, sizeof host, state.server_name, kYardMasterClusterIndex
    );
    char address[kAddressCapacity] = {};
    if (host[0] == '\0' || !format_host_address(address, sizeof address, host))
        address[0] = '\0';
    RoverConnection& link = rover_link(state, RoverLinkId::YardMaster);
    rover_copy_bounded(link.address, address, sizeof link.address);
    link.port = kRoverYardMasterPort;
    // A name that does not resolve never opens a socket. The close notice is what the login
    // console turns into the message box, and the progress message is the host it names.
    if (host[0] != '\0' && address[0] == '\0') {
        rover_link_fail_pending(state, link, platform->net_last_error());
        report(kStageResolvingYardMaster, kResultFailed, 0, 0, host);
        return;
    }
    platform->event_set(state.connect_event[static_cast<int32_t>(RoverLinkId::YardMaster)]);
    report(kStageResolvingYardMaster, kResultOk, 0, 0, host[0] != '\0' ? host : nullptr);
}

void rover_request_boneyard() {
    RoverState& state = *g_rover_state;
    state.handlers.connect_to = on_redirect;
    rearm(state.handlers, RoverHandlerId::ConnectTo);
    RoverRecord* request = rover_request_yard_master_connect();
    if (request)
        rover_queue_push(state.outbound[static_cast<int32_t>(RoverLinkId::YardMaster)], request);
}
