// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/match/packet_layer.hpp"

#include "oa/netgame/dplay/engine.hpp"

#include <cstdlib>
#include <cstring>

namespace oa::netgame::match {
namespace {

// Largest datagram taken off the transport only to be dropped: the
// engine never queues a longer message.
constexpr uint32_t oversized_datagram_limit = dplay::receive_queue_bytes;

// The loss roll scales a 15-bit rand() value, 0..0x7fff; the platform's
// rand() may run wider.
int32_t rand15() {
    return std::rand() & static_cast<int32_t>(rand15_range - 1);
}

/// Stages one frame for its destination and sends it through the condenser as a FrameSink.
///
/// Uses guaranteed delivery while the layer asks for it, and the Game's
/// compression and simulated-loss options. Counts sent frames and failures.
///
/// @param context The PacketLayer.
/// @param from Transport id of the sending player.
/// @param to Destination transport id; 0 broadcasts.
/// @param frame Frame bytes, sequence header included.
/// @param size Frame length in bytes.
void send_bytes_to_player(
    void* context, uint32_t from, uint32_t to, const uint8_t* frame, std::size_t size
) {
    auto* layer = static_cast<PacketLayer*>(context);
    if (!condenser_stage(&layer->send_condenser, to, frame, static_cast<uint32_t>(size))) {
        ++layer->send_failures;
        return;
    }
    const auto flags = layer->guaranteed ? send_flag_guaranteed : 0u;
    const Game* options = layer->send_options;
    const bool compress = options == nullptr || options->compression_off == 0;
    const int32_t loss_percent = options != nullptr ? options->send_error_percent : 0;
    const auto result = condenser_send(
        &layer->send_condenser,
        layer->transport,
        &layer->traffic,
        from,
        flags,
        compress,
        loss_percent,
        rand15
    );
    if (result == transport_result::ok)
        ++layer->sent_frames;
    else
        ++layer->send_failures;
}

FrameSink sink_of(PacketLayer* layer) {
    return FrameSink{layer, send_bytes_to_player};
}

/// Empties the receive state so nothing waits to be parsed again.
///
/// @param[in,out] layer Queue to reset.
void reset_receive(PacketLayer* layer) {
    layer->pending = false;
    layer->queued = false;
}

/// Resets the receive state and the channels: channel 0 broadcasts, the other ten wait for a destination.
///
/// The game also sizes each channel's pools here, always at 2 send buffers
/// and 100 packets, channel 0 included; these channels have fixed storage.
///
/// @param[in,out] layer Queue whose channels are set up at its pacing.
void init_channels(PacketLayer* layer) {
    reset_receive(layer);
    send_channel_init(&layer->channels[0], broadcast_destination_id, layer->ticks_between_sends);
    for (std::size_t i = 1; i < channel_count; ++i)
        send_channel_init(&layer->channels[i], free_channel_id, layer->ticks_between_sends);
}

/// Tells whether a transport id still names a player of the bound player table.
///
/// @param layer Queue whose receiver holds the table.
/// @param id Transport id.
/// @return False without a bound table, or when no player record carries the id.
bool names_player(const PacketLayer* layer, uint32_t id) {
    const Player* players = layer->receiver.players;
    if (players == nullptr)
        return false;
    for (std::size_t i = 0; i < OA_PLAYER_COUNT; ++i)
        if (players[i].player_id == id)
            return true;
    return false;
}

/// Returns the channel for a destination, claiming a free slot for a new unicast id.
///
/// With every unicast slot taken, the first one whose destination no longer
/// names a player of the bound table (a player who left) is started again for
/// the new id; its queued records are dropped with it.
///
/// @param[in,out] layer Queue holding the channels.
/// @param to_id Destination transport id; 0 is the broadcast channel.
/// @return The channel, or null when every unicast slot is taken by a player still in the table.
SendChannel* channel_for(PacketLayer* layer, uint32_t to_id) {
    if (to_id == broadcast_destination_id)
        return &layer->channels[0];
    SendChannel* free_slot = nullptr;
    for (std::size_t i = 1; i < channel_count; ++i) {
        auto& channel = layer->channels[i];
        if (channel.destination_id == to_id)
            return &channel;
        if (free_slot == nullptr && channel.destination_id == free_channel_id)
            free_slot = &channel;
    }
    for (std::size_t i = 1;
         i < channel_count && free_slot == nullptr && layer->receiver.players != nullptr;
         ++i)
        if (!names_player(layer, layer->channels[i].destination_id))
            free_slot = &layer->channels[i];
    if (free_slot != nullptr)
        send_channel_init(free_slot, to_id, layer->ticks_between_sends);
    return free_slot;
}

PeerFrameState* peer_of(PacketLayer* layer, uint32_t peer_id) {
    for (auto& peer : layer->receiver.peers)
        if (peer.peer_id == static_cast<int32_t>(peer_id))
            return &peer;
    return nullptr;
}

bool pop_any(PacketLayer* layer, int32_t tick, Packet* out) {
    for (auto& peer : layer->receiver.peers) {
        if (peer.peer_id == -1)
            break;
        auto& records = peer.records;
        const uint8_t* data = nullptr;
        uint16_t length = 0;
        if (pop_due_record(&records, tick, &data, &length)) {
            *out = Packet{PacketKind::record, records.from_id, records.to_id, length, data};
            return true;
        }
    }
    return false;
}

// Parse a frame into its peer's ring. False when the ring was busy and the
// frame must wait.
bool unpack(
    PacketLayer* layer,
    const uint8_t* frame,
    uint32_t size,
    uint32_t from,
    uint32_t to,
    bool fresh,
    int32_t tick
) {
    auto* peer = frame_receiver_find_peer(&layer->receiver, from);
    if (peer == nullptr) {
        ++layer->dropped_frames;
        return true;
    }
    UnpackOutcome outcome{};
    peer->records.recorder_records = layer->receiver.recorder_records;
    peer->records.presence_records = layer->receiver.presence_records;
    peer->records.wide_records = layer->receiver.wide_records;
    const auto error =
        unpack_frame_records(&peer->records, frame, size, tick, from, to, fresh, &outcome);
    if (error != WireError::ok) {
        ++layer->dropped_frames;
        return true;
    }
    return outcome == UnpackOutcome::unpacked;
}

void hold(
    bool* flag,
    bool* fresh_out,
    uint32_t* from_out,
    uint32_t* to_out,
    uint32_t* size_out,
    uint8_t* storage,
    const FrameDelivery& d
) {
    std::memcpy(storage, d.frame, d.size);
    *flag = true;
    *fresh_out = d.fresh;
    *from_out = d.from_id;
    *to_out = d.to_id;
    *size_out = static_cast<uint32_t>(d.size);
}

// Retry the waiting frames in arrival order; one whose peer's ring is still
// busy waits again.
void drain_waiting(PacketLayer* layer, int32_t tick) {
    if (layer->pending) {
        if (!unpack(
                layer,
                layer->pending_frame,
                layer->pending_size,
                layer->pending_from,
                layer->pending_to,
                layer->pending_fresh,
                tick
            ))
            return;
        layer->pending = false;
    }
    if (layer->queued) {
        if (!unpack(
                layer,
                layer->queued_frame,
                layer->queued_size,
                layer->queued_from,
                layer->queued_to,
                layer->queued_fresh,
                tick
            )) {
            std::memcpy(layer->pending_frame, layer->queued_frame, layer->queued_size);
            layer->pending = true;
            layer->pending_fresh = layer->queued_fresh;
            layer->pending_from = layer->queued_from;
            layer->pending_to = layer->queued_to;
            layer->pending_size = layer->queued_size;
        }
        layer->queued = false;
    }
}

// Take a datagram longer than the receive buffer off the transport and
// drop it; false when it cannot be taken off (beyond the limit, no memory
// or the transport refuses again), leaving it for a later call.
bool discard_oversized(PacketLayer* layer, uint32_t needed) {
    if (needed <= sizeof layer->rx || needed > oversized_datagram_limit ||
        layer->transport.receive == nullptr)
        return false;
    auto* scratch = static_cast<uint8_t*>(std::malloc(needed));
    if (scratch == nullptr)
        return false;
    uint32_t from = 0;
    uint32_t to = 0;
    uint32_t size = needed;
    const auto result =
        layer->transport.receive(layer->transport.context, &from, &to, scratch, &size);
    std::free(scratch);
    return result == transport_result::ok;
}

// Every packet handed out is counted by its first byte.
bool delivered(PacketLayer* layer, const Packet* out) {
    if (out->size != 0)
        traffic_stats_count_record(
            &layer->traffic, out->data[0], out->size, TrafficChannel::received
        );
    return true;
}

} // namespace

void packet_layer_create(PacketLayer* layer) noexcept {
    layer->transport = NetTransport{};
    layer->guaranteed = false;
    layer->send_options = nullptr;
    layer->ticks_between_sends = send_pacing(0).ticks_between_sends;
    layer->sent_frames = layer->send_failures = layer->dropped_frames = 0;
    for (auto& channel : layer->channels)
        send_channel_init(&channel, free_channel_id, layer->ticks_between_sends);
    condenser_init(&layer->send_condenser);
    condenser_init(&layer->receive_condenser);
    layer->traffic = TrafficStats{};
    layer->receiver.players = nullptr;
    frame_receiver_init(&layer->receiver);
    reset_receive(layer);
}

bool packet_layer_set_rate(PacketLayer* layer, int32_t sends_per_second) noexcept {
    const auto pacing = send_pacing(sends_per_second);
    if (!pacing.layer_enabled)
        return false;
    layer->ticks_between_sends = pacing.ticks_between_sends;
    for (auto& channel : layer->channels)
        channel.ticks_between_sends = pacing.ticks_between_sends;
    return true;
}

void packet_layer_start(PacketLayer* layer, const NetTransport& transport) noexcept {
    layer->transport = transport;
    init_channels(layer);
}

WireError packet_layer_send(
    PacketLayer* layer,
    uint32_t from_id,
    uint32_t to_id,
    const uint8_t* record,
    std::size_t size,
    uint32_t now_time
) noexcept {
    if (layer == nullptr || record == nullptr)
        return WireError::bad_argument;
    auto* channel = channel_for(layer, to_id);
    if (channel == nullptr)
        return WireError::overflow;
    const auto error = send_channel_queue(channel, from_id, record, size, now_time, sink_of(layer));
    if (error == WireError::ok && size != 0)
        traffic_stats_count_record(
            &layer->traffic, record[0], static_cast<uint32_t>(size), TrafficChannel::sent
        );
    return error;
}

void packet_layer_flush(PacketLayer* layer, uint32_t now_time, bool force) noexcept {
    const auto sink = sink_of(layer);
    for (auto& channel : layer->channels)
        if (channel.destination_id != free_channel_id)
            (void)send_channel_flush(&channel, now_time, force, sink);
}

bool packet_layer_receive(PacketLayer* layer, int32_t tick, Packet* out) noexcept {
    if (layer == nullptr || out == nullptr)
        return false;
    *out = Packet{};
    for (;;) {
        if (pop_any(layer, tick, out))
            return delivered(layer, out);
        // A waiting frame is parsed before another datagram is read, and the
        // call ends on what it made due: nothing read later, a system message
        // included, overtakes its records.
        if (layer->pending || layer->queued) {
            drain_waiting(layer, tick);
            return pop_any(layer, tick, out) && delivered(layer, out);
        }
        uint32_t from = 0;
        uint32_t to = 0;
        uint32_t size = sizeof layer->rx;
        const auto result = condenser_receive(
            &layer->receive_condenser,
            layer->transport,
            &layer->traffic,
            &from,
            &to,
            layer->rx,
            &size
        );
        if (result == transport_result::buffer_too_small) {
            ++layer->dropped_frames;
            if (!discard_oversized(layer, size))
                return false;
            continue;
        }
        if (result != transport_result::ok)
            return false;
        if (from == system_message_sender_id) {
            *out = Packet{PacketKind::system, from, to, size, layer->rx};
            return delivered(layer, out);
        }
        FrameDeliveries deliveries{};
        if (frame_receiver_accept(&layer->receiver, from, to, layer->rx, size, &deliveries) !=
            WireError::ok) {
            ++layer->dropped_frames;
            continue;
        }
        for (uint8_t i = 0; i < deliveries.count; ++i) {
            const auto& d = deliveries.items[i];
            if (layer->pending) {
                hold(
                    &layer->queued,
                    &layer->queued_fresh,
                    &layer->queued_from,
                    &layer->queued_to,
                    &layer->queued_size,
                    layer->queued_frame,
                    d
                );
                continue;
            }
            if (!unpack(
                    layer, d.frame, static_cast<uint32_t>(d.size), d.from_id, d.to_id, d.fresh, tick
                ))
                hold(
                    &layer->pending,
                    &layer->pending_fresh,
                    &layer->pending_from,
                    &layer->pending_to,
                    &layer->pending_size,
                    layer->pending_frame,
                    d
                );
        }
    }
}

void packet_layer_release_peer(PacketLayer* layer, uint32_t peer_id) noexcept {
    if (auto* peer = peer_of(layer, peer_id)) {
        peer_records_reset(&peer->records);
        peer->saved_length = 0;
    }
    for (std::size_t i = 1; i < channel_count; ++i)
        if (layer->channels[i].destination_id == peer_id)
            send_channel_init(&layer->channels[i], free_channel_id, layer->ticks_between_sends);
}

bool packet_layer_idle(const PacketLayer* layer) noexcept {
    if (layer->pending || layer->queued)
        return false;
    for (const auto& peer : layer->receiver.peers)
        if (peer.records.count != 0)
            return false;
    return true;
}

} // namespace oa::netgame::match
