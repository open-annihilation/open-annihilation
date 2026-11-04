// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The 50 message handlers of RoverState.handlers. A handler is armed while it waits
// for its message; the game-thread pump offers each "Rover" entry of an
// incoming record to the armed handlers in id order until one accepts it by
// command name. An accepting handler reads the entry's fields and calls the
// continuation it was armed with: a step of the connect sequence, or a
// callback the game handed to a slot.
#pragma once

#include "lobby_handlers.h"
#include "oa/rover/interface.h"
#include "record.h"

#include <stdint.h>

// Handler ids, in the order the pump offers an entry to them; handlers 3
// and 4 are one handler for yard 1 and yard 2. AdServer is armed with the
// file server after a redirect and keeps the banner host on the session.
// AreaList and UserCreated are never armed. GameLocated is armed by slot 66.
// The other handlers from 42 up but RoomCreated are armed only by interface
// slots the app never calls (see interface.cpp), so none of them has state
// or an acceptor here.
enum class RoverHandlerId : uint8_t {
    ConnectToYardMaster = 0, // _connect_to_yardmaster
    ConnectToBoneyard = 1,   // _connect_to_boneyard
    YardMasterClosed = 2,    // _yardmaster_closed
    Yard1Closed = 3,         // _boneyard_closed, yard_number 1
    Yard2Closed = 4,         // _boneyard_closed, yard_number 2
    ConnectTo = 5,
    ConnectInfo = 6,
    LoginConfirmed = 7,
    ReloginConfirmed = 8,
    LogoutConfirmed = 9,
    ServerList = 10,
    AdServer = 11,
    FileServer = 12,
    ProfileInfo = 13, // also ladder_profile
    ProfileUpdated = 14,
    AreaList = 15,
    RoomListFinal = 16, // room_list rows answering get_rooms
    RoomList = 17,      // room_list rows of the entered room
    RoomInfo = 18,
    EnteredRoom = 19,
    LeftRoom = 20,
    ChatMessage = 21,
    BattlesEntered = 22,
    BattleInfoFields = 23,
    BattleInfo = 24,
    IntelMessage = 25,
    BattleIntelFields = 26,
    BattleIntel = 27,
    BattleReportFields = 28,
    BattleReport = 29,
    BattlesLeft = 30,
    LaunchGameFields = 31,
    LaunchGame = 32,
    Debriefing = 33,
    HttpRequestComplete = 34,
    Census = 35,
    Allegiances = 36,
    SystemAlert = 37,
    Latency = 38,
    Activity = 39,
    UserLocated = 40,
    GameLocated = 41,
    UserCreated = 42,
    ChatArea = 43,
    RoomCreated = 44,
    Metagame = 45, // six metagame commands
    House = 46,    // four house commands
    HouseFound = 47,
    HouseInfoSet = 48,
    HouseCreated = 49,
};

constexpr int32_t kRoverHandlerCount = 50;

// Continuations of the connect sequence. The connect handlers pass whether
// the link's state is other than "open" and whether its result was
// kRoverLinkResultFailed.
using RoverConnectResultFn = void (*)(int32_t not_open, int32_t failed);
using RoverNoticeFn = void (*)();
using RoverYardClosedFn = void (*)(int32_t yard_number);
using RoverRedirectFn =
    void (*)(int32_t result, const char* address, uint16_t port, const char* message);
using RoverConnectInfoFn = void (*)(int32_t role, int32_t compression, const char* seed);
using RoverLoginResultFn = void (*)(int32_t result, uint32_t user_id, uint32_t area_id);
using RoverReloginResultFn = void (*)(int32_t result);
using RoverServerEntryFn = void (*)(
    const char* cluster_name,
    const char* domain,
    int32_t cluster_size,
    const char* location,
    bool last
);
using RoverFileServerFn = void (*)(const char* host, int32_t port, int32_t type_index);

// Which handlers are armed, and what each armed handler calls.
struct RoverHandlerSet {
    bool active[kRoverHandlerCount]{};
    RoverConnectResultFn yardmaster_connect{};
    RoverConnectResultFn boneyard_connect{};
    RoverNoticeFn yardmaster_closed{};
    RoverYardClosedFn yard_closed[2]{}; // yard 1, yard 2
    RoverRedirectFn connect_to{};
    RoverConnectInfoFn connect_info{};
    RoverLoginResultFn login_confirmed{};
    RoverReloginResultFn relogin_confirmed{};
    RoverLogoutFn logout_confirmed{};
    RoverServerEntryFn server_list{};
    RoverFileServerFn file_server{};
    RoverSystemAlertFn system_alert{};
    RoverLobbyHandlers lobby{}; // handlers 13, 14, 16-36, 39, 40, 41 and 44
};

/// Arms a handler for its message.
///
/// Arming an armed handler changes nothing: each handler is armed at most once.
///
/// @param[in,out] handlers handler set to change
/// @param id handler to arm
void rover_handler_arm(RoverHandlerSet& handlers, RoverHandlerId id);

/// Disarms a handler.
///
/// @param[in,out] handlers handler set to change
/// @param id handler to disarm
void rover_handler_disarm(RoverHandlerSet& handlers, RoverHandlerId id);

/// Reports whether a handler is armed.
///
/// @param handlers handler set to read
/// @param id handler to test
/// @return true while the handler waits for its message
bool rover_handler_is_armed(const RoverHandlerSet& handlers, RoverHandlerId id);

/// Disarms every handler.
///
/// @param[in,out] handlers handler set to change
void rover_handlers_disarm_all(RoverHandlerSet& handlers);

/// Offers one "Rover" entry to the armed handlers in id order until one takes it.
///
/// An entry without a command is dropped. No entry goes further: slots 80 and 81, which would
/// register and unregister other commands, fail, and the game never calls them.
///
/// @param[in,out] handlers handler set whose continuations may run and disarm handlers
/// @param entry entry to dispatch
void rover_dispatch_message(RoverHandlerSet& handlers, const RoverEntry& entry);

/// Queues ad_event on the boneyard link when a session is up.
///
/// The record carries game ta, the banner id, kind (1 shown, 2 link opened) and the interface
/// language. With no session the record is dropped after its text is written. Nothing is queued
/// when lang is null or empty.
///
/// @param banner_id banner id from ads.def
/// @param kind 1 for a showing, 2 for a click
/// @param lang interface language code
/// @param[out] wire buffer for the record text; null skips it
/// @param wire_capacity bytes available in wire, including the terminator
/// @return true when the record was built
bool rover_send_ad_event(
    int32_t banner_id, int32_t kind, const char* lang, char* wire, int32_t wire_capacity
);
