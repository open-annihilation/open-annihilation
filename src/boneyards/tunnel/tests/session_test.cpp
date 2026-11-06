// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Two clients through an in-process relay on 127.0.0.1. The relay speaks the
// server side of the handshake and forwards a data frame to the session whose
// overlay address is the frame's destination. Nothing here dials any other address.
#include "oa/boneyards/tunnel.hpp"
#include "oa/boneyards/tunnel/hmac.hpp"
#include "oa/boneyards/tunnel/stream.hpp"
#include "oa/netgame/stream_socket.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace tunnel = oa::boneyards::tunnel;

namespace {

constexpr char loopback_text[] = "127.0.0.1";
constexpr int poll_limit = 5000;
constexpr int io_rounds = 32;
// How long a pump loop waits when it must show that nothing arrives. Long enough
// that a frame already on the socket would have been delivered.
constexpr uint64_t quiet_ms = 300;

tunnel::OverlayIp address_of(uint8_t first, uint8_t second, uint8_t third, uint8_t fourth) {
    tunnel::OverlayIp address{};
    address.octet[0] = first;
    address.octet[1] = second;
    address.octet[2] = third;
    address.octet[3] = fourth;
    return address;
}

std::span<const uint8_t> text_bytes(std::string_view text) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::array<uint8_t, 4> ordinal_bytes(uint32_t ordinal) {
    return {
        static_cast<uint8_t>((ordinal >> 24) & 0xffu),
        static_cast<uint8_t>((ordinal >> 16) & 0xffu),
        static_cast<uint8_t>((ordinal >> 8) & 0xffu),
        static_cast<uint8_t>(ordinal & 0xffu),
    };
}

bool ordinal_of(const tunnel::Packet& packet, uint32_t& ordinal) {
    if (packet.bytes.size() != 4)
        return false;
    ordinal = (uint32_t{packet.bytes[0]} << 24) | (uint32_t{packet.bytes[1]} << 16) |
              (uint32_t{packet.bytes[2]} << 8) | uint32_t{packet.bytes[3]};
    return true;
}

bool payload_is(const tunnel::Packet& packet, std::string_view text) {
    if (packet.bytes.size() != text.size())
        return false;
    for (size_t index = 0; index < text.size(); ++index) {
        if (packet.bytes[index] != static_cast<uint8_t>(text[index]))
            return false;
    }
    return true;
}

void report_tunnel(const char* name, const tunnel::Tunnel& client) {
    std::fprintf(
        stderr,
        "%s state %u fault %s\n",
        name,
        static_cast<unsigned>(client.state()),
        tunnel::Tunnel::fault_text(client.fault())
    );
}

struct Clock {
    uint64_t now{1000000};
};

uint64_t clock_now(void* context) {
    return static_cast<Clock*>(context)->now;
}

/// One socket the relay owns. Closed when the relay drops it.
struct HeldSocket {
    intptr_t handle{oa::netgame::sock::invalid_socket};

    ~HeldSocket() { close(); }

    HeldSocket() = default;
    HeldSocket(const HeldSocket&) = delete;
    HeldSocket& operator=(const HeldSocket&) = delete;

    HeldSocket(HeldSocket&& other) noexcept : handle(other.handle) {
        other.handle = oa::netgame::sock::invalid_socket;
    }

    HeldSocket& operator=(HeldSocket&& other) noexcept {
        if (this != &other) {
            close();
            handle = other.handle;
            other.handle = oa::netgame::sock::invalid_socket;
        }
        return *this;
    }

    void close() { oa::netgame::sock::stream_close(&handle); }

    bool open() const { return handle != oa::netgame::sock::invalid_socket; }

    void adopt(intptr_t fd) {
        close();
        handle = fd;
    }
};

/// The relay's view of one client connection, before or after the handshake.
struct Conn {
    HeldSocket socket{};
    std::vector<uint8_t> inbound{};
    std::vector<uint8_t> outbound{};
    size_t sent{};
    bool pause_read{};
    bool authed{};
    std::array<uint8_t, tunnel::auth_nonce_bytes> nonce{};
    std::optional<size_t> session{};
};

/// Stream state that survives a dropped connection, keyed by overlay address.
struct Session {
    tunnel::OverlayIp overlay{};
    uint32_t key_id{};
    tunnel::Stream stream{};
    // Highest data sequence accepted from this client. A later sequence jumps
    // the frontier, which is the relay's rule. Sequence 0 is only an acknowledgement.
    uint64_t received{};
    std::optional<size_t> conn{};
    int handshakes{};
    std::vector<uint8_t> last_forward{};
    // Highest acknowledgement this client has advertised.
    uint64_t client_ack{};
    // Data records written to this client's connection.
    int forwarded{};
};

/// A data frame whose destination had no session yet.
struct HeldRecord {
    tunnel::OverlayIp destination{};
    std::vector<uint8_t> record{};
};

struct KnownKey {
    tunnel::OverlayIp overlay{};
    uint32_t key_id{};
    std::vector<uint8_t> key{};
};

enum class FrameRead : uint8_t { need_more, dropped, ready };

/// How the relay classified one inbound stream record.
///
/// Unlike the client, a sequence ahead of the frontier is accepted and the
/// frontier jumps to it. Sequence 0 is only an acknowledgement.
enum class ServerRecv : uint8_t { deliver, duplicate, bad };

/// Listens on 127.0.0.1 and forwards between authenticated sessions.
class TestRelay {
  public:

    ~TestRelay() = default;
    TestRelay() = default;
    TestRelay(const TestRelay&) = delete;
    TestRelay& operator=(const TestRelay&) = delete;

    bool listen() {
        char failure[160]{};
        uint16_t bound = 0;
        const intptr_t fd = oa::netgame::sock::stream_listen(
            oa::netgame::sock::Loopback::ipv4, 0, bound, failure, sizeof failure
        );
        if (fd == oa::netgame::sock::invalid_socket) {
            std::fprintf(stderr, "relay listen: %s\n", failure);
            return false;
        }
        listener_.adopt(fd);
        port_ = bound;
        return port_ != 0;
    }

    uint16_t port() const { return port_; }

    void add_key(tunnel::OverlayIp overlay, std::span<const uint8_t> key, uint32_t key_id) {
        KnownKey known;
        known.overlay = overlay;
        known.key_id = key_id;
        known.key.assign(key.begin(), key.end());
        keys_.push_back(std::move(known));
    }

    void arm_malformed() { malformed_once_ = true; }

    int responses_seen() const { return responses_seen_; }

    int handshakes(tunnel::OverlayIp overlay) const {
        const auto found = find_session(overlay);
        if (!found.has_value())
            return 0;
        return sessions_[*found].handshakes;
    }

    void pause_read(tunnel::OverlayIp overlay, bool pause) {
        const auto session_at = find_session(overlay);
        if (!session_at.has_value() || !sessions_[*session_at].conn.has_value())
            return;
        conns_[*sessions_[*session_at].conn].pause_read = pause;
    }

    void cut(tunnel::OverlayIp overlay) {
        const auto session_at = find_session(overlay);
        if (!session_at.has_value() || !sessions_[*session_at].conn.has_value())
            return;
        drop_conn(*sessions_[*session_at].conn);
    }

    /// Closes every connection and refuses new ones, so a client cannot resume.
    void seal() {
        refuse_ = true;
        for (size_t index = 0; index < conns_.size(); ++index)
            drop_conn(index);
    }

    /// Accepts connections again after seal.
    void unseal() { refuse_ = false; }

    /// TCP connections accepted, including ones seal closed at once.
    int accepts() const { return accepts_; }

    void send_go_away(tunnel::OverlayIp overlay) {
        const auto session_at = find_session(overlay);
        if (!session_at.has_value() || !sessions_[*session_at].conn.has_value())
            return;
        const auto body = tunnel::encode_go_away(tunnel::go_away_draining);
        append_record(*session_at, *sessions_[*session_at].conn, 0, body);
    }

    void replay_last(tunnel::OverlayIp overlay) {
        const auto session_at = find_session(overlay);
        if (!session_at.has_value() || sessions_[*session_at].last_forward.empty())
            return;
        if (!sessions_[*session_at].conn.has_value())
            return;
        append_bytes(*sessions_[*session_at].conn, sessions_[*session_at].last_forward);
    }

    /// Highest acknowledgement `overlay` has advertised for records sent to it.
    uint64_t client_ack(tunnel::OverlayIp overlay) const {
        const auto session_at = find_session(overlay);
        if (!session_at.has_value())
            return 0;
        return sessions_[*session_at].client_ack;
    }

    /// Data records this relay has written to `overlay`.
    int forwarded_to(tunnel::OverlayIp overlay) const {
        const auto session_at = find_session(overlay);
        if (!session_at.has_value())
            return 0;
        return sessions_[*session_at].forwarded;
    }

    /// Writes one datagram to `destination` with an explicit sequence.
    ///
    /// The relay's own sequence counter is left alone, so a test can deliver
    /// sequence 2 before sequence 1.
    bool inject(
        tunnel::OverlayIp destination,
        uint64_t sequence,
        tunnel::OverlayIp source,
        std::span<const uint8_t> payload
    ) {
        const auto session_at = find_session(destination);
        if (!session_at.has_value() || !sessions_[*session_at].conn.has_value())
            return false;
        tunnel::DataFrame data;
        data.source = source;
        data.destination = destination;
        data.protocol = tunnel::protocol_udp;
        data.source_port = 2300;
        data.destination_port = 2300;
        data.payload = payload;
        std::array<uint8_t, tunnel::max_record_bytes> encoded{};
        size_t encoded_len = 0;
        if (!tunnel::encode_data(data, encoded, encoded_len))
            return false;
        append_record(
            *session_at,
            *sessions_[*session_at].conn,
            sequence,
            std::span<const uint8_t>(encoded.data(), encoded_len)
        );
        return true;
    }

    void pump() {
        accept_new();
        for (size_t index = 0; index < conns_.size(); ++index) {
            if (!conns_[index].socket.open())
                continue;
            flush(index);
            if (!conns_[index].socket.open())
                continue;
            if (!conns_[index].pause_read)
                read_from(index);
            if (!conns_[index].socket.open())
                continue;
            if (!conns_[index].authed)
                authenticate(index);
            else
                process(index);
            if (index < conns_.size() && conns_[index].socket.open())
                flush(index);
        }
    }

  private:

    HeldSocket listener_{};
    uint16_t port_{};
    std::vector<KnownKey> keys_{};
    std::vector<Session> sessions_{};
    std::vector<Conn> conns_{};
    std::vector<HeldRecord> held_{};
    bool malformed_once_{};
    bool refuse_{};
    int accepts_{};
    int responses_seen_{};
    uint64_t nonce_counter_{1};

    std::optional<size_t> find_session(tunnel::OverlayIp overlay) const {
        for (size_t index = 0; index < sessions_.size(); ++index) {
            if (sessions_[index].overlay == overlay)
                return index;
        }
        return std::nullopt;
    }

    const KnownKey* find_key(tunnel::OverlayIp overlay, uint32_t key_id) const {
        for (const KnownKey& known : keys_) {
            if (known.overlay == overlay && known.key_id == key_id)
                return &known;
        }
        return nullptr;
    }

    void drop_conn(size_t conn_index) {
        if (conn_index >= conns_.size())
            return;
        conns_[conn_index].socket.close();
        conns_[conn_index].authed = false;
        if (!conns_[conn_index].session.has_value())
            return;
        Session& session = sessions_[*conns_[conn_index].session];
        if (session.conn.has_value() && *session.conn == conn_index)
            session.conn.reset();
    }

    void append_bytes(size_t conn_index, std::span<const uint8_t> bytes) {
        conns_[conn_index].outbound.insert(
            conns_[conn_index].outbound.end(), bytes.begin(), bytes.end()
        );
    }

    void append_record(
        size_t session_at, size_t conn_index, uint64_t sequence, std::span<const uint8_t> record
    ) {
        tunnel::StreamRecord frame;
        frame.sequence = sequence;
        frame.acknowledge = sessions_[session_at].received;
        frame.record = record;
        std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes> wire{};
        size_t written = 0;
        if (!tunnel::encode_stream(frame, wire, written))
            return;
        append_bytes(conn_index, std::span<const uint8_t>(wire.data(), written));
    }

    void fill_nonce(std::array<uint8_t, tunnel::auth_nonce_bytes>& nonce) {
        for (size_t index = 0; index < nonce.size(); ++index)
            nonce[index] = static_cast<uint8_t>(nonce_counter_ + index);
        ++nonce_counter_;
    }

    void push_challenge(Conn& conn) {
        std::array<uint8_t, tunnel::stream_header_bytes + tunnel::auth_challenge_bytes> wire{};
        std::array<uint8_t, tunnel::auth_challenge_bytes> body{};
        tunnel::StreamRecord frame;
        frame.sequence = 0;
        frame.acknowledge = 0;
        if (malformed_once_) {
            malformed_once_ = false;
            body[0] = tunnel::version;
            body[1] = tunnel::auth_response_kind;
            frame.record = std::span<const uint8_t>(body.data(), 2);
        } else {
            fill_nonce(conn.nonce);
            tunnel::AuthChallenge challenge;
            challenge.nonce = conn.nonce;
            body = tunnel::encode_challenge(challenge);
            frame.record = body;
        }
        size_t written = 0;
        if (!tunnel::encode_stream(frame, wire, written))
            return;
        conn.outbound.insert(
            conn.outbound.end(), wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(written)
        );
    }

    void accept_new() {
        if (!listener_.open())
            return;
        for (int round = 0; round < 8; ++round) {
            const intptr_t fd = oa::netgame::sock::stream_accept(listener_.handle);
            if (fd == oa::netgame::sock::invalid_socket)
                return;
            ++accepts_;
            if (refuse_) {
                HeldSocket discard;
                discard.adopt(fd);
                continue;
            }
            Conn conn;
            conn.socket.adopt(fd);
            push_challenge(conn);
            conns_.push_back(std::move(conn));
        }
    }

    void flush(size_t conn_index) {
        Conn& conn = conns_[conn_index];
        for (int round = 0; round < io_rounds && conn.sent < conn.outbound.size(); ++round) {
            const std::ptrdiff_t wrote = oa::netgame::sock::stream_write(
                conn.socket.handle,
                conn.outbound.data() + conn.sent,
                conn.outbound.size() - conn.sent
            );
            if (wrote < 0) {
                drop_conn(conn_index);
                return;
            }
            if (wrote == 0)
                return;
            conn.sent += static_cast<size_t>(wrote);
        }
        if (conn.sent >= conn.outbound.size()) {
            conn.outbound.clear();
            conn.sent = 0;
        }
    }

    void read_from(size_t conn_index) {
        Conn& conn = conns_[conn_index];
        for (int round = 0; round < io_rounds; ++round) {
            uint8_t buffer[4096];
            const std::ptrdiff_t got =
                oa::netgame::sock::stream_read(conn.socket.handle, buffer, sizeof buffer);
            if (got == 0)
                return;
            if (got < 0) {
                drop_conn(conn_index);
                return;
            }
            conn.inbound.insert(conn.inbound.end(), buffer, buffer + got);
        }
    }

    FrameRead take_frame(size_t conn_index, std::vector<uint8_t>& frame) {
        const auto total = tunnel::record_total_length(conns_[conn_index].inbound);
        if (!total.has_value() || conns_[conn_index].inbound.size() < *total)
            return FrameRead::need_more;
        if (*total > tunnel::stream_header_bytes + tunnel::max_record_bytes) {
            drop_conn(conn_index);
            return FrameRead::dropped;
        }
        const auto length = static_cast<std::ptrdiff_t>(*total);
        frame.assign(
            conns_[conn_index].inbound.begin(), conns_[conn_index].inbound.begin() + length
        );
        conns_[conn_index].inbound.erase(
            conns_[conn_index].inbound.begin(), conns_[conn_index].inbound.begin() + length
        );
        return FrameRead::ready;
    }

    void write_to_session(size_t dest_session, std::span<const uint8_t> inner) {
        std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes> wire{};
        size_t written = 0;
        if (!sessions_[dest_session].stream.enqueue(inner, wire, written))
            return;
        sessions_[dest_session].last_forward.assign(
            wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(written)
        );
        if (!sessions_[dest_session].conn.has_value())
            return;
        const size_t conn_index = *sessions_[dest_session].conn;
        if (!conns_[conn_index].socket.open())
            return;
        append_bytes(conn_index, std::span<const uint8_t>(wire.data(), written));
        ++sessions_[dest_session].forwarded;
    }

    void forward(size_t from_session, std::span<const uint8_t> inner) {
        tunnel::DataFrame data;
        if (tunnel::decode_data(inner, data) != tunnel::DecodeFault::none)
            return;
        if (!(data.source == sessions_[from_session].overlay))
            return;
        const auto dest = find_session(data.destination);
        if (!dest.has_value()) {
            // The destination may not have attached yet. Hold the frame and
            // deliver it when that overlay's session appears. The sender was
            // already acknowledged: the bytes live here, not in its resend ring.
            HeldRecord held;
            held.destination = data.destination;
            held.record.assign(inner.begin(), inner.end());
            held_.push_back(std::move(held));
            return;
        }
        write_to_session(*dest, inner);
    }

    void release_held(tunnel::OverlayIp overlay) {
        std::vector<HeldRecord> kept;
        kept.reserve(held_.size());
        for (HeldRecord& held : held_) {
            if (!(held.destination == overlay)) {
                kept.push_back(std::move(held));
                continue;
            }
            const auto dest = find_session(overlay);
            if (!dest.has_value()) {
                kept.push_back(std::move(held));
                continue;
            }
            write_to_session(*dest, held.record);
        }
        held_ = std::move(kept);
    }

    ServerRecv accept_server(
        Session& session, std::span<const uint8_t> frame, std::span<uint8_t> body, size_t& body_len
    ) {
        body_len = 0;
        tunnel::StreamRecord record;
        if (tunnel::decode_stream(frame, record) != tunnel::DecodeFault::none)
            return ServerRecv::bad;
        session.stream.observe_acknowledge(record.acknowledge);
        if (record.acknowledge > session.client_ack)
            session.client_ack = record.acknowledge;
        if (record.sequence == 0 || record.sequence <= session.received)
            return ServerRecv::duplicate;
        if (record.record.size() > body.size())
            return ServerRecv::bad;
        std::copy(record.record.begin(), record.record.end(), body.begin());
        body_len = record.record.size();
        session.received = record.sequence;
        return ServerRecv::deliver;
    }

    void process(size_t conn_index) {
        if (!conns_[conn_index].session.has_value())
            return;
        const size_t session_at = *conns_[conn_index].session;
        while (conns_[conn_index].socket.open()) {
            std::vector<uint8_t> frame;
            const FrameRead read = take_frame(conn_index, frame);
            if (read != FrameRead::ready)
                return;
            std::array<uint8_t, tunnel::max_record_bytes> body{};
            size_t body_len = 0;
            Session& session = sessions_[session_at];
            const uint64_t before = session.received;
            const ServerRecv result = accept_server(session, frame, body, body_len);
            if (result == ServerRecv::bad) {
                drop_conn(conn_index);
                return;
            }
            if (session.received != before)
                append_record(session_at, conn_index, 0, {});
            if (result == ServerRecv::deliver)
                forward(session_at, std::span<const uint8_t>(body.data(), body_len));
        }
    }

    void authenticate(size_t conn_index) {
        std::vector<uint8_t> frame;
        const FrameRead read = take_frame(conn_index, frame);
        if (read != FrameRead::ready)
            return;
        tunnel::StreamRecord record;
        if (tunnel::decode_stream(frame, record) != tunnel::DecodeFault::none) {
            drop_conn(conn_index);
            return;
        }
        tunnel::AuthResponse response;
        if (tunnel::decode_response(record.record, response) != tunnel::DecodeFault::none) {
            drop_conn(conn_index);
            return;
        }
        ++responses_seen_;
        const KnownKey* known = find_key(response.overlay, response.key_id);
        const auto want = known == nullptr
                              ? oa::base::sha256::Digest{}
                              : tunnel::hmac_sha256_two(
                                    known->key, tunnel::auth_context, conns_[conn_index].nonce
                                );
        if (known == nullptr || !tunnel::same_hmac(want, response.hmac)) {
            drop_conn(conn_index);
            return;
        }
        const uint64_t client_ack = record.acknowledge;
        const auto existing = find_session(response.overlay);
        size_t session_at = 0;
        bool resumed = false;
        if (!existing.has_value()) {
            Session created;
            created.overlay = response.overlay;
            created.key_id = response.key_id;
            created.stream.seed_outbound(client_ack);
            sessions_.push_back(std::move(created));
            session_at = sessions_.size() - 1;
        } else if (sessions_[*existing].key_id == response.key_id) {
            session_at = *existing;
            resumed = true;
        } else {
            session_at = *existing;
            sessions_[session_at].stream = tunnel::Stream{};
            sessions_[session_at].received = 0;
            sessions_[session_at].key_id = response.key_id;
            sessions_[session_at].stream.seed_outbound(client_ack);
        }
        if (sessions_[session_at].conn.has_value() && *sessions_[session_at].conn != conn_index)
            drop_conn(*sessions_[session_at].conn);
        sessions_[session_at].conn = conn_index;
        ++sessions_[session_at].handshakes;
        conns_[conn_index].authed = true;
        conns_[conn_index].session = session_at;
        if (resumed) {
            // The acknowledgement in the response is where the client asked
            // the replay to start. Records at or below it are not sent again.
            sessions_[session_at].stream.observe_acknowledge(client_ack);
            if (client_ack > sessions_[session_at].client_ack)
                sessions_[session_at].client_ack = client_ack;
            append_record(session_at, conn_index, 0, {});
            std::array<uint64_t, tunnel::resend_capacity> sequences{};
            size_t count = 0;
            sessions_[session_at].stream.unacknowledged_sequences(sequences, count);
            std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes> wire{};
            for (size_t index = 0; index < count; ++index) {
                size_t written = 0;
                if (!sessions_[session_at].stream.take_resend(sequences[index], wire, written))
                    continue;
                append_bytes(conn_index, std::span<const uint8_t>(wire.data(), written));
            }
        }
        process(conn_index);
        release_held(response.overlay);
    }
};

template <typename Ready>
bool wait_for(
    TestRelay& relay, tunnel::Tunnel* first, tunnel::Tunnel* second, Clock& clock, Ready ready
) {
    for (int step = 0; step < poll_limit; ++step) {
        relay.pump();
        if (first != nullptr)
            first->pump();
        if (second != nullptr)
            second->pump();
        if (ready())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        clock.now += 1;
    }
    return false;
}

void bind_clock(tunnel::Tunnel& client, Clock& clock) {
    client.set_clock(clock_now, &clock);
}

bool open_pair(
    TestRelay& relay,
    tunnel::Tunnel& first,
    tunnel::Tunnel& second,
    Clock& clock,
    tunnel::OverlayIp first_ip,
    std::string_view first_key,
    uint32_t first_id,
    tunnel::OverlayIp second_ip,
    std::string_view second_key,
    uint32_t second_id
) {
    relay.add_key(first_ip, text_bytes(first_key), first_id);
    relay.add_key(second_ip, text_bytes(second_key), second_id);
    bind_clock(first, clock);
    bind_clock(second, clock);
    if (!first.connect(loopback_text, relay.port(), first_ip, text_bytes(first_key), first_id))
        return false;
    if (!second.connect(loopback_text, relay.port(), second_ip, text_bytes(second_key), second_id))
        return false;
    return wait_for(relay, &first, &second, clock, [&] {
        return first.state() == tunnel::State::open && second.state() == tunnel::State::open;
    });
}

bool wait_until_attached(
    TestRelay& relay,
    tunnel::Tunnel& first,
    tunnel::Tunnel& second,
    Clock& clock,
    tunnel::OverlayIp first_ip,
    tunnel::OverlayIp second_ip
) {
    return wait_for(relay, &first, &second, clock, [&] {
        return relay.handshakes(first_ip) >= 1 && relay.handshakes(second_ip) >= 1;
    });
}

bool stays_quiet(TestRelay& relay, tunnel::Tunnel& first, tunnel::Tunnel& second, Clock& clock) {
    const uint64_t started = clock.now;
    bool leaked = false;
    const bool finished = wait_for(relay, &first, &second, clock, [&] {
        tunnel::Packet packet;
        if (first.take(packet) || second.take(packet)) {
            leaked = true;
            return true;
        }
        return clock.now - started >= quiet_ms;
    });
    return finished && !leaked;
}

void rejects_a_bad_request() {
    tunnel::Tunnel client;
    const auto self = address_of(10, 66, 1, 10);
    const auto key = text_bytes("alpha-key");
    OA_CHECK(!client.connect("", 1, self, key, 1));
    OA_CHECK(client.state() == tunnel::State::idle);
    OA_CHECK(!client.connect(loopback_text, 0, self, key, 1));
    OA_CHECK(!client.connect(loopback_text, 1, self, {}, 1));
    OA_CHECK(!client.send_datagram(address_of(10, 66, 1, 11), 2300, 2300, text_bytes("no")));
    OA_CHECK(!client.send_stream(address_of(10, 66, 1, 11), 2300, 2300, text_bytes("no")));
}

void exchange_datagrams_and_stream_bytes() {
    TestRelay relay;
    OA_CHECK(relay.listen());
    Clock clock;
    tunnel::Tunnel first;
    tunnel::Tunnel second;
    const auto first_ip = address_of(10, 66, 1, 10);
    const auto second_ip = address_of(10, 66, 1, 11);
    if (!open_pair(
            relay, first, second, clock, first_ip, "alpha-key", 1, second_ip, "beta-key", 2
        )) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    if (!wait_until_attached(relay, first, second, clock, first_ip, second_ip)) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }

    std::vector<uint8_t> too_big(tunnel::max_payload_bytes + 1, 0x22);
    std::vector<uint8_t> biggest(tunnel::max_payload_bytes, 0x11);
    OA_CHECK(!first.send_datagram(second_ip, 2300, 2300, too_big));
    OA_CHECK(first.send_datagram(second_ip, 2300, 2300, text_bytes("unit")));
    OA_CHECK(first.send_stream(second_ip, 47624, 100, text_bytes("orders")));
    OA_CHECK(first.send_datagram(second_ip, 2300, 2300, {}));
    OA_CHECK(first.send_datagram(second_ip, 2300, 2300, biggest));
    OA_CHECK(second.send_datagram(first_ip, 2300, 2350, text_bytes("shell")));
    OA_CHECK(second.send_stream(first_ip, 2000, 2001, text_bytes("plan")));

    std::vector<tunnel::Packet> to_second;
    std::vector<tunnel::Packet> to_first;
    const bool arrived = wait_for(relay, &first, &second, clock, [&] {
        tunnel::Packet packet;
        while (second.take(packet))
            to_second.push_back(std::move(packet));
        while (first.take(packet))
            to_first.push_back(std::move(packet));
        return to_second.size() >= 4 && to_first.size() >= 2;
    });
    if (!arrived) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        std::fprintf(stderr, "arrived %zu and %zu\n", to_second.size(), to_first.size());
        OA_CHECK(false);
        return;
    }
    OA_CHECK(to_second.size() == 4);
    OA_CHECK(to_first.size() == 2);
    OA_CHECK(payload_is(to_second[0], "unit"));
    OA_CHECK(to_second[0].channel == tunnel::Channel::datagram);
    OA_CHECK(!to_second[0].control);
    OA_CHECK(to_second[0].source == first_ip);
    OA_CHECK(to_second[0].source_port == 2300);
    OA_CHECK(to_second[0].destination_port == 2300);
    OA_CHECK(payload_is(to_second[1], "orders"));
    OA_CHECK(to_second[1].channel == tunnel::Channel::stream);
    OA_CHECK(to_second[1].control);
    OA_CHECK(to_second[1].source_port == 47624);
    OA_CHECK(to_second[1].destination_port == 100);
    OA_CHECK(to_second[2].bytes.empty());
    OA_CHECK(to_second[2].channel == tunnel::Channel::datagram);
    OA_CHECK(to_second[3].bytes.size() == tunnel::max_payload_bytes);
    OA_CHECK(to_second[3].bytes[0] == 0x11);
    OA_CHECK(payload_is(to_first[0], "shell"));
    OA_CHECK(to_first[0].channel == tunnel::Channel::datagram);
    OA_CHECK(to_first[0].destination_port == 2350);
    OA_CHECK(payload_is(to_first[1], "plan"));
    OA_CHECK(to_first[1].channel == tunnel::Channel::stream);
    OA_CHECK(!to_first[1].control);

    relay.replay_last(second_ip);
    OA_CHECK(stays_quiet(relay, first, second, clock));

    first.close();
    OA_CHECK(first.state() == tunnel::State::closed);
    OA_CHECK(!first.send_datagram(second_ip, 2300, 2300, text_bytes("late")));
}

void cut_mid_stream_delivers_once() {
    TestRelay relay;
    OA_CHECK(relay.listen());
    Clock clock;
    tunnel::Tunnel first;
    tunnel::Tunnel second;
    const auto first_ip = address_of(10, 66, 2, 10);
    const auto second_ip = address_of(10, 66, 2, 11);
    if (!open_pair(
            relay, first, second, clock, first_ip, "alpha-key", 1, second_ip, "beta-key", 2
        )) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    if (!wait_until_attached(relay, first, second, clock, first_ip, second_ip)) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    relay.pause_read(first_ip, true);
    OA_CHECK(first.send_datagram(second_ip, 2300, 2300, text_bytes("one")));
    OA_CHECK(first.send_datagram(second_ip, 2300, 2300, text_bytes("two")));
    OA_CHECK(first.send_datagram(second_ip, 2300, 2300, text_bytes("three")));
    if (!stays_quiet(relay, first, second, clock)) {
        OA_CHECK(false);
        return;
    }
    const int before = relay.handshakes(first_ip);
    relay.cut(first_ip);
    std::vector<tunnel::Packet> got;
    const bool arrived = wait_for(relay, &first, &second, clock, [&] {
        tunnel::Packet packet;
        while (second.take(packet))
            got.push_back(std::move(packet));
        return got.size() >= 3 && first.state() == tunnel::State::open;
    });
    if (!arrived) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        std::fprintf(
            stderr, "after cut %zu handshakes %d\n", got.size(), relay.handshakes(first_ip)
        );
        OA_CHECK(false);
        return;
    }
    OA_CHECK(got.size() == 3);
    OA_CHECK(payload_is(got[0], "one"));
    OA_CHECK(payload_is(got[1], "two"));
    OA_CHECK(payload_is(got[2], "three"));
    OA_CHECK(relay.handshakes(first_ip) > before);
}

void malformed_challenge_is_refused() {
    TestRelay relay;
    OA_CHECK(relay.listen());
    Clock clock;
    tunnel::Tunnel client;
    bind_clock(client, clock);
    const auto self = address_of(10, 66, 3, 10);
    relay.add_key(self, text_bytes("alpha-key"), 1);
    relay.arm_malformed();
    OA_CHECK(client.connect(loopback_text, relay.port(), self, text_bytes("alpha-key"), 1));
    const bool refused = wait_for(relay, &client, nullptr, clock, [&] {
        return client.fault() == tunnel::Fault::malformed_challenge &&
               client.state() == tunnel::State::waiting;
    });
    if (!refused) {
        report_tunnel("client", client);
        OA_CHECK(false);
        return;
    }
    OA_CHECK(relay.responses_seen() == 0);
    OA_CHECK(client.state() != tunnel::State::open);
}

void go_away_reconnects() {
    TestRelay relay;
    OA_CHECK(relay.listen());
    Clock clock;
    tunnel::Tunnel first;
    tunnel::Tunnel second;
    const auto first_ip = address_of(10, 66, 4, 10);
    const auto second_ip = address_of(10, 66, 4, 11);
    if (!open_pair(
            relay, first, second, clock, first_ip, "alpha-key", 1, second_ip, "beta-key", 2
        )) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    if (!wait_until_attached(relay, first, second, clock, first_ip, second_ip)) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    const int before = relay.handshakes(first_ip);
    const uint64_t started = clock.now;
    relay.send_go_away(first_ip);
    const bool back = wait_for(relay, &first, &second, clock, [&] {
        return relay.handshakes(first_ip) > before && first.state() == tunnel::State::open;
    });
    if (!back) {
        report_tunnel("first", first);
        OA_CHECK(false);
        return;
    }
    OA_CHECK(clock.now - started < 240);
    OA_CHECK(first.send_datagram(second_ip, 2300, 2300, text_bytes("after")));
    std::vector<tunnel::Packet> got;
    const bool arrived = wait_for(relay, &first, &second, clock, [&] {
        tunnel::Packet packet;
        while (second.take(packet))
            got.push_back(std::move(packet));
        return !got.empty();
    });
    if (!arrived) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    OA_CHECK(got.size() == 1);
    OA_CHECK(payload_is(got[0], "after"));
}

/// Sends `count` numbered datagrams from `first` to `second_ip`.
bool send_numbered(tunnel::Tunnel& first, tunnel::OverlayIp second_ip, uint32_t count) {
    for (uint32_t ordinal = 0; ordinal < count; ++ordinal) {
        const std::array<uint8_t, 4> payload = ordinal_bytes(ordinal);
        if (!first.send_datagram(second_ip, 2300, 2300, payload))
            return false;
    }
    return true;
}

void burst_between_takes_delivers_all_in_order() {
    constexpr uint32_t burst = 200;
    TestRelay relay;
    OA_CHECK(relay.listen());
    Clock clock;
    tunnel::Tunnel first;
    tunnel::Tunnel second;
    const auto first_ip = address_of(10, 66, 5, 10);
    const auto second_ip = address_of(10, 66, 5, 11);
    if (!open_pair(
            relay, first, second, clock, first_ip, "alpha-key", 1, second_ip, "beta-key", 2
        )) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    if (!wait_until_attached(relay, first, second, clock, first_ip, second_ip)) {
        OA_CHECK(false);
        return;
    }
    if (!send_numbered(first, second_ip, burst)) {
        OA_CHECK(false);
        return;
    }
    int settled = 0;
    const bool queued = wait_for(relay, &first, &second, clock, [&] {
        if (relay.forwarded_to(second_ip) < static_cast<int>(burst))
            return false;
        ++settled;
        return settled >= 8;
    });
    if (!queued) {
        std::fprintf(
            stderr,
            "burst forwarded %d ack %llu\n",
            relay.forwarded_to(second_ip),
            static_cast<unsigned long long>(relay.client_ack(second_ip))
        );
        OA_CHECK(false);
        return;
    }
    std::vector<uint32_t> got;
    tunnel::Packet packet;
    while (second.take(packet)) {
        uint32_t ordinal = 0;
        if (!ordinal_of(packet, ordinal)) {
            OA_CHECK(false);
            return;
        }
        got.push_back(ordinal);
    }
    OA_CHECK(got.size() == burst);
    for (uint32_t ordinal = 0; ordinal < got.size(); ++ordinal)
        OA_CHECK(got[ordinal] == ordinal);
    // Every acknowledged record was kept: the acknowledgement does not run past
    // the packets take returned, and nothing in the burst is missing.
    OA_CHECK(relay.client_ack(second_ip) <= static_cast<uint64_t>(got.size()));
    OA_CHECK(relay.client_ack(second_ip) == burst);
}

void full_queue_leaves_the_rest_unacknowledged() {
    constexpr uint32_t extra = 40;
    const auto total = static_cast<uint32_t>(tunnel::receive_bound + extra);
    TestRelay relay;
    OA_CHECK(relay.listen());
    Clock clock;
    tunnel::Tunnel first;
    tunnel::Tunnel second;
    const auto first_ip = address_of(10, 66, 6, 10);
    const auto second_ip = address_of(10, 66, 6, 11);
    if (!open_pair(
            relay, first, second, clock, first_ip, "alpha-key", 1, second_ip, "beta-key", 2
        )) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    if (!wait_until_attached(relay, first, second, clock, first_ip, second_ip)) {
        OA_CHECK(false);
        return;
    }
    if (!send_numbered(first, second_ip, total)) {
        OA_CHECK(false);
        return;
    }
    int settled = 0;
    const bool held = wait_for(relay, &first, &second, clock, [&] {
        if (relay.forwarded_to(second_ip) < static_cast<int>(total))
            return false;
        if (relay.client_ack(second_ip) != tunnel::receive_bound)
            return false;
        ++settled;
        return settled >= 30;
    });
    if (!held) {
        std::fprintf(
            stderr,
            "queue forwarded %d ack %llu bound %zu\n",
            relay.forwarded_to(second_ip),
            static_cast<unsigned long long>(relay.client_ack(second_ip)),
            tunnel::receive_bound
        );
        OA_CHECK(false);
        return;
    }
    std::vector<uint32_t> got;
    const bool arrived = wait_for(relay, &first, &second, clock, [&] {
        tunnel::Packet packet;
        while (second.take(packet)) {
            uint32_t ordinal = 0;
            if (!ordinal_of(packet, ordinal)) {
                OA_CHECK(false);
                return true;
            }
            got.push_back(ordinal);
        }
        return got.size() >= total;
    });
    if (!arrived) {
        std::fprintf(stderr, "queue delivered %zu of %u\n", got.size(), total);
        OA_CHECK(false);
        return;
    }
    OA_CHECK(got.size() == total);
    for (uint32_t ordinal = 0; ordinal < got.size(); ++ordinal)
        OA_CHECK(got[ordinal] == ordinal);
    int acked = 0;
    const bool complete = wait_for(relay, &first, &second, clock, [&] {
        ++acked;
        return relay.client_ack(second_ip) == total || acked >= 40;
    });
    OA_CHECK(complete);
    OA_CHECK(relay.client_ack(second_ip) == total);
}

void record_past_a_gap_is_delivered_when_the_gap_fills() {
    TestRelay relay;
    OA_CHECK(relay.listen());
    Clock clock;
    tunnel::Tunnel first;
    tunnel::Tunnel second;
    const auto first_ip = address_of(10, 66, 7, 10);
    const auto second_ip = address_of(10, 66, 7, 11);
    if (!open_pair(
            relay, first, second, clock, first_ip, "alpha-key", 1, second_ip, "beta-key", 2
        )) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    if (!wait_until_attached(relay, first, second, clock, first_ip, second_ip)) {
        OA_CHECK(false);
        return;
    }
    const int before = relay.handshakes(second_ip);
    OA_CHECK(relay.inject(second_ip, 2, first_ip, text_bytes("later")));
    OA_CHECK(relay.inject(second_ip, 1, first_ip, text_bytes("first")));
    std::vector<tunnel::Packet> got;
    const bool arrived = wait_for(relay, &first, &second, clock, [&] {
        tunnel::Packet packet;
        while (second.take(packet))
            got.push_back(std::move(packet));
        return got.size() >= 2;
    });
    if (!arrived) {
        std::fprintf(stderr, "gap fill delivered %zu\n", got.size());
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    OA_CHECK(got.size() == 2);
    OA_CHECK(payload_is(got[0], "first"));
    OA_CHECK(payload_is(got[1], "later"));
    OA_CHECK(relay.handshakes(second_ip) == before);
    OA_CHECK(second.state() == tunnel::State::open);
}

void open_gap_reconnects_without_a_new_connect() {
    TestRelay relay;
    OA_CHECK(relay.listen());
    Clock clock;
    tunnel::Tunnel first;
    tunnel::Tunnel second;
    const auto first_ip = address_of(10, 66, 8, 10);
    const auto second_ip = address_of(10, 66, 8, 11);
    if (!open_pair(
            relay, first, second, clock, first_ip, "alpha-key", 1, second_ip, "beta-key", 2
        )) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    if (!wait_until_attached(relay, first, second, clock, first_ip, second_ip)) {
        OA_CHECK(false);
        return;
    }
    const int before = relay.handshakes(second_ip);
    OA_CHECK(relay.inject(second_ip, 2, first_ip, text_bytes("early")));
    const bool resumed = wait_for(relay, &first, &second, clock, [&] {
        return relay.handshakes(second_ip) > before && second.state() == tunnel::State::open;
    });
    if (!resumed) {
        std::fprintf(stderr, "gap resume handshakes %d\n", relay.handshakes(second_ip));
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    tunnel::Packet packet;
    OA_CHECK(!second.take(packet));
}

// Nothing comes back for the stall interval while a record is unacknowledged.
// The tunnel connects again and that record arrives once.
void stall_reconnects_and_delivers_once() {
    TestRelay relay;
    OA_CHECK(relay.listen());
    Clock clock;
    tunnel::Tunnel first;
    tunnel::Tunnel second;
    const auto first_ip = address_of(10, 66, 9, 10);
    const auto second_ip = address_of(10, 66, 9, 11);
    if (!open_pair(
            relay, first, second, clock, first_ip, "alpha-key", 1, second_ip, "beta-key", 2
        )) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    if (!wait_until_attached(relay, first, second, clock, first_ip, second_ip)) {
        OA_CHECK(false);
        return;
    }
    relay.pause_read(first_ip, true);
    OA_CHECK(first.send_datagram(second_ip, 2300, 2300, text_bytes("kept")));
    const int before = relay.handshakes(first_ip);
    const uint64_t armed_at = clock.now;
    const bool armed = wait_for(relay, &first, &second, clock, [&] {
        return clock.now >= armed_at + 30 && first.state() == tunnel::State::open;
    });
    if (!armed || relay.handshakes(first_ip) != before) {
        std::fprintf(stderr, "stall armed handshakes %d\n", relay.handshakes(first_ip));
        OA_CHECK(false);
        return;
    }
    clock.now += tunnel::stall_ms;
    std::vector<tunnel::Packet> got;
    const bool arrived = wait_for(relay, &first, &second, clock, [&] {
        tunnel::Packet packet;
        while (second.take(packet))
            got.push_back(std::move(packet));
        return got.size() >= 1 && first.state() == tunnel::State::open &&
               relay.handshakes(first_ip) > before;
    });
    if (!arrived) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        std::fprintf(
            stderr, "stall delivered %zu handshakes %d\n", got.size(), relay.handshakes(first_ip)
        );
        OA_CHECK(false);
        return;
    }
    OA_CHECK(got.size() == 1);
    OA_CHECK(payload_is(got[0], "kept"));
    OA_CHECK(stays_quiet(relay, first, second, clock));
}

// A record past a gap is still delivered when the missing record arrives,
// including after the gap has connected again.
void held_gap_survives_reconnect_and_then_fills() {
    TestRelay relay;
    OA_CHECK(relay.listen());
    Clock clock;
    tunnel::Tunnel first;
    tunnel::Tunnel second;
    const auto first_ip = address_of(10, 66, 9, 20);
    const auto second_ip = address_of(10, 66, 9, 21);
    if (!open_pair(
            relay, first, second, clock, first_ip, "alpha-key", 1, second_ip, "beta-key", 2
        )) {
        report_tunnel("first", first);
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    if (!wait_until_attached(relay, first, second, clock, first_ip, second_ip)) {
        OA_CHECK(false);
        return;
    }
    const int before = relay.handshakes(second_ip);
    OA_CHECK(relay.inject(second_ip, 2, first_ip, text_bytes("later")));
    const bool resumed = wait_for(relay, &first, &second, clock, [&] {
        return relay.handshakes(second_ip) > before && second.state() == tunnel::State::open;
    });
    if (!resumed) {
        std::fprintf(stderr, "held gap handshakes %d\n", relay.handshakes(second_ip));
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    tunnel::Packet early;
    OA_CHECK(!second.take(early));
    OA_CHECK(relay.inject(second_ip, 1, first_ip, text_bytes("first")));
    std::vector<tunnel::Packet> got;
    const bool arrived = wait_for(relay, &first, &second, clock, [&] {
        tunnel::Packet packet;
        while (second.take(packet))
            got.push_back(std::move(packet));
        return got.size() >= 2;
    });
    if (!arrived) {
        std::fprintf(stderr, "held gap delivered %zu\n", got.size());
        report_tunnel("second", second);
        OA_CHECK(false);
        return;
    }
    OA_CHECK(got.size() == 2);
    OA_CHECK(payload_is(got[0], "first"));
    OA_CHECK(payload_is(got[1], "later"));
}

// The relay accepts nothing more. The tunnel retries, then reports the lost
// connection once the resume window has passed, and does not connect again.
void peer_that_never_returns_reports_the_loss() {
    TestRelay relay;
    OA_CHECK(relay.listen());
    Clock clock;
    tunnel::Tunnel client;
    bind_clock(client, clock);
    const auto self = address_of(10, 66, 9, 30);
    relay.add_key(self, text_bytes("alpha-key"), 1);
    OA_CHECK(client.connect(loopback_text, relay.port(), self, text_bytes("alpha-key"), 1));
    const bool opened = wait_for(relay, &client, nullptr, clock, [&] {
        return client.state() == tunnel::State::open;
    });
    if (!opened) {
        report_tunnel("client", client);
        OA_CHECK(false);
        return;
    }
    const int opened_accepts = relay.accepts();
    relay.seal();
    const bool retried =
        wait_for(relay, &client, nullptr, clock, [&] { return relay.accepts() > opened_accepts; });
    if (!retried) {
        report_tunnel("client", client);
        std::fprintf(stderr, "accepts %d\n", relay.accepts());
        OA_CHECK(false);
        return;
    }
    // The window is six minutes. Move the clock instead of waiting it out.
    clock.now += tunnel::resume_window_ms + 1;
    int drained = 0;
    const bool stopped = wait_for(relay, &client, nullptr, clock, [&] {
        ++drained;
        return drained >= 50 && client.fault() == tunnel::Fault::connection_lost &&
               client.state() == tunnel::State::waiting;
    });
    if (!stopped) {
        report_tunnel("client", client);
        OA_CHECK(false);
        return;
    }
    const int stopped_at = relay.accepts();
    relay.unseal();
    clock.now += 8000;
    bool grew = false;
    int after = 0;
    const bool settled = wait_for(relay, &client, nullptr, clock, [&] {
        if (relay.accepts() != stopped_at)
            grew = true;
        ++after;
        return grew || after >= 500;
    });
    OA_CHECK(settled);
    OA_CHECK(!grew);
    OA_CHECK(client.fault() == tunnel::Fault::connection_lost);
    OA_CHECK(client.state() == tunnel::State::waiting);
    OA_CHECK(
        std::strcmp(tunnel::Tunnel::fault_text(client.fault()), "the relay connection ended") == 0
    );
    OA_CHECK(!client.send_datagram(address_of(10, 66, 9, 31), 2300, 2300, text_bytes("late")));
}

} // namespace

int main() {
    rejects_a_bad_request();
    exchange_datagrams_and_stream_bytes();
    cut_mid_stream_delivers_once();
    malformed_challenge_is_refused();
    go_away_reconnects();
    burst_between_takes_delivers_all_in_order();
    full_queue_leaves_the_rest_unacknowledged();
    record_past_a_gap_is_delivered_when_the_gap_fills();
    open_gap_reconnects_without_a_new_connect();
    stall_reconnects_and_delivers_once();
    held_gap_survives_reconnect_and_then_fills();
    peer_that_never_returns_reports_the_loss();
    return oa::test::check_exit_status();
}
