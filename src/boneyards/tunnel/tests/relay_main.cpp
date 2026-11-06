// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A relay for one launch check. It listens on 127.0.0.1, speaks the server
// side of the handshake, and forwards a data frame to the session whose
// overlay address is the frame's destination. It dials no address.
//
// Arguments, repeated: OVERLAY KEY_ID HEXKEY. It prints the bound port, then
// one line of per-source forward counts whenever those counts change, and
// the same line again before it exits on SIGTERM or SIGINT.

#include "oa/boneyards/tunnel/hmac.hpp"
#include "oa/boneyards/tunnel/stream.hpp"
#include "oa/boneyards/tunnel/wire.hpp"
#include "oa/netgame/stream_socket.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace tunnel = oa::boneyards::tunnel;

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) {
    g_stop = 1;
}

constexpr int io_rounds = 8;

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

struct Conn {
    HeldSocket socket{};
    std::vector<uint8_t> inbound{};
    std::vector<uint8_t> outbound{};
    std::size_t sent{};
    bool authed{};
    std::array<uint8_t, tunnel::auth_nonce_bytes> nonce{};
    std::optional<std::size_t> session{};
};

struct Session {
    tunnel::OverlayIp overlay{};
    uint32_t key_id{};
    tunnel::Stream stream{};
    // Highest data sequence accepted from this client. A later sequence jumps
    // the frontier. Sequence 0 is only an acknowledgement.
    uint64_t received{};
    std::optional<std::size_t> conn{};
};

struct HeldRecord {
    tunnel::OverlayIp destination{};
    std::vector<uint8_t> record{};
};

struct KnownKey {
    tunnel::OverlayIp overlay{};
    uint32_t key_id{};
    std::vector<uint8_t> key{};
    uint64_t forwards{};
};

enum class FrameRead : uint8_t { need_more, dropped, ready };

enum class ServerRecv : uint8_t { deliver, duplicate, bad };

/// Listens on 127.0.0.1 and forwards between the overlays it was given.
class Relay {
  public:

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

    bool add_key(tunnel::OverlayIp overlay, std::span<const uint8_t> key, uint32_t key_id) {
        if (key.empty())
            return false;
        KnownKey known;
        known.overlay = overlay;
        known.key_id = key_id;
        known.key.assign(key.begin(), key.end());
        keys_.push_back(std::move(known));
        return true;
    }

    void pump() {
        accept_new();
        for (std::size_t index = 0; index < conns_.size(); ++index) {
            if (!conns_[index].socket.open())
                continue;
            flush(index);
            if (!conns_[index].socket.open())
                continue;
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

    /// True once a forward count has changed since the last print.
    bool counts_changed() const { return changed_; }

    /// Prints every configured overlay and how many data frames left it.
    void print_counts() {
        std::string line = "relay forwards";
        for (const KnownKey& known : keys_) {
            line += ' ';
            append_ip(line, known.overlay);
            line += ' ';
            line += std::to_string(known.forwards);
        }
        line += '\n';
        std::fputs(line.c_str(), stdout);
        std::fflush(stdout);
        changed_ = false;
    }

  private:

    HeldSocket listener_{};
    uint16_t port_{};
    std::vector<KnownKey> keys_{};
    std::vector<Session> sessions_{};
    std::vector<Conn> conns_{};
    std::vector<HeldRecord> held_{};
    uint64_t nonce_counter_{1};
    bool changed_{};

    static void append_ip(std::string& line, tunnel::OverlayIp ip) {
        for (int index = 0; index < 4; ++index) {
            if (index != 0)
                line += '.';
            line += std::to_string(static_cast<unsigned>(ip.octet[index]));
        }
    }

    std::optional<std::size_t> find_session(tunnel::OverlayIp overlay) const {
        for (std::size_t index = 0; index < sessions_.size(); ++index) {
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

    void note_forward(tunnel::OverlayIp source) {
        for (KnownKey& known : keys_) {
            if (!(known.overlay == source))
                continue;
            ++known.forwards;
            changed_ = true;
            return;
        }
    }

    void drop_conn(std::size_t conn_index) {
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

    void append_bytes(std::size_t conn_index, std::span<const uint8_t> bytes) {
        conns_[conn_index].outbound.insert(
            conns_[conn_index].outbound.end(), bytes.begin(), bytes.end()
        );
    }

    void append_record(
        std::size_t session_at,
        std::size_t conn_index,
        uint64_t sequence,
        std::span<const uint8_t> record
    ) {
        tunnel::StreamRecord frame;
        frame.sequence = sequence;
        frame.acknowledge = sessions_[session_at].received;
        frame.record = record;
        std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes> wire{};
        std::size_t written = 0;
        if (!tunnel::encode_stream(frame, wire, written))
            return;
        append_bytes(conn_index, std::span<const uint8_t>(wire.data(), written));
    }

    void fill_nonce(std::array<uint8_t, tunnel::auth_nonce_bytes>& nonce) {
        for (std::size_t index = 0; index < nonce.size(); ++index)
            nonce[index] = static_cast<uint8_t>(nonce_counter_ + index);
        ++nonce_counter_;
    }

    void push_challenge(Conn& conn) {
        std::array<uint8_t, tunnel::stream_header_bytes + tunnel::auth_challenge_bytes> wire{};
        tunnel::AuthChallenge challenge;
        fill_nonce(conn.nonce);
        challenge.nonce = conn.nonce;
        const std::array<uint8_t, tunnel::auth_challenge_bytes> body =
            tunnel::encode_challenge(challenge);
        tunnel::StreamRecord frame;
        frame.sequence = 0;
        frame.acknowledge = 0;
        frame.record = body;
        std::size_t written = 0;
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
            Conn conn;
            conn.socket.adopt(fd);
            push_challenge(conn);
            conns_.push_back(std::move(conn));
        }
    }

    void flush(std::size_t conn_index) {
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
            conn.sent += static_cast<std::size_t>(wrote);
        }
        if (conn.sent >= conn.outbound.size()) {
            conn.outbound.clear();
            conn.sent = 0;
        }
    }

    void read_from(std::size_t conn_index) {
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
            conn.inbound.insert(
                conn.inbound.end(), buffer, buffer + static_cast<std::ptrdiff_t>(got)
            );
        }
    }

    FrameRead take_frame(std::size_t conn_index, std::vector<uint8_t>& frame) {
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

    bool write_to_session(std::size_t dest_session, std::span<const uint8_t> inner) {
        std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes> wire{};
        std::size_t written = 0;
        if (!sessions_[dest_session].stream.enqueue(inner, wire, written))
            return false;
        if (!sessions_[dest_session].conn.has_value())
            return true;
        const std::size_t conn_index = *sessions_[dest_session].conn;
        if (conn_index >= conns_.size() || !conns_[conn_index].socket.open())
            return true;
        append_bytes(conn_index, std::span<const uint8_t>(wire.data(), written));
        return true;
    }

    void forward(std::size_t from_session, std::span<const uint8_t> inner) {
        tunnel::DataFrame data;
        if (tunnel::decode_data(inner, data) != tunnel::DecodeFault::none)
            return;
        if (!(data.source == sessions_[from_session].overlay))
            return;
        const auto dest = find_session(data.destination);
        if (!dest.has_value()) {
            // The destination may not have attached yet. Hold the frame and
            // deliver it when that overlay's session appears. The sender was
            // already acknowledged.
            HeldRecord held;
            held.destination = data.destination;
            held.record.assign(inner.begin(), inner.end());
            held_.push_back(std::move(held));
            return;
        }
        if (write_to_session(*dest, inner))
            note_forward(data.source);
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
            if (!write_to_session(*dest, held.record)) {
                kept.push_back(std::move(held));
                continue;
            }
            tunnel::DataFrame data;
            if (tunnel::decode_data(held.record, data) == tunnel::DecodeFault::none)
                note_forward(data.source);
        }
        held_ = std::move(kept);
    }

    ServerRecv accept_server(
        Session& session,
        std::span<const uint8_t> frame,
        std::span<uint8_t> body,
        std::size_t& body_len
    ) {
        body_len = 0;
        tunnel::StreamRecord record;
        if (tunnel::decode_stream(frame, record) != tunnel::DecodeFault::none)
            return ServerRecv::bad;
        session.stream.observe_acknowledge(record.acknowledge);
        if (record.sequence == 0 || record.sequence <= session.received)
            return ServerRecv::duplicate;
        if (record.record.size() > body.size())
            return ServerRecv::bad;
        std::copy(record.record.begin(), record.record.end(), body.begin());
        body_len = record.record.size();
        session.received = record.sequence;
        return ServerRecv::deliver;
    }

    void process(std::size_t conn_index) {
        if (!conns_[conn_index].session.has_value())
            return;
        const std::size_t session_at = *conns_[conn_index].session;
        while (conns_[conn_index].socket.open()) {
            std::vector<uint8_t> frame;
            const FrameRead read = take_frame(conn_index, frame);
            if (read != FrameRead::ready)
                return;
            std::array<uint8_t, tunnel::max_record_bytes> body{};
            std::size_t body_len = 0;
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

    void authenticate(std::size_t conn_index) {
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
        const KnownKey* known = find_key(response.overlay, response.key_id);
        if (known == nullptr) {
            drop_conn(conn_index);
            return;
        }
        const oa::base::sha256::Digest want =
            tunnel::hmac_sha256_two(known->key, tunnel::auth_context, conns_[conn_index].nonce);
        if (!tunnel::same_hmac(want, response.hmac)) {
            drop_conn(conn_index);
            return;
        }
        const uint64_t client_ack = record.acknowledge;
        const auto existing = find_session(response.overlay);
        std::size_t session_at = 0;
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
        conns_[conn_index].authed = true;
        conns_[conn_index].session = session_at;
        if (resumed) {
            // The acknowledgement in the response is where the client asked
            // the replay to start. Records at or below it are not sent again.
            sessions_[session_at].stream.observe_acknowledge(client_ack);
            append_record(session_at, conn_index, 0, {});
            std::array<uint64_t, tunnel::resend_capacity> sequences{};
            std::size_t count = 0;
            sessions_[session_at].stream.unacknowledged_sequences(sequences, count);
            std::array<uint8_t, tunnel::stream_header_bytes + tunnel::max_record_bytes> wire{};
            for (std::size_t index = 0; index < count; ++index) {
                std::size_t written = 0;
                if (!sessions_[session_at].stream.take_resend(sequences[index], wire, written))
                    continue;
                append_bytes(conn_index, std::span<const uint8_t>(wire.data(), written));
            }
        }
        process(conn_index);
        release_held(response.overlay);
    }
};

int hex_value(char digit) {
    if (digit >= '0' && digit <= '9')
        return digit - '0';
    if (digit >= 'a' && digit <= 'f')
        return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F')
        return digit - 'A' + 10;
    return -1;
}

bool parse_hex(std::string_view text, std::vector<uint8_t>& out) {
    if (text.empty() || (text.size() % 2) != 0 || text.size() / 2 > 64)
        return false;
    out.clear();
    out.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2) {
        const int high = hex_value(text[index]);
        const int low = hex_value(text[index + 1]);
        if (high < 0 || low < 0)
            return false;
        out.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return true;
}

bool parse_ipv4(std::string_view text, tunnel::OverlayIp& ip) {
    std::size_t at = 0;
    for (int index = 0; index < 4; ++index) {
        if (at >= text.size() || text[at] < '0' || text[at] > '9')
            return false;
        unsigned value = 0;
        int digits = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            value = value * 10u + static_cast<unsigned>(text[at] - '0');
            if (value > 255u)
                return false;
            ++at;
            ++digits;
            if (digits > 3)
                return false;
        }
        ip.octet[index] = static_cast<uint8_t>(value);
        if (index < 3) {
            if (at >= text.size() || text[at] != '.')
                return false;
            ++at;
        }
    }
    return at == text.size();
}

bool parse_key_id(std::string_view text, uint32_t& key_id) {
    if (text.empty())
        return false;
    uint32_t value = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9')
            return false;
        const uint32_t next = static_cast<uint32_t>(digit - '0');
        if (value > (UINT32_MAX - next) / 10u)
            return false;
        value = value * 10u + next;
    }
    key_id = value;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4 || ((argc - 1) % 3) != 0) {
        std::fprintf(stderr, "usage: relay OVERLAY KEY_ID HEXKEY ...\n");
        return 1;
    }
    Relay relay;
    for (int index = 1; index < argc; index += 3) {
        tunnel::OverlayIp overlay{};
        uint32_t key_id = 0;
        std::vector<uint8_t> key;
        if (!parse_ipv4(argv[index], overlay) || !parse_key_id(argv[index + 1], key_id) ||
            !parse_hex(argv[index + 2], key) || !relay.add_key(overlay, key, key_id)) {
            std::fprintf(stderr, "relay: bad identity %s\n", argv[index]);
            return 1;
        }
    }
    if (!relay.listen())
        return 1;
    std::printf("relay port %u\n", static_cast<unsigned>(relay.port()));
    std::fflush(stdout);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
    while (g_stop == 0) {
        relay.pump();
        if (relay.counts_changed())
            relay.print_counts();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    relay.print_counts();
    return 0;
}
