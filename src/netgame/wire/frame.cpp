// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/frame.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/recorder_messages.hpp"
#include "oa/base/game_loop.hpp"
#include <cstdint>
#include <cstring>

namespace oa::netgame {
namespace {

constexpr int32_t default_send_interval_ms = 200;
constexpr int32_t min_sends_per_second = 2;
constexpr int32_t max_sends_per_second = 30;
constexpr uint32_t ticks_per_second = 30;
constexpr int32_t spread_step_unit = 0x10;
constexpr uint32_t unset_to_id = 0xffffffffu;

QueuedPacket queue_pop(SendChannel* ch) noexcept {
    const auto packet = ch->queue[ch->queue_head];
    ch->queue_head = static_cast<uint16_t>((ch->queue_head + 1) % send_queue_capacity);
    --ch->queue_count;
    return packet;
}

void queue_push(SendChannel* ch, const QueuedPacket& packet) noexcept {
    ch->queue[(ch->queue_head + ch->queue_count) % send_queue_capacity] = packet;
    ++ch->queue_count;
}

void emit_frame(SendChannel* ch, uint32_t from, std::size_t size, const FrameSink& sink) noexcept {
    // The header is stamped at send time; unicast frames still step the counter.
    const auto sequence =
        ch->destination_id == broadcast_destination_id ? ch->frame_number : unicast_frame_sequence;
    store_u32(ch->frame, static_cast<uint32_t>(sequence));
    if (sink.emit)
        sink.emit(sink.context, from, ch->destination_id, ch->frame, size);
    ch->frame_number = next_frame_sequence(ch->frame_number);
}

/// Appends one record to a peer's ring.
///
/// @param[in,out] r The peer's record ring.
/// @param offset Record start in the ring's frame copy.
/// @param stamp Game tick the record is due at.
/// @param length Record length in bytes.
/// @return False, queueing nothing, once 0x200 records are queued.
bool record_queue_push(PeerRecords* r, uint16_t offset, int32_t stamp, uint16_t length) noexcept {
    if (r->count >= record_queue_capacity)
        return false;
    r->write_index = static_cast<int16_t>(
        r->write_index + 1 == static_cast<int32_t>(record_queue_capacity) ? 0 : r->write_index + 1
    );
    r->records[r->write_index] = {stamp, offset, length};
    ++r->count;
    return true;
}

/// Takes the oldest record off a peer's ring.
///
/// @param[in,out] r The peer's record ring.
/// @return The record, valid until the ring is pushed again, or null when the ring is empty.
const QueuedRecord* record_queue_pop(PeerRecords* r) noexcept {
    if (r->count == 0)
        return nullptr;
    --r->count;
    const auto* record = &r->records[r->read_index];
    r->read_index =
        static_cast<uint16_t>(r->read_index + 1 == record_queue_capacity ? 0 : r->read_index + 1);
    return record;
}

/// Returns the length of the record at an offset into the peer's frame copy.
///
/// A 0x2c record at the frame's end takes its length from the bytes after
/// it, which keep what an earlier frame left there, as in 3.1c. A presence
/// record is measured with presence_record_length only when it is the whole
/// frame; any other 0xf0 has no length, so the split ends.
///
/// @param r The peer's records, holding the frame copy.
/// @param at Offset of the record's type byte in the copy.
/// @return The record's length, or 0 when a presence record is not the whole frame.
uint16_t copy_record_length(const PeerRecords* r, std::size_t at) noexcept {
    const auto type = r->copy[at];
    if (type == static_cast<uint8_t>(RecordType::unit_state))
        return load_u16(r->copy + at + 1);
    if (r->recorder_records && is_recorder_record_type(type)) {
        uint16_t length = 0;
        const std::size_t available = sizeof r->copy - at;
        return recorder_record_length(r->copy + at, available, &length) == WireError::ok ? length
                                                                                         : 0;
    }
    if (type == presence_record_type) {
        if (r->presence_records && at == frame_header_bytes) {
            uint16_t length = 0;
            const std::size_t available = r->copy_size > at ? r->copy_size - at : 0;
            if (presence_record_length(r->copy + at, available, &length) == WireError::ok &&
                static_cast<std::size_t>(length) == available)
                return length;
        }
        return 0;
    }
    if (r->wide_records && is_wide_record_type(type))
        return wide_record_length_table[static_cast<std::size_t>(type - first_wide_record_type)];
    return record_length_table[type];
}

/// Tells whether a byte heads a record the ring's split takes.
///
/// @param r the peer's records
/// @param type the byte
/// @return true for 0x02..0x2c, for a recorder record when the ring splits
///         them, for 0xf0 when the ring splits presence records, and for
///         0xe0..0xe2 when the ring splits wide weapon records
bool splits_record(const PeerRecords* r, uint8_t type) noexcept {
    return is_record_type(type) || (r->recorder_records && is_recorder_record_type(type)) ||
           (r->presence_records && type == presence_record_type) ||
           (r->wide_records && is_wide_record_type(type));
}

/// Binds a peer slot to a sender with no sequence, nothing held and an empty ring.
///
/// @param[out] slot Slot to bind.
/// @param peer_id Transport id of the sender.
void peer_frame_info_init(PeerFrameState* slot, uint32_t peer_id) noexcept {
    slot->saved_to_id = unset_to_id;
    slot->last_sequence = no_frame_sequence;
    slot->saved_length = 0;
    slot->peer_id = static_cast<int32_t>(peer_id);
    peer_records_reset(&slot->records);
}

/// Marks a peer slot unused, with no sequence, nothing held and an empty ring.
///
/// @param[out] slot Slot to reset.
void peer_frame_slot_init(PeerFrameState* slot) noexcept {
    slot->peer_id = -1;
    slot->saved_to_id = unset_to_id;
    slot->last_sequence = no_frame_sequence;
    slot->saved_length = 0;
    peer_records_reset(&slot->records);
}

void add_delivery(
    FrameDeliveries* out,
    const uint8_t* frame,
    std::size_t size,
    uint32_t from,
    uint32_t to,
    bool fresh
) noexcept {
    out->items[out->count++] = {frame, size, from, to, fresh};
}

// Some player's Player.player_id names the peer.
bool listed_player(const Player* players, int32_t peer_id) noexcept {
    for (size_t i = 0; i < OA_PLAYER_COUNT; ++i)
        if (static_cast<int32_t>(players[i].player_id) == peer_id)
            return true;
    return false;
}

} // namespace

SendPacing send_pacing(int32_t sends_per_second) noexcept {
    SendPacing pacing{};
    if (sends_per_second < 0)
        return pacing;
    pacing.layer_enabled = true;
    if (sends_per_second == 0) {
        pacing.interval_ms = default_send_interval_ms;
    } else {
        auto n = sends_per_second;
        if (n < min_sends_per_second)
            n = min_sends_per_second;
        if (n > max_sends_per_second)
            n = max_sends_per_second;
        pacing.interval_ms = 1000 / n;
    }
    pacing.ticks_between_sends =
        (static_cast<uint32_t>(pacing.interval_ms) * ticks_per_second + 999u) / 1000u;
    return pacing;
}

void send_channel_init(
    SendChannel* ch, uint32_t destination_id, uint32_t ticks_between_sends
) noexcept {
    ch->destination_id = destination_id;
    ch->frame_number = first_broadcast_frame_sequence;
    ch->ticks_between_sends = ticks_between_sends;
    ch->next_send_tick = 0;
    ch->pending_bytes = 0;
    ch->queue_count = 0;
    ch->queue_head = 0;
    ch->bytes_used = 0;
}

WireError send_channel_queue(
    SendChannel* ch,
    uint32_t sender_id,
    const uint8_t* packet,
    std::size_t size,
    uint32_t now_tick,
    const FrameSink& sink
) noexcept {
    if (ch == nullptr || (packet == nullptr && size != 0))
        return WireError::bad_argument;
    if (size > send_buffer_bytes)
        return WireError::buffer_too_small;
    if (ch->pending_bytes >= send_buffer_bytes || ch->queue_count == send_queue_capacity) {
        send_channel_flush(ch, now_tick, true, sink);
        if (ch->queue_count != 0)
            return WireError::overflow;
    }
    if (ch->bytes_used + size > send_queued_bytes_capacity)
        return WireError::overflow;
    if (size != 0)
        std::memcpy(ch->bytes + ch->bytes_used, packet, size);
    queue_push(ch, {sender_id, ch->bytes_used, static_cast<uint16_t>(size)});
    ch->bytes_used = static_cast<uint16_t>(ch->bytes_used + size);
    ch->pending_bytes += static_cast<uint32_t>(size);
    return WireError::ok;
}

bool send_channel_flush(
    SendChannel* ch, uint32_t now_tick, bool force, const FrameSink& sink
) noexcept {
    // The clock turns over to 0 about every 39.8 hours: a reading that has
    // turned over since the next send tick was set lies after it.
    if (!force && base::game_loop::scaled_clock_before(now_tick, ch->next_send_tick))
        return false;
    ch->next_send_tick = now_tick + ch->ticks_between_sends;
    auto remaining = ch->queue_count;
    while (remaining != 0) {
        const auto sender = ch->queue[ch->queue_head].sender_id;
        std::size_t frame_size = frame_header_bytes;
        unsigned matched = 0;
        for (auto n = remaining; n != 0; --n) {
            const auto packet = queue_pop(ch);
            if (packet.sender_id != sender) {
                queue_push(ch, packet);
                continue;
            }
            std::memcpy(ch->frame + frame_size, ch->bytes + packet.offset, packet.length);
            frame_size += packet.length;
            ++matched;
        }
        ch->pending_bytes = 0;
        if (matched != 0)
            emit_frame(ch, sender, frame_size, sink);
        remaining = ch->queue_count;
    }
    ch->bytes_used = 0;
    return true;
}

void peer_records_reset(PeerRecords* r) noexcept {
    r->spread_base = 0;
    r->requeue_count = 0;
    r->count = 0;
    r->read_index = 0;
    r->write_index = -1;
    r->from_id = 0;
    r->to_id = 0;
    r->copy_size = 0;
}

WireError unpack_frame_records(
    PeerRecords* r,
    const uint8_t* frame,
    std::size_t size,
    int32_t tick,
    uint32_t from_id,
    uint32_t to_id,
    bool fresh,
    UnpackOutcome* outcome
) noexcept {
    if (r == nullptr || outcome == nullptr || (frame == nullptr && size != 0))
        return WireError::bad_argument;
    *outcome = UnpackOutcome::unpacked;
    if (size == 0)
        return WireError::ok;
    if (size > max_receive_frame_bytes)
        return WireError::overflow;
    if (r->count != 0) {
        ++r->requeue_count;
        for (auto n = r->count; n != 0; --n) {
            const auto record = *record_queue_pop(r);
            record_queue_push(r, record.offset, tick, record.length);
        }
        *outcome = UnpackOutcome::requeued;
        return WireError::ok;
    }
    r->requeue_count = 0;
    std::memcpy(r->copy, frame, size);
    r->copy_size = static_cast<uint32_t>(size);
    r->from_id = from_id;
    r->to_id = to_id;

    // Count pass.
    std::size_t at = frame_header_bytes;
    auto remaining = static_cast<int64_t>(size) - static_cast<int64_t>(frame_header_bytes);
    int32_t count = 0;
    if (remaining <= 0)
        return WireError::ok;
    for (;;) {
        const auto type = r->copy[at];
        if (!splits_record(r, type))
            break;
        const auto length = copy_record_length(r, at);
        if (length == 0) {
            if (is_record_type(type))
                return WireError::zero_length_record;
            break; // a recorder record whose length cannot be read
        }
        remaining -= length;
        if (remaining < 0)
            break;
        at += length;
        ++count;
        if (remaining <= 0)
            break;
    }
    if (count <= 0)
        return WireError::ok;

    // Queue pass; beyond the ring's capacity 0x2c records give way first.
    auto overflow = count - static_cast<int32_t>(record_queue_capacity);
    int32_t step = spread_step_unit;
    if (fresh) {
        auto span = static_cast<int32_t>(
            static_cast<uint32_t>(tick) - static_cast<uint32_t>(r->spread_base)
        );
        if (span < 1)
            span = 1;
        if (span > record_spread_max_ticks)
            span = record_spread_max_ticks;
        if (count > span)
            step = (count << 4) / span;
    }
    uint32_t accumulator = 0;
    uint32_t pushed = 0;
    auto stamp = tick;
    at = frame_header_bytes;
    for (int32_t i = 0; i < count; ++i) {
        const auto length = copy_record_length(r, at);
        if (r->copy[at] == static_cast<uint8_t>(RecordType::unit_state) && overflow > 0) {
            --overflow;
        } else {
            if (!record_queue_push(r, static_cast<uint16_t>(at), fresh ? stamp : tick, length))
                return WireError::ok;
            ++pushed;
            if (fresh && (pushed << 4) >= accumulator) {
                accumulator += static_cast<uint32_t>(step);
                stamp = static_cast<int32_t>(static_cast<uint32_t>(stamp) + 1u);
            }
        }
        at += length;
    }
    return WireError::ok;
}

bool pop_due_record(PeerRecords* r, int32_t tick, const uint8_t** data, uint16_t* length) noexcept {
    if (r == nullptr || r->count == 0)
        return false;
    const auto& head = r->records[r->read_index];
    const auto ahead =
        static_cast<int32_t>(static_cast<uint32_t>(head.tick_stamp) - static_cast<uint32_t>(tick));
    if (tick != 0 && ahead > 0 && ahead <= record_hold_window_ticks)
        return false;
    const auto* record = record_queue_pop(r);
    if (data)
        *data = r->copy + record->offset;
    if (length)
        *length = record->length;
    return true;
}

void frame_receiver_init(FrameReceiver* rx) noexcept {
    for (auto& slot : rx->peers)
        peer_frame_slot_init(&slot);
}

PeerFrameState* frame_receiver_find_peer(FrameReceiver* rx, uint32_t id) noexcept {
    const auto peer = static_cast<int32_t>(id);
    for (auto& slot : rx->peers) {
        if (slot.peer_id == peer)
            return &slot;
        if (slot.peer_id == -1) {
            peer_frame_info_init(&slot, id);
            return &slot;
        }
    }
    if (rx->players == nullptr)
        return nullptr;
    for (auto& slot : rx->peers)
        if (!listed_player(rx->players, slot.peer_id)) {
            peer_frame_info_init(&slot, id);
            return &slot;
        }
    return nullptr;
}

WireError frame_receiver_accept(
    FrameReceiver* rx,
    uint32_t from,
    uint32_t to,
    const uint8_t* frame,
    std::size_t size,
    FrameDeliveries* out
) noexcept {
    if (rx == nullptr || out == nullptr || frame == nullptr || from == 0)
        return WireError::bad_argument;
    out->count = 0;
    out->held = false;
    if (size <= frame_header_bytes)
        return WireError::truncated;
    if (size > max_receive_frame_bytes)
        return WireError::overflow;
    const auto sequence = static_cast<int32_t>(load_u32(frame));
    auto* peer = frame_receiver_find_peer(rx, from);
    if (peer == nullptr)
        return WireError::unknown_peer;
    if (sequence == unicast_frame_sequence) {
        add_delivery(out, frame, size, from, to, true);
        return WireError::ok;
    }
    if (peer->last_sequence == no_frame_sequence) {
        peer->last_sequence = sequence;
        add_delivery(out, frame, size, from, to, true);
        return WireError::ok;
    }
    const auto expected = next_frame_sequence(peer->last_sequence);
    const bool has_saved = peer->saved_length > 0;
    const auto ahead =
        static_cast<int32_t>(static_cast<uint32_t>(expected) - static_cast<uint32_t>(sequence));
    if (ahead > 0 && !has_saved) {
        std::memcpy(peer->saved_frame, frame, size);
        peer->saved_length = static_cast<uint32_t>(size);
        peer->saved_to_id = to;
        out->held = true;
        return WireError::ok;
    }
    const auto saved_sequence = has_saved ? static_cast<int32_t>(load_u32(peer->saved_frame)) : 0;
    const auto saved_size = peer->saved_length;
    peer->saved_length = 0;
    if (ahead > 0 && saved_sequence <= sequence) {
        // The held frame is the newer one: current first, then the held one.
        add_delivery(out, frame, size, from, to, false);
        add_delivery(out, peer->saved_frame, saved_size, from, peer->saved_to_id, false);
        peer->last_sequence = saved_sequence;
        return WireError::ok;
    }
    if (has_saved)
        add_delivery(out, peer->saved_frame, saved_size, from, peer->saved_to_id, false);
    add_delivery(out, frame, size, from, to, true);
    peer->last_sequence = sequence;
    return WireError::ok;
}

} // namespace oa::netgame
