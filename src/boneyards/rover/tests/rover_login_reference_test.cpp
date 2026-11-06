// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Logs in through the interface table against the bycomms reference server
// on loopback: attach, activate, start_login, pump until login_confirmed,
// ask for the dossier and run the galaxy screen's bootstrap (allegiances,
// census) with the home area's room and battle lists and two downloads from
// the server's HTTP side, logout, deactivate, detach. The requests must
// reach the server, each written promptly after the game queues it; its
// answers are reported, since the reference server does not implement the
// lobby queries.
//
// The client dials the yard master on port 9110; the test's platform table
// sends that connect to the port bycomms listens on. bycomms speaks the
// Kingdoms dialect in three places the TA client does not accept, so the
// same table rewrites what bycomms sends before the client reads it:
//   - server_list carries last=0, and the client asks for a boneyard only
//     after a list entry marked last; it becomes last=1;
//   - the yard master answers every request with server_list and connect_to,
//     where the client expects the list for get_servers and connect_to for
//     its "connect"; only those two are passed on;
//   - connect_info offers compression "none", which the client ignores
//     (text, compressed or encrypted only); it becomes "text".
//
// Usage: oa-rover-login-reference-test BYCOMMS_BINARY. Exits 77 when the
// binary is missing, which ctest reports as skipped only if the binary was
// also missing when the build was configured.

#include "../src/md5.h"
#include "oa/rover/cipher.h"
#include "oa/rover/interface.h"
#include "oa/rover/platform.h"
#include "oa/platform/system.hpp"
#include "oa/platform/files.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>

extern char** environ;

namespace {

constexpr int kSkipped = 77;
constexpr uint16_t kYardMasterPort = 9110;
constexpr int kPumpTimeoutMs = 15000;
constexpr int kPumpStepMs = 10;
// The reference server probes the latency this often once the login is done.
constexpr char kProbeInterval[] = "100ms";
constexpr uint32_t kProbeIntervalMs = 100;
constexpr char kServerLocation[] = "Reference Server";
constexpr char kUsername[] = "test";
constexpr char kPassword[] = "password";
// The seed the reference server sends in connect_info.
constexpr char kReferenceSeed[] = "coreprime";

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// ---- what the game callbacks saw -------------------------------------------

constexpr int kMaxReports = 64;

struct Progress {
    int32_t stage;
    int32_t result;
    int32_t a;
    int32_t b;
};

Progress g_reports[kMaxReports];
int g_report_count = 0;
bool g_logged_in = false;
bool g_login_failed = false;
int g_disconnects = 0;
int g_alerts = 0;

void ROVER_CALL
on_progress(int32_t stage, int32_t result, int32_t a, int32_t b, const char* message) {
    fprintf(
        stderr,
        "progress stage=%d result=%d a=%d b=%d message=%s\n",
        stage,
        result,
        a,
        b,
        message ? message : "(none)"
    );
    if (g_report_count < kMaxReports)
        g_reports[g_report_count++] = Progress{stage, result, a, b};
    if (stage == 8)
        (result == 0 ? g_logged_in : g_login_failed) = true;
    else if (result != 0)
        g_login_failed = true;
}

void ROVER_CALL on_alert(int32_t type, const char* message, void*) {
    fprintf(stderr, "system_alert type=%d message=%s\n", type, message);
    ++g_alerts;
}

void ROVER_CALL on_disconnect(int32_t which, char connecting, int32_t error) {
    fprintf(stderr, "disconnect which=%d connecting=%d error=%d\n", which, connecting, error);
    ++g_disconnects;
}

void ROVER_CALL on_logged_out() {
}

// ---- what the lobby callbacks saw ---------------------------------------------

int g_profile_replies = 0;
int g_allegiance_replies = 0;
int g_census_replies = 0;
int g_activity_replies = 0;
int g_room_rows = 0;
int g_battles_entered = 0;
int g_battle_rows = 0;
int g_fetches = 0;
int32_t g_fetch_status[2] = {-1, -1};

void ROVER_CALL on_profile(bool ok, RoverPropertyBag*) {
    fprintf(stderr, "profile ok=%d\n", ok);
    ++g_profile_replies;
}

void ROVER_CALL on_allegiances(const char* array) {
    fprintf(stderr, "allegiances %s\n", array);
    ++g_allegiance_replies;
}

void ROVER_CALL
on_census(uint32_t area, int32_t population, int32_t activity, int32_t, int32_t, int32_t, int32_t) {
    fprintf(stderr, "census area=%u pop=%d activity=%d\n", area, population, activity);
    ++g_census_replies;
}

void ROVER_CALL on_activity(const char*) {
    ++g_activity_replies;
}

void ROVER_CALL on_room_row(
    int32_t, int32_t, int32_t, const char*, const char*, bool, int32_t, int32_t, const char*, bool
) {
    ++g_room_rows;
}

void ROVER_CALL on_battles_entered(bool) {
    ++g_battles_entered;
}

void ROVER_CALL on_battle_row(int32_t, const char*, const char*, int32_t, int32_t, int32_t) {
    ++g_battle_rows;
}

void ROVER_CALL on_intel(int32_t, const char*, const char*) {
}

bool ROVER_CALL on_fetched(const char* url, const char* file, int32_t status) {
    fprintf(stderr, "fetched %s into %s, status %d\n", url, file, status);
    if (g_fetches < 2)
        g_fetch_status[g_fetches] = status;
    ++g_fetches;
    return true;
}

// ---- the platform table with the reference-server adjustments ---------------

enum class StreamRole { None, YardMaster, Boneyard };

// One connection seen through the table: what the server sent, deciphered
// and adjusted, waiting for the client; and what the client sent, deciphered.
struct Stream {
    RoverSocket socket = OA_ROVER_SOCKET_INVALID;
    StreamRole role = StreamRole::None;
    uint32_t server_counter =
        OA_ROVER_CIPHER_FIRST_COUNTER; // position in the server's cipher stream
    uint32_t client_counter =
        OA_ROVER_CIPHER_FIRST_COUNTER; // position in the stream the client reads
    uint32_t sent_counter =
        OA_ROVER_CIPHER_FIRST_COUNTER; // position in the client's own sending stream
    char partial[2048] = {};           // a server record still missing its '}'
    int32_t partial_length = 0;
    char ready[8192] = {}; // adjusted records for the client
    int32_t ready_length = 0;
    bool passed_server_list = false;
    bool passed_connect_to = false;
    char sent[8192] = {}; // the client's text
    int32_t sent_length = 0;
};

constexpr int kMaxStreams = 8;
Stream g_streams[kMaxStreams];
const RoverPlatform* g_native = nullptr;
uint16_t g_yard_port = 0;
uint16_t g_boneyard_port = 0;
char g_client_text[4][8192];
int g_client_text_count = 0;

Stream* stream_for(RoverSocket socket) {
    for (Stream& stream : g_streams) {
        if (stream.socket == socket && socket != OA_ROVER_SOCKET_INVALID)
            return &stream;
    }
    return nullptr;
}

/// Enciphers or deciphers a stream's bytes in place, as the link does.
///
/// @param[in,out] counter the stream direction's cipher counter; advances by length
/// @param[in,out] bytes bytes transformed in place
/// @param length number of bytes
void cipher(uint32_t& counter, char* bytes, int32_t length) {
    rover_cipher_apply(&counter, reinterpret_cast<uint8_t*>(bytes), length);
}

// Replaces one same-length text in a record.
void replace(char* record, int32_t length, const char* from, const char* to) {
    size_t size = strlen(from);
    for (int32_t at = 0; at + static_cast<int32_t>(size) <= length; ++at) {
        if (memcmp(record + at, from, size) == 0) {
            memcpy(record + at, to, size);
            return;
        }
    }
}

bool contains(const char* record, int32_t length, const char* text) {
    size_t size = strlen(text);
    for (int32_t at = 0; at + static_cast<int32_t>(size) <= length; ++at) {
        if (memcmp(record + at, text, size) == 0)
            return true;
    }
    return false;
}

// Adjusts one complete server record and queues it for the client, or drops it.
void pass_record(Stream& stream, char* record, int32_t length) {
    if (stream.role == StreamRole::YardMaster) {
        if (contains(record, length, "command=server_list>")) {
            if (stream.passed_server_list)
                return;
            stream.passed_server_list = true;
            replace(record, length, "<I:last=0>", "<I:last=1>");
        } else if (contains(record, length, "command=connect_to>")) {
            bool asked =
                contains(stream.sent, stream.sent_length, "{[YardMaster<S:command=connect>]}");
            if (!asked || stream.passed_connect_to)
                return;
            stream.passed_connect_to = true;
        }
    } else if (stream.role == StreamRole::Boneyard) {
        replace(record, length, "<S:compression=none>", "<S:compression=text>");
    }
    if (stream.ready_length + length <= static_cast<int32_t>(sizeof stream.ready)) {
        memcpy(stream.ready + stream.ready_length, record, static_cast<size_t>(length));
        stream.ready_length += length;
    }
}

bool test_connect_start(RoverSocket socket, uint32_t address, uint16_t port) {
    StreamRole role = StreamRole::None;
    if (port == kYardMasterPort) {
        port = g_yard_port;
        role = StreamRole::YardMaster;
    } else if (port == g_boneyard_port) {
        role = StreamRole::Boneyard;
    }
    for (Stream& stream : g_streams) {
        if (stream.socket == OA_ROVER_SOCKET_INVALID) {
            stream = Stream{};
            stream.socket = socket;
            stream.role = role;
            break;
        }
    }
    return g_native->tcp_connect_start(socket, address, port);
}

int32_t test_recv(RoverSocket socket, void* buffer, int32_t capacity) {
    Stream* stream = stream_for(socket);
    if (!stream)
        return g_native->tcp_recv(socket, buffer, capacity);
    char raw[1024];
    int32_t received = g_native->tcp_recv(socket, raw, sizeof raw);
    if (received > 0) {
        cipher(stream->server_counter, raw, received);
        for (int32_t i = 0; i < received; ++i) {
            if (stream->partial_length < static_cast<int32_t>(sizeof stream->partial))
                stream->partial[stream->partial_length++] = raw[i];
            if (raw[i] == '}') {
                pass_record(*stream, stream->partial, stream->partial_length);
                stream->partial_length = 0;
            }
        }
    }
    if (stream->ready_length > 0) {
        int32_t out = stream->ready_length < capacity ? stream->ready_length : capacity;
        memcpy(buffer, stream->ready, static_cast<size_t>(out));
        memmove(
            stream->ready, stream->ready + out, static_cast<size_t>(stream->ready_length - out)
        );
        stream->ready_length -= out;
        cipher(stream->client_counter, static_cast<char*>(buffer), out);
        return out;
    }
    return received > 0 ? OA_ROVER_IO_WOULD_BLOCK : received;
}

// A request the test queues after the login: when it was queued and when
// its text reached the socket (on the link thread).
struct TimedRequest {
    const char* text;
    uint32_t queued_ms = 0;
    uint32_t written_ms = 0;
    std::atomic<bool> written{false};
};

TimedRequest g_timed[] = {
    {"<S:command=get_profile>"},
    {"<S:command=get_allegiance>"},
    {"<S:command=get_census>"},
    {"<S:command=get_rooms>"},
    {"<S:command=enter_battles>"},
};

void note_queued(const char* text) {
    for (TimedRequest& request : g_timed) {
        if (strcmp(request.text, text) == 0)
            request.queued_ms = g_native->clock_ms();
    }
}

int32_t test_send(RoverSocket socket, const void* data, int32_t length) {
    int32_t sent = g_native->tcp_send(socket, data, length);
    Stream* stream = stream_for(socket);
    if (stream && sent > 0 &&
        stream->sent_length + sent <= static_cast<int32_t>(sizeof stream->sent)) {
        char* text = stream->sent + stream->sent_length;
        memcpy(text, data, static_cast<size_t>(sent));
        cipher(stream->sent_counter, text, sent);
        stream->sent_length += sent;
        for (TimedRequest& request : g_timed) {
            if (!request.written && contains(stream->sent, stream->sent_length, request.text)) {
                request.written_ms = g_native->clock_ms();
                request.written = true;
            }
        }
    }
    return sent;
}

void test_close(RoverSocket socket) {
    if (Stream* stream = stream_for(socket)) {
        if (g_client_text_count < 4) {
            memcpy(
                g_client_text[g_client_text_count],
                stream->sent,
                static_cast<size_t>(stream->sent_length)
            );
            g_client_text[g_client_text_count][stream->sent_length] = '\0';
            ++g_client_text_count;
        }
        stream->socket = OA_ROVER_SOCKET_INVALID;
    }
    g_native->tcp_close(socket);
}

RoverPlatform g_test_platform;

// ---- the reference server ------------------------------------------------------

uint16_t free_port() {
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof address;
    bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof address);
    getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length);
    close(probe);
    return ntohs(address.sin_port);
}

// A free port the client's URL parser accepts: it reads ports as 16-bit
// signed numbers, so the operating system's ephemeral ports are out of reach.
uint16_t free_low_port() {
    for (uint16_t port = static_cast<uint16_t>(20000 + getpid() % 10000); port < 32000; ++port) {
        int probe = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        bool free = bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0;
        close(probe);
        if (free)
            return port;
    }
    return 0;
}

bool port_accepts(uint16_t port) {
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    bool accepted = connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0;
    close(probe);
    return accepted;
}

pid_t start_server(
    const char* binary, uint16_t yard, uint16_t boneyard, uint16_t http, const char* log_path
) {
    char yard_addr[32];
    char boneyard_addr[32];
    char http_addr[32];
    snprintf(yard_addr, sizeof yard_addr, "127.0.0.1:%u", yard);
    snprintf(boneyard_addr, sizeof boneyard_addr, "127.0.0.1:%u", boneyard);
    snprintf(http_addr, sizeof http_addr, "127.0.0.1:%u", http);
    const char* arguments[] = {
        binary,
        "boneyard",
        "--yard-addr",
        yard_addr,
        "--boneyard-addr",
        boneyard_addr,
        "--http-addr",
        http_addr,
        "--interval",
        kProbeInterval,
        nullptr
    };
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    pid_t pid = 0;
    int spawned =
        posix_spawn(&pid, binary, &actions, nullptr, const_cast<char* const*>(arguments), environ);
    posix_spawn_file_actions_destroy(&actions);
    return spawned == 0 ? pid : -1;
}

void stop_server(pid_t pid) {
    kill(pid, SIGINT);
    for (int waited = 0; waited < 300; ++waited) {
        if (waitpid(pid, nullptr, WNOHANG) == pid)
            return;
        usleep(10000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

bool file_contains(const char* path, const char* text) {
    FILE* file = oa::platform::open_file(path, "rb");
    if (!file)
        return false;
    static char content[1 << 20];
    size_t length = fread(content, 1, sizeof content - 1, file);
    fclose(file);
    content[length] = '\0';
    return strstr(content, text) != nullptr;
}

long file_size(const char* path) {
    FILE* file = oa::platform::open_file(path, "rb");
    if (!file)
        return -1;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fclose(file);
    return size;
}

// ---- checks ------------------------------------------------------------------

bool pump_until(const RoverInterface* rover, bool (*done)()) {
    for (int waited = 0; waited < kPumpTimeoutMs; waited += kPumpStepMs) {
        rover->pump_events();
        if (done())
            return true;
        usleep(kPumpStepMs * 1000);
    }
    return false;
}

bool login_finished() {
    return g_logged_in || g_login_failed;
}

// The digest field the client must send: MD5 of password, (lower-case) name
// and seed, zero bytes sent as 1, reserved bytes escaped.
void expected_password_field(char* out, size_t capacity) {
    char text[64];
    snprintf(text, sizeof text, "%s%s%s", kPassword, kUsername, kReferenceSeed);
    RoverMd5 md5;
    rover_md5_init(md5);
    rover_md5_update(md5, text, static_cast<uint32_t>(strlen(text)));
    uint8_t digest[kRoverMd5DigestSize];
    rover_md5_final(md5, digest);
    size_t used = static_cast<size_t>(snprintf(out, capacity, "<S:password="));
    for (uint8_t byte : digest) {
        char c = static_cast<char>(byte ? byte : 1);
        const char* reserved = strchr("{}[]<>:=%", c);
        if (c && reserved) {
            out[used++] = '%';
            out[used++] = static_cast<char>('0' + (reserved - "{}[]<>:=%"));
        } else {
            out[used++] = c;
        }
    }
    out[used++] = '>';
    out[used] = '\0';
}

bool client_sent(const char* text) {
    for (int i = 0; i < g_client_text_count; ++i) {
        if (strstr(g_client_text[i], text))
            return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    const auto binary_value = oa::platform::environment_value("OA_BYCOMMS_BINARY");
    const char* binary = argc > 1 ? argv[1] : (binary_value ? binary_value->c_str() : nullptr);
    if (!binary || access(binary, X_OK) != 0) {
        fprintf(stderr, "skipped: bycomms binary not found (%s)\n", binary ? binary : "unset");
        return kSkipped;
    }
    const auto temporary_value = oa::platform::environment_value("TMPDIR");
    const char* temporary = temporary_value ? temporary_value->c_str() : nullptr;
    char directory[512];
    snprintf(
        directory,
        sizeof directory,
        "%s/rover-login-XXXXXX",
        temporary && *temporary ? temporary : "/tmp"
    );
    if (!mkdtemp(directory) || chdir(directory) != 0 || mkdir("boneyards", 0755) != 0) {
        fprintf(stderr, "cannot prepare a working directory\n");
        return 1;
    }
    FILE* servers = oa::platform::open_file("boneyards/serv_sav.byd", "wb");
    fprintf(servers, "127.0.0.1\t \t0\t%s\tD\r\n", kServerLocation);
    fclose(servers);

    g_yard_port = free_port();
    g_boneyard_port = free_port();
    uint16_t http_port = free_low_port();
    pid_t server = start_server(binary, g_yard_port, g_boneyard_port, http_port, "bycomms.log");
    check(server > 0, "bycomms starts");
    bool listening = false;
    for (int waited = 0; server > 0 && waited < 500 && !listening; ++waited) {
        listening = port_accepts(g_yard_port) && port_accepts(g_boneyard_port);
        if (!listening)
            usleep(10000);
    }
    check(listening, "bycomms listens");

    g_native = rover_native_platform();
    g_test_platform = *g_native;
    g_test_platform.tcp_connect_start = test_connect_start;
    g_test_platform.tcp_recv = test_recv;
    g_test_platform.tcp_send = test_send;
    g_test_platform.tcp_close = test_close;
    rover_use_platform(&g_test_platform);

    const RoverInterface* rover = nullptr;
    rover_get_interface(&rover);
    if (listening && rover->attach()) {
        check(rover->activate(), "activate");
        check(
            rover->start_login(
                kServerLocation, kUsername, kPassword, on_progress, on_alert, on_disconnect
            ),
            "start_login"
        );
        check(
            !rover->start_login(
                kServerLocation, kUsername, kPassword, on_progress, on_alert, on_disconnect
            ),
            "a second start_login while one runs fails"
        );
        check(pump_until(rover, login_finished) && g_logged_in, "login confirmed");

        static const int32_t kExpectedStages[] = {1, 2, 3, 4, 5, 6, 7, 8};
        bool in_order = g_report_count == 8;
        for (int i = 0; in_order && i < 8; ++i)
            in_order = g_reports[i].stage == kExpectedStages[i] && g_reports[i].result == 0;
        check(in_order, "stages 1-8 reported in order, each successful");
        const Progress& last = g_reports[g_report_count > 0 ? g_report_count - 1 : 0];
        check(last.a == 0x2a && last.b == 1, "login_confirmed user and area ids");

        // A few latency probes arrive every 100 ms and are answered.
        for (int step = 0; step < 50; ++step) {
            rover->pump_events();
            usleep(kPumpStepMs * 1000);
        }
        // What the game does after login: the dossier, then the galaxy
        // screen's bootstrap: allegiances and the census of every area; then
        // the home area's rooms and battles, and two downloads from the HTTP
        // side, one of them missing.
        uint32_t home_area = static_cast<uint32_t>(last.b);
        note_queued("<S:command=get_profile>");
        rover->request_get_profile(static_cast<uint32_t>(last.a), on_profile, nullptr);
        note_queued("<S:command=get_allegiance>");
        rover->request_allegiance(on_allegiances);
        note_queued("<S:command=get_census>");
        rover->request_census(0, on_census, on_activity);
        note_queued("<S:command=get_rooms>");
        check(rover->get_rooms(static_cast<int32_t>(home_area), on_room_row), "get_rooms");
        note_queued("<S:command=enter_battles>");
        check(
            rover->enter_battles(home_area, on_battles_entered, on_battle_row, on_intel),
            "enter_battles"
        );
        char url[128];
        snprintf(url, sizeof url, "http://127.0.0.1:%u/updates/esk0.esk", http_port);
        check(rover->fetch_http_file(url, "esk0.esk", on_fetched, nullptr), "fetch_http_file");
        snprintf(url, sizeof url, "http://127.0.0.1:%u/updates/missing.txt", http_port);
        rover->fetch_http_file(url, "missing.txt", on_fetched, nullptr);
        pump_until(rover, [] { return g_fetches >= 2; });
        for (int step = 0; step < 50; ++step) {
            rover->pump_events();
            usleep(kPumpStepMs * 1000);
        }
        check(g_fetches == 2, "both downloads complete");
        check(
            g_fetch_status[0] == 0 && g_fetch_status[1] == 0,
            "a download reports status 0, even a missing one"
        );
        check(file_size("esk0.esk") > 0, "the served key is saved");
        check(file_size("missing.txt") == 0, "a 404 leaves an empty file");
        // The queue's own event wakes the link thread, so each request is
        // written at once, not when the next probe happens to wake it.
        for (const TimedRequest& request : g_timed) {
            uint32_t delay = request.written_ms - request.queued_ms;
            char what[128];
            snprintf(
                what,
                sizeof what,
                "%s written within %u ms of being queued",
                request.text,
                kProbeIntervalMs
            );
            check(request.written && delay < kProbeIntervalMs, what);
            if (request.written)
                fprintf(stderr, "%s written %u ms after it was queued\n", request.text, delay);
        }
        fprintf(
            stderr,
            "reference server answers: profile %d, allegiances %d, census %d, activity %d, "
            "room_list %d, battles_entered %d, battle_info %d\n",
            g_profile_replies,
            g_allegiance_replies,
            g_census_replies,
            g_activity_replies,
            g_room_rows,
            g_battles_entered,
            g_battle_rows
        );
        check(rover->send_logout(g_reports[g_report_count - 1].a, on_logged_out), "send_logout");
        for (int step = 0; step < 20; ++step) {
            rover->pump_events();
            usleep(kPumpStepMs * 1000);
        }
        check(g_disconnects == 0 && g_alerts == 0, "no disconnect or alert during the session");
        rover->deactivate();
        rover->detach();
    } else {
        check(false, "attach");
    }
    rover_use_platform(nullptr);
    if (server > 0)
        stop_server(server);

    check(
        client_sent("{[YardMaster<S:command=get_servers>]}"), "get_servers sent to the yard master"
    );
    check(client_sent("{[YardMaster<S:command=connect>]}"), "connect sent to the yard master");
    check(
        client_sent(
            "{[CommManager<S:command=connect_type><S:compression_type=text>"
            "<S:requested_type=client><S:version=0.01>]}"
        ),
        "connect_type sent to the boneyard"
    );
    char password_field[64];
    expected_password_field(password_field, sizeof password_field);
    char login[160];
    snprintf(
        login,
        sizeof login,
        "{[UserManager<S:command=login>%s<S:type=registered><S:username=%s>]}",
        password_field,
        kUsername
    );
    check(client_sent(login), "login carries the MD5 digest of password, lower-case name and seed");
    check(client_sent("<S:command=latency_r>"), "latency probes answered");
    check(client_sent("{[UserManager<S:command=get_profile><U:user_id=2a>]}"), "get_profile sent");
    check(client_sent("{[WarManager<S:command=get_allegiance>]}"), "get_allegiance sent");
    check(client_sent("{[WarManager<S:command=get_census>]}"), "get_census sent for every area");
    check(client_sent("{[ChatManager<U:area_id=1><S:command=get_rooms>]}"), "get_rooms sent");
    check(
        client_sent("{[GameManager<U:area_id=1><S:command=enter_battles>]}"), "enter_battles sent"
    );
    check(client_sent("{[UserManager<S:command=logout><U:user_id=2a>]}"), "logout sent");
    check(file_contains("bycomms.log", "login accepted"), "bycomms accepted the login");
    check(file_contains("bycomms.log", "latency response"), "bycomms saw the latency replies");
    check(file_contains("bycomms.log", "get_profile"), "bycomms received get_profile");
    check(file_contains("bycomms.log", "get_census"), "bycomms received get_census");
    check(file_contains("bycomms.log", "get_allegiance"), "bycomms received get_allegiance");
    check(file_contains("bycomms.log", "get_rooms"), "bycomms received get_rooms");
    check(file_contains("bycomms.log", "enter_battles"), "bycomms received enter_battles");
    char saved[128];
    snprintf(saved, sizeof saved, "Core Prime\t127.0.0.1\t0\t127.0.0.1:%u\t\r\n", g_boneyard_port);
    check(
        file_contains("boneyards/serv_sav.byd", saved),
        "the yard master's list replaces the saved one"
    );

    if (failures) {
        fprintf(stderr, "working directory kept: %s\n", directory);
        return 1;
    }
    unlink("bycomms.log");
    unlink("http.log");
    unlink("esk0.esk");
    unlink("missing.txt");
    unlink("boneyards/serv_sav.byd");
    rmdir("boneyards");
    if (chdir("/") == 0)
        rmdir(directory);
    puts("rover login against the reference server: ok");
    return 0;
}
