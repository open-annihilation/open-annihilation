// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The native platform table: threads, events, locks, files, the clock, a
// loopback TCP round trip and the non-blocking socket calls the lobby link
// uses; and the table a host selects in its place.

#include "../src/platform_select.h"
#include "oa/rover/platform.h"

#include <atomic>
#include <stdio.h>
#include <string.h>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct Worker {
    const RoverPlatform* platform;
    RoverEvent* go;
    RoverEvent* done;
    RoverLock* lock;
    int counter;
};

void worker_body(void* context) {
    Worker* worker = static_cast<Worker*>(context);
    worker->platform->event_wait(worker->go, OA_ROVER_WAIT_FOREVER);
    for (int i = 0; i < 1000; ++i) {
        worker->platform->lock_enter(worker->lock);
        worker->platform->lock_enter(worker->lock);
        ++worker->counter;
        worker->platform->lock_leave(worker->lock);
        worker->platform->lock_leave(worker->lock);
    }
    worker->platform->event_set(worker->done);
}

void check_threads_events_locks(const RoverPlatform* platform) {
    Worker worker{
        platform,
        platform->event_create(false, false),
        platform->event_create(true, false),
        platform->lock_create(),
        0
    };
    check(!platform->event_wait(worker.go, 0), "auto-reset event starts clear");
    RoverThread* thread = platform->thread_start(worker_body, &worker);
    check(thread != nullptr, "thread_start");
    platform->event_set(worker.go);
    check(platform->event_wait(worker.done, 5000), "worker signals completion");
    check(platform->event_wait(worker.done, 0), "manual-reset event stays set");
    platform->event_reset(worker.done);
    check(!platform->event_wait(worker.done, 0), "event_reset clears");
    platform->thread_join(thread);
    check(worker.counter == 1000, "recursive lock serialises the worker");
    check(!platform->event_wait(worker.go, 0), "auto-reset event consumed by its waiter");
    platform->lock_destroy(worker.lock);
    platform->event_destroy(worker.done);
    platform->event_destroy(worker.go);
}

struct LockWaiter {
    const RoverPlatform* platform;
    RoverLock* lock;
    std::atomic<bool> waiting;
    std::atomic<bool> acquired;
};

void take_lock(void* context) {
    LockWaiter* waiter = static_cast<LockWaiter*>(context);
    waiter->waiting = true;
    waiter->platform->lock_enter(waiter->lock);
    waiter->acquired = true;
    waiter->platform->lock_leave(waiter->lock);
}

/// Checks that a lock goes to the thread already waiting for it, not back to the thread that
/// released it, as with the critical sections of the Windows versions 3.1c shipped for: the
/// select thread releases and retakes the socket-table lock in a tight loop.
///
/// @param platform the platform whose locks and threads are checked
void check_lock_handoff(const RoverPlatform* platform) {
    LockWaiter waiter{platform, platform->lock_create(), false, false};
    platform->lock_enter(waiter.lock);
    RoverThread* thread = platform->thread_start(take_lock, &waiter);
    while (!waiter.waiting)
        platform->sleep_ms(1);
    platform->sleep_ms(50);
    platform->lock_leave(waiter.lock);
    platform->lock_enter(waiter.lock);
    check(waiter.acquired, "a waiting thread takes the lock before its releaser retakes it");
    platform->lock_leave(waiter.lock);
    platform->thread_join(thread);
    platform->lock_destroy(waiter.lock);
}

struct Signaller {
    const RoverPlatform* platform;
    RoverEvent* event;
};

void signal_later(void* context) {
    Signaller* signaller = static_cast<Signaller*>(context);
    signaller->platform->sleep_ms(20);
    signaller->platform->event_set(signaller->event);
}

void check_wait_any(const RoverPlatform* platform) {
    RoverEvent* events[3] = {
        platform->event_create(true, false),
        platform->event_create(false, false),
        platform->event_create(true, false),
    };
    check(platform->event_wait_any(events, 3, 0) == OA_ROVER_WAIT_TIMEOUT, "wait_any times out");
    platform->event_set(events[2]);
    platform->event_set(events[1]);
    check(platform->event_wait_any(events, 3, 0) == 1, "wait_any returns the lowest index");
    check(platform->event_wait_any(events, 3, 0) == 2, "auto-reset event consumed by wait_any");
    check(platform->event_wait_any(events, 3, 0) == 2, "manual-reset event stays set");
    platform->event_reset(events[2]);
    Signaller signaller{platform, events[0]};
    RoverThread* thread = platform->thread_start(signal_later, &signaller);
    check(platform->event_wait_any(events, 3, 5000) == 0, "wait_any wakes on a later set");
    platform->thread_join(thread);
    for (RoverEvent* event : events)
        platform->event_destroy(event);
}

void check_clock(const RoverPlatform* platform) {
    uint32_t before = platform->clock_ms();
    platform->sleep_ms(20);
    uint32_t elapsed = platform->clock_ms() - before;
    check(elapsed >= 15 && elapsed < 5000, "clock advances across sleep_ms");
    platform->sleep_ms(0);
}

// `source` is this test's own source file, whose first bytes are "//"; CTest
// passes its path, since __FILE__ may be relative to the directory it was
// compiled in.
void check_file(const RoverPlatform* platform, const char* source) {
    check(platform->file_open("/nonexistent/rover/file") == nullptr, "missing file");
    RoverFile* file = platform->file_open(source);
    check(file != nullptr, "file_open on this source");
    if (!file)
        return;
    char head[2] = {};
    check(
        platform->file_read(file, head, 2) == 2 && head[0] == '/' && head[1] == '/',
        "file_read returns the first bytes"
    );
    char rest[4096];
    while (platform->file_read(file, rest, sizeof rest) > 0) {
    }
    check(platform->file_read(file, rest, sizeof rest) == 0, "file_read reports end of file");
    platform->file_close(file);
}

void check_file_write(const RoverPlatform* platform) {
    const char* path = "rover_platform_test.tmp";
    RoverFile* file = platform->file_create(path);
    check(file != nullptr, "file_create");
    if (!file)
        return;
    check(platform->file_write(file, "a\r\nb", 4) == 4, "file_write");
    platform->file_close(file);
    file = platform->file_open(path);
    char back[8] = {};
    check(file && platform->file_read(file, back, sizeof back) == 4, "written bytes read back");
    check(memcmp(back, "a\r\nb", 4) == 0, "file_write keeps bytes as given");
    if (file)
        platform->file_close(file);
    remove(path);
}

void check_ipv4(const RoverPlatform* platform) {
    uint32_t address = 0;
    check(platform->ipv4_parse("127.0.0.1", &address) && address == 0x7f000001u, "ipv4_parse");
    check(!platform->ipv4_parse("localhost", &address), "ipv4_parse rejects a name");
    check(platform->ipv4_lookup("localhost", &address) && address == 0x7f000001u, "ipv4_lookup");
}

#if !defined(_WIN32)
// Selects one socket until any of the wanted sets reports it.
uint8_t select_one(const RoverPlatform* platform, RoverSocket socket, uint8_t interest) {
    uint8_t ready = 0;
    for (int attempt = 0; attempt < 50 && !ready; ++attempt)
        platform->tcp_select(&socket, &interest, &ready, 1, 100);
    return ready;
}

void check_async_tcp(const RoverPlatform* platform) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof address;
    bool listening = listener >= 0 &&
                     bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 &&
                     listen(listener, 1) == 0 &&
                     getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    check(listening, "async loopback listener");
    if (!listening)
        return;
    uint16_t port = ntohs(address.sin_port);
    constexpr uint8_t kAll =
        OA_ROVER_SOCKET_READABLE | OA_ROVER_SOCKET_WRITABLE | OA_ROVER_SOCKET_FAILED;

    RoverSocket client = platform->tcp_create();
    check(client != OA_ROVER_SOCKET_INVALID, "tcp_create");
    check(platform->tcp_connect_start(client, 0x7f000001u, port), "tcp_connect_start");
    check(
        select_one(platform, client, kAll) == OA_ROVER_SOCKET_WRITABLE,
        "connect completes as writable"
    );
    check(platform->tcp_pending_error(client) == 0, "no pending error once connected");
    int server = accept(listener, nullptr, nullptr);
    // The select thread's interest in every socket: an idle connected one is
    // in neither set, so the wait runs to its timeout.
    uint8_t idle_interest = OA_ROVER_SOCKET_READABLE | OA_ROVER_SOCKET_FAILED;
    uint8_t idle_ready = 0xff;
    uint32_t idle_start = platform->clock_ms();
    check(
        platform->tcp_select(&client, &idle_interest, &idle_ready, 1, 200) == 0 && idle_ready == 0,
        "idle connected socket is not reported"
    );
    check(platform->clock_ms() - idle_start >= 150, "select on an idle socket waits its timeout");
    char buffer[16] = {};
    check(
        platform->tcp_recv(client, buffer, sizeof buffer) == OA_ROVER_IO_WOULD_BLOCK,
        "recv would block"
    );
    check(send(server, "{x}", 3, 0) == 3, "server sends");
    check(
        select_one(platform, client, OA_ROVER_SOCKET_READABLE) == OA_ROVER_SOCKET_READABLE,
        "readable"
    );
    check(platform->tcp_recv(client, buffer, sizeof buffer) == 3, "non-blocking recv");
    platform->tcp_shutdown_send(client);
    check(recv(server, buffer, sizeof buffer, 0) == 0, "shutdown_send ends the peer's stream");
    close(server);
    // How the yard master link ends: the peer answers the half close by
    // closing. No peer is left, yet Winsock reports the socket readable, not
    // as a failed connect, and the receive sees the end of the stream.
    check(
        select_one(platform, client, idle_interest) == OA_ROVER_SOCKET_READABLE,
        "a connection closed after a half close is readable"
    );
    check(platform->tcp_recv(client, buffer, sizeof buffer) == 0, "its receive sees the end");
    platform->tcp_close(client);

    // Nothing listens on the port once the listener is gone: the connect fails.
    close(listener);
    RoverSocket refused = platform->tcp_create();
    if (platform->tcp_connect_start(refused, 0x7f000001u, port)) {
        check(
            select_one(platform, refused, kAll) == OA_ROVER_SOCKET_FAILED, "refused connect fails"
        );
        check(platform->tcp_pending_error(refused) == 10061, "refusal is WSAECONNREFUSED");
    } else {
        check(platform->net_last_error() == 10061, "immediate refusal is WSAECONNREFUSED");
    }
    platform->tcp_close(refused);

    int pipe_ends[2] = {-1, -1};
    check(pipe(pipe_ends) == 0, "pipe");
    check(
        platform->tcp_pending_error(static_cast<RoverSocket>(pipe_ends[0])) == -1 &&
            platform->net_last_error() == 10038,
        "pending error of a non-socket fails with WSAENOTSOCK"
    );
    close(pipe_ends[0]);
    close(pipe_ends[1]);
}

// With many connected sockets open, and the last on a high descriptor, the
// last one still reads as readable once its peer closes after a half close.
void check_half_close_among_many(const RoverPlatform* platform) {
    constexpr int kOpen = 70;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof address;
    bool listening = listener >= 0 &&
                     bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 &&
                     listen(listener, kOpen) == 0 &&
                     getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    check(listening, "loopback listener for many sockets");
    if (!listening)
        return;
    uint16_t port = ntohs(address.sin_port);
    constexpr uint8_t kAll =
        OA_ROVER_SOCKET_READABLE | OA_ROVER_SOCKET_WRITABLE | OA_ROVER_SOCKET_FAILED;
    RoverSocket clients[kOpen];
    int servers[kOpen];
    int opened = 0;
    for (; opened < kOpen; ++opened) {
        clients[opened] = platform->tcp_create();
        if (clients[opened] == OA_ROVER_SOCKET_INVALID ||
            !platform->tcp_connect_start(clients[opened], 0x7f000001u, port) ||
            select_one(platform, clients[opened], kAll) != OA_ROVER_SOCKET_WRITABLE) {
            platform->tcp_close(clients[opened]);
            break;
        }
        servers[opened] = accept(listener, nullptr, nullptr);
    }
    check(opened == kOpen, "many sockets connect");
    if (opened == kOpen) {
        RoverSocket last = clients[kOpen - 1];
        int last_server = servers[kOpen - 1];
        check(static_cast<int>(last) >= 64, "the last socket has a high descriptor");
        platform->tcp_shutdown_send(last);
        char buffer[4];
        check(recv(last_server, buffer, sizeof buffer, 0) == 0, "its peer sees the half close");
        close(last_server);
        servers[kOpen - 1] = -1;
        uint8_t idle_interest = OA_ROVER_SOCKET_READABLE | OA_ROVER_SOCKET_FAILED;
        check(
            select_one(platform, last, idle_interest) == OA_ROVER_SOCKET_READABLE,
            "the last socket closed after its half close is readable"
        );
    }
    for (int i = 0; i < opened; ++i) {
        platform->tcp_close(clients[i]);
        if (servers[i] >= 0)
            close(servers[i]);
    }
    close(listener);
}

void check_tcp(const RoverPlatform* platform) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof address;
    bool listening = listener >= 0 &&
                     bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 &&
                     listen(listener, 1) == 0 &&
                     getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    check(listening, "loopback listener");
    if (!listening)
        return;
    check(platform->net_startup(), "net_startup");
    RoverSocket client = platform->tcp_connect("127.0.0.1", ntohs(address.sin_port));
    check(client != OA_ROVER_SOCKET_INVALID, "tcp_connect to loopback");
    int server = accept(listener, nullptr, nullptr);
    check(server >= 0, "accept");
    if (client != OA_ROVER_SOCKET_INVALID && server >= 0) {
        check(platform->tcp_send(client, "{hello}", 7) == 7, "tcp_send");
        char echoed[8] = {};
        check(
            recv(server, echoed, sizeof echoed, 0) == 7 && memcmp(echoed, "{hello}", 7) == 0,
            "server receives the bytes"
        );
        check(send(server, "{ok}", 4, 0) == 4, "server reply");
        char reply[8] = {};
        check(
            platform->tcp_recv(client, reply, sizeof reply) == 4 && memcmp(reply, "{ok}", 4) == 0,
            "tcp_recv"
        );
        close(server);
        check(platform->tcp_recv(client, reply, sizeof reply) == 0, "tcp_recv sees the close");
        platform->tcp_close(client);
    }
    platform->net_cleanup();
    close(listener);
}
#endif

} // namespace

void check_platform_selection(const RoverPlatform* native) {
    check(rover_platform() == native, "the core runs on the native table by default");
    RoverPlatform host = *native;
    rover_use_platform(&host);
    check(rover_platform() == &host, "a host table replaces the native one");
    rover_use_platform(nullptr);
    check(rover_platform() == native, "clearing the host table restores the native one");
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: oa-rover-platform-test SOURCE\n");
        return 2;
    }
    const RoverPlatform* platform = rover_native_platform();
    check_platform_selection(platform);
    check_threads_events_locks(platform);
    check_lock_handoff(platform);
    check_wait_any(platform);
    check_clock(platform);
    check_file(platform, argv[1]);
    check_file_write(platform);
    check(platform->net_startup(), "net_startup");
    check_ipv4(platform);
#if !defined(_WIN32)
    check_tcp(platform);
    check_async_tcp(platform);
    check_half_close_among_many(platform);
#endif
    platform->net_cleanup();
    if (failures)
        return 1;
    puts("rover platform: ok");
    return 0;
}
