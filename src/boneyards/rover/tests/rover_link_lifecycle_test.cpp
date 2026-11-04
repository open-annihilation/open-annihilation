// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The link's small routines without a network: the send and receive cipher
// counters, the socket event it creates, the scratch record free, the select
// table's slot locks, the fetch queue and the request service names.

#include "../src/http_fetch.h"
#include "../src/link.h"
#include "../src/platform_select.h"
#include "../src/record.h"
#include "../src/requests.h"
#include "../src/sockets.h"
#include "../src/state.h"

#include <stdio.h>
#include <string.h>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

int g_events_created = 0;
bool g_manual_reset = false;
bool g_initially_set = true;
RoverEvent* const kFakeEvent = reinterpret_cast<RoverEvent*>(0x5a5a);

RoverEvent* fake_event_create(bool manual_reset, bool initially_set) {
    ++g_events_created;
    g_manual_reset = manual_reset;
    g_initially_set = initially_set;
    return kFakeEvent;
}

void test_encrypt_uses_send_counter() {
    RoverConnection link{};
    link.send_counter = 1;
    link.receive_counter = 7;
    char bytes[] = {0x10, 0x20, 0x30};
    rover_link_encrypt_buffer(link, bytes, 3);
    check(
        bytes[0] == 0x11 && bytes[1] == 0x22 && bytes[2] == 0x33, "encrypt xors counter low bytes"
    );
    check(link.send_counter == 4, "encrypt advances the send counter per byte");
    check(link.receive_counter == 7, "encrypt leaves the receive counter");
}

void test_decrypt_uses_receive_counter() {
    RoverConnection link{};
    link.send_counter = 9;
    link.receive_counter = 0x1ff;
    char bytes[] = {static_cast<char>(0xff), 0x00};
    rover_link_decrypt_buffer(link, bytes, 2);
    check(bytes[0] == 0x00, "decrypt xors the low byte 0xff");
    check(bytes[1] == 0x00, "decrypt wraps the low byte to 0x00");
    check(link.receive_counter == 0x201, "decrypt keeps the full 32-bit counter");
    check(link.send_counter == 9, "decrypt leaves the send counter");
}

void test_empty_range_keeps_counter() {
    RoverConnection link{};
    link.send_counter = 5;
    char byte = 0x41;
    rover_link_encrypt_buffer(link, &byte, 0);
    rover_link_encrypt_buffer(link, &byte, -3);
    check(byte == 0x41 && link.send_counter == 5, "an empty or negative range changes nothing");
}

void test_round_trip() {
    RoverConnection sender{};
    RoverConnection receiver{};
    sender.send_counter = 1;
    receiver.receive_counter = 1;
    char text[] = "{Rover}";
    rover_link_encrypt_buffer(sender, text, 7);
    rover_link_decrypt_buffer(receiver, text, 7);
    check(text[0] == '{' && text[6] == '}', "matching counters decipher the stream");
}

void test_socket_event_arguments() {
    RoverPlatform fake = *rover_native_platform();
    fake.event_create = fake_event_create;
    rover_use_platform(&fake);
    RoverEvent* event = rover_link_new_socket_event();
    rover_use_platform(nullptr);
    check(event == kFakeEvent, "socket event comes from the platform");
    check(g_events_created == 1, "one event created");
    check(g_manual_reset && !g_initially_set, "manual reset, initially clear");
}

void test_free_scratch_record_keeps_slot() {
    RoverRecord* none = nullptr;
    rover_free_scratch_record(&none);
    check(none == nullptr, "a null slot stays null");
    RoverRecord* record = rover_record_create();
    check(record != nullptr, "scratch record created");
    RoverRecord* const kept = record;
    rover_free_scratch_record(&record);
    check(record == kept, "the slot is not cleared");
}

// A platform whose locks are distinct fake handles and whose threads never
// run, so the select table can be built and torn down on the test thread.
constexpr int32_t kLockLimit = 32;
uintptr_t g_next_lock = 0x1000;
RoverLock* g_created_locks[kLockLimit]{};
int32_t g_locks_created = 0;
RoverLock* g_destroyed_locks[kLockLimit]{};
int32_t g_locks_destroyed = 0;
int32_t g_threads_started = 0;
RoverThread* const kFakeThread = reinterpret_cast<RoverThread*>(0x7e7e);

RoverLock* fake_lock_create() {
    RoverLock* lock = reinterpret_cast<RoverLock*>(g_next_lock);
    g_next_lock += 0x10;
    if (g_locks_created < kLockLimit)
        g_created_locks[g_locks_created] = lock;
    ++g_locks_created;
    return lock;
}

void fake_lock_destroy(RoverLock* lock) {
    if (g_locks_destroyed < kLockLimit)
        g_destroyed_locks[g_locks_destroyed] = lock;
    ++g_locks_destroyed;
}

void fake_lock_use(RoverLock*) {
}

RoverThread* fake_thread_start(RoverThreadBody, void*) {
    ++g_threads_started;
    return kFakeThread;
}

void fake_thread_join(RoverThread*) {
}

void fake_event_use(RoverEvent*) {
}

bool was_created(RoverLock* lock) {
    for (int32_t i = 0; i < g_locks_created && i < kLockLimit; ++i) {
        if (g_created_locks[i] == lock)
            return true;
    }
    return false;
}

RoverPlatform quiet_platform() {
    RoverPlatform fake = *rover_native_platform();
    fake.lock_create = fake_lock_create;
    fake.lock_destroy = fake_lock_destroy;
    fake.lock_enter = fake_lock_use;
    fake.lock_leave = fake_lock_use;
    fake.thread_start = fake_thread_start;
    fake.thread_join = fake_thread_join;
    fake.event_create = fake_event_create;
    fake.event_set = fake_event_use;
    fake.event_reset = fake_event_use;
    fake.event_destroy = fake_event_use;
    return fake;
}

void test_socket_table_slot_locks() {
    RoverPlatform fake = quiet_platform();
    rover_use_platform(&fake);
    g_locks_created = 0;
    g_locks_destroyed = 0;
    g_threads_started = 0;
    check(rover_sockets_init(), "table lock created");
    const int32_t table_locks = g_locks_created;
    const RoverSocket socket = static_cast<RoverSocket>(7);
    check(
        rover_sockets_register(socket, kFakeEvent, rover_net_event_bit(RoverNetEvent::Read)) == 0,
        "first registration starts the table"
    );
    check(g_threads_started == 1, "one select thread started");
    check(g_locks_created - table_locks == 8, "each of the eight table slots gets its own lock");
    rover_sockets_shutdown();
    rover_use_platform(nullptr);
    check(g_locks_destroyed == 8, "the table teardown frees the eight slot locks");
    bool all_slot_locks = true;
    for (int32_t i = 0; i < g_locks_destroyed && i < kLockLimit; ++i) {
        all_slot_locks = all_slot_locks && was_created(g_destroyed_locks[i]);
        for (int32_t j = 0; j < i; ++j)
            all_slot_locks = all_slot_locks && g_destroyed_locks[j] != g_destroyed_locks[i];
    }
    check(all_slot_locks, "each slot lock is freed once");
}

void test_fetch_queue_starts_empty() {
    RoverPlatform fake = quiet_platform();
    rover_use_platform(&fake);
    g_locks_created = 0;
    RoverFetchQueue queue{};
    bool ready = rover_fetch_queue_init(queue);
    rover_use_platform(nullptr);
    check(ready && queue.lock != nullptr && g_locks_created == 1, "fetch queue lock created");
    check(queue.first == nullptr && queue.last == nullptr && queue.count == 0, "fetch queue empty");
    check(queue.ready == nullptr, "fetch queue event is left to the session");
}

void test_service_names() {
    const char* const expected[] = {
        "Dispatcher",
        "CommManager",
        "Administrator",
        "Rover",
        "ChatManager",
        "UserManager",
        "GameManager",
        "YardMonitor",
        "YardMaster",
        "Guarddog",
        "WarManager",
        "ExtensionDll",
        "Retriever",
        "GuarddogProxy",
        "Crusades",
        "HouseManager",
    };
    bool all = true;
    for (int32_t i = 0; i < 16; ++i) {
        const char* name = rover_service_name(static_cast<RoverService>(i));
        all = all && name && strcmp(name, expected[i]) == 0;
    }
    check(all, "service table order");
    check(rover_service_name(static_cast<RoverService>(-1)) == nullptr, "negative service");
    check(rover_service_name(static_cast<RoverService>(17)) == nullptr, "service past the table");
}

} // namespace

int main() {
    test_encrypt_uses_send_counter();
    test_decrypt_uses_receive_counter();
    test_empty_range_keeps_counter();
    test_round_trip();
    test_socket_event_arguments();
    test_free_scratch_record_keeps_slot();
    test_socket_table_slot_locks();
    test_fetch_queue_starts_empty();
    test_service_names();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("rover link lifecycle: ok\n");
    return 0;
}
