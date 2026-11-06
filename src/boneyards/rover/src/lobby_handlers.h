// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// State of the handlers behind the lobby slots (9-39, 63-66 and 69): rooms
// and chat, battle lists, game launches, the HTTP fetch, the galactic war
// feeds, profiles and the locators.
// Each handler keeps the filter it was armed with (an area, a room, a game)
// and the game's callback.
// Unless a slot says otherwise the callbacks run from slot 5, on the thread
// that pumps.
#pragma once

#include "oa/rover/interface.h"
#include "record.h"

#include <stdint.h>

// A game id as the handlers keep it: at most 36 characters.
constexpr int32_t kRoverGameIdLimit = 0x24;
// A room password as the entered_room handler keeps it: at most 15 characters.
constexpr int32_t kRoverRoomPasswordLimit = 0xf;

// profile_info and ladder_profile (handler 13): a profile's fields gather in
// a bag until a message marks the last.
struct RoverProfileHandler {
    uint32_t user_id{};
    RoverPropertyBag* properties{};
    RoverProfileFn callback{};
    RoverLadderProfileFn ladder{};
};

// profile_updated (handler 14).
struct RoverProfileUpdatedHandler {
    uint32_t user_id{};
    RoverProfileUpdatedFn callback{};
};

// room_list rows answering get_rooms (handler 16).
struct RoverRoomListHandler {
    int32_t area_id{};
    RoverRoomListFn callback{};
};

// room_list rows of the room's area while in it (handler 17).
struct RoverRoomUpdateHandler {
    int32_t area_id{};
    RoverRoomUpdateFn callback{};
};

// Handlers 18 (room_info), 20 (left_room) and 21 (chat_msg): one room.
struct RoverRoomInfoHandler {
    int32_t room_id{};
    int32_t area_id{};
    RoverRoomInfoFn callback{};
};

struct RoverRoomLeftHandler {
    int32_t room_id{};
    int32_t area_id{};
    RoverRoomLeftFn callback{};
};

struct RoverChatHandler {
    int32_t room_id{};
    int32_t area_id{};
    RoverChatMsgFn callback{};
};

// entered_room (handler 19). A switch (slot 11) keeps the room it leaves and
// the callbacks the new room's handlers take once the enter succeeds.
struct RoverEnteredRoomHandler {
    int32_t room_id{};
    int32_t area_id{};
    RoverRoomEnteredFn callback{};
    char password[kRoverRoomPasswordLimit + 1]{};
    bool switching{};
    int32_t old_room_id{};
    int32_t old_area_id{};
    RoverRoomInfoFn room_info{};
    RoverRoomUpdateFn room_list{};
    RoverChatMsgFn chat_msg{};
};

// Handlers 22 and 30 (battles_entered, battles_left): one area's battle list.
struct RoverBattlesDoneHandler {
    uint32_t area_id{};
    RoverBattlesDoneFn callback{};
};

struct RoverBattleInfoHandler { // 24
    uint32_t area_id{};
    RoverBattleInfoFn callback{};
};

struct RoverBattleInfoFieldsHandler { // 23
    uint32_t area_id{};
    RoverBattleInfoKvFn callback{};
};

struct RoverIntelHandler { // 25
    uint32_t area_id{};
    RoverIntelMsgFn callback{};
};

// Handlers 26-29: the intel and reports of one battle.
struct RoverBattleIntelHandler {
    uint32_t area_id{};
    char game_id[kRoverGameIdLimit + 1]{};
    RoverBattleIntelFn callback{};
};

struct RoverBattleIntelFieldsHandler {
    uint32_t area_id{};
    char game_id[kRoverGameIdLimit + 1]{};
    RoverBattleIntelKvFn callback{};
};

struct RoverBattleReportHandler {
    uint32_t area_id{};
    char game_id[kRoverGameIdLimit + 1]{};
    RoverBattleReportFn callback{};
};

struct RoverBattleReportFieldsHandler {
    uint32_t area_id{};
    char game_id[kRoverGameIdLimit + 1]{};
    RoverBattleReportKvFn callback{};
};

// launch_game (handlers 31 and 32). A host takes the launch of any game in
// its area; a joiner only that of the game it asked to join.
struct RoverLaunchHandler {
    bool any_game{};
    uint32_t area_id{};
    char game_id[kRoverGameIdLimit + 1]{};
    RoverLaunchGameFn callback{};
    RoverDebriefingFn debriefing{};
};

struct RoverLaunchFieldsHandler {
    bool any_game{};
    uint32_t area_id{};
    char game_id[kRoverGameIdLimit + 1]{};
    RoverLaunchGameKvFn callback{};
    RoverDebriefingFn debriefing{};
};

// debriefing (handler 33), armed by a successful launch for two messages.
struct RoverDebriefingHandler {
    uint32_t area_id{};
    char game_id[kRoverGameIdLimit + 1]{};
    RoverDebriefingFn callback{};
    int32_t remaining{};
};

// A name as the user_located handler keeps it: at most 15 characters.
constexpr int32_t kRoverLocateNameLimit = 0xf;

// user_located (handler 40), armed by slot 65 for the answer about one name.
struct RoverUserLocatedHandler {
    RoverLocateFn callback{};
    char name[kRoverLocateNameLimit + 1]{};
};

// game_located (handler 41), armed by slot 66. It keeps no name: every
// game_located while it is armed is the answer, until one is marked last.
struct RoverGameLocatedHandler {
    RoverGameLocatedFn callback{};
};

// A room name as the room_created handler keeps it: at most 23 characters.
constexpr int32_t kRoverRoomNameLimit = 0x17;

// room_created (handler 44), armed by slot 69. The area and name it keeps
// are not compared with the reply: any room_created answers it.
struct RoverRoomCreatedHandler {
    uint32_t area_id{};
    char name[kRoverRoomNameLimit + 1]{};
    RoverRoomCreatedFn callback{};
};

struct RoverLobbyHandlers {
    RoverProfileHandler profile{};                         // 13
    RoverProfileUpdatedHandler profile_updated{};          // 14
    RoverRoomListHandler room_list_final{};                // 16
    RoverRoomUpdateHandler room_list{};                    // 17
    RoverRoomInfoHandler room_info{};                      // 18
    RoverEnteredRoomHandler entered_room{};                // 19
    RoverRoomLeftHandler left_room{};                      // 20
    RoverChatHandler chat_msg{};                           // 21
    RoverBattlesDoneHandler battles_entered{};             // 22
    RoverBattleInfoFieldsHandler battle_info_fields{};     // 23
    RoverBattleInfoHandler battle_info{};                  // 24
    RoverIntelHandler intel_msg{};                         // 25
    RoverBattleIntelFieldsHandler battle_intel_fields{};   // 26
    RoverBattleIntelHandler battle_intel{};                // 27
    RoverBattleReportFieldsHandler battle_report_fields{}; // 28
    RoverBattleReportHandler battle_report{};              // 29
    RoverBattlesDoneHandler battles_left{};                // 30
    RoverLaunchFieldsHandler launch_fields{};              // 31
    RoverLaunchHandler launch{};                           // 32
    RoverDebriefingHandler debriefing{};                   // 33
    RoverHttpCompleteFn http_complete{};                   // 34
    RoverCensusFn census{};                                // 35
    RoverAllegianceFn allegiances{};                       // 36
    RoverActivityFn activity{};                            // 39
    RoverUserLocatedHandler user_located{};                // 40
    RoverGameLocatedHandler game_located{};                // 41
    RoverRoomCreatedHandler room_created{};                // 44
};

struct RoverHandlerSet;
enum class RoverHandlerId : uint8_t;

// Offers an entry to one of the lobby handlers; true when it takes the message.
bool rover_offer_lobby_message(
    RoverHandlerSet& handlers, RoverHandlerId id, const RoverField& command, const RoverEntry& entry
);

// Per-group acceptors, each in the file that owns the group's slots.
bool rover_offer_profile_message(
    RoverHandlerSet& handlers, RoverHandlerId id, const RoverField& command, const RoverEntry& entry
);
bool rover_offer_room_message(
    RoverHandlerSet& handlers, RoverHandlerId id, const RoverField& command, const RoverEntry& entry
);
bool rover_offer_battle_message(
    RoverHandlerSet& handlers, RoverHandlerId id, const RoverField& command, const RoverEntry& entry
);
bool rover_offer_game_message(
    RoverHandlerSet& handlers, RoverHandlerId id, const RoverField& command, const RoverEntry& entry
);

/// Takes httprequestcomplete and hands the game the URL, the file and the status letter's distance
/// from 'A'.
///
/// The handler stays armed. A message without URL, File or Status is taken but not reported.
///
/// @param handlers handler set holding the completion callback
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the message is httprequestcomplete, false otherwise
bool rover_accept_http_complete(
    RoverHandlerSet& handlers, const RoverField& command, const RoverEntry& entry
);

/// Bytes, terminator included, of a matchmaking offer id or game id.
inline constexpr int32_t kRoverMatchmakingIdBytes = 16;

/// Bytes, terminator included, of a matchmaking opponent or battle name.
inline constexpr int32_t kRoverMatchmakingNameBytes = 48;

/// Filters of matchmaking_look and matchmaking_join. The bracket is not a field.
struct RoverMatchmakingQuery {
    int32_t game_type{};
    int32_t skill_mode{};
    int32_t side{};
    int32_t planets{};
    uint32_t area_id{};
    bool area_known{};
};

/// One matchmaking_status, copied out of the entry before it is destroyed.
struct RoverMatchmakingStatus {
    int32_t result{};
    int32_t queued{};
    int32_t count{};
    int32_t meter{};
};

/// One matchmaking_found. The strings live in this record.
struct RoverMatchmakingFound {
    char offer_id[kRoverMatchmakingIdBytes]{};
    char name[kRoverMatchmakingNameBytes]{};
    int32_t skill{};
    int32_t game_type{};
    int32_t role{};
    int32_t side{};
    uint32_t area_id{};
};

/// One matchmaking_go. Optional strings are empty, and their known flags false, when the entry
/// omitted them.
struct RoverMatchmakingGo {
    char offer_id[kRoverMatchmakingIdBytes]{};
    int32_t result{};
    int32_t role{};
    uint32_t area_id{};
    char game_id[kRoverMatchmakingIdBytes]{};
    bool game_known{};
    char name[kRoverMatchmakingNameBytes]{};
    bool name_known{};
    int32_t game_type{};
    int32_t skill{};
    int32_t seats{};
    int32_t on_death{};
    int32_t side{};
    bool side_known{};
};

/// Receives a matchmaking_status. The status lives only for the call.
using RoverMatchmakingStatusFn = void (*)(const RoverMatchmakingStatus* status);

/// Receives a matchmaking_found. The found record lives only for the call.
using RoverMatchmakingFoundFn = void (*)(const RoverMatchmakingFound* found);

/// Receives a matchmaking_go. The go record lives only for the call.
using RoverMatchmakingGoFn = void (*)(const RoverMatchmakingGo* go);

/// Installs the three matchmaking replies. Any of them may be null, and a matching command is still
/// taken. This is not a handler slot.
///
/// @param status called for matchmaking_status
/// @param found called for matchmaking_found
/// @param go called for matchmaking_go
void rover_matchmaking_set_handlers(
    RoverMatchmakingStatusFn status, RoverMatchmakingFoundFn found, RoverMatchmakingGoFn go
);

/// Sends matchmaking_look. area_id is omitted unless the query says it is known and this planet.
/// The bracket is not sent: the server reads the bracket itself.
///
/// @param query filters; null sends nothing
/// @return true when the record was queued
bool rover_matchmaking_look(const RoverMatchmakingQuery* query);

/// Sends matchmaking_join, replacing the filters of a ticket already queued. The bracket is not sent.
///
/// @param query filters; null sends nothing
/// @return true when the record was queued
bool rover_matchmaking_join(const RoverMatchmakingQuery* query);

/// Sends matchmaking_cancel. Close does not call this.
///
/// @return true when the record was queued
bool rover_matchmaking_cancel();

/// Sends matchmaking_answer. accept is 1 for Yes and 0 for No.
///
/// @param offer_id the offer being answered; null or empty omits the id
/// @param accept 1 to take the battle, 0 to refuse it
/// @return true when the record was queued
bool rover_matchmaking_answer(const char* offer_id, int32_t accept);

/// Takes matchmaking_status, matchmaking_found and matchmaking_go.
///
/// Called from dispatch before the armed handlers. A matchmaking command is consumed even when the
/// callbacks are null. Any other command is left for the handlers.
///
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the command is one of the three matchmaking replies
bool rover_matchmaking_offer(const RoverField& command, const RoverEntry& entry);

/// Bytes, terminator included, of a QuickPlay planet name.
inline constexpr int32_t kRoverQuickPlayPlanetBytes = 16;

/// Bytes, terminator included, of a QuickPlay battle or offer name.
inline constexpr int32_t kRoverQuickPlayBattleBytes = 32;

/// Bytes, terminator included, of a QuickPlay favorite or mission name.
inline constexpr int32_t kRoverQuickPlayMapBytes = 48;

/// Filters of quickplay_join. favorite is empty when the check is off.
struct RoverQuickPlayQuery {
    int32_t game_type{};
    int32_t where{};
    int32_t skill{};
    int32_t side{};
    int32_t prefer_favorite{};
    char favorite[kRoverQuickPlayMapBytes]{};
    uint32_t area_id{};
    bool area_known{};
};

/// One quickplay_status, copied out of the entry before it is destroyed.
struct RoverQuickPlayStatus {
    int32_t result{};
    int32_t queued{};
    int32_t meter{};
    int32_t fits{};
    int32_t planets{};
};

/// One quickplay_scan. No field here is a finished sentence.
struct RoverQuickPlayScan {
    int32_t seq{};
    int32_t kind{};
    int32_t reason{};
    int32_t game_type{};
    int32_t skill{};
    int32_t seats{};
    int32_t seats_max{};
    char planet[kRoverQuickPlayPlanetBytes]{};
    char battle[kRoverQuickPlayBattleBytes]{};
};

/// One quickplay_found. Optional strings are empty, and their known flags false, when omitted.
struct RoverQuickPlayFound {
    char offer_id[kRoverMatchmakingIdBytes]{};
    int32_t role{};
    uint32_t area_id{};
    char game_id[kRoverMatchmakingIdBytes]{};
    bool game_known{};
    char name[kRoverQuickPlayBattleBytes]{};
    char planet[kRoverQuickPlayPlanetBytes]{};
    int32_t game_type{};
    int32_t skill{};
    int32_t seats{};
    int32_t on_death{};
    char map[kRoverQuickPlayMapBytes]{};
    bool map_known{};
};

/// Receives a quickplay_status. The status lives only for the call.
using RoverQuickPlayStatusFn = void (*)(const RoverQuickPlayStatus* status);

/// Receives a quickplay_scan. The scan lives only for the call.
using RoverQuickPlayScanFn = void (*)(const RoverQuickPlayScan* scan);

/// Receives a quickplay_found. The found record lives only for the call.
using RoverQuickPlayFoundFn = void (*)(const RoverQuickPlayFound* found);

/// One quickplay_withdrawn. game_id is empty, and game_known false, when the offer named no battle.
/// reason is 0 filled, 1 started, 2 abandoned, 3 the seat reservation lapsed.
struct RoverQuickPlayWithdrawn {
    char offer_id[kRoverMatchmakingIdBytes]{};
    char game_id[kRoverMatchmakingIdBytes]{};
    bool game_known{};
    int32_t reason{};
};

/// Receives a quickplay_withdrawn. The record lives only for the call.
using RoverQuickPlayWithdrawnFn = void (*)(const RoverQuickPlayWithdrawn* withdrawn);

/// Installs the QuickPlay replies. Any of them may be null, and a matching command is still
/// taken. This is not a handler slot.
///
/// @param status called for quickplay_status
/// @param scan called for quickplay_scan
/// @param found called for quickplay_found
/// @param withdrawn called for quickplay_withdrawn
void rover_quickplay_set_handlers(
    RoverQuickPlayStatusFn status,
    RoverQuickPlayScanFn scan,
    RoverQuickPlayFoundFn found,
    RoverQuickPlayWithdrawnFn withdrawn
);

/// Sends quickplay_join. area_id is omitted unless the query says it is known. favorite is omitted
/// unless prefer_favorite is set and the name is not empty.
///
/// @param query filters; null sends nothing
/// @return true when the record was queued
bool rover_quickplay_join(const RoverQuickPlayQuery* query);

/// Sends quickplay_cancel. No fields.
///
/// @return true when the record was queued
bool rover_quickplay_cancel();

/// Sends quickplay_answer. accept is 1 for Yes and 0 for No.
///
/// @param offer_id the offer being answered; null or empty omits the id
/// @param accept 1 to take the battle, 0 to refuse it
/// @return true when the record was queued
bool rover_quickplay_answer(const char* offer_id, int32_t accept);

/// Takes quickplay_status, quickplay_scan, quickplay_found and quickplay_withdrawn.
///
/// Called from dispatch before the armed handlers. A QuickPlay command is consumed even when the
/// callbacks are null. Any other command is left for the handlers.
///
/// @param command the entry's command field
/// @param entry the entry's fields
/// @return true when the command is one of the QuickPlay replies
bool rover_quickplay_offer(const RoverField& command, const RoverEntry& entry);
