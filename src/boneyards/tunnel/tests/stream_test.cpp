// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Sequence, acknowledgement and resend. The cases are the Rust overlay_stream
// tests, named as that suite names them.
#include "oa/boneyards/tunnel/stream.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace tunnel = oa::boneyards::tunnel;

namespace {

std::span<const uint8_t> text_bytes(std::string_view text) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size());
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

tunnel::Inbound ship(
    tunnel::Stream& sender,
    tunnel::Stream& receiver,
    std::span<const uint8_t> record,
    std::span<uint8_t> delivered,
    size_t& delivered_len
) {
    std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes> wire{};
    size_t written = 0;
    if (!sender.enqueue(record, wire, written))
        return tunnel::Inbound::bad;
    return receiver.on_inbound(
        std::span<const uint8_t>(wire.data(), written), delivered, delivered_len
    );
}

void in_order_delivery_advances_ack() {
    tunnel::Stream sender;
    tunnel::Stream receiver;
    std::array<uint8_t, tunnel::max_record_bytes> out{};
    size_t delivered = 0;
    OA_CHECK(
        ship(sender, receiver, text_bytes("first"), out, delivered) == tunnel::Inbound::deliver
    );
    OA_CHECK(delivered == 5);
    OA_CHECK(same_bytes(std::span<const uint8_t>(out.data(), delivered), text_bytes("first")));
    OA_CHECK(receiver.our_acknowledge() == 1);
    OA_CHECK(
        ship(sender, receiver, text_bytes("second"), out, delivered) == tunnel::Inbound::deliver
    );
    OA_CHECK(delivered == 6);
    OA_CHECK(receiver.our_acknowledge() == 2);
}

void peer_ack_prunes_resend_buffer() {
    tunnel::Stream sender;
    tunnel::Stream receiver;
    std::array<uint8_t, tunnel::max_record_bytes> out{};
    std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes> wire{};
    size_t written = 0;
    size_t delivered = 0;
    OA_CHECK(sender.enqueue(text_bytes("r1"), wire, written));
    OA_CHECK(
        receiver.on_inbound(std::span<const uint8_t>(wire.data(), written), out, delivered) ==
        tunnel::Inbound::deliver
    );
    OA_CHECK(sender.enqueue(text_bytes("r2"), wire, written));
    OA_CHECK(
        receiver.on_inbound(std::span<const uint8_t>(wire.data(), written), out, delivered) ==
        tunnel::Inbound::deliver
    );
    OA_CHECK(sender.buffered() == 2);
    OA_CHECK(receiver.enqueue(text_bytes("ack-carrier"), wire, written));
    OA_CHECK(
        sender.on_inbound(std::span<const uint8_t>(wire.data(), written), out, delivered) ==
        tunnel::Inbound::deliver
    );
    OA_CHECK(sender.buffered() == 0);
    OA_CHECK(!sender.first_unacknowledged().has_value());
}

void duplicate_is_ignored() {
    tunnel::Stream sender;
    tunnel::Stream receiver;
    std::array<uint8_t, tunnel::max_record_bytes> out{};
    std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes> wire{};
    size_t written = 0;
    size_t delivered = 0;
    OA_CHECK(sender.enqueue(text_bytes("x"), wire, written));
    OA_CHECK(
        receiver.on_inbound(std::span<const uint8_t>(wire.data(), written), out, delivered) ==
        tunnel::Inbound::deliver
    );
    OA_CHECK(delivered == 1);
    OA_CHECK(
        receiver.on_inbound(std::span<const uint8_t>(wire.data(), written), out, delivered) ==
        tunnel::Inbound::duplicate
    );
    OA_CHECK(receiver.our_acknowledge() == 1);
}

void gap_is_dropped_then_filled_by_resume() {
    tunnel::Stream sender;
    tunnel::Stream receiver;
    std::array<uint8_t, tunnel::max_record_bytes> out{};
    std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes> first{};
    std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes> second{};
    size_t first_len = 0;
    size_t second_len = 0;
    size_t delivered = 0;
    OA_CHECK(sender.enqueue(text_bytes("one"), first, first_len));
    OA_CHECK(sender.enqueue(text_bytes("two"), second, second_len));
    OA_CHECK(
        receiver.on_inbound(std::span<const uint8_t>(second.data(), second_len), out, delivered) ==
        tunnel::Inbound::gap
    );
    OA_CHECK(receiver.our_acknowledge() == 0);
    OA_CHECK(
        receiver.on_inbound(std::span<const uint8_t>(first.data(), first_len), out, delivered) ==
        tunnel::Inbound::deliver
    );
    OA_CHECK(delivered == 3);
    OA_CHECK(same_bytes(std::span<const uint8_t>(out.data(), delivered), text_bytes("one")));
    OA_CHECK(
        receiver.on_inbound(std::span<const uint8_t>(second.data(), second_len), out, delivered) ==
        tunnel::Inbound::deliver
    );
    OA_CHECK(delivered == 3);
    OA_CHECK(same_bytes(std::span<const uint8_t>(out.data(), delivered), text_bytes("two")));
    OA_CHECK(receiver.our_acknowledge() == 2);
}

void reconnect_redelivers_unacked_tail_without_loss() {
    tunnel::Stream sender;
    tunnel::Stream receiver;
    std::array<uint8_t, tunnel::max_record_bytes> out{};
    std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes> wire{};
    size_t written = 0;
    size_t delivered = 0;
    OA_CHECK(sender.enqueue(text_bytes("r1"), wire, written));
    OA_CHECK(
        receiver.on_inbound(std::span<const uint8_t>(wire.data(), written), out, delivered) ==
        tunnel::Inbound::deliver
    );
    OA_CHECK(sender.enqueue(text_bytes("r2"), wire, written));
    OA_CHECK(
        receiver.on_inbound(std::span<const uint8_t>(wire.data(), written), out, delivered) ==
        tunnel::Inbound::deliver
    );
    OA_CHECK(sender.enqueue(text_bytes("r3"), wire, written));

    OA_CHECK(receiver.enqueue(text_bytes("b-ack"), wire, written));
    OA_CHECK(
        sender.on_inbound(std::span<const uint8_t>(wire.data(), written), out, delivered) ==
        tunnel::Inbound::deliver
    );
    OA_CHECK(sender.first_unacknowledged() == uint64_t{3});
    OA_CHECK(sender.latest_sequence() == uint64_t{3});

    // A reconnect keeps the sequences. The unacknowledged tail is sent again.
    const uint64_t from = sender.first_unacknowledged().value_or(0);
    const uint64_t to = sender.latest_sequence().value_or(0);
    int arrivals = 0;
    for (uint64_t sequence = from; sequence <= to; ++sequence) {
        size_t resent = 0;
        if (!sender.take_resend(sequence, wire, resent))
            continue;
        if (receiver.on_inbound(std::span<const uint8_t>(wire.data(), resent), out, delivered) !=
            tunnel::Inbound::deliver)
            continue;
        ++arrivals;
        OA_CHECK(same_bytes(std::span<const uint8_t>(out.data(), delivered), text_bytes("r3")));
    }
    OA_CHECK(arrivals == 1);
    OA_CHECK(receiver.our_acknowledge() == 3);
}

void record_total_len_frames_the_byte_stream() {
    tunnel::Stream sender;
    std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes> wire{};
    size_t written = 0;
    OA_CHECK(sender.enqueue(text_bytes("frame-me"), wire, written));
    OA_CHECK(
        tunnel::record_total_length(std::span<const uint8_t>(wire.data(), written)) == written
    );
    OA_CHECK(!tunnel::record_total_length(
                  std::span<const uint8_t>(wire.data(), tunnel::stream_header_bytes - 1)
    )
                  .has_value());
    OA_CHECK(
        tunnel::record_total_length(
            std::span<const uint8_t>(wire.data(), tunnel::stream_header_bytes)
        ) == written
    );
}

void enqueue_rejects_oversize_record() {
    tunnel::Stream sender;
    std::array<uint8_t, tunnel::max_record_bytes + 1> big{};
    std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes + 8> wire{};
    size_t written = 7;
    OA_CHECK(!sender.enqueue(big, wire, written));
    OA_CHECK(written == 0);
    std::array<uint8_t, 4> tiny{};
    OA_CHECK(!sender.enqueue(text_bytes("fits"), tiny, written));
    OA_CHECK(written == 0);
    OA_CHECK(sender.buffered() == 0);
}

void take_resend_returns_none_for_acked_seq() {
    tunnel::Stream sender;
    tunnel::Stream receiver;
    std::array<uint8_t, tunnel::max_record_bytes> out{};
    std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes> wire{};
    size_t written = 0;
    size_t delivered = 0;
    OA_CHECK(sender.enqueue(text_bytes("r1"), wire, written));
    OA_CHECK(
        receiver.on_inbound(std::span<const uint8_t>(wire.data(), written), out, delivered) ==
        tunnel::Inbound::deliver
    );
    OA_CHECK(receiver.enqueue(text_bytes("carry"), wire, written));
    OA_CHECK(
        sender.on_inbound(std::span<const uint8_t>(wire.data(), written), out, delivered) ==
        tunnel::Inbound::deliver
    );
    size_t resent = 3;
    OA_CHECK(!sender.take_resend(1, wire, resent));
    OA_CHECK(resent == 0);
}

} // namespace

int main() {
    in_order_delivery_advances_ack();
    peer_ack_prunes_resend_buffer();
    duplicate_is_ignored();
    gap_is_dropped_then_filled_by_resume();
    reconnect_redelivers_unacked_tail_without_loss();
    record_total_len_frames_the_byte_stream();
    enqueue_rejects_oversize_record();
    take_resend_returns_none_for_acked_seq();
    return oa::test::check_exit_status();
}
