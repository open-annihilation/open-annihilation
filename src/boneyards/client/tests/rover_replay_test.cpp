// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The recorded Rover interface: the checked-in alpha replays drive the
// Central Command session through the login to the dossier, answer slot 72,
// the war-map feeds, the locator, the chat rooms, the profiles, the content
// servers, the downloads, a planet's battle list and a battle's launch; delays
// hold replies back; the chat requests and the records Rover queues for the
// Boneyards are recorded; malformed replays are refused with their line.
#include "oa/boneyards/rover_calls.hpp"
#include "oa/boneyards/rover_replay.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/navigator.hpp"
#include "oa/boneyards/screens/profile.hpp"
#include "oa/rover/platform.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace oa::boneyards;
namespace fs = std::filesystem;

int g_failures = 0;

void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr const char* kAlphaReplay = OA_BONEYARDS_CLIENT_TEST_DATA "/alpha_post_login.replay";
constexpr const char* kAlphaPlanetsReplay = OA_BONEYARDS_CLIENT_TEST_DATA "/alpha_planets.replay";
constexpr const char* kAlphaChatReplay = OA_BONEYARDS_CLIENT_TEST_DATA "/alpha_chat.replay";
constexpr const char* kAlphaTicker = "Slag is under siege: every commander to the front lines";
constexpr const char* kAlphaPage = "<Page from Ghost_Commander> Meet me in the Clan Hall";
constexpr const char* kAlphaAllegiances = "CCCCIIIAAACCCCCCAAAAAAAAC";
constexpr uint32_t kFirstAlphaPlanet = 1796;
constexpr uint32_t kAlphaPlanets = 25;
constexpr int kPumpLimit = 50;

// What reached the screens, and what the feeds delivered.
struct Sink {
    screens::BoneyardsScreens* nav;
    ServiceEvents screens;
    SessionRecord session;
    std::string allegiances;
    std::string activity;
    std::vector<uint32_t> census_areas;
    std::vector<CensusReport> census;
};

Sink g_sink{};

ServiceEvents sink_events() {
    ServiceEvents events{};
    events.user = &g_sink;
    events.login_status = [](void*, const char* text) {
        if (g_sink.screens.login_status != nullptr)
            g_sink.screens.login_status(g_sink.screens.user, text);
    };
    events.login_reset = [](void*) {
        if (g_sink.screens.login_reset != nullptr)
            g_sink.screens.login_reset(g_sink.screens.user);
    };
    events.session_changed = [](void*, const SessionRecord* session) {
        g_sink.session = *session;
        if (g_sink.screens.session_changed != nullptr)
            g_sink.screens.session_changed(g_sink.screens.user, session);
    };
    events.allegiances = [](void*, const char* text) { g_sink.allegiances = text; };
    events.activity = [](void*, const char* text) { g_sink.activity = text; };
    events.census = [](void*, const CensusReport* report) {
        g_sink.census_areas.push_back(report->area_id);
        g_sink.census.push_back(*report);
    };
    return events;
}

void pump(const BoneyardsService& service, int times) {
    for (int i = 0; i < times; ++i)
        service.pump(service.user);
}

// Login stays at logged_in while the sector request is open. One idle tick
// after the bound moves it on. Sleeping here, instead of pumping through the
// bound, leaves the replay's later replies queued.
template <typename Done>
void pump_until(const BoneyardsService& service, Done done) {
    for (int i = 0; i < kPumpLimit && !done(); ++i) {
        pump(service, 1);
        if (done())
            return;
        if (!rover_session_sector_fetch_pending())
            continue;
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kSectorFetchWaitMs + 100);
        while (std::chrono::steady_clock::now() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        pump(service, 1);
        return;
    }
}

fs::path scratch_directory() {
    const fs::path root = fs::temp_directory_path() / "oa-boneyards-replay-test";
    fs::create_directories(root);
    return root;
}

void test_alpha_login() {
    char error[0x200] = {};
    CHECK(rover_replay_install(kAlphaReplay, error, sizeof error));
    if (error[0] != '\0')
        std::fprintf(stderr, "%s\n", error);
    *rover_session() = RoverSession{};
    BoneyardsService service = rover_session_service();
    auto* nav = new screens::BoneyardsScreens{};
    screens::boneyards_screens_init(nav, &service, screens::SettingsStore{});
    g_sink = Sink{nav, screens::boneyards_screens_events(nav), {}, {}, {}, {}, {}};
    const ServiceEvents events = sink_events();
    service.set_events(service.user, &events);
    CHECK(service.activate(service.user) == ActivationResult::ok);

    screens::boneyards_open(nav, screens::ByScreen::login);
    std::snprintf(nav->login.name, sizeof nav->login.name, "guest");
    screens::login_on_control(&nav->login, screens::LoginControl::guest_radio);
    screens::login_on_control(&nav->login, screens::LoginControl::login_button);
    CHECK(nav->login.state == screens::LoginState::connecting);
    pump_until(service, [&] { return nav->login.state != screens::LoginState::connecting; });
    CHECK(g_sink.session.level == SessionLevel::dossier_loaded);
    CHECK(g_sink.session.user_id == 1 && g_sink.session.allegiance == 'C');
    CHECK(std::strcmp(rover_session()->record.user_name, "oa_check") == 0);
    CHECK(nav->login.state == screens::LoginState::planetary);
    CHECK(
        service.area_name(service.user) != nullptr &&
        std::strcmp(service.area_name(service.user), "alpha") == 0
    );

    service.request_allegiance(service.user);
    service.request_census(service.user, 0);
    CHECK(g_sink.allegiances.empty()); // replies arrive from the pump
    pump(service, 2);
    CHECK(g_sink.allegiances == kAlphaAllegiances);
    CHECK(g_sink.census_areas.size() == kAlphaPlanets);
    CHECK(
        g_sink.census_areas.front() == kFirstAlphaPlanet &&
        g_sink.census_areas.back() == kFirstAlphaPlanet + kAlphaPlanets - 1
    );
    CHECK(g_sink.activity.size() == kAlphaPlanets);
    g_sink.census_areas.clear();
    g_sink.activity.clear();
    service.request_census(service.user, 1801);
    pump(service, 2);
    CHECK(g_sink.census_areas.size() == 1 && g_sink.census_areas[0] == 1801);
    CHECK(g_sink.activity.empty()); // only every-area requests carry the activity

    service.close(service.user);
    pump(service, 2);
    CHECK(g_sink.session.level == SessionLevel::offline && !rover_module_state()->bound);
    service.set_events(service.user, nullptr);
    rover_replay_uninstall();
    screens::boneyards_screens_release(nav);
    delete nav;
}

std::vector<std::string> g_room_events;

std::string room_text(
    uint32_t area_id,
    uint32_t room_id,
    const char* name,
    const char* description,
    bool needs_password,
    int32_t users,
    int32_t type,
    const char* tag
) {
    return std::to_string(area_id) + "," + std::to_string(room_id) + "," + name + "," +
           description + "," + std::to_string(needs_password) + "," + std::to_string(users) + "," +
           std::to_string(type) + "," + tag;
}

// Every chat feed the service reports, one line per event.
ServiceEvents room_events() {
    ServiceEvents events{};
    events.room_listed = [](void*,
                            int32_t change,
                            uint32_t area_id,
                            uint32_t room_id,
                            const char* name,
                            const char* description,
                            bool needs_password,
                            int32_t users,
                            int32_t type,
                            const char* tag,
                            bool last,
                            bool,
                            int32_t) {
        g_room_events.push_back(
            "listed " + std::to_string(change) + "," +
            room_text(area_id, room_id, name, description, needs_password, users, type, tag) + "," +
            std::to_string(last)
        );
    };
    events.room_updated = [](void*,
                             int32_t change,
                             uint32_t area_id,
                             uint32_t room_id,
                             const char* name,
                             const char* description,
                             bool needs_password,
                             int32_t users,
                             int32_t type,
                             const char* tag) {
        g_room_events.push_back(
            "updated " + std::to_string(change) + "," +
            room_text(area_id, room_id, name, description, needs_password, users, type, tag)
        );
    };
    events.room_entered = [](void*, int32_t result) {
        g_room_events.push_back("entered " + std::to_string(result));
    };
    events.room_member = [](void*,
                            int32_t change,
                            uint32_t user_id,
                            const char* name,
                            int32_t latency,
                            int32_t icon,
                            int32_t rank,
                            const char* allegiance) {
        g_room_events.push_back(
            "member " + std::to_string(change) + "," + std::to_string(user_id) + "," + name + "," +
            std::to_string(latency) + "," + std::to_string(icon) + "," + std::to_string(rank) +
            "," + allegiance
        );
    };
    events.chat_message = [](void*, bool from_server, uint32_t user_id, const char* text) {
        g_room_events.push_back(
            "chat " + std::to_string(from_server) + "," + std::to_string(user_id) + "," + text
        );
    };
    events.room_left = [](void*, bool failed) {
        g_room_events.push_back("left " + std::to_string(failed));
    };
    events.room_created = [](void*, uint32_t area_id, uint32_t room_id, const char* name) {
        g_room_events.push_back(
            "created " + std::to_string(area_id) + "," + std::to_string(room_id) + "," + name
        );
    };
    events.system_alert = [](void*, const AlertReport* alert) {
        g_room_events.push_back(
            "alert " + std::to_string(alert->kind) + "," + alert->text + "," +
            std::to_string(alert->sender_id) + "," + alert->notification_type
        );
    };
    return events;
}

using Events = std::vector<std::string>;

// The events since the last call.
Events take_events() {
    Events events;
    events.swap(g_room_events);
    return events;
}

bool expect_events(const Events& got, const Events& want, int line) {
    if (got == want)
        return true;
    std::fprintf(stderr, "FAIL line %d: events differ\n", line);
    for (const std::string& e : got)
        std::fprintf(stderr, "  got  %s\n", e.c_str());
    for (const std::string& e : want)
        std::fprintf(stderr, "  want %s\n", e.c_str());
    ++g_failures;
    return false;
}

#define CHECK_EVENTS(...) expect_events(take_events(), Events __VA_ARGS__, __LINE__)

bool sent_matches(
    std::size_t index,
    ReplayRequest request,
    uint32_t area_id,
    uint32_t room_id,
    const char* name,
    const char* text
) {
    ReplaySent sent{};
    return rover_replay_sent(index, &sent) && sent.request == request && sent.area_id == area_id &&
           sent.room_id == room_id && std::strcmp(sent.name, name) == 0 &&
           std::strcmp(sent.text, text) == 0;
}

// The planet checks' replay: alpha_post_login.replay's login and allegiance
// line with a synthetic census in which every planet is populated, the
// contested ones (1800-1802) have wins and the activity feed repeats the
// census activity digits; request_census(1801) answers with 1801's line,
// which rover_replay_census looks up; 1801 alone has chat rooms.
void test_alpha_planets() {
    char error[0x200] = {};
    CHECK(rover_replay_install(kAlphaPlanetsReplay, error, sizeof error));
    if (error[0] != '\0')
        std::fprintf(stderr, "%s\n", error);
    *rover_session() = RoverSession{};
    BoneyardsService service = rover_session_service();
    g_sink = Sink{};
    const ServiceEvents events = sink_events();
    service.set_events(service.user, &events);
    CHECK(service.activate(service.user) == ActivationResult::ok);
    service.request_allegiance(service.user);
    service.request_census(service.user, 0);
    pump(service, 2);
    CHECK(g_sink.allegiances == kAlphaAllegiances);
    CHECK(g_sink.census.size() == kAlphaPlanets);
    std::string digits;
    for (std::size_t i = 0; i < g_sink.census.size(); ++i) {
        const CensusReport& c = g_sink.census[i];
        CHECK(c.area_id == kFirstAlphaPlanet + i && c.population > 0);
        const bool contested = kAlphaAllegiances[i] == 'I';
        CHECK((c.core_wins + c.arm_wins > 0) == contested);
        digits += static_cast<char>('0' + c.activity);
    }
    CHECK(g_sink.activity == digits);

    g_sink.census.clear();
    service.request_census(service.user, 1801);
    pump(service, 2);
    CensusReport recorded{};
    CHECK(rover_replay_census(1801, &recorded));
    CHECK(
        g_sink.census.size() == 1 && g_sink.census[0].area_id == 1801 &&
        g_sink.census[0].core_wins == recorded.core_wins &&
        g_sink.census[0].arm_wins == recorded.arm_wins &&
        g_sink.census[0].population == recorded.population
    );
    CHECK(recorded.core_wins != 0 && recorded.arm_wins != 0);
    CHECK(!rover_replay_census(1821, &recorded));

    // Planet 1801's chat: three rooms, Main the only public one, whose
    // entry brings its four members and three lines.
    CHECK(rover_replay_room_count(1801) == 3 && rover_replay_room_count(1796) == 0);
    const ServiceEvents rooms = room_events();
    service.set_events(service.user, &rooms);
    g_room_events.clear();
    service.request_room_list(service.user, 1801);
    service.join_room(service.user, 1801, 1, nullptr, "Main");
    pump(service, 3);
    CHECK(g_room_events.size() == 3 + 1 + 4 + 3);
    if (g_room_events.size() == 11) {
        CHECK(g_room_events[0] == "listed 0,1801,1,Main,General chat,0,4,1,,0");
        CHECK(g_room_events[2] == "listed 0,1801,3,War_Room,,1,1,2,,1");
        CHECK(g_room_events[3] == "entered 0");
        CHECK(g_room_events[5] == "member 0,7,Ghost_Commander,2,8,0,C");
        CHECK(g_room_events[10] == "chat 0,9,Sure, host it and I'll join.");
    }
    service.set_events(service.user, &events);
    service.close(service.user);
    pump(service, 2);
    service.set_events(service.user, nullptr);
    rover_replay_uninstall();
    CHECK(!rover_replay_census(1801, &recorded));
}

std::string write_replay(const char* name, const char* text) {
    const fs::path path = scratch_directory() / name;
    std::ofstream(path, std::ios::binary) << text;
    return path.string();
}

// A reply waits `delay` pumps after its request.
void test_delay() {
    const std::string path = write_replay(
        "delay.replay", "# held back\ndelay 3\nallegiances CAI\narea_name bypacific\n"
    );
    char error[0x200] = {};
    CHECK(rover_replay_install(path.c_str(), error, sizeof error));
    *rover_session() = RoverSession{};
    BoneyardsService service = rover_session_service();
    g_sink = Sink{};
    const ServiceEvents events = sink_events();
    service.set_events(service.user, &events);
    CHECK(service.activate(service.user) == ActivationResult::ok);
    CHECK(std::strcmp(service.area_name(service.user), "bypacific") == 0);
    service.request_allegiance(service.user);
    pump(service, 2);
    CHECK(g_sink.allegiances.empty());
    pump(service, 1);
    CHECK(g_sink.allegiances == "CAI");
    service.request_census(service.user, 0); // nothing recorded: no replies
    pump(service, 4);
    CHECK(g_sink.census_areas.empty() && g_sink.activity.empty());
    service.close(service.user);
    service.set_events(service.user, nullptr);
    rover_replay_uninstall();
}

void record_locate(void* context, const LocateReport* report) {
    static_cast<std::vector<LocateReport>*>(context)->push_back(*report);
}

// The locator through the Boneyards service: slot 65 finds the answer
// recorded for the name in any case, and its bag reaches the console's
// LocateReport with the eight fields the game's locator callback reads; an
// unrecorded name gets no answer, and a new request drops an answer still
// held back for the last.
void test_locate() {
    const std::string path = write_replay(
        "locate.replay",
        "delay 2\n"
        "located Fighter 3 20 4\n"
        "located_field area_id uint 1801\n"
        "located_field room_id uint 5\n"
        "located_field game_id string 4f1c2d3e\n"
        "located_field pass_req int 1\n"
        "located_field game_name string Duel at dawn\n"
        "located_field area_name string Slag\n"
        "located_field room_name string Lobby\n"
        "located_field server_name string Alpha\n"
        "delay 1\n"
        "located Roamer 4 21 0\n"
        "located_field server_name string Beta\n"
    );
    char error[0x200] = {};
    CHECK(rover_replay_install(path.c_str(), error, sizeof error));
    if (error[0] != '\0')
        std::fprintf(stderr, "%s\n", error);
    *rover_session() = RoverSession{};
    BoneyardsService service = rover_session_service();
    CHECK(service.activate(service.user) == ActivationResult::ok);
    std::vector<LocateReport> located;
    CHECK(
        service.locate_user(service.user, "FIGHTER", record_locate, &located) ==
        ServiceResult::pending
    );
    pump(service, 1);
    CHECK(located.empty());
    pump(service, 1);
    CHECK(located.size() == 1);
    if (located.size() == 1) {
        const LocateReport& r = located[0];
        CHECK(r.status == 3 && std::strcmp(r.user_name, "Fighter") == 0);
        CHECK(r.area_id == 1801 && r.room_id == 5 && r.pass_required == 1);
        CHECK(std::strcmp(r.game_id, "4f1c2d3e") == 0);
        CHECK(std::strcmp(r.game_name, "Duel at dawn") == 0);
        CHECK(std::strcmp(r.area_name, "Slag") == 0 && std::strcmp(r.room_name, "Lobby") == 0);
        CHECK(std::strcmp(r.server_name, "Alpha") == 0);
    }

    located.clear();
    service.locate_user(service.user, "ghost", record_locate, &located);
    pump(service, 4);
    CHECK(located.empty());
    service.locate_user(service.user, "fighter", record_locate, &located);
    service.locate_user(service.user, "roamer", record_locate, &located);
    pump(service, 4);
    CHECK(
        located.size() == 1 && located[0].status == 4 &&
        std::strcmp(located[0].server_name, "Beta") == 0 && located[0].area_name[0] == '\0'
    );
    service.close(service.user);
    rover_replay_uninstall();
}

// The chat rooms through the Boneyards service: get_rooms answers with the
// area's recorded rows, the last ending the list; entering or switching to
// a room answers with its recorded result, and an entered room's members
// and lines follow after their own delays; an unrecorded room gets no
// answer, a refused entry brings no members, and a new entry drops the
// answer still queued for the last.
void test_rooms() {
    const std::string path = write_replay(
        "rooms.replay",
        "room 1801 1 3 1 0 Main|The main frequency\n"
        "room 1802 7 0 1 0 Elsewhere\n"
        "room 1801 2 1 3 1 Vault||Eve\n"
        "entered 1801 1 0\n"
        "entered 1801 2 3\n"
        "delay 2\n"
        "member 1801 1 9 2 3 12 C Dan\n"
        "member 1801 2 10 1 3 0 A Eve\n"
        "said 1801 1 9 hello  there\n"
    );
    char error[0x200] = {};
    CHECK(rover_replay_install(path.c_str(), error, sizeof error));
    if (error[0] != '\0')
        std::fprintf(stderr, "%s\n", error);
    *rover_session() = RoverSession{};
    BoneyardsService service = rover_session_service();
    const ServiceEvents events = room_events();
    service.set_events(service.user, &events);
    CHECK(service.activate(service.user) == ActivationResult::ok);
    g_room_events.clear();
    CHECK(service.request_room_list(service.user, 1801) == ServiceResult::pending);
    CHECK(g_room_events.empty());
    pump(service, 1);
    CHECK_EVENTS(
        {"listed 0,1801,1,Main,The main frequency,0,3,1,,0", "listed 0,1801,2,Vault,,1,1,3,Eve,1"}
    );

    CHECK(service.join_room(service.user, 1801, 1, nullptr, "Main") == ServiceResult::pending);
    pump(service, 1);
    CHECK_EVENTS({"entered 0"});
    pump(service, 1);
    CHECK_EVENTS({});
    pump(service, 1);
    CHECK_EVENTS({"member 0,9,Dan,2,3,12,C", "chat 0,9,hello  there"});

    CHECK(
        service.switch_room(service.user, 1801, 2, "pw", 1801, 1, "Vault") == ServiceResult::pending
    );
    pump(service, 4);
    CHECK_EVENTS({"entered 3"});
    service.switch_room(service.user, 1801, 2, "pw", 1801, 1, "Vault");
    CHECK(
        service.switch_room(service.user, 1801, 5, nullptr, 1801, 1, "Nowhere") ==
        ServiceResult::pending
    );
    pump(service, 4);
    CHECK_EVENTS({});
    service.close(service.user);
    service.set_events(service.user, nullptr);
    rover_replay_uninstall();
}

void login(const BoneyardsService& service) {
    LoginRequest request{};
    request.user_name = "oa_check";
    request.password = "pw";
    CHECK(service.login(service.user, &request) == ServiceResult::pending);
}

bool sent_switch(
    std::size_t index,
    uint32_t area_id,
    uint32_t room_id,
    uint32_t old_area_id,
    uint32_t old_room_id,
    const char* password
) {
    ReplaySent sent{};
    return rover_replay_sent(index, &sent) && sent.request == ReplayRequest::switch_room &&
           sent.area_id == area_id && sent.room_id == room_id && sent.old_area_id == old_area_id &&
           sent.old_room_id == old_room_id && std::strcmp(sent.password, password) == 0;
}

// The chat verbs, each delivered through its Rover callback after the delay
// in force at its line: a page and a promotion after the login; the updates
// and a page after an entry or a creation; echoes of lines said to the
// entered room from the user id the login confirmed; the leave and creation
// answers. Only the entered
// room's feeds hear its members, lines and echoes, and every say, page,
// switch, leave and creation is recorded with its arguments.
void test_chat_verbs() {
    const std::string path = write_replay(
        "chat.replay",
        "progress 8 0 4 1801\n"
        "delay 3\n"
        "page 7 <Page from Rex> hi\n"
        "alert 3 Rex made Major|PROMOTION\n"
        "delay 1\n"
        "room 1801 1 2 1 0 Main|Default\n"
        "entered 1801 1 0\n"
        "delay 2\n"
        "update 0 1801 1 3 1 0 Main|Default\n"
        "update 1 1801 9 0 3 0 Gone\n"
        "page 9 <Page from Ann> psst\n"
        "delay 1\n"
        "member 1801 1 4 1 1 0 C Me\n"
        "said 1801 1 0 Server says hi\n"
        "said 1801 1 7 Rex says hi\n"
        "entered 1801 2 0\n"
        "member 1801 2 4 1 1 0 C Me\n"
        "delay 2\n"
        "echo on\n"
        "delay 3\n"
        "left 1801 1 0\n"
        "left 1801 2 1\n"
        "delay 2\n"
        "created 1801 6 Den\n"
        "update 0 1801 6 1 3 1 Den|Our den|Me\n"
        "created 0 0 Taken\n"
    );
    char error[0x200] = {};
    CHECK(rover_replay_install(path.c_str(), error, sizeof error));
    if (error[0] != '\0')
        std::fprintf(stderr, "%s\n", error);
    *rover_session() = RoverSession{};
    BoneyardsService service = rover_session_service();
    ServiceEvents events = room_events();
    events.session_changed = [](void*, const SessionRecord* session) { g_sink.session = *session; };
    service.set_events(service.user, &events);
    CHECK(service.activate(service.user) == ActivationResult::ok);
    g_room_events.clear();
    g_sink = Sink{};

    login(service);
    pump(service, 1);
    CHECK(g_sink.session.level == SessionLevel::logged_in && g_sink.session.user_id == 4);
    pump(service, 1);
    CHECK_EVENTS({});
    pump(service, 1);
    CHECK_EVENTS({"alert 2,<Page from Rex> hi,7,", "alert 3,Rex made Major,0,PROMOTION"});

    service.join_room(service.user, 1801, 1, nullptr, "Main");
    pump(service, 1);
    CHECK_EVENTS({"entered 0"});
    pump(service, 1);
    CHECK_EVENTS({"member 0,4,Me,1,1,0,C", "chat 1,0,Server says hi", "chat 0,7,Rex says hi"});
    pump(service, 1);
    CHECK_EVENTS(
        {"updated 0,1801,1,Main,Default,0,3,1,",
         "updated 1,1801,9,Gone,,0,0,3,",
         "alert 2,<Page from Ann> psst,9,"}
    );

    CHECK(service.send_chat(service.user, 1, 1801, 0, "hello") == ServiceResult::pending);
    service.send_chat(service.user, 1, 1801, 7, "psst");
    pump(service, 1);
    CHECK_EVENTS({});
    pump(service, 1);
    CHECK_EVENTS({"chat 0,4,hello"});
    CHECK(service.page_user(service.user, "Rex", "yo") == ServiceResult::pending);

    // The old room keeps its feeds until the switch is answered.
    service.send_chat(service.user, 1, 1801, 0, "before");
    pump(service, 1);
    service.switch_room(service.user, 1801, 2, "pw", 1801, 1, "Other");
    service.send_chat(service.user, 1, 1801, 0, "after");
    pump(service, 1);
    CHECK_EVENTS({"chat 0,4,before", "entered 0"});
    pump(service, 1);
    CHECK_EVENTS({"member 0,4,Me,1,1,0,C"});
    pump(service, 2);
    CHECK_EVENTS({});

    // A creation still awaited is dropped by the next; a refusal brings no
    // updates; an unrecorded name no answer.
    service.create_room(service.user, 1801, "Unheard", "", "");
    pump(service, 2);
    CHECK_EVENTS({});
    service.create_room(service.user, 1801, "taken", "", "");
    CHECK(
        service.create_room(service.user, 1801, "DEN", "Our den", "secret") ==
        ServiceResult::pending
    );
    pump(service, 1);
    CHECK_EVENTS({});
    pump(service, 1);
    CHECK_EVENTS({"created 1801,6,Den"});
    pump(service, 2);
    CHECK_EVENTS({"updated 0,1801,6,Den,Our den,1,1,3,Me"});
    service.create_room(service.user, 1801, "Taken", "", "");
    pump(service, 4);
    CHECK_EVENTS({"created 0,0,Taken"});

    // Leaving drops the room's feeds at once.
    CHECK(service.leave_room(service.user, 1801, 2) == ServiceResult::pending);
    service.send_chat(service.user, 2, 1801, 0, "gone");
    pump(service, 2);
    CHECK_EVENTS({});
    pump(service, 1);
    CHECK_EVENTS({"left 1"});

    CHECK(rover_replay_sent_count() == 12);
    CHECK(sent_matches(0, ReplayRequest::say, 1801, 1, "", "hello"));
    ReplaySent whisper{};
    CHECK(
        rover_replay_sent(1, &whisper) && whisper.target_user_id == 7 &&
        std::strcmp(whisper.text, "psst") == 0
    );
    CHECK(sent_matches(2, ReplayRequest::page, 0, 0, "Rex", "yo"));
    CHECK(sent_matches(3, ReplayRequest::say, 1801, 1, "", "before"));
    CHECK(sent_switch(4, 1801, 2, 1801, 1, "pw"));
    CHECK(sent_matches(5, ReplayRequest::say, 1801, 1, "", "after"));
    CHECK(sent_matches(6, ReplayRequest::create_room, 1801, 0, "Unheard", ""));
    CHECK(sent_matches(8, ReplayRequest::create_room, 1801, 0, "DEN", "Our den"));
    ReplaySent created{};
    CHECK(rover_replay_sent(8, &created) && std::strcmp(created.password, "secret") == 0);
    CHECK(sent_matches(10, ReplayRequest::leave_room, 1801, 2, "", ""));
    CHECK(sent_matches(11, ReplayRequest::say, 1801, 2, "", "gone"));
    CHECK(!rover_replay_sent(12, &created));
    service.close(service.user);
    pump(service, 2);
    service.set_events(service.user, nullptr);
    rover_replay_uninstall();
    CHECK(rover_replay_sent_count() == 0);
}

// Each profile callback as an event with the fields it reads: the user id,
// then LOGINNAME, ICON and USER_STATUS of a profile, RANK and SCORE of a
// ladder standing ("-" when absent).
std::string profile_field(const screens::ProfileFields* f, const char* key) {
    char text[0x40];
    if (f->get_string(f->user, key, text, sizeof text))
        return text;
    int32_t number = 0;
    return f->get_int(f->user, key, &number) ? std::to_string(number) : "-";
}

std::string profile_user(const screens::ProfileFields* f) {
    uint32_t user_id = 0;
    return f->get_uint(f->user, "user_id", &user_id) ? std::to_string(user_id) : "-";
}

void record_profile(void*, bool ok, const screens::ProfileFields* f) {
    g_room_events.push_back(
        std::string(ok ? "profile " : "failed ") + profile_user(f) + "," +
        profile_field(f, "LOGINNAME") + "," + profile_field(f, "ICON") + "," +
        profile_field(f, "USER_STATUS")
    );
}

void record_ladder(void*, int32_t ladder_id, int32_t subid, const screens::ProfileFields* f) {
    g_room_events.push_back(
        "ladder " + std::to_string(ladder_id) + "/" + std::to_string(subid) + " " +
        profile_user(f) + "," + profile_field(f, "RANK") + "," + profile_field(f, "SCORE")
    );
}

// Slot 63 answers from the profile and ladder_profile lines of the user
// asked for: the ladder standings in the order of their first lines, then
// the profile, each bag holding the user id, after the delay in force at the
// user's first line. The player's own id without such lines gets the
// dossier; an unrecorded user gets nothing; a new request drops the answer
// still queued for the last.
void test_profiles() {
    const std::string path = write_replay(
        "profiles.replay",
        "progress 8 0 1 0\n"
        "dossier LOGINNAME string oa_check\n"
        "dossier USER_STATUS int 1\n"
        "delay 2\n"
        "ladder_profile 7 4 2 RANK string 3\n"
        "profile 7 LOGINNAME string Ghost_Commander\n"
        "ladder_profile 7 1 0 RANK string 12\n"
        "profile 7 ICON int 8\n"
        "ladder_profile 7 4 2 SCORE int 1640\n"
        "delay 1\n"
        "profile 9 LOGINNAME string Dan the Man\n"
    );
    char error[0x200] = {};
    CHECK(rover_replay_install(path.c_str(), error, sizeof error));
    if (error[0] != '\0')
        std::fprintf(stderr, "%s\n", error);
    *rover_session() = RoverSession{};
    BoneyardsService service = rover_session_service();
    ServiceEvents events{};
    events.session_changed = [](void*, const SessionRecord* session) { g_sink.session = *session; };
    service.set_events(service.user, &events);
    CHECK(service.activate(service.user) == ActivationResult::ok);
    g_sink = Sink{};
    g_room_events.clear();
    login(service);
    pump_until(service, [] { return g_sink.session.level == SessionLevel::dossier_loaded; });
    CHECK(g_sink.session.level == SessionLevel::dossier_loaded && g_sink.session.user_id == 1);

    CHECK(
        service.request_profile(service.user, 7, record_profile, record_ladder, nullptr) ==
        ServiceResult::pending
    );
    pump(service, 1);
    CHECK_EVENTS({});
    pump(service, 1);
    CHECK_EVENTS({
        "ladder 4/2 7,3,1640",
        "ladder 1/0 7,12,-",
        "profile 7,Ghost_Commander,8,-",
    });

    service.request_profile(service.user, 1, record_profile, record_ladder, nullptr);
    pump(service, 2);
    CHECK_EVENTS({"profile 1,oa_check,-,1"});

    service.request_profile(service.user, 5, record_profile, record_ladder, nullptr);
    pump(service, 4);
    CHECK_EVENTS({});

    service.request_profile(service.user, 7, record_profile, record_ladder, nullptr);
    service.request_profile(service.user, 9, record_profile, record_ladder, nullptr);
    pump(service, 4);
    CHECK_EVENTS({"profile 9,Dan the Man,-,-"});
    service.close(service.user);
    pump(service, 2);
    service.set_events(service.user, nullptr);
    rover_replay_uninstall();
}

// The checked-in chat replay: the alpha login brings a ticker line; planet
// 1801 lists two planetary defaults, a user-created frequency needing a
// password and a debriefing; entering Main brings the check user and
// Ghost_Commander, the server's notice, his line, the count update and his
// page; the check user's lines come back; Main cannot be created again,
// Outpost is; DP Alpha's Main is switched to and left.
void test_alpha_chat() {
    char error[0x200] = {};
    CHECK(rover_replay_install(kAlphaChatReplay, error, sizeof error));
    if (error[0] != '\0')
        std::fprintf(stderr, "%s\n", error);
    CHECK(rover_replay_room_count(1801) == 4 && rover_replay_room_count(1796) == 1);
    ReplayRoom room{};
    CHECK(
        rover_replay_room(1801, 2, &room) && room.room_id == 3 && room.users == 2 &&
        room.type == 3 && room.needs_password && std::strcmp(room.name, "Clan Hall") == 0
    );
    CHECK(
        rover_replay_room(1796, 0, &room) && room.room_id == 1 && room.type == 1 &&
        !room.needs_password && std::strcmp(room.name, "Main") == 0
    );
    CHECK(!rover_replay_room(1801, 4, &room) && !rover_replay_room(1797, 0, &room));
    ReplayMember member{};
    CHECK(
        rover_replay_member(1801, 2, 1, &member) && member.user_id == 7 &&
        std::strcmp(member.name, "Ghost_Commander") == 0
    );
    CHECK(!rover_replay_member(1801, 2, 2, &member) && !rover_replay_member(1801, 3, 0, &member));
    ReplaySaid said{};
    CHECK(
        rover_replay_said(1801, 1, 0, &said) && said.user_id == 0 &&
        std::strcmp(said.text, "Welcome to Slag, commander.") == 0
    );
    CHECK(
        rover_replay_said(1796, 1, 0, &said) && said.user_id == 9 &&
        std::strcmp(said.text, "Quiet out here on DP Alpha.") == 0
    );
    CHECK(!rover_replay_said(1801, 1, 2, &said));
    ReplayAlert alert{};
    CHECK(
        rover_replay_alert(0, &alert) && alert.kind == 0 && alert.area_id == 0 &&
        std::strcmp(alert.text, kAlphaTicker) == 0 && alert.notification_type[0] == '\0'
    );
    CHECK(
        rover_replay_alert(1, &alert) && alert.kind == 2 && alert.user_id == 7 &&
        alert.area_id == 1801 && alert.room_id == 1 && std::strcmp(alert.text, kAlphaPage) == 0
    );
    CHECK(rover_replay_alert(2, &alert) && alert.room_id == 2 && !rover_replay_alert(3, &alert));
    *rover_session() = RoverSession{};
    BoneyardsService service = rover_session_service();
    ServiceEvents events = room_events();
    events.session_changed = [](void*, const SessionRecord* session) { g_sink.session = *session; };
    service.set_events(service.user, &events);
    CHECK(service.activate(service.user) == ActivationResult::ok);
    g_sink = Sink{};
    g_room_events.clear();
    login(service);
    pump_until(service, [] { return g_sink.session.level == SessionLevel::dossier_loaded; });
    CHECK(g_sink.session.level == SessionLevel::dossier_loaded && g_sink.session.user_id == 1);
    CHECK_EVENTS({std::string("alert 0,") + kAlphaTicker + ",0,"});

    // Ghost_Commander's synthetic profile: the Duel and Core war standings,
    // then the profile.
    service.request_profile(service.user, 7, record_profile, record_ladder, nullptr);
    pump(service, 1);
    CHECK_EVENTS({"ladder 1/0 7,37,1642", "ladder 4/2 7,-,-", "profile 7,Ghost_Commander,8,1"});
    ReplayProfile recorded{};
    CHECK(rover_replay_profile(7, &recorded));
    CHECK(std::strcmp(recorded.name, "Ghost_Commander") == 0);
    CHECK(std::strcmp(recorded.clan, "Red Dawn") == 0);
    CHECK(!rover_replay_profile(1, &recorded) && !rover_replay_profile(9, &recorded));

    service.request_room_list(service.user, 1801);
    pump(service, 1);
    CHECK_EVENTS({
        "listed 0,1801,1,Main,Planetary default frequency,0,1,1,,0",
        "listed 0,1801,2,Staging Area,Planetary default frequency,0,1,1,,0",
        "listed 0,1801,3,Clan Hall,Clan members only,1,2,3,Ghost_Commander,0",
        "listed 0,1801,4,Debriefing,Battle debriefing,0,0,2,,1",
    });
    service.join_room(service.user, 1801, 3, nullptr, "Clan Hall");
    pump(service, 2);
    CHECK_EVENTS({"entered 1"});
    service.join_room(service.user, 1801, 1, nullptr, "Main");
    pump(service, 2);
    CHECK_EVENTS({
        "entered 0",
        "member 0,1,oa_check,1,1,0,C",
        "member 0,7,Ghost_Commander,2,8,0,C",
        "chat 1,0,Welcome to Slag, commander.",
        "chat 0,7,Anyone up for a 1v1 on Acid Pools?",
        "updated 0,1801,1,Main,Planetary default frequency,0,2,1,",
        std::string("alert 2,") + kAlphaPage + ",7,",
    });

    service.send_chat(service.user, 1, 1801, 0, "Sure, on my way.");
    pump(service, 1);
    CHECK_EVENTS({"chat 0,1,Sure, on my way."});

    service.create_room(service.user, 1801, "Main", "", "");
    pump(service, 1);
    CHECK_EVENTS({"created 0,0,Main"});
    service.create_room(service.user, 1801, "outpost", "", "");
    pump(service, 2);
    CHECK_EVENTS({"created 1801,5,Outpost", "updated 0,1801,5,Outpost,,0,1,3,oa_check"});

    service.switch_room(service.user, 1796, 1, "", 1801, 1, "Main");
    pump(service, 2);
    CHECK_EVENTS({
        "entered 0",
        "member 0,1,oa_check,1,1,0,C",
        "member 0,9,Dan,1,17,0,A",
        "chat 0,9,Quiet out here on DP Alpha.",
        "updated 0,1796,1,Main,Planetary default frequency,0,2,1,",
    });
    service.leave_room(service.user, 1796, 1);
    pump(service, 1);
    CHECK_EVENTS({"left 0"});
    // The login's get_sector is the one request this client adds. It is
    // recorded on the idle tick after the dossier, before any chat verb.
    ReplaySent sector{};
    CHECK(
        rover_replay_sent(0, &sector) && sector.request == ReplayRequest::record &&
        std::strcmp(sector.text, "{[WarManager<U:area_id=0><S:command=get_sector>]}") == 0
    );
    CHECK(sent_switch(4, 1796, 1, 1801, 1, ""));
    CHECK(sent_matches(5, ReplayRequest::leave_room, 1796, 1, "", ""));
    service.close(service.user);
    pump(service, 2);
    service.set_events(service.user, nullptr);
    rover_replay_uninstall();
}

// Malformed entries are refused with the file and line.
struct Arrival {
    std::string url;
    std::string path;
    int32_t status;
    std::string bytes; // the file's contents when it arrived
};

std::vector<Arrival> g_arrivals;

std::string file_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The content servers and downloads: slot 40 answers Rover's invalid record
// until the login delivers the file_server records, then each kind's host
// and port; slot 32 saves into a new 0BY temporary file, a recorded URL
// brings its file's bytes after its delay, and any other completes after
// one pump with the file left empty. Every download is recorded.
void test_contents_verbs() {
    const fs::path page = scratch_directory() / "news1.htm";
    std::ofstream(page, std::ios::binary) << "<HTML>news</HTML>";
    const std::string path = write_replay(
        "contents.replay",
        "progress 8 0 4 1801\n"
        "delay 2\n"
        "file_server news news.example 80\n"
        "file_server ladder ladders.example 8080\n"
        "delay 3\n"
        "http http://news.example:80/ta/news1.htm news1.htm\n"
    );
    char error[0x200] = {};
    CHECK(rover_replay_install(path.c_str(), error, sizeof error));
    if (error[0] != '\0')
        std::fprintf(stderr, "%s\n", error);
    *rover_session() = RoverSession{};
    BoneyardsService service = rover_session_service();
    ServiceEvents events{};
    events.file_arrived = [](void*, const char* url, const char* file, int32_t status) {
        g_arrivals.push_back(Arrival{url, file, status, file_text(file)});
    };
    service.set_events(service.user, &events);
    CHECK(service.activate(service.user) == ActivationResult::ok);
    g_arrivals.clear();

    char host[0x100] = "unset";
    uint16_t port = 1;
    CHECK(!service.content_server(service.user, kContentServerNews, host, sizeof host, &port));
    login(service);
    pump(service, 1);
    CHECK(!service.content_server(service.user, kContentServerNews, host, sizeof host, &port));
    pump(service, 1);
    CHECK(service.content_server(service.user, kContentServerNews, host, sizeof host, &port));
    CHECK(std::strcmp(host, "news.example") == 0 && port == 80);
    CHECK(service.content_server(service.user, kContentServerLadders, host, sizeof host, &port));
    CHECK(std::strcmp(host, "ladders.example") == 0 && port == 8080);
    CHECK(!service.content_server(service.user, 2, host, sizeof host, &port));
    CHECK(!service.content_server(service.user, -1, host, sizeof host, &port));

    const std::size_t sent_from = rover_replay_sent_count();
    char recorded[0x104] = {};
    CHECK(
        service.fetch_file(
            service.user, "http://news.example:80/ta/news1.htm", recorded, sizeof recorded
        ) == ServiceResult::pending
    );
    CHECK(fs::path(recorded).filename().string().rfind("0BY", 0) == 0);
    CHECK(fs::path(recorded).extension() == ".TMP");
    CHECK(fs::exists(recorded) && fs::file_size(recorded) == 0);
    char unrecorded[0x104] = {};
    CHECK(
        service.fetch_file(
            service.user, "http://news.example:80/ta/gone.htm", unrecorded, sizeof unrecorded
        ) == ServiceResult::pending
    );
    CHECK(std::strcmp(recorded, unrecorded) != 0 && fs::exists(unrecorded));
    ReplaySent sent{};
    CHECK(
        rover_replay_sent(sent_from, &sent) && sent.request == ReplayRequest::fetch &&
        std::strcmp(sent.text, "http://news.example:80/ta/news1.htm") == 0 &&
        std::strcmp(sent.path, recorded) == 0
    );
    CHECK(
        rover_replay_sent(sent_from + 1, &sent) && sent.request == ReplayRequest::fetch &&
        std::strcmp(sent.path, unrecorded) == 0
    );

    pump(service, 1);
    CHECK(g_arrivals.size() == 1);
    if (g_arrivals.size() == 1) {
        CHECK(g_arrivals[0].url == "http://news.example:80/ta/gone.htm");
        CHECK(g_arrivals[0].path == unrecorded && g_arrivals[0].status == 0);
        CHECK(g_arrivals[0].bytes.empty());
    }
    pump(service, 2);
    CHECK(g_arrivals.size() == 2);
    if (g_arrivals.size() == 2) {
        CHECK(g_arrivals[1].url == "http://news.example:80/ta/news1.htm");
        CHECK(g_arrivals[1].path == recorded && g_arrivals[1].status == 0);
        CHECK(g_arrivals[1].bytes == "<HTML>news</HTML>");
    }
    fs::remove(recorded);
    fs::remove(unrecorded);
    service.close(service.user);
    pump(service, 2);
    service.set_events(service.user, nullptr);
    rover_replay_uninstall();
}

std::vector<std::string> g_launch_events;
std::vector<uint32_t> g_launch_areas;

// A replay's launch lines through oa-rover: a host request goes to oa-rover,
// which queues its record for the Boneyards, and the first launch line
// answers it after its delay, oa-rover taking it as it takes the Boneyards'
// records and handing the launch to the service; the record lines after it
// follow (a debriefing for the launched game). A join request brings the
// next launch line; without one it gets no answer. Every record oa-rover
// queues for the Boneyards, the battle's reports among them, is kept with
// its text among the requests the replay received.
void test_launch() {
    const std::string path = write_replay(
        "launch.replay",
        "delay 2\n"
        "launch "
        "{[Rover<S:command=launch_game><U:area_id=709><S:game_id=a1b2c3d4><S:address=127.0.0.1>"
        "<S:password=QWERTY><S:session=a1b2c3d4><I:creator=1><I:lockOptions=1><I:result=0>"
        "<S:mission=Coast To Coast><I:playerlimit=2><S:allegiance=A>]}\n"
        "delay 1\n"
        "record {[Rover<S:command=debriefing><U:area_id=709><S:game_id=a1b2c3d4><U:room_id=5>]}\n"
        "delay 0\n"
        "launch "
        "{[Rover<S:command=launch_game><U:area_id=709><S:game_id=a1b2c3d4><S:address=127.0.0.1>"
        "<S:password=QWERTY><I:result=4>]}\n"
    );
    char error[0x200] = {};
    CHECK(rover_replay_install(path.c_str(), error, sizeof error));
    if (error[0] != '\0')
        std::fprintf(stderr, "%s\n", error);
    *rover_session() = RoverSession{};
    BoneyardsService service = rover_session_service();
    g_launch_events.clear();
    g_launch_areas.clear();
    ServiceEvents events{};
    events.launch = [](void*, const LaunchReport* launch) {
        g_launch_areas.push_back(launch != nullptr ? launch->area_id : 0);
        g_launch_events.push_back(
            std::string("launch ") + launch->game_id + "," + launch->session + "," +
            launch->address + "," + launch->password + "," + std::to_string(launch->creator) + "," +
            std::to_string(launch->lock_options) + "," + launch->mission + "," +
            std::to_string(launch->playerlimit) + "," + launch->allegiance + "," +
            std::to_string(launch->result)
        );
    };
    events.debriefing = [](void*, uint32_t area_id, uint32_t room_id, bool first) {
        g_launch_events.push_back(
            "debriefing " + std::to_string(area_id) + "," + std::to_string(room_id) + "," +
            std::to_string(first)
        );
    };
    service.set_events(service.user, &events);
    CHECK(service.activate(service.user) == ActivationResult::ok);
    const std::size_t sent_from = rover_replay_sent_count();

    const HostGameRequest request{0x709, 4, "Launch Check", nullptr, 0, 0, 0};
    CHECK(service.host_game(service.user, &request) == ServiceResult::pending);
    pump(service, 1);
    CHECK(g_launch_events.empty());
    ReplaySent sent{};
    CHECK(
        rover_replay_sent(sent_from, &sent) && sent.request == ReplayRequest::record &&
        std::strstr(sent.text, "<S:command=host_game>") != nullptr &&
        std::strstr(sent.text, "<S:name=Launch Check>") != nullptr
    );
    pump(service, 1);
    CHECK(
        (g_launch_events == std::vector<std::string>{
                                "launch a1b2c3d4,a1b2c3d4,127.0.0.1,QWERTY,1,1,Coast To Coast,2,A,0"
                            })
    );
    // <U:area_id=709> is hexadecimal, the same planet the debriefing names.
    CHECK(g_launch_areas.size() == 1 && g_launch_areas[0] == 1801);
    pump(service, 1);
    CHECK(g_launch_events.size() == 2 && g_launch_events[1] == "debriefing 1801,5,1");

    // The battle's reports reach oa-rover, which queues them for the Boneyards.
    CHECK(rover_call_send_score_report_message(0x709, "a1b2c3d4", 0x21, "gg"));
    pump(service, 1);
    CHECK(
        rover_replay_sent(sent_from + 1, &sent) && sent.request == ReplayRequest::record &&
        std::strcmp(
            sent.text,
            "{[GameManager<U:area_id=709><S:command=score_report><U:dpid=21>"
            "<S:game_id=a1b2c3d4><S:message=gg>]}"
        ) == 0
    );

    // A join brings the next launch line, here a refusal.
    CHECK(service.join_game(service.user, 0x709, "a1b2c3d4", "QWERTY") == ServiceResult::pending);
    pump(service, 1);
    CHECK(
        g_launch_events.size() == 3 && g_launch_events[2].rfind("launch a1b2c3d4,", 0) == 0 &&
        g_launch_events[2].substr(g_launch_events[2].size() - 2) == ",4"
    );
    CHECK(g_launch_areas.size() == 2 && g_launch_areas[1] == 1801);
    CHECK(
        rover_replay_sent(sent_from + 2, &sent) &&
        std::strstr(sent.text, "<S:command=join_game>") != nullptr
    );
    // No launch line is left for a further request.
    CHECK(service.join_game(service.user, 0x709, "a1b2c3d4", nullptr) == ServiceResult::pending);
    pump(service, 3);
    CHECK(g_launch_events.size() == 3);
    service.close(service.user);
    pump(service, 2);
    service.set_events(service.user, nullptr);
    rover_replay_uninstall();
}

std::vector<std::string> g_battle_events;

// A planet's battle list: enter_battles brings that area's rows and then the
// entered answer, an area with none still completes, and a second enter sends
// the list again. get_battle answers with intel that names no password, and
// leave_battles answers that the list was left.
void test_battles() {
    const std::string path = write_replay(
        "battles.replay",
        "battle 1801 0c0ffee0d15ea5e0 4 1 A Launch Check\n"
        "battle 1802 other 0 0 C Somewhere Else\n"
    );
    char error[0x200] = {};
    CHECK(rover_replay_install(path.c_str(), error, sizeof error));
    if (error[0] != '\0')
        std::fprintf(stderr, "%s\n", error);
    *rover_session() = RoverSession{};
    BoneyardsService service = rover_session_service();
    g_battle_events.clear();
    ServiceEvents events{};
    events.battle_info = [](void*,
                            int32_t change,
                            const char* game_id,
                            const char* name,
                            int32_t type,
                            int32_t forming,
                            int32_t host_code) {
        g_battle_events.push_back(
            std::string("info ") + std::to_string(change) + "," + game_id + "," + name + "," +
            std::to_string(type) + "," + std::to_string(forming) + "," + std::to_string(host_code)
        );
    };
    events.battles_done = [](void*, bool failed) {
        g_battle_events.push_back(std::string("done ") + (failed ? "1" : "0"));
    };
    events.battle_intel = [](void*, const BattleIntel* intel) {
        g_battle_events.push_back(
            std::string("intel ") + intel->game_id + "," + std::to_string(intel->has_password) +
            "," + std::to_string(intel->user_id) + "," + (intel->failed ? "1" : "0") + "," +
            std::to_string(intel->on_death) + "," +
            (intel->user_name != nullptr ? intel->user_name : "") + "," +
            (intel->mission != nullptr ? intel->mission : "")
        );
    };
    service.set_events(service.user, &events);
    CHECK(service.activate(service.user) == ActivationResult::ok);

    CHECK(service.enter_battles(service.user, 1801) == ServiceResult::pending);
    pump(service, 1);
    CHECK(
        (g_battle_events ==
         std::vector<std::string>{"info 0,0c0ffee0d15ea5e0,Launch Check,4,1,0", "done 0"})
    );

    g_battle_events.clear();
    CHECK(service.get_battle(service.user, 1801, "0c0ffee0d15ea5e0") == ServiceResult::pending);
    pump(service, 1);
    CHECK((g_battle_events == std::vector<std::string>{"intel 0c0ffee0d15ea5e0,0,0,0,2,,"}));

    g_battle_events.clear();
    CHECK(service.leave_battles(service.user, 1801) == ServiceResult::pending);
    pump(service, 1);
    CHECK((g_battle_events == std::vector<std::string>{"done 0"}));

    g_battle_events.clear();
    CHECK(service.enter_battles(service.user, 1796) == ServiceResult::pending);
    pump(service, 1);
    CHECK((g_battle_events == std::vector<std::string>{"done 0"}));

    g_battle_events.clear();
    CHECK(service.enter_battles(service.user, 1801) == ServiceResult::pending);
    pump(service, 1);
    CHECK(
        g_battle_events.size() == 2 && g_battle_events[0].rfind("info 0,0c0ffee0d15ea5e0,", 0) == 0
    );
    service.close(service.user);
    pump(service, 2);
    service.set_events(service.user, nullptr);
    rover_replay_uninstall();
}

void test_malformed() {
    struct Case {
        const char* text;
        const char* where;
    };

    const Case cases[] = {
        {"progress 4 0 0 0\n\nprogress 7 x 0 0\n", ":3: progress"},
        {"# ok\ncensus 1796 1 2 3\n", ":2: census"},
        {"dossier USER_STATUS float 1\n", ":1: a dossier field"},
        {"area_name alpha\narea_name beta\n", ":2: area_name"},
        {"delay -1\n", ":1: delay"},
        {"allegiances CA\nfrobnicate\n", ":2: unknown entry"},
        {"located Ghost 1 2\n", ":1: located takes"},
        {"located_field area_id uint 1801\n", ":1: located_field follows"},
        {"located Ghost 1 2 3\nlocated_field pass_req bool 1\n", ":2: a located field"},
        {"room 1801 1 3 1 0\n", ":1: room takes"},
        {"entered 1801 1\n", ":1: entered takes"},
        {"member 1801 1 9 2 3 12 C\n", ":1: member takes"},
        {"said 1801 1 9\n", ":1: said takes"},
        {"room 1801 1 3 1 0 A|B|C|D\n", ":1: room takes"},
        {"update 0 1801 1 2 1 0 Main\n", ":1: update follows"},
        {"entered 1801 1 0\n# members\nupdate 0 1801 1 2 1 0\n", ":3: update takes"},
        {"left 1801 1\n", ":1: left takes"},
        {"created 1801 5\n", ":1: created takes"},
        {"page 7\n", ":1: page takes"},
        {"alert 3\n", ":1: alert takes"},
        {"alert 0 |PROMOTION\n", ":1: alert takes"},
        {"alert ticker hi\n", ":1: alert takes"},
        {"echo maybe\n", ":1: echo takes"},
        {"profile 7 ICON float 1\n", ":1: a profile field is int or string"},
        {"profile Ghost LOGINNAME string Ghost\n", ":1: profile takes"},
        {"profile 7 ICON int 8 9\n", ":1: a profile int"},
        {"ladder_profile 7 1 0 RANK bool 3\n", ":1: a ladder_profile field"},
        {"ladder_profile 7 1 RANK string 3\n", ":1: ladder_profile takes"},
        {"ladder_profile 7 1\n", ":1: ladder_profile takes"},
        {"file_server gossip news.example 80\n", ":1: a file_server's type"},
        {"file_server news news.example 0\n", ":1: file_server takes"},
        {"file_server news news.example\n", ":1: file_server takes"},
        {"http http://news.example/a.htm\n", ":1: http takes"},
        {"http http://news.example/a.htm no_such_page.htm\n", ":1: an http entry's file"},
        {"battle 1801\n", ":1: battle takes"},
        {"battle 1801 gid 4 1 X Launch Check\n", ":1: battle takes"},
        {"launch launch_game\n", ":1: launch takes a record"},
        {"record {[Rover<S:command=debriefing>]}\n", ":1: record follows a launch"},
        {"launch {[Rover<S:command=launch_game>]}\nrecord room 5\n", ":2: record takes a record"},
    };
    for (const Case& c : cases) {
        const std::string path = write_replay("bad.replay", c.text);
        char error[0x200] = {};
        CHECK(!rover_replay_install(path.c_str(), error, sizeof error));
        const bool located = std::strstr(error, c.where) != nullptr &&
                             std::strncmp(error, path.c_str(), path.size()) == 0;
        CHECK(located);
        if (!located)
            std::fprintf(stderr, "  got \"%s\", want \"%s\"\n", error, c.where);
    }
    std::string long_line = "progress 4 0 0 0 ";
    long_line.append(0x900, 'x');
    const std::string path = write_replay("long.replay", (long_line + "\n").c_str());
    char error[0x200] = {};
    CHECK(!rover_replay_install(path.c_str(), error, sizeof error));
    CHECK(std::strstr(error, ":1: line too long") != nullptr);
    CHECK(!rover_replay_install("/nonexistent/oa.replay", error, sizeof error));
    CHECK(std::strstr(error, "cannot open") != nullptr);
}

} // namespace

int main() {
    const fs::path root = scratch_directory();
    rover_session_configure(root.string().c_str(), "");
    test_alpha_login();
    test_alpha_planets();
    test_delay();
    test_locate();
    test_rooms();
    test_chat_verbs();
    test_profiles();
    test_alpha_chat();
    test_contents_verbs();
    test_launch();
    test_battles();
    test_malformed();
    rover_use_platform(nullptr);
    fs::remove_all(root);
    if (g_failures != 0) {
        std::fprintf(stderr, "rover replay: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("rover replay: ok");
    return EXIT_SUCCESS;
}
