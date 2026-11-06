// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The relay tunnel's frames. Every multi-byte integer is big-endian. A decoder
// reads one frame from the front of a buffer and ignores bytes that follow it.
// A decoded payload or record refers to the caller's buffer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace oa::boneyards::tunnel {

/// Version stamped on every frame.
inline constexpr uint8_t version = 1;

/// Protocol number of a stream of bytes inside a data frame.
inline constexpr uint8_t protocol_tcp = 6;
/// Protocol number of a datagram inside a data frame.
inline constexpr uint8_t protocol_udp = 17;

/// Classification bit set when either port is the control port.
inline constexpr uint8_t flag_control = 0x01;

/// Port that marks a data frame as control traffic. Routing ignores the bit.
inline constexpr uint16_t control_port = 47624;

/// Kind of an authentication challenge.
inline constexpr uint8_t auth_challenge_kind = 1;
/// Kind of an authentication response.
inline constexpr uint8_t auth_response_kind = 2;
/// Kind of a drain notice.
inline constexpr uint8_t go_away_kind = 3;

/// Reason on a drain notice: the relay is closing this task.
inline constexpr uint8_t go_away_draining = 1;

/// Bytes in a data frame before its payload.
inline constexpr size_t data_header_bytes = 17;
/// Bytes of the nonce in a challenge.
inline constexpr size_t auth_nonce_bytes = 16;
/// Bytes of the HMAC in a response.
inline constexpr size_t auth_hmac_bytes = 32;
/// Bytes in a challenge.
inline constexpr size_t auth_challenge_bytes = 18;
/// Bytes in a response.
inline constexpr size_t auth_response_bytes = 42;
/// Bytes in a drain notice.
inline constexpr size_t go_away_bytes = 3;
/// Bytes in a stream record before its inner record.
inline constexpr size_t stream_header_bytes = 18;

/// Length of the authentication context prefix.
inline constexpr size_t auth_context_bytes = 20;

/// Bytes hashed ahead of the nonce so this handshake cannot be reused elsewhere.
inline constexpr std::array<uint8_t, auth_context_bytes> auth_context = {
    'c', 'o', 'r', 'e', 'p', 'r', 'i', 'm', 'e', '-',
    'o', 'v', 'e', 'r', 'l', 'a', 'y', '-', 'v', '1',
};

/// The authentication context as text.
inline constexpr std::string_view auth_context_text{"coreprime-overlay-v1"};

/// Why a frame could not be read.
enum class DecodeFault : uint8_t {
    none,         ///< the frame was read
    short_buffer, ///< the buffer is shorter than the frame it claims to be
    bad_version,  ///< the version byte is not `version`
    bad_frame,    ///< a kind or length does not match the frame
};

/// One overlay address, four octets, first octet first.
struct OverlayIp {
    /// The four octets.
    uint8_t octet[4]{};
};

/// Tells whether two overlay addresses have the same four octets.
///
/// @param one the first address
/// @param other the second address
/// @return true when every octet matches
[[nodiscard]] inline bool operator==(const OverlayIp& one, const OverlayIp& other) noexcept {
    for (size_t index = 0; index < 4; ++index) {
        if (one.octet[index] != other.octet[index])
            return false;
    }
    return true;
}

/// One encapsulated datagram or stream of bytes.
///
/// `payload` refers to the caller's buffer after a decode.
struct DataFrame {
    /// Classification bits. `flag_control` marks control traffic.
    uint8_t flags{};
    /// Overlay address the frame came from.
    OverlayIp source{};
    /// Overlay address the frame is for.
    OverlayIp destination{};
    /// `protocol_tcp` or `protocol_udp`.
    uint8_t protocol{};
    /// Port the frame came from, in host order.
    uint16_t source_port{};
    /// Port the frame is for, in host order.
    uint16_t destination_port{};
    /// The carried bytes. Empty is a complete frame.
    std::span<const uint8_t> payload{};
};

/// The relay's challenge: a nonce the client signs.
struct AuthChallenge {
    /// The nonce the response must sign.
    std::array<uint8_t, auth_nonce_bytes> nonce{};
};

/// The client's proof that it holds the overlay key.
struct AuthResponse {
    /// Overlay address the client claims.
    OverlayIp overlay{};
    /// Identifier of the key that signed the response.
    uint32_t key_id{};
    /// HMAC over the authentication context and then the nonce.
    std::array<uint8_t, auth_hmac_bytes> hmac{};
};

/// A notice that the relay is dropping this connection.
struct GoAway {
    /// Why the relay is dropping the connection. The client reconnects either way.
    uint8_t reason{};
};

/// One reliable record: a sequence, a cumulative acknowledgement, and an inner frame.
///
/// `record` refers to the caller's buffer after a decode.
struct StreamRecord {
    /// Sequence of this record. Zero marks a record that carries only an acknowledgement.
    uint64_t sequence{};
    /// Highest contiguous sequence the sender has delivered from the peer.
    uint64_t acknowledge{};
    /// The inner frame, an authentication frame, a drain notice or a data frame.
    std::span<const uint8_t> record{};
};

/// Returns the classification flag for a pair of ports.
///
/// @param source_port the source port
/// @param destination_port the destination port
/// @return `flag_control` when either port is the control port, otherwise 0
[[nodiscard]] inline uint8_t
flags_for_ports(uint16_t source_port, uint16_t destination_port) noexcept {
    if (source_port == control_port || destination_port == control_port)
        return flag_control;
    return 0;
}

/// Tells whether a protocol number is a datagram rather than a stream of bytes.
///
/// @param protocol the protocol number from a data frame
/// @return false for `protocol_tcp`, true for every other number
[[nodiscard]] inline bool is_datagram(uint8_t protocol) noexcept {
    return protocol != protocol_tcp;
}

/// Writes a data frame.
///
/// @param frame the frame, whose payload may be empty
/// @param[out] out receives the frame
/// @param[out] written the bytes written; 0 when the frame does not fit
/// @return false when `out` is too small or the payload is longer than 65535 bytes
[[nodiscard]] bool
encode_data(const DataFrame& frame, std::span<uint8_t> out, size_t& written) noexcept;

/// Reads a data frame from the front of a buffer.
///
/// @param bytes the buffer
/// @param[out] frame receives the frame; unchanged when the read fails. Its payload
///        refers to `bytes`
/// @return `DecodeFault::none` when the frame was read
[[nodiscard]] DecodeFault decode_data(std::span<const uint8_t> bytes, DataFrame& frame) noexcept;

/// Writes a challenge.
///
/// @param challenge the challenge
/// @return the encoded challenge
[[nodiscard]] std::array<uint8_t, auth_challenge_bytes>
encode_challenge(const AuthChallenge& challenge) noexcept;

/// Reads a challenge from the front of a buffer.
///
/// @param bytes the buffer
/// @param[out] challenge receives the challenge; unchanged when the read fails
/// @return `DecodeFault::none` when the challenge was read
[[nodiscard]] DecodeFault
decode_challenge(std::span<const uint8_t> bytes, AuthChallenge& challenge) noexcept;

/// Writes a response.
///
/// @param response the response
/// @return the encoded response
[[nodiscard]] std::array<uint8_t, auth_response_bytes>
encode_response(const AuthResponse& response) noexcept;

/// Reads a response from the front of a buffer.
///
/// @param bytes the buffer
/// @param[out] response receives the response; unchanged when the read fails
/// @return `DecodeFault::none` when the response was read
[[nodiscard]] DecodeFault
decode_response(std::span<const uint8_t> bytes, AuthResponse& response) noexcept;

/// Writes a drain notice.
///
/// @param reason the reason byte
/// @return the encoded notice
[[nodiscard]] std::array<uint8_t, go_away_bytes> encode_go_away(uint8_t reason) noexcept;

/// Reads a drain notice from the front of a buffer.
///
/// @param bytes the buffer
/// @param[out] frame receives the notice; unchanged when the read fails
/// @return `DecodeFault::none` when the notice was read
[[nodiscard]] DecodeFault decode_go_away(std::span<const uint8_t> bytes, GoAway& frame) noexcept;

/// Tells whether an inner record is a drain notice.
///
/// @param record the inner record of a stream record
/// @return true when it begins with the version and the drain kind
[[nodiscard]] bool is_go_away(std::span<const uint8_t> record) noexcept;

/// Writes a stream record.
///
/// @param record the record, whose inner record may be empty
/// @param[out] out receives the record
/// @param[out] written the bytes written; 0 when the record does not fit
/// @return false when `out` is too small or the inner record is longer than 65535 bytes
[[nodiscard]] bool
encode_stream(const StreamRecord& record, std::span<uint8_t> out, size_t& written) noexcept;

/// Reads a stream record from the front of a buffer.
///
/// @param bytes the buffer
/// @param[out] record receives the record; unchanged when the read fails. Its inner
///        record refers to `bytes`
/// @return `DecodeFault::none` when the record was read
[[nodiscard]] DecodeFault
decode_stream(std::span<const uint8_t> bytes, StreamRecord& record) noexcept;

/// Writes a signed authentication response.
///
/// The signature is the HMAC of the authentication context followed by the nonce,
/// keyed by `key`.
///
/// @param overlay the client's overlay address
/// @param key_id the key's identifier
/// @param key the overlay key
/// @param nonce the challenge nonce, `auth_nonce_bytes` long
/// @param[out] out receives the encoded response
/// @param[out] written the bytes written; 0 when the response does not fit
/// @return false when the nonce is the wrong length or `out` is shorter than a response
[[nodiscard]] bool encode_auth_response(
    OverlayIp overlay,
    uint32_t key_id,
    std::span<const uint8_t> key,
    std::span<const uint8_t> nonce,
    std::span<uint8_t> out,
    size_t& written
) noexcept;

} // namespace oa::boneyards::tunnel
