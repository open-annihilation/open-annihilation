// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Rover session block, reached through one global pointer: the three
// lobby links, the queues between the game thread and the link thread, the
// server list, the handlers and the login in progress. Slot 1 creates it and
// slot 2 frees it.
#pragma once

#include "handlers.h"
#include "http_fetch.h"
#include "oa/rover/interface.h"
#include "oa/rover/platform.h"
#include "platform_select.h"
#include "record.h"

#include <atomic>
#include <stdint.h>

// The links, in the order of RoverState.links. The yard master answers first
// and redirects the client to a boneyard.
enum class RoverLinkId : int32_t {
    Boneyard1 = 0,
    Boneyard2 = 1,
    YardMaster = 2,
};
constexpr int32_t kRoverLinkCount = 3;

// Where a lobby link is in its connection.
enum class RoverLinkState : int32_t {
    Idle = 0,
    Connecting = 1,
    Connected = 2,
    Failed = 3,
    Closed = 4,
};

// Records passed between threads: a manual-reset event is set while any wait.
struct RoverEventQueue {
    RoverEvent* ready{};
    RoverRecord* first{};
    RoverRecord* last{};
    int32_t count{};
    RoverLock* lock{};
};

// One lobby link.
struct RoverConnection {
    RoverEvent* socket_event{}; // set by the select thread
    RoverSocket socket{OA_ROVER_SOCKET_INVALID};
    RoverLock* lock{};
    RoverLinkState state{};
    bool connecting{};             // from the start of a connect until it succeeds
    uint32_t connect_started_ms{}; // clock when this connect started; the yard master times out
    bool send_waiting{};           // a record is part sent and waits for the socket
    int32_t last_error{};          // -1 after the peer closed
    char address[16]{};            // dotted quad
    uint16_t port{};
    char send_buffer[kRoverRecordTextLimit + 1]{}; // a record's text and the NUL written after it
    char receive_buffer[kRoverRecordTextLimit]{};
    uint32_t send_counter{}; // cipher position
    uint32_t receive_counter{};
    int32_t send_left{};
    char* send_cursor{};
};

// One saved server (0x21b bytes); the game reads entries through slots 70-72.
#pragma pack(push, 1)

struct RoverServerEntry {
    char name[0xff]{};
    char domain[0xff]{};
    int32_t cluster_size{}; // hosts name1.domain .. nameN.domain; 0 for name.domain
    char location[0x17]{};  // what the game lists and slot 7 names
    bool persistent{};      // 'P' in the file; kept when the yard master sends its list
    bool selected{};        // 'D' in the file
};

#pragma pack(pop)
static_assert(sizeof(RoverServerEntry) == 0x21b, "server entry size");

struct RoverServerList {
    RoverServerEntry* entries{};
    // Parallel to entries, kRoverServerRegionCapacity bytes each. Empty when
    // the server file or the yard master named no region. Not part of the
    // 0x21b entry the game reads through slots 70-72.
    char* regions{};
    int32_t count{};
    int32_t capacity{};
};

// A move of the session to the other boneyard, which an entered_room reply
// with result 2 names: that boneyard's link logs in again with the magic and
// the room is entered there. Address, port and stage are written, never read.
struct RoverBoneyardMove {
    char address[0x100]{};
    int32_t port{};
    uint32_t magic{};  // sent with relogin
    int32_t room_id{}; // the room entered again
    int32_t area_id{};
    int32_t stage{};  // numbered as the login stages
    bool half_done{}; // relogin_confirmed or the old link's close seen
    bool active{};
    char password[0x10]{};
    RoverRoomEnteredFn entered{}; // told 1 when the move fails
};

// The banner host from the last ad_server record. Active only while send_id is 1 and the url is set.
struct RoverAdServer {
    char url[0x104]{};
    char params[0x104]{};
    int32_t send_id{};
    bool active{};
};

struct RoverState {
    RoverEventQueue outbound[kRoverLinkCount]{}; // sent on the link
    RoverEventQueue inbound{};                   // drained by slot 5
    RoverEvent* stop_event{};
    RoverEvent* connect_event[kRoverLinkCount]{};
    RoverConnection links[kRoverLinkCount]{};
    RoverServerList servers{};
    RoverFetchQueue fetches{}; // served by the fetch thread
    RoverEvent* fetch_stop_event{};
    RoverFileServer file_servers[2]{}; // news and ladder
    RoverAdServer ad_server{};         // banner host; inactive until ad_server names one
    RoverHandlerSet handlers{};
    char server_name[0x18]{};              // a server entry's location
    uint16_t reserved_after_server_name{}; // slot 7 stores 1; nothing reads it
    char username[0x10]{};
    char password[0x10]{};
    RoverLoginProgressFn progress{};
    RoverSystemAlertFn system_alert{};
    RoverDisconnectFn disconnect{};
    int32_t login_stage{}; // last stage reported
    bool login_started{};
    RoverBoneyardMove move{};
    char resolved_server[0x17]{}; // location of the server whose yard master answered
    // Name and domain of the server this login dialled. The yard master's list marks that server
    // selected even when its location text differs from the one the login named.
    char dialled_name[sizeof(RoverServerEntry::name)]{};
    char dialled_domain[sizeof(RoverServerEntry::domain)]{};
    uint32_t latency_reply_flags{}; // sent with the next latency reply
    uint32_t user_id{};             // from login_confirmed
    std::atomic<int32_t> worker_count{};
    bool stopping{};        // set by slot 4 and never cleared
    bool second_boneyard{}; // selects boneyard link 2
};

// The session block; null while detached.
extern RoverState* g_rover_state;

/// Returns one of the session's links; shared by the slots and the connect steps.
///
/// @param state session block
/// @param id link to return
/// @return the link
RoverConnection& rover_link(RoverState& state, RoverLinkId id);

/// Returns the boneyard link, and so the outbound queue, the session is using.
///
/// @param state session block
/// @return Boneyard2 after a move to the second boneyard, else Boneyard1
RoverLinkId rover_active_boneyard(const RoverState& state);

/// Creates a queue's lock; its event is created with the session's other events.
///
/// @param[out] queue queue to set up
/// @return false when the lock could not be created
bool rover_queue_init(RoverEventQueue& queue);

/// Destroys every queued record and frees the queue's lock and event.
///
/// @param[in,out] queue queue to tear down
void rover_queue_teardown(RoverEventQueue& queue);

/// Appends a record; the first one waiting sets the queue's event.
///
/// @param[in,out] queue queue to append to
/// @param record record to queue; the queue takes ownership
void rover_queue_push(RoverEventQueue& queue, RoverRecord* record);

/// Takes the oldest record; taking the last resets the queue's event.
///
/// @param[in,out] queue queue to take from
/// @return the record, which the caller now owns, or null when the queue is empty
RoverRecord* rover_queue_pop(RoverEventQueue& queue);

/// Copies at most capacity - 1 bytes and terminates.
///
/// @param[out] destination buffer of capacity bytes
/// @param source text to copy, or null
/// @param capacity size of destination in bytes
/// @quirk A null source leaves the destination as it was.
void rover_copy_bounded(char* destination, const char* source, int32_t capacity);
