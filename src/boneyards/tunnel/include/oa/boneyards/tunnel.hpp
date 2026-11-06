// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// One client of the relay tunnel. The game calls pump once a frame. pump never
// waits. Datagrams and streams of bytes for an overlay address are queued and
// carried on the one TCP connection; what arrives for this client's overlay
// address is taken from the receive queue.

#include "oa/boneyards/tunnel/wire.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace oa::boneyards::tunnel {

/// How far the connection to the relay has got.
enum class State : uint8_t {
    idle,           ///< connect has not been called
    connecting,     ///< the TCP connection is opening
    authenticating, ///< the TCP connection is open and the challenge has not arrived
    open,           ///< the handshake succeeded
    waiting,        ///< a retry is waiting, or the resume window has passed
    closed,         ///< close was called
};

/// Why the last attempt failed.
enum class Fault : uint8_t {
    none,                ///< no failure, or the handshake has since succeeded
    cannot_resolve,      ///< the relay address did not resolve
    cannot_connect,      ///< the TCP connection was refused or did not open
    malformed_challenge, ///< the relay's first record was not a challenge
    too_large,           ///< the relay declared a record larger than the ring holds
    connection_lost,     ///< the TCP connection ended
    going_away,          ///< the relay asked the client to reconnect
};

/// Whether a received packet is a datagram or a stream of bytes.
enum class Channel : uint8_t {
    datagram, ///< protocol was not TCP
    stream,   ///< protocol was TCP
};

/// Packets `take` can hold. A full queue is left unread and unacknowledged,
/// so a record is never acknowledged and then discarded. The sender keeps it.
inline constexpr size_t receive_bound = 512;

/// How long the relay keeps a dropped session, in milliseconds.
///
/// pump connects again until this long has passed since the connection was
/// lost, then reports a lost connection and does not open another one.
inline constexpr uint64_t resume_window_ms = 6u * 60u * 1000u;

/// Silence, with a record still unacknowledged, before pump connects again.
///
/// Measured in milliseconds. An arrival, or a stretch with nothing waiting
/// for an acknowledgement, starts it over. An idle connection stays up.
inline constexpr uint64_t stall_ms = 5000;

/// One packet taken from the receive queue.
struct Packet {
    /// Overlay address the packet came from.
    OverlayIp source{};
    /// Overlay address the packet was sent to.
    OverlayIp destination{};
    /// Port the packet came from.
    uint16_t source_port{};
    /// Port the packet was sent to.
    uint16_t destination_port{};
    /// Datagram or stream of bytes.
    Channel channel{Channel::datagram};
    /// True when either port is the control port.
    bool control{};
    /// The payload.
    std::vector<uint8_t> bytes{};
};

/// The client side of one relay session.
///
/// A tunnel owns its socket. It is not copied or moved.
class Tunnel {
  public:

    /// Creates a tunnel that stays idle until connect.
    Tunnel();

    /// Closes the socket this tunnel holds.
    ~Tunnel();

    /// Starts a session toward a relay.
    ///
    /// A later call drops the socket and starts a new sequence. The relay keeps
    /// a session for a key identifier and treats the same identifier as a resume,
    /// so a new session uses a new key identifier. pump connects again, with a
    /// bounded delay, when the connection is lost or when nothing arrives for
    /// the stall interval while a record is still unacknowledged. The resume
    /// sends every record the relay has not acknowledged and carries the
    /// acknowledgement of the last contiguous record received, which asks the
    /// relay to replay from the next one. pump stops once the resume window has
    /// passed and reports a lost connection. The host is resolved again on every
    /// attempt.
    ///
    /// @param host the relay's numeric IPv4 address or host name
    /// @param port the relay's port, in host order
    /// @param self this client's overlay address
    /// @param key the overlay key the handshake signs with
    /// @param key_id the key's identifier, sent in the response
    /// @return false when `host` is empty, `port` is 0 or `key` is empty
    [[nodiscard]] bool connect(
        std::string_view host,
        uint16_t port,
        OverlayIp self,
        std::span<const uint8_t> key,
        uint32_t key_id
    );

    /// Queues one datagram for an overlay address.
    ///
    /// The datagram is sent once the handshake has succeeded, including after a
    /// reconnect if the relay has not acknowledged it. Either port equal to the
    /// control port sets the classification bit.
    ///
    /// @param destination the overlay address the datagram is for
    /// @param source_port the port this client sends from
    /// @param destination_port the port the datagram is for
    /// @param bytes the datagram, which may be empty
    /// @return false when the tunnel is idle or closed, the resume window has
    ///         passed, or `bytes` does not fit in one record
    [[nodiscard]] bool send_datagram(
        OverlayIp destination,
        uint16_t source_port,
        uint16_t destination_port,
        std::span<const uint8_t> bytes
    );

    /// Queues one stream of bytes for an overlay address.
    ///
    /// The same rules as send_datagram apply. The frame's protocol is TCP.
    ///
    /// @param destination the overlay address the bytes are for
    /// @param source_port the port this client sends from
    /// @param destination_port the port the bytes are for
    /// @param bytes the bytes, which may be empty
    /// @return false when the tunnel is idle or closed, the resume window has
    ///         passed, or `bytes` does not fit in one record
    [[nodiscard]] bool send_stream(
        OverlayIp destination,
        uint16_t source_port,
        uint16_t destination_port,
        std::span<const uint8_t> bytes
    );

    /// Advances the connection, the handshake and the queues without waiting.
    ///
    /// A lost connection or a stall connects again until the resume window has
    /// passed. After that, pump leaves the fault as a lost connection.
    void pump();

    /// Takes the next packet that arrived for this client.
    ///
    /// @param[out] packet receives the packet; left unchanged when the queue is empty
    /// @return false when the queue is empty
    [[nodiscard]] bool take(Packet& packet);

    /// Returns how far the connection has got.
    ///
    /// @return the state
    [[nodiscard]] State state() const;

    /// Returns the latest fault.
    ///
    /// A successful handshake clears the fault. close leaves it as it was.
    /// Once the resume window has passed, the fault stays `connection_lost`
    /// and pump does not open another connection.
    ///
    /// @return the fault
    [[nodiscard]] Fault fault() const;

    /// Returns a short description of a fault.
    ///
    /// @param fault the fault
    /// @return text that stays valid for the life of the program
    [[nodiscard]] static const char* fault_text(Fault fault);

    /// Stops retrying and closes the socket.
    ///
    /// The tunnel protocol has no end or goodbye frame (its kinds are the
    /// challenge, the response and the relay's drain notice), so this sends
    /// nothing. The relay sees the TCP connection close. Packets already in
    /// the receive queue stay there until take.
    void close();

    /// Sets the clock pump reads, so a caller can move time.
    ///
    /// A null function reads a steady clock. The function is called only from pump
    /// and connect, on the thread that calls them. The stall interval and the
    /// resume window are read from this clock.
    ///
    /// @param now_ms returns milliseconds; null uses a steady clock
    /// @param context passed to `now_ms`
    void set_clock(uint64_t (*now_ms)(void* context), void* context);

  private:

    /// Hidden state, including the socket and the resend ring.
    struct Impl;
    /// The hidden state.
    std::unique_ptr<Impl> impl_;
};

} // namespace oa::boneyards::tunnel
