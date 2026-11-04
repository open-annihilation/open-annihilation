// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Logs in to a live Boneyards back end through the interface table, as the
// game would: the saved server list names the server, the client dials its
// yard master on port 9110 and follows it to a boneyard. Then it does what
// the game does next: asks for the dossier, without which the game aborts
// the login, and runs what the galaxy screen does on entry: asks for the
// planets' allegiances and every area's census, reads the sector map
// (boneyards/<server name>_map.gwm) with slots 34-36 and asks for each
// planet's census (a planet's id is the area the census names), printing
// what arrives. The dossier and the allegiances must arrive.
//
// Skipped (exit 77) unless all three are set:
//   OA_BY_LIVE_SERVER  a serv_sav.byd line (tab separated) or a file path
//   OA_BY_USER         the account name
//   OA_BY_PASSWORD     its password
// For example:
//   OA_BY_LIVE_SERVER=$'alpha\tclusters.coreprime.net\t3\tCorePrime Alpha Test\tD'
// The server listed with 'D' is used, else the first. The password is never
// printed. The game installs the sector map with its updates; give it with
//   OA_BY_MAP_URL   an http URL the map is downloaded from through slot 32
//   OA_BY_MAP_FILE  a local copy (e.g. the install's Boneyards/alpha_map.gwm)
// When the server announces a news file server, its news definition is
// downloaded too, as the game's news screen does.

#include "../src/state.h"
#include "oa/rover/interface.h"
#include "oa/platform/system.hpp"
#include "oa/platform/files.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#include <windows.h>
#define make_directory(path) _mkdir(path)
#define change_directory(path) _chdir(path)
#define remove_directory(path) _rmdir(path)
#define process_id() _getpid()
#define sleep_milliseconds(ms) Sleep(ms)
#else
#include <sys/stat.h>
#include <unistd.h>
#define make_directory(path) mkdir(path, 0755)
#define change_directory(path) chdir(path)
#define remove_directory(path) rmdir(path)
#define process_id() getpid()
#define sleep_milliseconds(ms) usleep((ms) * 1000)
#endif

namespace {

constexpr int kSkipped = 77;
constexpr int kLoginTimeoutMs = 60000;
constexpr int kPumpStepMs = 20;
constexpr int32_t kLoginConfirmedStage = 8;

bool g_finished = false;
// Set when a link drops. The lobby's waits stop on it; a finished login does
// not end them.
bool g_dropped = false;
bool g_logged_in = false;
bool g_lobby_ok = false;
int32_t g_user_id = 0;

void ROVER_CALL
on_progress(int32_t stage, int32_t result, int32_t a, int32_t b, const char* message) {
    printf(
        "progress stage=%d result=%d a=%d b=%d message=%s\n",
        stage,
        result,
        a,
        b,
        message ? message : ""
    );
    if (stage == kLoginConfirmedStage) {
        g_finished = true;
        g_logged_in = result == 0;
        g_user_id = a;
    } else if (result != 0) {
        g_finished = true;
    }
}

void ROVER_CALL on_alert(int32_t type, const char* message, void*) {
    printf("system_alert type=%d message=%s\n", type, message);
}

void ROVER_CALL on_disconnect(int32_t which, char connecting, int32_t error) {
    printf("disconnect which=%d connecting=%d error=%d\n", which, connecting, error);
    g_finished = true;
    g_dropped = true;
}

void ROVER_CALL on_logged_out() {
    puts("logout confirmed");
}

// ---- the galaxy screen's feeds ------------------------------------------------

constexpr int kLobbyWaitMs = 15000;
constexpr int kCensusWaitMs = 3000;
constexpr int32_t kMaxPlanets = 256;
constexpr int32_t kMaxConnectors = 1024;
constexpr char kNewsPath[] = "/boneyards/ta/news/news.def";

int g_profile_replies = 0;
bool g_profile_ok = false;
int g_allegiance_replies = 0;
char g_allegiances[kMaxPlanets + 1];
int g_census_replies = 0;
int g_fetches = 0;

void ROVER_CALL on_profile(bool ok, RoverPropertyBag*) {
    printf("dossier %s\n", ok ? "received" : "refused");
    g_profile_ok = ok;
    ++g_profile_replies;
}

void ROVER_CALL on_allegiances(const char* array) {
    printf("allegiances %s (%d planets)\n", array, static_cast<int>(strlen(array)));
    snprintf(g_allegiances, sizeof g_allegiances, "%s", array);
    ++g_allegiance_replies;
}

void ROVER_CALL on_census(
    uint32_t area,
    int32_t population,
    int32_t activity,
    int32_t in_progress,
    int32_t forming,
    int32_t core_wins,
    int32_t arm_wins
) {
    printf(
        "census area=%u pop=%d activity=%d in_progress=%d forming=%d core_wins=%d arm_wins=%d\n",
        area,
        population,
        activity,
        in_progress,
        forming,
        core_wins,
        arm_wins
    );
    ++g_census_replies;
}

void ROVER_CALL on_activity(const char* activity) {
    printf("activity %s\n", activity);
}

bool ROVER_CALL on_fetched(const char* url, const char* file, int32_t status) {
    printf("fetched %s into %s, status %d\n", url, file, status);
    ++g_fetches;
    return true;
}

// Pumps until done() holds, the session is lost or wait_ms pass.
bool pump_until(const RoverInterface* rover, int wait_ms, bool (*done)()) {
    for (int waited = 0; waited < wait_ms; waited += kPumpStepMs) {
        rover->pump_events();
        if (done())
            return true;
        sleep_milliseconds(kPumpStepMs);
    }
    return done();
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

// Downloads url into path through slot 32; true when it completed.
bool download(const RoverInterface* rover, const char* url, const char* path) {
    int before = g_fetches;
    rover->fetch_http_file(url, path, on_fetched, nullptr);
    static int s_expected = 0;
    s_expected = before + 1;
    bool done = pump_until(rover, kLobbyWaitMs, [] { return g_fetches >= s_expected; });
    printf("%s: %ld bytes\n", path, file_size(path));
    return done;
}

bool copy_file(const char* from, const char* to) {
    FILE* in = oa::platform::open_file(from, "rb");
    if (!in)
        return false;
    FILE* out = oa::platform::open_file(to, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buffer[4096];
    size_t length = 0;
    while ((length = fread(buffer, 1, sizeof buffer, in)) > 0)
        fwrite(buffer, 1, length, out);
    fclose(in);
    fclose(out);
    return true;
}

// The dossier and the galaxy screen's bootstrap; false when the dossier or
// the allegiances do not arrive, or a map download given by URL fails.
bool run_galaxy_screen(const RoverInterface* rover, uint32_t user_id, const char* server_name) {
    bool ok = true;
    rover->request_get_profile(user_id, on_profile, nullptr);
    if (!pump_until(rover, kLobbyWaitMs, [] { return g_profile_replies > 0 || g_dropped; }) ||
        !g_profile_ok) {
        printf("no dossier after %d ms\n", kLobbyWaitMs);
        ok = false;
    }
    rover->request_allegiance(on_allegiances);
    rover->request_census(0, on_census, on_activity);
    if (!pump_until(rover, kLobbyWaitMs, [] { return g_allegiance_replies > 0 || g_dropped; }) ||
        g_allegiance_replies == 0) {
        printf("no allegiances after %d ms\n", kLobbyWaitMs);
        ok = false;
    }

    RoverFileServer news{};
    rover->get_file_server(0, &news);
    if (news.type_index != OA_ROVER_FILE_SERVER_INVALID) {
        char url[512];
        snprintf(url, sizeof url, "http://%s:%d%s", news.host, news.port, kNewsPath);
        download(rover, url, "news.def");
        remove("news.def");
    } else {
        puts("no news file server announced");
    }

    char map_path[512];
    snprintf(map_path, sizeof map_path, "boneyards/%s_map.gwm", server_name);
    const auto map_url_value = oa::platform::environment_value("OA_BY_MAP_URL");
    const char* map_url = map_url_value ? map_url_value->c_str() : nullptr;
    const auto map_file_value = oa::platform::environment_value("OA_BY_MAP_FILE");
    const char* map_file = map_file_value ? map_file_value->c_str() : nullptr;
    if (map_url && *map_url) {
        if (!download(rover, map_url, map_path) || file_size(map_path) <= 0) {
            printf("the sector map did not download from %s\n", map_url);
            ok = false;
        }
    } else if (map_file && *map_file) {
        if (!copy_file(map_file, map_path))
            printf("cannot copy %s\n", map_file);
    } else {
        puts("no sector map given (OA_BY_MAP_URL or OA_BY_MAP_FILE)");
    }

    int32_t sector_id = 0;
    char sector_name[64] = {};
    int32_t width = 0;
    int32_t height = 0;
    int32_t planet_count = 0;
    int32_t connector_count = 0;
    RoverSectorReader* reader = rover->open_sector_map(
        map_path, &sector_id, sector_name, &width, &height, &planet_count, &connector_count
    );
    if (!reader) {
        printf("sector map %s not read (absent, or slots 34-36 are not in this build)\n", map_path);
    } else if (
        planet_count > kMaxPlanets || connector_count > kMaxConnectors || planet_count < 0 ||
        connector_count < 0
    ) {
        printf(
            "sector map %s: %d planets, %d connectors, too many\n",
            map_path,
            planet_count,
            connector_count
        );
        rover->close_sector_map(reader);
    } else {
        static RoverSectorPlanet planets[kMaxPlanets];
        static RoverSectorConnector connectors[kMaxConnectors];
        bool read = rover->read_sector_planets(
            reader, width, height, planets, planet_count, connectors, connector_count
        );
        rover->close_sector_map(reader);
        printf(
            "sector %d \"%s\" %dx%d: %d planets, %d connectors%s\n",
            sector_id,
            sector_name,
            width,
            height,
            planet_count,
            connector_count,
            read ? "" : " (planets not read)"
        );
        size_t sides = strlen(g_allegiances);
        for (int32_t i = 0; read && i < planet_count; ++i) {
            const RoverSectorPlanet& planet = planets[i];
            printf(
                "  planet %d %-16.16s at %d,%d map side %c, live side %c\n",
                planet.id,
                planet.name,
                planet.x,
                planet.y,
                planet.side ? planet.side : ' ',
                static_cast<size_t>(i) < sides ? g_allegiances[i] : '?'
            );
            rover->request_census(static_cast<uint32_t>(planet.id), on_census, on_activity);
        }
        if (read && sides != static_cast<size_t>(planet_count))
            printf(
                "the allegiances name %d planets, the map %d\n",
                static_cast<int>(sides),
                planet_count
            );
        static int s_expected = 0;
        s_expected = g_census_replies + (read ? planet_count : 0);
        pump_until(rover, kCensusWaitMs, [] { return g_census_replies >= s_expected; });
    }
    printf("%d census replies\n", g_census_replies);
    return ok;
}

// Writes the saved server list from a line or copies it from a file.
bool write_server_list(const char* server) {
    FILE* out = oa::platform::open_file("boneyards/serv_sav.byd", "wb");
    if (!out)
        return false;
    FILE* in = strchr(server, '\t') ? nullptr : oa::platform::open_file(server, "rb");
    if (in) {
        char buffer[4096];
        size_t length = 0;
        while ((length = fread(buffer, 1, sizeof buffer, in)) > 0)
            fwrite(buffer, 1, length, out);
        fclose(in);
    } else {
        fprintf(out, "%s\r\n", server);
    }
    fclose(out);
    return true;
}

} // namespace

int main() {
    const auto server_value = oa::platform::environment_value("OA_BY_LIVE_SERVER");
    const char* server = server_value ? server_value->c_str() : nullptr;
    const auto user_value = oa::platform::environment_value("OA_BY_USER");
    const char* user = user_value ? user_value->c_str() : nullptr;
    const auto password_value = oa::platform::environment_value("OA_BY_PASSWORD");
    const char* password = password_value ? password_value->c_str() : nullptr;
    if (!server || !*server || !user || !*user || !password) {
        puts(
            "skipped: set OA_BY_LIVE_SERVER, OA_BY_USER and OA_BY_PASSWORD to log in to a live "
            "server"
        );
        return kSkipped;
    }
    const auto temporary_value = oa::platform::environment_value("TMPDIR");
    const char* temporary = temporary_value ? temporary_value->c_str() : nullptr;
    char directory[512];
    snprintf(
        directory,
        sizeof directory,
        "%s/rover-live-%d",
        temporary && *temporary ? temporary : ".",
        static_cast<int>(process_id())
    );
    make_directory(directory);
    if (change_directory(directory) != 0) {
        fprintf(stderr, "cannot enter %s\n", directory);
        return 1;
    }
    make_directory("boneyards");
    if (!write_server_list(server)) {
        fprintf(stderr, "cannot write boneyards/serv_sav.byd\n");
        return 1;
    }

    const RoverInterface* rover = nullptr;
    rover_get_interface(&rover);
    if (!rover->attach()) {
        fprintf(stderr, "attach failed\n");
        return 1;
    }
    const RoverServerList& servers = g_rover_state->servers;
    const RoverServerEntry* chosen = servers.count > 0 ? &servers.entries[0] : nullptr;
    for (int32_t i = 0; i < servers.count; ++i) {
        if (servers.entries[i].selected)
            chosen = &servers.entries[i];
    }
    char location[sizeof(RoverServerEntry::location)] = {};
    // The list is replaced during the login, so the entry's texts are copied now.
    char server_name[sizeof(RoverServerEntry::name)] = {};
    if (chosen) {
        memcpy(location, chosen->location, sizeof location);
        memcpy(server_name, chosen->name, sizeof server_name);
    }
    printf("server \"%s\", user %s\n", location, user);

    bool activated = rover->activate();
    if (activated &&
        rover->start_login(location, user, password, on_progress, on_alert, on_disconnect)) {
        for (int waited = 0; waited < kLoginTimeoutMs && !g_finished; waited += kPumpStepMs) {
            rover->pump_events();
            sleep_milliseconds(kPumpStepMs);
        }
        if (!g_finished)
            printf("no login result after %d ms\n", kLoginTimeoutMs);
        if (g_logged_in) {
            g_lobby_ok = run_galaxy_screen(rover, static_cast<uint32_t>(g_user_id), server_name);
            rover->send_logout(g_user_id, on_logged_out);
            for (int waited = 0; waited < 1000; waited += kPumpStepMs) {
                rover->pump_events();
                sleep_milliseconds(kPumpStepMs);
            }
        }
    }
    rover->deactivate();
    rover->detach();
    remove("http.log");
    remove("boneyards/serv_sav.byd");
    char map_path[512];
    snprintf(map_path, sizeof map_path, "boneyards/%s_map.gwm", server_name);
    remove(map_path);
    remove_directory("boneyards");
    if (change_directory("..") == 0)
        remove_directory(directory);
    printf("%s\n", g_logged_in ? "logged in" : "login failed");
    return g_logged_in && g_lobby_ok ? 0 : 1;
}
