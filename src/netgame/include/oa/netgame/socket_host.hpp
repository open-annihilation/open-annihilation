// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The system socket driver for the DirectPlay engine: a TCP listener and
// outbound connections for stream messages, one UDP socket for datagrams,
// and (while hosting) the UDP enumeration port. Everything is non-blocking;
// connection buffers are fixed-size and a peer that overflows them or sends
// a malformed stream header is disconnected.
//
// A datagram to 255.255.255.255 is a search of the local networks: it goes
// out as the directed broadcast of every local IPv4 network, which reaches
// a game hosted on this machine too. Only when no directed broadcast could
// be sent does it go to 255.255.255.255 itself and as a copy to this
// machine.
//
// Unless an advertised address is configured, every message header carries
// this machine's address on the way to its destination, as a DirectPlay
// sender writes it: the bind address when one is set, else the address the
// system routes the destination through, and on a search each network's
// own interface address.

#include "oa/netgame/dplay/engine.hpp"
#include "oa/netgame/session.hpp"

#include <cstdint>

namespace oa::netgame::sock {

using dplay::Address;

inline constexpr std::size_t max_connections = 16;
inline constexpr std::size_t connection_rx_bytes = dplay::max_message_bytes;
inline constexpr std::size_t connection_tx_bytes = 0x20000;
inline constexpr intptr_t invalid_socket = -1;

/// How long a closing host keeps its stream connections open after the last
/// bytes it sent on one: a 3.1c machine takes a departure (DeletePlayer) only
/// when it reads it before the connection ends, and it reads late.
inline constexpr uint32_t close_linger_ms = 1000;

/// The enumeration target that searches every local IPv4 network and this machine.
inline constexpr uint8_t local_networks_ip[4] = {255, 255, 255, 255};

/// Most local networks one search sends a directed broadcast to.
inline constexpr std::size_t max_search_targets = 16;

/// A clock a host reads in place of the steady clock, for checks that run
/// DirectPlay's timers in simulated time.
///
/// With advance set, a pump that finds nothing to read calls advance with
/// the time it was asked to wait, instead of waiting. It waits for real, at
/// most 1 ms, only while in_flight counts bytes one host has sent and another
/// has not yet read. Every host of one test shares one clock.
struct HostClock {
    void* context = nullptr;
    /// Returns the clock in milliseconds; null means the steady clock.
    uint32_t (*now_ms)(void* context) = nullptr;
    /// Moves the clock on by the given milliseconds; null means real waits.
    void (*advance)(void* context, uint32_t milliseconds) = nullptr;
    /// Bytes sent and not yet read between the hosts sharing the clock; null
    /// when the other side is not one of them.
    int64_t* in_flight = nullptr;
};

struct HostConfig {
    uint8_t bind_ip[4]{};       // 0.0.0.0 = all interfaces
    uint8_t advertised_ip[4]{}; // written into headers; 0 = this machine's, per destination
    uint16_t stream_port_first{dplay::stream_port_first}; // 0 = ephemeral
    uint16_t stream_port_last{dplay::stream_port_last};
    uint16_t datagram_port_first{dplay::datagram_port_first};
    uint16_t datagram_port_last{dplay::datagram_port_last};
    uint16_t enum_port{dplay::enum_port}; // 0 = ephemeral (tests)
    /// Where enumeration requests go; local_networks_ip searches every local
    /// network and this machine.
    uint8_t enum_target[4]{255, 255, 255, 255};
    uint32_t seed{};   // 0 = derive from the clock
    HostClock clock{}; // null members: the steady clock and real waits
};

/// One IPv4 address of a network interface of this machine, as the system lists it.
struct LocalInterface {
    uint8_t address[4]{};
    uint8_t netmask[4]{};
    uint8_t broadcast[4]{}; ///< 0.0.0.0 when the system names none
    bool up{};
    bool running{}; ///< the link is up; the same as up where the system does not report links
    bool broadcast_capable{};
    bool loopback{};
    bool point_to_point{};
};

/// The directed broadcast addresses one search sends its request to.
struct SearchTargets {
    uint8_t ip[max_search_targets][4]{};
    /// This machine's address on each target's network, which the request's
    /// header carries there.
    uint8_t source[max_search_targets][4]{};
    uint32_t count{};
};

/// Destinations whose local address a host remembers: enough for a
/// connection to every other machine and a search's every target, so that a
/// game's traffic finds each one remembered.
inline constexpr std::size_t max_route_entries = max_connections + max_search_targets;

/// This machine's address on the way to one destination.
struct RouteEntry {
    uint8_t destination[4]{};
    uint8_t local[4]{};
};

struct Connection {
    intptr_t fd{invalid_socket};
    bool outbound{};
    bool connecting{};
    bool failed{};
    /// Bytes are carried by an overlay route, or looped to another connection
    /// of this host. fd stays invalid, so the socket is never polled or closed.
    bool carried{};
    /// pair_slot is the other end of a stream this host dialled to itself.
    bool paired{};
    uint8_t pair_slot{};
    /// Port the other end sees this connection send from.
    uint16_t local_port{};
    Address peer{};   // remote end as seen by the socket
    Address target{}; // outbound: the endpoint dialled
    uint32_t rx_used{};
    uint32_t tx_used{};
    uint8_t rx[connection_rx_bytes]{};
    uint8_t tx[connection_tx_bytes]{};
};

struct Host {
    HostConfig config{};
    dplay::Engine engine{};
    intptr_t listener{invalid_socket};
    intptr_t datagram{invalid_socket};
    intptr_t enumeration{invalid_socket};
    uint16_t enum_port_bound{};
    int64_t clock_origin{};
    Connection connections[max_connections]{};
    uint32_t dropped_connections{};
    /// Host clock when a stream connection last took bytes from this host;
    /// valid while stream_sent is set.
    uint32_t last_stream_send_ms{};
    bool stream_sent{};
    /// Local addresses found for recent destinations, oldest replaced first.
    RouteEntry routes[max_route_entries]{};
    uint32_t route_count{};
    uint32_t next_route{};
    char error[160]{};
};

/// Binds the stream listener and datagram socket in their port ranges and initialises the engine.
///
/// Host is large: allocate it on the heap. A port the system refuses writes one line on standard error naming
/// the step and the system's reason, and the same words in the host's error text.
///
/// @param[in,out] host Host to open; on failure its error text says why and every socket is closed.
/// @param config Addresses, port ranges and engine seed.
/// @return False when the socket subsystem, a socket or a free port is missing.
[[nodiscard]] bool host_open(Host* host, const HostConfig& config) noexcept;

/// Binds the enumeration port; called when a session is created, and when this machine takes the name server's
/// place in a session another machine created. One game holds the port. A refusal writes one line on standard
/// error naming the port and the system's reason, and the same words in the host's error text.
///
/// @param[in,out] host Open host.
/// @return True when the port is bound, also when it already was.
[[nodiscard]] bool host_listen_enumeration(Host* host) noexcept;

/// Services every socket once, waiting at most wait_ms for activity.
///
/// Under a HostClock with advance set, the clock moves on by wait_ms when
/// nothing was ready, after a real wait of at most 1 ms while bytes are in
/// flight and none otherwise.
///
/// @param[in,out] host Open host.
/// @param wait_ms Longest wait in milliseconds.
void host_pump(Host* host, uint32_t wait_ms) noexcept;

/// Returns the host clock.
///
/// @param host Open host.
/// @return Milliseconds since host_open, or the HostClock's time when it has one.
[[nodiscard]] uint32_t host_now_ms(const Host* host) noexcept;

/// Closes the engine session and every socket.
///
/// The listener and the datagram and enumeration ports close at once. The
/// stream connections are flushed and stay open, read and discarded, until
/// close_linger_ms after the last bytes this host sent on any of them (or
/// until their peers close them), so the other machines read what was sent
/// last, the departure among it, before the connection ends. Under a
/// HostClock that advances, the wait is simulated.
///
/// @param[in,out] host Host to close.
void host_close(Host* host) noexcept;

/// Returns the game transport over a host: send goes through the engine; receive services the sockets first.
///
/// @param host Host passed back to the transport as its context.
/// @return The transport.
[[nodiscard]] NetTransport host_transport(Host* host) noexcept;

/// Returns the session backend driving a host, with the enumeration target from its config.
///
/// @param host Host passed back to the backend as its context.
/// @return The backend.
[[nodiscard]] session::Backend host_session_backend(Host* host) noexcept;

/// Lists the directed broadcast address of every local IPv4 network a search reaches, with the interface's
/// own address beside it.
///
/// An interface counts when it is up, running and broadcast-capable, and
/// neither loopback nor point-to-point. With a bind address other than
/// 0.0.0.0, only the interface holding that address counts. Its broadcast
/// address is the one the system names, or address | ~netmask when it names
/// none. A broadcast address equal to the interface's own address (a 32-bit
/// netmask), 0.0.0.0 or 255.255.255.255 is left out, and each address is
/// listed once, with the first interface that gives it, in the order the
/// interfaces come.
///
/// @param interfaces the interface addresses, as the system lists them
/// @param count number of entries in interfaces
/// @param bind_ip the address the datagram socket is bound to; 0.0.0.0 for every interface
/// @return the targets; interfaces past max_search_targets distinct addresses are left out
[[nodiscard]] SearchTargets search_targets(
    const LocalInterface* interfaces, std::size_t count, const uint8_t bind_ip[4]
) noexcept;

/// Finds this machine's address on the way to a destination.
///
/// When an overlay route is installed and it claims the destination, the
/// address is the one the route names for this client, and the system is
/// not asked. Otherwise a host bound to one address always uses it. With
/// no such binding the system is asked which of this machine's addresses
/// it routes the destination through (127.0.0.1 for this machine's
/// loopback address), and the answer is remembered for that destination
/// (the last max_route_entries destinations). Nothing is sent.
///
/// @param[in,out] host Open host; its bind address and remembered routes are read and kept.
/// @param to Destination address.
/// @param[out] ip The local address; left unchanged on failure.
/// @return False for 0.0.0.0 or 255.255.255.255, or when the system names no address.
[[nodiscard]] bool local_address_toward(Host* host, const uint8_t to[4], uint8_t ip[4]) noexcept;

/// Resolves an IPv4 address or host name to one IPv4 address.
///
/// Accepts dotted IPv4 and its other numeric forms (shorter, octal or
/// hexadecimal, such as 127.1 or 0x7f.1), read the same way on every system,
/// and host names the system resolves, which may wait on a name server.
///
/// @param name address or host name, without surrounding white space
/// @param[out] ip the first IPv4 address found; left unchanged on failure
/// @return false when name is null or empty or resolves to no IPv4 address
[[nodiscard]] bool resolve_ipv4(const char* name, uint8_t ip[4]) noexcept;

} // namespace oa::netgame::sock
