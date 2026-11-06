// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The client connection: one non-blocking TCP socket, the handshake, and the
// resend of anything the relay has not acknowledged. A full receive queue is
// left unread. A record ahead of the next one is kept until the gap closes.
// The tunnel connects again on its own when the connection is lost, or when
// nothing arrives while a record is still unacknowledged, and it stops after
// the relay's resume window.
#include "oa/boneyards/tunnel.hpp"

#include "oa/boneyards/tunnel/stream.hpp"
#include "oa/netgame/stream_socket.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace oa::boneyards::tunnel {
namespace {

/// First retry delay, in milliseconds. Each later failure doubles it.
constexpr uint64_t backoff_first_ms = 250;
/// Largest power applied to the first delay. 250 shifted four times is 4000.
constexpr int backoff_shift_cap = 4;
/// Added to the delay: the clock modulo this many milliseconds.
constexpr uint64_t backoff_jitter_ms = 250;
/// Attempts remembered. The delay stops growing before this.
constexpr int attempt_cap = 16;
/// Reads and writes attempted in one pump.
constexpr int io_rounds = 32;
/// Records kept when they arrive ahead of the next one.
constexpr size_t reorder_bound = 64;
/// How long a gap may stay open before the tunnel connects again, in milliseconds.
constexpr uint64_t reorder_wait_ms = 500;
/// Bytes asked of the socket in one read.
constexpr size_t read_chunk = 4096;

/// What accepting one stream record did.
enum class Intake : uint8_t {
    kept,         ///< the record was queued, held or ignored
    full,         ///< the receive queue is full; the record stays unread
    dropped_link, ///< the record was unreadable and the socket was dropped
};

/// One record held because it arrived ahead of the next sequence.
struct ReorderSlot {
    /// Sequence of the held record.
    uint64_t sequence{};
    /// The encoded stream record.
    std::vector<uint8_t> frame{};
};

/// Milliseconds since the steady clock's epoch.
///
/// @return the time pump uses when no clock was set
uint64_t steady_now_ms() {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(ticks).count()
    );
}

/// Delay before the next attempt.
///
/// @param attempt how many failures have already been scheduled; the first is 0
/// @param now_ms the clock, used for the jitter
/// @return the delay in milliseconds
uint64_t backoff_delay(int attempt, uint64_t now_ms) {
    int shift = attempt;
    if (shift < 0)
        shift = 0;
    if (shift > backoff_shift_cap)
        shift = backoff_shift_cap;
    const uint64_t delay = backoff_first_ms << static_cast<unsigned>(shift);
    return delay + (now_ms % backoff_jitter_ms);
}

/// Reads a drain notice's acknowledgement.
///
/// @param frame one encoded stream record
/// @param[out] acknowledge the record's acknowledgement, when it is a drain notice
/// @return true when the inner record is a drain notice
bool read_go_away(std::span<const uint8_t> frame, uint64_t& acknowledge) {
    StreamRecord record;
    if (decode_stream(frame, record) != DecodeFault::none)
        return false;
    if (!is_go_away(record.record))
        return false;
    acknowledge = record.acknowledge;
    return true;
}

} // namespace

/// One non-blocking TCP socket. Closing an empty one does nothing.
struct ClientSocket {
    /// The system handle, or `invalid_socket`.
    intptr_t handle{oa::netgame::sock::invalid_socket};

    /// Starts with no socket.
    ClientSocket() = default;

    /// Closes the handle.
    ~ClientSocket() { close(); }

    /// Sockets are not copied.
    ClientSocket(const ClientSocket&) = delete;

    /// Sockets are not copied.
    ClientSocket& operator=(const ClientSocket&) = delete;

    /// Closes the handle and stores `invalid_socket`.
    void close() { oa::netgame::sock::stream_close(&handle); }

    /// Tells whether a handle is stored.
    ///
    /// @return true when close has not dropped it
    [[nodiscard]] bool open() const { return handle != oa::netgame::sock::invalid_socket; }

    /// Closes any handle and stores another.
    ///
    /// @param fd the handle stream_connect or stream_accept returned
    void adopt(intptr_t fd) {
        close();
        handle = fd;
    }
};

struct Tunnel::Impl {
    /// The TCP connection, empty until an attempt starts.
    ClientSocket socket{};
    /// Sequences for this connect(). A retry keeps them.
    Stream stream{};
    /// Relay host, resolved again on every attempt.
    std::string host{};
    /// Relay port, in host order.
    uint16_t port{};
    /// This client's overlay address.
    OverlayIp self{};
    /// Overlay key. Held so a retry can sign the next nonce.
    std::vector<uint8_t> key{};
    /// Key identifier sent in the response.
    uint32_t key_id{};
    /// How far the connection has got.
    State state{State::idle};
    /// Latest fault. Cleared when a handshake succeeds.
    Fault fault{Fault::none};
    /// Packets waiting for take.
    std::deque<Packet> incoming{};
    /// Bytes still to write, including a partial write.
    std::vector<uint8_t> outbound{};
    /// How much of `outbound` the socket has accepted.
    size_t outbound_sent{};
    /// Bytes read and not yet consumed as records.
    std::vector<uint8_t> inbound{};
    /// Failures since the last successful handshake.
    int attempt{};
    /// True when a retry is waiting for the clock.
    bool retry_armed{};
    /// Clock value at which the retry may start.
    uint64_t retry_at{};
    /// Clock pump reads. Null uses the steady clock.
    uint64_t (*now_ms)(void* context){};
    /// Passed to `now_ms`.
    void* clock_context{};
    /// Records that arrived ahead of the next sequence, kept until it arrives.
    /// A reconnect leaves them in place so the replay can fill the gap.
    std::vector<ReorderSlot> reorder{};
    /// Clock value when the current gap was first seen. Zero when there is no gap.
    uint64_t reorder_since{};
    /// True from the loss until the next handshake succeeds.
    bool outage_{};
    /// Clock value when the current outage started.
    uint64_t lost_since_{};
    /// True when the resume window has passed and pump must not connect again.
    bool stopped_{};
    /// Clock value when the stall interval started. Zero when it is not running.
    uint64_t stall_since_{};

    /// Returns the clock, in milliseconds.
    ///
    /// @return the injected clock, or the steady clock
    uint64_t now() const {
        if (now_ms != nullptr)
            return now_ms(clock_context);
        return steady_now_ms();
    }

    /// Tells whether the relay's resume window has passed since the loss.
    ///
    /// @return true when retries must stop
    bool resume_expired() const {
        if (!outage_)
            return false;
        const uint64_t current = now();
        if (current < lost_since_)
            return false;
        return current - lost_since_ > resume_window_ms;
    }

    /// Stops retrying and reports a lost connection.
    ///
    /// The relay no longer keeps the session. Packets already in the receive
    /// queue stay until take. A record held for a gap is dropped here: nothing
    /// will replay it. pump does not open another connection.
    void give_up() {
        socket.close();
        outbound.clear();
        outbound_sent = 0;
        inbound.clear();
        reorder.clear();
        reorder_since = 0;
        stall_since_ = 0;
        retry_armed = false;
        stopped_ = true;
        fault = Fault::connection_lost;
        state = State::waiting;
    }

    /// Drops the socket, forgets partial reads and writes, and schedules a retry.
    ///
    /// Sequences and the resend ring stay, and so does a record held for a gap:
    /// the next handshake replays every record the relay has not acknowledged,
    /// and the held record is delivered when the gap fills. An immediate retry
    /// does not count as another attempt. Once the resume window has passed,
    /// the loss is reported and no retry is scheduled.
    ///
    /// @param why the fault to report
    /// @param immediate true to retry on the next pump
    void fail(Fault why, bool immediate) {
        if (stopped_)
            return;
        // Only a session that reached open can be resumed. A connection that
        // never opened keeps its own fault and keeps trying. Once that session
        // has been lost, later failures stay inside the same window.
        const bool resumable = outage_ || state == State::open;
        socket.close();
        outbound.clear();
        outbound_sent = 0;
        inbound.clear();
        stall_since_ = 0;
        const uint64_t current = now();
        if (resumable) {
            if (!outage_) {
                outage_ = true;
                lost_since_ = current;
            }
            if (resume_expired()) {
                give_up();
                return;
            }
        }
        fault = why;
        if (immediate) {
            retry_armed = true;
            retry_at = current;
        } else {
            const uint64_t delay = backoff_delay(attempt, current);
            if (attempt < attempt_cap)
                ++attempt;
            retry_armed = true;
            retry_at = current + delay;
        }
        state = State::waiting;
    }

    /// Opens the TCP connection, or schedules a retry when it cannot.
    void begin_connect() {
        retry_armed = false;
        socket.close();
        outbound.clear();
        outbound_sent = 0;
        inbound.clear();

        uint8_t resolved[4]{};
        if (!oa::netgame::sock::resolve_ipv4(host.c_str(), resolved)) {
            fail(Fault::cannot_resolve, false);
            return;
        }
        oa::netgame::sock::Address address{};
        for (size_t index = 0; index < 4; ++index)
            address.ip[index] = resolved[index];
        address.port = port;
        char failure[128]{};
        const intptr_t opened = oa::netgame::sock::stream_connect(address, failure, sizeof failure);
        if (opened == oa::netgame::sock::invalid_socket) {
            fail(Fault::cannot_connect, false);
            return;
        }
        socket.adopt(opened);
        state = State::connecting;
        progress_connect();
    }

    /// Moves a connecting socket to authenticating once the system says it is open.
    void progress_connect() {
        const oa::netgame::sock::StreamOpen opened =
            oa::netgame::sock::stream_wait_open(socket.handle, 0);
        if (opened == oa::netgame::sock::StreamOpen::opening)
            return;
        if (opened != oa::netgame::sock::StreamOpen::open) {
            fail(Fault::cannot_connect, false);
            return;
        }
        state = State::authenticating;
    }

    /// Writes queued bytes until the socket would block or the queue is empty.
    ///
    /// @return false when the connection failed and a retry was scheduled
    bool flush_writes() {
        for (int round = 0; round < io_rounds && outbound_sent < outbound.size(); ++round) {
            const std::ptrdiff_t wrote = oa::netgame::sock::stream_write(
                socket.handle, outbound.data() + outbound_sent, outbound.size() - outbound_sent
            );
            if (wrote < 0) {
                fail(Fault::connection_lost, false);
                return false;
            }
            if (wrote == 0)
                return true;
            outbound_sent += static_cast<size_t>(wrote);
        }
        if (outbound_sent >= outbound.size()) {
            outbound.clear();
            outbound_sent = 0;
        }
        return true;
    }

    /// Reads what has arrived.
    ///
    /// A full receive queue is left in the socket, so the sender slows down
    /// and the records past the queue stay unacknowledged.
    ///
    /// @return false when the connection ended and a retry was scheduled
    bool read_some() {
        if (incoming.size() >= receive_bound)
            return true;
        for (int round = 0; round < io_rounds; ++round) {
            uint8_t buffer[read_chunk];
            const std::ptrdiff_t got =
                oa::netgame::sock::stream_read(socket.handle, buffer, sizeof buffer);
            if (got == 0)
                return true;
            if (got < 0) {
                fail(Fault::connection_lost, false);
                return false;
            }
            // Any arrival starts the stall interval over.
            stall_since_ = 0;
            inbound.insert(inbound.end(), buffer, buffer + got);
        }
        return true;
    }

    /// Appends bytes to the write queue.
    ///
    /// @param bytes the bytes
    void append_bytes(std::span<const uint8_t> bytes) {
        outbound.insert(outbound.end(), bytes.begin(), bytes.end());
    }

    /// Appends one stream record that is not kept for resend.
    ///
    /// Sequence 0 carries the authentication response and pure acknowledgements.
    ///
    /// @param sequence the sequence, 0 for a record that is not resent
    /// @param record the inner record, which may be empty
    void append_record(uint64_t sequence, std::span<const uint8_t> record) {
        StreamRecord frame;
        frame.sequence = sequence;
        frame.acknowledge = stream.our_acknowledge();
        frame.record = record;
        std::array<uint8_t, stream_header_bytes + max_record_bytes> wire{};
        size_t written = 0;
        if (!encode_stream(frame, wire, written))
            return;
        append_bytes(std::span<const uint8_t>(wire.data(), written));
    }

    /// Appends every record the relay has not acknowledged, with the acknowledgement now.
    void append_resends() {
        std::array<uint64_t, resend_capacity> sequences{};
        size_t count = 0;
        stream.unacknowledged_sequences(sequences, count);
        std::array<uint8_t, stream_header_bytes + max_record_bytes> wire{};
        for (size_t index = 0; index < count; ++index) {
            size_t written = 0;
            if (!stream.take_resend(sequences[index], wire, written))
                continue;
            append_bytes(std::span<const uint8_t>(wire.data(), written));
        }
    }

    /// Answers a challenge, then replays unacknowledged records.
    ///
    /// @param frame the stream record that should hold the challenge
    /// @return false when it was not a challenge and the socket was dropped
    bool handle_challenge(std::span<const uint8_t> frame) {
        StreamRecord record;
        if (decode_stream(frame, record) != DecodeFault::none) {
            fail(Fault::malformed_challenge, false);
            return false;
        }
        AuthChallenge challenge;
        if (decode_challenge(record.record, challenge) != DecodeFault::none) {
            fail(Fault::malformed_challenge, false);
            return false;
        }
        stream.observe_acknowledge(record.acknowledge);
        std::array<uint8_t, auth_response_bytes> response{};
        size_t response_len = 0;
        if (!encode_auth_response(self, key_id, key, challenge.nonce, response, response_len)) {
            fail(Fault::malformed_challenge, false);
            return false;
        }
        append_record(0, std::span<const uint8_t>(response.data(), response_len));
        append_resends();
        attempt = 0;
        fault = Fault::none;
        retry_armed = false;
        outage_ = false;
        lost_since_ = 0;
        stopped_ = false;
        stall_since_ = 0;
        // A gap that survived the reconnect gets a fresh wait. Leaving the old
        // stamp would connect again on the next pump.
        if (reorder.empty())
            reorder_since = 0;
        else
            reorder_since = now();
        state = State::open;
        return true;
    }

    /// Keeps a record that arrived ahead of the next sequence.
    ///
    /// The copy outlives the read buffer. A record too far ahead, or one that
    /// will not fit, is not kept: the sender still has it, and the gap timer
    /// connects again so the replay can fill the hole. A second copy of a
    /// sequence already held is ignored.
    ///
    /// @param sequence the record's sequence
    /// @param frame the encoded stream record
    void keep_reorder(uint64_t sequence, std::span<const uint8_t> frame) {
        const uint64_t expected = stream.our_acknowledge() + 1;
        if (sequence <= expected)
            return;
        if (reorder_since == 0)
            reorder_since = now();
        if (sequence - expected > reorder_bound)
            return;
        for (const ReorderSlot& slot : reorder) {
            if (slot.sequence == sequence)
                return;
        }
        if (reorder.size() >= reorder_bound)
            return;
        ReorderSlot slot;
        slot.sequence = sequence;
        slot.frame.assign(frame.begin(), frame.end());
        reorder.push_back(std::move(slot));
    }

    /// Delivers held records whose sequence is now the next one.
    ///
    /// Stops when the receive queue is full, so the next record stays held and
    /// unacknowledged. An empty hold clears the gap timer.
    void release_reorder() {
        for (;;) {
            if (state != State::open)
                return;
            const uint64_t expected = stream.our_acknowledge() + 1;
            size_t found = reorder.size();
            for (size_t slot_at = 0; slot_at < reorder.size(); ++slot_at) {
                if (reorder[slot_at].sequence == expected) {
                    found = slot_at;
                    break;
                }
            }
            if (found == reorder.size()) {
                if (reorder.empty())
                    reorder_since = 0;
                return;
            }
            if (incoming.size() >= receive_bound)
                return;
            std::vector<uint8_t> held = std::move(reorder[found].frame);
            reorder.erase(reorder.begin() + static_cast<std::ptrdiff_t>(found));
            const Intake intake = handle_record(held);
            if (intake == Intake::dropped_link)
                return;
            if (intake == Intake::full) {
                ReorderSlot slot;
                slot.sequence = expected;
                slot.frame = std::move(held);
                reorder.push_back(std::move(slot));
                return;
            }
        }
    }

    /// Connects again when a gap has stayed open long enough to be a loss.
    ///
    /// The missing records were not acknowledged, so the sender's replay fills
    /// the gap in order. Records already held stay held. A gap that closes
    /// before the wait does not connect.
    void recover_reorder() {
        if (reorder_since == 0 || state != State::open)
            return;
        const uint64_t current = now();
        if (current < reorder_since)
            return;
        if (current - reorder_since < reorder_wait_ms)
            return;
        fail(Fault::connection_lost, true);
    }

    /// Connects again when records are unacknowledged and nothing has arrived.
    ///
    /// The delay is the same backoff as a lost connection. An arrival, or a
    /// stretch with nothing outstanding, starts the interval over. An idle
    /// connection stays up.
    void watch_stall() {
        if (state != State::open)
            return;
        if (stream.buffered() == 0) {
            stall_since_ = 0;
            return;
        }
        const uint64_t current = now();
        if (stall_since_ == 0) {
            stall_since_ = current;
            return;
        }
        if (current < stall_since_)
            return;
        if (current - stall_since_ < stall_ms)
            return;
        fail(Fault::connection_lost, false);
    }

    /// Delivers one data record, holds one that arrived early, or notes a duplicate.
    ///
    /// The next record is not acknowledged while the receive queue is full.
    /// It stays at the front of the read buffer, and the sender keeps it.
    /// A record for this client is acknowledged only after it has been stored
    /// for take. take is the only way that packet leaves the queue.
    ///
    /// @param frame one encoded stream record
    /// @return `full` when the queue cannot take the next record, `dropped_link`
    ///         when the frame was unreadable and the socket was dropped
    Intake handle_record(std::span<const uint8_t> frame) {
        StreamRecord peeked;
        if (decode_stream(frame, peeked) != DecodeFault::none) {
            fail(Fault::connection_lost, false);
            return Intake::dropped_link;
        }
        const uint64_t expected = stream.our_acknowledge() + 1;
        if (peeked.sequence == expected && incoming.size() >= receive_bound)
            return Intake::full;

        std::array<uint8_t, max_record_bytes> body{};
        size_t body_len = 0;
        const uint64_t before = stream.our_acknowledge();
        const Inbound result = stream.on_inbound(frame, body, body_len);
        if (result == Inbound::bad) {
            fail(Fault::connection_lost, false);
            return Intake::dropped_link;
        }
        if (result == Inbound::gap)
            keep_reorder(peeked.sequence, frame);
        if (result == Inbound::deliver) {
            DataFrame data;
            const std::span<const uint8_t> payload(body.data(), body_len);
            if (decode_data(payload, data) == DecodeFault::none && data.destination == self) {
                Packet packet;
                packet.source = data.source;
                packet.destination = data.destination;
                packet.source_port = data.source_port;
                packet.destination_port = data.destination_port;
                packet.channel = is_datagram(data.protocol) ? Channel::datagram : Channel::stream;
                packet.control = (data.flags & flag_control) != 0;
                packet.bytes.assign(data.payload.begin(), data.payload.end());
                incoming.push_back(std::move(packet));
            }
        }
        if (stream.our_acknowledge() != before)
            append_record(0, {});
        return Intake::kept;
    }

    /// Consumes complete records from the read buffer.
    ///
    /// The next record is left unread when the receive queue is full. Held
    /// records are delivered once the sequence they were waiting on arrives.
    void drain() {
        while (state == State::authenticating || state == State::open) {
            if (state == State::open)
                release_reorder();
            if (state != State::authenticating && state != State::open)
                return;
            const std::optional<size_t> total = record_total_length(inbound);
            if (!total.has_value())
                return;
            if (*total > stream_header_bytes + max_record_bytes) {
                fail(Fault::too_large, false);
                return;
            }
            if (inbound.size() < *total)
                return;
            const std::span<const uint8_t> frame(inbound.data(), *total);
            if (state == State::authenticating) {
                if (!handle_challenge(frame))
                    return;
            } else {
                uint64_t acknowledge = 0;
                if (read_go_away(frame, acknowledge)) {
                    stream.observe_acknowledge(acknowledge);
                    fail(Fault::going_away, true);
                    return;
                }
                const Intake intake = handle_record(frame);
                if (intake == Intake::dropped_link)
                    return;
                if (intake == Intake::full)
                    return;
            }
            inbound.erase(inbound.begin(), inbound.begin() + static_cast<std::ptrdiff_t>(*total));
        }
    }

    /// One frame of work: retry, open, read, deliver, write.
    void pump() {
        if (state == State::idle || state == State::closed)
            return;
        if (stopped_)
            return;
        if (outage_ && resume_expired()) {
            give_up();
            return;
        }
        if (state == State::waiting) {
            if (!retry_armed || now() < retry_at)
                return;
            begin_connect();
        }
        if (state == State::connecting)
            progress_connect();
        if (state != State::authenticating && state != State::open)
            return;
        if (state == State::open)
            recover_reorder();
        if (state != State::authenticating && state != State::open)
            return;
        if (!flush_writes())
            return;
        if (!read_some())
            return;
        drain();
        if (state == State::authenticating || state == State::open)
            flush_writes();
        if (state == State::open)
            watch_stall();
    }

    /// Queues one data frame, and writes it when the handshake has succeeded.
    ///
    /// @param protocol `protocol_udp` or `protocol_tcp`
    /// @param destination the overlay address the frame is for
    /// @param source_port the source port
    /// @param destination_port the destination port
    /// @param bytes the payload
    /// @return false when the tunnel cannot send, or the payload does not fit
    bool send_frame(
        uint8_t protocol,
        OverlayIp destination,
        uint16_t source_port,
        uint16_t destination_port,
        std::span<const uint8_t> bytes
    ) {
        if (stopped_ || state == State::idle || state == State::closed)
            return false;
        if (bytes.size() > max_payload_bytes)
            return false;
        DataFrame frame;
        frame.flags = flags_for_ports(source_port, destination_port);
        frame.source = self;
        frame.destination = destination;
        frame.protocol = protocol;
        frame.source_port = source_port;
        frame.destination_port = destination_port;
        frame.payload = bytes;
        std::array<uint8_t, max_record_bytes> encoded{};
        size_t encoded_len = 0;
        if (!encode_data(frame, encoded, encoded_len))
            return false;
        std::array<uint8_t, stream_header_bytes + max_record_bytes> wire{};
        size_t wire_len = 0;
        if (!stream.enqueue(std::span<const uint8_t>(encoded.data(), encoded_len), wire, wire_len))
            return false;
        if (state == State::open)
            append_bytes(std::span<const uint8_t>(wire.data(), wire_len));
        return true;
    }
};

Tunnel::Tunnel() : impl_(std::make_unique<Impl>()) {
}

Tunnel::~Tunnel() = default;

bool Tunnel::connect(
    std::string_view host,
    uint16_t port,
    OverlayIp self,
    std::span<const uint8_t> key,
    uint32_t key_id
) {
    if (host.empty() || port == 0 || key.empty())
        return false;
    impl_->host.assign(host.begin(), host.end());
    impl_->port = port;
    impl_->self = self;
    impl_->key.assign(key.begin(), key.end());
    impl_->key_id = key_id;
    impl_->stream = Stream{};
    impl_->incoming.clear();
    impl_->reorder.clear();
    impl_->reorder_since = 0;
    impl_->outage_ = false;
    impl_->lost_since_ = 0;
    impl_->stopped_ = false;
    impl_->stall_since_ = 0;
    impl_->attempt = 0;
    impl_->fault = Fault::none;
    impl_->retry_armed = false;
    impl_->begin_connect();
    return true;
}

bool Tunnel::send_datagram(
    OverlayIp destination,
    uint16_t source_port,
    uint16_t destination_port,
    std::span<const uint8_t> bytes
) {
    return impl_->send_frame(protocol_udp, destination, source_port, destination_port, bytes);
}

bool Tunnel::send_stream(
    OverlayIp destination,
    uint16_t source_port,
    uint16_t destination_port,
    std::span<const uint8_t> bytes
) {
    return impl_->send_frame(protocol_tcp, destination, source_port, destination_port, bytes);
}

void Tunnel::pump() {
    impl_->pump();
}

bool Tunnel::take(Packet& packet) {
    if (impl_->incoming.empty())
        return false;
    packet = std::move(impl_->incoming.front());
    impl_->incoming.pop_front();
    return true;
}

State Tunnel::state() const {
    return impl_->state;
}

Fault Tunnel::fault() const {
    return impl_->fault;
}

const char* Tunnel::fault_text(Fault fault) {
    switch (fault) {
    case Fault::none:
        return "none";
    case Fault::cannot_resolve:
        return "the relay address did not resolve";
    case Fault::cannot_connect:
        return "the relay connection did not open";
    case Fault::malformed_challenge:
        return "the relay's challenge was not readable";
    case Fault::too_large:
        return "a record was larger than the tunnel holds";
    case Fault::connection_lost:
        return "the relay connection ended";
    case Fault::going_away:
        return "the relay asked for a new connection";
    }
    return "unknown";
}

void Tunnel::close() {
    impl_->socket.close();
    impl_->outbound.clear();
    impl_->outbound_sent = 0;
    impl_->inbound.clear();
    impl_->reorder.clear();
    impl_->reorder_since = 0;
    impl_->stall_since_ = 0;
    impl_->retry_armed = false;
    impl_->stopped_ = true;
    impl_->state = State::closed;
}

void Tunnel::set_clock(uint64_t (*now_ms)(void* context), void* context) {
    impl_->now_ms = now_ms;
    impl_->clock_context = context;
}

} // namespace oa::boneyards::tunnel
