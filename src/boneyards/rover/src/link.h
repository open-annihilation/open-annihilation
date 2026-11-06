// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The lobby links: one TCP connection each, every byte enciphered with the
// rolling counter, records framed by their braces. The link thread opens
// connections on request, sends queued records and turns received text into
// records for the game-thread pump, together with its own "Rover" records
// announcing each connect and close.
#pragma once

#include "state.h"

// Result of rover_link_send.
enum class RoverSendResult : int32_t {
    Failed = 0,
    Sent = 1,
    WouldBlock = 2, // the rest goes out when the socket has room
};

/// Initialises a link object and creates its lock.
///
/// @param[out] link link to initialise
/// @return false when the lock could not be created
bool rover_link_init(RoverConnection& link);

/// Closes the link's socket and frees its lock and event.
///
/// @param[in,out] link link to tear down; one without a lock is left alone
void rover_link_teardown(RoverConnection& link);

/// Creates the event the select thread signals for the link.
///
/// @param[in,out] link link receiving the event
/// @return false when the event could not be created
bool rover_link_create_event(RoverConnection& link);

/// Shuts down the sending side of a connecting or connected link; the peer then closes it.
///
/// @param[in,out] link link to stop
void rover_link_stop(RoverConnection& link);

/// Marks a link as still connecting and tells the pump it closed, without opening a socket.
///
/// A yard master whose name does not resolve takes this path, so the game hears the same close
/// it hears when a connect is refused.
///
/// @param[in,out] state session block whose inbound queue receives the close
/// @param[in,out] link link that never opened
/// @param error Winsock-numbered error stored when the link has none yet
void rover_link_fail_pending(RoverState& state, RoverConnection& link, int32_t error);

/// Enciphers bytes with the link's send counter, advancing it per byte.
///
/// @param[in,out] link link whose send counter is used
/// @param[in,out] bytes bytes enciphered in place
/// @param length number of bytes; nothing happens below 1
void rover_link_encrypt_buffer(RoverConnection& link, char* bytes, int32_t length);

/// Deciphers bytes with the link's receive counter, advancing it per byte.
///
/// @param[in,out] link link whose receive counter is used
/// @param[in,out] bytes bytes deciphered in place
/// @param length number of bytes; nothing happens below 1
void rover_link_decrypt_buffer(RoverConnection& link, char* bytes, int32_t length);

/// Creates a link's socket event: manual reset, initially clear, unnamed.
///
/// @return the event, or null when it could not be created
RoverEvent* rover_link_new_socket_event();

/// Drops a socket whose peer closed from the select table.
///
/// @param socket socket to drop
void rover_link_remove_socket_dispatch(RoverSocket socket);

/// Fails a broken socket in the select table.
///
/// @param socket socket to fail
void rover_link_set_socket_failed_dispatch(RoverSocket socket);

/// Counts a send finished on the socket.
///
/// @param socket socket the send was on
void rover_link_end_send_shim(RoverSocket socket);

/// Counts a send starting on the socket.
///
/// @param socket socket the send is on
void rover_link_begin_send_shim(RoverSocket socket);

/// Runs the link thread: serves connect requests, socket events and outbound queues until the stop
/// event is set.
///
/// On stop, closes every open socket, lowers the session's worker count and resets the stop event.
///
/// @param context thread context; unused
void rover_run_link_thread(void* context);
