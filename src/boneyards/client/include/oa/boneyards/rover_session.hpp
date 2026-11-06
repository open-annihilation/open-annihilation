// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The game's Central Command session: the Rover-facing fields of the Boneyards
// system object, the callbacks a login hands Rover, and the BoneyardsService
// binding the Boneyards screens use. Rover's callbacks carry no context, so
// they reach the one session through a single shared instance.
#pragma once

#include "oa/boneyards/people_lists.hpp"
#include "oa/boneyards/service.hpp"
#include "oa/rover/platform.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oa::boneyards {

// The session fields the Rover calls read and write.
struct RoverSession {
    SessionRecord record;
    uint8_t pump_enabled; // the idle tick pumps Rover while set
    char password[kSessionNameCapacity];
    // Host the login is reaching, from the resolve step, so a refused or timed-out connect can
    // name it. Empty when that step named none.
    char yard_host[256]{};
    PeopleLists* people_lists; // created on first use
    ServiceEvents events;
};

/// Returns the one Rover session.
///
/// @return the session
RoverSession* rover_session();

/// Returns the session's people lists, creating them on first use.
///
/// @param[in,out] session session holding the lists
/// @return the lists, or null when they cannot be allocated
PeopleLists* rover_session_people_lists(RoverSession* session);

/// Writes the session's people lists back to their files and frees them.
///
/// @param[in,out] session session holding the lists
void rover_session_release_people_lists(RoverSession* session);

/// Copies the player's folder into `out`, or an empty string when it is not chosen yet.
///
/// `capacity` counts the terminating NUL. A folder that does not fit is
/// reported as empty, so a cut path is never used. The query must not throw.
using PlayerFolderQuery = void (*)(void* context, char* out, std::size_t capacity);

/// Places Rover's files and selects the platform table oa-rover runs on.
///
/// Call it before the client is activated. A relative path is read from the
/// player's folder when a copy is there, then from the game's files when a
/// reader is set, then from the expansion folder when one is named and the
/// file is there, and otherwise from `install_dir`. `boneyards/server.byd`
/// is the exception, in any spelling: only the player's copy is read, so
/// the 1998 list in the expansion or the game folder is absent. A relative path is
/// never created inside `install_dir` or the expansion folder: it is created
/// under the player's folder, once that folder is named. Clears a folder
/// named with rover_session_set_player_folder, a query set with
/// rover_session_set_player_folder_query, an expansion folder named with
/// rover_session_set_expansion_folder, a query set with
/// rover_session_set_expansion_folder_query, a reader set with
/// rover_session_set_virtual_files and an ad server named with
/// rover_session_set_ad_server.
///
/// @param install_dir game folder; null for none
/// @param server_list server list read in place of boneyards/serv_sav.byd; null or
///     empty keeps the saved one. A rewrite of that list still goes to the player's folder
void rover_session_configure(const char* install_dir, const char* server_list);

/// Names the ad server the banner strip fetches from, in place of one a record named.
///
/// Used by tests. send_id other than 1 suppresses the fetch, and does not fall through to a
/// host Rover stored. A null url is an empty one. Cleared by rover_session_configure.
///
/// @param url host the catalogue is fetched from; null suppresses the fetch
/// @param send_id 1 fetches; any other value suppresses the fetch
/// @param params query appended to the catalogue request; null is none
void rover_session_set_ad_server(const char* url, int32_t send_id, const char* params);

/// Forgets an ad server named with rover_session_set_ad_server.
///
/// A host Rover stored is then reported again.
void rover_session_clear_ad_server();

/// Copies the ad server the banner strip should fetch from.
///
/// An override set with rover_session_set_ad_server wins, including one whose send_id suppresses
/// the fetch. Otherwise the host the last ad_server record stored is copied. False when no fetch
/// should be made, and then the buffers are left unchanged.
///
/// @param[out] url buffer for the url
/// @param url_capacity bytes available in url, including the terminator
/// @param[out] send_id receives 1 when a fetch should be made
/// @param[out] params buffer for the params; empty when the record named none
/// @param params_capacity bytes available in params, including the terminator
/// @return true when send_id is 1 and url is not empty
bool rover_session_ad_server(
    char* url, std::size_t url_capacity, int32_t* send_id, char* params, std::size_t params_capacity
);

// ad_event kinds. An impression is one showing of a server banner. A click is one opening of its
// link.
inline constexpr int32_t kAdEventImpression = 1;
inline constexpr int32_t kAdEventClick = 2;

/// Receives the wire text of one ad_event the session has just handed on.
///
/// Used by tests. The note must not throw. A null note stops the notes.
using AdEventNote = void (*)(const char* wire, void* context);

/// Installs a note for ad events, or none.
///
/// @param note called with the wire text, or null
/// @param context passed back to note
void rover_session_set_ad_event_note(AdEventNote note, void* context);

/// Sends ad_event for one banner showing or one click.
///
/// The record is addressed to Rover: game ta, the banner id from ads.def, kind 1 when the banner
/// was shown and kind 2 when its link was opened, and the interface language code. It is queued on
/// the boneyard link when a session is up. A note installed with rover_session_set_ad_event_note
/// receives that same text. Nothing is sent when lang is null or empty.
///
/// @param banner_id banner id from ads.def
/// @param kind 1 for a showing, 2 for a click
/// @param lang interface language code
void rover_session_report_ad_event(int32_t banner_id, int32_t kind, const char* lang);

/// Names the player's own folder for this run.
///
/// The people lists, the fetch log and the saved server list are written
/// under its boneyards directory. A file there is read ahead of the game
/// folder's copy, except the server list named at configuration, which is
/// the list read for this run. Null or "" names no folder, so a relative
/// file is read from the game folder and nothing is written there. An
/// explicit folder is used ahead of the query.
///
/// @param folder the player's folder, or null
void rover_session_set_player_folder(const char* folder);

/// Asks for the player's folder whenever a file is opened or created.
///
/// Used when the folder is chosen after configuration. The query runs on
/// the thread that opens or creates the file and must not throw. A null
/// query stops asking. An explicit folder is used instead while one is set.
///
/// @param query called with `context` to fill a buffer, or null
/// @param context passed back to `query`
void rover_session_set_player_folder_query(PlayerFolderQuery query, void* context);

/// Reads one game file into `out`.
///
/// False, leaving `out` unchanged, when the file is missing. An empty file
/// is true and leaves `out` empty. `path` uses '/' and has no leading "./".
/// The reader must not throw; a throw is a missing file.
using VirtualFileRead = bool (*)(void* context, const char* path, std::vector<uint8_t>* out);

/// Names how the game's files are read for this run.
///
/// A relative path with no copy in the player's folder, and that is not the
/// server list named at configuration, is read through `read` before the
/// expansion folder's disk and the install's disk. `boneyards/server.byd`
/// is never read this way. The path handed to `read`
/// uses '/' and has no leading "./". Null stops asking. Cleared by
/// rover_session_configure.
///
/// @param read called with `context` and the relative path, or null
/// @param context passed back to `read`
void rover_session_set_virtual_files(VirtualFileRead read, void* context);

/// Names the expansion folder whose loose files are read for this run.
///
/// A relative path with no copy in the player's folder is read from this
/// folder when the file is there, after the game's files and ahead of the
/// install's disk. `boneyards/server.byd` is not read from this folder.
/// The disk path of a file (rover_session_placed_path) names
/// this folder ahead of the install when the file is there. Null or "" names
/// no folder. An explicit folder is used ahead of the query. Cleared by
/// rover_session_configure.
///
/// @param folder the expansion folder, or null
void rover_session_set_expansion_folder(const char* folder);

/// Asks for the expansion folder whenever a file is opened.
///
/// Used when the folder is known only once the player's folder is chosen,
/// which is after configuration. The query runs on the thread that opens
/// the file and must not throw. A null query stops asking. An explicit
/// folder is used instead while one is set. A folder that does not fit in
/// `capacity` is reported empty, so a cut path is never used.
///
/// @param query called with `context` to fill a buffer, or null
/// @param context passed back to `query`
void rover_session_set_expansion_folder_query(PlayerFolderQuery query, void* context);

/// Reads a whole file the way the platform opens it.
///
/// The player's copy, then the game's files, then the expansion folder, then
/// the install's disk. `boneyards/server.byd` is only the player's copy.
/// False when `path` or `out` is null or the file cannot be opened, and
/// then `out` is left unchanged. An empty file is true and leaves `out`
/// empty.
///
/// @param path path as Rover or the game names it
/// @param[out] out receives the bytes
/// @return true when the file was read
bool rover_session_read_file(const char* path, std::vector<uint8_t>* out);

/// Returns the native disk path a relative file is read from.
///
/// The player's copy when one is there, otherwise the expansion folder when
/// the file is there, otherwise the same path under the game folder, its
/// names matched without regard to case. `boneyards/server.byd` names the
/// player's copy only, and no file when the player has none. The saved server list names the
/// configured substitute while one is set. An absolute path is unchanged. A
/// file that exists only among the game's files has this disk path only as
/// the place a loose copy would be.
///
/// @param path path as Rover or the game names it
/// @return the native path
std::string rover_session_placed_path(const char* path);

/// Returns where a file the game writes is created.
///
/// A relative path is placed under the player's folder. The fetch log,
/// named http.log, is placed in that folder's Boneyards directory, with
/// the saved server list and the people lists. A boneyards directory of
/// another spelling is renamed to Boneyards on that write where the file
/// system has two names. An absolute path is
/// unchanged. Empty when a relative path has no player folder yet, so
/// nothing is written into the game folder.
///
/// @param path path as Rover or the game names it
/// @return the native path, or "" when a relative file cannot be written yet
std::string rover_session_write_path(const char* path);

/// Tells whether rover_session_configure has run (oa-game runs it at start-up).
///
/// @return true once configured
bool rover_session_configured();

/// Returns the platform table rover_session_configure selected for oa-rover.
///
/// @return the table, or null before configuration
const RoverPlatform* rover_session_platform();

// Location text of the entry in a server list written for a host.
inline constexpr const char* kHostServerLocation = "Command Line Server";

/// Resolves a server given in place of the saved server list.
///
/// An existing file is the list itself. Otherwise `server` names the yard
/// master's host (":9110", the port Rover always dials, may follow) and a
/// list holding just that host, selected, is written to the temporary
/// directory.
///
/// @param server server list file or host name
/// @param[out] error reason when the server cannot be resolved
/// @return the list's path, or "" with `error` set
std::string rover_session_server_list(const std::string& server, std::string* error);

/// Tells whether the configured server list is one written for a host.
///
/// @return true while it is
bool rover_session_uses_host_list();

/// Returns the service the Boneyards screens use, bound to the Rover calls.
///
/// Rover serves every request. The offline service answers only checks that
/// build a screen without a session.
///
/// @return the service
BoneyardsService rover_session_service();

/// Pumps Rover's events while the session is pumping (the idle tick's Rover call).
///
/// @param session session to pump; null does nothing
void rover_session_idle(const RoverSession* session);

/// Remembers the frontend clock, in the same units the game draws by.
///
/// Thirty units are one second. The idle tick ends a sector request once this
/// clock has moved that far, or once three seconds of real time have passed,
/// whichever is first. A run that steps the clock and does not sleep still
/// finishes. Without a reading, only real time bounds the wait.
///
/// @param tick the clock's current reading
void rover_session_note_clock(uint32_t tick);

// How long a login waits for the current sector before the map loads anyway.
inline constexpr int32_t kSectorFetchWaitMs = 3000;

// Frontend clock units in that wait. The clock moves thirty units a second.
inline constexpr uint32_t kSectorFetchWaitTicks = 90;

/// Asks for the logged-in area's war map, naming the SHA-256 of the player's
/// copy when that file is already there.
///
/// area is the server's area name, used only as the map's file name. An empty
/// or unsafe name sends nothing. False when Rover is detached. The request
/// stays open until the idle tick settles it; this does not wait.
///
/// @param area_id area id from the login; sent with the request
/// @param area area name, such as alpha
/// @return true when the request was queued
bool rover_session_begin_sector_fetch(uint32_t area_id, const char* area);

/// True while a sector request begun for this login is still open.
///
/// @return true until the idle tick settles it, or finish runs
bool rover_session_sector_fetch_pending();

/// Stores a finished sector reply, or gives the request up once
/// kSectorFetchWaitMs of real time, or kSectorFetchWaitTicks of the frontend
/// clock, have passed since begin.
///
/// One pass. Does not pump and does not sleep. The idle tick calls it after
/// its pump. A Ready reply is stored before the session may leave logged_in.
///
/// @return true when the request ended on this pass
bool rover_session_settle_sector_fetch();

/// Gives a request begun directly, rather than by a login, wait_ms to finish
/// and replaces the player's map when the records, the SHA-256 and Rover's
/// sector reader all agree.
///
/// Sleeps, and does not pump: the caller offers any records first. Not for a
/// Rover callback. A timeout, an error, a record out of place or a failed
/// check writes nothing.
///
/// @param wait_ms bound on the wait, in milliseconds; negative waits not at all
void rover_session_finish_sector_fetch(int32_t wait_ms);

/// Tells whether a confirmed login is still on the link.
///
/// True from the login confirmation until logout finishes or the open link
/// drops. False for a client that is only activated, a login that has not
/// been confirmed, a finished logout and a dropped link. Null is false.
///
/// @param session session to read; null is not live
/// @return whether the session is live
bool rover_session_live(const RoverSession* session);

/// Returns the location text of the server list's selected entry.
///
/// When Rover names none and the configured list is one written for a host,
/// it is that list's only entry.
///
/// @return the location text, or "" when none is selected
const char* rover_session_selected_server();

} // namespace oa::boneyards
