// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Socket readiness for the lobby links. The links keep to Winsock 1.1, which
// has no event selection, so a "select thread" watches up to eight
// registered sockets and turns readiness into per-socket event counts and a
// signalled event, as WSAEventSelect would: the link thread waits on that
// event and collects the counts.
#pragma once

#include "oa/rover/platform.h"

#include <stdint.h>

// Network events a socket is registered for, as Winsock numbers them.
enum class RoverNetEvent : int32_t {
    Read = 0,
    Write = 1,
    OutOfBand = 2,
    Accept = 3,
    Connect = 4,
    Close = 5,
};
constexpr int32_t kRoverNetEventCount = 8;

/// Returns the mask bit of an event.
///
/// @param event event to convert
/// @return 1 shifted by the event's Winsock number
constexpr uint32_t rover_net_event_bit(RoverNetEvent event) {
    return 1u << static_cast<int32_t>(event);
}

// What the link thread collects for one socket.
struct RoverNetEvents {
    uint32_t signalled{}; // rover_net_event_bit of each event raised since the last collect
    int32_t errors[kRoverNetEventCount]{}; // Winsock error per event, 0 on success
};

/// Creates the lock that guards the socket table, once per process.
///
/// @return false when the lock cannot be created
bool rover_sockets_init();

/// Registers socket for the events in mask, updates its mask, or with mask 0 removes it.
///
/// Starts the select thread on first use.
///
/// @param socket socket to watch
/// @param notify event to signal when an event is raised; required for a new registration
/// @param mask rover_net_event_bit of each wanted event, or 0 to remove the socket
/// @return 0, or -1 when the table is full, notify is null for a new socket, or the thread cannot
///     start
int32_t rover_sockets_register(RoverSocket socket, RoverEvent* notify, uint32_t mask);

/// Collects and clears the events raised for socket.
///
/// Each raised event is taken back once; the link's event is reset when none is left.
///
/// @param socket registered socket
/// @param[out] out_events receives the raised events and each event's error
/// @return 0, or -1 when the socket is not registered
int32_t rover_sockets_collect(RoverSocket socket, RoverNetEvents* out_events);

/// Forgets a socket's read events after a receive found nothing to read.
///
/// @param socket registered socket; an unknown one is ignored
void rover_sockets_clear_read(RoverSocket socket);

/// Marks a socket's connection broken and raises its events.
///
/// @param socket registered socket; an unknown one is ignored
void rover_sockets_fail(RoverSocket socket);

/// Drops a socket from the table.
///
/// @param socket registered socket; an unknown one is ignored
void rover_sockets_remove(RoverSocket socket);

/// Counts a send starting on a socket registered for write events; while one is unfinished the
/// select thread watches for room to write.
///
/// @param socket socket the send is on
void rover_sockets_begin_send(RoverSocket socket);

/// Counts a send finished on a socket registered for write events.
///
/// @param socket socket the send was on
void rover_sockets_end_send(RoverSocket socket);

/// Stops the select thread, waits for it and frees the table.
void rover_sockets_shutdown();
