// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Recorded replies for the working searches. A Beta Tester login and an
// ordinary login each locate a player, match a planet on the war map, search
// the battles with one game locate, walk the frequencies and list the
// servers this build knows. Nothing is dialled.
#include "oa/boneyards/rover_replay.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/locator.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

using namespace oa::boneyards;

int g_failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr const char* kBetaReplay = OA_BY_CLIENT_TEST_DATA "/alpha_features_beta.replay";
constexpr const char* kPlayerReplay = OA_BY_CLIENT_TEST_DATA "/alpha_features_player.replay";
constexpr uint32_t kFirstPlanet = 1796;
constexpr int32_t kPlanetCount = 25;
constexpr uint32_t kSlag = 1801;
constexpr int kLoginPumps = 50;
// A planet that answered no frequencies would be passed over after the
// Locator's wait; every planet of the replays answers.
constexpr int kWalkPumps = (kPlanetCount + 1) * (screens::kLocatorAnswerWaitPulses + 2);

screens::Locator* g_locator = nullptr;

struct JoinSeen {
    int32_t calls = 0;
    bool needs_password = false;
    uint32_t area = 0;
    char game[40]{};
};

void on_join_open(void* user, uint32_t area_id, const char* game_id, bool needs_password) {
    auto* seen = static_cast<JoinSeen*>(user);
    ++seen->calls;
    seen->needs_password = needs_password;
    seen->area = area_id;
    std::snprintf(seen->game, sizeof seen->game, "%s", game_id != nullptr ? game_id : "");
}

bool sent_join_game(std::size_t from) {
    ReplaySent sent{};
    for (std::size_t i = from; rover_replay_sent(i, &sent); ++i) {
        if (sent.request == ReplayRequest::record && std::strstr(sent.text, "join_game") != nullptr)
            return true;
    }
    return false;
}

void on_room(
    void*,
    int32_t change,
    uint32_t area_id,
    uint32_t room_id,
    const char* name,
    const char* /*description*/,
    bool needs_password,
    int32_t users,
    int32_t type,
    const char* /*tag*/,
    bool last,
    bool global_known,
    int32_t global
) {
    screens::locator_on_room(
        g_locator,
        change,
        area_id,
        room_id,
        name,
        needs_password,
        users,
        type,
        last,
        global_known,
        global
    );
}

ServiceEvents locator_events() {
    ServiceEvents events{};
    events.room_listed = on_room;
    return events;
}

void fill_planets(screens::LocatorPlanet* planets) {
    for (int32_t i = 0; i < kPlanetCount; ++i) {
        planets[i] = {};
        planets[i].id = kFirstPlanet + static_cast<uint32_t>(i);
        planets[i].side = 'I';
        std::snprintf(planets[i].name, sizeof planets[i].name, "P%u", planets[i].id);
    }
    screens::LocatorPlanet& slag = planets[kSlag - kFirstPlanet];
    std::snprintf(slag.name, sizeof slag.name, "Slag");
    slag.side = 'C';
}

const screens::LocatorResult* row_named(const screens::Locator* locator, const char* name) {
    const int32_t count = screens::locator_result_count(locator);
    for (int32_t i = 0; i < count; ++i) {
        const screens::LocatorResult* row = screens::locator_result(locator, i);
        if (row != nullptr && std::strstr(row->name, name) != nullptr)
            return row;
    }
    return nullptr;
}

void pump_until(const BoneyardsService& service, int limit, bool (*done)(const screens::Locator*)) {
    for (int i = 0; i < limit && !done(g_locator); ++i)
        service.pump(service.user);
}

bool has_row(const screens::Locator* locator) {
    return screens::locator_result_count(locator) > 0;
}

bool battles_finished(const screens::Locator* locator) {
    return !screens::locator_listening_battles(locator);
}

bool rooms_finished(const screens::Locator* locator) {
    return !screens::locator_listening_rooms(locator);
}

void expect_player(
    screens::Locator* locator,
    const BoneyardsService& service,
    const char* name,
    const char* status,
    const char* where
) {
    screens::locator_set_kind(locator, screens::locator_kind::player);
    screens::locator_set_query(locator, name);
    screens::locator_search(locator);
    pump_until(service, 10, has_row);
    const screens::LocatorResult* row = screens::locator_result(locator, 0);
    CHECK(row != nullptr);
    if (row == nullptr)
        return;
    CHECK(std::strcmp(row->status, status) == 0);
    if (where != nullptr)
        CHECK(std::strstr(row->where, where) != nullptr);
}

void expect_servers(screens::Locator* locator) {
    screens::locator_set_kind(locator, screens::locator_kind::server);
    const int32_t count = screens::locator_result_count(locator);
    CHECK(count > 0);
    int32_t current = -1;
    for (int32_t i = 0; i < count; ++i) {
        const screens::LocatorResult* row = screens::locator_result(locator, i);
        if (row != nullptr && row->current_server)
            current = i;
        if (row != nullptr && std::strcmp(row->name, "CorePrime (Alpha)") == 0)
            CHECK(std::strcmp(row->where, "US East") == 0);
    }
    CHECK(current >= 0);
    if (current >= 0)
        screens::locator_select(locator, current);
    CHECK(!screens::locator_relocate_enabled(locator));
    CHECK(std::strstr(screens::locator_detail(locator), "logs you out") != nullptr);
}

void run_replay(const char* path, const char* account, int beta) {
    char error[0x200] = {};
    CHECK(rover_replay_install(path, error, sizeof error));
    if (error[0] != '\0')
        std::fprintf(stderr, "%s\n", error);
    *rover_session() = RoverSession{};
    BoneyardsService service = rover_session_service();
    const ServiceEvents events = locator_events();
    service.set_events(service.user, &events);
    if (service.activate(service.user) != ActivationResult::ok) {
        CHECK(false);
        rover_replay_uninstall();
        return;
    }

    LoginRequest request{};
    request.user_name = account;
    request.password = "";
    request.register_new = false;
    CHECK(service.login(service.user, &request) == ServiceResult::pending);
    for (int i = 0;
         i < kLoginPumps && rover_session()->record.level != SessionLevel::dossier_loaded;
         ++i) {
        service.pump(service.user);
        if (rover_session()->record.level == SessionLevel::dossier_loaded)
            break;
        if (!rover_session_sector_fetch_pending())
            continue;
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kSectorFetchWaitMs + 100);
        while (std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        service.pump(service.user);
        break;
    }
    CHECK(rover_session()->record.level == SessionLevel::dossier_loaded);
    CHECK(std::strcmp(rover_session()->record.user_name, account) == 0);
    CHECK(rover_session()->record.matchmaking_enabled == (beta != 0 ? 1 : 0));

    screens::Locator locator{};
    screens::locator_init(&locator, &service);
    g_locator = &locator;
    screens::LocatorPlanet planets[kPlanetCount]{};
    fill_planets(planets);
    screens::locator_set_planets(&locator, planets, kPlanetCount);

    expect_player(&locator, service, "Ghost_Commander", "chatting", "Slag");
    expect_player(&locator, service, "Ghost_Commander", "chatting", "Main");
    expect_player(&locator, service, "Yard_Dog", "examining battles", "Slag");
    expect_player(&locator, service, "Dan", "engaged in battle", "Slag");
    expect_player(&locator, service, "Wanderer", "not logged in", nullptr);
    expect_player(&locator, service, "Pages", "viewing pages", nullptr);
    expect_player(&locator, service, "Away", "other server", "Other Yard");
    expect_player(&locator, service, "Nobody", "no such player", nullptr);

    screens::locator_set_kind(&locator, screens::locator_kind::planet);
    screens::locator_set_query(&locator, "Slag");
    screens::locator_search(&locator);
    CHECK(row_named(&locator, "Slag") != nullptr);

    screens::locator_set_kind(&locator, screens::locator_kind::battle);
    screens::locator_set_query(&locator, "rumble");
    screens::locator_search(&locator);
    for (int i = 0; i < 10 && screens::locator_listening_battles(&locator); ++i)
        service.pump(service.user);
    CHECK(battles_finished(&locator));
    const screens::LocatorResult* battle = row_named(&locator, "Rumble");
    CHECK(battle != nullptr);
    if (battle != nullptr) {
        CHECK(battle->area_id == kSlag && std::strcmp(battle->where, "Slag") == 0);
        CHECK(std::strcmp(battle->game_id, "rumble1") == 0);
        CHECK(std::strcmp(battle->status, "forming") == 0);
    }

    screens::locator_set_query(&locator, "seat");
    screens::locator_search(&locator);
    for (int i = 0; i < 10 && screens::locator_listening_battles(&locator); ++i)
        service.pump(service.user);
    CHECK(battles_finished(&locator));
    const screens::LocatorResult* seated = row_named(&locator, "Seated");
    CHECK(seated != nullptr);
    if (seated != nullptr) {
        CHECK(std::strcmp(seated->status, "forming 2/4") == 0);
        CHECK(seated->bright);
    }

    JoinSeen seen{};
    locator.user = &seen;
    locator.open_join = on_join_open;
    screens::locator_set_query(&locator, "lock");
    screens::locator_search(&locator);
    for (int i = 0; i < 10 && screens::locator_listening_battles(&locator); ++i)
        service.pump(service.user);
    CHECK(battles_finished(&locator));
    const screens::LocatorResult* locked = row_named(&locator, "Locked");
    CHECK(locked != nullptr && locked->password);
    if (locked != nullptr)
        CHECK(std::strcmp(locked->status, "forming") == 0);
    const std::size_t sent_from = rover_replay_sent_count();
    screens::locator_extra(&locator);
    service.pump(service.user);
    CHECK(!sent_join_game(sent_from));
    CHECK(seen.calls == 1 && seen.needs_password);
    CHECK(seen.area == kSlag && std::strcmp(seen.game, "locked") == 0);

    screens::locator_set_query(&locator, "Nothing");
    screens::locator_search(&locator);
    for (int i = 0; i < 10 && screens::locator_listening_battles(&locator); ++i)
        service.pump(service.user);
    CHECK(battles_finished(&locator));
    CHECK(screens::locator_result_count(&locator) == 0);

    screens::locator_set_kind(&locator, screens::locator_kind::frequency);
    screens::locator_set_query(&locator, "Tactics");
    screens::locator_search(&locator);
    for (int i = 0; i < kWalkPumps && screens::locator_listening_rooms(&locator); ++i) {
        service.pump(service.user);
        screens::locator_pump(&locator);
    }
    CHECK(rooms_finished(&locator));
    CHECK(row_named(&locator, "Tactics") != nullptr);

    // Every planet lists the Sector Chat: one row, on every planet.
    screens::locator_set_query(&locator, "Sector");
    screens::locator_search(&locator);
    for (int i = 0; i < kWalkPumps && screens::locator_listening_rooms(&locator); ++i) {
        service.pump(service.user);
        screens::locator_pump(&locator);
    }
    CHECK(rooms_finished(&locator));
    CHECK(screens::locator_result_count(&locator) == 1);
    const screens::LocatorResult* sector = row_named(&locator, "Sector Chat");
    CHECK(sector != nullptr && std::strcmp(sector->where, screens::kLocatorEveryPlanet) == 0);

    // One planet lists Rally, and the row says global, so WHERE is every
    // planet without a second planet to fold.
    screens::locator_set_query(&locator, "Rally");
    screens::locator_search(&locator);
    for (int i = 0; i < kWalkPumps && screens::locator_listening_rooms(&locator); ++i) {
        service.pump(service.user);
        screens::locator_pump(&locator);
    }
    CHECK(rooms_finished(&locator));
    CHECK(screens::locator_result_count(&locator) == 1);
    const screens::LocatorResult* rally = row_named(&locator, "Rally");
    CHECK(rally != nullptr && rally->every_planet);
    CHECK(rally != nullptr && std::strcmp(rally->where, screens::kLocatorEveryPlanet) == 0);

    expect_servers(&locator);

    g_locator = nullptr;
    service.close(service.user);
    for (int i = 0; i < 10 && rover_session()->record.level != SessionLevel::offline; ++i)
        service.pump(service.user);
    service.set_events(service.user, nullptr);
    rover_replay_uninstall();
}

} // namespace

int main() {
    run_replay(kBetaReplay, "oa_beta", 1);
    run_replay(kPlayerReplay, "oa_player", 0);
    return g_failures == 0 ? 0 : 1;
}
