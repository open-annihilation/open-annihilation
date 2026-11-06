// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Frame bytes. The cases are the Go overlaywire tests and the Rust overlay_wire
// and overlay_encap tests, named as those suites name them.
#include "oa/boneyards/tunnel/hmac.hpp"
#include "oa/boneyards/tunnel/wire.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace tunnel = oa::boneyards::tunnel;

namespace {

tunnel::OverlayIp address_of(uint8_t first, uint8_t second, uint8_t third, uint8_t fourth) {
    tunnel::OverlayIp address{};
    address.octet[0] = first;
    address.octet[1] = second;
    address.octet[2] = third;
    address.octet[3] = fourth;
    return address;
}

bool same_bytes(std::span<const uint8_t> got, std::span<const uint8_t> want) {
    if (got.size() != want.size())
        return false;
    for (size_t index = 0; index < got.size(); ++index) {
        if (got[index] != want[index])
            return false;
    }
    return true;
}

std::span<const uint8_t> text_bytes(std::string_view text) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

void data_roundtrip_and_layout() {
    const auto payload = text_bytes("directplay enum");
    tunnel::DataFrame frame;
    frame.flags = tunnel::flag_control;
    frame.source = address_of(10, 66, 1, 10);
    frame.destination = address_of(10, 66, 2, 11);
    frame.protocol = tunnel::protocol_udp;
    frame.source_port = 2300;
    frame.destination_port = 47624;
    frame.payload = payload;

    std::array<uint8_t, 64> buffer{};
    size_t written = 0;
    OA_CHECK(tunnel::encode_data(frame, buffer, written));
    OA_CHECK(written == tunnel::data_header_bytes + payload.size());

    const uint8_t expect[] = {
        0x01, 0x01, 0x0a, 0x42, 0x01, 0x0a, 0x0a, 0x42, 0x02, 0x0b, 0x11,
        0x08, 0xfc, 0xba, 0x08, 0x00, 0x0f, 'd',  'i',  'r',  'e',  'c',
        't',  'p',  'l',  'a',  'y',  ' ',  'e',  'n',  'u',  'm',
    };
    OA_CHECK(same_bytes(std::span<const uint8_t>(buffer.data(), written), expect));

    tunnel::DataFrame decoded;
    OA_CHECK(
        tunnel::decode_data(std::span<const uint8_t>(buffer.data(), written), decoded) ==
        tunnel::DecodeFault::none
    );
    OA_CHECK(decoded.flags == frame.flags);
    OA_CHECK(decoded.source == frame.source);
    OA_CHECK(decoded.destination == frame.destination);
    OA_CHECK(decoded.protocol == frame.protocol);
    OA_CHECK(decoded.source_port == 2300);
    OA_CHECK(decoded.destination_port == 47624);
    OA_CHECK(same_bytes(decoded.payload, payload));

    buffer[written] = 0xff;
    tunnel::DataFrame extra;
    OA_CHECK(
        tunnel::decode_data(std::span<const uint8_t>(buffer.data(), written + 1), extra) ==
        tunnel::DecodeFault::none
    );
    OA_CHECK(same_bytes(extra.payload, payload));
}

void TestDataRoundtripAndLayout() {
    data_roundtrip_and_layout();
}

void data_empty_payload() {
    tunnel::DataFrame frame;
    frame.source = address_of(10, 66, 0, 10);
    frame.destination = address_of(10, 66, 0, 11);
    frame.protocol = tunnel::protocol_tcp;
    frame.source_port = 1;
    frame.destination_port = 2;
    std::array<uint8_t, tunnel::data_header_bytes> buffer{};
    size_t written = 0;
    OA_CHECK(tunnel::encode_data(frame, buffer, written));
    OA_CHECK(written == tunnel::data_header_bytes);
    tunnel::DataFrame decoded;
    OA_CHECK(tunnel::decode_data(buffer, decoded) == tunnel::DecodeFault::none);
    OA_CHECK(decoded.payload.empty());
    OA_CHECK(decoded.protocol == tunnel::protocol_tcp);
    OA_CHECK(decoded.source_port == 1);
    OA_CHECK(decoded.destination_port == 2);
}

void TestDataEmptyPayload() {
    data_empty_payload();
}

void data_rejects_short_and_bad_version() {
    std::array<uint8_t, tunnel::data_header_bytes - 1> short_buffer{};
    tunnel::DataFrame decoded;
    OA_CHECK(tunnel::decode_data(short_buffer, decoded) == tunnel::DecodeFault::short_buffer);
    std::array<uint8_t, tunnel::data_header_bytes> bad{};
    bad[0] = static_cast<uint8_t>(tunnel::version + 1);
    OA_CHECK(tunnel::decode_data(bad, decoded) == tunnel::DecodeFault::bad_version);
}

void TestDataRejectsShortAndBadVersion() {
    data_rejects_short_and_bad_version();
}

void data_rejects_truncated_payload() {
    std::array<uint8_t, tunnel::data_header_bytes + 2> buffer{};
    buffer[0] = tunnel::version;
    buffer[15] = 0;
    buffer[16] = 10;
    tunnel::DataFrame decoded;
    OA_CHECK(tunnel::decode_data(buffer, decoded) == tunnel::DecodeFault::short_buffer);
}

void TestDataRejectsTruncatedPayload() {
    data_rejects_truncated_payload();
}

void data_encode_rejects_small_buffer() {
    tunnel::DataFrame frame;
    frame.protocol = tunnel::protocol_udp;
    frame.payload = text_bytes("too big");
    std::array<uint8_t, tunnel::data_header_bytes> buffer{};
    size_t written = 99;
    OA_CHECK(!tunnel::encode_data(frame, buffer, written));
    OA_CHECK(written == 0);
}

void TestDataEncodeRejectsSmallBuffer() {
    data_encode_rejects_small_buffer();
}

void auth_challenge_roundtrip_and_layout() {
    tunnel::AuthChallenge challenge;
    challenge.nonce.fill(0xa5);
    const auto encoded = tunnel::encode_challenge(challenge);
    OA_CHECK(encoded.size() == tunnel::auth_challenge_bytes);
    OA_CHECK(encoded[0] == tunnel::version);
    OA_CHECK(encoded[1] == tunnel::auth_challenge_kind);
    OA_CHECK(same_bytes(
        std::span<const uint8_t>(encoded.data() + 2, tunnel::auth_nonce_bytes),
        std::span<const uint8_t>(challenge.nonce.data(), challenge.nonce.size())
    ));
    tunnel::AuthChallenge decoded;
    OA_CHECK(tunnel::decode_challenge(encoded, decoded) == tunnel::DecodeFault::none);
    OA_CHECK(decoded.nonce == challenge.nonce);
}

void TestAuthChallengeRoundtripAndLayout() {
    auth_challenge_roundtrip_and_layout();
}

void auth_challenge_rejects_wrong_kind() {
    tunnel::AuthChallenge challenge;
    auto encoded = tunnel::encode_challenge(challenge);
    encoded[1] = tunnel::auth_response_kind;
    tunnel::AuthChallenge decoded;
    OA_CHECK(tunnel::decode_challenge(encoded, decoded) == tunnel::DecodeFault::bad_frame);
    std::array<uint8_t, tunnel::auth_challenge_bytes - 1> short_buffer{};
    OA_CHECK(tunnel::decode_challenge(short_buffer, decoded) == tunnel::DecodeFault::short_buffer);
}

void TestAuthChallengeRejectsWrongKind() {
    auth_challenge_rejects_wrong_kind();
}

void auth_response_roundtrip_and_layout() {
    tunnel::AuthResponse response;
    response.overlay = address_of(10, 66, 7, 11);
    response.key_id = 0x01020304;
    response.hmac.fill(0x3c);
    const auto encoded = tunnel::encode_response(response);
    OA_CHECK(encoded.size() == tunnel::auth_response_bytes);
    OA_CHECK(encoded[0] == tunnel::version);
    OA_CHECK(encoded[1] == tunnel::auth_response_kind);
    const uint8_t head[] = {0x01, 0x02, 0x0a, 0x42, 0x07, 0x0b, 0x01, 0x02, 0x03, 0x04};
    OA_CHECK(same_bytes(std::span<const uint8_t>(encoded.data(), 10), head));
    OA_CHECK(same_bytes(
        std::span<const uint8_t>(encoded.data() + 10, tunnel::auth_hmac_bytes),
        std::span<const uint8_t>(response.hmac.data(), response.hmac.size())
    ));
    tunnel::AuthResponse decoded;
    OA_CHECK(tunnel::decode_response(encoded, decoded) == tunnel::DecodeFault::none);
    OA_CHECK(decoded.overlay == response.overlay);
    OA_CHECK(decoded.key_id == response.key_id);
    OA_CHECK(decoded.hmac == response.hmac);
}

void TestAuthResponseRoundtripAndLayout() {
    auth_response_roundtrip_and_layout();
}

void auth_response_rejects_short_and_bad_version() {
    std::array<uint8_t, tunnel::auth_response_bytes - 1> short_buffer{};
    tunnel::AuthResponse decoded;
    OA_CHECK(tunnel::decode_response(short_buffer, decoded) == tunnel::DecodeFault::short_buffer);
    auto encoded = tunnel::encode_response(tunnel::AuthResponse{});
    encoded[0] = static_cast<uint8_t>(tunnel::version + 1);
    OA_CHECK(tunnel::decode_response(encoded, decoded) == tunnel::DecodeFault::bad_version);
}

void TestAuthResponseRejectsShortAndBadVersion() {
    auth_response_rejects_short_and_bad_version();
}

void goaway_roundtrip_and_layout() {
    const auto encoded = tunnel::encode_go_away(tunnel::go_away_draining);
    const uint8_t expect[] = {0x01, 0x03, 0x01};
    OA_CHECK(same_bytes(encoded, expect));
    OA_CHECK(tunnel::is_go_away(encoded));
    const auto response = tunnel::encode_response(tunnel::AuthResponse{});
    OA_CHECK(!tunnel::is_go_away(response));
    tunnel::GoAway decoded;
    OA_CHECK(tunnel::decode_go_away(encoded, decoded) == tunnel::DecodeFault::none);
    OA_CHECK(decoded.reason == tunnel::go_away_draining);
}

void TestGoAwayRoundtripAndLayout() {
    goaway_roundtrip_and_layout();
}

void goaway_rejects_short_and_wrong_kind() {
    std::array<uint8_t, tunnel::go_away_bytes - 1> short_buffer{};
    tunnel::GoAway decoded;
    OA_CHECK(tunnel::decode_go_away(short_buffer, decoded) == tunnel::DecodeFault::short_buffer);
    auto encoded = tunnel::encode_go_away(tunnel::go_away_draining);
    encoded[1] = tunnel::auth_response_kind;
    OA_CHECK(tunnel::decode_go_away(encoded, decoded) == tunnel::DecodeFault::bad_frame);
}

void TestGoAwayRejectsShortAndWrongKind() {
    goaway_rejects_short_and_wrong_kind();
}

void stream_record_roundtrip_and_layout() {
    tunnel::AuthResponse inner;
    inner.overlay = address_of(10, 66, 1, 10);
    inner.key_id = 5;
    inner.hmac.fill(0x11);
    const auto inner_bytes = tunnel::encode_response(inner);
    tunnel::StreamRecord record;
    record.sequence = 0x0102030405060708ULL;
    record.acknowledge = 0x1112131415161718ULL;
    record.record = inner_bytes;
    std::array<uint8_t, 128> buffer{};
    size_t written = 0;
    OA_CHECK(tunnel::encode_stream(record, buffer, written));
    OA_CHECK(written == tunnel::stream_header_bytes + inner_bytes.size());
    tunnel::StreamRecord decoded;
    OA_CHECK(
        tunnel::decode_stream(std::span<const uint8_t>(buffer.data(), written), decoded) ==
        tunnel::DecodeFault::none
    );
    OA_CHECK(decoded.sequence == record.sequence);
    OA_CHECK(decoded.acknowledge == record.acknowledge);
    OA_CHECK(same_bytes(decoded.record, inner_bytes));
    tunnel::AuthResponse response;
    OA_CHECK(tunnel::decode_response(decoded.record, response) == tunnel::DecodeFault::none);
    OA_CHECK(response.key_id == 5);
}

void TestStreamRecordRoundtripAndLayout() {
    stream_record_roundtrip_and_layout();
}

void stream_record_carries_data_frame() {
    const auto payload = text_bytes("game");
    tunnel::DataFrame frame;
    frame.source = address_of(10, 66, 1, 10);
    frame.destination = address_of(10, 66, 1, 11);
    frame.protocol = tunnel::protocol_udp;
    frame.source_port = 2300;
    frame.destination_port = 2300;
    frame.payload = payload;
    std::array<uint8_t, 64> inner{};
    size_t inner_len = 0;
    OA_CHECK(tunnel::encode_data(frame, inner, inner_len));
    tunnel::StreamRecord record;
    record.sequence = 1;
    record.acknowledge = 0;
    record.record = std::span<const uint8_t>(inner.data(), inner_len);
    std::array<uint8_t, 128> buffer{};
    size_t written = 0;
    OA_CHECK(tunnel::encode_stream(record, buffer, written));
    tunnel::StreamRecord decoded;
    OA_CHECK(
        tunnel::decode_stream(std::span<const uint8_t>(buffer.data(), written), decoded) ==
        tunnel::DecodeFault::none
    );
    OA_CHECK(decoded.sequence == 1);
    tunnel::DataFrame data;
    OA_CHECK(tunnel::decode_data(decoded.record, data) == tunnel::DecodeFault::none);
    OA_CHECK(same_bytes(data.payload, payload));
}

void TestStreamRecordCarriesDataFrame() {
    stream_record_carries_data_frame();
}

void stream_record_rejects_short_and_truncated() {
    std::array<uint8_t, tunnel::stream_header_bytes - 1> short_buffer{};
    tunnel::StreamRecord decoded;
    OA_CHECK(tunnel::decode_stream(short_buffer, decoded) == tunnel::DecodeFault::short_buffer);
    std::array<uint8_t, tunnel::stream_header_bytes> truncated{};
    truncated[16] = 0;
    truncated[17] = 8;
    OA_CHECK(tunnel::decode_stream(truncated, decoded) == tunnel::DecodeFault::short_buffer);
}

void TestStreamRecordRejectsShortAndTruncated() {
    stream_record_rejects_short_and_truncated();
}

void auth_context_is_stable() {
    OA_CHECK(tunnel::auth_context_text == "coreprime-overlay-v1");
    OA_CHECK(tunnel::auth_context.size() == tunnel::auth_context_text.size());
    bool matches = true;
    for (size_t index = 0; index < tunnel::auth_context.size(); ++index) {
        if (tunnel::auth_context[index] != static_cast<uint8_t>(tunnel::auth_context_text[index]))
            matches = false;
    }
    OA_CHECK(matches);
}

void TestParityConstants() {
    auth_context_is_stable();
    OA_CHECK(tunnel::version == 1);
    OA_CHECK(tunnel::data_header_bytes == 17);
    OA_CHECK(tunnel::auth_challenge_bytes == 18);
    OA_CHECK(tunnel::auth_response_bytes == 42);
    OA_CHECK(tunnel::go_away_bytes == 3);
    OA_CHECK(tunnel::stream_header_bytes == 18);
    OA_CHECK(tunnel::auth_nonce_bytes == 16);
    OA_CHECK(tunnel::auth_hmac_bytes == 32);
    OA_CHECK(tunnel::protocol_tcp == 6);
    OA_CHECK(tunnel::protocol_udp == 17);
    OA_CHECK(tunnel::flag_control == 0x01);
    OA_CHECK(tunnel::auth_challenge_kind == 1);
    OA_CHECK(tunnel::auth_response_kind == 2);
    OA_CHECK(tunnel::go_away_kind == 3);
    OA_CHECK(tunnel::go_away_draining == 1);
}

void auth_response_hmac_matches_independent_recompute() {
    const auto key = text_bytes("the-broker-key");
    std::array<uint8_t, tunnel::auth_nonce_bytes> nonce{};
    nonce.fill(0x5a);
    std::array<uint8_t, tunnel::auth_response_bytes> encoded{};
    size_t written = 0;
    OA_CHECK(
        tunnel::encode_auth_response(address_of(10, 66, 1, 10), 7, key, nonce, encoded, written)
    );
    OA_CHECK(written == encoded.size());
    tunnel::AuthResponse response;
    OA_CHECK(tunnel::decode_response(encoded, response) == tunnel::DecodeFault::none);
    OA_CHECK(response.overlay == address_of(10, 66, 1, 10));
    OA_CHECK(response.key_id == 7);
    const auto want = tunnel::hmac_sha256_two(key, tunnel::auth_context, nonce);
    OA_CHECK(response.hmac == want);
}

void key_swap_changes_id_and_signature() {
    const auto key_a = text_bytes("key-A");
    const auto key_b = text_bytes("key-B");
    std::array<uint8_t, tunnel::auth_nonce_bytes> nonce{};
    nonce.fill(9);
    const auto overlay = address_of(10, 66, 2, 11);
    std::array<uint8_t, tunnel::auth_response_bytes> first{};
    std::array<uint8_t, tunnel::auth_response_bytes> second{};
    size_t written = 0;
    OA_CHECK(tunnel::encode_auth_response(overlay, 1, key_a, nonce, first, written));
    OA_CHECK(tunnel::encode_auth_response(overlay, 2, key_b, nonce, second, written));
    tunnel::AuthResponse response_a;
    tunnel::AuthResponse response_b;
    OA_CHECK(tunnel::decode_response(first, response_a) == tunnel::DecodeFault::none);
    OA_CHECK(tunnel::decode_response(second, response_b) == tunnel::DecodeFault::none);
    OA_CHECK(response_a.key_id == 1);
    OA_CHECK(response_b.key_id == 2);
    OA_CHECK(response_a.hmac != response_b.hmac);
}

void data_roundtrip_preserves_ports_and_proto() {
    const auto payload = text_bytes("game-datagram");
    tunnel::DataFrame frame;
    frame.flags = tunnel::flags_for_ports(2300, 2350);
    frame.source = address_of(10, 66, 7, 10);
    frame.destination = address_of(10, 66, 7, 11);
    frame.protocol = tunnel::protocol_udp;
    frame.source_port = 2300;
    frame.destination_port = 2350;
    frame.payload = payload;
    std::array<uint8_t, 256> buffer{};
    size_t written = 0;
    OA_CHECK(tunnel::encode_data(frame, buffer, written));
    tunnel::DataFrame decoded;
    OA_CHECK(
        tunnel::decode_data(std::span<const uint8_t>(buffer.data(), written), decoded) ==
        tunnel::DecodeFault::none
    );
    OA_CHECK(decoded.source == frame.source);
    OA_CHECK(decoded.destination == frame.destination);
    OA_CHECK(tunnel::is_datagram(decoded.protocol));
    OA_CHECK(decoded.source_port == 2300);
    OA_CHECK(decoded.destination_port == 2350);
    OA_CHECK(same_bytes(decoded.payload, payload));
}

void control_port_sets_the_hint_flag() {
    const auto payload = text_bytes("enum");
    tunnel::DataFrame frame;
    frame.flags = tunnel::flags_for_ports(47624, 47624);
    frame.source = address_of(10, 66, 7, 10);
    frame.destination = address_of(10, 66, 7, 11);
    frame.protocol = tunnel::protocol_udp;
    frame.source_port = 47624;
    frame.destination_port = 47624;
    frame.payload = payload;
    std::array<uint8_t, 64> buffer{};
    size_t written = 0;
    OA_CHECK(tunnel::encode_data(frame, buffer, written));
    OA_CHECK((buffer[1] & tunnel::flag_control) == tunnel::flag_control);
    tunnel::DataFrame decoded;
    OA_CHECK(
        tunnel::decode_data(std::span<const uint8_t>(buffer.data(), written), decoded) ==
        tunnel::DecodeFault::none
    );
    OA_CHECK(decoded.destination_port == 47624);
}

void tcp_proto_decodes_as_not_udp() {
    const auto payload = text_bytes("control");
    tunnel::DataFrame frame;
    frame.flags = tunnel::flags_for_ports(47624, 1234);
    frame.source = address_of(10, 66, 1, 1);
    frame.destination = address_of(10, 66, 1, 2);
    frame.protocol = tunnel::protocol_tcp;
    frame.source_port = 47624;
    frame.destination_port = 1234;
    frame.payload = payload;
    std::array<uint8_t, 64> buffer{};
    size_t written = 0;
    OA_CHECK(tunnel::encode_data(frame, buffer, written));
    tunnel::DataFrame decoded;
    OA_CHECK(
        tunnel::decode_data(std::span<const uint8_t>(buffer.data(), written), decoded) ==
        tunnel::DecodeFault::none
    );
    OA_CHECK(!tunnel::is_datagram(decoded.protocol));
}

} // namespace

int main() {
    TestDataRoundtripAndLayout();
    TestDataEmptyPayload();
    TestDataRejectsShortAndBadVersion();
    TestDataRejectsTruncatedPayload();
    TestDataEncodeRejectsSmallBuffer();
    TestAuthChallengeRoundtripAndLayout();
    TestAuthChallengeRejectsWrongKind();
    TestAuthResponseRoundtripAndLayout();
    TestAuthResponseRejectsShortAndBadVersion();
    TestGoAwayRoundtripAndLayout();
    TestGoAwayRejectsShortAndWrongKind();
    TestStreamRecordRoundtripAndLayout();
    TestStreamRecordCarriesDataFrame();
    TestStreamRecordRejectsShortAndTruncated();
    TestParityConstants();
    auth_context_is_stable();
    auth_response_hmac_matches_independent_recompute();
    key_swap_changes_id_and_signature();
    data_roundtrip_preserves_ports_and_proto();
    control_port_sets_the_hint_flag();
    tcp_proto_decodes_as_not_udp();
    return oa::test::check_exit_status();
}
