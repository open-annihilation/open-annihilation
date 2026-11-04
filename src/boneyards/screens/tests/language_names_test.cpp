// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Registration, Locator and chat with names outside ASCII. A name is kept
// whole in the line the player reads, and a locate, a frequency, a battle
// and a chat line carry it the same way.
#include "oa/boneyards/screens/chat.hpp"
#include "oa/boneyards/screens/locator.hpp"
#include "oa/boneyards/screens/registration.hpp"
#include "oa/boneyards/utf8.hpp"

#include "check.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace oa::boneyards;
using namespace oa::boneyards::screens;

namespace {

constexpr const char* kJose = "Jos\xC3\xA9";
constexpr const char* kSkoda = "\xC5\xA0koda";
constexpr const char* kGreek = "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xAC\xCE\xB4\xCE\xB1";
constexpr const char* kCyrillic = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
// Three characters: a registration name is three to fifteen characters.
constexpr const char* kTokyo = "\xE6\x9D\xB1\xE4\xBA\xAC\xE9\x83\xBD";
constexpr const char* kNames[] = {kJose, kSkoda, kGreek, kCyrillic, kTokyo};

bool intact(const char* text) {
    if (text == nullptr)
        return false;
    for (int32_t at = 0; text[at] != '\0';) {
        const utf8::Unit unit = utf8::read(text + at);
        if (!unit.well_formed || unit.bytes <= 0)
            return false;
        at += unit.bytes;
    }
    return true;
}

void fill_form(RegistrationForm* form, const char* name) {
    registration_form_init(form);
    utf8::copy(form->name, sizeof form->name, name, 0);
    std::snprintf(form->password, sizeof form->password, "secret");
    std::snprintf(form->confirm, sizeof form->confirm, "secret");
    std::snprintf(form->email, sizeof form->email, "a@b.co");
    form->rules = 1;
}

void test_registration() {
    for (const char* name : kNames) {
        RegistrationForm form{};
        fill_form(&form, name);
        CHECK(registration_form_submit(&form) == registration_check::ok);
        CHECK(form.sending == 1);
        CHECK_STR(form.name, name);
        CHECK(intact(form.name));
        const std::string checking = std::string("Checking name ") + name;
        CHECK_STR(form.status[2], checking.c_str());
        CHECK(intact(form.status[2]));

        char welcome[160];
        registration_reply_text(registration_reply::registered, form.name, welcome, sizeof welcome);
        const std::string wanted = std::string("Welcome, ") + name + "!";
        CHECK(std::strncmp(welcome, wanted.c_str(), wanted.size()) == 0);
        CHECK(intact(welcome));
    }
}

struct LocateTake {
    std::string name;
};

LocateTake g_locate;

ServiceResult on_locate(void*, const char* name, LocateCallback, void*) {
    g_locate.name = name != nullptr ? name : "";
    return ServiceResult::pending;
}

ServiceResult on_pending(void*, uint32_t) {
    return ServiceResult::pending;
}

ServiceResult on_locate_game(void*, const char*, GameLocateCallback, void*) {
    return ServiceResult::pending;
}

LocatorPlanet planet_named(const char* name) {
    LocatorPlanet planet{};
    planet.id = 7;
    utf8::copy(planet.name, sizeof planet.name, name, 0);
    planet.side = 'A';
    std::snprintf(planet.ta_map, sizeof planet.ta_map, "Coast to Coast");
    return planet;
}

void test_locator() {
    BoneyardsService service = boneyards_offline_service(nullptr);
    service.locate_user = on_locate;
    service.request_room_list = on_pending;
    service.locate_game = on_locate_game;
    static Locator locator;
    locator_init(&locator, &service);

    for (const char* name : kNames) {
        g_locate = {};
        locator_set_kind(&locator, locator_kind::player);
        locator_set_query(&locator, name);
        locator_search(&locator);
        CHECK(g_locate.name == name);
        LocateReport report{};
        report.status = static_cast<int32_t>(LocateStatus::chatting);
        utf8::copy(report.user_name, sizeof report.user_name, name, 0);
        report.area_id = 7;
        report.room_id = 3;
        std::snprintf(report.area_name, sizeof report.area_name, "Empyrrean");
        std::snprintf(report.room_name, sizeof report.room_name, "Ops");
        locator_on_located(&locator, &report);
        CHECK(locator.result_count == 1);
        CHECK_STR(locator.results[0].name, name);
        CHECK(intact(locator.results[0].name));
        CHECK(intact(locator.results[0].detail));
        const std::string detail = std::string(name) + " is currently chatting";
        CHECK(std::strstr(locator.results[0].detail, detail.c_str()) != nullptr);
    }

    const LocatorPlanet central = planet_named(kSkoda);
    locator_set_planets(&locator, &central, 1);
    locator_set_kind(&locator, locator_kind::planet);
    locator_set_query(&locator, kSkoda);
    locator_search(&locator);
    CHECK(locator.result_count == 1);
    CHECK_STR(locator.results[0].name, kSkoda);
    CHECK(intact(locator.results[0].detail));
    CHECK(std::strstr(locator.results[0].detail, kSkoda) != nullptr);

    locator_set_kind(&locator, locator_kind::frequency);
    locator_set_query(&locator, kSkoda);
    locator_search(&locator);
    locator_on_room(&locator, 0, central.id, 4, kSkoda, false, 2, room_type::user_created, true);
    CHECK(locator.result_count == 1);
    CHECK_STR(locator.results[0].name, kSkoda);
    CHECK(intact(locator.results[0].detail));

    const LocatorPlanet east = planet_named(kTokyo);
    locator_set_planets(&locator, &east, 1);
    locator_set_kind(&locator, locator_kind::battle);
    locator_set_query(&locator, kTokyo);
    locator_search(&locator);
    GameLocateReport battle{};
    battle.area_id = east.id;
    std::snprintf(battle.game_id, sizeof battle.game_id, "yard");
    utf8::copy(battle.name, sizeof battle.name, kTokyo, 0);
    battle.type = 1;
    battle.forming = true;
    battle.last = true;
    locator_on_game_located(&locator, &battle);
    CHECK(locator.result_count == 1);
    CHECK_STR(locator.results[0].name, kTokyo);
    CHECK(intact(locator.results[0].name));

    locator_set_identity(&locator, kCyrillic, "secret");
    CHECK_STR(locator.session_name, kCyrillic);
    CHECK(intact(locator.session_name));
}

struct ChatTake {
    std::vector<std::string> lines;
    std::vector<std::string> sent;
};

ChatTake g_chat;

void on_line(void*, const char* text, int32_t, bool) {
    g_chat.lines.emplace_back(text != nullptr ? text : "");
}

ServiceResult on_chat(void*, uint32_t, uint32_t, uint32_t, const char* text) {
    g_chat.sent.emplace_back(text != nullptr ? text : "");
    return ServiceResult::ok;
}

void submit(ChatConsole* console, const char* text) {
    utf8::copy(console->input, sizeof console->input, text, 0);
    chat_submit_input(console);
}

void test_chat() {
    g_chat = {};
    BoneyardsService service = boneyards_offline_service(nullptr);
    service.send_chat = on_chat;
    ChatSession session{};
    session.area_id = 7;
    session.room_id = 3;
    session.self_user_id = 1;
    static ChatConsole console;
    ChatUi ui{};
    ui.print_line = on_line;
    chat_console_init(&console, &session, &service, ui);

    chat_update_player(&console, PlayerListChange::joined, 8, kJose, 0, user_icon::core_private);
    CHECK_STR(console.players[0].name, kJose);
    CHECK(intact(console.players[0].name));
    const std::string spoken = std::string(kCyrillic) + " " + kTokyo;
    chat_receive_message(&console, 8, spoken.c_str(), false);
    CHECK(!g_chat.lines.empty());
    const std::string line = std::string(kJose) + ": " + spoken;
    CHECK_STR(g_chat.lines.back().c_str(), line.c_str());
    CHECK(intact(g_chat.lines.back().c_str()));

    chat_update_player(&console, PlayerListChange::joined, 9, kCyrillic, 0, user_icon::arm_private);
    chat_receive_message(&console, 9, kGreek, false);
    const std::string greek = std::string(kCyrillic) + ": " + kGreek;
    CHECK_STR(g_chat.lines.back().c_str(), greek.c_str());

    const std::size_t sent = g_chat.sent.size();
    const std::string whisper = std::string("+w ") + kJose + " hi";
    submit(&console, whisper.c_str());
    CHECK(g_chat.sent.size() == sent + 1);
    CHECK_STR(g_chat.sent.back().c_str(), "<Whispered> hi");

    const std::string typed = std::string(kSkoda) + " " + kTokyo;
    submit(&console, typed.c_str());
    CHECK_STR(g_chat.sent.back().c_str(), typed.c_str());
    CHECK(intact(g_chat.sent.back().c_str()));
}

} // namespace

int main() {
    test_registration();
    test_locator();
    test_chat();
    return by_test::finish("language names");
}
