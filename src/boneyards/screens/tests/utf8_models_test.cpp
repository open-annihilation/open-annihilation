// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Text outside ASCII in the models the Boneyards keeps: chat commands and
// rows, the profile record and its entries, battle names, the login and
// locator session fields, a registration name, and a planet name. A limit
// counts characters. A cut leaves the text well formed.
#include "oa/boneyards/screens/battle_list.hpp"
#include "oa/boneyards/screens/chat.hpp"
#include "oa/boneyards/screens/galaxy.hpp"
#include "oa/boneyards/screens/locator.hpp"
#include "oa/boneyards/screens/login.hpp"
#include "oa/boneyards/screens/registration.hpp"
#include "oa/boneyards/utf8.hpp"

#include "check.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace oa::boneyards;
using namespace oa::boneyards::screens;

namespace {

// U+6771, U+00E9, and U+6771 U+4EAC.
constexpr const char* kEast = "\xE6\x9D\xB1";
constexpr const char* kAcute = "\xC3\xA9";
constexpr const char* kTokyo = "\xE6\x9D\xB1\xE4\xBA\xAC";

std::string repeat(const char* unit, int32_t count) {
    std::string out;
    for (int32_t i = 0; i < count; ++i)
        out += unit;
    return out;
}

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

struct Bag {
    std::map<std::string, std::string> values;
};

bool bag_string(void* user, const char* key, char* out, std::size_t capacity) {
    const auto& values = static_cast<Bag*>(user)->values;
    const auto it = values.find(key);
    if (it == values.end())
        return false;
    std::snprintf(out, capacity, "%s", it->second.c_str());
    return true;
}

bool bag_int(void* user, const char* key, int32_t* out) {
    const auto& values = static_cast<Bag*>(user)->values;
    const auto it = values.find(key);
    if (it == values.end())
        return false;
    *out = static_cast<int32_t>(std::strtol(it->second.c_str(), nullptr, 10));
    return true;
}

bool bag_uint(void* user, const char* key, uint32_t* out) {
    const auto& values = static_cast<Bag*>(user)->values;
    const auto it = values.find(key);
    if (it == values.end())
        return false;
    *out = static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10));
    return true;
}

ProfileFields fields_of(Bag* bag) {
    return ProfileFields{bag, bag_string, bag_int, bag_uint, nullptr};
}

struct Created {
    std::string name;
    std::string description;
    std::string password;
};

struct Take {
    std::vector<std::string> boxes;
    std::vector<std::string> chats;
    std::vector<std::string> locates;
    std::vector<Created> creates;
};

Take* g_take = nullptr;

void on_box(void*, const char* text) {
    g_take->boxes.emplace_back(text);
}

ServiceResult on_chat(void*, uint32_t, uint32_t, uint32_t, const char* text) {
    g_take->chats.emplace_back(text);
    return ServiceResult::ok;
}

ServiceResult on_locate(void*, const char* name, LocateCallback, void*) {
    g_take->locates.emplace_back(name);
    return ServiceResult::pending;
}

ServiceResult
on_create(void*, uint32_t, const char* name, const char* description, const char* password) {
    g_take->creates.push_back(
        {name != nullptr ? name : "",
         description != nullptr ? description : "",
         password != nullptr ? password : ""}
    );
    return ServiceResult::pending;
}

void submit(ChatConsole* console, const std::string& text) {
    utf8::copy(console->input, sizeof console->input, text.c_str(), 0);
    chat_submit_input(console);
}

void test_chat() {
    const std::string fifteen = repeat(kEast, 15);
    const std::string sixteen = repeat(kEast, 16);
    const std::string room_ok = repeat(kEast, 23);
    const std::string room_long = repeat(kEast, 24);
    const std::string desc_ok = repeat(kEast, 127);
    const std::string desc_long = repeat(kEast, 128);
    const std::string acute_row = repeat(kAcute, 128);

    Take take;
    g_take = &take;
    BoneyardsService service = boneyards_offline_service(nullptr);
    service.send_chat = on_chat;
    service.locate_user = on_locate;
    service.create_room = on_create;
    service.page_user = [](void*, const char*, const char*) { return ServiceResult::ok; };
    ChatSession session{};
    session.area_id = 7;
    session.room_id = 70;
    session.self_user_id = 1;
    static ChatConsole console;
    ChatUi ui{};
    ui.message_box = on_box;
    chat_console_init(&console, &session, &service, ui);

    chat_update_player(&console, PlayerListChange::joined, 8, fifteen.c_str(), 0, 1);
    CHECK_STR(console.players[0].name, fifteen.c_str());
    CHECK(intact(console.players[0].name));
    chat_update_player(&console, PlayerListChange::joined, 9, acute_row.c_str(), 0, 1);
    CHECK(utf8::characters(console.players[1].name) == 127);
    CHECK(std::strlen(console.players[1].name) == 254);
    CHECK_STR(console.players[1].name, repeat(kAcute, 127).c_str());
    CHECK(intact(console.players[1].name));

    const std::string tag = std::string(62, 'T') + kAcute + "Z";
    const RoomListing listing{
        11, room_long.c_str(), desc_long.c_str(), false, 1, room_type::user_created, tag.c_str()
    };
    chat_update_room_row(&console, PlayerListChange::joined, &listing);
    CHECK(console.room_total == 1);
    CHECK_STR(console.rooms[0].name, room_ok.c_str());
    CHECK_STR(console.rooms[0].description, desc_ok.c_str());
    CHECK(std::strlen(console.rooms[0].tag) == 62);
    CHECK_STR(console.rooms[0].tag, std::string(62, 'T').c_str());
    CHECK(
        intact(console.rooms[0].name) && intact(console.rooms[0].description) &&
        intact(console.rooms[0].tag)
    );

    const std::size_t chats = take.chats.size();
    submit(&console, "+w " + fifteen + " hi");
    CHECK(take.boxes.empty());
    CHECK(take.chats.size() == chats + 1);
    CHECK_STR(take.chats.back().c_str(), "<Whispered> hi");
    submit(&console, "+w " + sixteen + " hi");
    CHECK_STR(take.boxes.back().c_str(), "Usage:\n\nUser name too long");
    submit(&console, "+p " + sixteen + " hi");
    CHECK_STR(take.boxes.back().c_str(), "Usage:\n\nUser name too long");
    submit(&console, "+wl " + sixteen);
    CHECK_STR(take.boxes.back().c_str(), "Usage:\n\nUser name too long");
    submit(&console, "+l " + fifteen);
    CHECK(take.locates.size() == 1 && take.locates.back() == fifteen);
    submit(&console, "+l " + sixteen);
    CHECK_STR(take.boxes.back().c_str(), "Usage:\n\nUser name too long");

    submit(&console, "+c " + room_ok);
    CHECK(take.creates.size() == 1);
    CHECK(take.creates.back().name == room_ok);
    CHECK(take.creates.back().description.empty() && take.creates.back().password.empty());
    CHECK_STR(console.create_dialog.name, room_ok.c_str());
    console.create_dialog.submitting = false;
    submit(&console, "+c " + room_long);
    CHECK_STR(take.boxes.back().c_str(), "Usage:\n\nRoom name too long");
    CHECK(take.creates.size() == 1);

    submit(&console, "+cp Yard " + fifteen);
    CHECK(take.creates.size() == 2);
    CHECK(take.creates.back().name == "Yard" && take.creates.back().password == fifteen);
    CHECK_STR(console.create_dialog.password, fifteen.c_str());
    console.create_dialog.submitting = false;
    submit(&console, "+cp Yard " + sixteen);
    CHECK_STR(take.boxes.back().c_str(), "Usage:\n\nRoom password too long");
    CHECK(take.creates.size() == 2);

    submit(&console, "+c Yard " + desc_long);
    CHECK_STR(take.boxes.back().c_str(), "Usage:\n\nRoom description too long");
    CHECK(take.creates.size() == 2);
    submit(&console, "+c Yard " + desc_ok);
    CHECK(take.creates.size() == 3);
    CHECK(take.creates.back().description == desc_ok);
    CHECK_STR(console.create_dialog.description, desc_ok.c_str());
    CHECK(intact(console.create_dialog.description));

    const std::string line = std::string(254, 'A') + kAcute + "Z";
    submit(&console, line);
    CHECK_STR(take.chats.back().c_str(), (std::string(254, 'A') + kAcute).c_str());
    CHECK(utf8::characters(take.chats.back().c_str()) == 255);
    CHECK(intact(take.chats.back().c_str()));
    submit(&console, std::string(300, 'B'));
    CHECK(take.chats.back() == std::string(255, 'B'));
    submit(&console, kTokyo);
    CHECK_STR(take.chats.back().c_str(), kTokyo);
}

void test_profile() {
    const std::string fifteen = repeat(kEast, 15);
    const std::string sixteen = repeat(kEast, 16);

    Bag exact;
    exact.values = {{"user_id", "7"}, {"LOGINNAME", fifteen}};
    ProfileFields exact_fields = fields_of(&exact);
    ProfileRecord record{};
    profile_record_init(&record);
    profile_from_fields(&record, nullptr, &exact_fields);
    CHECK_STR(record.name, fifteen.c_str());
    CHECK(intact(record.name));
    profile_record_release(&record);

    Bag ascii;
    ascii.values = {{"user_id", "7"}, {"LOGINNAME", "SentinelOfTheYards"}};
    ProfileFields ascii_fields = fields_of(&ascii);
    profile_record_init(&record);
    profile_from_fields(&record, nullptr, &ascii_fields);
    CHECK_STR(record.name, "SentinelOfTheYa");
    profile_record_release(&record);

    Bag bag;
    bag.values = {
        {"user_id", "7"},
        {"LOGINNAME", sixteen},
        {"TAG", repeat(kEast, 256)},
        {"TAFORUM_ALIAS", std::string(14, 'P') + kAcute + "Z"},
    };
    ProfileFields fields = fields_of(&bag);
    BoneyardsService service = boneyards_offline_service(nullptr);
    ChatSession session{};
    session.self_user_id = 7;
    static ProfileConsole console;
    profile_console_init(&console, &service, &session, SettingsStore{}, nullptr, nullptr, nullptr);
    profile_from_fields(&console.profile, nullptr, &fields);
    profile_update_tab(&console, 0);
    profile_select_tab(&console, 1);
    CHECK_STR(console.profile.name, fifteen.c_str());
    CHECK_STR(console.tabs.login_name, fifteen.c_str());
    CHECK(utf8::characters(console.profile.tag_line) == 256);
    CHECK(utf8::characters(console.tabs.tag_line.text) == 255);
    CHECK_STR(console.tabs.tag_line.text, repeat(kEast, 255).c_str());
    CHECK(intact(console.tabs.tag_line.text) && intact(console.profile.tag_line));
    CHECK_STR(console.tabs.forum_alias.text, (std::string(14, 'P') + kAcute).c_str());
    CHECK(intact(console.tabs.forum_alias.text));

    bag.values["TAG"] = std::string(300, 'x');
    bag.values["LOGINNAME"] = "SentinelOfTheYards";
    profile_record_release(&console.profile);
    console.record = &console.profile;
    profile_from_fields(&console.profile, nullptr, &fields);
    profile_update_tab(&console, 0);
    profile_update_tab(&console, 1);
    CHECK_STR(console.tabs.login_name, "SentinelOfTheYa");
    CHECK(std::strlen(console.tabs.tag_line.text) == 255);
    CHECK(std::strlen(console.profile.tag_line) == 300);
    profile_record_release(&console.profile);
}

void test_battles() {
    const std::string fifteen = repeat(kEast, 15);
    const std::string sixteen = repeat(kEast, 16);
    const std::string battle_name = repeat(kEast, 32);
    const std::string long_id = std::string(35, 'A') + kAcute + "Z";
    const std::string mission = std::string(62, 'M') + kAcute + "Z";
    const std::string reported = repeat(kEast, 80);

    static BattleList list;
    BoneyardsService service = boneyards_offline_service(nullptr);
    battle_list_init(&list, &service, BattleUi{});
    battle_list_on_battle_info(&list, 0x709, 0, "yard", battle_name.c_str(), 4, 1, 2);
    battle_list_on_battle_info(&list, 0x709, 0, long_id.c_str(), "id", 4, 1, 2);

    const BattleEntry* named = battle_list_entry(&list, battle_list_find(&list, "yard"));
    const BattleEntry* ident =
        battle_list_entry(&list, battle_list_find(&list, std::string(35, 'A').c_str()));
    CHECK(named != nullptr && ident != nullptr);
    if (named == nullptr || ident == nullptr) {
        battle_list_release(&list);
        return;
    }
    CHECK_STR(named->name, repeat(kEast, 31).c_str());
    CHECK(utf8::characters(named->name) == 31 && intact(named->name));
    CHECK_STR(ident->game_id, std::string(35, 'A').c_str());
    CHECK(intact(ident->game_id));
    CHECK(std::strchr(ident->game_id, '\xC3') == nullptr);

    BattleIntel intel{};
    intel.game_id = "yard";
    intel.user_id = 4;
    intel.user_name = sixteen.c_str();
    intel.mission = mission.c_str();
    battle_list_on_battle_intel(&list, &intel);
    named = battle_list_entry(&list, battle_list_find(&list, "yard"));
    CHECK(named != nullptr);
    if (named != nullptr) {
        CHECK_STR(named->host_name, fifteen.c_str());
        CHECK(intact(named->host_name));
        CHECK_STR(named->mission, std::string(62, 'M').c_str());
        CHECK(intact(named->mission));
    }

    BattleReport report{};
    report.game_id = "yard";
    report.user_id = 3;
    report.user_name = reported.c_str();
    report.message = "";
    battle_list_on_battle_report(&list, &report);
    named = battle_list_entry(&list, battle_list_find(&list, "yard"));
    CHECK(named != nullptr);
    if (named != nullptr) {
        CHECK_STR(named->scores[0].name, fifteen.c_str());
        CHECK(intact(named->scores[0].name));
        CHECK_STR(named->report_names, repeat(kEast, 62).c_str());
        CHECK(intact(named->report_names));
    }
    battle_list_release(&list);
}

struct LoginTake {
    std::string user;
    std::string password;
};

LoginTake g_login;

ServiceResult take_login(void*, const LoginRequest* request) {
    g_login.user = request->user_name != nullptr ? request->user_name : "";
    g_login.password = request->password != nullptr ? request->password : "";
    return ServiceResult::pending;
}

void test_login_and_locator() {
    const std::string fifteen = repeat(kEast, 15);
    const std::string sixteen = repeat(kEast, 16);
    const std::string password(20, 'p');

    g_login = LoginTake{};
    BoneyardsService service = boneyards_offline_service(nullptr);
    service.login = take_login;
    LoginDialog dialog{};
    login_init(&dialog, &service, nullptr, nullptr);
    utf8::copy(dialog.name, sizeof dialog.name, sixteen.c_str(), 0);
    utf8::copy(dialog.password, sizeof dialog.password, password.c_str(), 0);
    login_on_control(&dialog, LoginControl::login_button);
    CHECK_STR(dialog.name, sixteen.c_str());
    CHECK_STR(dialog.password, password.c_str());
    CHECK_STR(dialog.session_name, fifteen.c_str());
    CHECK_STR(dialog.session_password, password.substr(0, 15).c_str());
    CHECK(g_login.user == fifteen && g_login.password == password.substr(0, 15));
    CHECK(intact(dialog.session_name) && intact(dialog.session_password));

    static Locator locator;
    locator_init(&locator, nullptr);
    locator_set_identity(&locator, sixteen.c_str(), password.c_str());
    CHECK_STR(locator.session_name, fifteen.c_str());
    CHECK_STR(locator.session_password, password.substr(0, 15).c_str());
    CHECK(intact(locator.session_name));
}

void fill_registration(RegistrationForm* form, const std::string& name) {
    registration_form_init(form);
    utf8::copy(form->name, sizeof form->name, name.c_str(), 0);
    std::snprintf(form->password, sizeof form->password, "secret");
    std::snprintf(form->confirm, sizeof form->confirm, "secret");
    std::snprintf(form->email, sizeof form->email, "a@b.co");
    form->rules = 1;
}

void test_registration() {
    RegistrationForm form{};
    fill_registration(&form, repeat(kEast, 15));
    registration_form_review(&form);
    CHECK(form.check == registration_check::ok);
    CHECK(utf8::characters(form.name) == 15 && intact(form.name));

    fill_registration(&form, repeat(kAcute, 16));
    registration_form_review(&form);
    CHECK(form.check == registration_check::name_too_long);
    CHECK(utf8::characters(form.name) == 16);
}

void test_planet() {
    PlanetRecord record{};
    record.name = kTokyo;
    record.side = 'A';
    record.ta_map = "";
    record.ta_mapsource = "";
    record.ta_maptype = "";
    GalaxyPlanet planet{};
    galaxy_build_planet(&planet, &record);
    CHECK_STR(planet.name, kTokyo);
    CHECK(intact(planet.name));

    const std::string five = repeat(kEast, 5);
    record.name = five.c_str();
    galaxy_build_planet(&planet, &record);
    CHECK_STR(planet.name, five.c_str());
    CHECK(std::strlen(planet.name) == 15);

    const std::string cut = std::string(14, 'P') + kAcute + "Z";
    record.name = cut.c_str();
    galaxy_build_planet(&planet, &record);
    CHECK_STR(planet.name, std::string(14, 'P').c_str());
    CHECK(intact(planet.name));
    CHECK(std::strchr(planet.name, '\xC3') == nullptr);
}

} // namespace

int main() {
    test_chat();
    test_profile();
    test_battles();
    test_login_and_locator();
    test_registration();
    test_planet();
    return by_test::finish("utf8 models");
}
