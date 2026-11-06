// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The interface table: the version handshake truth table and slots 70-72
// over the server lists the CorePrime and BY1117a installs ship.

#include "../src/state.h"
#include "oa/rover/interface.h"
#include "oa/platform/system.hpp"
#include "oa/platform/files.hpp"

#include <stddef.h>
#include <stdint.h>
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

// 3.1c's handshake cases: exact floor, one below, zero, wrong
// version, largest signed stamp, and two stamps that are only large unsigned.
void check_version_truth_table(const RoverInterface* table) {
    struct Case {
        uint32_t iface_version;
        uint32_t build_stamp;
        int32_t expected;
    };

    const Case cases[] = {
        {0xe, 19991103u, 1},
        {0xe, 19991102u, 0},
        {0xe, 0u, 0},
        {0xd, 0x7fffffffu, 0},
        {0xe, 0x7fffffffu, 1},
        {0xe, 0x80000000u, 0},
        {0xe, 0xffffffffu, 0},
    };
    for (const Case& c : cases) {
        char what[96];
        snprintf(what, sizeof what, "check_version(0x%x, 0x%x)", c.iface_version, c.build_stamp);
        int32_t result = table->check_version(
            static_cast<int32_t>(c.iface_version), static_cast<int32_t>(c.build_stamp)
        );
        check(result == c.expected, what);
    }
}

// Boneyards/serv_sav.byd of the CorePrime install.
constexpr char kCorePrimeSavedList[] =
    "alpha\tclusters.coreprime.net\t3\tCorePrime Alpha Test\tD\r\n";
// Boneyards/server.byd of the BY1117a install.
constexpr char kShippedList[] = "bypacific\tboneyards.net\t2\tUSA, West Coast\t\r\n\r\n";
// Two servers at one location, only the second selected, then another.
constexpr char kSharedLocationList[] =
    "alpha\tclusters.coreprime.net\t3\tCorePrime Alpha Test\t\r\n"
    "beta\tclusters.coreprime.net\t1\tCorePrime Alpha Test\tD\r\n"
    "gamma\tclusters.coreprime.net\t1\tElsewhere\tD\r\n";

constexpr char kSavedList[] = "boneyards/serv_sav.byd";
constexpr char kShippedListFile[] = "boneyards/server.byd";

constexpr int32_t kLocationCapacity = 8;
const char* g_locations[kLocationCapacity];
int32_t g_location_count = 0;

void ROVER_CALL remember_location(const char* location) {
    if (g_location_count < kLocationCapacity)
        g_locations[g_location_count] = location;
    ++g_location_count;
}

int32_t enumerate(const RoverInterface* table) {
    g_location_count = 0;
    table->enumerate_areas(remember_location);
    return g_location_count;
}

void write_file(const char* path, const char* text) {
    FILE* file = oa::platform::open_file(path, "wb");
    fputs(text, file);
    fclose(file);
}

void check_detached_server_slots(const RoverInterface* table) {
    check(enumerate(table) == 0, "detached enumerate_areas calls nothing");
    const char* location = "unset";
    table->find_current_area(&location);
    check(location == nullptr, "detached find_current_area stores null");
    check(table->match_area_name() == nullptr, "detached match_area_name");
}

void check_saved_server_slots(const RoverInterface* table) {
    write_file(kSavedList, kCorePrimeSavedList);
    check(table->attach(), "attach with serv_sav.byd");
    if (!g_rover_state)
        return;
    check(
        enumerate(table) == 1 && strcmp(g_locations[0], "CorePrime Alpha Test") == 0,
        "enumerate_areas walks the saved list's one location"
    );
    table->enumerate_areas(nullptr);
    const char* location = nullptr;
    table->find_current_area(&location);
    check(
        location && strcmp(location, "CorePrime Alpha Test") == 0,
        "find_current_area names the 'D' server"
    );
    check(table->match_area_name() == nullptr, "match_area_name before a yard master answers");
    rover_copy_bounded(
        g_rover_state->resolved_server,
        "CorePrime Alpha Test",
        sizeof g_rover_state->resolved_server
    );
    const char* name = table->match_area_name();
    check(name && strcmp(name, "alpha") == 0, "match_area_name reads the entry's name");
    table->detach();

    write_file(kSavedList, kSharedLocationList);
    check(table->attach(), "attach with a shared location");
    if (!g_rover_state)
        return;
    const RoverServerEntry* entries = g_rover_state->servers.entries;
    check(
        enumerate(table) == 3 && g_locations[0] == entries[0].location &&
            g_locations[1] == entries[1].location && g_locations[2] == entries[2].location,
        "enumerate_areas walks the list in order"
    );
    table->find_current_area(&location);
    check(location == entries[1].location, "find_current_area takes the first selected");
    rover_copy_bounded(
        g_rover_state->resolved_server,
        "CorePrime Alpha Test",
        sizeof g_rover_state->resolved_server
    );
    name = table->match_area_name();
    check(name == entries[1].name, "match_area_name takes the last at the location");
    table->detach();
    remove(kSavedList);
}

void check_shipped_server_slots(const RoverInterface* table) {
    write_file(kShippedListFile, kShippedList);
    check(table->attach(), "attach with server.byd");
    if (!g_rover_state)
        return;
    check(
        enumerate(table) == 1 && strcmp(g_locations[0], "USA, West Coast") == 0,
        "enumerate_areas walks the shipped list"
    );
    const char* location = "unset";
    table->find_current_area(&location);
    check(location == nullptr, "the shipped list selects no server");
    table->detach();
    remove(kShippedListFile);
}

// Attaching reads ./boneyards, so the lists are written under a scratch
// working directory.
void check_server_slots(const RoverInterface* table) {
    check_detached_server_slots(table);
    const auto temporary_value = oa::platform::environment_value("TMPDIR");
    const char* temporary = temporary_value ? temporary_value->c_str() : nullptr;
    char directory[512];
    snprintf(
        directory,
        sizeof directory,
        "%s/rover-interface-test-%d",
        temporary && *temporary ? temporary : ".",
        static_cast<int>(process_id())
    );
    make_directory(directory);
    if (change_directory(directory) != 0) {
        check(false, "prepare the server list directory");
        return;
    }
    make_directory("boneyards");
    check_saved_server_slots(table);
    check_shipped_server_slots(table);
    check_detached_server_slots(table);
    remove_directory("boneyards");
    if (change_directory("..") == 0)
        remove_directory(directory);
}

} // namespace

int main() {
    const RoverInterface* table = nullptr;
    rover_get_interface(&table);
    check(table != nullptr, "rover_get_interface");
    if (!table)
        return 1;
    const RoverInterface* again = nullptr;
    rover_get_interface(&again);
    check(again == table, "one table per process");

    check_version_truth_table(table);
    check_server_slots(table);

    if (failures)
        return 1;
    puts("rover interface: ok");
    return 0;
}
