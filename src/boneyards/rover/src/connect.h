// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The connect and login sequence behind slot 7. Each step arms the handler
// for the next message with the step that follows and reports its stage to
// the game's progress callback as (stage, result, a, b, message); result 0
// is success:
//
//   0  a different server was chosen: its yard master connects once the
//      current link has closed
//   1  resolving the yard master (name1.domain) and connecting on port 9110
//   2  yard master connect started           3  yard master connected
//      (then get_servers; the reply ends with server_list "last", and the
//      client asks the yard master to "connect" it to a boneyard)
//   4  connect_to received; the boneyard at its ip and port is dialled
//   5  boneyard connect started              6  boneyard connected
//      (then connect_type; the boneyard answers connect_info with a seed)
//   7  login sent with MD5(password . lower(username) . seed)
//   8  login_confirmed: result, user id, home area
//
// entered_room result 2 moves the session to the other boneyard, which the
// reply names with its ip, port and a magic: that boneyard's link connects,
// asks for a client link and sends relogin with the user id and the magic.
// Once relogin_confirmed has arrived and the old boneyard's link has closed,
// in either order (the client closes it after the confirmation), the other
// boneyard becomes the session's and the room is entered there. The game's
// entered-room callback hears the result of that enter, or 1 when the move
// fails.
#pragma once

#include "lobby_handlers.h"
#include "record.h"

#include <stdint.h>

/// Resolves the chosen server's yard master (cluster member 1) and has the link thread connect to
/// it on port 9110, reporting stage 1.
///
/// Arms the yard master's close and connect handlers first. An unknown server or host leaves the
/// address empty and the connect fails.
void rover_connect_yard_master();

/// Queues the yard master "connect" and arms connect_to to wait for its answer.
void rover_request_boneyard();

/// Starts a move to the boneyard an entered_room reply names by ip, port and magic.
///
/// The room and the game's callback are kept, the other boneyard's link is dialled, and its connect
/// and close handlers are armed. An address of more than 15 characters is cut to 15, the most the
/// link's address field holds.
///
/// @param handler entered_room handler whose room, password and callback the move keeps
/// @param entry the entered_room reply
/// @return false when the reply lacks ip, port or magic or a move is already running, true once the
///     move starts
bool rover_move_to_other_boneyard(const RoverEnteredRoomHandler& handler, const RoverEntry& entry);
