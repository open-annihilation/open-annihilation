// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

/* The operating-system services the Rover core uses: TCP streams, worker
 * threads, auto-reset and manual-reset events, recursive locks, files and a
 * millisecond clock. The core reaches them only through a RoverPlatform
 * table, so tests and oa-game can substitute their own; the native table
 * wraps Winsock and Win32 or POSIX sockets and pthreads.
 *
 * The lobby link uses a Winsock 1.1 subset:
 * non-blocking sockets, select over read/write/exception sets, the pending
 * socket error and a half close. Socket error codes are Winsock-numbered
 * (10061 refused, 10054 reset, ...) on every host, because the lobby stores
 * them and hands them to the game's disconnect callback.
 *
 * Compiles as C11 and C++20. */
#ifndef OA_ROVER_PLATFORM_H
#define OA_ROVER_PLATFORM_H

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A connected stream socket, or OA_ROVER_SOCKET_INVALID. */
typedef intptr_t RoverSocket;
#define OA_ROVER_SOCKET_INVALID ((RoverSocket) - 1)

/* Waits that never time out. */
#define OA_ROVER_WAIT_FOREVER 0xffffffffu

/* tcp_send / tcp_recv on a non-blocking socket that would have had to wait. */
#define OA_ROVER_IO_WOULD_BLOCK (-2)

/* tcp_select interest and readiness bits, one per select set. */
#define OA_ROVER_SOCKET_READABLE 0x1u
#define OA_ROVER_SOCKET_WRITABLE 0x2u /* also: a connect finished */
#define OA_ROVER_SOCKET_FAILED 0x4u   /* exception set: a connect failed */

/* event_wait_any result when nothing was signalled in time. */
#define OA_ROVER_WAIT_TIMEOUT (-1)

typedef struct RoverThread RoverThread;
typedef struct RoverEvent RoverEvent;
typedef struct RoverLock RoverLock;
typedef struct RoverFile RoverFile;

typedef void (*RoverThreadBody)(void* context);

typedef struct RoverPlatform {
    /* Brings up the socket library (Winsock 1.1 on Windows); false when unavailable. */
    bool (*net_startup)(void);
    void (*net_cleanup)(void);
    /* Resolves host (name or dotted quad) and connects; blocks until done. */
    RoverSocket (*tcp_connect)(const char* host, uint16_t port);
    /* Returns bytes accepted, OA_ROVER_IO_WOULD_BLOCK, or -1 on a broken connection. */
    int32_t (*tcp_send)(RoverSocket socket, const void* data, int32_t length);
    /* Returns bytes read, 0 when the peer closed, OA_ROVER_IO_WOULD_BLOCK, or -1 on error. */
    int32_t (*tcp_recv)(RoverSocket socket, void* buffer, int32_t capacity);
    void (*tcp_close)(RoverSocket socket);
    /* Creates a non-blocking stream socket. */
    RoverSocket (*tcp_create)(void);
    /* Starts connecting a tcp_create socket to a host-order IPv4 address; true while the
     * connect completed or is still in progress, false when it failed at once. */
    bool (*tcp_connect_start)(RoverSocket socket, uint32_t address, uint16_t port);
    /* Waits like select: for each socket, interest selects the sets it joins and ready
     * receives the sets it was found in. Returns the number of ready sockets, 0 on
     * timeout, or -1 on error. A failed connect is reported in the exception set. */
    int32_t (*tcp_select)(
        const RoverSocket* sockets,
        const uint8_t* interest,
        uint8_t* ready,
        int32_t count,
        uint32_t timeout_ms
    );
    /* The socket's pending error (SO_ERROR), 0 when none, or -1 when it cannot be read
     * (net_last_error says why: WSAENOTSOCK once the socket is gone). */
    int32_t (*tcp_pending_error)(RoverSocket socket);
    /* Stops sending; the peer sees end of stream. */
    void (*tcp_shutdown_send)(RoverSocket socket);
    /* The calling thread's last socket error. */
    int32_t (*net_last_error)(void);
    /* Parses a numeric IPv4 address as inet_addr does, into host byte order. */
    bool (*ipv4_parse)(const char* text, uint32_t* out_address);
    /* Resolves a host name to its first IPv4 address, in host byte order. */
    bool (*ipv4_lookup)(const char* host, uint32_t* out_address);

    /* Returns null when the thread could not be started. */
    RoverThread* (*thread_start)(RoverThreadBody body, void* context);
    /* Waits for the body to return and releases the handle. */
    void (*thread_join)(RoverThread* thread);

    RoverEvent* (*event_create)(bool manual_reset, bool initially_set);
    void (*event_set)(RoverEvent* event);
    void (*event_reset)(RoverEvent* event);
    /* True when signalled within timeout_ms; an auto-reset event is consumed. */
    bool (*event_wait)(RoverEvent* event, uint32_t timeout_ms);
    /* Waits for any of count events (at most 64) like WaitForMultipleObjects: returns
     * the lowest signalled index, consuming it if auto-reset, or OA_ROVER_WAIT_TIMEOUT. */
    int32_t (*event_wait_any)(RoverEvent* const* events, int32_t count, uint32_t timeout_ms);
    void (*event_destroy)(RoverEvent* event);

    /* Recursive, like a Win32 critical section, and handed on release to a thread
     * already waiting, as the critical sections of the Windows versions 3.1c
     * shipped for were: the releasing thread cannot take it straight back. */
    RoverLock* (*lock_create)(void);
    void (*lock_enter)(RoverLock* lock);
    void (*lock_leave)(RoverLock* lock);
    void (*lock_destroy)(RoverLock* lock);

    /* Opens for reading; null when absent. */
    RoverFile* (*file_open)(const char* path);
    /* Creates or truncates for writing; null when it cannot. Bytes are written as given. */
    RoverFile* (*file_create)(const char* path);
    /* Returns bytes read, 0 at end of file, or -1 on error. */
    int32_t (*file_read)(RoverFile* file, void* buffer, int32_t capacity);
    /* Returns bytes written, or -1 on error. */
    int32_t (*file_write)(RoverFile* file, const void* data, int32_t length);
    void (*file_close)(RoverFile* file);

    /* Monotonic milliseconds; wraps like GetTickCount. */
    uint32_t (*clock_ms)(void);
    /* Yields when milliseconds is 0. */
    void (*sleep_ms)(uint32_t milliseconds);
} RoverPlatform;

/// Returns the platform table for the build target's operating system.
///
/// @return the static native table
const RoverPlatform* rover_native_platform(void);

/// Selects the table the Rover core runs on.
///
/// Takes effect for everything after the call, so change it only while the interface is detached.
///
/// @param platform table to use, or null to restore the native one; it must outlive its use
void rover_use_platform(const RoverPlatform* platform);

#ifdef __cplusplus
}
#endif

#endif
