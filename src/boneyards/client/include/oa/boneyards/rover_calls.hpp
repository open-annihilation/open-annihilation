// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The game's side of the Rover boundary: binding the interface table, the lock
// held around every call into it, and the wrappers the game calls instead of
// the table. Each wrapper enters the lock, counts the call as in progress,
// calls its slot when a table is bound (otherwise answers false, null or
// nothing) and leaves. A close requested while a call is in progress (from a
// callback inside the pump) is finished when the pump returns.
//
// Native builds bind the statically linked oa-rover table and keep the
// version handshake. Wrappers are named after the slot they call.
#pragma once

#include "oa/rover/interface.h"

#include <cstdint>

namespace oa::boneyards {

// The game's half of the slot 0 handshake.
inline constexpr int32_t kRoverInterfaceVersion = OA_ROVER_INTERFACE_VERSION;
inline constexpr int32_t kRoverBuildStamp = OA_ROVER_MINIMUM_BUILD_STAMP;

// Where the table comes from: the Rover library's exported entry point.
using RoverGetInterfaceFn = void(ROVER_CALL*)(const RoverInterface** out);

// The module state kept beside the table.
struct RoverModuleState {
    int32_t call_depth;          // wrapper calls in progress
    uint8_t unload_deferred;     // a close arrived while a call was in progress
    bool bound;                  // module handle: the table source was loaded
    const RoverInterface* table; // null until the handshake passes
    // Close detaches (slot 2) first while set; the game never sets it.
    uint8_t detach_on_close;
};

/// Replaces the source of the Rover interface table.
///
/// @param get_interface table source; null makes every load fail as a missing Rover library would
///     (the default is oa-rover's rover_get_interface)
void rover_module_set_source(RoverGetInterfaceFn get_interface);

/// Returns the module state kept beside the Rover table.
///
/// @return the module state
const RoverModuleState* rover_module_state();

/// Binds the table and runs the slot 0 version handshake.
///
/// The table is kept only when the handshake accepts the game's interface
/// version and build stamp.
///
/// @return false when there is no table source or the handshake fails
bool rover_module_load();

/// Unbinds the table now, or once the call in progress returns.
///
/// Detaches first while the module is marked started. A close arriving during
/// a call (a callback inside the pump) is finished when the pump returns.
void rover_module_close();

/// Clears the table and releases the module.
void rover_module_unload();

/// Runs the slot 0 version handshake.
///
/// @param iface_version interface version the game speaks (kRoverInterfaceVersion)
/// @param build_stamp build stamp the game presents (kRoverBuildStamp)
/// @return true when the table accepts the pair; only the low byte of the slot's result is read
bool rover_call_check_version(int32_t iface_version, int32_t build_stamp);

/// Attaches to the client module (slot 1).
///
/// @return false when the attach fails or no table is bound
bool rover_call_attach();

/// Detaches from the client module (slot 2).
void rover_call_detach();

/// Starts the client module's workers (slot 3).
///
/// @return false when the activation fails or no table is bound
bool rover_call_activate();

/// Stops the client module's workers (slot 4).
void rover_call_deactivate();

/// Runs every callback Rover has queued (slot 5), then finishes a close requested meanwhile.
void rover_call_pump_events();

/// Restarts Rover's session (slot 6); its result is not passed on.
void rover_call_reset_session();

/// Starts a login to Central Command (slot 7).
///
/// @param server_name location text of the server to log in to
/// @param username account name
/// @param password account password; empty for a guest
/// @param progress login progress callback
/// @param system_alert callback for system alerts and pages during the session
/// @param disconnect callback for a dropped or failed connection
/// @return true when Rover accepted the request
bool rover_call_start_login(
    const char* server_name,
    const char* username,
    const char* password,
    RoverLoginProgressFn progress,
    RoverSystemAlertFn system_alert,
    RoverDisconnectFn disconnect
);

/// Logs the user out (slot 8).
///
/// @param user_id user id the login confirmed
/// @param logged_out callback run once the logout is done
/// @return true when Rover accepted the request
bool rover_call_send_logout(int32_t user_id, RoverLogoutFn logged_out);

/// Asks for an area's chat room list (slot 9).
///
/// @param area_id area whose rooms are listed
/// @param room_list callback receiving each row, the last marked
/// @return true when Rover accepted the request
bool rover_call_get_rooms(int32_t area_id, RoverRoomListFn room_list);

/// Enters a chat room (slot 10).
///
/// @param room_id room to enter
/// @param area_id area of the room
/// @param password room password; empty for an open room
/// @param entered callback with the entry result
/// @param room_info member feed of the entered room
/// @param room_list room-list update feed of the area
/// @param chat_msg chat line feed of the entered room
/// @return true when Rover accepted the request
bool rover_call_enter_room(
    int32_t room_id,
    int32_t area_id,
    const char* password,
    RoverRoomEnteredFn entered,
    RoverRoomInfoFn room_info,
    RoverRoomUpdateFn room_list,
    RoverChatMsgFn chat_msg
);

/// Leaves one chat room for another (slot 11).
///
/// @param room_id room to enter
/// @param area_id area of the room to enter
/// @param password password of the room to enter
/// @param old_room_id room being left
/// @param old_area_id area of the room being left
/// @param entered callback with the entry result
/// @param room_info member feed of the new room
/// @param room_list room-list update feed of the area
/// @param chat_msg chat line feed of the new room
/// @return true when Rover accepted the request
bool rover_call_switch_room(
    int32_t room_id,
    int32_t area_id,
    const char* password,
    int32_t old_room_id,
    int32_t old_area_id,
    RoverRoomEnteredFn entered,
    RoverRoomInfoFn room_info,
    RoverRoomUpdateFn room_list,
    RoverChatMsgFn chat_msg
);

/// Leaves a chat room (slot 12).
///
/// @param room_id room to leave
/// @param area_id area of the room
/// @param left callback with the result
/// @return true when Rover accepted the request
bool rover_call_leave_room(int32_t room_id, int32_t area_id, RoverRoomLeftFn left);

/// Says a line in a chat room (slot 13).
///
/// @param room_id room the line is said in
/// @param area_id area of the room
/// @param target_user_id user whispered to; 0 for the whole room
/// @param message text of the line
/// @return true when Rover accepted the request
bool rover_call_send_chat(
    int32_t room_id, int32_t area_id, int32_t target_user_id, const char* message
);

/// Starts following an area's battle list (slot 15).
///
/// @param area_id area whose battles are listed
/// @param entered callback with the result
/// @param battle_info battle list feed
/// @param intel_msg intelligence line feed
/// @return true when Rover accepted the request
bool rover_call_enter_battles(
    uint32_t area_id,
    RoverBattlesDoneFn entered,
    RoverBattleInfoFn battle_info,
    RoverIntelMsgFn intel_msg
);

/// Stops following an area's battle list (slot 17).
///
/// @param area_id area left
/// @param left callback with the result
/// @return true when Rover accepted the request
bool rover_call_leave_battles(uint32_t area_id, RoverBattlesDoneFn left);

/// Asks for one battle's intelligence and reports (slot 19).
///
/// @param area_id area of the battle
/// @param game_id battle id
/// @param battle_intel callback with the battle's intelligence
/// @param battle_report callback with each player's report
/// @return true when Rover accepted the request
bool rover_call_get_battle(
    uint32_t area_id,
    const char* game_id,
    RoverBattleIntelFn battle_intel,
    RoverBattleReportFn battle_report
);

/// Hosts a battle (slot 21).
///
/// @param area_id area the battle is listed in
/// @param game_type game mode (GameMode value)
/// @param name battle name
/// @param password battle password; null for an open battle
/// @param description battle description (the Host panel passes null)
/// @param keyword battle keyword (the Host panel passes null)
/// @param skill_sel skill radio index (0 novice, 1 seasoned, 2 expert)
/// @param players_sel number-of-players radio index
/// @param on_death_sel commander death option (1 continues, 2 ends, 3 deathmatch)
/// @param launch callback that launches the game
/// @param debriefing callback after the game
/// @return true when Rover accepted the request
bool rover_call_host_game(
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
);

/// Joins a battle (slot 23).
///
/// @param area_id area of the battle
/// @param game_id battle id
/// @param password battle password; empty for an open battle
/// @param launch callback that launches the game
/// @param debriefing callback after the game
/// @return true when Rover accepted the request
bool rover_call_join_game(
    uint32_t area_id,
    const char* game_id,
    const char* password,
    RoverLaunchGameFn launch,
    RoverDebriefingFn debriefing
);

/// Reports that the player left a battle (slot 24).
///
/// @param area_id area of the battle
/// @param game_id battle id
/// @return true when Rover accepted the request
bool rover_call_send_left_game(uint32_t area_id, const char* game_id);

/// Sends a battle's settings (slot 26).
///
/// The settings mirror a Galactic War planet's game options
/// (RoverSectorPlanet).
///
/// @param area_id area of the battle
/// @param game_id battle id
/// @param lock_options nonzero when the options are locked
/// @param mission mission (map) name
/// @param playerlimit player limit
/// @param deathopt commander death option
/// @param energy starting energy option
/// @param metal starting metal option
/// @param location start location option
/// @param los line-of-sight option
/// @param mapping mapping option
/// @param maxunits unit limit
/// @param watching watching option
/// @param cheats cheats option
/// @param tournament tournament option
/// @return true when Rover accepted the request
bool rover_call_send_game_settings(
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
);

/// Sends one player's entry in a battle (slot 28).
///
/// @param area_id area of the battle
/// @param game_id battle id
/// @param dpid player id in the game session
/// @param name player name
/// @param allegiance player allegiance text
/// @param team player team
/// @return true when Rover accepted the request
bool rover_call_send_player_info(
    uint32_t area_id,
    const char* game_id,
    uint32_t dpid,
    const char* name,
    const char* allegiance,
    int32_t team
);

/// Sends one player's score report after a battle (slot 30).
///
/// @param area_id area of the battle
/// @param game_id battle id
/// @param dpid player id in the game session
/// @param kills units killed
/// @param losses units lost
/// @param energy energy produced
/// @param metal metal produced
/// @param xs_energy excess energy
/// @param xs_metal excess metal
/// @param ignored_9 not forwarded to the request
/// @param ignored_10 not forwarded to the request
/// @param score final score
/// @param victory whether the player won
/// @param last whether this is the last report of the battle
/// @return true when Rover accepted the request
bool rover_call_send_score_report(
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
);

/// Sends a player's message with the score reports (slot 31).
///
/// @param area_id area of the battle
/// @param game_id battle id
/// @param dpid player id in the game session
/// @param message text of the message
/// @return true when Rover accepted the request
bool rover_call_send_score_report_message(
    uint32_t area_id, const char* game_id, uint32_t dpid, const char* message
);

/// Downloads a file over HTTP (slot 32); its result is not passed on.
///
/// @param url address of the file
/// @param dest_path local file the download is written to
/// @param complete callback when the download ends
/// @param progress callback with the percentage done; may be null
void rover_call_fetch_http_file(
    const char* url,
    const char* dest_path,
    RoverHttpCompleteFn complete,
    RoverHttpProgressFn progress
);

/// Parses a record file, calling `record` once per record inside the call (slot 33).
///
/// @param path record file to read
/// @param record callback for each record
/// @param user context handed to each callback
/// @return false when the file cannot be parsed or no table is bound
bool rover_call_parse_record_file(const char* path, RoverParsedRecordFn record, void* user);

/// Opens a Galactic War sector map and reads its header (slot 34).
///
/// @param path sector map file
/// @param[out] out_sector_id sector id
/// @param[out] out_sector_name sector name
/// @param[out] out_width sector width
/// @param[out] out_height sector height
/// @param[out] out_planet_count number of planets
/// @param[out] out_connector_count number of planet-to-planet links
/// @return the open reader, or null on failure or when no table is bound
RoverSectorReader* rover_call_open_sector_map(
    const char* path,
    int32_t* out_sector_id,
    char* out_sector_name,
    int32_t* out_width,
    int32_t* out_height,
    int32_t* out_planet_count,
    int32_t* out_connector_count
);

/// Reads the planets and links of an open sector map (slot 35).
///
/// @param reader reader from rover_call_open_sector_map
/// @param sector_width sector width from the header
/// @param sector_height sector height from the header
/// @param[out] out_planets caller-allocated array of `planet_count` planets
/// @param planet_count number of planets to read
/// @param[out] out_connectors caller-allocated array of `connector_count` links
/// @param connector_count number of links to read
/// @return false when the map cannot be read or no table is bound
bool rover_call_read_sector_planets(
    RoverSectorReader* reader,
    int32_t sector_width,
    int32_t sector_height,
    RoverSectorPlanet* out_planets,
    int32_t planet_count,
    RoverSectorConnector* out_connectors,
    int32_t connector_count
);

/// Closes a sector map reader (slot 36).
///
/// @param reader reader to close
void rover_call_close_sector_map(RoverSectorReader* reader);

/// Asks for the war map's census (slot 38).
///
/// @param area_id area asked about; 0 for every area
/// @param census callback with each area's census
/// @param activity callback with the activity digits
void rover_call_request_census(uint32_t area_id, RoverCensusFn census, RoverActivityFn activity);

/// Asks for the planets' allegiances (slot 39).
///
/// @param allegiances callback with one allegiance letter per planet
void rover_call_request_allegiance(RoverAllegianceFn allegiances);

/// Looks up a content download server (slot 40).
///
/// @param server_type kind of server asked for
/// @param[in,out] out_server record filled with the host and port; its type index is
///     OA_ROVER_FILE_SERVER_INVALID when none is known
void rover_call_get_file_server(int32_t server_type, RoverFileServer* out_server);

/// Pages a user by name (slot 41).
///
/// @param user_name user paged
/// @param message text of the page
void rover_call_send_page_by_name(const char* user_name, const char* message);

/// Sets the latency reply flags (slot 43).
///
/// @param flags flag bits; only bits 0..7 are used
void rover_call_set_latency_reply_flags(uint32_t flags);

/// Creates an empty property bag (slot 44).
///
/// @return the bag, or null when no table is bound
RoverPropertyBag* rover_call_create_property_bag();

/// Destroys a property bag (slot 45).
///
/// A null bag is not passed on and takes no lock.
///
/// @param bag bag to destroy; may be null
void rover_call_destroy_property_bag(RoverPropertyBag* bag);

/// Reads a signed field of a property bag (slot 46).
///
/// The key is passed to Rover as a temporary string object.
///
/// @param bag bag to read
/// @param key field name
/// @param[out] out_value field value
/// @return false when the bag has no such field or no table is bound
bool rover_call_get_field_int(RoverPropertyBag* bag, const char* key, int32_t* out_value);

/// Reads an unsigned field of a property bag (slot 47).
///
/// The key is passed to Rover as a temporary string object.
///
/// @param bag bag to read
/// @param key field name
/// @param[out] out_value field value
/// @return false when the bag has no such field or no table is bound
bool rover_call_get_field_uint(RoverPropertyBag* bag, const char* key, uint32_t* out_value);

/// Reads a text field of a property bag (slot 48).
///
/// The key is passed to Rover as a temporary string object.
///
/// @param bag bag to read
/// @param key field name
/// @param[out] out_value string assigned the field's text; release it with rover_string_release
/// @return false when the bag has no such field or no table is bound
bool rover_call_get_field_string(RoverPropertyBag* bag, const char* key, RoverString* out_value);

/// Stores a text field in a property bag (slot 51).
///
/// The key is passed to Rover as a temporary string object.
///
/// @param bag bag to write
/// @param key field name
/// @param value text stored
/// @return false when the field cannot be stored or no table is bound
bool rover_call_set_bag_string(RoverPropertyBag* bag, const char* key, const RoverString* value);

/// Returns the first entry of a property bag (slot 52).
///
/// @param bag bag to walk
/// @return the entry, or null for an empty bag or when no table is bound
RoverBagEntry* rover_call_get_first_bag_entry(RoverPropertyBag* bag);

/// Asks for a user's profile and ladder standings (slot 63).
///
/// @param user_id user whose profile is asked for
/// @param profile callback with the profile's bag
/// @param ladder_profile callback with each ladder standing's bag; may be null
void rover_call_request_get_profile(
    uint32_t user_id, RoverProfileFn profile, RoverLadderProfileFn ladder_profile
);

/// Submits changes to a user's profile (slot 64).
///
/// @param user_id user whose profile changes
/// @param properties bag of the changed fields
/// @param updated callback with the result
void rover_call_request_set_profile(
    uint32_t user_id, RoverPropertyBag* properties, RoverProfileUpdatedFn updated
);

/// Asks where a user is (slot 65).
///
/// @param username user looked for
/// @param located callback with the answer
void rover_call_locate_player(const char* username, RoverLocateFn located);

/// Asks the game manager for the battles whose names hold some text (slot 66).
///
/// Every battle that matches is answered in turn, the last marked as such.
///
/// @param name text the battles' names are searched for
/// @param located callback with each answer
void rover_call_locate_game(const char* name, RoverGameLocatedFn located);

/// Creates a chat room (slot 69).
///
/// @param area_id area the room is made in
/// @param room_name name of the room
/// @param description room description
/// @param password room password; empty for an open room
/// @param created callback with the new room's ids, 0 when refused
void rover_call_create_room(
    uint32_t area_id,
    const char* room_name,
    const char* description,
    const char* password,
    RoverRoomCreatedFn created
);

/// Hands `location_fn` each server's location text, in server list order (slot 70).
///
/// @param location_fn callback called inside the call for each server
void rover_call_enumerate_areas(RoverAreaEntryFn location_fn);

/// Reads the selected server's location text (slot 71).
///
/// @param[out] out_location location text; left unchanged when no server is selected
void rover_call_find_current_area(const char** out_location);

/// Names the server whose yard master answered (slot 72).
///
/// @return the server's leading name, or null when no table is bound
const char* rover_call_match_area_name();

} // namespace oa::boneyards
