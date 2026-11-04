// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Recorded Rover interface for offline checks. This is test scaffolding: it
// stands in for a Central Command session by replaying recorded replies
// through oa-rover's own table.
//
// The table is a copy of rover_get_interface's with the requests a live
// session answers replaced: the login progress, the dossier and profiles,
// the war-map feeds, the locator, the chat rooms (list, entry, switch,
// leave, say, creation) and pages, slot 72's server name, the logout and the
// session reset, the content servers (slot 40) and downloads (slot 32), the
// battle list (enter_battles, leave_battles and get_battle), and the battle
// search (slot 66). Attach,
// activate, the sector-map slots and the property-bag slots stay oa-rover's,
// so the war map is read from the install through Rover's placed files and
// the dossier, the profiles, the locator's fields and a page's fields are
// real Rover bags. Hosting and joining a battle (slots 21 and 23) and the
// battle's reports go to oa-rover too, which queues their
// records for the Boneyards; the replay takes each such record at the next
// pump and keeps its text among the requests it received, and answers a host
// or join request with the file's next launch record, handed to oa-rover as
// the Boneyards sends it. Replies are delivered from slot 5 (the pump) once
// they are due, before oa-rover's own pump runs. Every reply waits the
// delay in force at its line, counted from the request (or the answer) it
// follows.
//
// As Rover does, the replay keeps the callbacks each request passes: the
// entered room's member, chat and room-list feeds (armed by enter_room at
// once, by switch_room once the new room is entered, dropped by
// leave_room), one entry, one leave, one creation and one profile awaited at
// a time (a new request drops the answer still queued for the last), and the
// login's system alert callback for alerts and pages. Members, lines and
// echoes reach only the entered room; room-list updates reach its area.
//
// A replay file holds one entry per line; blank lines and lines starting
// with '#' are ignored:
//   delay N          pumps between a request and the replies on the lines
//                    that follow (1 until set)
//   progress STAGE RESULT DETAIL_A DETAIL_B [MESSAGE]
//                    a login progress report, queued by start_login; the
//                    confirmation (stage 8, result 0) names the user id
//                    echoes come from
//   alert KIND TEXT[|NOTIFICATION_TYPE]
//                    a system alert of KIND (0 ticker, 1 message box, 2
//                    page, 3 notification) whose bag holds the command and,
//                    when given, notification_type (string); start_login
//                    queues it after the progress reports, or when it
//                    follows an entered or created answer in the file, that
//                    answer does, as it does its updates
//   page USER_ID TEXT
//                    alert 2 from USER_ID, the bag also holding user_id
//                    (uint)
//   dossier KEY int VALUE
//   dossier KEY string TEXT
//                    a dossier field; request_get_profile for the user id
//                    the login confirmed answers, when no profile lines name
//                    it, with a bag of every field and user_id (uint)
//   profile USER_ID KEY int VALUE
//   profile USER_ID KEY string TEXT
//   ladder_profile USER_ID LADDER_ID SUBID KEY int VALUE
//   ladder_profile USER_ID LADDER_ID SUBID KEY string TEXT
//                    a field of USER_ID's profile, or of its ladder standing
//                    LADDER_ID/SUBID; request_get_profile (slot 63) for
//                    USER_ID answers, after the delay in force at the first
//                    line naming USER_ID, with each ladder standing's bag in
//                    order of its first line, then the profile's, every bag
//                    holding user_id (uint); another user gets no answer
//   area_name NAME   the server name the area-name request (slot 72) returns
//   allegiances LETTERS
//                    request_allegiance's answer
//   census AREA POPULATION ACTIVITY IN_PROGRESS FORMING CORE_WINS ARM_WINS
//                    request_census(0) answers with every census line, then
//                    the activity line; request_census(AREA) with AREA's
//   activity DIGITS
//   located NAME CODE USER_ID ICON
//                    the locator's answer (slot 65) about NAME, asked in any
//                    case: where the user is, the id and the icon
//   located_field KEY int|uint VALUE
//   located_field KEY string TEXT
//                    a field of the last located answer's bag
//   room AREA ROOM USERS TYPE NEEDS_PW [global] NAME[|DESCRIPTION[|TAG]]
//                    a row of AREA's room list (NEEDS_PW 1 when a password
//                    is needed; the word global, when present, is the
//                    sector-wide frequency; the name may hold spaces, the
//                    fields are split at '|'); get_rooms (slot 9) answers
//                    with AREA's rows, the last marked as ending the list
//   entered AREA ROOM RESULT
//                    the answer to entering (slot 10) or switching to (slot
//                    11) ROOM (0 entered); once entered, the room's members
//                    and lines follow, then the updates after this line
//   member AREA ROOM USER_ID LATENCY ICON RANK ALLEGIANCE NAME
//   said AREA ROOM USER_ID TEXT
//                    a member of ROOM and a line said there (USER_ID 0: a
//                    line from the server)
//   update CHANGE AREA ROOM USERS TYPE NEEDS_PW NAME[|DESCRIPTION[|TAG]]
//                    a room-list row of AREA through the entered room's
//                    room-list feed (CHANGE 0 adds or refreshes, 1 removes),
//                    delivered after the entered or created answer it
//                    follows in the file
//   left AREA ROOM RESULT
//                    the answer to leaving ROOM (slot 12; 0 left)
//   created AREA ROOM NAME
//                    the answer to creating the frequency NAME (slot 69),
//                    asked in any case: AREA or ROOM 0 refuses it; a
//                    frequency made brings the updates after this line
//   echo on|off      whether a line said to the whole room (slot 13) comes
//                    back to it from the confirmed user id (off until set;
//                    the echo waits the delay in force at this line)
//   file_server news|ladder HOST PORT
//                    a file_server record naming the news or ladder content
//                    server, queued by start_login after the pages; slot 40
//                    answers with it once delivered (Rover's invalid record
//                    until then)
//   http URL LOCALFILE
//                    a download: fetch_http_file (slot 32) of exactly URL
//                    copies LOCALFILE (relative to the replay's directory)
//                    over the destination, then completes with status 0;
//                    any other URL completes after one pump with the
//                    destination untouched, as a failed fetch does
//   battle AREA GAME_ID TYPE FORMING HOST [pass] [seats N M] NAME
//                    a row of AREA's battle list (FORMING 1 while the battle
//                    forms, 0 once it is in progress; HOST is A, C or U; pass
//                    means the battle needs a password; seats N M are the
//                    seats taken and the seat limit; the name may hold
//                    spaces). enter_battles answers with AREA's
//                    rows and then that the list is entered; an area with no
//                    rows still completes, and a new enter drops the list
//                    still queued for the last. leave_battles answers that
//                    the list was left. get_battle answers after one pump
//                    with the battle's intel: it succeeded, the battle has no
//                    password, and no host or mission is named. The
//                    battle search (slot 66) answers with every battle
//                    whose name holds the text, in file order, the last
//                    marked as ending the search, or with one closing row
//                    with no game id when none does
//   launch RECORD    the launch_game record ({[...]}) that answers a host
//                    or join request: the first such request brings the
//                    first launch line, the next the next; oa-rover takes it
//                    as it takes the Boneyards' records
//   record RECORD    another record oa-rover receives, delivered after the
//                    launch line it follows (a debriefing, say)
//   reply RECORD     an inbound record answering the next outbound whose
//                    text contains command=matchmaking_ or command=quickplay_.
//                    The delay in force applies. A delay of 0 is delivered
//                    in that same pump
//   then RECORD      another inbound record delivered with the reply it
//                    follows, in the same answer; it is an error unless a
//                    reply (or another then) came before it
#pragma once

#include "oa/boneyards/service.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::boneyards {

/// Reads a replay file and makes its table the Rover table source.
///
/// @param path replay file
/// @param[out] error message naming the file and line when the file is refused
/// @param error_capacity size of `error` in bytes
/// @return false when the file cannot be read or parsed; nothing is installed then
bool rover_replay_install(const char* path, char* error, std::size_t error_capacity);

/// Restores oa-rover's table as the source and drops the replay.
void rover_replay_uninstall();

/// Returns the installed replay's census line for an area, as request_census delivers it.
///
/// @param area_id area looked up
/// @param[out] out census of the area
/// @return false when no replay is installed or it has no line for the area
bool rover_replay_census(uint32_t area_id, CensusReport* out);

/// Counts the rows of an area's room list in the installed replay.
///
/// @param area_id area counted
/// @return number of rows; 0 when no replay is installed
std::size_t rover_replay_room_count(uint32_t area_id);

// The requests of the consoles a Central Command session would carry out.
enum class ReplayRequest : uint8_t {
    say,         // send_chat (slot 13)
    page,        // send_page_by_name (slot 41)
    switch_room, // slot 11
    leave_room,  // slot 12, for a room other than 0
    create_room, // slot 69
    fetch,       // fetch_http_file (slot 32): the URL in text, the file in path
    record,      // a record oa-rover queued for the Boneyards: its text
};

inline constexpr std::size_t kReplaySentNameCapacity = 0x40;
// Holds a Rover record's text and its terminator.
inline constexpr std::size_t kReplaySentTextCapacity = 0x401;

// One request with its arguments; the fields a request lacks are 0 or "".
// The area and room are the say's, the room switched to, the room left or
// the area of a creation; the old ones the room a switch leaves once the
// new one is entered. The name is the user paged or the frequency created;
// the text the message said or paged, the frequency's description, or a
// record's text.
struct ReplaySent {
    ReplayRequest request;
    uint32_t area_id;
    uint32_t room_id;
    uint32_t old_area_id;
    uint32_t old_room_id;
    uint32_t target_user_id; // say: 0 for the whole room
    char name[kReplaySentNameCapacity];
    char text[kReplaySentTextCapacity];
    char password[kReplaySentNameCapacity]; // switch_room, create_room
    char path[kReplaySentTextCapacity];     // fetch: the destination file
};

/// Counts the requests the installed replay received since it was installed.
///
/// @return number of requests
std::size_t rover_replay_sent_count();

/// Returns one request the installed replay received, in order of arrival.
///
/// @param index request index, from 0
/// @param[out] out request and its arguments
/// @return false past the last request
bool rover_replay_sent(std::size_t index, ReplaySent* out);

// What the installed replay's room, member and said lines record: row
// `index` of `area_id`'s room list, and member or line `index` of one of its
// rooms; each false past the last or without a replay.
struct ReplayRoom {
    uint32_t room_id;
    int32_t users;
    int32_t type;
    bool needs_password;
    char name[kReplaySentNameCapacity];
};

struct ReplayMember {
    uint32_t user_id;
    char name[kReplaySentNameCapacity];
};

struct ReplaySaid {
    uint32_t user_id; // 0: a line from the server
    char text[kReplaySentTextCapacity];
};

// The LOGINNAME and CLAN texts of a user's recorded profile ("" for one it
// lacks); false without a replay or when it records no profile for the user.
struct ReplayProfile {
    char name[kReplaySentNameCapacity];
    char clan[kReplaySentNameCapacity];
};

/// Returns the LOGINNAME and CLAN texts of a user's recorded profile.
///
/// @param user_id user looked up
/// @param[out] out the two texts ("" for one the profile lacks)
/// @return false without a replay or when it records no profile for the user
bool rover_replay_profile(uint32_t user_id, ReplayProfile* out);

/// Returns one row of an area's room list in the installed replay.
///
/// @param area_id area of the list
/// @param index row index, from 0
/// @param[out] out the row
/// @return false past the last row or without a replay
bool rover_replay_room(uint32_t area_id, std::size_t index, ReplayRoom* out);

/// Returns one recorded member of a room in the installed replay.
///
/// @param area_id area of the room
/// @param room_id room
/// @param index member index, from 0
/// @param[out] out the member
/// @return false past the last member or without a replay
bool rover_replay_member(uint32_t area_id, uint32_t room_id, std::size_t index, ReplayMember* out);

/// Returns one recorded line said in a room in the installed replay.
///
/// @param area_id area of the room
/// @param room_id room
/// @param index line index, from 0
/// @param[out] out the line
/// @return false past the last line or without a replay
bool rover_replay_said(uint32_t area_id, uint32_t room_id, std::size_t index, ReplaySaid* out);

// Alert or page line `index` of the installed replay; false past the last
// or without a replay.
struct ReplayAlert {
    int32_t kind;
    uint32_t user_id; // a page's sender
    // The frequency whose entry or creation brings it; both 0 when the login
    // does.
    uint32_t area_id;
    uint32_t room_id;
    char text[kReplaySentTextCapacity];
    char notification_type[kReplaySentNameCapacity];
};

/// Returns one alert or page line of the installed replay.
///
/// @param index alert index, from 0
/// @param[out] out the alert, with the frequency whose entry or creation brings it (both 0 when the
///     login does)
/// @return false past the last alert or without a replay
bool rover_replay_alert(std::size_t index, ReplayAlert* out);

} // namespace oa::boneyards
