// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/dplay.hpp"
#include "oa/netgame/overlay_route.hpp"
#include "oa/netgame/socket_host.hpp"
#include "oa/netgame/stream_socket.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if defined(OA_WINDOWS_95)
// Windows 95 ships Winsock 1.1, and its headers: Winsock 2's names sockets,
// interfaces and name resolution differently and arrived with an update to
// 95 and with Windows 98.
#include <winsock.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
using socket_len = int;
using native_socket = SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_len = socklen_t;
using native_socket = int;
#endif

namespace oa::netgame::sock {
namespace {

constexpr std::size_t datagram_buffer_bytes = dplay::max_datagram_bytes + 64;
constexpr int datagrams_per_pump = 64;
constexpr int accept_backlog = 16;
constexpr std::size_t max_wait_entries = 3 + max_connections;
constexpr std::size_t max_local_interfaces = 64;
constexpr uint8_t this_machine_ip[4] = {127, 0, 0, 1};
constexpr std::size_t header_ip_offset = 8;   // IPv4 address inside a message header's sockaddr
constexpr std::size_t max_search_bytes = 256; // an enumeration request is far shorter

/// Returns an IPv4 address as one number, first byte highest.
///
/// @param ip the address, in the order it is written
/// @return the address as a number; 0.0.0.0 is 0
uint32_t ipv4_value(const uint8_t ip[4]) {
    return static_cast<uint32_t>(ip[0]) << 24 | static_cast<uint32_t>(ip[1]) << 16 |
           static_cast<uint32_t>(ip[2]) << 8 | static_cast<uint32_t>(ip[3]);
}

/// Writes an IPv4 address from the number ipv4_value makes of it.
///
/// @param value the address as a number
/// @param[out] ip the address, in the order it is written
void store_ipv4(uint32_t value, uint8_t ip[4]) {
    ip[0] = static_cast<uint8_t>(value >> 24);
    ip[1] = static_cast<uint8_t>(value >> 16);
    ip[2] = static_cast<uint8_t>(value >> 8);
    ip[3] = static_cast<uint8_t>(value);
}

/// Reads an IPv4 address written in one of its numeric forms: one to four
/// parts separated by dots, each decimal, octal with a leading 0 or
/// hexadecimal with a leading 0x. The last part fills the bytes the
/// earlier parts leave, so 127.1 is 127.0.0.1 and 10.1.2 is 10.1.0.2.
///
/// @param text the address, without surrounding white space
/// @param[out] value the address as ipv4_value makes it; unchanged on failure
/// @return false when text is not an address in one of these forms
bool parse_numeric_ipv4(const char* text, uint32_t& value) {
    constexpr int max_parts = 4;
    constexpr uint64_t max_value = 0xffffffff;
    constexpr uint64_t max_byte = 0xff;
    uint64_t parts[max_parts]{};
    int count = 0;
    const char* at = text;
    for (;;) {
        if (count == max_parts || *at < '0' || *at > '9')
            return false;
        uint64_t base = 10;
        if (at[0] == '0' && (at[1] == 'x' || at[1] == 'X')) {
            base = 16;
            at += 2;
        } else if (at[0] == '0') {
            base = 8;
        }
        uint64_t part = 0;
        int digits = 0;
        for (;; ++at, ++digits) {
            const char c = *at;
            uint64_t digit = 0;
            if (c >= '0' && c <= '9')
                digit = static_cast<uint64_t>(c - '0');
            else if (base == 16 && c >= 'a' && c <= 'f')
                digit = static_cast<uint64_t>(c - 'a' + 10);
            else if (base == 16 && c >= 'A' && c <= 'F')
                digit = static_cast<uint64_t>(c - 'A' + 10);
            else
                break;
            if (digit >= base)
                return false;
            part = part * base + digit;
            if (part > max_value)
                return false;
        }
        if (digits == 0)
            return false;
        parts[count++] = part;
        if (*at == '\0')
            break;
        if (*at != '.')
            return false;
        ++at;
    }
    const int leading = count - 1;
    for (int i = 0; i < leading; ++i)
        if (parts[i] > max_byte)
            return false;
    if (parts[leading] > (max_value >> (8 * leading)))
        return false;
    uint64_t result = parts[leading];
    for (int i = 0; i < leading; ++i)
        result |= parts[i] << (24 - 8 * i);
    value = static_cast<uint32_t>(result);
    return true;
}

native_socket native(intptr_t fd) {
    return static_cast<native_socket>(fd);
}

bool socket_valid(intptr_t fd) {
    return fd != invalid_socket;
}

// What wait_for_sockets watches a socket for, and what it found ready.
constexpr unsigned socket_readable = 1u << 0;
constexpr unsigned socket_writable = 1u << 1;
constexpr unsigned socket_failed = 1u << 2; // an error or a hang-up
// Not an open socket. Only poll reports it; select on Windows has no such
// result.
[[maybe_unused]] constexpr unsigned socket_invalid = 1u << 3;

// One socket host_pump waits on.
struct SocketWait {
    native_socket fd{};
    unsigned wanted = 0; // socket_readable and socket_writable
    unsigned found = 0;  // the socket_* bits that are ready
};

/// Waits until one of the sockets is ready or the time runs out, and
/// records in each entry's found what is ready on it.
///
/// @param waits the sockets and what to wait for on each
/// @param count entries at waits, at most max_wait_entries
/// @param wait_ms how long to wait, in milliseconds
/// @return the number of entries with something ready; 0 when the time ran
///         out, below 0 on failure
int wait_for_sockets(SocketWait* waits, std::size_t count, uint32_t wait_ms) {
#ifdef _WIN32
    // select, not WSAPoll, which Windows XP does not have. A connection
    // that fails to open is reported in the exception set.
    static_assert(max_wait_entries <= FD_SETSIZE);
    fd_set readable;
    fd_set writable;
    fd_set failed;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    for (std::size_t i = 0; i < count; ++i) {
        if ((waits[i].wanted & socket_readable) != 0)
            FD_SET(waits[i].fd, &readable);
        if ((waits[i].wanted & socket_writable) != 0)
            FD_SET(waits[i].fd, &writable);
        FD_SET(waits[i].fd, &failed);
    }
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(wait_ms / 1000);
    timeout.tv_usec = static_cast<long>(wait_ms % 1000 * 1000);
    const int selected = select(0, &readable, &writable, &failed, &timeout);
    if (selected <= 0) {
        for (std::size_t i = 0; i < count; ++i)
            waits[i].found = 0;
        return selected;
    }
    int ready = 0;
    for (std::size_t i = 0; i < count; ++i) {
        unsigned found = 0;
        if (FD_ISSET(waits[i].fd, &readable))
            found |= socket_readable;
        if (FD_ISSET(waits[i].fd, &writable))
            found |= socket_writable;
        if (FD_ISSET(waits[i].fd, &failed))
            found |= socket_failed;
        waits[i].found = found;
        ready += found != 0 ? 1 : 0;
    }
    return ready;
#else
    pollfd fds[max_wait_entries]{};
    for (std::size_t i = 0; i < count; ++i) {
        fds[i].fd = waits[i].fd;
        fds[i].events = static_cast<short>(
            ((waits[i].wanted & socket_readable) != 0 ? POLLIN : 0) |
            ((waits[i].wanted & socket_writable) != 0 ? POLLOUT : 0)
        );
    }
    const int ready = poll(fds, static_cast<nfds_t>(count), static_cast<int>(wait_ms));
    for (std::size_t i = 0; i < count; ++i) {
        const short revents = ready > 0 ? fds[i].revents : short{0};
        unsigned found = 0;
        if ((revents & POLLIN) != 0)
            found |= socket_readable;
        if ((revents & POLLOUT) != 0)
            found |= socket_writable;
        if ((revents & (POLLERR | POLLHUP)) != 0)
            found |= socket_failed;
        if ((revents & POLLNVAL) != 0)
            found |= socket_invalid;
        waits[i].found = found;
    }
    return ready;
#endif
}

bool platform_startup() {
#ifdef _WIN32
    static bool started = false;
    if (!started) {
        WSADATA data;
        started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    return started;
#else
    return true;
#endif
}

void close_socket(intptr_t* fd) {
    if (!socket_valid(*fd))
        return;
#ifdef _WIN32
    closesocket(native(*fd));
#else
    close(native(*fd));
#endif
    *fd = invalid_socket;
}

bool would_block() {
#ifdef _WIN32
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EINTR;
#endif
}

bool set_nonblocking(intptr_t fd) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(native(fd), FIONBIO, &on) == 0;
#else
    const int flags = fcntl(native(fd), F_GETFL, 0);
    return flags >= 0 && fcntl(native(fd), F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void set_option(intptr_t fd, int level, int name, int value) {
    (void)setsockopt(native(fd), level, name, reinterpret_cast<const char*>(&value), sizeof value);
}

intptr_t open_socket(int type) {
    const native_socket s = socket(AF_INET, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET)
        return invalid_socket;
#else
    if (s < 0)
        return invalid_socket;
#endif
    const auto fd = static_cast<intptr_t>(s);
    if (!set_nonblocking(fd)) {
        intptr_t tmp = fd;
        close_socket(&tmp);
        return invalid_socket;
    }
#ifdef SO_NOSIGPIPE
    set_option(fd, SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
    return fd;
}

sockaddr_in to_sockaddr(const Address& a) {
    sockaddr_in s{};
    s.sin_family = AF_INET;
    s.sin_port = htons(a.port);
    std::memcpy(&s.sin_addr, a.ip, 4);
    return s;
}

Address from_sockaddr(const sockaddr_in& s) {
    Address a{};
    a.port = ntohs(s.sin_port);
    std::memcpy(a.ip, &s.sin_addr, 4);
    return a;
}

/// The last port a bind refused, and the system code captured at that bind.
struct BindMiss {
    uint16_t port{};
    int code{};
    bool failed{};
};

/// The system code from the socket call that just returned.
///
/// @return the code, unchanged until the next socket call
int last_socket_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

/// Writes the system's text for a socket failure, as one line with no trailing break.
///
/// @param code the code last_socket_error returned
/// @param[out] out receives the text and a zero byte
/// @param size the bytes `out` holds
void system_socket_reason(int code, char* out, std::size_t size) {
    if (size == 0)
        return;
    out[0] = '\0';
#ifdef _WIN32
#if defined(OA_WINDOWS_95)
    // Windows 95's socket headers name the code; the message table is not
    // linked with that socket library.
    if (code == WSAEADDRINUSE)
        std::snprintf(out, size, "Address already in use");
    else
        std::snprintf(out, size, "system error %d", code);
#else
    char text[160];
    const DWORD written = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(code),
        0,
        text,
        static_cast<DWORD>(sizeof text),
        nullptr
    );
    std::size_t length = written;
    if (length >= sizeof text)
        length = sizeof text - 1;
    while (length > 0 && (text[length - 1] == '\r' || text[length - 1] == '\n' ||
                          text[length - 1] == ' ' || text[length - 1] == '\t'))
        --length;
    text[length] = '\0';
    if (length == 0)
        std::snprintf(out, size, "system error %d", code);
    else
        std::snprintf(out, size, "%s", text);
#endif
#else
    const char* text = std::strerror(code);
    if (text == nullptr || text[0] == '\0')
        std::snprintf(out, size, "system error %d", code);
    else
        std::snprintf(out, size, "%s", text);
#endif
}

/// Binds the first free port of [first, last]. A first of 0 binds a port the system chooses.
///
/// @param fd socket to bind
/// @param ip address to bind
/// @param first first port of the range; 0 asks the system for one
/// @param last last port of the range
/// @param[out] miss the last port the system refused, and its code, when none bound
/// @return the bound port, or 0 when none bound
uint16_t
bind_range(intptr_t fd, const uint8_t ip[4], uint16_t first, uint16_t last, BindMiss* miss) {
    miss->failed = false;
    miss->port = 0;
    miss->code = 0;
    if (first == 0)
        last = 0;
    for (uint32_t port = first; port <= last; ++port) {
        Address a{};
        std::memcpy(a.ip, ip, 4);
        a.port = static_cast<uint16_t>(port);
        const sockaddr_in s = to_sockaddr(a);
        if (bind(native(fd), reinterpret_cast<const sockaddr*>(&s), sizeof s) == 0) {
            sockaddr_in bound{};
            socket_len len = sizeof bound;
            if (getsockname(native(fd), reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
                miss->failed = true;
                miss->port = static_cast<uint16_t>(port);
                miss->code = last_socket_error();
                return 0;
            }
            return ntohs(bound.sin_port);
        }
        miss->failed = true;
        miss->port = static_cast<uint16_t>(port);
        miss->code = last_socket_error();
    }
    return 0;
}

/// Writes one standard-error line naming the ports a host could not bind, and the same words in its error.
///
/// @param[in,out] h host whose error text is set
/// @param step which socket was being bound: enumeration, stream or datagram
/// @param first first port of the range that was tried; 0 asks the system for one
/// @param last last port of the range that was tried
/// @param miss the last refusal, or a miss that never failed when the range was empty
void report_refused_bind(
    Host* h, const char* step, uint16_t first, uint16_t last, const BindMiss& miss
) {
    char reason[128];
    if (!miss.failed)
        std::snprintf(reason, sizeof reason, "no port in the range");
    else
        system_socket_reason(miss.code, reason, sizeof reason);
    if (first == 0 || first == last) {
        const unsigned shown =
            miss.failed ? static_cast<unsigned>(miss.port) : static_cast<unsigned>(first);
        std::snprintf(h->error, sizeof h->error, "%s port %u: %s", step, shown, reason);
    } else {
        std::snprintf(
            h->error,
            sizeof h->error,
            "%s ports %u-%u: %s",
            step,
            static_cast<unsigned>(first),
            static_cast<unsigned>(last),
            reason
        );
    }
    std::fprintf(stderr, "multiplayer: hosting failed while binding the %s\n", h->error);
}

int send_flags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

void fail(Host* h, const char* what) {
    std::snprintf(h->error, sizeof h->error, "%s", what);
    std::fprintf(stderr, "multiplayer: hosting failed while %s\n", h->error);
}

// Buffers are left as they are: the struct is too large to copy.
void init_connection(Connection* c, intptr_t fd, const Address& peer) {
    c->fd = fd;
    c->outbound = c->connecting = c->failed = false;
    c->carried = c->paired = false;
    c->pair_slot = 0;
    c->local_port = 0;
    c->peer = peer;
    c->target = Address{};
    c->rx_used = c->tx_used = 0;
}

void reset_connection(Host* h, Connection* c) {
    if (c->carried) {
        if (c->paired && c->pair_slot < max_connections) {
            Connection& other = h->connections[c->pair_slot];
            if (&other != c)
                other.paired = false;
        }
        init_connection(c, invalid_socket, Address{});
        return;
    }
    if (socket_valid(c->fd))
        ++h->dropped_connections;
    close_socket(&c->fd);
    init_connection(c, invalid_socket, Address{});
}

/// Adds bytes to, or takes them from, the count of bytes in flight between
/// the hosts that share a HostClock with one.
///
/// @param[in,out] h host that sent or received them
/// @param bytes bytes handed to the system (positive) or read from it (negative)
void note_in_flight(Host* h, int64_t bytes) {
    if (h->config.clock.in_flight != nullptr)
        *h->config.clock.in_flight = std::max<int64_t>(0, *h->config.clock.in_flight + bytes);
}

void flush(Host* h, Connection* c) {
    while (c->tx_used != 0 && !c->connecting && !c->failed) {
        const auto sent = send(
            native(c->fd),
            reinterpret_cast<const char*>(c->tx),
            static_cast<int>(c->tx_used),
            send_flags()
        );
        if (sent < 0) {
            if (!would_block())
                c->failed = true;
            return;
        }
        note_in_flight(h, sent);
        if (sent > 0) {
            h->last_stream_send_ms = host_now_ms(h);
            h->stream_sent = true;
        }
        std::memmove(c->tx, c->tx + sent, c->tx_used - static_cast<uint32_t>(sent));
        c->tx_used -= static_cast<uint32_t>(sent);
    }
}

Connection* free_connection(Host* h) {
    for (auto& c : h->connections)
        if (!socket_valid(c.fd) && !c.carried)
            return &c;
    return nullptr;
}

/// True when the installed route carries this address.
///
/// @param ip the address
/// @return false when no route is installed or it does not claim ip
bool route_claims(const uint8_t ip[4]) {
    const OverlayRoute* route = overlay_route();
    return route != nullptr && route->claims != nullptr && route->claims(route->context, ip);
}

/// True when ip is the installed route's own address.
///
/// @param ip the address
/// @return false when no route is installed or the addresses differ
bool route_is_self(const uint8_t ip[4]) {
    const OverlayRoute* route = overlay_route();
    if (route == nullptr || route->self_address == nullptr || ip == nullptr)
        return false;
    uint8_t self[4]{};
    if (!route->self_address(route->context, self))
        return false;
    return std::memcmp(self, ip, 4) == 0;
}

/// Returns the port a socket is bound to.
///
/// @param fd the socket
/// @param fallback the port to return when the system names none
/// @return the bound port, or fallback
uint16_t bound_port(intptr_t fd, uint16_t fallback) {
    if (!socket_valid(fd))
        return fallback;
    sockaddr_in local{};
    socket_len length = sizeof local;
    if (getsockname(native(fd), reinterpret_cast<sockaddr*>(&local), &length) != 0)
        return fallback;
    return ntohs(local.sin_port);
}

constexpr std::size_t max_route_hosts = 8;
constexpr std::size_t max_looped_datagrams = 16;
constexpr int overlay_rounds = 8;

/// One datagram queued for this host, delivered on a later pump.
struct LoopedDatagram {
    uint8_t source[4]{};
    uint16_t source_port{};
    uint8_t bytes[dplay::max_datagram_bytes]{};
    std::size_t size{};
};

/// Loopback queue and ephemeral ports for one host. The host struct stays
/// as it is; several hosts exist in one process.
struct HostRouteState {
    Host* host{};
    LoopedDatagram looped[max_looped_datagrams]{};
    uint8_t looped_head{};
    uint8_t looped_count{};
    uint16_t next_ephemeral{40000};
};

HostRouteState g_route_states[max_route_hosts]{};

/// Returns the route state for a host, creating it when this host has none.
///
/// @param h the host
/// @return the state, or null when every slot is in use
HostRouteState* route_state(Host* h) {
    HostRouteState* free_slot = nullptr;
    for (auto& slot : g_route_states) {
        if (slot.host == h)
            return &slot;
        if (slot.host == nullptr && free_slot == nullptr)
            free_slot = &slot;
    }
    if (free_slot == nullptr)
        return nullptr;
    free_slot->host = h;
    free_slot->looped_head = 0;
    free_slot->looped_count = 0;
    free_slot->next_ephemeral = 40000;
    return free_slot;
}

/// Returns the route state for a host.
///
/// @param h the host
/// @return the state, or null when this host has none
HostRouteState* find_route_state(Host* h) {
    for (auto& slot : g_route_states)
        if (slot.host == h)
            return &slot;
    return nullptr;
}

/// Forgets a host's loopback queue.
///
/// @param h the host
void drop_route_state(Host* h) {
    for (auto& slot : g_route_states) {
        if (slot.host == h) {
            slot.host = nullptr;
            slot.looped_head = 0;
            slot.looped_count = 0;
            slot.next_ephemeral = 40000;
        }
    }
}

/// Appends bytes to a connection's receive buffer.
///
/// @param[in,out] connection the connection; failed when the bytes do not fit
/// @param bytes the bytes
/// @param size how many
void append_rx(Connection* connection, const uint8_t* bytes, std::size_t size) {
    if (size > connection_rx_bytes - connection->rx_used) {
        connection->failed = true;
        return;
    }
    if (size != 0 && bytes != nullptr)
        std::memcpy(connection->rx + connection->rx_used, bytes, size);
    connection->rx_used += static_cast<uint32_t>(size);
}

/// Hands complete stream messages in a connection's receive buffer to the engine.
///
/// @param[in,out] h the host
/// @param[in,out] connection the connection; failed when a header is malformed
void consume_stream_messages(Host* h, Connection* connection) {
    while (connection->rx_used >= 4 && !connection->failed) {
        const std::size_t size = dplay::stream_message_size(connection->rx);
        if (size == 0) {
            connection->failed = true;
            return;
        }
        if (connection->rx_used < size)
            break;
        dplay::engine_on_stream(&h->engine, connection->peer, connection->rx, size, host_now_ms(h));
        std::memmove(
            connection->rx, connection->rx + size, connection->rx_used - static_cast<uint32_t>(size)
        );
        connection->rx_used -= static_cast<uint32_t>(size);
    }
}

/// Sends a carried connection's queued bytes through the route, or to its pair.
///
/// @param[in,out] h the host
/// @param[in,out] connection the carried connection
void flush_carried(Host* h, Connection* connection) {
    if (!connection->carried || connection->connecting || connection->failed ||
        connection->tx_used == 0)
        return;
    if (connection->paired) {
        if (connection->pair_slot >= max_connections) {
            connection->failed = true;
            return;
        }
        Connection& other = h->connections[connection->pair_slot];
        if (!other.carried || !other.paired ||
            connection_rx_bytes - other.rx_used < connection->tx_used) {
            connection->failed = true;
            return;
        }
        append_rx(&other, connection->tx, connection->tx_used);
        connection->tx_used = 0;
        h->last_stream_send_ms = host_now_ms(h);
        h->stream_sent = true;
        return;
    }
    const OverlayRoute* route = overlay_route();
    if (route == nullptr || route->send_stream == nullptr) {
        connection->failed = true;
        return;
    }
    if (!route->send_stream(
            route->context,
            connection->target.ip,
            connection->local_port,
            connection->target.port,
            connection->tx,
            connection->tx_used
        )) {
        connection->failed = true;
        return;
    }
    h->last_stream_send_ms = host_now_ms(h);
    h->stream_sent = true;
    connection->tx_used = 0;
}

/// Returns an ephemeral port this host is not already sending from.
///
/// @param h the host
/// @return the port, or 0 when none is free
uint16_t next_ephemeral(Host* h) {
    HostRouteState* state = route_state(h);
    if (state == nullptr)
        return 0;
    const uint16_t listen = h->engine.config.stream.port;
    for (int attempt = 0; attempt < 64; ++attempt) {
        const uint16_t candidate = state->next_ephemeral;
        state->next_ephemeral = state->next_ephemeral == 65535
                                    ? static_cast<uint16_t>(40000)
                                    : static_cast<uint16_t>(state->next_ephemeral + 1);
        if (candidate == 0 || candidate == listen)
            continue;
        bool used = false;
        for (const auto& existing : h->connections)
            if (existing.carried && existing.local_port == candidate)
                used = true;
        if (!used)
            return candidate;
    }
    return 0;
}

/// The index of a connection in its host's table.
///
/// @param h the host
/// @param connection a connection of that host
/// @return the index
uint8_t connection_slot(const Host* h, const Connection* connection) {
    return static_cast<uint8_t>(connection - h->connections);
}

/// Queues one datagram for an address the route claims.
///
/// A datagram for this client is kept and delivered on a later pump. Any
/// other claimed address is handed to the route. Neither calls the system.
///
/// @param[in,out] h the host
/// @param ip the destination address
/// @param port the destination port
/// @param bytes the datagram
/// @param size the datagram length
/// @return false when the datagram was not taken
bool carry_datagram(
    Host* h, const uint8_t ip[4], uint16_t port, const uint8_t* bytes, std::size_t size
) {
    if (size > dplay::max_datagram_bytes)
        return false;
    const uint16_t from_port = bound_port(h->datagram, h->engine.config.datagram.port);
    if (route_is_self(ip)) {
        HostRouteState* state = route_state(h);
        if (state == nullptr || state->looped_count == max_looped_datagrams)
            return false;
        const OverlayRoute* route = overlay_route();
        uint8_t self[4]{};
        if (route == nullptr || route->self_address == nullptr ||
            !route->self_address(route->context, self))
            return false;
        const std::size_t slot =
            (static_cast<std::size_t>(state->looped_head) + state->looped_count) %
            max_looped_datagrams;
        LoopedDatagram& item = state->looped[slot];
        std::memcpy(item.source, self, 4);
        item.source_port = from_port;
        if (size != 0 && bytes != nullptr)
            std::memcpy(item.bytes, bytes, size);
        item.size = size;
        ++state->looped_count;
        return true;
    }
    const OverlayRoute* route = overlay_route();
    if (route == nullptr || route->send_datagram == nullptr)
        return false;
    return route->send_datagram(route->context, ip, from_port, port, bytes, size);
}

/// Opens a carried stream to a claimed address and queues the bytes.
///
/// A dial of this client's own listen port pairs two local connections, so
/// the bytes come back without leaving the process. Any other claimed
/// address is sent by the route. connecting is clear at once: the route
/// keeps the bytes until its handshake has finished.
///
/// @param[in,out] h the host
/// @param to the endpoint dialled
/// @param bytes the bytes
/// @param size how many
/// @return false when the bytes were not taken
bool carry_stream(Host* h, const Address& to, const uint8_t* bytes, std::size_t size) {
    Connection* connection = nullptr;
    for (auto& candidate : h->connections)
        if (candidate.carried && candidate.outbound && !candidate.failed &&
            dplay::address_equal(candidate.target, to))
            connection = &candidate;
    if (connection == nullptr) {
        if (to.port == 0)
            return false;
        const uint16_t ephemeral = next_ephemeral(h);
        if (ephemeral == 0)
            return false;
        Connection* outbound = free_connection(h);
        if (outbound == nullptr)
            return false;
        if (route_is_self(to.ip)) {
            if (to.port != h->engine.config.stream.port) {
                return false;
            }
            init_connection(outbound, invalid_socket, to);
            outbound->carried = true;
            outbound->outbound = true;
            outbound->target = to;
            outbound->local_port = ephemeral;
            Connection* inbound = free_connection(h);
            if (inbound == nullptr) {
                init_connection(outbound, invalid_socket, Address{});
                return false;
            }
            Address from{};
            std::memcpy(from.ip, to.ip, 4);
            from.port = ephemeral;
            init_connection(inbound, invalid_socket, from);
            inbound->carried = true;
            inbound->local_port = to.port;
            outbound->paired = true;
            outbound->pair_slot = connection_slot(h, inbound);
            inbound->paired = true;
            inbound->pair_slot = connection_slot(h, outbound);
            connection = outbound;
        } else {
            init_connection(outbound, invalid_socket, to);
            outbound->carried = true;
            outbound->outbound = true;
            outbound->target = to;
            outbound->local_port = ephemeral;
            connection = outbound;
        }
    }
    if (connection_tx_bytes - connection->tx_used < size)
        return false;
    if (size != 0 && bytes != nullptr)
        std::memcpy(connection->tx + connection->tx_used, bytes, size);
    connection->tx_used += static_cast<uint32_t>(size);
    flush_carried(h, connection);
    return !connection->failed;
}

/// Finds the carried stream a packet continues.
///
/// @param h the host
/// @param packet the packet
/// @return the connection, or null when none matches
Connection* match_stream(Host* h, const OverlayPacket& packet) {
    for (auto& candidate : h->connections) {
        if (!candidate.carried || candidate.failed)
            continue;
        if (candidate.outbound) {
            if (std::memcmp(packet.source, candidate.target.ip, 4) == 0 &&
                packet.destination_port == candidate.local_port &&
                packet.source_port == candidate.target.port)
                return &candidate;
        } else if (
            std::memcmp(packet.source, candidate.peer.ip, 4) == 0 &&
            packet.source_port == candidate.peer.port &&
            packet.destination_port == candidate.local_port
        ) {
            return &candidate;
        }
    }
    return nullptr;
}

/// Appends one stream packet to the connection it belongs to.
///
/// A packet for the listen port with no connection opens an inbound one.
///
/// @param[in,out] h the host
/// @param packet the packet; its bytes are copied
void deliver_stream_packet(Host* h, const OverlayPacket& packet) {
    Connection* connection = match_stream(h, packet);
    if (connection == nullptr) {
        if (packet.destination_port != h->engine.config.stream.port || route_is_self(packet.source))
            return;
        connection = free_connection(h);
        if (connection == nullptr) {
            ++h->dropped_connections;
            return;
        }
        Address peer{};
        std::memcpy(peer.ip, packet.source, 4);
        peer.port = packet.source_port;
        init_connection(connection, invalid_socket, peer);
        connection->carried = true;
        connection->local_port = packet.destination_port;
    }
    append_rx(connection, packet.bytes, packet.size);
}

/// Delivers datagrams this host sent to itself.
///
/// Datagrams queued while one is delivered wait for the next round.
///
/// @param[in,out] h the host
void deliver_looped(Host* h) {
    HostRouteState* state = find_route_state(h);
    if (state == nullptr || state->looped_count == 0)
        return;
    const uint8_t queued = state->looped_count;
    for (uint8_t n = 0; n < queued; ++n) {
        if (state->looped_count == 0)
            break;
        LoopedDatagram& item = state->looped[state->looped_head];
        Address from{};
        std::memcpy(from.ip, item.source, 4);
        from.port = item.source_port;
        uint8_t copy[dplay::max_datagram_bytes];
        const std::size_t size = item.size > sizeof copy ? 0 : item.size;
        if (size != 0)
            std::memcpy(copy, item.bytes, size);
        state->looped_head = static_cast<uint8_t>(
            (static_cast<std::size_t>(state->looped_head) + 1) % max_looped_datagrams
        );
        --state->looped_count;
        if (item.size <= sizeof copy)
            dplay::engine_on_datagram(
                &h->engine, from, size == 0 ? nullptr : copy, size, host_now_ms(h)
            );
    }
}

/// True when a carried connection or a looped datagram is still waiting.
///
/// @param h the host
/// @return true when a pump should not wait
bool overlay_pending(const Host* h) {
    const HostRouteState* state = nullptr;
    for (const auto& slot : g_route_states)
        if (slot.host == h)
            state = &slot;
    if (state != nullptr && state->looped_count != 0)
        return true;
    for (const auto& candidate : h->connections)
        if (candidate.carried && !candidate.failed &&
            (candidate.rx_used != 0 || candidate.tx_used != 0))
            return true;
    return false;
}

/// Pumps the route and delivers what it and the loopback queue are holding.
///
/// @param[in,out] h the host
void service_overlay_once(Host* h) {
    const OverlayRoute* route = overlay_route();
    if (route == nullptr)
        return;
    if (route->pump != nullptr)
        route->pump(route->context);
    if (route->take != nullptr) {
        for (int taken = 0; taken < 64; ++taken) {
            OverlayPacket packet{};
            if (!route->take(route->context, &packet))
                break;
            if (packet.stream) {
                deliver_stream_packet(h, packet);
                continue;
            }
            if (packet.size > dplay::max_datagram_bytes ||
                (packet.bytes == nullptr && packet.size != 0))
                continue;
            uint8_t copy[dplay::max_datagram_bytes];
            if (packet.size != 0)
                std::memcpy(copy, packet.bytes, packet.size);
            Address from{};
            std::memcpy(from.ip, packet.source, 4);
            from.port = packet.source_port;
            dplay::engine_on_datagram(
                &h->engine, from, packet.size == 0 ? nullptr : copy, packet.size, host_now_ms(h)
            );
        }
    }
    deliver_looped(h);
    for (auto& candidate : h->connections)
        if (candidate.carried && !candidate.failed && candidate.tx_used != 0)
            flush_carried(h, &candidate);
    for (auto& candidate : h->connections)
        if (candidate.carried && !candidate.failed)
            consume_stream_messages(h, &candidate);
}

/// Services the route for a few rounds, so a reply queued by a delivery is delivered too.
///
/// @param[in,out] h the host
void service_overlay(Host* h) {
    if (overlay_route() == nullptr)
        return;
    for (int round = 0; round < overlay_rounds; ++round)
        service_overlay_once(h);
}

bool io_send_stream(void* context, const Address& to, const uint8_t* bytes, std::size_t size) {
    auto* h = static_cast<Host*>(context);
    if (route_claims(to.ip))
        return carry_stream(h, to, bytes, size);
    Connection* c = nullptr;
    for (auto& candidate : h->connections)
        if (socket_valid(candidate.fd) && candidate.outbound && !candidate.failed &&
            dplay::address_equal(candidate.target, to))
            c = &candidate;
    if (c == nullptr) {
        c = free_connection(h);
        if (c == nullptr || to.port == 0)
            return false;
        const intptr_t fd = open_socket(SOCK_STREAM);
        if (!socket_valid(fd))
            return false;
        set_option(fd, IPPROTO_TCP, TCP_NODELAY, 1);
        note_overlay_direct(to.ip);
        const sockaddr_in s = to_sockaddr(to);
        const int r = connect(native(fd), reinterpret_cast<const sockaddr*>(&s), sizeof s);
        if (r != 0 && !would_block()) {
            intptr_t tmp = fd;
            close_socket(&tmp);
            return false;
        }
        init_connection(c, fd, to);
        c->outbound = true;
        c->connecting = r != 0;
        c->target = to;
    }
    if (connection_tx_bytes - c->tx_used < size)
        return false;
    std::memcpy(c->tx + c->tx_used, bytes, size);
    c->tx_used += static_cast<uint32_t>(size);
    flush(h, c);
    return !c->failed;
}

#ifdef _WIN32
/// Lists the IPv4 addresses of this machine's network interfaces.
///
/// The broadcast address Winsock lists may be 255.255.255.255, so the
/// directed one is left for search_targets to work out from the netmask.
/// Winsock does not report the link state, so running follows up.
///
/// @param h open host; its datagram socket asks for the list
/// @param[out] out the interfaces
/// @param capacity entries available in out
/// @return the entries written; 0 when the system gives no list
std::size_t list_local_interfaces(Host* h, LocalInterface* out, std::size_t capacity) {
#if defined(OA_WINDOWS_95)
    // Winsock 1.1 has no call that lists the interfaces. A search that finds
    // none of them falls back to the broadcast and loopback addresses.
    (void)h;
    (void)out;
    (void)capacity;
    return 0;
#else
    INTERFACE_INFO listed[max_local_interfaces]{};
    DWORD listed_bytes = 0;
    if (WSAIoctl(
            native(h->datagram),
            SIO_GET_INTERFACE_LIST,
            nullptr,
            0,
            listed,
            sizeof listed,
            &listed_bytes,
            nullptr,
            nullptr
        ) != 0)
        return 0;
    const std::size_t listed_count =
        std::min<std::size_t>(listed_bytes / sizeof listed[0], max_local_interfaces);
    std::size_t count = 0;
    for (std::size_t i = 0; i < listed_count && count < capacity; ++i) {
        const INTERFACE_INFO& entry = listed[i];
        if (entry.iiAddress.Address.sa_family != AF_INET)
            continue;
        LocalInterface& local = out[count++];
        local = LocalInterface{};
        std::memcpy(local.address, &entry.iiAddress.AddressIn.sin_addr, 4);
        std::memcpy(local.netmask, &entry.iiNetmask.AddressIn.sin_addr, 4);
        local.up = (entry.iiFlags & IFF_UP) != 0;
        local.running = local.up;
        local.broadcast_capable = (entry.iiFlags & IFF_BROADCAST) != 0;
        local.loopback = (entry.iiFlags & IFF_LOOPBACK) != 0;
        local.point_to_point = (entry.iiFlags & IFF_POINTTOPOINT) != 0;
    }
    return count;
#endif
}
#else
/// Copies the IPv4 address out of an interface address.
///
/// BSD systems shorten a netmask to the bytes up to its last non-zero one,
/// so only the length the address states is read and the rest is zero.
///
/// @param address an AF_INET interface address, netmask or broadcast address
/// @param[out] ip the address
void copy_interface_ipv4(const sockaddr* address, uint8_t ip[4]) {
    sockaddr_in s{};
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    std::memcpy(&s, address, std::min<std::size_t>(address->sa_len, sizeof s));
#else
    std::memcpy(&s, address, sizeof s);
#endif
    std::memcpy(ip, &s.sin_addr, 4);
}

/// Lists the IPv4 addresses of this machine's network interfaces.
///
/// @param[out] out the interfaces
/// @param capacity entries available in out
/// @return the entries written; 0 when the system gives no list
std::size_t list_local_interfaces(Host*, LocalInterface* out, std::size_t capacity) {
    ifaddrs* listed = nullptr;
    if (getifaddrs(&listed) != 0)
        return 0;
    std::size_t count = 0;
    for (const ifaddrs* entry = listed; entry != nullptr && count < capacity;
         entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET ||
            entry->ifa_netmask == nullptr)
            continue;
        LocalInterface& local = out[count++];
        local = LocalInterface{};
        copy_interface_ipv4(entry->ifa_addr, local.address);
        copy_interface_ipv4(entry->ifa_netmask, local.netmask);
        local.up = (entry->ifa_flags & IFF_UP) != 0;
        local.running = (entry->ifa_flags & IFF_RUNNING) != 0;
        local.broadcast_capable = (entry->ifa_flags & IFF_BROADCAST) != 0;
        local.loopback = (entry->ifa_flags & IFF_LOOPBACK) != 0;
        local.point_to_point = (entry->ifa_flags & IFF_POINTOPOINT) != 0;
        // The broadcast address shares its slot with a point-to-point link's far end.
        if (local.broadcast_capable && entry->ifa_broadaddr != nullptr &&
            entry->ifa_broadaddr->sa_family == AF_INET)
            copy_interface_ipv4(entry->ifa_broadaddr, local.broadcast);
    }
    freeifaddrs(listed);
    return count;
}
#endif

/// Sends one datagram from the host's datagram socket.
///
/// @param h open host
/// @param ip destination address
/// @param port destination port
/// @param bytes the datagram
/// @param size datagram length in bytes
/// @return true when the whole datagram was handed to the system
bool send_datagram_to(
    Host* h, const uint8_t ip[4], uint16_t port, const uint8_t* bytes, std::size_t size
) {
    note_overlay_direct(ip);
    Address to{};
    std::memcpy(to.ip, ip, 4);
    to.port = port;
    const sockaddr_in s = to_sockaddr(to);
    const auto sent = sendto(
        native(h->datagram),
        reinterpret_cast<const char*>(bytes),
        static_cast<int>(size),
        0,
        reinterpret_cast<const sockaddr*>(&s),
        sizeof s
    );
    if (sent > 0)
        note_in_flight(h, sent);
    return sent >= 0 && static_cast<std::size_t>(sent) == size;
}

/// Asks the system which of this machine's addresses reaches a destination.
///
/// A datagram socket connected to the destination names it; nothing is sent.
///
/// @param to destination address
/// @param[out] ip the local address; unchanged on failure
/// @return false when the system names none
bool probe_local_address(const uint8_t to[4], uint8_t ip[4]) {
    if (!platform_startup())
        return false;
    intptr_t fd = open_socket(SOCK_DGRAM);
    if (!socket_valid(fd))
        return false;
    note_overlay_direct(to);
    Address target{};
    std::memcpy(target.ip, to, 4);
    target.port = dplay::enum_port;
    const sockaddr_in s = to_sockaddr(target);
    bool found = false;
    if (connect(native(fd), reinterpret_cast<const sockaddr*>(&s), sizeof s) == 0) {
        sockaddr_in local{};
        socket_len len = sizeof local;
        if (getsockname(native(fd), reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            const Address address = from_sockaddr(local);
            if (!dplay::address_ip_is_zero(address)) {
                std::memcpy(ip, address.ip, 4);
                found = true;
            }
        }
    }
    close_socket(&fd);
    return found;
}

bool io_local_address(void* context, const Address& to, uint8_t ip[4]) {
    return local_address_toward(static_cast<Host*>(context), to.ip, ip);
}

/// Opens the host's enumeration socket, where session enumeration requests arrive;
/// true once it listens.
bool io_listen_enumeration(void* context) {
    return host_listen_enumeration(static_cast<Host*>(context));
}

/// Sends one datagram carrying a message header, with the header's address set to a local address.
///
/// The header keeps what the engine wrote when an advertised address is
/// configured, the datagram carries no header, or it is too long to copy.
///
/// @param h open host
/// @param ip destination address
/// @param port destination port
/// @param bytes the datagram
/// @param size datagram length in bytes
/// @param source this machine's address on the destination's network
/// @return true when the whole datagram was handed to the system
bool send_stamped_datagram_to(
    Host* h,
    const uint8_t ip[4],
    uint16_t port,
    const uint8_t* bytes,
    std::size_t size,
    const uint8_t source[4]
) {
    const bool enveloped =
        size >= header_ip_offset + 4 && (load_u32(bytes) >> 20) == dplay_envelope_token;
    const uint8_t zero[4]{};
    if (!enveloped || size > max_search_bytes || std::memcmp(h->config.advertised_ip, zero, 4) != 0)
        return send_datagram_to(h, ip, port, bytes, size);
    uint8_t copy[max_search_bytes];
    std::memcpy(copy, bytes, size);
    std::memcpy(copy + header_ip_offset, source, 4);
    return send_datagram_to(h, ip, port, copy, size);
}

/// Sends a search: the datagram to every local IPv4 network, and through them to this machine.
///
/// Each network gets its directed broadcast. A game hosted on this machine
/// receives it too, from this machine's address on that network, so a player
/// who joins that game from here is known to the other players by an address
/// they can reach, never by 127.0.0.1. Only when no directed broadcast goes
/// out is the datagram sent to 255.255.255.255, which some systems refuse,
/// and as a copy to 127.0.0.1, so a game hosted here is still found without
/// a network.
///
/// @param h open host
/// @param port destination port
/// @param bytes the datagram
/// @param size datagram length in bytes
/// @return true when any copy was sent
bool send_search(Host* h, uint16_t port, const uint8_t* bytes, std::size_t size) {
    LocalInterface interfaces[max_local_interfaces]{};
    const std::size_t interface_count = list_local_interfaces(h, interfaces, max_local_interfaces);
    const SearchTargets targets = search_targets(interfaces, interface_count, h->config.bind_ip);
    bool broadcast_sent = false;
    for (uint32_t i = 0; i < targets.count; ++i)
        broadcast_sent =
            send_stamped_datagram_to(h, targets.ip[i], port, bytes, size, targets.source[i]) ||
            broadcast_sent;
    if (broadcast_sent)
        return true;
    const bool limited_sent = send_datagram_to(h, local_networks_ip, port, bytes, size);
    uint8_t source[4];
    std::memcpy(source, this_machine_ip, 4);
    (void)local_address_toward(h, this_machine_ip, source);
    const bool copy_sent = send_stamped_datagram_to(h, this_machine_ip, port, bytes, size, source);
    return limited_sent || copy_sent;
}

/// True when a datagram is a session search (EnumSessions).
///
/// @param bytes the datagram
/// @param size its length in bytes
/// @return false for anything else, including player data
bool is_session_search(const uint8_t* bytes, std::size_t size) {
    DplayEnvelope envelope{};
    return decode_dplay_envelope(bytes, size, &envelope, nullptr) == WireError::ok &&
           envelope.has_play_header &&
           envelope.command == static_cast<uint16_t>(DplayCommand::enum_sessions);
}

bool io_send_datagram(void* context, const Address& to, const uint8_t* bytes, std::size_t size) {
    auto* h = static_cast<Host*>(context);
    if (!socket_valid(h->datagram))
        return false;
    if (route_claims(to.ip)) {
        // Every machine on the route answers a session search at the
        // standard port, whatever port this machine looks for games at
        // (none, with a DirectPlay port of 0).
        const uint16_t port = is_session_search(bytes, size) ? dplay::enum_port : to.port;
        return port != 0 && carry_datagram(h, to.ip, port, bytes, size);
    }
    if (to.port == 0)
        return false;
    if (std::memcmp(to.ip, local_networks_ip, 4) == 0)
        return send_search(h, to.port, bytes, size);
    return send_datagram_to(h, to.ip, to.port, bytes, size);
}

void read_datagrams(Host* h, intptr_t fd) {
    uint8_t buffer[datagram_buffer_bytes];
    for (int i = 0; i < datagrams_per_pump; ++i) {
        sockaddr_in from{};
        socket_len len = sizeof from;
        const auto n = recvfrom(
            native(fd),
            reinterpret_cast<char*>(buffer),
            sizeof buffer,
            0,
            reinterpret_cast<sockaddr*>(&from),
            &len
        );
        if (n <= 0)
            return;
        const Address arrived = from_sockaddr(from);
        note_overlay_direct(arrived.ip);
        note_in_flight(h, -static_cast<int64_t>(n));
        dplay::engine_on_datagram(
            &h->engine, arrived, buffer, static_cast<std::size_t>(n), host_now_ms(h)
        );
    }
}

void accept_connections(Host* h) {
    for (;;) {
        sockaddr_in from{};
        socket_len len = sizeof from;
        const native_socket s =
            accept(native(h->listener), reinterpret_cast<sockaddr*>(&from), &len);
#ifdef _WIN32
        if (s == INVALID_SOCKET)
            return;
#else
        if (s < 0)
            return;
#endif
        intptr_t fd = static_cast<intptr_t>(s);
        const Address arrived = from_sockaddr(from);
        note_overlay_direct(arrived.ip);
        Connection* c = free_connection(h);
        if (c == nullptr || !set_nonblocking(fd)) {
            close_socket(&fd);
            ++h->dropped_connections;
            continue;
        }
#ifdef SO_NOSIGPIPE
        set_option(fd, SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
        set_option(fd, IPPROTO_TCP, TCP_NODELAY, 1);
        init_connection(c, fd, arrived);
    }
}

void read_connection(Host* h, Connection* c) {
    for (;;) {
        if (c->rx_used == connection_rx_bytes) {
            c->failed = true;
            return;
        }
        const auto n = recv(
            native(c->fd),
            reinterpret_cast<char*>(c->rx + c->rx_used),
            static_cast<int>(connection_rx_bytes - c->rx_used),
            0
        );
        if (n == 0 || (n < 0 && !would_block())) {
            c->failed = true;
            return;
        }
        if (n < 0)
            return;
        note_in_flight(h, -static_cast<int64_t>(n));
        c->rx_used += static_cast<uint32_t>(n);
        consume_stream_messages(h, c);
        if (c->failed)
            return;
    }
}

void finish_connect(Connection* c) {
    int error = 0;
    socket_len len = sizeof error;
    if (getsockopt(native(c->fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) !=
            0 ||
        error != 0) {
        c->failed = true;
        return;
    }
    c->connecting = false;
}

uint32_t transport_send(
    void* context, uint32_t from, uint32_t to, uint32_t flags, const uint8_t* data, uint32_t size
) {
    auto* h = static_cast<Host*>(context);
    return dplay::engine_send(&h->engine, from, to, flags, data, size);
}

uint32_t
transport_receive(void* context, uint32_t* from, uint32_t* to, uint8_t* buffer, uint32_t* size) {
    auto* h = static_cast<Host*>(context);
    host_pump(h, 0);
    return dplay::engine_receive(&h->engine, from, to, buffer, size);
}

int64_t clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

} // namespace

bool host_open(Host* h, const HostConfig& config) noexcept {
    drop_route_state(h);
    h->config = config;
    h->error[0] = '\0';
    h->clock_origin = clock_ms();
    if (!platform_startup()) {
        fail(h, "starting the socket subsystem");
        return false;
    }
    h->listener = open_socket(SOCK_STREAM);
    h->datagram = open_socket(SOCK_DGRAM);
    if (!socket_valid(h->listener) || !socket_valid(h->datagram)) {
        fail(h, "creating sockets");
        host_close(h);
        return false;
    }
    BindMiss stream_miss{};
    const uint16_t stream_port = bind_range(
        h->listener, config.bind_ip, config.stream_port_first, config.stream_port_last, &stream_miss
    );
    if (stream_port == 0) {
        report_refused_bind(
            h, "stream", config.stream_port_first, config.stream_port_last, stream_miss
        );
        host_close(h);
        return false;
    }
    if (listen(native(h->listener), accept_backlog) != 0) {
        char reason[128];
        system_socket_reason(last_socket_error(), reason, sizeof reason);
        std::snprintf(
            h->error,
            sizeof h->error,
            "stream port %u: %s",
            static_cast<unsigned>(stream_port),
            reason
        );
        std::fprintf(stderr, "multiplayer: hosting failed while listening on the %s\n", h->error);
        host_close(h);
        return false;
    }
    set_option(h->datagram, SOL_SOCKET, SO_BROADCAST, 1);
    BindMiss datagram_miss{};
    const uint16_t datagram_port = bind_range(
        h->datagram,
        config.bind_ip,
        config.datagram_port_first,
        config.datagram_port_last,
        &datagram_miss
    );
    if (datagram_port == 0) {
        report_refused_bind(
            h, "datagram", config.datagram_port_first, config.datagram_port_last, datagram_miss
        );
        host_close(h);
        return false;
    }
    dplay::EngineConfig engine{};
    std::memcpy(engine.stream.ip, config.advertised_ip, 4);
    engine.stream.port = stream_port;
    std::memcpy(engine.datagram.ip, config.advertised_ip, 4);
    engine.datagram.port = datagram_port;
    engine.enum_port = config.enum_port;
    engine.seed =
        config.seed != 0 ? config.seed : static_cast<uint32_t>(clock_ms() * 2654435761u) | 1u;
    h->route_count = 0;
    h->next_route = 0;
    dplay::engine_init(
        &h->engine,
        engine,
        dplay::EngineIo{
            h, io_send_stream, io_send_datagram, io_local_address, io_listen_enumeration
        }
    );
    return true;
}

bool host_listen_enumeration(Host* h) noexcept {
    if (socket_valid(h->enumeration))
        return true;
    h->enumeration = open_socket(SOCK_DGRAM);
    if (!socket_valid(h->enumeration)) {
        fail(h, "creating the enumeration socket");
        return false;
    }
    // One game holds the enumeration port. Another host on this machine is
    // refused, and the refusal names the port and the system's reason.
    BindMiss miss{};
    h->enum_port_bound = bind_range(
        h->enumeration, h->config.bind_ip, h->config.enum_port, h->config.enum_port, &miss
    );
    if (h->enum_port_bound == 0) {
        report_refused_bind(h, "enumeration", h->config.enum_port, h->config.enum_port, miss);
        close_socket(&h->enumeration);
        return false;
    }
    return true;
}

void host_pump(Host* h, uint32_t wait_ms) noexcept {
    if (overlay_route() != nullptr)
        service_overlay(h);
    SocketWait waits[max_wait_entries]{};
    Connection* owners[max_wait_entries]{};
    std::size_t count = 0;
    const auto add = [&](intptr_t fd, unsigned wanted, Connection* owner) {
        if (!socket_valid(fd))
            return;
        waits[count].fd = native(fd);
        waits[count].wanted = wanted;
        owners[count] = owner;
        ++count;
    };
    add(h->listener, socket_readable, nullptr);
    add(h->datagram, socket_readable, nullptr);
    add(h->enumeration, socket_readable, nullptr);
    for (auto& c : h->connections)
        if (socket_valid(c.fd) && !c.failed)
            add(c.fd,
                socket_readable | ((c.connecting || c.tx_used != 0) ? socket_writable : 0u),
                &c);
    const HostClock& clock = h->config.clock;
    const bool simulated = clock.advance != nullptr;
    // Under a simulated clock, only bytes still on their way between the
    // hosts sharing it are worth a real wait, and at most 1 ms of one.
    const bool in_flight = simulated && clock.in_flight != nullptr && *clock.in_flight > 0;
    const uint32_t real_wait = !simulated  ? wait_ms
                               : in_flight ? std::min<uint32_t>(wait_ms, 1)
                                           : 0;
    const uint32_t capped_wait = overlay_route() != nullptr && overlay_pending(h) ? 0 : real_wait;
    int ready = 0;
    if (count != 0)
        ready = wait_for_sockets(waits, count, capped_wait);
    if (simulated && ready <= 0) {
        // Bytes that a real wait did not bring were lost (a datagram to a
        // port no host listens on): they no longer hold the clock back.
        if (in_flight && real_wait != 0)
            *clock.in_flight = 0;
        if (wait_ms != 0)
            clock.advance(clock.context, wait_ms);
    }

    for (std::size_t i = 0; i < count; ++i) {
        const unsigned found = waits[i].found;
        if (found == 0)
            continue;
        Connection* c = owners[i];
        if (c == nullptr) {
            if (static_cast<intptr_t>(waits[i].fd) == h->listener)
                accept_connections(h);
            else
                read_datagrams(h, static_cast<intptr_t>(waits[i].fd));
            continue;
        }
        if (!socket_valid(c->fd) || c->failed)
            continue;
        if (c->connecting && (found & (socket_writable | socket_failed)) != 0)
            finish_connect(c);
        if (!c->connecting && (found & socket_writable) != 0)
            flush(h, c);
        if ((found & (socket_readable | socket_failed)) != 0 && !c->connecting)
            read_connection(h, c);
    }
    for (auto& c : h->connections) {
        if (c.carried) {
            if (!c.failed && !c.connecting)
                flush_carried(h, &c);
        } else if (socket_valid(c.fd) && !c.failed && !c.connecting)
            flush(h, &c);
        if (c.failed)
            reset_connection(h, &c);
    }
    if (overlay_route() != nullptr)
        service_overlay(h);
    dplay::engine_poll(&h->engine, host_now_ms(h));
}

uint32_t host_now_ms(const Host* h) noexcept {
    if (h->config.clock.now_ms != nullptr)
        return h->config.clock.now_ms(h->config.clock.context);
    return static_cast<uint32_t>(clock_ms() - h->clock_origin);
}

namespace {

/// Reads and drops what a closing host's connection has received.
///
/// @param[in,out] h closing host
/// @param[in,out] c open connection; marked failed once its peer has closed it
void discard_received(Host* h, Connection* c) {
    uint8_t scratch[0x1000];
    for (;;) {
        const auto n = recv(native(c->fd), reinterpret_cast<char*>(scratch), sizeof scratch, 0);
        if (n == 0 || (n < 0 && !would_block())) {
            c->failed = true;
            return;
        }
        if (n < 0)
            return;
        note_in_flight(h, -static_cast<int64_t>(n));
    }
}

/// Keeps a closing host's open stream connections until close_linger_ms
/// after the last bytes it sent, flushing what is queued and dropping what
/// arrives, or until every peer has closed its end.
///
/// @param[in,out] h closing host, its listener and datagram sockets closed
void linger_streams(Host* h) {
    for (;;) {
        SocketWait waits[max_connections]{};
        Connection* owners[max_connections]{};
        std::size_t count = 0;
        for (auto& c : h->connections) {
            if (!socket_valid(c.fd) || c.failed || c.connecting)
                continue;
            flush(h, &c);
            if (c.failed)
                continue;
            waits[count].fd = native(c.fd);
            waits[count].wanted = socket_readable | (c.tx_used != 0 ? socket_writable : 0u);
            owners[count] = &c;
            ++count;
        }
        const uint32_t elapsed = host_now_ms(h) - h->last_stream_send_ms;
        if (count == 0 || !h->stream_sent || elapsed >= close_linger_ms)
            return;
        const uint32_t remaining = close_linger_ms - elapsed;
        const HostClock& clock = h->config.clock;
        const bool simulated = clock.advance != nullptr;
        const int ready = wait_for_sockets(waits, count, simulated ? 0 : remaining);
        if (simulated && ready <= 0)
            clock.advance(clock.context, remaining);
        for (std::size_t i = 0; i < count; ++i)
            if ((waits[i].found & (socket_readable | socket_failed)) != 0)
                discard_received(h, owners[i]);
    }
}

} // namespace

void host_close(Host* h) noexcept {
    drop_route_state(h);
    dplay::engine_close(&h->engine);
    close_socket(&h->listener);
    close_socket(&h->datagram);
    close_socket(&h->enumeration);
    linger_streams(h);
    for (auto& c : h->connections) {
        close_socket(&c.fd);
        init_connection(&c, invalid_socket, Address{});
    }
    h->stream_sent = false;
}

NetTransport host_transport(Host* h) noexcept {
    return NetTransport{h, transport_send, transport_receive};
}

session::Backend host_session_backend(Host* h) noexcept {
    session::Backend b{};
    b.engine = &h->engine;
    b.context = h;
    b.pump = [](void* c, uint32_t wait_ms) { host_pump(static_cast<Host*>(c), wait_ms); };
    b.now_ms = [](void* c) { return host_now_ms(static_cast<Host*>(c)); };
    b.listen_enumeration = [](void* c) { return host_listen_enumeration(static_cast<Host*>(c)); };
    std::memcpy(b.enum_target, h->config.enum_target, 4);
    return b;
}

SearchTargets search_targets(
    const LocalInterface* interfaces, std::size_t count, const uint8_t bind_ip[4]
) noexcept {
    SearchTargets targets{};
    const uint32_t bound = ipv4_value(bind_ip);
    const uint32_t limited_broadcast = ipv4_value(local_networks_ip);
    for (std::size_t i = 0; i < count; ++i) {
        const LocalInterface& local = interfaces[i];
        if (!local.up || !local.running || !local.broadcast_capable || local.loopback ||
            local.point_to_point)
            continue;
        const uint32_t address = ipv4_value(local.address);
        if (bound != 0 && address != bound)
            continue;
        uint32_t broadcast = ipv4_value(local.broadcast);
        if (broadcast == 0)
            broadcast = address | ~ipv4_value(local.netmask);
        if (broadcast == address || broadcast == 0 || broadcast == limited_broadcast)
            continue;
        bool listed = false;
        for (uint32_t t = 0; t < targets.count && !listed; ++t)
            listed = ipv4_value(targets.ip[t]) == broadcast;
        if (listed || targets.count == max_search_targets)
            continue;
        store_ipv4(broadcast, targets.ip[targets.count]);
        std::memcpy(targets.source[targets.count], local.address, 4);
        ++targets.count;
    }
    return targets;
}

bool local_address_toward(Host* h, const uint8_t to[4], uint8_t ip[4]) noexcept {
    const uint32_t destination = ipv4_value(to);
    if (destination == 0 || destination == ipv4_value(local_networks_ip))
        return false;
    if (route_claims(to)) {
        const OverlayRoute* route = overlay_route();
        if (route == nullptr || route->self_address == nullptr)
            return false;
        return route->self_address(route->context, ip);
    }
    if (ipv4_value(h->config.bind_ip) != 0) {
        std::memcpy(ip, h->config.bind_ip, 4);
        return true;
    }
    for (uint32_t i = 0; i < h->route_count; ++i)
        if (ipv4_value(h->routes[i].destination) == destination) {
            std::memcpy(ip, h->routes[i].local, 4);
            return true;
        }
    uint8_t found[4]{};
    if (!probe_local_address(to, found))
        return false;
    uint32_t index = 0;
    if (h->route_count < max_route_entries) {
        index = h->route_count++;
    } else {
        index = h->next_route;
        h->next_route = (h->next_route + 1) % max_route_entries;
    }
    RouteEntry& entry = h->routes[index];
    std::memcpy(entry.destination, to, 4);
    std::memcpy(entry.local, found, 4);
    std::memcpy(ip, found, 4);
    return true;
}

bool resolve_ipv4(const char* name, uint8_t ip[4]) noexcept {
    if (name == nullptr || name[0] == '\0')
        return false;
    // Numeric forms are read here, the same way on every system and without
    // a name server; the system's resolver does not take them all.
    if (uint32_t value = 0; parse_numeric_ipv4(name, value)) {
        store_ipv4(value, ip);
        return true;
    }
    if (!platform_startup())
        return false;
#if defined(OA_WINDOWS_95)
    // Winsock 1.1 resolves a host name whole, with no service or address
    // family to ask for.
    const hostent* const found = gethostbyname(name);
    if (found == nullptr || found->h_addrtype != AF_INET || found->h_length != 4 ||
        found->h_addr_list == nullptr || found->h_addr_list[0] == nullptr)
        return false;
    std::memcpy(ip, found->h_addr_list[0], 4);
    return true;
#else
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &found) != 0)
        return false;
    bool resolved = false;
    for (const addrinfo* entry = found; entry != nullptr && !resolved; entry = entry->ai_next) {
        if (entry->ai_family != AF_INET || entry->ai_addr == nullptr ||
            entry->ai_addrlen < sizeof(sockaddr_in))
            continue;
        sockaddr_in s{};
        std::memcpy(&s, entry->ai_addr, sizeof s);
        std::memcpy(ip, &s.sin_addr, 4);
        resolved = true;
    }
    freeaddrinfo(found);
    return resolved;
#endif
}

// Streams (stream_socket.hpp) ---------------------------------------------------

namespace {

// Connections a loopback listener holds waiting to be accepted.
constexpr int stream_backlog = 4;
// The most bytes one read or write hands the system, which takes an int.
constexpr std::size_t max_stream_transfer = std::size_t{1} << 30;

/// Writes why a loopback stream failed.
///
/// @param[out] error receives the text and a zero byte
/// @param error_size the bytes `error` holds
/// @param what the text
void stream_error(char* error, std::size_t error_size, const char* what) {
    if (error != nullptr && error_size > 0)
        std::snprintf(error, error_size, "%s", what);
}

/// Prepares a loopback stream's socket: non-blocking, and never ending the
/// program when the other end has gone.
///
/// @param fd the socket
/// @return false when the system refused
bool prepare_stream(intptr_t fd) {
    if (!set_nonblocking(fd))
        return false;
#ifdef SO_NOSIGPIPE
    set_option(fd, SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
    return true;
}

/// Tells whether an address is refused before a connection is attempted.
///
/// 0.0.0.0 names no machine, 255.255.255.255 is not a host, and port 0 names
/// no server. A connection to 0.0.0.0 reaches this machine on some systems,
/// so it is refused here rather than opened.
///
/// @param to the address
/// @return true when stream_connect refuses it at once
bool address_is_unusable(const Address& to) {
    return to.port == 0 || dplay::address_ip_is_zero(to) ||
           std::memcmp(to.ip, local_networks_ip, 4) == 0;
}

/// Tells whether the system's error on a connection is zero, so it has opened.
///
/// This is the SO_ERROR read finish_connect uses once a connection is writable.
///
/// @param fd the connection
/// @return true when the connection opened
bool connection_is_open(intptr_t fd) {
    int error = 0;
    socket_len len = sizeof error;
    return getsockopt(native(fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) ==
               0 &&
           error == 0;
}

} // namespace

intptr_t stream_listen(
    Loopback loopback, uint16_t port, uint16_t& bound_port, char* error, std::size_t error_size
) noexcept {
    if (!platform_startup()) {
        stream_error(error, error_size, "the socket subsystem failed to start");
        return invalid_socket;
    }
#if defined(OA_WINDOWS_95)
    // Winsock 1.1 has no IPv6 at all: neither AF_INET6 nor sockaddr_in6 is
    // declared. Asking for it is refused rather than answered with the IPv4
    // address the caller did not ask for.
    if (loopback == Loopback::ipv6) {
        stream_error(error, error_size, "this Windows has no IPv6");
        return invalid_socket;
    }
#endif
#if defined(OA_WINDOWS_95)
    // Winsock 1.1 has no AF_INET6 to name, and the refusal above leaves only
    // the IPv4 address here.
    const int family = AF_INET;
#else
    const int family = loopback == Loopback::ipv6 ? AF_INET6 : AF_INET;
#endif
    const native_socket s = socket(family, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    intptr_t fd = s == INVALID_SOCKET ? invalid_socket : static_cast<intptr_t>(s);
#else
    intptr_t fd = s < 0 ? invalid_socket : static_cast<intptr_t>(s);
#endif
    if (!socket_valid(fd) || !prepare_stream(fd)) {
        close_socket(&fd);
        stream_error(error, error_size, "cannot create a socket");
        return invalid_socket;
    }
#ifndef _WIN32
    // A port given again soon after the last run binds while that run's
    // connections wait out their close.
    set_option(fd, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
#if defined(OA_WINDOWS_95)
    // Winsock 1.1 declares no sockaddr_storage, and every address here is an
    // IPv4 one: there is no IPv6 on that Windows to hold, as the refusal above
    // says.
    using address_buffer = sockaddr_in;
#else
    using address_buffer = sockaddr_storage;
#endif
    address_buffer address{};
    socket_len address_size = 0;
#if !defined(OA_WINDOWS_95)
    if (loopback == Loopback::ipv6) {
        sockaddr_in6 ipv6{};
        ipv6.sin6_family = AF_INET6;
        ipv6.sin6_port = htons(port);
        ipv6.sin6_addr.s6_addr[15] = 1; // ::1
        std::memcpy(&address, &ipv6, sizeof ipv6);
        address_size = sizeof ipv6;
    } else
#endif
    {
        sockaddr_in ipv4{};
        ipv4.sin_family = AF_INET;
        ipv4.sin_port = htons(port);
        std::memcpy(&ipv4.sin_addr, this_machine_ip, 4);
        std::memcpy(&address, &ipv4, sizeof ipv4);
        address_size = sizeof ipv4;
    }
    if (bind(native(fd), reinterpret_cast<const sockaddr*>(&address), address_size) != 0) {
        close_socket(&fd);
        stream_error(error, error_size, "the address is taken or cannot be bound");
        return invalid_socket;
    }
    if (listen(native(fd), stream_backlog) != 0) {
        close_socket(&fd);
        stream_error(error, error_size, "cannot listen on the address");
        return invalid_socket;
    }
    address_buffer bound{};
    socket_len bound_size = sizeof bound;
    if (getsockname(native(fd), reinterpret_cast<sockaddr*>(&bound), &bound_size) != 0) {
        close_socket(&fd);
        stream_error(error, error_size, "cannot read the port bound");
        return invalid_socket;
    }
#if defined(OA_WINDOWS_95)
    {
        sockaddr_in ipv4{};
        std::memcpy(&ipv4, &bound, sizeof ipv4);
        bound_port = ntohs(ipv4.sin_port);
    }
#else
    if (bound.ss_family == AF_INET6) {
        sockaddr_in6 ipv6{};
        std::memcpy(&ipv6, &bound, sizeof ipv6);
        bound_port = ntohs(ipv6.sin6_port);
    } else {
        sockaddr_in ipv4{};
        std::memcpy(&ipv4, &bound, sizeof ipv4);
        bound_port = ntohs(ipv4.sin_port);
    }
#endif
    return fd;
}

intptr_t stream_accept(intptr_t listener) noexcept {
    if (!socket_valid(listener))
        return invalid_socket;
    const native_socket s = accept(native(listener), nullptr, nullptr);
#ifdef _WIN32
    intptr_t fd = s == INVALID_SOCKET ? invalid_socket : static_cast<intptr_t>(s);
#else
    intptr_t fd = s < 0 ? invalid_socket : static_cast<intptr_t>(s);
#endif
    if (!socket_valid(fd))
        return invalid_socket;
    if (!prepare_stream(fd)) {
        close_socket(&fd);
        return invalid_socket;
    }
    set_option(fd, IPPROTO_TCP, TCP_NODELAY, 1);
    return fd;
}

std::ptrdiff_t stream_read(intptr_t stream, uint8_t* bytes, std::size_t size) noexcept {
    if (!socket_valid(stream))
        return stream_failed;
    // An empty read would look like the other end's end.
    if (size == 0)
        return 0;
    const auto wanted = static_cast<int>(std::min(size, max_stream_transfer));
    const auto got = recv(native(stream), reinterpret_cast<char*>(bytes), wanted, 0);
    if (got > 0)
        return static_cast<std::ptrdiff_t>(got);
    if (got == 0)
        return stream_ended;
    return would_block() ? 0 : stream_failed;
}

std::ptrdiff_t stream_write(intptr_t stream, const uint8_t* bytes, std::size_t size) noexcept {
    if (!socket_valid(stream))
        return stream_failed;
    const auto wanted = static_cast<int>(std::min(size, max_stream_transfer));
    const auto sent =
        send(native(stream), reinterpret_cast<const char*>(bytes), wanted, send_flags());
    if (sent >= 0)
        return static_cast<std::ptrdiff_t>(sent);
    return would_block() ? 0 : stream_failed;
}

void stream_finish(intptr_t stream) noexcept {
    if (!socket_valid(stream))
        return;
#ifdef _WIN32
#if defined(OA_WINDOWS_95)
    // Winsock 1.1's header names no how for shutdown, and the value is the 1
    // that Winsock 2 kept.
    constexpr int shut_write = 1;
#else
    constexpr int shut_write = SD_SEND;
#endif
    (void)shutdown(native(stream), shut_write);
#else
    (void)shutdown(native(stream), SHUT_WR);
#endif
}

void stream_close(intptr_t* stream) noexcept {
    close_socket(stream);
}

intptr_t stream_connect(const Address& to, char* error, std::size_t error_size) noexcept {
    if (!platform_startup()) {
        stream_error(error, error_size, "the socket subsystem failed to start");
        return invalid_socket;
    }
    if (address_is_unusable(to)) {
        stream_error(error, error_size, "the address cannot be connected to");
        return invalid_socket;
    }
    // connect, the wait and SO_ERROR are all in Winsock 1.1, so a Windows 95
    // build makes the same calls as later Windows. Nothing here needs a
    // separate path.
    const intptr_t fd = open_socket(SOCK_STREAM);
    if (!socket_valid(fd) || !prepare_stream(fd)) {
        intptr_t tmp = fd;
        close_socket(&tmp);
        stream_error(error, error_size, "cannot create a socket");
        return invalid_socket;
    }
    set_option(fd, IPPROTO_TCP, TCP_NODELAY, 1);
    const sockaddr_in peer = to_sockaddr(to);
    const int connected =
        connect(native(fd), reinterpret_cast<const sockaddr*>(&peer), sizeof peer);
    if (connected != 0 && !would_block()) {
        intptr_t tmp = fd;
        close_socket(&tmp);
        stream_error(error, error_size, "the connection was refused");
        return invalid_socket;
    }
    return fd;
}

StreamOpen stream_wait_open(intptr_t stream, uint32_t wait_ms) noexcept {
    if (!platform_startup() || !socket_valid(stream))
        return StreamOpen::failed;
    SocketWait wait{};
    wait.fd = native(stream);
    wait.wanted = socket_writable;
    const int ready = wait_for_sockets(&wait, 1, wait_ms);
    if (ready < 0)
        return StreamOpen::failed;
    if (ready == 0)
        return StreamOpen::opening;
    if ((wait.found & socket_invalid) != 0)
        return StreamOpen::failed;
    const bool writable = (wait.found & socket_writable) != 0;
    const bool broken = (wait.found & socket_failed) != 0;
    if (!writable && !broken)
        return StreamOpen::opening;
    // Writable with no system error is open. A refusal is an error here, the
    // same read finish_connect makes, including when the wait reported only
    // the failure.
    if (!connection_is_open(stream) || !writable)
        return StreamOpen::failed;
    return StreamOpen::open;
}

int stream_wait(StreamWaitEntry* entries, std::size_t count, uint32_t wait_ms) noexcept {
    // The public cap stays inside the wait the host already makes, and so
    // inside FD_SETSIZE.
    static_assert(stream_wait_most <= max_wait_entries);
    if (!platform_startup() || entries == nullptr || count == 0 || count > stream_wait_most)
        return -1;
    SocketWait waits[stream_wait_most]{};
    std::size_t index[stream_wait_most]{};
    std::size_t waiting = 0;
    int found_count = 0;
    for (std::size_t i = 0; i < count; ++i) {
        entries[i].readable = false;
        entries[i].writable = false;
        entries[i].failed = false;
        if (!socket_valid(entries[i].stream)) {
            entries[i].failed = true;
            ++found_count;
            continue;
        }
        index[waiting] = i;
        waits[waiting].fd = native(entries[i].stream);
        waits[waiting].wanted =
            (entries[i].read ? socket_readable : 0u) | (entries[i].write ? socket_writable : 0u);
        ++waiting;
    }
    if (waiting == 0)
        return found_count;
    const int ready = wait_for_sockets(waits, waiting, wait_ms);
    if (ready < 0)
        return -1;
    for (std::size_t n = 0; n < waiting; ++n) {
        StreamWaitEntry& entry = entries[index[n]];
        const unsigned found = waits[n].found;
        entry.readable = (found & socket_readable) != 0;
        entry.writable = (found & socket_writable) != 0;
        entry.failed = (found & (socket_failed | socket_invalid)) != 0;
        if (entry.readable || entry.writable || entry.failed)
            ++found_count;
    }
    return found_count;
}

} // namespace oa::netgame::sock
