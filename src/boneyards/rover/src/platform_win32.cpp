// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rover platform services on Windows: Winsock, Win32 threads and events,
// handoff locks, and kernel file handles.

#include "oa/rover/platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// inet_addr and gethostbyname are the Winsock 1.1 calls the lobby is built on.
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif
#include <winsock2.h>
#include <windows.h>

#include <stdlib.h>
#include <string.h>

struct RoverThread {
    HANDLE handle;
    RoverThreadBody body;
    void* context;
};

// A recursive lock handed to a waiting thread on release, as the critical
// sections of the Windows versions 3.1c shipped for were. Today's let the releasing thread
// take them straight back, and the select thread, which releases and retakes
// the socket-table lock in a tight loop, would keep the link thread out.
struct RoverLock {
    volatile LONG claims; // -1 while free; the owner and each waiter add one
    volatile LONG owner;  // thread id of the owner, 0 while free
    uint32_t depth;       // touched by the owner only
    HANDLE handoff;       // auto-reset; set when the lock passes to a waiter
};

namespace {

// Rover asks for Winsock 1.1 and rejects any other negotiated version.
constexpr WORD kWinsockVersion = MAKEWORD(1, 1);

HANDLE event_handle(RoverEvent* event) {
    return reinterpret_cast<HANDLE>(event);
}

HANDLE file_handle(RoverFile* file) {
    return reinterpret_cast<HANDLE>(file);
}

bool net_startup() {
    WSADATA data{};
    if (WSAStartup(kWinsockVersion, &data) != 0)
        return false;
    if (data.wVersion != kWinsockVersion) {
        WSACleanup();
        return false;
    }
    return true;
}

void net_cleanup() {
    WSACleanup();
}

RoverSocket tcp_connect(const char* host, uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = inet_addr(host);
    if (address.sin_addr.s_addr == INADDR_NONE) {
        hostent* entry = gethostbyname(host);
        if (!entry || entry->h_addrtype != AF_INET || !entry->h_addr_list[0])
            return OA_ROVER_SOCKET_INVALID;
        memcpy(&address.sin_addr, entry->h_addr_list[0], sizeof address.sin_addr);
    }
    SOCKET handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET)
        return OA_ROVER_SOCKET_INVALID;
    if (connect(handle, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        closesocket(handle);
        return OA_ROVER_SOCKET_INVALID;
    }
    return static_cast<RoverSocket>(handle);
}

int32_t socket_failure() {
    return WSAGetLastError() == WSAEWOULDBLOCK ? OA_ROVER_IO_WOULD_BLOCK : -1;
}

int32_t tcp_send(RoverSocket socket, const void* data, int32_t length) {
    int sent = send(static_cast<SOCKET>(socket), static_cast<const char*>(data), length, 0);
    return sent == SOCKET_ERROR ? socket_failure() : sent;
}

int32_t tcp_recv(RoverSocket socket, void* buffer, int32_t capacity) {
    int received = recv(static_cast<SOCKET>(socket), static_cast<char*>(buffer), capacity, 0);
    return received == SOCKET_ERROR ? socket_failure() : received;
}

void tcp_close(RoverSocket socket) {
    closesocket(static_cast<SOCKET>(socket));
}

RoverSocket tcp_create() {
    SOCKET handle = socket(AF_INET, SOCK_STREAM, 0);
    if (handle == INVALID_SOCKET)
        return OA_ROVER_SOCKET_INVALID;
    u_long non_blocking = 1;
    if (ioctlsocket(handle, FIONBIO, &non_blocking) == SOCKET_ERROR) {
        int error = WSAGetLastError();
        closesocket(handle);
        WSASetLastError(error);
        return OA_ROVER_SOCKET_INVALID;
    }
    return static_cast<RoverSocket>(handle);
}

bool tcp_connect_start(RoverSocket socket, uint32_t address, uint16_t port) {
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    target.sin_addr.s_addr = htonl(address);
    if (connect(
            static_cast<SOCKET>(socket), reinterpret_cast<const sockaddr*>(&target), sizeof target
        ) != SOCKET_ERROR)
        return true;
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

int32_t tcp_select(
    const RoverSocket* sockets,
    const uint8_t* interest,
    uint8_t* ready,
    int32_t count,
    uint32_t timeout_ms
) {
    fd_set readable;
    fd_set writable;
    fd_set failed;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    for (int32_t i = 0; i < count; ++i) {
        SOCKET handle = static_cast<SOCKET>(sockets[i]);
        if (interest[i] & OA_ROVER_SOCKET_READABLE)
            FD_SET(handle, &readable);
        if (interest[i] & OA_ROVER_SOCKET_WRITABLE)
            FD_SET(handle, &writable);
        if (interest[i] & OA_ROVER_SOCKET_FAILED)
            FD_SET(handle, &failed);
        ready[i] = 0;
    }
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(timeout_ms / 1000u);
    timeout.tv_usec = static_cast<long>(timeout_ms % 1000u) * 1000L;
    int selected = select(
        0, &readable, &writable, &failed, timeout_ms == OA_ROVER_WAIT_FOREVER ? nullptr : &timeout
    );
    if (selected == SOCKET_ERROR)
        return -1;
    int32_t found = 0;
    for (int32_t i = 0; i < count; ++i) {
        SOCKET handle = static_cast<SOCKET>(sockets[i]);
        if (FD_ISSET(handle, &readable))
            ready[i] |= OA_ROVER_SOCKET_READABLE;
        if (FD_ISSET(handle, &writable))
            ready[i] |= OA_ROVER_SOCKET_WRITABLE;
        if (FD_ISSET(handle, &failed))
            ready[i] |= OA_ROVER_SOCKET_FAILED;
        if (ready[i])
            ++found;
    }
    return found;
}

int32_t tcp_pending_error(RoverSocket socket) {
    int error = 0;
    int length = sizeof error;
    if (getsockopt(
            static_cast<SOCKET>(socket),
            SOL_SOCKET,
            SO_ERROR,
            reinterpret_cast<char*>(&error),
            &length
        ) == SOCKET_ERROR)
        return -1;
    return error;
}

void tcp_shutdown_send(RoverSocket socket) {
    shutdown(static_cast<SOCKET>(socket), SD_SEND);
}

int32_t net_last_error() {
    return WSAGetLastError();
}

bool ipv4_parse(const char* text, uint32_t* out_address) {
    unsigned long address = inet_addr(text);
    if (address == INADDR_NONE)
        return false;
    *out_address = ntohl(address);
    return true;
}

bool ipv4_lookup(const char* host, uint32_t* out_address) {
    hostent* entry = gethostbyname(host);
    if (!entry || entry->h_addrtype != AF_INET || !entry->h_addr_list[0])
        return false;
    in_addr address{};
    memcpy(&address, entry->h_addr_list[0], sizeof address);
    *out_address = ntohl(address.s_addr);
    return true;
}

DWORD WINAPI run_thread(LPVOID argument) {
    RoverThread* thread = static_cast<RoverThread*>(argument);
    thread->body(thread->context);
    return 0;
}

RoverThread* thread_start(RoverThreadBody body, void* context) {
    RoverThread* thread = static_cast<RoverThread*>(calloc(1, sizeof(RoverThread)));
    if (!thread)
        return nullptr;
    thread->body = body;
    thread->context = context;
    DWORD id = 0;
    thread->handle = CreateThread(nullptr, 0, run_thread, thread, 0, &id);
    if (!thread->handle) {
        free(thread);
        return nullptr;
    }
    return thread;
}

void thread_join(RoverThread* thread) {
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
    free(thread);
}

RoverEvent* event_create(bool manual_reset, bool initially_set) {
    return reinterpret_cast<RoverEvent*>(
        CreateEventA(nullptr, manual_reset, initially_set, nullptr)
    );
}

void event_set(RoverEvent* event) {
    SetEvent(event_handle(event));
}

void event_reset(RoverEvent* event) {
    ResetEvent(event_handle(event));
}

bool event_wait(RoverEvent* event, uint32_t timeout_ms) {
    return WaitForSingleObject(event_handle(event), timeout_ms) == WAIT_OBJECT_0;
}

int32_t event_wait_any(RoverEvent* const* events, int32_t count, uint32_t timeout_ms) {
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    if (count <= 0 || count > MAXIMUM_WAIT_OBJECTS)
        return OA_ROVER_WAIT_TIMEOUT;
    for (int32_t i = 0; i < count; ++i)
        handles[i] = event_handle(events[i]);
    DWORD waited = WaitForMultipleObjects(static_cast<DWORD>(count), handles, FALSE, timeout_ms);
    if (waited < WAIT_OBJECT_0 + static_cast<DWORD>(count))
        return static_cast<int32_t>(waited - WAIT_OBJECT_0);
    return OA_ROVER_WAIT_TIMEOUT;
}

void event_destroy(RoverEvent* event) {
    CloseHandle(event_handle(event));
}

RoverLock* lock_create() {
    RoverLock* lock = static_cast<RoverLock*>(calloc(1, sizeof(RoverLock)));
    if (!lock)
        return nullptr;
    lock->claims = -1;
    lock->handoff = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!lock->handoff) {
        free(lock);
        return nullptr;
    }
    return lock;
}

// A thread that finds the lock claimed waits for the owner to hand it over.
void lock_enter(RoverLock* lock) {
    LONG self = static_cast<LONG>(GetCurrentThreadId());
    if (lock->owner == self) {
        ++lock->depth;
        return;
    }
    if (InterlockedIncrement(&lock->claims) != 0)
        WaitForSingleObject(lock->handoff, INFINITE);
    InterlockedExchange(&lock->owner, self);
    lock->depth = 1;
}

void lock_leave(RoverLock* lock) {
    if (--lock->depth != 0)
        return;
    InterlockedExchange(&lock->owner, 0);
    if (InterlockedDecrement(&lock->claims) >= 0)
        SetEvent(lock->handoff);
}

void lock_destroy(RoverLock* lock) {
    CloseHandle(lock->handoff);
    free(lock);
}

// A path written as UTF-8, which is how a folder outside the game folder is
// named. CreateFileA reads the code page, so a name it cannot open is tried
// as UTF-8. An ASCII name is opened by CreateFileA and never reaches here.
static HANDLE open_utf8(const char* path) {
    wchar_t wide[4096];
    if (path == nullptr || path[0] == '\0')
        return INVALID_HANDLE_VALUE;
    const int written = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        path,
        -1,
        wide,
        static_cast<int>(sizeof wide / sizeof wide[0])
    );
    if (written <= 0)
        return INVALID_HANDLE_VALUE;
    return CreateFileW(
        wide, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr
    );
}

RoverFile* file_open(const char* path) {
    HANDLE handle = CreateFileA(
        path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr
    );
    if (handle == INVALID_HANDLE_VALUE)
        handle = open_utf8(path);
    return handle == INVALID_HANDLE_VALUE ? nullptr : reinterpret_cast<RoverFile*>(handle);
}

RoverFile* file_create(const char* path) {
    HANDLE handle =
        CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return handle == INVALID_HANDLE_VALUE ? nullptr : reinterpret_cast<RoverFile*>(handle);
}

int32_t file_read(RoverFile* file, void* buffer, int32_t capacity) {
    DWORD read = 0;
    if (!ReadFile(file_handle(file), buffer, static_cast<DWORD>(capacity), &read, nullptr))
        return -1;
    return static_cast<int32_t>(read);
}

int32_t file_write(RoverFile* file, const void* data, int32_t length) {
    DWORD written = 0;
    if (!WriteFile(file_handle(file), data, static_cast<DWORD>(length), &written, nullptr))
        return -1;
    return static_cast<int32_t>(written);
}

void file_close(RoverFile* file) {
    CloseHandle(file_handle(file));
}

uint32_t clock_ms() {
    return GetTickCount();
}

void sleep_ms(uint32_t milliseconds) {
    Sleep(milliseconds);
}

const RoverPlatform kWin32Platform = {
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
    return &kWin32Platform;
}
