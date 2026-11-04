// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Encode and decode of the relay frames. Multi-byte integers are big-endian.
#include "oa/boneyards/tunnel/wire.hpp"

#include "oa/boneyards/tunnel/hmac.hpp"

#include <algorithm>

namespace oa::boneyards::tunnel {
namespace {

/// The longest payload or inner record a 16-bit length can name.
constexpr size_t max_body_bytes = 65535;

/// Writes a 16-bit integer, most significant byte first.
///
/// @param out the two bytes
/// @param value the integer
void store_be16(uint8_t* out, uint16_t value) noexcept {
    out[0] = static_cast<uint8_t>(value >> 8);
    out[1] = static_cast<uint8_t>(value);
}

/// Writes a 32-bit integer, most significant byte first.
///
/// @param out the four bytes
/// @param value the integer
void store_be32(uint8_t* out, uint32_t value) noexcept {
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

/// Writes a 64-bit integer, most significant byte first.
///
/// @param out the eight bytes
/// @param value the integer
void store_be64(uint8_t* out, uint64_t value) noexcept {
    for (size_t index = 0; index < 8; ++index) {
        const unsigned shift = static_cast<unsigned>((7 - index) * 8);
        out[index] = static_cast<uint8_t>(value >> shift);
    }
}

/// Reads a 16-bit integer stored most significant byte first.
///
/// @param in the two bytes
/// @return the integer
uint16_t load_be16(const uint8_t* in) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8) | in[1]);
}

/// Reads a 32-bit integer stored most significant byte first.
///
/// @param in the four bytes
/// @return the integer
uint32_t load_be32(const uint8_t* in) noexcept {
    return (static_cast<uint32_t>(in[0]) << 24) | (static_cast<uint32_t>(in[1]) << 16) |
           (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
}

/// Reads a 64-bit integer stored most significant byte first.
///
/// @param in the eight bytes
/// @return the integer
uint64_t load_be64(const uint8_t* in) noexcept {
    uint64_t value = 0;
    for (size_t index = 0; index < 8; ++index)
        value = (value << 8) | in[index];
    return value;
}

/// Copies four octets into an overlay address.
///
/// @param in the four octets
/// @return the address
OverlayIp load_overlay(const uint8_t* in) noexcept {
    OverlayIp address{};
    for (size_t index = 0; index < 4; ++index)
        address.octet[index] = in[index];
    return address;
}

/// Writes four octets of an overlay address.
///
/// @param out the four octets
/// @param address the address
void store_overlay(uint8_t* out, const OverlayIp& address) noexcept {
    for (size_t index = 0; index < 4; ++index)
        out[index] = address.octet[index];
}

} // namespace

bool encode_data(const DataFrame& frame, std::span<uint8_t> out, size_t& written) noexcept {
    written = 0;
    if (frame.payload.size() > max_body_bytes)
        return false;
    const size_t total = data_header_bytes + frame.payload.size();
    if (out.size() < total)
        return false;
    out[0] = version;
    out[1] = frame.flags;
    store_overlay(out.data() + 2, frame.source);
    store_overlay(out.data() + 6, frame.destination);
    out[10] = frame.protocol;
    store_be16(out.data() + 11, frame.source_port);
    store_be16(out.data() + 13, frame.destination_port);
    store_be16(out.data() + 15, static_cast<uint16_t>(frame.payload.size()));
    if (!frame.payload.empty())
        std::copy(frame.payload.begin(), frame.payload.end(), out.begin() + data_header_bytes);
    written = total;
    return true;
}

DecodeFault decode_data(std::span<const uint8_t> bytes, DataFrame& frame) noexcept {
    if (bytes.size() < data_header_bytes)
        return DecodeFault::short_buffer;
    if (bytes[0] != version)
        return DecodeFault::bad_version;
    const uint16_t payload_len = load_be16(bytes.data() + 15);
    if (bytes.size() < data_header_bytes + static_cast<size_t>(payload_len))
        return DecodeFault::short_buffer;
    DataFrame decoded;
    decoded.flags = bytes[1];
    decoded.source = load_overlay(bytes.data() + 2);
    decoded.destination = load_overlay(bytes.data() + 6);
    decoded.protocol = bytes[10];
    decoded.source_port = load_be16(bytes.data() + 11);
    decoded.destination_port = load_be16(bytes.data() + 13);
    decoded.payload = bytes.subspan(data_header_bytes, payload_len);
    frame = decoded;
    return DecodeFault::none;
}

std::array<uint8_t, auth_challenge_bytes>
encode_challenge(const AuthChallenge& challenge) noexcept {
    std::array<uint8_t, auth_challenge_bytes> out{};
    out[0] = version;
    out[1] = auth_challenge_kind;
    std::copy(challenge.nonce.begin(), challenge.nonce.end(), out.begin() + 2);
    return out;
}

DecodeFault decode_challenge(std::span<const uint8_t> bytes, AuthChallenge& challenge) noexcept {
    if (bytes.size() < auth_challenge_bytes)
        return DecodeFault::short_buffer;
    if (bytes[0] != version)
        return DecodeFault::bad_version;
    if (bytes[1] != auth_challenge_kind)
        return DecodeFault::bad_frame;
    AuthChallenge decoded;
    std::copy_n(bytes.data() + 2, auth_nonce_bytes, decoded.nonce.begin());
    challenge = decoded;
    return DecodeFault::none;
}

std::array<uint8_t, auth_response_bytes> encode_response(const AuthResponse& response) noexcept {
    std::array<uint8_t, auth_response_bytes> out{};
    out[0] = version;
    out[1] = auth_response_kind;
    store_overlay(out.data() + 2, response.overlay);
    store_be32(out.data() + 6, response.key_id);
    std::copy(response.hmac.begin(), response.hmac.end(), out.begin() + 10);
    return out;
}

DecodeFault decode_response(std::span<const uint8_t> bytes, AuthResponse& response) noexcept {
    if (bytes.size() < auth_response_bytes)
        return DecodeFault::short_buffer;
    if (bytes[0] != version)
        return DecodeFault::bad_version;
    if (bytes[1] != auth_response_kind)
        return DecodeFault::bad_frame;
    AuthResponse decoded;
    decoded.overlay = load_overlay(bytes.data() + 2);
    decoded.key_id = load_be32(bytes.data() + 6);
    std::copy_n(bytes.data() + 10, auth_hmac_bytes, decoded.hmac.begin());
    response = decoded;
    return DecodeFault::none;
}

std::array<uint8_t, go_away_bytes> encode_go_away(uint8_t reason) noexcept {
    std::array<uint8_t, go_away_bytes> out{version, go_away_kind, reason};
    return out;
}

DecodeFault decode_go_away(std::span<const uint8_t> bytes, GoAway& frame) noexcept {
    if (bytes.size() < go_away_bytes)
        return DecodeFault::short_buffer;
    if (bytes[0] != version)
        return DecodeFault::bad_version;
    if (bytes[1] != go_away_kind)
        return DecodeFault::bad_frame;
    frame.reason = bytes[2];
    return DecodeFault::none;
}

bool is_go_away(std::span<const uint8_t> record) noexcept {
    return record.size() >= 2 && record[0] == version && record[1] == go_away_kind;
}

bool encode_stream(const StreamRecord& record, std::span<uint8_t> out, size_t& written) noexcept {
    written = 0;
    if (record.record.size() > max_body_bytes)
        return false;
    const size_t total = stream_header_bytes + record.record.size();
    if (out.size() < total)
        return false;
    store_be64(out.data(), record.sequence);
    store_be64(out.data() + 8, record.acknowledge);
    store_be16(out.data() + 16, static_cast<uint16_t>(record.record.size()));
    if (!record.record.empty())
        std::copy(record.record.begin(), record.record.end(), out.begin() + stream_header_bytes);
    written = total;
    return true;
}

DecodeFault decode_stream(std::span<const uint8_t> bytes, StreamRecord& record) noexcept {
    if (bytes.size() < stream_header_bytes)
        return DecodeFault::short_buffer;
    const uint16_t record_len = load_be16(bytes.data() + 16);
    if (bytes.size() < stream_header_bytes + static_cast<size_t>(record_len))
        return DecodeFault::short_buffer;
    StreamRecord decoded;
    decoded.sequence = load_be64(bytes.data());
    decoded.acknowledge = load_be64(bytes.data() + 8);
    decoded.record = bytes.subspan(stream_header_bytes, record_len);
    record = decoded;
    return DecodeFault::none;
}

bool encode_auth_response(
    OverlayIp overlay,
    uint32_t key_id,
    std::span<const uint8_t> key,
    std::span<const uint8_t> nonce,
    std::span<uint8_t> out,
    size_t& written
) noexcept {
    written = 0;
    if (nonce.size() != auth_nonce_bytes || out.size() < auth_response_bytes)
        return false;
    AuthResponse response;
    response.overlay = overlay;
    response.key_id = key_id;
    response.hmac = hmac_sha256_two(key, auth_context, nonce);
    const std::array<uint8_t, auth_response_bytes> encoded = encode_response(response);
    std::copy(encoded.begin(), encoded.end(), out.begin());
    written = encoded.size();
    return true;
}

} // namespace oa::boneyards::tunnel
