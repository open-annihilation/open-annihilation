// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

/* Every slot of the Rover interface table. interface.cpp fills the table from
 * these; a slot gains its behaviour when its stub there is replaced by a
 * definition in the source file that owns it. */
#ifndef OA_ROVER_SLOTS_H
#define OA_ROVER_SLOTS_H

#include "oa/rover/interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Accepts only the interface version and build floor the game was built against.
///
/// @param iface_version interface version the caller speaks; only OA_ROVER_INTERFACE_VERSION passes
/// @param build_stamp caller's build stamp, compared signed against OA_ROVER_MINIMUM_BUILD_STAMP
/// @return 1 when both pass, else 0, in the whole 32-bit result, not only its low byte
int32_t ROVER_CALL rover_check_version(int32_t iface_version, int32_t build_stamp); /* 0 */

/// Brings up sockets and the session and loads the server list: the saved file, else the shipped
/// one, else the master server list the build baked in.
///
/// @return false when the socket library or the session block could not be brought up
bool ROVER_CALL rover_attach(void); /* 1 */

/// Waits for the workers to finish, frees the session and shuts sockets down.
///
/// Does nothing while detached.
void ROVER_CALL rover_detach(void); /* 2 */

/// Starts the link and fetch workers unless they run, then arms the latency handler, which answers
/// every probe from here on.
///
/// @return false while detached or when a worker could not be started
bool ROVER_CALL rover_activate(void); /* 3 */

/// Stops both workers and the select thread.
///
/// Does nothing while detached.
void ROVER_CALL rover_deactivate(void); /* 4 */

/// Hands every queued incoming record to the handlers, entry by entry, on the calling thread.
///
/// Only entries addressed to "Rover" are dispatched; every stored callback runs from here.
void ROVER_CALL rover_pump_events(void); /* 5 */

/// Abandons the session: forgets the resolved server, disarms every handler, swallows the next
/// disconnect report, closes the links and restarts the workers.
///
/// @return false while detached or when the workers could not be restarted
bool ROVER_CALL rover_reset_session(void); /* 6 */

/// Starts logging in to the server listed at server_name.
///
/// Progress, alerts and disconnects arrive through the callbacks from slot 5; the stages are listed
/// in connect.h. A server other than the one already resolved first has its old yard master closed
/// (stage 0); a resolved one goes straight to asking for a boneyard (stage 3).
///
/// @param server_name location of the server to log in to, as the server list names it
/// @param username user name
/// @param password password, or null or empty for an anonymous login
/// @param progress called with each stage of the login
/// @param system_alert called for each system_alert message
/// @param disconnect called when a link closes while the login runs
/// @return false while detached or while a login already runs, true otherwise
bool ROVER_CALL rover_start_login(
    const char* server_name,
    const char* username,
    const char* password,
    RoverLoginProgressFn progress,
    RoverSystemAlertFn system_alert,
    RoverDisconnectFn disconnect
); /* 7 */

/// Sends logout for the user and arms the logout_confirmed handler with the game's callback.
///
/// @param user_id user to log out, sent unsigned
/// @param logged_out called once logout_confirmed arrives
/// @return false while detached, true otherwise
bool ROVER_CALL rover_send_logout(int32_t user_id, RoverLogoutFn logged_out); /* 8 */

/// Asks for an area's room list; the rows arrive through room_list until one is marked last.
///
/// @param area_id area whose rooms to list
/// @param room_list called for each row of the list, from slot 5
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_get_rooms(int32_t area_id, RoverRoomListFn room_list); /* 9 */

/// Enters a room: arms the handlers of the area's room list, the room's members, the enter result
/// and the room's chat, then asks to enter.
///
/// @param room_id room to enter
/// @param area_id area of the room
/// @param password room password, or null; the handler keeps 15 characters
/// @param entered called once with the enter result
/// @param room_info called for each member who arrives, changes or leaves
/// @param room_list called for each room_list row of the area
/// @param chat_msg called for each line said in the room
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_enter_room(
    int32_t room_id,
    int32_t area_id,
    const char* password,
    RoverRoomEnteredFn entered,
    RoverRoomInfoFn room_info,
    RoverRoomUpdateFn room_list,
    RoverChatMsgFn chat_msg
); /* 10 */

/// Moves to another room: only the enter result is armed now; the old room's handlers stay until
/// the new room is confirmed.
///
/// On success the entered_room handler moves the room handlers to the new room and leaves the old
/// one.
///
/// @param room_id room to enter
/// @param area_id area of the room
/// @param password room password, or null; the handler keeps 15 characters
/// @param old_room_id room to leave once the new one is entered
/// @param old_area_id area of the room to leave
/// @param entered called once with the enter result
/// @param room_info member callback for the new room
/// @param room_list room_list callback for the new room's area
/// @param chat_msg chat callback for the new room
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_switch_room(
    int32_t room_id,
    int32_t area_id,
    const char* password,
    int32_t old_room_id,
    int32_t old_area_id,
    RoverRoomEnteredFn entered,
    RoverRoomInfoFn room_info,
    RoverRoomUpdateFn room_list,
    RoverChatMsgFn chat_msg
); /* 11 */

/// Leaves a room: its chat, member and room-list handlers are dropped, and for a room other than 0
/// leave_room is sent and its result armed.
///
/// @param room_id room to leave; 0 only drops the handlers
/// @param area_id area of the room
/// @param left called once with whether leaving failed
/// @return false while detached, true otherwise
bool ROVER_CALL rover_leave_room(int32_t room_id, int32_t area_id, RoverRoomLeftFn left); /* 12 */

/// Says a line in a room, or to one member when target_user_id is not 0.
///
/// @param room_id room to speak in
/// @param area_id area of the room
/// @param target_user_id member to address, or 0 for the whole room
/// @param message line to say
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_send_chat(
    int32_t room_id, int32_t area_id, int32_t target_user_id, const char* message
); /* 13 */

/// Subscribes to an area's battle list with the key/value battle_info callback.
///
/// Arms battles_entered, the key/value battle_info handler and intel_msg for the area, then sends
/// enter_battles.
///
/// @param area_id area whose battle list to join
/// @param entered called once with whether the request failed
/// @param battle_info called for each battle added or removed, with the row's other fields in a bag
///     that lives for the call
/// @param intel_msg called for each line of the battle list
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_enter_battles_kv(
    uint32_t area_id,
    RoverBattlesDoneFn entered,
    RoverBattleInfoKvFn battle_info,
    RoverIntelMsgFn intel_msg
); /* 14 */

/// Subscribes to an area's battle list: battles_entered once, then battle_info rows and intel_msg
/// lines until the area is left.
///
/// @param area_id area whose battle list to join
/// @param entered called once with whether the request failed
/// @param battle_info called for each battle added or removed, with the host's side
/// @param intel_msg called for each line of the battle list
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_enter_battles(
    uint32_t area_id,
    RoverBattlesDoneFn entered,
    RoverBattleInfoFn battle_info,
    RoverIntelMsgFn intel_msg
); /* 15 */

/// Leaves the key/value battle list subscription of an area.
///
/// Drops the key/value battle_info handler and any armed key/value intel and report handlers, drops
/// intel_msg, arms battles_left and sends leave_battles.
///
/// @param area_id area whose battle list to leave
/// @param left called once with whether the request failed
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_leave_battles_kv(uint32_t area_id, RoverBattlesDoneFn left); /* 16 */

/// Leaves an area's battle list; battles_left reports the result.
///
/// Drops the battle_info handler and any armed intel and report handlers, drops intel_msg, arms
/// battles_left and sends leave_battles.
///
/// @param area_id area whose battle list to leave
/// @param left called once with whether the request failed
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_leave_battles(uint32_t area_id, RoverBattlesDoneFn left); /* 17 */

/// Asks for one battle with the key/value intel and report callbacks.
///
/// Rearms both handlers for the battle and sends get_battle.
///
/// @param area_id area of the battle
/// @param game_id id of the battle's game, kept to 36 characters
/// @param battle_intel called once with the battle's settings and its other fields in a bag
/// @param battle_report called once per player with the player's line and its other fields in a bag
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_get_battle_kv(
    uint32_t area_id,
    const char* game_id,
    RoverBattleIntelKvFn battle_intel,
    RoverBattleReportKvFn battle_report
); /* 18 */

/// Asks for one battle: battle_intel answers once, battle_report once per player.
///
/// Rearms both handlers for the battle and sends get_battle.
///
/// @param area_id area of the battle
/// @param game_id id of the battle's game, kept to 36 characters
/// @param battle_intel called once with the battle's settings
/// @param battle_report called once per player with the player's score line
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_get_battle(
    uint32_t area_id,
    const char* game_id,
    RoverBattleIntelFn battle_intel,
    RoverBattleReportFn battle_report
); /* 19 */

/// Hosts a game with extra fields from a property bag.
///
/// Arms the key/value launch handler for any game of the area, then sends host_game with the bag's
/// typed entries added.
///
/// @param area_id area to host in
/// @param type game type
/// @param name game name, or null to leave it out
/// @param password game password, or null to leave it out
/// @param description game description, or null to leave it out
/// @param extra_fields bag whose entries are added to the request, or null
/// @param launch called once launch_game arrives, with the reply's other fields in a bag
/// @param debriefing called for each of the two debriefings after a started game
/// @return false while detached or when the request cannot be built, true once it is queued
bool ROVER_CALL rover_host_game_kv(
    uint32_t area_id,
    int32_t type,
    const char* name,
    const char* password,
    const char* description,
    RoverPropertyBag* extra_fields,
    RoverLaunchGameKvFn launch,
    RoverDebriefingFn debriefing
); /* 20 */

/// Hosts a game from the host dialog; launch_game answers with the game to start.
///
/// The dialog's selectors go on the wire as skill 1 and 2 as they are (others left out), players
/// 1-3 as 3, 4 and 1 (others 2), and on_death 1 and 3 as they are (others 2).
///
/// @param area_id area to host in
/// @param game_type game type
/// @param name game name, or null to leave it out
/// @param password game password, or null to leave it out
/// @param description game description, or null to leave it out
/// @param keyword keyword, or null to leave it out
/// @param skill_sel host dialog's skill selector
/// @param players_sel host dialog's player-count selector
/// @param on_death_sel host dialog's commander-death selector
/// @param launch called once launch_game arrives
/// @param debriefing called for each of the two debriefings after a started game
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_host_game(
    uint32_t area_id,
    int32_t game_type,
    const char* name,
    const char* password,
    const char* description,
    const char* keyword,
    int32_t skill_sel,
    int32_t players_sel,
    int32_t on_death_sel,
    RoverLaunchGameFn launch,
    RoverDebriefingFn debriefing
); /* 21 */

/// Joins a game with extra fields from a property bag.
///
/// Arms the key/value launch handler for that game, then sends join_game with the bag's typed
/// entries added.
///
/// @param area_id area of the game
/// @param game_id game to join, kept to 36 characters by the handler
/// @param password game password, or null to leave it out
/// @param extra_fields bag whose entries are added to the request, or null
/// @param launch called once launch_game for the game arrives, with the reply's other fields in a
///     bag
/// @param debriefing called for each of the two debriefings after a started game
/// @return false while detached or when the request cannot be built, true once it is queued
bool ROVER_CALL rover_join_game_with_fields(
    uint32_t area_id,
    const char* game_id,
    const char* password,
    RoverPropertyBag* extra_fields,
    RoverLaunchGameKvFn launch,
    RoverDebriefingFn debriefing
); /* 22 */

/// Joins a game; launch_game for that game answers.
///
/// @param area_id area of the game
/// @param game_id game to join, kept to 36 characters by the handler
/// @param password game password, or null to leave it out
/// @param launch called once launch_game for the game arrives
/// @param debriefing called for each of the two debriefings after a started game
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_join_game(
    uint32_t area_id,
    const char* game_id,
    const char* password,
    RoverLaunchGameFn launch,
    RoverDebriefingFn debriefing
); /* 23 */

/// Tells the server the player left a game.
///
/// @param area_id area of the game
/// @param game_id game left
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_send_left_game(uint32_t area_id, const char* game_id); /* 24 */

/// Sends a game's settings from a property bag.
///
/// @param area_id area of the game
/// @param game_id game whose settings these are
/// @param extra_fields bag whose entries are the settings, or null
/// @return false while detached or when the request cannot be built, true once it is queued
bool ROVER_CALL rover_send_game_settings_fields(
    uint32_t area_id, const char* game_id, RoverPropertyBag* extra_fields
); /* 25 */

/// Sends a game's settings; each option holding its usual value is left out.
///
/// The usual values are lockOptions 0, playerlimit 2, deathopt 2, energy and metal 1000, location
/// 2, los 1, mapping 1, maxunits 250, watching 1, cheats 1 and tournament 0; an empty mission is
/// left out too.
///
/// @param area_id area of the game
/// @param game_id game whose settings these are
/// @param lock_options whether the options are locked
/// @param mission mission name, or null or empty to leave it out
/// @param playerlimit player limit
/// @param deathopt commander-death option
/// @param energy starting energy
/// @param metal starting metal
/// @param location start location option
/// @param los line-of-sight option
/// @param mapping mapping option
/// @param maxunits unit limit
/// @param watching watching option
/// @param cheats cheats option
/// @param tournament tournament option
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_send_game_settings(
    uint32_t area_id,
    const char* game_id,
    int32_t lock_options,
    const char* mission,
    int32_t playerlimit,
    int32_t deathopt,
    int32_t energy,
    int32_t metal,
    int32_t location,
    int32_t los,
    int32_t mapping,
    int32_t maxunits,
    int32_t watching,
    int32_t cheats,
    int32_t tournament
); /* 26 */

/// Sends one player's info with extra fields from a property bag.
///
/// @param area_id area of the game
/// @param game_id game the player is in
/// @param dpid the player's DirectPlay id
/// @param name the player's name, or null to leave it out
/// @param extra_fields bag whose entries are added to the request, or null
/// @return false while detached or when the request cannot be built, true once it is queued
bool ROVER_CALL rover_send_player_info_fields(
    uint32_t area_id,
    const char* game_id,
    uint32_t dpid,
    const char* name,
    RoverPropertyBag* extra_fields
); /* 27 */

/// Sends one player's side and team.
///
/// @param area_id area of the game
/// @param game_id game the player is in
/// @param dpid the player's DirectPlay id
/// @param name the player's name, or null to leave it out
/// @param allegiance the player's side, or null to leave it out
/// @param team the player's team
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_send_player_info(
    uint32_t area_id,
    const char* game_id,
    uint32_t dpid,
    const char* name,
    const char* allegiance,
    int32_t team
); /* 28 */

/// Sends a score report whose statistics come from a property bag.
///
/// @param area_id area of the game
/// @param game_id game the report is about
/// @param dpid the reporting player's DirectPlay id
/// @param props bag whose entries are the statistics, or null
/// @param last true to mark the player's last report
/// @return false while detached or when the request cannot be built, true once it is queued
bool ROVER_CALL rover_send_score_report_props(
    uint32_t area_id, const char* game_id, uint32_t dpid, RoverPropertyBag* props, bool last
); /* 29 */

/// Sends a player's statistics as a score report; negative values are left out.
///
/// @param area_id area of the game
/// @param game_id game the report is about
/// @param dpid the reporting player's DirectPlay id
/// @param kills kills statistic
/// @param losses losses statistic
/// @param energy energy statistic
/// @param metal metal statistic
/// @param xs_energy excess-energy statistic
/// @param xs_metal excess-metal statistic
/// @param ignored_9 commander count; never sent
/// @param ignored_10 commander count; never sent
/// @param score score statistic
/// @param victory true to add the victory flag
/// @param last true to mark the player's last report
/// @return false while detached, true once the request is queued
/// @quirk Arguments 9 and 10, the commander counts, are never forwarded to the request.
bool ROVER_CALL rover_send_score_report(
    uint32_t area_id,
    const char* game_id,
    uint32_t dpid,
    int32_t kills,
    int32_t losses,
    int32_t energy,
    int32_t metal,
    int32_t xs_energy,
    int32_t xs_metal,
    int32_t ignored_9,
    int32_t ignored_10,
    int32_t score,
    bool victory,
    bool last
); /* 30 */

/// Sends an in-game chat line as a score report message.
///
/// @param area_id area of the game
/// @param game_id game the message is about
/// @param dpid the sending player's DirectPlay id
/// @param message chat text, or null to leave it out
/// @return false while detached, true once the request is queued
bool ROVER_CALL rover_send_score_report_message(
    uint32_t area_id, const char* game_id, uint32_t dpid, const char* message
); /* 31 */

/// Queues a download of url into dest_path for the fetch thread.
///
/// complete is called from slot 5 when the download is done, progress (when given) from the fetch
/// thread. Only one completion callback is held: a later fetch replaces it. Both texts are kept to
/// 259 characters.
///
/// @param url URL to fetch, or null for none
/// @param dest_path file to save the body to, or null for none
/// @param complete called from slot 5 with the URL, the file and status 0
/// @param progress called on the fetch thread with whole percentages, or null
/// @return false while detached, true otherwise, even when the fetch could not be allocated
bool ROVER_CALL rover_fetch_http_file(
    const char* url,
    const char* dest_path,
    RoverHttpCompleteFn complete,
    RoverHttpProgressFn progress
); /* 32 */

/// Hands every record of a file to the callback: seven named fields as text and all other fields,
/// by type, in a fresh bag destroyed after the call.
///
/// The named fields are command, id, name, sort, category, headline and file, "" when absent. A
/// record without an entry list arrives with no fields, and each record is freed after its call.
///
/// @param path record file to read
/// @param record called synchronously once per record
/// @param user passed through to each call
/// @return false only when the file cannot be opened
bool ROVER_CALL
rover_parse_record_file(const char* path, RoverParsedRecordFn record, void* user); /* 33 */

/// Opens a Galactic War sector map and checks its "sector" header.
///
/// The header's "Map" entry must carry id, name (1-50 characters), width, height and the planet and
/// connector counts, none of them -1. A header without a readable entry name is rejected. The
/// outputs are written only when the header passes.
///
/// @param path sector map file to read
/// @param[out] out_sector_id receives the sector id
/// @param[out] out_sector_name buffer of at least 51 bytes for the sector name
/// @param[out] out_width receives the sector width
/// @param[out] out_height receives the sector height
/// @param[out] out_planet_count receives the number of planet records that follow
/// @param[out] out_connector_count receives the number of connector records that follow
/// @return the reader positioned after the header, or null
RoverSectorReader* ROVER_CALL rover_open_sector_map(
    const char* path,
    int32_t* out_sector_id,
    char* out_sector_name,
    int32_t* out_width,
    int32_t* out_height,
    int32_t* out_planet_count,
    int32_t* out_connector_count
); /* 34 */

/// Reads planet_count "planet" records, then connector_count "connector" records whose ids both
/// name one of those planets.
///
/// Each planet is range-checked field by field against the sector size and the planets read before
/// it. Reading stops at the first record that fails, leaving the entries read so far filled. A
/// connector record missing a field, a negative connector count and a null reader all fail.
///
/// @param reader reader from rover_open_sector_map, or null
/// @param sector_width sector width the planet positions must keep inside
/// @param sector_height sector height the planet positions must keep inside
/// @param[out] out_planets array of planet_count planets to fill
/// @param planet_count number of planet records to read
/// @param[out] out_connectors array of connector_count connectors to fill
/// @param connector_count number of connector records to read
/// @return true when every record was read and passed its checks
bool ROVER_CALL rover_read_sector_planets(
    RoverSectorReader* reader,
    int32_t sector_width,
    int32_t sector_height,
    RoverSectorPlanet* out_planets,
    int32_t planet_count,
    RoverSectorConnector* out_connectors,
    int32_t connector_count
); /* 35 */

/// Closes the map's file and frees the reader; null is ignored.
///
/// @param reader reader from rover_open_sector_map, or null
void ROVER_CALL rover_close_sector_map(RoverSectorReader* reader); /* 36 */

/// Hands each record of a file to the callback as a bag of all its fields, with is_last set when no
/// further record could be read.
///
/// Each record is freed after its call.
///
/// @param path record file to read
/// @param record called synchronously once per record; the bag is destroyed after the call
/// @return false for a null callback, an unreadable file or first record, or a record without an
///     entry list (enumeration stops there)
bool ROVER_CALL rover_enumerate_record_file(const char* path, RoverRecordFieldsFn record); /* 37 */

/// Asks for one area's census, or every area's, and arms the census and activity handlers with the
/// game's callbacks.
///
/// A handler already armed with a different callback is rearmed with the new one. Does nothing
/// while detached.
///
/// @param area_id area to count; 0 asks for every area
/// @param census called for each census answer, from slot 5
/// @param activity called with each activity digit string, from slot 5
void ROVER_CALL
rover_request_census(uint32_t area_id, RoverCensusFn census, RoverActivityFn activity); /* 38 */

/// Asks for the planets' allegiances and arms their handler with the game's callback.
///
/// Does nothing while detached.
///
/// @param allegiances called with each allegiance string, one letter per planet, from slot 5
void ROVER_CALL rover_request_allegiance(RoverAllegianceFn allegiances); /* 39 */

/// Copies one of the two download-server records into the caller's record.
///
/// The "file_server" handler keeps them in the session block, index 0 for news and 1 for ladders.
/// The host name is copied with a bound of 0xff: at most 254 characters and a terminator. A
/// detached session reports the invalid index.
///
/// @param server_type record to copy: 0 news, 1 ladders
/// @param[out] out_server receives type_index, host and port; for any other type only type_index is
///     set, to OA_ROVER_FILE_SERVER_INVALID
void ROVER_CALL rover_get_file_server(int32_t server_type, RoverFileServer* out_server); /* 40 */

/// Pages a user by name.
///
/// Does nothing while detached.
///
/// @param user_name user to page; sent only when non-null and non-empty
/// @param message page text; sent only when non-null and non-empty
void ROVER_CALL rover_send_page_by_name(const char* user_name, const char* message); /* 41 */

/// Pages a user by id.
///
/// Does nothing while detached.
///
/// @param user_id user to page, sent unsigned
/// @param message page text
void ROVER_CALL rover_send_page_by_id(int32_t user_id, const char* message); /* 42 */

/// Sets the flags the next latency reply carries; the reply clears them.
///
/// Does nothing while detached.
///
/// @param flags latency flags; bits 0-7 are sent
void ROVER_CALL rover_set_latency_reply_flags(uint32_t flags); /* 43 */

/// Allocates an empty property bag with its cursor at the end.
///
/// @return the new bag, or null when Rover's heap is exhausted
RoverPropertyBag* ROVER_CALL rover_create_property_bag(void); /* 44 */

/// Releases every entry's strings and frees the bag.
///
/// The validation field is cleared before the free, so a stale handle fails validation instead of
/// being freed twice.
///
/// @param bag bag to destroy; an invalid handle is ignored
void ROVER_CALL rover_destroy_property_bag(RoverPropertyBag* bag); /* 45 */

/// Copies the signed value stored under a key.
///
/// @param bag bag to read
/// @param key key to look up
/// @param[out] out_value receives the value; untouched on failure
/// @return true when bag is valid and key holds a signed entry; false for a missing key or another
///     type
bool ROVER_CALL
rover_get_field_int(RoverPropertyBag* bag, const RoverString* key, int32_t* out_value); /* 46 */

/// Copies the unsigned value stored under a key.
///
/// @param bag bag to read
/// @param key key to look up
/// @param[out] out_value receives the value; untouched on failure
/// @return true when bag is valid and key holds an unsigned entry; false for a missing key or
///     another type
bool ROVER_CALL
rover_get_field_uint(RoverPropertyBag* bag, const RoverString* key, uint32_t* out_value); /* 47 */

/// Assigns the string stored under a key to out_value, sharing its buffer when allowed.
///
/// @param bag bag to read
/// @param key key to look up
/// @param[in,out] out_value string assigned the value; left alone on failure
/// @return true when bag is valid and key holds a string entry; false for a missing key or another
///     type
bool ROVER_CALL rover_get_field_string(
    RoverPropertyBag* bag, const RoverString* key, RoverString* out_value
); /* 48 */

/// Stores a signed value under a key, inserting the entry in key order or retyping an existing one.
///
/// The entry's name becomes key; values of other types stay in the entry.
///
/// @param bag bag to write
/// @param key key to store under
/// @param value signed value to store
/// @return true when stored; false for an invalid bag or when a new entry could not be allocated
bool ROVER_CALL
rover_set_field_int(RoverPropertyBag* bag, const RoverString* key, int32_t value); /* 49 */

/// Stores a unsigned value under a key, inserting the entry in key order or retyping an existing
/// one.
///
/// The entry's name becomes key; values of other types stay in the entry.
///
/// @param bag bag to write
/// @param key key to store under
/// @param value unsigned value to store
/// @return true when stored; false for an invalid bag or when a new entry could not be allocated
bool ROVER_CALL
rover_set_field_uint(RoverPropertyBag* bag, const RoverString* key, uint32_t value); /* 50 */

/// Stores a string under a key, inserting the entry in key order or retyping an existing one.
///
/// The entry's name becomes key; values of other types stay in the entry.
///
/// @param bag bag to write
/// @param key key to store under
/// @param value string to store; its buffer is shared when allowed
/// @return true when stored; false for an invalid bag or when a new entry could not be allocated
bool ROVER_CALL rover_set_bag_string(
    RoverPropertyBag* bag, const RoverString* key, const RoverString* value
); /* 51 */

/// Restarts the cursor at the lowest key and returns that entry, moving the cursor past it.
///
/// @param bag bag to walk
/// @return the entry with the lowest key, or null for an empty or invalid bag
RoverBagEntry* ROVER_CALL rover_get_first_bag_entry(RoverPropertyBag* bag); /* 52 */

/// Returns the cursor's entry and advances the cursor to the next key.
///
/// @param bag bag to walk
/// @return the entry, or null once the cursor has passed the last entry or for an invalid bag
RoverBagEntry* ROVER_CALL rover_get_next_bag_entry(RoverPropertyBag* bag); /* 53 */

/// Looks up the entry stored under a key, whatever its type.
///
/// @param bag bag to search
/// @param key key to look up
/// @return the entry, or null when it is absent or bag is invalid
RoverBagEntry* ROVER_CALL
rover_find_bag_entry(RoverPropertyBag* bag, const RoverString* key); /* 54 */

/// Returns an entry's type tag.
///
/// @param entry entry to read, or null
/// @return an OA_ROVER_ENTRY_* tag; OA_ROVER_ENTRY_NONE both for a null entry and an untyped one
uint32_t ROVER_CALL rover_get_entry_type(const RoverBagEntry* entry); /* 55 */

/// Assigns an entry's key to out_name, whatever the entry's type.
///
/// @param entry entry to read, or null
/// @param[in,out] out_name string assigned the key
/// @return false for a null entry, true otherwise
bool ROVER_CALL rover_get_entry_name(const RoverBagEntry* entry, RoverString* out_name); /* 56 */

/// Copies a signed entry's value.
///
/// @param entry entry to read, or null
/// @param[out] out_value receives the value; untouched on failure
/// @return true when entry is non-null and signed, false otherwise
bool ROVER_CALL rover_get_entry_int(const RoverBagEntry* entry, int32_t* out_value); /* 57 */

/// Copies a unsigned entry's value.
///
/// @param entry entry to read, or null
/// @param[out] out_value receives the value; untouched on failure
/// @return true when entry is non-null and unsigned, false otherwise
bool ROVER_CALL rover_get_entry_uint(const RoverBagEntry* entry, uint32_t* out_value); /* 58 */

/// Assigns a string entry's value to out_value.
///
/// @param entry entry to read, or null
/// @param[in,out] out_value string assigned the value; left alone on failure
/// @return true when entry is non-null and a string, false otherwise
bool ROVER_CALL rover_get_entry_string(const RoverBagEntry* entry, RoverString* out_value); /* 59 */

/// Retypes the entry as signed and stores a value.
///
/// @param entry entry to write, or null
/// @param value signed value to store
/// @return false for a null entry, true otherwise
bool ROVER_CALL rover_set_entry_int(RoverBagEntry* entry, int32_t value); /* 60 */

/// Retypes the entry as unsigned and stores a value.
///
/// @param entry entry to write, or null
/// @param value unsigned value to store
/// @return false for a null entry, true otherwise
bool ROVER_CALL rover_set_entry_uint(RoverBagEntry* entry, uint32_t value); /* 61 */

/// Assigns value to the entry's string and retypes the entry as a string.
///
/// @param entry entry to write, or null
/// @param value string to store; its buffer is shared when allowed
/// @return false for a null entry, true otherwise
bool ROVER_CALL rover_set_entry_string(RoverBagEntry* entry, const RoverString* value); /* 62 */

/// Asks for a user's profile; the fields gather in a new bag, replacing one still gathering, until
/// the reply marked last.
///
/// The game asks for it right after login and aborts the login without it. Does nothing while
/// detached.
///
/// @param user_id user whose profile to read
/// @param profile called once with whether the profile was read and its bag, which is freed after
///     the call
/// @param ladder_profile called for each ladder profile with its ladder id, sub-id and a bag of its
///     fields
void ROVER_CALL rover_request_get_profile(
    uint32_t user_id, RoverProfileFn profile, RoverLadderProfileFn ladder_profile
); /* 63 */

/// Sends changed profile fields from a bag; profile_updated reports the result.
///
/// Does nothing while detached.
///
/// @param user_id user whose profile to change
/// @param properties bag whose typed entries are sent, or null
/// @param updated called once with whether the update succeeded
void ROVER_CALL rover_request_set_profile(
    uint32_t user_id, RoverPropertyBag* properties, RoverProfileUpdatedFn updated
); /* 64 */

/// Asks the user manager where a user is, arming user_located with the game's callback and the
/// name.
///
/// An answer still awaited for an earlier name is forgotten. Does nothing while detached.
///
/// @param username user to locate; the handler keeps 15 characters at most, null as ""
/// @param located called once with the answer, from slot 5
void ROVER_CALL rover_locate_player(const char* username, RoverLocateFn located); /* 65 */

/// Asks the game manager where a game is, arming game_located with the game's callback.
///
/// The name is only the request: every game_located while the handler is armed is reported, and a
/// later request replaces the callback. The handler stays armed until a reply whose last field is
/// not zero. Does nothing while detached.
///
/// @param name game to locate; sent only when non-null and non-empty
/// @param located called for each game_located answer, from slot 5, until one is marked last
void ROVER_CALL rover_locate_target(const char* name, RoverGameLocatedFn located); /* 66 */

/// Does nothing; 3.1c starts an HTTP login on the worker thread here.
///
/// The game never uses this slot.
///
/// @param url_suffix URL suffix; ignored
/// @param login_name login name; ignored
/// @param email e-mail address; ignored
/// @param password password; ignored
/// @param cd_key CD key; ignored
/// @param complete completion callback; ignored
void ROVER_CALL rover_start_http_login(
    const char* url_suffix,
    const char* login_name,
    const char* email,
    const char* password,
    const char* cd_key,
    RoverHttpLoginFn complete
); /* 67 */

/// Does nothing; 3.1c queues an areas request here.
///
/// The game never uses this slot.
///
/// @param area_mask area mask; ignored
/// @param area_id area id; ignored
/// @param areas callback; ignored
void ROVER_CALL
rover_request_areas(uint32_t area_mask, uint32_t area_id, RoverOpaqueFn areas); /* 68 */

/// Creates a room in an area: arms room_created with the game's callback and the area and name,
/// dropping a creation still awaited, then asks the chat manager.
///
/// Does nothing while detached.
///
/// @param area_id area to create the room in
/// @param room_name room name; the handler keeps 23 characters, null as ""
/// @param description room description, or null or empty to leave it out
/// @param password room password, or null or empty to leave it out
/// @param created called once with the new room's area, id and name
void ROVER_CALL rover_create_room(
    uint32_t area_id,
    const char* room_name,
    const char* description,
    const char* password,
    RoverRoomCreatedFn created
); /* 69 */

/// Calls location_fn with the location of every server, in list order.
///
/// Does nothing for a null callback or while detached.
///
/// @param location_fn called synchronously once per server
void ROVER_CALL rover_enumerate_areas(RoverAreaEntryFn location_fn); /* 70 */

/// Points out_location at the location of the first selected server.
///
/// @param[out] out_location receives the location text, or null when no server is selected or while
///     detached
void ROVER_CALL rover_find_current_area(const char** out_location); /* 71 */

/// Returns the leading name of the last server listed at the location whose yard master answered.
///
/// @return the server's name, or null when none matches or while detached
const char* ROVER_CALL rover_match_area_name(void); /* 72 */

/// Reports failure; 3.1c queues the request and always reports true.
///
/// The game never uses this slot, and its five arguments are ignored. ? They are the callbacks for
/// the metagame replies; slot 74's argument is the last of them.
///
/// @return false
bool ROVER_CALL rover_enter_metagame(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t); /* 73 */

/// Reports failure; 3.1c queues the request and always reports true.
///
/// The game never uses this slot, and its argument is ignored. ? It is the callback for the reply
/// to leaving the metagame.
///
/// @return false
bool ROVER_CALL rover_leave_metagame(uint32_t); /* 74 */

/// Reports failure; 3.1c queues the request and always reports true.
///
/// The game never uses this slot, and its arguments are ignored. ? The first four are the callbacks
/// for the four house replies.
///
/// @param type ? house type text; ignored
/// @param extra_fields extra request fields; ignored
/// @return false
bool ROVER_CALL rover_get_house_info(
    uint32_t, uint32_t, uint32_t, uint32_t, const char* type, void* extra_fields
); /* 75 */

/// Reports failure; 3.1c queues the request and always reports true.
///
/// The game never uses this slot, and its arguments are ignored. ? The first is the callback for
/// the house_found reply.
///
/// @param type ? house type text; ignored
/// @param extra_fields extra request fields; ignored
/// @return false
bool ROVER_CALL rover_find_house(uint32_t, const char* type, void* extra_fields); /* 76 */

/// Reports failure; 3.1c queues the request and always reports true.
///
/// The game never uses this slot, and its arguments are ignored. ? The first is the callback for
/// the house_info_set_conf reply.
///
/// @param house_id ? house the request is for; ignored
/// @param extra_fields extra request fields; ignored
/// @return false
bool ROVER_CALL rover_set_house_info(uint32_t, uint32_t house_id, void* extra_fields); /* 77 */

/// Reports failure; 3.1c queues the request and always reports true.
///
/// The game never uses this slot, and its arguments are ignored. ? The first is the callback for
/// the house_created_conf reply.
///
/// @param house_name ? name of the house to create; ignored
/// @param crest ? the house's crest; ignored
/// @param extra_fields extra request fields; ignored
/// @return false
bool ROVER_CALL
rover_create_house(uint32_t, const char* house_name, uint32_t crest, void* extra_fields); /* 78 */

/// Reports failure; 3.1c queues the command and always reports true.
///
/// The game never uses this slot.
///
/// @param destination service the command is for; ignored
/// @param command command name; ignored
/// @param extra_fields extra request fields; ignored
/// @return false
bool ROVER_CALL
rover_queue_command(const char* destination, const char* command, void* extra_fields); /* 79 */

/// Reports failure; 3.1c keeps the handler for the command and always reports true.
///
/// The game never uses this slot.
///
/// @param command command name; ignored
/// @param handler callback; ignored
/// @return false
bool ROVER_CALL rover_register_command_handler(const char* command, RoverOpaqueFn handler); /* 80 */

/// Reports failure; 3.1c drops the command's handler and always reports true.
///
/// The game never uses this slot.
///
/// @param command command name; ignored
/// @return false
bool ROVER_CALL rover_unregister_command(const char* command); /* 81 */

#ifdef __cplusplus
}
#endif

#endif
