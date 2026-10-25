// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Frame layer between game packets and the condenser envelope.
//
//   frame = i32 LE sequence + records back to back (one sender per frame)
//
// Broadcast channels (destination 0) number frames -2, -3, ... and wrap
// from INT32_MIN back to -2; unicast frames always carry -1. Receivers keep
// the last sequence per sending peer and hold back at most one frame that
// arrives ahead of the expected one. All storage is fixed-size.

#include "oa/core/player.h"
#include "oa/netgame/wire.hpp"

#include <cstdint>

namespace oa::netgame {

inline constexpr std::size_t frame_header_bytes = 4;
inline constexpr int32_t unicast_frame_sequence = -1;
inline constexpr int32_t first_broadcast_frame_sequence = -2;
inline constexpr int32_t no_frame_sequence = -1; // receiver: nothing seen yet
inline constexpr uint32_t broadcast_destination_id = 0;

// Send-side bounds. A queued packet fits one 0x42a-byte send buffer; the
// channel flushes before an enqueue once 0x42a bytes are pending.
inline constexpr std::size_t send_buffer_bytes = 0x42a;
inline constexpr std::size_t send_queue_capacity = 0x400;
inline constexpr std::size_t max_send_frame_bytes = 0x857;
inline constexpr std::size_t send_queued_bytes_capacity = 2 * send_buffer_bytes - 1;
static_assert(frame_header_bytes + send_queued_bytes_capacity == max_send_frame_bytes);

// Receive-side bounds (condenser receive storage).
inline constexpr std::size_t max_receive_frame_bytes = 28000;
inline constexpr std::size_t receive_peer_slots = 10;
inline constexpr std::size_t record_queue_capacity = 0x200;
inline constexpr int32_t record_hold_window_ticks = 0x1e;
inline constexpr int32_t record_spread_max_ticks = 0x1e;

/// Returns the broadcast frame sequence that follows another.
///
/// @param sequence Current broadcast sequence.
/// @return sequence - 1 in wrapping arithmetic, or -2 when that is -1 or above (after INT32_MIN).
[[nodiscard]] constexpr int32_t next_frame_sequence(int32_t sequence) noexcept {
    const auto next = static_cast<int32_t>(static_cast<uint32_t>(sequence) - 1u);
    return next >= -1 ? first_broadcast_frame_sequence : next;
}

// Send pacing: n < 0 disables the frame layer, 0 means 200 ms, otherwise
// 1000 / clamp(n, 2, 30) ms; ticks are ceil(ms * 30 / 1000).
struct SendPacing {
    bool layer_enabled{};
    int32_t interval_ms{};
    uint32_t ticks_between_sends{};
};

/// Derives the frame layer's send pacing from a sends-per-second setting.
///
/// @param sends_per_second Negative disables the frame layer; 0 means one send per 200 ms; otherwise
///        clamped to 2..30 sends per second.
/// @return The pacing; ticks_between_sends is ceil(interval_ms * 30 / 1000) game ticks.
[[nodiscard]] SendPacing send_pacing(int32_t sends_per_second) noexcept;

struct FrameSink {
    void* context{};
    // Receives one complete frame (sequence header included).
    void (*emit)(
        void* context, uint32_t from_id, uint32_t to_id, const uint8_t* frame, std::size_t size
    ){};
};

struct QueuedPacket {
    uint32_t sender_id{};
    uint16_t offset{};
    uint16_t length{};
};

// One destination's outgoing queue.
struct SendChannel {
    uint32_t destination_id{};
    int32_t frame_number{first_broadcast_frame_sequence};
    uint32_t ticks_between_sends{6};
    uint32_t next_send_tick{};
    uint32_t pending_bytes{};
    uint16_t queue_count{};
    uint16_t queue_head{};
    QueuedPacket queue[send_queue_capacity]{};
    uint16_t bytes_used{};
    uint8_t bytes[send_queued_bytes_capacity]{};
    uint8_t frame[max_send_frame_bytes]{};
};

/// Empties a channel and starts its broadcast sequence at -2.
///
/// @param[out] channel Channel to initialise; the queue storage is not cleared.
/// @param destination_id Transport id the channel sends to; 0 broadcasts.
/// @param ticks_between_sends Pacing interval in game ticks.
void send_channel_init(
    SendChannel* channel, uint32_t destination_id, uint32_t ticks_between_sends
) noexcept;

/// Queues one packet from a sender, flushing first when 0x42a bytes are pending or the queue is full.
///
/// @param[in,out] channel Channel to queue on.
/// @param sender_id Transport id of the local player the packet is from.
/// @param packet Packet bytes; may be null only when size is 0. Zero-length packets are accepted.
/// @param size Packet length in bytes, at most 0x42a.
/// @param now_tick Current game tick, used by a forced flush.
/// @param sink Receiver of frames a forced flush emits.
/// @return ok; bad_argument for a null channel or packet; buffer_too_small above 0x42a bytes; overflow
///         when the queued bytes cannot take the packet.
[[nodiscard]] WireError send_channel_queue(
    SendChannel* channel,
    uint32_t sender_id,
    const uint8_t* packet,
    std::size_t size,
    uint32_t now_tick,
    const FrameSink& sink
) noexcept;

/// Emits the queued packets as frames unless the channel is paced out.
///
/// Each frame takes every queued packet of the sender at the queue head, in
/// queue order; packets of other senders keep their relative order and go
/// out in later frames of the same flush. The next send tick moves to
/// now_tick plus the channel's interval.
///
/// @param[in,out] channel Channel to flush.
/// @param now_tick The connection's clock, in base::game_loop::scaled_clock units; before
///        next_send_tick (base::game_loop::scaled_clock_before) the channel is paced out.
/// @param force Flush even when paced out.
/// @param sink Receiver of each emitted frame.
/// @return True when the queue was flushed, false when paced out.
bool send_channel_flush(
    SendChannel* channel, uint32_t now_tick, bool force, const FrameSink& sink
) noexcept;

// ---- receive side ----

struct QueuedRecord {
    int32_t tick_stamp{};
    uint16_t offset{}; // into PeerRecords::copy
    uint16_t length{};
};

// Per-peer record ring and private frame copy.
struct PeerRecords {
    int32_t spread_base{}; // subtracted from the tick for the spreading span; only ever 0
    uint32_t requeue_count{};
    uint16_t count{};
    uint16_t read_index{};
    int16_t write_index{-1};
    QueuedRecord records[record_queue_capacity]{};
    uint32_t from_id{};
    uint32_t to_id{};
    uint32_t copy_size{};
    /// Records of the recorder (0xf6, 0xf9..0xff) are split out by their own
    /// lengths instead of ending the frame, as a machine running the recorder
    /// splits them.
    bool recorder_records{};
    /// A 0xf0 that starts right after the frame's sequence number, and whose
    /// length word equals the rest of the frame, is split out as a record.
    /// A 0xf0 anywhere else ends the split, as on 3.1c.
    bool presence_records{};
    /// OA weapon records 0xe0..0xe2 are split out by their own lengths
    /// instead of ending the frame. Without this they end the split, as on
    /// 3.1c.
    bool wide_records{};
    // Two spare bytes: a 0x2c record at the very end of a frame takes its
    // length from the bytes after the frame, which keep what an earlier,
    // longer frame left there, as in 3.1c.
    uint8_t copy[max_receive_frame_bytes + 2]{};
};

/// Empties a peer's record ring and forgets its frame copy.
///
/// @param[out] records Ring to reset; the copy bytes are left as they are.
void peer_records_reset(PeerRecords* records) noexcept;

enum class UnpackOutcome : uint8_t {
    unpacked, // records queued (possibly none)
    requeued, // ring was not empty: waiting records re-stamped, frame NOT parsed
};

/// Splits a frame into records and queues them with tick stamps.
///
/// When the ring still holds records the frame is not parsed: the waiting
/// records are re-stamped with tick instead. Otherwise the frame is copied
/// into the ring. The split stops at a byte <= 0x01 or >= 0x2d, or at a
/// record that does not fit (dropping the rest); with the ring's
/// recorder_records set, recorder records are split out by their lengths.
/// With presence_records set, a 0xf0 that starts right after the sequence
/// number and whose length word equals the rest of the frame is split out
/// as a record; a 0xf0 anywhere else ends the split, as on 3.1c. With
/// wide_records set, 0xe0..0xe2 are split out by their table lengths;
/// without it those bytes end the split, as on 3.1c. Beyond 0x200 records,
/// that many 0x2c records are dropped from the front. A zero-length record
/// would never advance the split, so it reports zero_length_record.
///
/// @param[in,out] records The sending peer's record ring.
/// @param frame Frame bytes, sequence header included; may be null only when size is 0.
/// @param size Frame length in bytes, at most 28000.
/// @param tick Current game tick.
/// @param from_id Transport id of the sender.
/// @param to_id Transport id of the addressee.
/// @param fresh True to spread the records over clamp(tick - spread_base, 1, 30) ticks; false stamps
///        every record with tick.
/// @param[out] outcome Whether the frame was unpacked or the waiting records were re-stamped.
/// @return ok; bad_argument for a null pointer; overflow above 28000 bytes; zero_length_record.
[[nodiscard]] WireError unpack_frame_records(
    PeerRecords* records,
    const uint8_t* frame,
    std::size_t size,
    int32_t tick,
    uint32_t from_id,
    uint32_t to_id,
    bool fresh,
    UnpackOutcome* outcome
) noexcept;

/// Takes the next record off a peer's ring unless it is not due yet.
///
/// @param[in,out] records The peer's record ring; null reads as empty.
/// @param tick Current game tick; a head stamped 1..30 ticks after it is held, and tick 0 never holds.
/// @param[out] data Record start inside the ring's frame copy, when not null.
/// @param[out] length Record length, when not null.
/// @return True when a record was taken.
bool pop_due_record(
    PeerRecords* records, int32_t tick, const uint8_t** data, uint16_t* length
) noexcept;

struct PeerFrameState {
    int32_t peer_id{-1}; // -1 = unused; scans stop at the first unused slot
    uint32_t saved_to_id{};
    int32_t last_sequence{no_frame_sequence};
    uint32_t saved_length{};
    uint8_t saved_frame[max_receive_frame_bytes]{};
    PeerRecords records{}; // the peer's record ring
};

struct FrameReceiver {
    PeerFrameState peers[receive_peer_slots]{};
    // The game's player table (Game.players), read when every slot is
    // taken: a slot whose peer no player's player_id names is reclaimed.
    // Null reclaims nothing.
    const Player* players{};
    /// Every peer's frames are split with the recorder's records
    /// (PeerRecords::recorder_records).
    bool recorder_records{};
    /// Every peer's frames are split with presence records
    /// (PeerRecords::presence_records).
    bool presence_records{};
    /// Every peer's frames are split with the wide weapon records
    /// (PeerRecords::wide_records).
    bool wide_records{};
};

struct FrameDelivery {
    const uint8_t* frame{}; // caller's frame or the receiver's saved copy
    std::size_t size{};
    uint32_t from_id{};
    uint32_t to_id{};
    bool fresh{}; // mode for unpack_frame_records
};

struct FrameDeliveries {
    FrameDelivery items[2]{};
    uint8_t count{};
    bool held{}; // the frame was kept back as this peer's out-of-order frame
};

/// Marks every peer slot unused; the player table binding is kept.
///
/// @param[in,out] receiver Receiver to reset.
void frame_receiver_init(FrameReceiver* receiver) noexcept;

/// Returns the peer slot of a sender, taking a slot over when the sender has none.
///
/// The sender's own slot wins, else the first unused one, else the first
/// whose peer no player of the table names. A slot taken over starts with no
/// sequence, nothing held and an empty ring.
///
/// @param[in,out] receiver Receiver whose slots are searched.
/// @param peer_id Transport id of the sender.
/// @return The slot, or null when every slot is bound and the player table names all ten peers or is unset.
[[nodiscard]] PeerFrameState*
frame_receiver_find_peer(FrameReceiver* receiver, uint32_t peer_id) noexcept;

/// Applies sequence acceptance to one frame and lists the frames to hand to unpack_frame_records.
///
/// In order of delivery:
/// - sequence -1, or the first frame from a peer: the frame, fresh (-1 leaves the peer's sequence alone);
/// - expected or older (no de-duplication): the frame, fresh, preceded by a
///   held frame (not fresh) when one exists, so the newer held frame's
///   records come first;
/// - newer than expected with nothing held: held (no delivery);
/// - newer with a frame already held: both, older sequence first; when the
///   held frame is the newer one the current frame goes first and both are
///   not fresh.
/// As in 3.1c, the second frame is parsed only after the first one's records have been drained.
///
/// @param[in,out] receiver Receiver holding the sender's sequence and held frame.
/// @param from_id Transport id of the sender; never 0, the system-message sender.
/// @param to_id Transport id of the addressee.
/// @param frame Frame bytes, sequence header included.
/// @param size Frame length in bytes, 5..28000.
/// @param[out] out Frames to unpack; pointers stay valid until the next call.
/// @return ok; bad_argument for a null pointer or sender 0; truncated at 4 bytes or fewer; overflow above
///         28000 bytes; unknown_peer when no peer slot is available.
[[nodiscard]] WireError frame_receiver_accept(
    FrameReceiver* receiver,
    uint32_t from_id,
    uint32_t to_id,
    const uint8_t* frame,
    std::size_t size,
    FrameDeliveries* out
) noexcept;

} // namespace oa::netgame
