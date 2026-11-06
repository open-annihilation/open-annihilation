// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// An optional route for one battle. While one is installed, datagrams and
// streams whose destination the route claims are handed to it instead of to
// the system, and this machine's address toward such a destination is the
// address the route names. A session search for an address the route claims
// goes to the standard enumeration port, whatever port this machine looks for
// games at. With none installed, the sockets behave as they do without a
// route.

#include <cstddef>
#include <cstdint>

namespace oa::netgame::sock {

/// One packet the route has taken.
///
/// `bytes` stays valid until the next take or pump on the same route.
struct OverlayPacket {
    /// Address the packet came from.
    uint8_t source[4]{};
    /// Port the packet came from.
    uint16_t source_port{};
    /// Address the packet was sent to.
    uint8_t destination[4]{};
    /// Port the packet was sent to.
    uint16_t destination_port{};
    /// True when the packet is a stream of bytes, false when it is a datagram.
    bool stream{};
    /// The payload. May be null when size is 0.
    const uint8_t* bytes{};
    /// The payload length in bytes.
    std::size_t size{};
};

/// The functions a route provides. A null function is skipped.
struct OverlayRoute {
    /// Passed back to every function.
    void* context{};
    /// True when ip is an address this route carries.
    ///
    /// @param context OverlayRoute::context
    /// @param ip the destination address
    /// @return true when the route carries ip
    bool (*claims)(void* context, const uint8_t ip[4]){};
    /// Writes this client's own address on the route.
    ///
    /// @param context OverlayRoute::context
    /// @param[out] ip the address
    /// @return false when the route has no address
    bool (*self_address)(void* context, uint8_t ip[4]){};
    /// Carries one datagram.
    ///
    /// @param context OverlayRoute::context
    /// @param to the destination address
    /// @param source_port the port this client sends from
    /// @param destination_port the port the datagram is for
    /// @param bytes the datagram
    /// @param size the datagram length in bytes
    /// @return false when the datagram was not taken
    bool (*send_datagram)(
        void* context,
        const uint8_t to[4],
        uint16_t source_port,
        uint16_t destination_port,
        const uint8_t* bytes,
        std::size_t size
    ){};
    /// Carries one stream of bytes.
    ///
    /// @param context OverlayRoute::context
    /// @param to the destination address
    /// @param source_port the port this client sends from
    /// @param destination_port the port the bytes are for
    /// @param bytes the bytes
    /// @param size the byte count
    /// @return false when the bytes were not taken
    bool (*send_stream)(
        void* context,
        const uint8_t to[4],
        uint16_t source_port,
        uint16_t destination_port,
        const uint8_t* bytes,
        std::size_t size
    ){};
    /// Takes the next packet that arrived for this client.
    ///
    /// @param context OverlayRoute::context
    /// @param[out] packet receives the packet; unchanged when none is waiting
    /// @return false when none is waiting
    bool (*take)(void* context, OverlayPacket* packet){};
    /// Advances the route without waiting.
    ///
    /// @param context OverlayRoute::context
    void (*pump)(void* context){};
};

/// Installs a route, or removes the installed one when route is null.
///
/// The route is copied. It stays until the next call. Installing one clears
/// the direct-packet count.
///
/// @param route the route, or null
void set_overlay_route(const OverlayRoute* route) noexcept;

/// Returns the installed route.
///
/// @return the route, or null when none is installed
[[nodiscard]] const OverlayRoute* overlay_route() noexcept;

/// Returns how many datagrams and streams the sockets sent or received
/// directly, to or from a claimed address or this machine's loopback, while
/// a route was installed.
///
/// @return the count since the route was installed
[[nodiscard]] uint64_t overlay_direct_packets() noexcept;

/// Counts one direct packet while a route is installed.
///
/// A loopback address and an address the route claims are counted. With no
/// route installed, nothing is counted.
///
/// @param ip the address of the packet; null counts nothing
void note_overlay_direct(const uint8_t ip[4]) noexcept;

} // namespace oa::netgame::sock
