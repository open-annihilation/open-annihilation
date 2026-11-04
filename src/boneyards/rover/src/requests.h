// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Requests the client sends. Each builder appends an entry addressed to the
// service that owns the command (to record, or to a new record when null)
// and returns the record; the caller queues it on a link.
#pragma once

#include "record.h"

#include <stdint.h>

// The services a request can address, as 3.1c's service table numbers them.
enum class RoverService : int32_t {
    Dispatcher = 0,
    CommManager = 1,
    Administrator = 2,
    Rover = 3,
    ChatManager = 4,
    UserManager = 5,
    GameManager = 6,
    YardMonitor = 7,
    YardMaster = 8,
    Guarddog = 9,
    WarManager = 10,
    ExtensionDll = 11,
    Retriever = 12,
    GuarddogProxy = 13,
    Crusades = 14,
    HouseManager = 15,
};

// Role and stream encoding the client asks for in connect_type.
enum class RoverConnectRole : int32_t {
    Unknown = -1,
    Client = 0,
    Peer = 1,
};
enum class RoverCompression : int32_t {
    Text = 0,
    Compressed = 1,
    Encrypted = 2,
};

// login's type: a registered login carries the password digest.
enum class RoverLoginKind : int32_t {
    Registered = 0,
    Anonymous = 1,
};

/// Returns the name of a service in the request table.
///
/// @param service service to name
/// @return the service's name, or null outside the table
const char* rover_service_name(RoverService service);

/// Adds an entry at the back of *record, addressed to service and carrying command.
///
/// @param[in,out] record record to append to; created when *record is null
/// @param service service the entry is addressed to
/// @param command the entry's command field
/// @return the entry, or null when out of memory
RoverEntry* rover_add_request(RoverRecord** record, RoverService service, const char* command);

/// Appends connect_type to the comm manager: protocol version 0.01, role and stream encoding.
///
/// @param role role to ask for; Unknown is sent as "unknown"
/// @param compression stream encoding to ask for
/// @param record record to append to, or null to start a new one
/// @return the record, or null when a new one could not be allocated
RoverRecord* rover_request_connect_type(
    RoverConnectRole role, RoverCompression compression, RoverRecord* record
);

/// Writes the username bytes a login digest hashes.
///
/// They are the username as the current wire encoding writes it, ASCII letters folded to lower
/// case, at most 15 bytes. A registered login hashes those bytes between the password and the seed.
///
/// @param[out] out buffer; always terminated when capacity is positive
/// @param capacity size of out in bytes
/// @param username user name in the interface's text, or null
void rover_login_digest_username(char* out, int32_t capacity, const char* username);

/// Appends login to the user manager; the digest goes only with a registered login.
///
/// @param kind registered or anonymous
/// @param username user to log in
/// @param digest password digest, sent when registered and not empty
/// @param record record to append to, or null to start a new one
/// @return the record, or null when a new one could not be allocated
RoverRecord* rover_request_login(
    RoverLoginKind kind, const char* username, const char* digest, RoverRecord* record
);

/// Appends relogin to the user manager: the user id login_confirmed gave and the magic of the
/// boneyard the session moves to.
///
/// @param user_id the user's id
/// @param magic magic the entered_room reply named
/// @param record record to append to, or null to start a new one
/// @return the record, or null when a new one could not be allocated
RoverRecord* rover_request_relogin(uint32_t user_id, uint32_t magic, RoverRecord* record);

/// Appends logout to the user manager.
///
/// @param record record to append to, or null to start a new one
/// @return the record, or null when a new one could not be allocated
RoverRecord* rover_request_logout(RoverRecord* record);

/// Appends get_servers to the yard master.
///
/// @param record record to append to, or null to start a new one
/// @return the record, or null when a new one could not be allocated
RoverRecord* rover_request_get_servers(RoverRecord* record);

/// Appends latency_r to the comm manager: the probe's times echoed and a sequence value of three
/// draws of rover_rand with the flags spread through it.
///
/// Flags 2, 3, 4 and 6 are inverted as they are spread over the bits the random value leaves free.
///
/// @param seconds the probe's sec value
/// @param microseconds the probe's usec value
/// @param flags latency flags the game set through slot 43, bits 0-7
/// @param record record to append to, or null to start a new one
/// @return the record, or null when a new one could not be allocated
RoverRecord* rover_request_latency_reply(
    uint32_t seconds, uint32_t microseconds, uint32_t flags, RoverRecord* record
);

/// Builds the yard master's "connect", sent to reach a boneyard through it.
///
/// @return a new record, or null when out of memory
RoverRecord* rover_request_yard_master_connect();

/// Returns the next value of the per-thread random sequence 3.1c's Rover draws from.
///
/// Each thread's sequence starts from seed 1 and steps x = x * 214013 + 2531011, returning
/// bits 16-30 of the new state.
///
/// @return a value from 0 to 0x7fff
int32_t rover_rand();
