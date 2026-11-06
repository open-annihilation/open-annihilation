// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rover platform services on POSIX: BSD sockets, pthreads and stdio.

#include "oa/rover/platform.h"
#include "oa/platform/files.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct RoverThread {
    pthread_t thread;
    RoverThreadBody body;
    void* context;
};

struct RoverEvent {
    pthread_mutex_t mutex;
    pthread_cond_t signalled;
    bool manual_reset;
    bool set;
};

// A recursive lock served in ticket order, so a thread releasing it cannot
// take it back ahead of a thread already waiting (see lock_create in
// platform.h). The select thread releases and retakes the socket-table lock
// in a tight loop while a socket stays ready; a pthread mutex, which lets the
// releaser win, kept the link thread out for as long as that lasted.
struct RoverLock {
    pthread_mutex_t mutex;
    pthread_cond_t turn;
    pthread_t owner;
    uint32_t depth; // 0 while free
    uint32_t next_ticket;
    uint32_t serving;
};

namespace {

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

// Winsock numbering of the socket errors the lobby can meet.
enum WinsockError : int32_t {
    kWsaInterrupted = 10004,
    kWsaAccess = 10013,
    kWsaFault = 10014,
    kWsaInvalid = 10022,
    kWsaTooManyOpen = 10024,
    kWsaWouldBlock = 10035,
    kWsaInProgress = 10036,
    kWsaAlready = 10037,
    kWsaNotSocket = 10038,
    kWsaMessageSize = 10040,
    kWsaAddressInUse = 10048,
    kWsaAddressUnavailable = 10049,
    kWsaNetworkDown = 10050,
    kWsaNetworkUnreachable = 10051,
    kWsaNetworkReset = 10052,
    kWsaConnectionAborted = 10053,
    kWsaConnectionReset = 10054,
    kWsaNoBuffers = 10055,
    kWsaConnected = 10056,
    kWsaNotConnected = 10057,
    kWsaShutdown = 10058,
    kWsaTimedOut = 10060,
    kWsaRefused = 10061,
    kWsaHostDown = 10064,
    kWsaHostUnreachable = 10065,
    kWsaHostNotFound = 11001,
};

thread_local int32_t t_last_error = 0;

int32_t winsock_error(int code) {
    switch (code) {
    case EINTR:
        return kWsaInterrupted;
    case EBADF:
        return kWsaNotSocket;
    case EACCES:
        return kWsaAccess;
    case EFAULT:
        return kWsaFault;
    case EMFILE:
        return kWsaTooManyOpen;
    case EAGAIN:
        return kWsaWouldBlock;
    case EINPROGRESS:
        return kWsaInProgress;
    case EALREADY:
        return kWsaAlready;
    case ENOTSOCK:
        return kWsaNotSocket;
    case EMSGSIZE:
        return kWsaMessageSize;
    case EADDRINUSE:
        return kWsaAddressInUse;
    case EADDRNOTAVAIL:
        return kWsaAddressUnavailable;
    case ENETDOWN:
        return kWsaNetworkDown;
    case ENETUNREACH:
        return kWsaNetworkUnreachable;
    case ENETRESET:
        return kWsaNetworkReset;
    case ECONNABORTED:
        return kWsaConnectionAborted;
    case ECONNRESET:
        return kWsaConnectionReset;
    case ENOBUFS:
        return kWsaNoBuffers;
    case EISCONN:
        return kWsaConnected;
    case ENOTCONN:
        return kWsaNotConnected;
    case EPIPE:
        return kWsaShutdown;
    case ETIMEDOUT:
        return kWsaTimedOut;
    case ECONNREFUSED:
        return kWsaRefused;
    case EHOSTDOWN:
        return kWsaHostDown;
    case EHOSTUNREACH:
        return kWsaHostUnreachable;
    default:
        return kWsaInvalid;
    }
}

int32_t fail_with_errno() {
    t_last_error = winsock_error(errno);
    return -1;
}

bool net_startup() {
    return true;
}

void net_cleanup() {
}

RoverSocket tcp_connect(const char* host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    char service[8];
    snprintf(service, sizeof service, "%u", static_cast<unsigned>(port));
    addrinfo* results = nullptr;
    if (getaddrinfo(host, service, &hints, &results) != 0)
        return OA_ROVER_SOCKET_INVALID;
    int connected = -1;
    for (addrinfo* candidate = results; candidate && connected < 0;
         candidate = candidate->ai_next) {
        int fd = socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd < 0)
            continue;
#if defined(SO_NOSIGPIPE)
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
        if (connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0)
            connected = fd;
        else
            close(fd);
    }
    freeaddrinfo(results);
    return connected < 0 ? OA_ROVER_SOCKET_INVALID : static_cast<RoverSocket>(connected);
}

// Whether tcp_select has seen each descriptor connected (see has_peer),
// indexed by descriptor and grown in steps as higher descriptors appear.
// Should the table fail to grow, that socket counts as connected only while
// it has a peer.
constexpr int kConnectedGrowth = 64;
pthread_mutex_t g_connected_mutex = PTHREAD_MUTEX_INITIALIZER;
uint8_t* g_connected{};
int g_connected_count{};

// fd's flag, growing the table to reach it when grow is set; null otherwise.
uint8_t* connected_flag(int fd, bool grow) {
    if (fd < 0)
        return nullptr;
    if (fd >= g_connected_count) {
        if (!grow)
            return nullptr;
        int count = (fd / kConnectedGrowth + 1) * kConnectedGrowth;
        uint8_t* grown = static_cast<uint8_t*>(realloc(g_connected, static_cast<size_t>(count)));
        if (!grown)
            return nullptr;
        memset(grown + g_connected_count, 0, static_cast<size_t>(count - g_connected_count));
        g_connected = grown;
        g_connected_count = count;
    }
    return &g_connected[fd];
}

void forget_connected(int fd) {
    pthread_mutex_lock(&g_connected_mutex);
    if (uint8_t* flag = connected_flag(fd, false))
        *flag = 0;
    pthread_mutex_unlock(&g_connected_mutex);
}

int32_t tcp_send(RoverSocket socket, const void* data, int32_t length) {
    for (;;) {
        ssize_t sent =
            send(static_cast<int>(socket), data, static_cast<size_t>(length), kSendFlags);
        if (sent >= 0)
            return static_cast<int32_t>(sent);
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return OA_ROVER_IO_WOULD_BLOCK;
        if (errno != EINTR)
            return fail_with_errno();
    }
}

int32_t tcp_recv(RoverSocket socket, void* buffer, int32_t capacity) {
    for (;;) {
        ssize_t received = recv(static_cast<int>(socket), buffer, static_cast<size_t>(capacity), 0);
        if (received >= 0)
            return static_cast<int32_t>(received);
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return OA_ROVER_IO_WOULD_BLOCK;
        if (errno != EINTR)
            return fail_with_errno();
    }
}

void tcp_close(RoverSocket socket) {
    forget_connected(static_cast<int>(socket));
    close(static_cast<int>(socket));
}

RoverSocket tcp_create() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fail_with_errno();
        return OA_ROVER_SOCKET_INVALID;
    }
    forget_connected(fd);
#if defined(SO_NOSIGPIPE)
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        fail_with_errno();
        close(fd);
        return OA_ROVER_SOCKET_INVALID;
    }
    return static_cast<RoverSocket>(fd);
}

bool tcp_connect_start(RoverSocket socket, uint32_t address, uint16_t port) {
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    target.sin_addr.s_addr = htonl(address);
    for (;;) {
        if (connect(
                static_cast<int>(socket), reinterpret_cast<sockaddr*>(&target), sizeof target
            ) == 0)
            return true;
        if (errno == EINPROGRESS)
            return true;
        if (errno != EINTR) {
            fail_with_errno();
            return false;
        }
    }
}

// A socket whose connect has concluded without a peer is Winsock's exception
// set; poll reports it as readable or writable instead. Winsock puts a
// connected socket in that set only for out-of-band data, which the lobby
// never sends, so poll watches for writability only until a peer exists:
// an idle connected socket is always writable and would end every wait.
// Once the connection ends the peer is gone again (after a half close the
// peer's close leaves the socket disconnected), but Winsock reports such a
// socket readable, recv returning 0 or the error, so a socket keeps counting
// as connected once it has had a peer.
bool has_peer(int fd) {
    sockaddr_in peer{};
    socklen_t length = sizeof peer;
    return getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &length) == 0;
}

bool is_connected(int fd) {
    bool peer = has_peer(fd);
    pthread_mutex_lock(&g_connected_mutex);
    uint8_t* flag = connected_flag(fd, peer);
    if (flag && peer)
        *flag = 1;
    bool connected = peer || (flag && *flag);
    pthread_mutex_unlock(&g_connected_mutex);
    return connected;
}

int32_t tcp_select(
    const RoverSocket* sockets,
    const uint8_t* interest,
    uint8_t* ready,
    int32_t count,
    uint32_t timeout_ms
) {
    constexpr int32_t kMaxSockets = 64;
    if (count < 0 || count > kMaxSockets) {
        t_last_error = kWsaInvalid;
        return -1;
    }
    pollfd polls[kMaxSockets]{};
    for (int32_t i = 0; i < count; ++i) {
        polls[i].fd = static_cast<int>(sockets[i]);
        if (interest[i] & OA_ROVER_SOCKET_READABLE)
            polls[i].events |= POLLIN;
        if (interest[i] & OA_ROVER_SOCKET_WRITABLE)
            polls[i].events |= POLLOUT;
        else if ((interest[i] & OA_ROVER_SOCKET_FAILED) && !is_connected(polls[i].fd))
            polls[i].events |= POLLOUT;
        ready[i] = 0;
    }
    int timeout = timeout_ms == OA_ROVER_WAIT_FOREVER ? -1 : static_cast<int>(timeout_ms);
    int polled = poll(polls, static_cast<nfds_t>(count), timeout);
    if (polled < 0)
        return errno == EINTR ? 0 : fail_with_errno();
    int32_t found = 0;
    for (int32_t i = 0; i < count; ++i) {
        short events = polls[i].revents;
        if (!events)
            continue;
        if (events & POLLNVAL) {
            t_last_error = kWsaNotSocket;
            return -1;
        }
        if (!is_connected(polls[i].fd)) {
            if ((interest[i] & OA_ROVER_SOCKET_FAILED) && (events & (POLLOUT | POLLERR | POLLHUP)))
                ready[i] |= OA_ROVER_SOCKET_FAILED;
            else if ((interest[i] & OA_ROVER_SOCKET_READABLE) && (events & POLLIN))
                ready[i] |= OA_ROVER_SOCKET_READABLE;
        } else {
            if ((interest[i] & OA_ROVER_SOCKET_READABLE) && (events & (POLLIN | POLLHUP | POLLERR)))
                ready[i] |= OA_ROVER_SOCKET_READABLE;
            if ((interest[i] & OA_ROVER_SOCKET_WRITABLE) && (events & POLLOUT))
                ready[i] |= OA_ROVER_SOCKET_WRITABLE;
        }
        if (ready[i])
            ++found;
    }
    return found;
}

int32_t tcp_pending_error(RoverSocket socket) {
    int error = 0;
    socklen_t length = sizeof error;
    if (getsockopt(static_cast<int>(socket), SOL_SOCKET, SO_ERROR, &error, &length) != 0)
        return fail_with_errno();
    return error ? winsock_error(error) : 0;
}

void tcp_shutdown_send(RoverSocket socket) {
    shutdown(static_cast<int>(socket), SHUT_WR);
}

int32_t net_last_error() {
    return t_last_error;
}

bool ipv4_parse(const char* text, uint32_t* out_address) {
    in_addr_t address = inet_addr(text);
    if (address == INADDR_NONE)
        return false;
    *out_address = ntohl(address);
    return true;
}

bool ipv4_lookup(const char* host, uint32_t* out_address) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &results) != 0 || !results) {
        t_last_error = kWsaHostNotFound;
        return false;
    }
    const sockaddr_in* first = reinterpret_cast<const sockaddr_in*>(results->ai_addr);
    *out_address = ntohl(first->sin_addr.s_addr);
    freeaddrinfo(results);
    return true;
}

void* run_thread(void* argument) {
    RoverThread* thread = static_cast<RoverThread*>(argument);
    thread->body(thread->context);
    return nullptr;
}

RoverThread* thread_start(RoverThreadBody body, void* context) {
    RoverThread* thread = static_cast<RoverThread*>(calloc(1, sizeof(RoverThread)));
    if (!thread)
        return nullptr;
    thread->body = body;
    thread->context = context;
    if (pthread_create(&thread->thread, nullptr, run_thread, thread) != 0) {
        free(thread);
        return nullptr;
    }
    return thread;
}

void thread_join(RoverThread* thread) {
    pthread_join(thread->thread, nullptr);
    free(thread);
}

RoverEvent* event_create(bool manual_reset, bool initially_set) {
    RoverEvent* event = static_cast<RoverEvent*>(calloc(1, sizeof(RoverEvent)));
    if (!event)
        return nullptr;
    pthread_mutex_init(&event->mutex, nullptr);
    pthread_cond_init(&event->signalled, nullptr);
    event->manual_reset = manual_reset;
    event->set = initially_set;
    return event;
}

// Every multi-event waiter sleeps on this one condition, which each event_set
// broadcasts after raising its event.
pthread_mutex_t g_any_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_any_signalled = PTHREAD_COND_INITIALIZER;

void event_set(RoverEvent* event) {
    pthread_mutex_lock(&event->mutex);
    event->set = true;
    if (event->manual_reset)
        pthread_cond_broadcast(&event->signalled);
    else
        pthread_cond_signal(&event->signalled);
    pthread_mutex_unlock(&event->mutex);
    pthread_mutex_lock(&g_any_mutex);
    pthread_cond_broadcast(&g_any_signalled);
    pthread_mutex_unlock(&g_any_mutex);
}

void event_reset(RoverEvent* event) {
    pthread_mutex_lock(&event->mutex);
    event->set = false;
    pthread_mutex_unlock(&event->mutex);
}

timespec deadline_after(uint32_t timeout_ms) {
    timespec deadline{};
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += static_cast<time_t>(timeout_ms / 1000u);
    deadline.tv_nsec += static_cast<long>(timeout_ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    return deadline;
}

bool event_wait(RoverEvent* event, uint32_t timeout_ms) {
    timespec deadline = deadline_after(timeout_ms);
    pthread_mutex_lock(&event->mutex);
    while (!event->set) {
        int waited = timeout_ms == OA_ROVER_WAIT_FOREVER
                         ? pthread_cond_wait(&event->signalled, &event->mutex)
                         : pthread_cond_timedwait(&event->signalled, &event->mutex, &deadline);
        if (waited == ETIMEDOUT)
            break;
    }
    bool signalled = event->set;
    if (signalled && !event->manual_reset)
        event->set = false;
    pthread_mutex_unlock(&event->mutex);
    return signalled;
}

// Takes the event if it is set; an auto-reset event is consumed.
bool event_take(RoverEvent* event) {
    pthread_mutex_lock(&event->mutex);
    bool signalled = event->set;
    if (signalled && !event->manual_reset)
        event->set = false;
    pthread_mutex_unlock(&event->mutex);
    return signalled;
}

int32_t event_wait_any(RoverEvent* const* events, int32_t count, uint32_t timeout_ms) {
    timespec deadline = deadline_after(timeout_ms);
    pthread_mutex_lock(&g_any_mutex);
    for (;;) {
        for (int32_t i = 0; i < count; ++i) {
            if (event_take(events[i])) {
                pthread_mutex_unlock(&g_any_mutex);
                return i;
            }
        }
        int waited = timeout_ms == OA_ROVER_WAIT_FOREVER
                         ? pthread_cond_wait(&g_any_signalled, &g_any_mutex)
                         : pthread_cond_timedwait(&g_any_signalled, &g_any_mutex, &deadline);
        if (waited == ETIMEDOUT) {
            for (int32_t i = 0; i < count; ++i) {
                if (event_take(events[i])) {
                    pthread_mutex_unlock(&g_any_mutex);
                    return i;
                }
            }
            pthread_mutex_unlock(&g_any_mutex);
            return OA_ROVER_WAIT_TIMEOUT;
        }
    }
}

void event_destroy(RoverEvent* event) {
    pthread_cond_destroy(&event->signalled);
    pthread_mutex_destroy(&event->mutex);
    free(event);
}

RoverLock* lock_create() {
    RoverLock* lock = static_cast<RoverLock*>(calloc(1, sizeof(RoverLock)));
    if (!lock)
        return nullptr;
    pthread_mutex_init(&lock->mutex, nullptr);
    pthread_cond_init(&lock->turn, nullptr);
    return lock;
}

// Takes a ticket and waits for it to be served; the owner only counts depth.
void lock_enter(RoverLock* lock) {
    pthread_t self = pthread_self();
    pthread_mutex_lock(&lock->mutex);
    if (lock->depth != 0 && pthread_equal(lock->owner, self)) {
        ++lock->depth;
        pthread_mutex_unlock(&lock->mutex);
        return;
    }
    uint32_t ticket = lock->next_ticket++;
    while (lock->serving != ticket)
        pthread_cond_wait(&lock->turn, &lock->mutex);
    lock->owner = self;
    lock->depth = 1;
    pthread_mutex_unlock(&lock->mutex);
}

void lock_leave(RoverLock* lock) {
    pthread_mutex_lock(&lock->mutex);
    if (--lock->depth == 0) {
        ++lock->serving;
        pthread_cond_broadcast(&lock->turn);
    }
    pthread_mutex_unlock(&lock->mutex);
}

void lock_destroy(RoverLock* lock) {
    pthread_cond_destroy(&lock->turn);
    pthread_mutex_destroy(&lock->mutex);
    free(lock);
}

RoverFile* file_open(const char* path) {
    return reinterpret_cast<RoverFile*>(oa::platform::open_file(path, "rb"));
}

RoverFile* file_create(const char* path) {
    return reinterpret_cast<RoverFile*>(oa::platform::open_file(path, "wb"));
}

int32_t file_read(RoverFile* file, void* buffer, int32_t capacity) {
    FILE* stream = reinterpret_cast<FILE*>(file);
    size_t read = fread(buffer, 1, static_cast<size_t>(capacity), stream);
    if (read == 0 && ferror(stream))
        return -1;
    return static_cast<int32_t>(read);
}

int32_t file_write(RoverFile* file, const void* data, int32_t length) {
    FILE* stream = reinterpret_cast<FILE*>(file);
    size_t written = fwrite(data, 1, static_cast<size_t>(length), stream);
    if (written != static_cast<size_t>(length) && ferror(stream))
        return -1;
    return static_cast<int32_t>(written);
}

void file_close(RoverFile* file) {
    fclose(reinterpret_cast<FILE*>(file));
}

uint32_t clock_ms() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint32_t>(
        static_cast<uint64_t>(now.tv_sec) * 1000u + static_cast<uint64_t>(now.tv_nsec) / 1000000u
    );
}

void sleep_ms(uint32_t milliseconds) {
    if (milliseconds == 0) {
        sched_yield();
        return;
    }
    timespec delay{};
    delay.tv_sec = static_cast<time_t>(milliseconds / 1000u);
    delay.tv_nsec = static_cast<long>(milliseconds % 1000u) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

const RoverPlatform kPosixPlatform = {
    .net_startup = net_startup,
    .net_cleanup = net_cleanup,
    .tcp_connect = tcp_connect,
    .tcp_send = tcp_send,
    .tcp_recv = tcp_recv,
    .tcp_close = tcp_close,
    .tcp_create = tcp_create,
    .tcp_connect_start = tcp_connect_start,
    .tcp_select = tcp_select,
    .tcp_pending_error = tcp_pending_error,
    .tcp_shutdown_send = tcp_shutdown_send,
    .net_last_error = net_last_error,
    .ipv4_parse = ipv4_parse,
    .ipv4_lookup = ipv4_lookup,
    .thread_start = thread_start,
    .thread_join = thread_join,
    .event_create = event_create,
    .event_set = event_set,
    .event_reset = event_reset,
    .event_wait = event_wait,
    .event_wait_any = event_wait_any,
    .event_destroy = event_destroy,
    .lock_create = lock_create,
    .lock_enter = lock_enter,
    .lock_leave = lock_leave,
    .lock_destroy = lock_destroy,
    .file_open = file_open,
    .file_create = file_create,
    .file_read = file_read,
    .file_write = file_write,
    .file_close = file_close,
    .clock_ms = clock_ms,
    .sleep_ms = sleep_ms,
};

} // namespace

const RoverPlatform* rover_native_platform(void) {
    return &kPosixPlatform;
}
