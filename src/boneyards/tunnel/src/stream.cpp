// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Sequence numbers, the cumulative acknowledgement and the resend ring.
#include "oa/boneyards/tunnel/stream.hpp"

#include <algorithm>
#include <cstdint>

namespace oa::boneyards::tunnel {
namespace {

/// Reads a 16-bit integer stored most significant byte first.
///
/// @param in the two bytes
/// @return the integer
uint16_t load_be16(const uint8_t* in) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8) | in[1]);
}

} // namespace

Stream::Stream() {
    slots_.resize(resend_capacity);
}

bool Stream::enqueue(std::span<const uint8_t> record, std::span<uint8_t> out, size_t& written) {
    written = 0;
    if (record.size() > max_record_bytes)
        return false;
    StreamRecord frame;
    frame.sequence = next_seq_;
    frame.acknowledge = recv_ack_;
    frame.record = record;
    if (!encode_stream(frame, out, written))
        return false;

    const size_t index = static_cast<size_t>(next_seq_ % resend_capacity);
    Slot& slot = slots_[index];
    if (slot.used && slot.sequence != next_seq_ && buffered_ > 0)
        --buffered_;
    slot.sequence = next_seq_;
    slot.length = record.size();
    slot.used = true;
    std::copy(record.begin(), record.end(), slot.bytes.begin());
    ++buffered_;
    ++next_seq_;
    return true;
}

void Stream::observe_acknowledge(uint64_t acknowledge) {
    if (acknowledge <= peer_ack_)
        return;
    // Every slot, not every sequence: a peer that acknowledges the whole range
    // must not walk it, and a wrapped ring still holds the live sequences.
    for (Slot& slot : slots_) {
        if (!slot.used || slot.sequence > acknowledge)
            continue;
        slot.used = false;
        if (buffered_ > 0)
            --buffered_;
    }
    peer_ack_ = acknowledge;
}

Inbound Stream::on_inbound(
    std::span<const uint8_t> frame, std::span<uint8_t> record_out, size_t& delivered
) {
    delivered = 0;
    StreamRecord decoded;
    if (decode_stream(frame, decoded) != DecodeFault::none)
        return Inbound::bad;
    observe_acknowledge(decoded.acknowledge);
    const uint64_t expected = recv_ack_ + 1;
    if (decoded.sequence < expected)
        return Inbound::duplicate;
    if (decoded.sequence > expected)
        return Inbound::gap;
    if (decoded.record.size() > record_out.size())
        return Inbound::bad;
    std::copy(decoded.record.begin(), decoded.record.end(), record_out.begin());
    delivered = decoded.record.size();
    recv_ack_ = decoded.sequence;
    return Inbound::deliver;
}

bool Stream::take_resend(uint64_t sequence, std::span<uint8_t> out, size_t& written) const {
    written = 0;
    const size_t index = static_cast<size_t>(sequence % resend_capacity);
    const Slot& slot = slots_[index];
    if (!slot.used || slot.sequence != sequence)
        return false;
    StreamRecord frame;
    frame.sequence = sequence;
    frame.acknowledge = recv_ack_;
    frame.record = std::span<const uint8_t>(slot.bytes.data(), slot.length);
    return encode_stream(frame, out, written);
}

std::optional<uint64_t> Stream::first_unacknowledged() const {
    if (buffered_ == 0)
        return std::nullopt;
    return peer_ack_ + 1;
}

std::optional<uint64_t> Stream::latest_sequence() const {
    if (next_seq_ <= 1)
        return std::nullopt;
    return next_seq_ - 1;
}

void Stream::unacknowledged_sequences(std::span<uint64_t> sequences, size_t& count) const {
    count = 0;
    for (const Slot& slot : slots_) {
        if (!slot.used || slot.sequence <= peer_ack_)
            continue;
        if (count >= sequences.size())
            break;
        sequences[count] = slot.sequence;
        ++count;
    }
    std::sort(sequences.begin(), sequences.begin() + static_cast<std::ptrdiff_t>(count));
}

void Stream::seed_outbound(uint64_t acknowledge) {
    if (acknowledge + 1 > next_seq_)
        next_seq_ = acknowledge + 1;
    observe_acknowledge(acknowledge);
}

uint64_t Stream::our_acknowledge() const {
    return recv_ack_;
}

uint64_t Stream::next_sequence() const {
    return next_seq_;
}

size_t Stream::buffered() const {
    return buffered_;
}

std::optional<size_t> record_total_length(std::span<const uint8_t> bytes) noexcept {
    if (bytes.size() < stream_header_bytes)
        return std::nullopt;
    const uint16_t body = load_be16(bytes.data() + 16);
    return stream_header_bytes + static_cast<size_t>(body);
}

} // namespace oa::boneyards::tunnel
