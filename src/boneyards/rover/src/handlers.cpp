// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The message handlers and the dispatcher that offers them each entry.

#include "handlers.h"

#include "ad_server.h"
#include "lobby.h"
#include "servers.h"
#include "protocol.h"
#include "requests.h"
#include "state.h"
#include "wire_text.h"

#include <string.h>

namespace {

constexpr char kCommandConnectTo[] = "connect_to";
constexpr char kCommandConnectInfo[] = "connect_info";
constexpr char kCommandLoginConfirmed[] = "login_confirmed";
constexpr char kCommandReloginConfirmed[] = "relogin_confirmed";
constexpr char kCommandLogoutConfirmed[] = "logout_confirmed";
constexpr char kCommandServerList[] = "server_list";
constexpr char kCommandFileServer[] = "file_server";
constexpr char kCommandAdServer[] = "ad_server";
constexpr char kCommandSystemAlert[] = "system_alert";
constexpr char kCommandLatency[] = "latency";

constexpr char kKeyAddress[] = "ip";
constexpr char kKeySeed[] = "seed";
constexpr char kKeyCompression[] = "compression";
constexpr char kKeyClusterName[] = "cluster_name";
constexpr char kKeyDomain[] = "domain";
constexpr char kKeyClusterSize[] = "cluster_size";
constexpr char kKeyRegion[] = "region";
constexpr char kKeyLast[] = "last";
constexpr char kKeySeconds[] = "sec";
constexpr char kKeyMicroseconds[] = "usec";
constexpr char kKeyUrl[] = "url";
constexpr char kKeySendId[] = "send_id";
constexpr char kKeyParams[] = "params";

constexpr char kRoleClient[] = "client";
constexpr char kRoleServer[] = "server";
constexpr char kCompressionText[] = "text";
constexpr char kCompressionCompressed[] = "compressed";
constexpr char kCompressionEncrypted[] = "encrypted";
constexpr char kFileServerNews[] = "news";
constexpr char kFileServerLadder[] = "ladder";

// Roles, encodings and file servers as the continuations receive them.
constexpr int32_t kRoleIsClient = 0;
constexpr int32_t kRoleIsServer = 1;
constexpr int32_t kCompressionIsText = 0;
constexpr int32_t kCompressionIsCompressed = 1;
constexpr int32_t kCompressionIsEncrypted = 2;
constexpr int32_t kFileServerIsNews = 0;
constexpr int32_t kFileServerIsLadder = 1;
// Port of a file_server message that names none.
constexpr int32_t kDefaultFileServerPort = 80;

// The empty string 3.1c hands out for absent text.
constexpr char kNoText[] = "";

bool command_is(const RoverField& command, const char* name) {
    return rover_text_equals(command.value, command.value_length, name);
}

bool field_is(const RoverField& field, const char* text) {
    return rover_text_equals(field.value, field.value_length, text);
}

/// Takes the yard master link's connect notice and reports whether its state is other than "open"
/// and whether its result was kRoverLinkResultFailed.
///
/// A notice without state or result is taken but not reported.
///
/// @param handlers handler set holding the yard master continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is the yard master connect notice, false otherwise
bool accept_yard_master_connect(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!command_is(command, kRoverCommandConnectToYardMaster))
        return false;
    const RoverField* state = rover_entry_find(entry, kRoverKeyState);
    const RoverField* result = rover_entry_find(entry, kRoverKeyResult);
    if (state && result && handlers.yardmaster_connect) {
        int32_t code = rover_field_int(*result, -1);
        bool open = field_is(*state, kRoverLinkStateOpen);
        handlers.yardmaster_connect(!open, code == kRoverLinkResultFailed);
    }
    return true;
}

/// Takes a boneyard link's connect notice and reports it like the yard master's, but only for yard
/// 1 or 2.
///
/// A notice without state, result or yard_number, or for another yard, is taken but not reported.
///
/// @param handlers handler set holding the boneyard continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is a boneyard connect notice, false otherwise
bool accept_boneyard_connect(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!command_is(command, kRoverCommandConnectToBoneyard))
        return false;
    const RoverField* state = rover_entry_find(entry, kRoverKeyState);
    const RoverField* result = rover_entry_find(entry, kRoverKeyResult);
    const RoverField* yard = rover_entry_find(entry, kRoverKeyYardNumber);
    if (state && result && yard && handlers.boneyard_connect) {
        int32_t code = rover_field_int(*result, -1);
        bool open = field_is(*state, kRoverLinkStateOpen);
        int32_t yard_number = rover_field_int(*yard, -1);
        if (yard_number > 0 && yard_number < 3)
            handlers.boneyard_connect(!open, code == kRoverLinkResultFailed);
    }
    return true;
}

/// Takes the yard master link's close notice and reports it.
///
/// @param handlers handler set holding the yard master close continuation
/// @param command the entry's command field
/// @return true when the message is the yard master close notice, false otherwise
bool accept_yard_master_closed(RoverHandlerSet& handlers, const RoverField& command) {
    if (!command_is(command, kRoverCommandYardMasterClosed))
        return false;
    if (handlers.yardmaster_closed)
        handlers.yardmaster_closed();
    return true;
}

/// Takes a boneyard link's close notice for one yard and reports it with the yard number.
///
/// The handler of each yard takes only its own yard number; a notice without one is taken by the
/// first armed and ignored.
///
/// @param handlers handler set holding the per-yard close continuations
/// @param yard_index 0 for yard 1, 1 for yard 2
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the notice is for this yard or names none, false otherwise
bool accept_yard_closed(
    RoverHandlerSet& handlers,
    int32_t yard_index,
    const RoverField& command,
    const RoverEntry& entry
) {
    if (!command_is(command, kRoverCommandBoneyardClosed))
        return false;
    const RoverField* yard = rover_entry_find(entry, kRoverKeyYardNumber);
    if (!yard)
        return true;
    int32_t yard_number = yard_index + 1;
    if (rover_field_int(*yard, -1) != yard_number)
        return false;
    if (handlers.yard_closed[yard_index])
        handlers.yard_closed[yard_index](yard_number);
    return true;
}

/// Takes connect_to, in which the yard master names the boneyard to use, and hands on its result,
/// address, port and message.
///
/// An entry without ip or port is taken and ignored, and an empty address is not reported; a
/// missing result reads 0 and a missing message null.
///
/// @param handlers handler set holding the redirect continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is connect_to, false otherwise
bool accept_connect_to(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!command_is(command, kCommandConnectTo))
        return false;
    const RoverField* address = rover_entry_find(entry, kKeyAddress);
    const RoverField* port = rover_entry_find(entry, kRoverKeyPort);
    if (!address || !port)
        return true;
    int32_t result = 0;
    const char* message = nullptr;
    rover_entry_get_int(entry, kRoverKeyResult, &result, 0);
    rover_entry_get_string(entry, kRoverKeyMessage, &message, nullptr);
    int32_t port_number = rover_field_int(*port, -1);
    if (address->value_length != 0 && handlers.connect_to)
        handlers.connect_to(result, address->value, static_cast<uint16_t>(port_number), message);
    return true;
}

/// Takes connect_info: the boneyard's role, stream encoding and login seed.
///
/// A missing or empty seed, or a role or encoding the client does not know, leaves the message
/// unanswered. A missing type means client; the encodings are text, compressed and encrypted, the
/// last when none is named.
///
/// @param handlers handler set holding the connect-info continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is connect_info, false otherwise
bool accept_connect_info(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!command_is(command, kCommandConnectInfo))
        return false;
    const RoverField* seed = rover_entry_find(entry, kKeySeed);
    if (!seed || seed->value_length == 0)
        return true;
    int32_t role = kRoleIsClient;
    const char* type = nullptr;
    rover_entry_get_string(entry, kRoverKeyType, &type, nullptr);
    if (type && strcmp(type, kRoleClient) != 0) {
        if (strcmp(type, kRoleServer) != 0)
            return true;
        role = kRoleIsServer;
    }
    int32_t compression = kCompressionIsEncrypted;
    const char* encoding = nullptr;
    rover_entry_get_string(entry, kKeyCompression, &encoding, nullptr);
    if (encoding) {
        if (strcmp(encoding, kCompressionText) == 0)
            compression = kCompressionIsText;
        else if (strcmp(encoding, kCompressionCompressed) == 0)
            compression = kCompressionIsCompressed;
        else if (strcmp(encoding, kCompressionEncrypted) != 0)
            return true;
    }
    if (handlers.connect_info)
        handlers.connect_info(role, compression, seed->value);
    return true;
}

/// Takes login_confirmed, keeps the user id in the session block for a later relogin and reports it
/// with the home area and the result.
///
/// An entry without user_id or area_id is taken and ignored; a missing result reads 0.
///
/// @param handlers handler set holding the login continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is login_confirmed, false otherwise
bool accept_login_confirmed(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!command_is(command, kCommandLoginConfirmed))
        return false;
    const RoverField* user = rover_entry_find(entry, kRoverKeyUserId);
    const RoverField* area = rover_entry_find(entry, kRoverKeyAreaId);
    if (!user || !area)
        return true;
    int32_t result = 0;
    rover_entry_get_int(entry, kRoverKeyResult, &result, 0);
    uint32_t user_id = rover_field_uint(*user, 0);
    g_rover_state->user_id = user_id;
    uint32_t area_id = rover_field_uint(*area, 0);
    if (handlers.login_confirmed)
        handlers.login_confirmed(result, user_id, area_id);
    return true;
}

/// Takes relogin_confirmed and hands on the result, 0 when the reply gives none.
///
/// @param handlers handler set holding the relogin continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is relogin_confirmed, false otherwise
bool accept_relogin_confirmed(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!command_is(command, kCommandReloginConfirmed))
        return false;
    int32_t result = 0;
    rover_entry_get_int(entry, kRoverKeyResult, &result, 0);
    if (handlers.relogin_confirmed)
        handlers.relogin_confirmed(result);
    return true;
}

/// Takes logout_confirmed and reports it.
///
/// @param handlers handler set holding the game's logout callback
/// @param command the entry's command field
/// @return true when the message is logout_confirmed, false otherwise
bool accept_logout_confirmed(RoverHandlerSet& handlers, const RoverField& command) {
    if (!command_is(command, kCommandLogoutConfirmed))
        return false;
    if (handlers.logout_confirmed)
        handlers.logout_confirmed();
    return true;
}

/// Takes one server_list row of the yard master's list and hands it on; "last" ends the list.
///
/// Missing text fields read as empty and missing numbers as 0.
///
/// @param handlers handler set holding the server list continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is server_list, false otherwise
bool accept_server_list(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!command_is(command, kCommandServerList))
        return false;
    const char* cluster_name = nullptr;
    const char* domain = nullptr;
    const char* location = nullptr;
    int32_t cluster_size = 0;
    int32_t last = 0;
    rover_entry_get_string(entry, kKeyClusterName, &cluster_name, kNoText);
    rover_entry_get_string(entry, kKeyDomain, &domain, kNoText);
    rover_entry_get_int(entry, kKeyClusterSize, &cluster_size, 0);
    rover_entry_get_string(entry, kRoverKeyName, &location, kNoText);
    rover_entry_get_int(entry, kKeyLast, &last, 0);
    const char* region = nullptr;
    const bool region_known = rover_entry_get_string(entry, kKeyRegion, &region, nullptr);
    rover_note_server_region(region_known ? region : nullptr);
    if (handlers.server_list)
        handlers.server_list(cluster_name, domain, cluster_size, location, last != 0);
    rover_clear_server_region_note();
    return true;
}

/// Takes ad_server and keeps its url, send_id and params for the banner strip.
///
/// url and send_id are required. A record missing either is taken and ignored, so a later good
/// record is kept. send_id other than 1 clears a host already stored: the strip then fetches
/// nothing. params may be absent.
///
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is ad_server, false otherwise
bool accept_ad_server(const RoverField& command, const RoverEntry& entry) {
    if (!command_is(command, kCommandAdServer))
        return false;
    const char* url = nullptr;
    if (!rover_entry_get_string(entry, kKeyUrl, &url, nullptr) || url == nullptr || url[0] == '\0')
        return true;
    int32_t send_id = 0;
    if (!rover_entry_get_int(entry, kKeySendId, &send_id, 0))
        return true;
    const char* params = nullptr;
    rover_entry_get_string(entry, kKeyParams, &params, nullptr);
    if (g_rover_state == nullptr)
        return true;
    RoverAdServer& ad = g_rover_state->ad_server;
    ad = {};
    ad.send_id = send_id;
    if (send_id == 1) {
        rover_copy_bounded(ad.url, url, static_cast<int32_t>(sizeof ad.url));
        if (params != nullptr)
            rover_copy_bounded(ad.params, params, static_cast<int32_t>(sizeof ad.params));
        ad.active = ad.url[0] != '\0';
    }
    return true;
}

/// Takes file_server and hands on the host and port of the news or ladder download server.
///
/// A missing port reads 80. An entry without name or type, with an empty name, or with a type other
/// than news or ladder is taken and ignored.
///
/// @param handlers handler set holding the file server continuation
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is file_server, false otherwise
bool accept_file_server(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!command_is(command, kCommandFileServer))
        return false;
    const RoverField* host = rover_entry_find(entry, kRoverKeyName);
    const RoverField* type = rover_entry_find(entry, kRoverKeyType);
    if (!host || !type || host->value_length == 0)
        return true;
    int32_t type_index = kFileServerIsNews;
    if (!field_is(*type, kFileServerNews)) {
        if (!field_is(*type, kFileServerLadder))
            return true;
        type_index = kFileServerIsLadder;
    }
    int32_t port = 0;
    rover_entry_get_int(entry, kRoverKeyPort, &port, kDefaultFileServerPort);
    if (handlers.file_server)
        handlers.file_server(rover_show(host->value), port, type_index);
    return true;
}

/// Takes system_alert and hands the game the alert's type and message, and its other fields,
/// command included, in a property bag that lives for the call.
///
/// The game reads the bag through slot 47 (type 2, user_id) and slot 48 (type 3,
/// notification_type). A missing or empty message is taken and ignored; a missing type reads 0.
///
/// @param handlers handler set holding the game's alert callback
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is system_alert, false otherwise
bool accept_system_alert(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
) {
    if (!command_is(command, kCommandSystemAlert))
        return false;
    const RoverField* message = rover_entry_find(entry, kRoverKeyMessage);
    if (!message || message->value_length == 0)
        return true;
    int32_t type = 0;
    rover_entry_get_int(entry, kRoverKeyType, &type, 0);
    static const char* const kPassedAlone[] = {kRoverKeyMessage, kRoverKeyType};
    constexpr int32_t kPassedAloneCount =
        static_cast<int32_t>(sizeof kPassedAlone / sizeof kPassedAlone[0]);
    RoverPropertyBag* fields =
        rover_collect_field_bag(entry, kPassedAlone, kPassedAloneCount, RoverBagCommand::Kept);
    if (handlers.system_alert)
        handlers.system_alert(type, rover_show(message->value), fields);
    rover_release_field_bag(fields);
    return true;
}

/// Takes latency and answers the server's probe at once with latency_r on the active boneyard link.
///
/// The reply echoes sec and usec and carries the flags the game set through slot 43, which are then
/// cleared.
///
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is latency, false otherwise
bool accept_latency(const RoverField& command, const RoverEntry& entry) {
    if (!command_is(command, kCommandLatency))
        return false;
    uint32_t seconds = 0;
    uint32_t microseconds = 0;
    rover_entry_get_uint(entry, kKeySeconds, &seconds, 0);
    rover_entry_get_uint(entry, kKeyMicroseconds, &microseconds, 0);
    RoverState& state = *g_rover_state;
    RoverRecord* reply =
        rover_request_latency_reply(seconds, microseconds, state.latency_reply_flags, nullptr);
    if (reply)
        rover_queue_push(state.outbound[static_cast<int32_t>(rover_active_boneyard(state))], reply);
    state.latency_reply_flags = 0;
    return true;
}

// Offers the entry to one handler; true when it takes the message.
bool offer(
    RoverHandlerSet& handlers, RoverHandlerId id, const RoverField& command, const RoverEntry& entry
) {
    switch (id) {
    case RoverHandlerId::ConnectToYardMaster:
        return accept_yard_master_connect(handlers, command, entry);
    case RoverHandlerId::ConnectToBoneyard:
        return accept_boneyard_connect(handlers, command, entry);
    case RoverHandlerId::YardMasterClosed:
        return accept_yard_master_closed(handlers, command);
    case RoverHandlerId::Yard1Closed:
        return accept_yard_closed(handlers, 0, command, entry);
    case RoverHandlerId::Yard2Closed:
        return accept_yard_closed(handlers, 1, command, entry);
    case RoverHandlerId::ConnectTo:
        return accept_connect_to(handlers, command, entry);
    case RoverHandlerId::ConnectInfo:
        return accept_connect_info(handlers, command, entry);
    case RoverHandlerId::LoginConfirmed:
        return accept_login_confirmed(handlers, command, entry);
    case RoverHandlerId::ReloginConfirmed:
        return accept_relogin_confirmed(handlers, command, entry);
    case RoverHandlerId::LogoutConfirmed:
        return accept_logout_confirmed(handlers, command);
    case RoverHandlerId::ServerList:
        return accept_server_list(handlers, command, entry);
    case RoverHandlerId::AdServer:
        return accept_ad_server(command, entry);
    case RoverHandlerId::FileServer:
        return accept_file_server(handlers, command, entry);
    case RoverHandlerId::SystemAlert:
        return accept_system_alert(handlers, command, entry);
    case RoverHandlerId::Latency:
        return accept_latency(command, entry);
    default:
        return rover_offer_lobby_message(handlers, id, command, entry);
    }
}

} // namespace

bool rover_send_ad_event(
    int32_t banner_id, int32_t kind, const char* lang, char* wire, int32_t wire_capacity
) {
    constexpr char kCommandAdEvent[] = "ad_event";
    constexpr char kKeyGame[] = "game";
    constexpr char kKeyBanner[] = "banner";
    constexpr char kKeyKind[] = "kind";
    constexpr char kKeyLang[] = "lang";
    constexpr char kGameTa[] = "ta";
    if (lang == nullptr || lang[0] == '\0')
        return false;
    RoverRecord* record = nullptr;
    RoverEntry* entry = rover_add_request(&record, RoverService::Rover, kCommandAdEvent);
    if (entry == nullptr || !rover_entry_set_string(*entry, kKeyGame, kGameTa) ||
        !rover_entry_set_int(*entry, kKeyBanner, banner_id) ||
        !rover_entry_set_int(*entry, kKeyKind, kind) ||
        !rover_entry_set_string(*entry, kKeyLang, lang)) {
        if (record != nullptr)
            rover_record_destroy(&record);
        return false;
    }
    if (wire != nullptr && wire_capacity > 1) {
        int32_t length = 0;
        if (rover_record_format(*record, wire, wire_capacity - 1, &length) !=
            RoverRecordStatus::Ok) {
            rover_record_destroy(&record);
            wire[0] = '\0';
            return false;
        }
        wire[length] = '\0';
    }
    if (g_rover_state != nullptr)
        rover_send_to_boneyard(record);
    else
        rover_record_destroy(&record);
    return true;
}

bool rover_ad_server_copy(
    char* url, size_t url_cap, int32_t* send_id, char* params, size_t params_cap
) {
    if (url == nullptr || url_cap == 0 || g_rover_state == nullptr)
        return false;
    const RoverAdServer& ad = g_rover_state->ad_server;
    if (!ad.active || ad.send_id != 1 || ad.url[0] == '\0')
        return false;
    size_t url_length = 0;
    while (ad.url[url_length] != '\0')
        ++url_length;
    if (url_length + 1 > url_cap)
        return false;
    size_t params_length = 0;
    while (ad.params[params_length] != '\0')
        ++params_length;
    if (params != nullptr && params_length + 1 > params_cap)
        return false;
    rover_copy_bounded(url, ad.url, static_cast<int32_t>(url_cap));
    if (send_id != nullptr)
        *send_id = ad.send_id;
    if (params != nullptr)
        rover_copy_bounded(params, ad.params, static_cast<int32_t>(params_cap));
    return true;
}

void rover_handler_arm(RoverHandlerSet& handlers, RoverHandlerId id) {
    handlers.active[static_cast<int32_t>(id)] = true;
}

void rover_handler_disarm(RoverHandlerSet& handlers, RoverHandlerId id) {
    handlers.active[static_cast<int32_t>(id)] = false;
}

bool rover_handler_is_armed(const RoverHandlerSet& handlers, RoverHandlerId id) {
    return handlers.active[static_cast<int32_t>(id)];
}

void rover_handlers_disarm_all(RoverHandlerSet& handlers) {
    for (bool& active : handlers.active)
        active = false;
}

void rover_dispatch_message(RoverHandlerSet& handlers, const RoverEntry& entry) {
    const RoverField* command = rover_entry_find(entry, kRoverKeyCommand);
    if (!command)
        return;
    // Matchmaking and QuickPlay are not handler slots. They are taken before the armed loop, the
    // way a record addressed to Rover is, and those commands are consumed even when nothing is
    // listening.
    if (rover_matchmaking_offer(*command, entry))
        return;
    if (rover_quickplay_offer(*command, entry))
        return;
    for (int32_t id = 0; id < kRoverHandlerCount; ++id) {
        if (handlers.active[id] &&
            offer(handlers, static_cast<RoverHandlerId>(id), *command, entry))
            return;
    }
}
