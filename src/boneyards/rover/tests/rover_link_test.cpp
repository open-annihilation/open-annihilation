// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The lobby link on a platform whose sockets reach a scripted server in
// memory: a login runs through the yard master and a boneyard, and then each
// request the game queues must reach the socket on the link thread's first
// wake-up, woken by the queue's event and by nothing else.

#include "../src/state.h"
#include "oa/rover/cipher.h"
#include "oa/rover/interface.h"
#include "oa/rover/platform.h"
#include "oa/platform/system.hpp"
#include "oa/platform/files.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#define make_directory(path) _mkdir(path)
#define change_directory(path) _chdir(path)
#define remove_directory(path) _rmdir(path)
#define process_id() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_directory(path) mkdir(path, 0755)
#define change_directory(path) chdir(path)
#define remove_directory(path) rmdir(path)
#define process_id() getpid()
#endif

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

constexpr char kServerLocation[] = "Scripted Server";
constexpr uint16_t kYardMasterPort = 9110;
constexpr uint16_t kBoneyardPort = 9111;
constexpr uint32_t kUserId = 0x2a;
constexpr int32_t kLoginConfirmedStage = 8;
constexpr int kWaitLimitMs = 5000;
// How long the link thread must sleep before the link counts as idle.
constexpr int kIdleMs = 100;

// ---- the scripted server behind the fake sockets ------------------------------

constexpr RoverSocket kFirstSocket = 0x7000;
constexpr int kStreamCount = 4;
constexpr int kBufferSize = 4096;

enum class Role { None, YardMaster, Boneyard };

// One connection: what the server has sent and the client not yet read,
// enciphered, and the client's text not yet answered, deciphered.
struct Stream {
    bool open = false;
    Role role = Role::None;
    bool ended = false; // the server closed its side
    uint32_t to_client = OA_ROVER_CIPHER_FIRST_COUNTER;
    uint32_t from_client = OA_ROVER_CIPHER_FIRST_COUNTER;
    char unread[kBufferSize] = {};
    int unread_length = 0;
    char text[kBufferSize] = {};
    int text_length = 0;
};

struct Network {
    std::mutex mutex;
    std::condition_variable changed;
    Stream streams[kStreamCount];
};

Network g_net;
std::atomic<bool> g_yard_master_closed{false};

// What the link thread's waits returned, counted in the fake event_wait_any.
std::atomic<int> g_link_wakes{0};
std::atomic<RoverEvent*> g_last_wake{nullptr};

// The first time the server saw each request: the wake count and the event
// of the wake that sent it.
struct Sighting {
    const char* command;
    std::atomic<bool> seen;
    int wakes;
    RoverEvent* woken_by;
};

Sighting g_sightings[] = {
    {"<S:command=get_profile>", false, 0, nullptr},
    {"<S:command=get_allegiance>", false, 0, nullptr},
    {"<S:command=get_census>", false, 0, nullptr},
    {"<S:command=logout>", false, 0, nullptr},
};

Sighting& sighting(const char* command) {
    for (Sighting& candidate : g_sightings) {
        if (strcmp(candidate.command, command) == 0)
            return candidate;
    }
    return g_sightings[0];
}

const RoverPlatform* g_native = nullptr;
RoverPlatform g_fake;

Stream* stream_for(RoverSocket socket) {
    int32_t index = static_cast<int32_t>(socket - kFirstSocket);
    if (index < 0 || index >= kStreamCount || !g_net.streams[index].open)
        return nullptr;
    return &g_net.streams[index];
}

/// Enciphers or deciphers a stream's bytes in place, as the link does.
///
/// @param[in,out] counter the stream direction's cipher counter; advances by length
/// @param[in,out] bytes bytes transformed in place
/// @param length number of bytes
void transform(uint32_t& counter, char* bytes, int length) {
    rover_cipher_apply(&counter, reinterpret_cast<uint8_t*>(bytes), length);
}

// Queues a server record for the client. Network lock held.
void reply(Stream& stream, const char* record) {
    int length = static_cast<int>(strlen(record));
    if (stream.unread_length + length > kBufferSize)
        return;
    char* at = stream.unread + stream.unread_length;
    memcpy(at, record, static_cast<size_t>(length));
    transform(stream.to_client, at, length);
    stream.unread_length += length;
    g_net.changed.notify_all();
}

// Answers one complete client record as the yard master or the boneyard.
// Network lock held; runs on the link thread, inside its send.
void answer(Stream& stream, const char* record) {
    for (Sighting& candidate : g_sightings) {
        if (strstr(record, candidate.command) && !candidate.seen) {
            candidate.wakes = g_link_wakes;
            candidate.woken_by = g_last_wake;
            candidate.seen = true;
        }
    }
    if (stream.role == Role::YardMaster) {
        if (strstr(record, "<S:command=get_servers>"))
            reply(
                stream,
                "{[Rover<S:command=server_list><S:cluster_name=127.0.0.1><S:domain= >"
                "<I:cluster_size=0><S:name=Scripted Server><I:last=1>]}"
            );
        else if (strstr(record, "<S:command=connect>"))
            reply(
                stream, "{[Rover<S:command=connect_to><S:ip=127.0.0.1><I:port=9111><I:result=0>]}"
            );
        return;
    }
    if (strstr(record, "<S:command=connect_type>"))
        reply(
            stream,
            "{[Rover<S:command=connect_info><S:seed=scripted><S:type=server>"
            "<S:compression=text>]}"
        );
    else if (strstr(record, "<S:command=login>"))
        reply(
            stream, "{[Rover<S:command=login_confirmed><U:user_id=2a><U:area_id=1><I:result=0>]}"
        );
    else if (strstr(record, "<S:command=get_profile>"))
        reply(stream, "{[Rover<S:command=profile_info><U:user_id=2a><S:name=tester><I:last=1>]}");
    else if (strstr(record, "<S:command=get_allegiance>"))
        reply(stream, "{[Rover<S:command=allegiances><S:array=CCAAI>]}");
    else if (strstr(record, "<S:command=logout>"))
        reply(stream, "{[Rover<S:command=logout_confirmed>]}");
}

RoverSocket fake_create() {
    std::lock_guard<std::mutex> hold(g_net.mutex);
    for (int i = 0; i < kStreamCount; ++i) {
        if (!g_net.streams[i].open) {
            g_net.streams[i] = Stream{};
            g_net.streams[i].open = true;
            return kFirstSocket + i;
        }
    }
    return OA_ROVER_SOCKET_INVALID;
}

bool fake_connect_start(RoverSocket socket, uint32_t, uint16_t port) {
    std::lock_guard<std::mutex> hold(g_net.mutex);
    Stream* stream = stream_for(socket);
    if (!stream)
        return false;
    stream->role = port == kYardMasterPort ? Role::YardMaster
                   : port == kBoneyardPort ? Role::Boneyard
                                           : Role::None;
    g_net.changed.notify_all();
    return stream->role != Role::None;
}

// Winsock's select over the streams: a connected stream is always writable,
// readable while the server has unread bytes or has closed, never failed.
int32_t fake_select(
    const RoverSocket* sockets,
    const uint8_t* interest,
    uint8_t* ready,
    int32_t count,
    uint32_t timeout_ms
) {
    std::unique_lock<std::mutex> hold(g_net.mutex);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int32_t found = 0;
        for (int32_t i = 0; i < count; ++i) {
            ready[i] = 0;
            const Stream* stream = stream_for(sockets[i]);
            if (!stream || stream->role == Role::None)
                continue;
            if (interest[i] & OA_ROVER_SOCKET_WRITABLE)
                ready[i] |= OA_ROVER_SOCKET_WRITABLE;
            if ((interest[i] & OA_ROVER_SOCKET_READABLE) &&
                (stream->unread_length > 0 || stream->ended))
                ready[i] |= OA_ROVER_SOCKET_READABLE;
            if (ready[i])
                ++found;
        }
        if (found)
            return found;
        if (g_net.changed.wait_until(hold, deadline) == std::cv_status::timeout)
            return 0;
    }
}

int32_t fake_recv(RoverSocket socket, void* buffer, int32_t capacity) {
    std::lock_guard<std::mutex> hold(g_net.mutex);
    Stream* stream = stream_for(socket);
    if (!stream)
        return -1;
    if (stream->unread_length == 0)
        return stream->ended ? 0 : OA_ROVER_IO_WOULD_BLOCK;
    int32_t length = stream->unread_length < capacity ? stream->unread_length : capacity;
    memcpy(buffer, stream->unread, static_cast<size_t>(length));
    memmove(
        stream->unread, stream->unread + length, static_cast<size_t>(stream->unread_length - length)
    );
    stream->unread_length -= length;
    return length;
}

int32_t fake_send(RoverSocket socket, const void* data, int32_t length) {
    std::lock_guard<std::mutex> hold(g_net.mutex);
    Stream* stream = stream_for(socket);
    if (!stream || stream->text_length + length >= kBufferSize)
        return -1;
    char* at = stream->text + stream->text_length;
    memcpy(at, data, static_cast<size_t>(length));
    transform(stream->from_client, at, length);
    stream->text_length += length;
    while (char* end = static_cast<char*>(memchr(stream->text, '}', stream->text_length))) {
        int record_length = static_cast<int>(end - stream->text) + 1;
        char record[kBufferSize];
        memcpy(record, stream->text, static_cast<size_t>(record_length));
        record[record_length] = '\0';
        memmove(
            stream->text,
            stream->text + record_length,
            static_cast<size_t>(stream->text_length - record_length)
        );
        stream->text_length -= record_length;
        answer(*stream, record);
    }
    return length;
}

// The server answers the client's half close by closing too.
void fake_shutdown_send(RoverSocket socket) {
    std::lock_guard<std::mutex> hold(g_net.mutex);
    if (Stream* stream = stream_for(socket)) {
        stream->ended = true;
        g_net.changed.notify_all();
    }
}

void fake_close(RoverSocket socket) {
    std::lock_guard<std::mutex> hold(g_net.mutex);
    if (Stream* stream = stream_for(socket)) {
        if (stream->role == Role::YardMaster)
            g_yard_master_closed = true;
        stream->open = false;
        g_net.changed.notify_all();
    }
}

int32_t fake_pending_error(RoverSocket) {
    return 0;
}

bool is_link_wait(RoverEvent* const* events, int32_t count) {
    if (!g_rover_state)
        return false;
    for (int32_t i = 0; i < count; ++i) {
        if (events[i] == g_rover_state->stop_event)
            return true;
    }
    return false;
}

int32_t counting_wait_any(RoverEvent* const* events, int32_t count, uint32_t timeout_ms) {
    int32_t index = g_native->event_wait_any(events, count, timeout_ms);
    if (index >= 0 && is_link_wait(events, count)) {
        g_last_wake = events[index];
        ++g_link_wakes;
    }
    return index;
}

// ---- the game's side ------------------------------------------------------------

const RoverInterface* g_rover = nullptr;
std::atomic<int32_t> g_stage{0};
std::atomic<bool> g_login_failed{false};
std::atomic<int> g_profiles{0};
std::atomic<int> g_allegiances{0};
std::atomic<int> g_logouts{0};

void ROVER_CALL on_progress(int32_t stage, int32_t result, int32_t, int32_t, const char*) {
    g_stage = stage;
    if (result != 0)
        g_login_failed = true;
}

void ROVER_CALL on_alert(int32_t, const char*, void*) {
}

void ROVER_CALL on_disconnect(int32_t, char, int32_t) {
    g_login_failed = true;
}

void ROVER_CALL on_profile(bool ok, RoverPropertyBag*) {
    if (ok)
        ++g_profiles;
}

void ROVER_CALL on_allegiances(const char*) {
    ++g_allegiances;
}

void ROVER_CALL on_census(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {
}

void ROVER_CALL on_activity(const char*) {
}

void ROVER_CALL on_logged_out() {
    ++g_logouts;
}

// Pumps slot 5 until done() holds or the wait runs out.
bool pump_until(bool (*done)()) {
    for (int waited = 0; waited < kWaitLimitMs; ++waited) {
        g_rover->pump_events();
        if (done())
            return true;
        g_native->sleep_ms(1);
    }
    return done();
}

// Waits, without pumping, until the link thread has slept for kIdleMs.
bool wait_until_idle() {
    int wakes = g_link_wakes;
    int quiet = 0;
    for (int waited = 0; waited < kWaitLimitMs; ++waited) {
        g_native->sleep_ms(1);
        if (g_link_wakes != wakes) {
            wakes = g_link_wakes;
            quiet = 0;
        } else if (++quiet >= kIdleMs) {
            return true;
        }
    }
    return false;
}

bool wait_until_seen(const char* command) {
    for (int waited = 0; waited < kWaitLimitMs && !sighting(command).seen; ++waited)
        g_native->sleep_ms(1);
    return sighting(command).seen;
}

// Checks that command went out on the first link wake-up counted after
// wakes_before, and that the boneyard queue's event caused that wake-up.
void check_sent_on_first_wake(const char* command, int wakes_before, const char* what) {
    const Sighting& seen = sighting(command);
    char label[160];
    snprintf(label, sizeof label, "%s reaches the socket", what);
    check(seen.seen, label);
    snprintf(label, sizeof label, "%s is sent on the link thread's first wake-up", what);
    check(seen.wakes == wakes_before + 1, label);
    snprintf(label, sizeof label, "%s: the boneyard queue's event woke the link", what);
    RoverEvent* queue_event =
        g_rover_state->outbound[static_cast<int32_t>(RoverLinkId::Boneyard1)].ready;
    check(seen.woken_by == queue_event, label);
}

void run_session() {
    check(g_rover->activate(), "activate");
    check(
        g_rover->start_login(
            kServerLocation, "tester", "secret", on_progress, on_alert, on_disconnect
        ),
        "start_login"
    );
    check(
        pump_until([] { return g_stage == kLoginConfirmedStage || g_login_failed; }) &&
            !g_login_failed,
        "the login runs through the yard master and the boneyard"
    );
    check(
        pump_until([] { return g_yard_master_closed.load(); }),
        "the yard master link closes after the redirect"
    );
    check(wait_until_idle(), "the link goes idle after the login");

    // What the game asks for first: the dossier.
    int wakes = g_link_wakes;
    g_rover->request_get_profile(kUserId, on_profile, nullptr);
    wait_until_seen("<S:command=get_profile>");
    check_sent_on_first_wake("<S:command=get_profile>", wakes, "get_profile");
    check(pump_until([] { return g_profiles > 0; }), "the dossier arrives");

    // The galaxy screen's two requests, queued together: the first goes out
    // on the first wake-up; the queue stays signalled for the second.
    check(wait_until_idle(), "the link goes idle after the dossier");
    wakes = g_link_wakes;
    g_rover->request_allegiance(on_allegiances);
    g_rover->request_census(0, on_census, on_activity);
    wait_until_seen("<S:command=get_census>");
    check_sent_on_first_wake("<S:command=get_allegiance>", wakes, "get_allegiance");
    check(sighting("<S:command=get_census>").seen, "get_census reaches the socket");
    check(pump_until([] { return g_allegiances > 0; }), "the allegiances arrive");

    check(wait_until_idle(), "the link goes idle before the logout");
    wakes = g_link_wakes;
    g_rover->send_logout(static_cast<int32_t>(kUserId), on_logged_out);
    wait_until_seen("<S:command=logout>");
    check_sent_on_first_wake("<S:command=logout>", wakes, "logout");
    check(pump_until([] { return g_logouts > 0; }), "the logout is confirmed");
    g_rover->deactivate();
}

} // namespace

int main() {
    const auto temporary_value = oa::platform::environment_value("TMPDIR");
    const char* temporary = temporary_value ? temporary_value->c_str() : nullptr;
    char directory[512];
    snprintf(
        directory,
        sizeof directory,
        "%s/rover-link-test-%d",
        temporary && *temporary ? temporary : ".",
        static_cast<int>(process_id())
    );
    make_directory(directory);
    if (change_directory(directory) != 0) {
        fprintf(stderr, "cannot prepare %s\n", directory);
        return 1;
    }
    make_directory("boneyards");
    FILE* servers = oa::platform::open_file("boneyards/serv_sav.byd", "wb");
    if (!servers) {
        fprintf(stderr, "cannot write boneyards/serv_sav.byd\n");
        return 1;
    }
    fprintf(servers, "127.0.0.1\t \t0\t%s\tD\r\n", kServerLocation);
    fclose(servers);

    g_native = rover_native_platform();
    g_fake = *g_native;
    g_fake.tcp_create = fake_create;
    g_fake.tcp_connect_start = fake_connect_start;
    g_fake.tcp_select = fake_select;
    g_fake.tcp_recv = fake_recv;
    g_fake.tcp_send = fake_send;
    g_fake.tcp_shutdown_send = fake_shutdown_send;
    g_fake.tcp_close = fake_close;
    g_fake.tcp_pending_error = fake_pending_error;
    g_fake.event_wait_any = counting_wait_any;
    rover_use_platform(&g_fake);

    rover_get_interface(&g_rover);
    check(g_rover->attach(), "attach");
    if (g_rover_state) {
        run_session();
        g_rover->detach();
    }
    rover_use_platform(nullptr);

    remove("http.log");
    remove("boneyards/serv_sav.byd");
    remove_directory("boneyards");
    if (change_directory("..") == 0)
        remove_directory(directory);
    if (failures)
        return 1;
    puts("rover link: ok");
    return 0;
}
