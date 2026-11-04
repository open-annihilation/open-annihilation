// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The select thread and its table of registered sockets.

#include "sockets.h"

#include "state.h"

#include <stdlib.h>

namespace {

constexpr int32_t kSlotCount = 8;
// How long one select waits before it rescans the table.
constexpr uint32_t kSelectTimeoutMs = 1000;
// Pause after a select error while no socket is registered.
constexpr uint32_t kIdleRetryMs = 100;
// Pending-error polls of a failed socket, and the pause between them.
constexpr int32_t kErrorPolls = 10;
constexpr uint32_t kErrorPollMs = 10;
constexpr int32_t kWsaNotSocket = 10038;
constexpr int32_t kWsaNotConnected = 10057;

// One registered socket.
struct RoverSocketSlot {
    RoverSocket socket{OA_ROVER_SOCKET_INVALID};
    RoverLock* lock{};
    bool connected{};
    uint32_t mask{};                        // rover_net_event_bit of each wanted event
    int32_t pending[kRoverNetEventCount]{}; // raised and not yet collected
    int32_t errors[kRoverNetEventCount]{};
    RoverEvent* notify{};     // owned by the registering link
    int32_t writes_pending{}; // unfinished sends
};

struct RoverSocketTable {
    RoverLock* lock{};
    RoverSocketSlot* slots{}; // kSlotCount, allocated with the thread
    RoverEvent* wake{};       // auto-reset; wakes the thread when a socket registers
    RoverThread* thread{};
    int32_t registered{};
    bool stopping{};
    bool running{};
};

RoverSocketTable g_sockets;

bool wants(const RoverSocketSlot& slot, RoverNetEvent event) {
    return (slot.mask & rover_net_event_bit(event)) != 0;
}

/// Raises one event of a slot and signals its link.
///
/// @param[in,out] slot table slot of the socket
/// @param event event to raise
void signal_event(RoverSocketSlot& slot, RoverNetEvent event) {
    ++slot.pending[static_cast<int32_t>(event)];
    rover_platform()->event_set(slot.notify);
}

/// Takes back one raised event; the link's event is reset once that count is spent.
///
/// @param[in,out] slot table slot of the socket
/// @param event index of the event, a RoverNetEvent value
void acknowledge_event(RoverSocketSlot& slot, int32_t event) {
    int32_t& pending = slot.pending[event];
    if (pending > 0)
        --pending;
    if (pending == 0)
        rover_platform()->event_reset(slot.notify);
}

/// Returns a slot to its empty state: no socket, no events, errors or wanted events, and one
/// unfinished send.
///
/// @param[in,out] slot table slot of the socket
void reset_slot(RoverSocketSlot& slot) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(slot.lock);
    slot.socket = OA_ROVER_SOCKET_INVALID;
    slot.connected = false;
    if (slot.notify)
        platform->event_reset(slot.notify);
    slot.notify = nullptr;
    slot.mask = 0;
    for (int32_t i = 0; i < kRoverNetEventCount; ++i) {
        slot.errors[i] = 0;
        slot.pending[i] = 0;
    }
    slot.writes_pending = 1;
    platform->lock_leave(slot.lock);
}

/// Gives a table slot its lock and empties it.
///
/// @param[out] slot slot to set up
void init_slot(RoverSocketSlot& slot) {
    slot = RoverSocketSlot{};
    slot.lock = rover_platform()->lock_create();
    reset_slot(slot);
}

/// Frees a table slot's lock.
///
/// @param[in,out] slot table slot of the socket
void destroy_slot(RoverSocketSlot& slot) {
    rover_platform()->lock_destroy(slot.lock);
    slot.lock = nullptr;
}

/// Destroys every slot of the table and frees it.
///
/// @param slots the table's kSlotCount slots
void free_slot_table(RoverSocketSlot* slots) {
    for (int32_t i = 0; i < kSlotCount; ++i)
        destroy_slot(slots[i]);
    free(slots);
}

/// Records a socket's failure against every event and raises the ones its link waits for.
///
/// The pending error is polled up to ten times, 10 ms apart; none found is recorded as WSAENOTCONN.
/// A connecting socket raises connect, a connected one write and read, and accept and close are
/// raised when wanted.
///
/// @param[in,out] slot table slot of the socket
void fail_slot(RoverSocketSlot& slot) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(slot.lock);
    int32_t error = 0;
    for (int32_t poll = 0; poll < kErrorPolls; ++poll) {
        error = platform->tcp_pending_error(slot.socket);
        if (error == -1)
            error = platform->net_last_error();
        if (error)
            break;
        platform->sleep_ms(kErrorPollMs);
    }
    if (!error)
        error = kWsaNotConnected;
    for (int32_t& slot_error : slot.errors)
        slot_error = error;
    if (wants(slot, RoverNetEvent::Connect) && !slot.connected)
        signal_event(slot, RoverNetEvent::Connect);
    if (wants(slot, RoverNetEvent::Write) && slot.connected)
        signal_event(slot, RoverNetEvent::Write);
    if (wants(slot, RoverNetEvent::Read) && slot.connected)
        signal_event(slot, RoverNetEvent::Read);
    if (wants(slot, RoverNetEvent::Accept))
        signal_event(slot, RoverNetEvent::Accept);
    if (wants(slot, RoverNetEvent::Close))
        signal_event(slot, RoverNetEvent::Close);
    platform->lock_leave(slot.lock);
    platform->sleep_ms(0);
}

/// Raises a read event when the link wants one.
///
/// @param[in,out] slot table slot of the socket
void signal_readable(RoverSocketSlot& slot) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(slot.lock);
    if (wants(slot, RoverNetEvent::Read))
        signal_event(slot, RoverNetEvent::Read);
    platform->lock_leave(slot.lock);
    platform->sleep_ms(0);
}

/// Handles a writable report: the first finishes the connect; later ones end a send that had to
/// wait.
///
/// @param[in,out] slot table slot of the socket
void signal_writable(RoverSocketSlot& slot) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(slot.lock);
    if (!slot.connected) {
        slot.connected = true;
        if (wants(slot, RoverNetEvent::Connect))
            signal_event(slot, RoverNetEvent::Connect);
        platform->lock_leave(slot.lock);
        platform->sleep_ms(0);
        return;
    }
    if (slot.writes_pending != 0) {
        if (wants(slot, RoverNetEvent::Write))
            signal_event(slot, RoverNetEvent::Write);
        slot.writes_pending = 0;
    }
    platform->lock_leave(slot.lock);
}

// Slot holding socket; null when none does. Table lock held.
RoverSocketSlot* find_slot(RoverSocket socket) {
    if (!g_sockets.slots)
        return nullptr;
    for (int32_t i = 0; i < kSlotCount; ++i) {
        if (g_sockets.slots[i].socket == socket)
            return &g_sockets.slots[i];
    }
    return nullptr;
}

/// Runs the select thread: watches the registered sockets until the table shuts down.
///
/// It sleeps while no socket is registered, selects for up to a second at a time, and turns each
/// readiness into slot events. A select error other than WSAENOTSOCK stops the thread.
///
/// @param context thread context; unused
void run_select_thread(void* /*context*/) {
    const RoverPlatform* platform = rover_platform();
    g_sockets.running = true;
    for (;;) {
        platform->lock_enter(g_sockets.lock);
        while (g_sockets.registered == 0) {
            if (g_sockets.stopping) {
                platform->lock_leave(g_sockets.lock);
                g_sockets.running = false;
                return;
            }
            platform->lock_leave(g_sockets.lock);
            platform->event_wait(g_sockets.wake, OA_ROVER_WAIT_FOREVER);
            platform->lock_enter(g_sockets.lock);
        }
        if (g_sockets.stopping) {
            platform->lock_leave(g_sockets.lock);
            g_sockets.running = false;
            return;
        }
        RoverSocket sockets[kSlotCount];
        uint8_t interest[kSlotCount];
        uint8_t ready[kSlotCount];
        int32_t watched = 0;
        bool watching_writes = false;
        for (int32_t i = 0; i < kSlotCount; ++i) {
            const RoverSocketSlot& slot = g_sockets.slots[i];
            if (slot.socket == OA_ROVER_SOCKET_INVALID)
                continue;
            sockets[watched] = slot.socket;
            interest[watched] = OA_ROVER_SOCKET_READABLE | OA_ROVER_SOCKET_FAILED;
            if (slot.writes_pending > 0) {
                interest[watched] |= OA_ROVER_SOCKET_WRITABLE;
                watching_writes = true;
            }
            ++watched;
        }
        platform->lock_leave(g_sockets.lock);

        int32_t selected =
            platform->tcp_select(sockets, interest, ready, watched, kSelectTimeoutMs);
        if (selected < 0) {
            if (platform->net_last_error() == kWsaNotSocket) {
                // A socket was closed under the wait: fail each one whose
                // pending error can no longer be read, whatever the reason.
                if (g_sockets.registered > 0) {
                    platform->lock_enter(g_sockets.lock);
                    for (int32_t i = 0; i < kSlotCount; ++i) {
                        RoverSocketSlot& slot = g_sockets.slots[i];
                        if (slot.socket != OA_ROVER_SOCKET_INVALID &&
                            platform->tcp_pending_error(slot.socket) == -1)
                            fail_slot(slot);
                    }
                    platform->lock_leave(g_sockets.lock);
                } else {
                    platform->sleep_ms(kIdleRetryMs);
                }
                continue;
            }
            if (g_sockets.registered > 0) {
                platform->lock_enter(g_sockets.lock);
                g_sockets.stopping = true;
                g_sockets.registered = 0;
                if (g_sockets.wake)
                    platform->event_set(g_sockets.wake);
                platform->lock_leave(g_sockets.lock);
                g_sockets.running = false;
                return;
            }
            continue;
        }
        if (selected == 0)
            continue;
        platform->lock_enter(g_sockets.lock);
        for (int32_t i = 0; i < watched; ++i) {
            RoverSocketSlot* slot = find_slot(sockets[i]);
            if (!slot)
                continue;
            if (ready[i] & OA_ROVER_SOCKET_FAILED)
                fail_slot(*slot);
            if (watching_writes && (ready[i] & OA_ROVER_SOCKET_WRITABLE))
                signal_writable(*slot);
            if (ready[i] & OA_ROVER_SOCKET_READABLE)
                signal_readable(*slot);
        }
        platform->lock_leave(g_sockets.lock);
        if (watching_writes)
            platform->sleep_ms(0);
    }
}

/// Creates the wake event and the table and starts the select thread.
///
/// Called with the table lock held.
///
/// @return false when the event, the table or the thread could not be created, or the table already
///     exists
bool start_select_thread() {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(g_sockets.lock);
    g_sockets.stopping = false;
    if (!g_sockets.wake) {
        g_sockets.wake = platform->event_create(false, false);
        if (!g_sockets.wake) {
            platform->lock_leave(g_sockets.lock);
            return false;
        }
    } else {
        platform->event_reset(g_sockets.wake);
    }
    if (g_sockets.slots) {
        platform->lock_leave(g_sockets.lock);
        return false;
    }
    RoverSocketSlot* slots =
        static_cast<RoverSocketSlot*>(calloc(kSlotCount, sizeof(RoverSocketSlot)));
    if (!slots) {
        platform->lock_leave(g_sockets.lock);
        return false;
    }
    for (int32_t i = 0; i < kSlotCount; ++i)
        init_slot(slots[i]);
    g_sockets.slots = slots;
    g_sockets.thread = platform->thread_start(run_select_thread, nullptr);
    bool started = g_sockets.thread != nullptr;
    platform->lock_leave(g_sockets.lock);
    return started;
}

} // namespace

bool rover_sockets_init() {
    if (!g_sockets.lock)
        g_sockets.lock = rover_platform()->lock_create();
    return g_sockets.lock != nullptr;
}

int32_t rover_sockets_register(RoverSocket socket, RoverEvent* notify, uint32_t mask) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(g_sockets.lock);
    if (!g_sockets.slots && !start_select_thread()) {
        platform->lock_leave(g_sockets.lock);
        return -1;
    }
    if (RoverSocketSlot* slot = find_slot(socket)) {
        if (mask == 0)
            rover_sockets_remove(socket);
        else
            slot->mask = mask;
        platform->lock_leave(g_sockets.lock);
        return 0;
    }
    int32_t result = 0;
    if (mask != 0) {
        RoverSocketSlot* free_slot = notify ? find_slot(OA_ROVER_SOCKET_INVALID) : nullptr;
        if (free_slot) {
            ++g_sockets.registered;
            free_slot->socket = socket;
            free_slot->notify = notify;
            free_slot->mask = mask;
            if (g_sockets.registered == 1)
                platform->event_set(g_sockets.wake);
        } else {
            result = -1;
        }
    }
    platform->lock_leave(g_sockets.lock);
    return result;
}

int32_t rover_sockets_collect(RoverSocket socket, RoverNetEvents* out_events) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(g_sockets.lock);
    RoverSocketSlot* slot = find_slot(socket);
    if (!slot) {
        platform->lock_leave(g_sockets.lock);
        return -1;
    }
    platform->lock_enter(slot->lock);
    out_events->signalled = 0;
    for (int32_t i = 0; i < kRoverNetEventCount; ++i) {
        if (slot->pending[i]) {
            out_events->signalled |= 1u << i;
            acknowledge_event(*slot, i);
        }
        out_events->errors[i] = slot->errors[i];
        slot->errors[i] = 0;
    }
    platform->lock_leave(slot->lock);
    platform->lock_leave(g_sockets.lock);
    return 0;
}

void rover_sockets_clear_read(RoverSocket socket) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(g_sockets.lock);
    if (RoverSocketSlot* slot = find_slot(socket)) {
        slot->pending[static_cast<int32_t>(RoverNetEvent::Read)] = 0;
        platform->event_reset(slot->notify);
    }
    platform->lock_leave(g_sockets.lock);
}

void rover_sockets_fail(RoverSocket socket) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(g_sockets.lock);
    if (RoverSocketSlot* slot = find_slot(socket))
        fail_slot(*slot);
    platform->lock_leave(g_sockets.lock);
}

void rover_sockets_remove(RoverSocket socket) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(g_sockets.lock);
    if (RoverSocketSlot* slot = find_slot(socket)) {
        reset_slot(*slot);
        --g_sockets.registered;
    }
    platform->lock_leave(g_sockets.lock);
}

void rover_sockets_begin_send(RoverSocket socket) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(g_sockets.lock);
    if (g_sockets.registered > 0) {
        if (RoverSocketSlot* slot = find_slot(socket)) {
            platform->lock_enter(slot->lock);
            if (wants(*slot, RoverNetEvent::Write))
                ++slot->writes_pending;
            platform->lock_leave(slot->lock);
        }
    }
    platform->lock_leave(g_sockets.lock);
}

void rover_sockets_end_send(RoverSocket socket) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(g_sockets.lock);
    if (g_sockets.registered > 0) {
        if (RoverSocketSlot* slot = find_slot(socket)) {
            platform->lock_enter(slot->lock);
            if (wants(*slot, RoverNetEvent::Write))
                --slot->writes_pending;
            platform->lock_leave(slot->lock);
        }
    }
    platform->lock_leave(g_sockets.lock);
}

void rover_sockets_shutdown() {
    const RoverPlatform* platform = rover_platform();
    if (!g_sockets.lock)
        return;
    platform->lock_enter(g_sockets.lock);
    g_sockets.stopping = true;
    g_sockets.registered = 0;
    if (g_sockets.wake)
        platform->event_set(g_sockets.wake);
    platform->lock_leave(g_sockets.lock);
    if (g_sockets.thread) {
        platform->thread_join(g_sockets.thread);
        g_sockets.thread = nullptr;
    }
    if (g_sockets.wake) {
        platform->event_destroy(g_sockets.wake);
        g_sockets.wake = nullptr;
    }
    g_sockets.running = false;
    if (g_sockets.slots) {
        free_slot_table(g_sockets.slots);
        g_sockets.slots = nullptr;
    }
}
