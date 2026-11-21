// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The multiplayer lobby's network boundary (LobbyNet) bound to the
// DirectPlay session over sockets: TCP/IP provider, session enumeration,
// host/join with TA's CreatePlayer join block, the host's join verdict on
// DirectPlay create-player messages, and lobby records carried through the
// packet layer (frames + condenser) as the game sends them: queued, each
// from the player it concerns, and flushed at the battle room's set points.
// The same connection then carries the match (see net_match.hpp).

#include "oa/core/game_state.h"
#include "oa/netgame/match/packet_layer.hpp"
#include "oa/netgame/session.hpp"
#include "oa/netgame/socket_host.hpp"
#include "oa/netgame/wire_rules.hpp"
#include "oa/ui/frontend_multiplayer/lobby_net.hpp"

namespace oa::netgame::match {

// The live connection shared by the lobby and the match.
struct NetConnection {
    sock::Host* host{};     // heap
    PacketLayer* packets{}; // heap
    /// The socket configuration every LobbyNet::open starts from; a blank
    /// address enumerates at its enum_target.
    sock::HostConfig host_config{};
    /// Application GUID every session of this connection hosts under and
    /// searches for. net_connection_create fills it from the configuration,
    /// or with Total Annihilation's own GUID when none was named.
    dplay::Guid application{};
    session::Session session{};
    bool opened{};
    /// Opened with an address that resolved to nothing; enumeration fails.
    bool address_unresolved{};
    bool in_session{};
    bool hosting{};
    int32_t sends_per_second{}; // configured frame rate (0 = the queue's 200 ms)
    uint32_t local_id{};
    char join_tag[0x11]{};   // host: password joiners must present (Game.password)
    uint16_t host_options{}; // host: last published option word (PlayerSetupInfo.options)
    // Players already in the session when this machine joined, announced to
    // the lobby like the game's player enumeration after a join.
    uint32_t roster[dplay::max_players]{};
    uint32_t roster_count{};
    // Extra servicing while the session waits (tests running two machines
    // in one thread service the other machine here).
    void* pump_context{};
    void (*pump_other)(void* context, uint32_t wait_ms){};
    /// Economy periods run on this connection since it was created. Games
    /// played on it one after another continue the count, as 3.1c's count
    /// runs for the whole program.
    uint32_t economy_periods{};
    /// Tells whether a launch is active; a host with a launch active admits
    /// joiners into a closed game. Null answers false.
    void* launch_context{};
    bool (*launch_active)(void* context){};
    /// The network rules every session of the connection plays by
    /// (net_connection_set_rules).
    WireRules rules{};
};

struct SessionLobbyConfig {
    sock::HostConfig host{};    // ports, bind and enumeration addresses
    int32_t sends_per_second{}; // frame pacing (0 = 200 ms, 2..30 per second)
    /// When set, `application` is the GUID to host under and to search for,
    /// including an all-zero GUID. When clear, that GUID is Total
    /// Annihilation's own.
    bool application_named{};
    dplay::Guid application{};
};

/// Allocates the connection's socket host and packet queue; no socket is opened until LobbyNet::open.
///
/// @param[out] connection Connection to create; on failure it is left destroyed.
/// @param config Socket host configuration, frame pacing and application GUID.
/// @return False when either allocation fails.
[[nodiscard]] bool
net_connection_create(NetConnection* connection, const SessionLobbyConfig& config) noexcept;

/// Closes an open connection and frees its socket host and packet queue.
///
/// @param[in,out] connection Connection to destroy; safe to call twice.
void net_connection_destroy(NetConnection* connection) noexcept;

/// Returns the LobbyNet table over a connection made by net_connection_create.
///
/// open takes the TCP/IP address the player typed, without the white space
/// around it. A blank address enumerates at the configured target, which by
/// default searches every local IPv4 network and this machine. Dotted IPv4
/// is used as it is and anything else is resolved as a host name. Only an
/// address that resolves to nothing makes enumerate fail (open still
/// succeeds); a request that cannot be sent anywhere lists no games, so, as
/// in 3.1c, a blank address always reaches the game list.
///
/// @param connection Connection passed back to every entry as its context.
/// @return The table.
[[nodiscard]] ui::frontend_multiplayer::LobbyNet
session_lobby_net(NetConnection* connection) noexcept;

/// Sets the frame rate for every later session and applies it to the queue at once.
///
/// @param[in,out] connection Connection to pace.
/// @param sends_per_second 0 means one send per 200 ms; otherwise clamped to 2..30.
/// @return False, changing nothing, for a negative rate.
bool net_connection_set_packet_rate(NetConnection* connection, int32_t sends_per_second) noexcept;

/// Sets the network rules of the connection's sessions.
///
/// The receiver splits the recorder's records out of every frame when the
/// rules present the recorder, the wide weapon records when they raise the
/// weapon types and the route batch records when they say so
/// (WireRules::route_batches); the match copies the rules as it begins. A
/// published session's description keeps an empty password either way.
///
/// @param[in,out] connection Connection whose rules change.
/// @param rules The rules.
void net_connection_set_rules(NetConnection* connection, const WireRules& rules) noexcept;

/// Returns the transport clock in 1/30 s ticks (the game's time base at rate 30).
///
/// @param connection Connection whose socket host keeps the clock.
/// @return Milliseconds since the host started, times 30 / 1000; 0 without a host.
[[nodiscard]] uint32_t net_connection_time(const NetConnection* connection) noexcept;

/// Sends one record from the local player and pushes it out at once.
///
/// Does nothing outside a session or for an empty record.
///
/// @param[in,out] connection Connection to send on.
/// @param to_id Destination transport id; 0 broadcasts.
/// @param record Record bytes, type byte first.
/// @param size Record length in bytes.
void net_connection_send_now(
    NetConnection* connection, uint32_t to_id, const uint8_t* record, std::size_t size
) noexcept;

/// Queues one record from a player this machine created; net_connection_flush sends it.
///
/// A from id that names no player this machine created (the local player or
/// one of its computer players) sends from the local player instead. An
/// unguaranteed record goes out at once in a frame of its own, without
/// guaranteed delivery, after everything queued before it is flushed; the
/// delivery mode is then restored. Does nothing outside a session or for
/// an empty record.
///
/// @param[in,out] connection Connection to send on.
/// @param from_id Transport id of the sending player.
/// @param to_id Destination transport id; 0 broadcasts.
/// @param record Record bytes, type byte first.
/// @param size Record length in bytes.
/// @param unguaranteed Send it at once without guaranteed delivery.
void net_connection_send_from(
    NetConnection* connection,
    uint32_t from_id,
    uint32_t to_id,
    const uint8_t* record,
    std::size_t size,
    bool unguaranteed
) noexcept;

/// Sends every queued record now.
///
/// @param[in,out] connection Connection to flush; nothing happens outside a session.
void net_connection_flush(NetConnection* connection) noexcept;

/// Publishes the hosted session's description again.
///
/// The description keeps the full name last published, its flags (0x20,
/// once published, stays set) and its player limit, and takes `user` as its
/// user bytes, from which the host's options are read again. Does nothing
/// unless this machine hosts a session.
///
/// @param[in,out] connection Hosting connection.
/// @param user The 16 user bytes, as the battle room builds them from the
///        host's setup block; null keeps the last ones.
void net_connection_republish(NetConnection* connection, const uint8_t* user) noexcept;

/// Ends a multiplayer game: flushes every channel, releases the session and clears the live-game bit.
///
/// The session is always released. Does nothing unless the live-game bit is
/// set.
///
/// @param[in,out] connection Connection to close.
/// @param[in,out] game Game whose net_flags live-game bit is cleared.
void net_connection_finish(NetConnection* connection, Game* game) noexcept;

} // namespace oa::netgame::match
