// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The client's reliable stream. Sequence numbers start at 1. Each outbound
// record carries the highest contiguous inbound sequence delivered so far.
// A record the peer has not acknowledged stays in a fixed ring and is sent
// again after the connection returns. A gap is dropped; the replay fills it.
// Sequence 0 carries only an acknowledgement and is never delivered.

#include "oa/boneyards/tunnel/wire.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace oa::boneyards::tunnel {

/// Most bytes in one inner record. A data frame's header counts toward this.
inline constexpr size_t max_record_bytes = 2048;

/// Most payload bytes that still fit in one record.
inline constexpr size_t max_payload_bytes = max_record_bytes - data_header_bytes;

/// How many unacknowledged records the ring holds. A full ring drops the oldest.
inline constexpr size_t resend_capacity = 256;

/// What feeding one inbound record did.
enum class Inbound : uint8_t {
    deliver,   ///< the next record, copied into the caller's buffer
    duplicate, ///< a sequence already delivered
    gap,       ///< a sequence ahead of the next one; the record was dropped
    bad,       ///< the frame could not be read, or the caller's buffer was too small
};

/// The client's sequence and acknowledgement state for one session.
///
/// The resend ring lives on the heap. Copying a stream copies that ring.
class Stream {
  public:

    /// Starts a session. The next outbound sequence is 1 and nothing is acknowledged.
    Stream();

    /// Assigns the next sequence, encodes the record and keeps a copy for resend.
    ///
    /// @param record the inner record
    /// @param[out] out receives the encoded stream record
    /// @param[out] written the bytes written; 0 when the record does not fit
    /// @return false when the record is larger than `max_record_bytes` or `out` is too small
    [[nodiscard]] bool
    enqueue(std::span<const uint8_t> record, std::span<uint8_t> out, size_t& written);

    /// Feeds one complete stream record from the peer.
    ///
    /// The peer's acknowledgement is applied before the sequence is classified.
    /// A sequence of 0, and any sequence already delivered, prunes and returns
    /// `duplicate`. A sequence ahead of the next expected one returns `gap` and
    /// does not move the acknowledgement. When the caller's buffer is shorter
    /// than the inner record, the acknowledgement of our own records has already
    /// been applied and the inbound acknowledgement does not move.
    ///
    /// @param frame one encoded stream record
    /// @param[out] record_out receives the inner record when the result is `deliver`
    /// @param[out] delivered the inner record's length when the result is `deliver`; 0 otherwise
    /// @return how the record was classified
    [[nodiscard]] Inbound
    on_inbound(std::span<const uint8_t> frame, std::span<uint8_t> record_out, size_t& delivered);

    /// Encodes a still-buffered record again, with the acknowledgement as it is now.
    ///
    /// @param sequence the record's sequence
    /// @param[out] out receives the encoded stream record
    /// @param[out] written the bytes written; 0 when that sequence is not buffered
    /// @return false when that sequence is not buffered or `out` is too small
    [[nodiscard]] bool
    take_resend(uint64_t sequence, std::span<uint8_t> out, size_t& written) const;

    /// Returns the lowest sequence still waiting for an acknowledgement.
    ///
    /// @return one past the peer's acknowledgement, or nullopt when nothing is waiting
    [[nodiscard]] std::optional<uint64_t> first_unacknowledged() const;

    /// Returns the highest sequence assigned so far.
    ///
    /// @return the highest sequence, or nullopt when nothing has been sent
    [[nodiscard]] std::optional<uint64_t> latest_sequence() const;

    /// Writes the sequences still buffered, lowest first.
    ///
    /// @param[out] sequences receives the sequences
    /// @param[out] count how many were written, at most `sequences.size()`
    void unacknowledged_sequences(std::span<uint64_t> sequences, size_t& count) const;

    /// Moves the outbound frontier forward on a fresh session.
    ///
    /// The next record uses `acknowledge + 1` when that is ahead of the frontier,
    /// and `acknowledge` prunes the ring. A client that is resuming does not call
    /// this; a relay does, when the key identifies a new session.
    ///
    /// @param acknowledge the acknowledgement the client presented
    void seed_outbound(uint64_t acknowledge);

    /// Forgets buffered records the peer has now acknowledged.
    ///
    /// @param acknowledge the peer's cumulative acknowledgement
    void observe_acknowledge(uint64_t acknowledge);

    /// Returns the acknowledgement this side advertises.
    ///
    /// @return the highest contiguous inbound sequence delivered
    [[nodiscard]] uint64_t our_acknowledge() const;

    /// Returns the sequence the next enqueued record will use.
    ///
    /// @return the next sequence
    [[nodiscard]] uint64_t next_sequence() const;

    /// Returns how many records are waiting for an acknowledgement.
    ///
    /// @return the count
    [[nodiscard]] size_t buffered() const;

  private:

    /// One slot of the resend ring.
    struct Slot {
        /// Sequence stored here.
        uint64_t sequence{};
        /// Bytes of the inner record stored here.
        size_t length{};
        /// True when the slot holds a record the peer has not acknowledged.
        bool used{};
        /// The inner record.
        std::array<uint8_t, max_record_bytes> bytes{};
    };

    /// Sequence the next enqueue assigns.
    uint64_t next_seq_{1};
    /// Highest contiguous inbound sequence delivered.
    uint64_t recv_ack_{};
    /// Highest acknowledgement the peer has sent.
    uint64_t peer_ack_{};
    /// Slots whose `used` flag is set.
    size_t buffered_{};
    /// The ring, indexed by sequence modulo `resend_capacity`.
    std::vector<Slot> slots_{};
};

/// Returns the wire length of the stream record at the front of a buffer.
///
/// The body need not have arrived. The length is the header plus the size the
/// header declares.
///
/// @param bytes the bytes gathered so far
/// @return the wire length, or nullopt when fewer than the header bytes are present
[[nodiscard]] std::optional<size_t> record_total_length(std::span<const uint8_t> bytes) noexcept;

} // namespace oa::boneyards::tunnel
