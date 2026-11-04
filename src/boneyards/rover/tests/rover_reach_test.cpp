// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A login whose yard master cannot be reached: an unresolvable name (.invalid, which is never
// looked up beyond the resolver's own refusal) and a connect refused on a loopback port. Neither
// case opens a connection to a real host. The refused case needs nothing listening on the yard
// master's port: a port bound without listening is not enough, as macOS drops a connect to it
// instead of refusing it, so the port is left closed and the case is skipped when it is open.

#include "../src/protocol.h"
#include "../src/state.h"
#include "oa/platform/files.hpp"
#include "oa/platform/system.hpp"
#include "oa/rover/interface.h"
#include "oa/rover/platform.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <direct.h>
#include <process.h>
#define make_directory(path) _mkdir(path)
#define change_directory(path) _chdir(path)
#define remove_directory(path) _rmdir(path)
#define process_id() _getpid()
using ProbeSocket = SOCKET;
constexpr ProbeSocket kProbeInvalid = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#define make_directory(path) mkdir(path, 0755)
#define change_directory(path) chdir(path)
#define remove_directory(path) rmdir(path)
#define process_id() getpid()
using ProbeSocket = int;
constexpr ProbeSocket kProbeInvalid = -1;
#endif

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct Reach {
    // The resolve step. A later connect notice must not replace it.
    int32_t stage{};
    int32_t result{};
    char message[256]{};
    bool saw_resolve{};
    int disconnects{};
    int32_t which{};
    char connecting{};
};

Reach g_reach{};

void ROVER_CALL
on_progress(int32_t stage, int32_t result, int32_t /*a*/, int32_t /*b*/, const char* message) {
    if (g_reach.saw_resolve)
        return;
    g_reach.saw_resolve = true;
    g_reach.stage = stage;
    g_reach.result = result;
    snprintf(g_reach.message, sizeof g_reach.message, "%s", message != nullptr ? message : "");
}

void ROVER_CALL on_disconnect(int32_t which, char connecting, int32_t /*error*/) {
    ++g_reach.disconnects;
    g_reach.which = which;
    g_reach.connecting = connecting;
}

bool disconnected() {
    return g_reach.disconnects > 0;
}

bool pump_until(const RoverInterface* rover, uint32_t budget_ms) {
    const RoverPlatform* platform = rover_native_platform();
    const uint32_t start = platform->clock_ms();
    while (platform->clock_ms() - start < budget_ms) {
        rover->pump_events();
        if (disconnected())
            return true;
        platform->sleep_ms(10);
    }
    rover->pump_events();
    return disconnected();
}

void write_servers(const char* line) {
    FILE* file = oa::platform::open_file("boneyards/serv_sav.byd", "wb");
    if (file == nullptr) {
        check(false, "serv_sav.byd");
        return;
    }
    fputs(line, file);
    fclose(file);
}

// Whether something accepts connections on 127.0.0.1:port.
bool loopback_port_open(uint16_t port) {
    ProbeSocket probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (probe == kProbeInvalid)
        return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool open = connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0;
#if defined(_WIN32)
    closesocket(probe);
#else
    close(probe);
#endif
    return open;
}

void run_login(const RoverInterface* rover, const char* location) {
    g_reach = Reach{};
    check(rover->activate(), "activate");
    check(
        rover->start_login(location, "guest", nullptr, on_progress, nullptr, on_disconnect),
        "start_login"
    );
    check(pump_until(rover, 3000), "the connect failure is reported");
    rover->deactivate();
}

void check_unresolvable(const RoverInterface* rover) {
    write_servers("nope\tinvalid\t2\tUnreachable\tD\r\n");
    check(rover->attach(), "attach unresolvable");
    run_login(rover, "Unreachable");
    check(g_reach.stage == 1 && g_reach.result == 1, "an unresolvable host fails the resolve step");
    check(strcmp(g_reach.message, "nope1.invalid") == 0, "the resolve step names nope1.invalid");
    check(
        g_reach.which == 1 && g_reach.connecting != 0,
        "the unresolvable host is reported as a yard master still connecting"
    );
    rover->detach();
}

void check_refused(const RoverInterface* rover) {
    if (loopback_port_open(kRoverYardMasterPort)) {
        puts(
            "rover reach: 127.0.0.1:9110 accepts connections here, so the refused case is skipped"
        );
        return;
    }
    write_servers("127.0.0.1\t \t0\tLoopback\tD\r\n");
    check(rover->attach(), "attach refused");
    run_login(rover, "Loopback");
    check(g_reach.stage == 1 && g_reach.result == 0, "a numeric loopback host resolves");
    check(strcmp(g_reach.message, "127.0.0.1") == 0, "the resolve step names 127.0.0.1");
    check(
        g_reach.which == 1 && g_reach.connecting != 0,
        "a refused loopback connect is reported as a yard master still connecting"
    );
    rover->detach();
}

} // namespace

int main() {
    const auto temporary_value = oa::platform::environment_value("TMPDIR");
    const char* temporary = temporary_value ? temporary_value->c_str() : nullptr;
    char directory[512];
    snprintf(
        directory,
        sizeof directory,
        "%s/rover-reach-test-%d",
        temporary && *temporary ? temporary : ".",
        static_cast<int>(process_id())
    );
    make_directory(directory);
    if (change_directory(directory) != 0) {
        fprintf(stderr, "cannot prepare %s\n", directory);
        return 1;
    }
    make_directory("boneyards");

    const RoverInterface* rover = nullptr;
    rover_get_interface(&rover);
    check_unresolvable(rover);
    check_refused(rover);

    remove("boneyards/serv_sav.byd");
    remove_directory("boneyards");
    if (change_directory("..") == 0)
        remove_directory(directory);
    if (failures)
        return 1;
    puts("rover reach: ok");
    return 0;
}
