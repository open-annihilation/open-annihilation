// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A live login and the working searches, for a maintainer who supplies the
// server and the account. Without those it skips, and it never prints the
// account.
#include "oa/boneyards/rover_session.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/system.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

using namespace oa::boneyards;
namespace fs = std::filesystem;

constexpr int kSkipped = 77;
constexpr int kLoginSeconds = 60;
constexpr int kRunSeconds = 170;

struct Live {
    bool located = false;
    int areas = 0;
    uint32_t area = 0;
    bool room_row = false;
    bool battle_row = false;
    bool battles_done = false;
};

Live g_live{};

void on_located(void*, const LocateReport*) {
    g_live.located = true;
}

void on_census(void*, const CensusReport* report) {
    if (report == nullptr)
        return;
    if (g_live.areas == 0)
        g_live.area = report->area_id;
    ++g_live.areas;
}

void on_room(
    void*,
    int32_t,
    uint32_t,
    uint32_t,
    const char*,
    const char*,
    bool,
    int32_t,
    int32_t,
    const char*,
    bool,
    bool,
    int32_t
) {
    g_live.room_row = true;
}

void on_battle(void*, int32_t, const char*, const char*, int32_t, int32_t, int32_t) {
    g_live.battle_row = true;
}

void on_battles_done(void*, bool) {
    g_live.battles_done = true;
}

ServiceEvents live_events() {
    ServiceEvents events{};
    events.census = on_census;
    events.room_listed = on_room;
    events.battle_info = on_battle;
    events.battles_done = on_battles_done;
    return events;
}

bool pump_for(const BoneyardsService& service, int seconds, bool (*done)()) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        service.pump(service.user);
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return done();
}

bool login_done() {
    return rover_session()->record.level == SessionLevel::dossier_loaded ||
           rover_session()->record.level == SessionLevel::disconnected;
}

bool located_done() {
    return g_live.located;
}

bool census_done() {
    return g_live.areas > 0;
}

bool lists_done() {
    return g_live.battles_done;
}

std::string server_list_path(const char* server, std::string* error) {
    if (std::strchr(server, '\t') != nullptr) {
        std::error_code status;
        const fs::path path = fs::temp_directory_path(status) / "oa-by-features-live-servers.byd";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << server << "\r\n";
        out.close();
        if (!out) {
            *error = "cannot write the server list";
            return {};
        }
        return path.string();
    }
    return rover_session_server_list(server, error);
}

// The location the login console names: that of the list's selected line
// (flags holding D), or of its first line.
std::string selected_location(const std::string& list) {
    std::ifstream in(oa::platform::path_from_utf8(list), std::ios::binary);
    std::string line;
    std::string first;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::string fields[5];
        std::size_t start = 0;
        for (int index = 0; index < 5; ++index) {
            const std::size_t tab = line.find('\t', start);
            fields[index] = line.substr(start, tab == std::string::npos ? tab : tab - start);
            if (tab == std::string::npos)
                break;
            start = tab + 1;
        }
        if (fields[3].empty())
            continue;
        if (first.empty())
            first = fields[3];
        if (fields[4].find('D') != std::string::npos)
            return fields[3];
    }
    return first;
}

} // namespace

int main() {
    const auto server_value = oa::platform::environment_value("OA_BY_LIVE_SERVER");
    const char* server = server_value ? server_value->c_str() : nullptr;
    const auto user_value = oa::platform::environment_value("OA_BY_USER");
    const char* user = user_value ? user_value->c_str() : nullptr;
    const auto password_value = oa::platform::environment_value("OA_BY_PASSWORD");
    const char* password = password_value ? password_value->c_str() : nullptr;
    if (server == nullptr || server[0] == '\0' || user == nullptr || user[0] == '\0' ||
        password == nullptr || password[0] == '\0') {
        std::puts(
            "skipped: set OA_BY_LIVE_SERVER, OA_BY_USER and OA_BY_PASSWORD to log in to a live "
            "server"
        );
        return kSkipped;
    }
    const auto game_value = oa::platform::environment_value("OA_GAME_DIR");
    const char* game = game_value ? game_value->c_str() : nullptr;
    if (game == nullptr || game[0] == '\0') {
        std::fputs("OA_GAME_DIR is not set\n", stderr);
        return 1;
    }
    std::string error;
    const std::string list = server_list_path(server, &error);
    if (list.empty()) {
        std::fputs("server list was not accepted\n", stderr);
        return 1;
    }

    *rover_session() = RoverSession{};
    rover_session_configure(game, list.c_str());
    std::error_code status;
    const fs::path player = fs::temp_directory_path(status) / "oa-by-features-live-player";
    fs::create_directories(player, status);
    rover_session_set_player_folder(player.string().c_str());

    BoneyardsService service = rover_session_service();
    const ServiceEvents events = live_events();
    service.set_events(service.user, &events);
    if (service.activate(service.user) != ActivationResult::ok) {
        std::fputs("could not start the online client\n", stderr);
        rover_session_set_player_folder(nullptr);
        return 1;
    }

    LoginRequest request{};
    request.user_name = user;
    request.password = password;
    request.register_new = false;
    const std::string location = selected_location(list);
    request.server_name = location.c_str();
    if (service.login(service.user, &request) != ServiceResult::pending) {
        std::fputs("login was not accepted\n", stderr);
        service.close(service.user);
        service.pump(service.user);
        return 1;
    }
    const auto started = std::chrono::steady_clock::now();
    if (!pump_for(service, kLoginSeconds, login_done) ||
        rover_session()->record.level != SessionLevel::dossier_loaded) {
        std::fputs("login did not finish\n", stderr);
        service.close(service.user);
        for (int i = 0; i < 20; ++i)
            service.pump(service.user);
        return 1;
    }
    std::puts("live login reached the dossier");

    const auto remaining = [&started]() {
        const auto used = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - started
        )
                              .count();
        const auto left = kRunSeconds - used;
        return left > 1 ? static_cast<int>(left) : 1;
    };

    const char* name = rover_session()->record.user_name;
    (void)service.locate_user(service.user, name, on_located, nullptr);
    if (!pump_for(service, remaining() / 3 > 0 ? remaining() / 3 : 1, located_done)) {
        std::fputs("player locate did not answer\n", stderr);
        service.close(service.user);
        service.pump(service.user);
        return 1;
    }

    service.request_census(service.user, 0);
    if (!pump_for(service, remaining() / 2 > 0 ? remaining() / 2 : 1, census_done)) {
        std::fputs("the war map census did not answer\n", stderr);
        service.close(service.user);
        service.pump(service.user);
        return 1;
    }
    const ServiceResult rooms = service.request_room_list(service.user, g_live.area);
    const ServiceResult battles = service.enter_battles(service.user, g_live.area);
    if ((rooms != ServiceResult::ok && rooms != ServiceResult::pending) ||
        (battles != ServiceResult::ok && battles != ServiceResult::pending)) {
        std::fputs("a planet's lists were not accepted\n", stderr);
        service.close(service.user);
        service.pump(service.user);
        return 1;
    }
    (void)pump_for(service, remaining(), lists_done);
    std::puts("live searches asked");

    service.close(service.user);
    for (int i = 0; i < 20; ++i)
        service.pump(service.user);
    service.set_events(service.user, nullptr);
    rover_session_set_player_folder(nullptr);
    return 0;
}
