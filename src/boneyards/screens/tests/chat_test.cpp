// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/chat.hpp"
#include "oa/boneyards/screens/navigator.hpp"

#include "check.hpp"
#include "oa/boneyards/screens/player_pane.hpp"
#include "profile_reply.hpp"

#include <cstring>
#include <string>
#include <vector>
#include <cstdio>

using namespace oa::boneyards;
using namespace oa::boneyards::screens;

namespace {

struct Line {
    std::string text;
    std::int32_t color;
    bool quiet;
};

struct Recorder {
    std::vector<Line> lines;
    std::vector<std::string> boxes;
    std::vector<std::string> chats; // "room/area/target:text"
    std::vector<std::string> pages;
    std::vector<std::string> locates;
    std::vector<std::string> joins;
    std::vector<std::string> switches; // "area/room/pw/old_area/old_room/name"
    std::vector<std::string> creates;
    int room_list_requests = 0;
    std::uint32_t roll = 0;
    // Scripted random draws, taken in order before `roll` is used.
    std::vector<std::uint32_t> rolls;
    // The activity light and frequency list calls, in order with the
    // service's room calls: "light on", "light off", "show N", "join ...",
    // "switch ...", "rooms"; "input" when the input line is replaced.
    std::vector<std::string> ui_calls;
    // The list row calls: "room+I name count", "room=I name count",
    // "room-I name", "player+I name icon/latency" (then " m" when muzzled
    // and " l" when whisper-locked), "player=I ...", "player-I name",
    // "rooms cleared", "players cleared", "select I", "select player I".
    std::vector<std::string> rows;
    // The player pane's change notices: "page" when it has a new page,
    // else "fields".
    std::vector<std::string> panes;
};

Recorder* rec(void* user) {
    return static_cast<Recorder*>(user);
}

void ui_print(void* u, const char* text, std::int32_t color, bool quiet) {
    rec(u)->lines.push_back({text, color, quiet});
}

void ui_box(void* u, const char* text) {
    rec(u)->boxes.emplace_back(text);
}

std::uint32_t ui_random(void* u) {
    Recorder* r = rec(u);
    if (r->rolls.empty())
        return r->roll;
    const std::uint32_t roll = r->rolls.front();
    r->rolls.erase(r->rolls.begin());
    return roll;
}

void ui_activity(void* u, bool running) {
    rec(u)->ui_calls.emplace_back(running ? "light on" : "light off");
}

void ui_show_room(void* u, std::size_t row) {
    rec(u)->ui_calls.push_back("show " + std::to_string(row));
}

const char* change_mark(RowChange change) {
    return change == RowChange::added ? "+" : change == RowChange::updated ? "=" : "-";
}

void ui_room_row(void* u, RowChange change, const ChatRoom* room, std::size_t index) {
    std::string row =
        std::string("room") + change_mark(change) + std::to_string(index) + " " + room->name;
    if (change != RowChange::removed)
        row += " " + std::to_string(room->population);
    rec(u)->rows.push_back(row);
}

void ui_player_row(void* u, RowChange change, const ChatPlayer* player, std::size_t index) {
    std::string row =
        std::string("player") + change_mark(change) + std::to_string(index) + " " + player->name;
    if (change != RowChange::removed) {
        row += " " + std::to_string(player->user_icon) + "/" + std::to_string(player->latency_icon);
        row += player->silenced ? " m" : "";
        row += player->whisper_locked ? " l" : "";
    }
    rec(u)->rows.push_back(row);
}

void ui_rooms_cleared(void* u) {
    rec(u)->rows.emplace_back("rooms cleared");
}

void ui_players_cleared(void* u) {
    rec(u)->rows.emplace_back("players cleared");
}

void ui_select_room(void* u, std::int32_t row) {
    rec(u)->rows.push_back("select " + std::to_string(row));
}

void ui_pane_changed(void* u, bool page) {
    rec(u)->panes.emplace_back(page ? "page" : "fields");
}

void ui_select_player(void* u, int32_t row) {
    rec(u)->rows.push_back("select player " + std::to_string(row));
}

void ui_show_input(void* u) {
    rec(u)->ui_calls.emplace_back("input");
}

ServiceResult
svc_chat(void* u, std::uint32_t room, std::uint32_t area, std::uint32_t target, const char* text) {
    rec(u)->chats.push_back(
        std::to_string(room) + "/" + std::to_string(area) + "/" + std::to_string(target) + ":" +
        text
    );
    return ServiceResult::ok;
}

ServiceResult svc_page(void* u, const char* name, const char* text) {
    rec(u)->pages.push_back(std::string(name) + ":" + text);
    return ServiceResult::ok;
}

ServiceResult svc_locate(void* u, const char* name, LocateCallback, void*) {
    rec(u)->locates.emplace_back(name);
    return ServiceResult::pending;
}

ServiceResult
svc_join(void* u, std::uint32_t area, std::uint32_t room, const char* pw, const char* name) {
    rec(u)->joins.push_back(
        std::to_string(area) + "/" + std::to_string(room) + "/" + (pw != nullptr ? pw : "-") + "/" +
        name
    );
    rec(u)->ui_calls.push_back("join " + std::to_string(room));
    return ServiceResult::pending;
}

ServiceResult svc_switch(
    void* u,
    std::uint32_t area,
    std::uint32_t room,
    const char* pw,
    std::uint32_t old_area,
    std::uint32_t old_room,
    const char* name
) {
    rec(u)->switches.push_back(
        std::to_string(area) + "/" + std::to_string(room) + "/" + (pw != nullptr ? pw : "-") + "/" +
        std::to_string(old_area) + "/" + std::to_string(old_room) + "/" + name
    );
    rec(u)->ui_calls.push_back("switch " + std::to_string(room));
    return ServiceResult::pending;
}

ServiceResult svc_rooms(void* u, std::uint32_t) {
    ++rec(u)->room_list_requests;
    rec(u)->ui_calls.emplace_back("rooms");
    return ServiceResult::pending;
}

ServiceResult
svc_create(void* u, std::uint32_t area, const char* name, const char* desc, const char* pw) {
    rec(u)->creates.push_back(
        std::to_string(area) + ":" + name + "|" + desc + "|" + (pw != nullptr ? pw : "-")
    );
    return ServiceResult::pending;
}

struct Fixture {
    Recorder r;
    OfflineServiceState people{};
    BoneyardsService service{};
    ChatSession session{};
    ChatConsole console{};

    Fixture() {
        service = boneyards_offline_service(&people);
        // Route chat traffic to the recorder while keeping the offline people list.
        static Fixture* self;
        self = this;
        service.send_chat =
            [](void*, std::uint32_t a, std::uint32_t b, std::uint32_t c, const char* t) {
                return svc_chat(&self->r, a, b, c, t);
            };
        service.page_user = [](void*, const char* n, const char* t) {
            return svc_page(&self->r, n, t);
        };
        service.locate_user = [](void*, const char* n, LocateCallback cb, void* ctx) {
            return svc_locate(&self->r, n, cb, ctx);
        };
        service.join_room =
            [](void*, std::uint32_t a, std::uint32_t b, const char* p, const char* n) {
                return svc_join(&self->r, a, b, p, n);
            };
        service.switch_room = [](void*,
                                 std::uint32_t a,
                                 std::uint32_t b,
                                 const char* p,
                                 std::uint32_t oa,
                                 std::uint32_t ob,
                                 const char* n) {
            return svc_switch(&self->r, a, b, p, oa, ob, n);
        };
        service.request_room_list = [](void*, std::uint32_t a) { return svc_rooms(&self->r, a); };
        service.create_room =
            [](void*, std::uint32_t a, const char* n, const char* d, const char* p) {
                return svc_create(&self->r, a, n, d, p);
            };
        session.system_font = 250;
        session.rank_0x15_font = 251;
        session.arm_font = 252;
        session.core_font = 253;
        session.self_user_id = 1;
        session.area_id = 7;
        session.room_id = 70;
        chat_console_init(
            &console,
            &session,
            &service,
            ChatUi{
                &r,
                ui_print,
                ui_box,
                ui_random,
                ui_activity,
                ui_show_room,
                ui_room_row,
                ui_player_row,
                ui_rooms_cleared,
                ui_players_cleared,
                ui_select_room,
                ui_pane_changed,
                ui_select_player,
                ui_show_input
            }
        );
        chat_update_player(&console, PlayerListChange::joined, 1, "Me", 0, 1);
        chat_update_player(&console, PlayerListChange::joined, 2, "Bob", 1, 0x17);
        chat_update_player(&console, PlayerListChange::joined, 3, "Carol", 1, 0x09);
        chat_update_player(&console, PlayerListChange::joined, 0x80000004u, "Guest", 1, 2);
    }

    bool submit(const char* text) {
        std::snprintf(console.input, sizeof console.input, "%s", text);
        return chat_submit_input(&console);
    }
};

void test_plain_chat_and_empty_line() {
    Fixture f;
    CHECK(!f.submit("    "));
    CHECK(f.r.chats.empty());
    CHECK(f.submit("  hello there"));
    CHECK(f.r.chats.size() == 1);
    CHECK_STR(f.r.chats[0].c_str(), "70/7/0:  hello there");
    CHECK_STR(f.console.input, "");
    // Unknown commands are sent verbatim as chat.
    CHECK(f.submit("+dance now"));
    CHECK_STR(f.r.chats[1].c_str(), "70/7/0:+dance now");
}

void test_help_summary() {
    Fixture f;
    CHECK(f.submit("+?"));
    CHECK(f.r.lines.size() == 12);
    CHECK_STR(f.r.lines[0].text.c_str(), "<<<< Command Summary >>>>>");
    // The summary prints the heading with GREEN_BUTTON and the rest quietly.
    CHECK(!f.r.lines[0].quiet);
    CHECK(f.r.lines[1].quiet && f.r.lines[11].quiet);
    CHECK_STR(f.r.lines[11].text.c_str(), "Go to frequency:  +g | +goto <frequency> [<password>]");
    Fixture g;
    g.submit("+HELP");
    CHECK(g.r.lines.size() == 12);
}

void test_whisper() {
    Fixture f;
    f.submit("+w bob  are you  there");
    CHECK(f.r.chats.size() == 1);
    CHECK_STR(f.r.chats[0].c_str(), "70/7/2:<Whispered> are you  there");
    CHECK_STR(f.r.lines.back().text.c_str(), "<You whispered to bob> are you  there");
    f.submit("+whisper");
    CHECK_STR(f.r.boxes.back().c_str(), "Usage:\n\n+whisper username msg ...");
    f.submit("+w nobody hi");
    CHECK_STR(f.r.boxes.back().c_str(), "User 'nobody' not found in this chat room.");
    f.submit("+w abcdefghijklmnop hi");
    CHECK_STR(f.r.boxes.back().c_str(), "Usage:\n\nUser name too long");
}

void test_whisper_lock() {
    Fixture f;
    f.submit("+wl Carol");
    CHECK(f.console.whisper_lock_user == 3);
    CHECK(chat_find_player(&f.console, 3)->whisper_locked);
    CHECK_STR(f.r.lines.back().text.c_str(), "[Whisper Lock on 'Carol' Activated]");
    CHECK(f.r.lines.back().color == 250);
    // Plain text is now rewritten into a whisper to Carol.
    f.submit("psst");
    CHECK_STR(f.r.chats.back().c_str(), "70/7/3:<Whispered> psst");
    // Switching the lock clears the previous row.
    f.submit("+wl bob");
    CHECK(!chat_find_player(&f.console, 3)->whisper_locked);
    CHECK(chat_find_player(&f.console, 2)->whisper_locked);
    f.submit("-wl");
    CHECK(f.console.whisper_lock_user == 0);
    CHECK_STR(f.r.lines.back().text.c_str(), "[Whisper Lock Deactivated]");
    // A locked user who leaves makes the next line fail and restores the input.
    f.submit("+wl carol");
    f.console.pane.visible = true;
    f.console.pane.user_id = 3;
    f.console.pane.whisper_icon_value = 4;
    chat_update_player(&f.console, PlayerListChange::left, 3, nullptr, 0, 0);
    CHECK(f.console.whisper_lock_user == 0);
    // A member leaving disables the shown pane's whisper icon.
    CHECK(f.console.pane.whisper_icon_value == 0);
    f.console.whisper_lock_user = 3; // the lock survives in 3.1c if the row vanished first
    f.submit("lost words");
    CHECK_STR(f.r.lines[f.r.lines.size() - 2].text.c_str(), "[Whisper failed - Message Not Sent]");
    CHECK_STR(f.console.input, "lost words");
}

void test_muzzle() {
    Fixture f;
    f.submit("+m carol");
    CHECK(chat_is_silenced(&f.console, 3));
    CHECK(chat_find_player(&f.console, 3)->silenced);
    CHECK(f.people.silenced_count == 1);
    f.submit("+m carol"); // already muzzled: no toggle
    CHECK(chat_is_silenced(&f.console, 3));
    // A muzzled sender is dropped; SysOp lines are never dropped.
    const std::size_t before = f.r.lines.size();
    chat_receive_message(&f.console, 3, "noise");
    CHECK(f.r.lines.size() == before);
    f.submit("-m CAROL");
    CHECK(!chat_is_silenced(&f.console, 3));
    // Guests are muzzled in the console, not the people list.
    f.submit("+m guest");
    CHECK(chat_is_silenced(&f.console, 0x80000004u));
    CHECK(f.people.silenced_count == 0);
    f.submit("-muzzle");
    CHECK_STR(f.r.boxes.back().c_str(), "Usage:\n\n-muzzle username ...");
}

void test_receive_colours() {
    Fixture f;
    chat_receive_message(&f.console, 2, "hello all");
    CHECK_STR(f.r.lines.back().text.c_str(), "Bob [SysOp] : hello all");
    CHECK(f.r.lines.back().color == 250);
    chat_receive_message(&f.console, 3, "hi");
    CHECK_STR(f.r.lines.back().text.c_str(), "Carol: hi");
    CHECK(f.r.lines.back().color == 253);
    chat_receive_message(&f.console, 1, "me");
    CHECK(f.r.lines.back().color == kDefaultTextColor);
}

// A line from the server names no member, so it is not shown. A listed
// member's rank icon is the icon frame he arrived with, and his rank and
// allegiance stay on the row. A SysOp's line carries the tag.
void test_server_line_rank_and_sysop() {
    Fixture f;
    const std::size_t before = f.r.lines.size();
    chat_receive_message(&f.console, 0, "Welcome to Slag, commander.", true);
    CHECK(f.r.lines.size() == before);

    f.r.rows.clear();
    chat_update_player(&f.console, PlayerListChange::joined, 9, "Dan", 2, 0x0d, 3, "ARM");
    const ChatPlayer* dan = chat_find_player(&f.console, 9);
    CHECK(dan != nullptr && dan->user_icon == 0x0d && dan->rank == 3);
    CHECK_STR(dan->allegiance, "ARM");
    CHECK(f.r.rows.back() == "player+4 Dan 13/2");

    chat_receive_message(&f.console, 2, "keep it clean", false);
    CHECK_STR(f.r.lines.back().text.c_str(), "Bob [SysOp] : keep it clean");
    CHECK(f.r.lines.back().color == f.session.system_font);

    chat_receive_message(&f.console, 9, "on my way", false);
    CHECK_STR(f.r.lines.back().text.c_str(), "Dan: on my way");
    CHECK(f.r.lines.back().color == kDefaultTextColor);

    // A refresh keeps the new rank and allegiance, and the rank icon follows
    // the new icon frame.
    chat_update_player(&f.console, PlayerListChange::joined, 9, "Dan", 1, 0x11, 8, "CORE");
    dan = chat_find_player(&f.console, 9);
    CHECK(dan->user_icon == 0x11 && dan->rank == 8 && dan->latency_icon == 1);
    CHECK_STR(dan->allegiance, "CORE");
    CHECK(f.r.rows.back() == "player=4 Dan 17/1");
    chat_receive_message(&f.console, 9, "blue", false);
    CHECK(f.r.lines.back().color == f.session.arm_font);
}

void test_page_and_locate() {
    Fixture f;
    f.submit("+p zed hello  you");
    CHECK_STR(f.r.pages.back().c_str(), "zed:hello  you");
    CHECK_STR(f.r.lines.back().text.c_str(), "<You paged user zed> hello  you");
    f.session.account_flags = kAccountUnregistered;
    f.submit("+page zed hi");
    CHECK(f.r.pages.size() == 1);
    CHECK_STR(
        f.r.boxes.back().c_str(),
        "Only registered users have access to this feature.\n"
        "Visit the Registration console to register."
    );
    f.submit("+l ghost");
    CHECK_STR(f.r.locates.back().c_str(), "ghost");

    LocateReport report{};
    std::snprintf(report.user_name, sizeof report.user_name, "Ghost");
    std::snprintf(report.area_name, sizeof report.area_name, "Rigel");
    std::snprintf(report.room_name, sizeof report.room_name, "Lobby");
    std::snprintf(report.game_name, sizeof report.game_name, "Brawl");
    char out[0x100];
    report.status = 1;
    chat_format_locate_result(&report, out, sizeof out);
    CHECK_STR(out, "Ghost is currently chatting on Planet Rigel Frequency Lobby.");
    report.status = 3;
    chat_format_locate_result(&report, out, sizeof out);
    CHECK_STR(out, "Ghost is currently engaged in Battle Brawl on Planet Rigel.");
    report.status = 9;
    chat_format_locate_result(&report, out, sizeof out);
    CHECK_STR(out, "There has been a locator system error.  Please try again later.");
    report.status = 0;
    chat_on_locate_result(&f.console, &report);
    CHECK_STR(f.r.lines.back().text.c_str(), "Ghost is not currently logged in.");
}

void add_room(
    Fixture& f,
    std::uint32_t id,
    const char* name,
    bool pw,
    std::int32_t pop,
    std::int32_t type,
    bool last
) {
    RoomListing l{id, name, "", pw, pop, type, ""};
    chat_on_room_listed(&f.console, PlayerListChange::joined, &l, last);
}

std::string room_order(const ChatConsole& c) {
    std::string order;
    for (std::size_t i = 0; i < c.room_total; ++i) {
        order += i == 0 ? "" : ",";
        order += c.rooms[i].name;
    }
    return order;
}

void test_goto_and_create() {
    Fixture f;
    add_room(f, 70, "Lobby", false, 3, 1, false);
    add_room(f, 71, "Secret", true, 1, 2, false);
    add_room(f, 72, "Open", false, 1, 2, true);
    f.submit("+g nowhere");
    CHECK_STR(f.r.boxes.back().c_str(), "Frequency 'nowhere' not found in this chat area.");
    f.submit("+g secret");
    CHECK_STR(f.r.boxes.back().c_str(), "Frequency 'secret' requires password.");
    f.submit("+g lobby");
    CHECK_STR(f.r.boxes.back().c_str(), "Already in frequency 'lobby'");
    // Going to a frequency switches from the current one (slot 11 gets the
    // current room and area as the old ones).
    f.submit("+g secret pw");
    CHECK(f.r.joins.empty());
    CHECK_STR(f.r.switches.back().c_str(), "7/71/pw/7/70/secret");
    CHECK(f.session.pending_room_id == 71 && f.session.pending_area_id == 7);
    CHECK_STR(f.session.pending_room_name, "secret");
    const std::size_t switches = f.r.switches.size();
    f.submit("+g open"); // another entry is pending: ignored
    CHECK(f.r.switches.size() == switches);
    f.session.pending_room_id = 72;
    f.submit("+g open");
    CHECK_STR(f.r.boxes.back().c_str(), "Already trying to go to frequency 'open'");
    f.session.pending_room_id = 0;

    // A creation is made in the current area and slot 69 gets the dialog's
    // password copy, empty for a public frequency.
    f.submit("+c MyRoom a nice  place");
    CHECK_STR(f.r.creates.back().c_str(), "7:MyRoom|a nice  place|");
    f.submit("+c Another");
    CHECK_STR(f.r.boxes.back().c_str(), "Room creation failed");
    f.console.create_dialog.submitting = false;
    f.submit("+cp Priv");
    CHECK_STR(
        f.r.boxes.back().c_str(),
        "Usage:\n\n+createprivate <frequency> <password> [<description>] ..."
    );
    f.submit("+cp Priv 0123456789abcdef");
    CHECK_STR(f.r.boxes.back().c_str(), "Usage:\n\nRoom password too long");
    f.submit("+cp Priv sesame");
    CHECK_STR(f.r.creates.back().c_str(), "7:Priv||sesame");
    f.submit("+c 012345678901234567890123");
    CHECK_STR(f.r.boxes.back().c_str(), "Usage:\n\nRoom name too long");
}

void test_join_results_and_default_room() {
    Fixture f;
    f.session.room_id = 0;
    chat_open_on_planet(&f.console);
    CHECK(f.console.enter_default_room);
    CHECK(f.r.room_list_requests == 1);
    f.r.roll = 3;
    add_room(f, 10, "Crowded", false, 31, 1, false);
    add_room(f, 11, "Quiet", false, 2, 1, false);
    add_room(f, 12, "Private", false, 0, 2, false);
    add_room(f, 13, "Calm", false, 30, 1, true);
    // Candidates: 11 and 13; roll 3 % 2 picks 13.
    CHECK_STR(f.r.joins.back().c_str(), "7/13/-/Calm");
    CHECK(!f.console.enter_default_room);
    chat_on_join_result(&f.console, JoinResult::entered);
    CHECK_STR(f.r.lines.back().text.c_str(), "[Entered 'Calm']");
    CHECK(f.session.room_id == 13);
    CHECK(f.session.pending_room_id == 0);
    CHECK(f.r.room_list_requests == 2);
    // Reopening the planet goes straight to the remembered room.
    chat_open_on_planet(&f.console);
    CHECK_STR(f.r.joins.back().c_str(), "7/13/-/Preferred Room");
    chat_on_join_result(&f.console, JoinResult::failed);
    CHECK_STR(
        f.r.lines.back().text.c_str(), "[Entry to 'Preferred Room' failed - Attempting a Default]"
    );
    CHECK(f.console.enter_default_room);
    // An empty listing while a default is pending reports the failure.
    add_room(f, 20, "Busy", false, 40, 1, true);
    CHECK_STR(f.r.boxes.back().c_str(), "Unable to enter chat on this planet. Please try another.");
}

void test_controls_and_pane() {
    Fixture f;
    // The frequency information pane starts hidden with no frequency
    // (kNoRoomInfo).
    CHECK(!f.console.room_info_visible && f.console.room_info_room_id == 0xffffffffu);
    // Building the pane's record reads Profile.def the first time (the
    // profile request runs the record constructor).
    static int template_requests = 0;
    f.service.profile_templates = [](void*) -> const ProfileTemplates* {
        ++template_requests;
        return nullptr;
    };
    static std::vector<by_test::ProfileRequestSeen> profile_requests;
    f.service.request_profile = [](void*,
                                   std::uint32_t user_id,
                                   ProfileCallback on_profile,
                                   LadderCallback on_ladder,
                                   void* context) {
        profile_requests.push_back({user_id, on_profile, on_ladder, context});
        return ServiceResult::pending;
    };
    add_room(f, 70, "Lobby", false, 3, 1, false);
    add_room(f, 75, "Other", false, 3, 1, true);
    chat_on_control(&f.console, ChatControl::room_list, kListActivate, 1);
    CHECK(f.r.joins.empty());
    CHECK_STR(f.r.switches.back().c_str(), "7/75/-/7/70/Other");
    chat_on_control(&f.console, ChatControl::matchmaking_button, 1, 0);
    CHECK_STR(f.r.boxes.back().c_str(), "Matchmaking coming soon!");
    f.r.ui_calls.clear();
    chat_on_control(&f.console, ChatControl::player_list, 1, 2);
    CHECK(f.console.pane.visible && f.console.pane.user_id == 3 && !f.console.pane.sysop);
    CHECK(std::strstr(f.console.pane.html, "ACCESSING...") != nullptr);
    CHECK(f.r.panes == (std::vector<std::string>{"page"}));
    CHECK(template_requests == 1);
    // The profile request starts the Main Frame's light.
    CHECK(f.r.ui_calls == (std::vector<std::string>{"light on"}));
    chat_pane_reset(&f.console); // shown: untouched
    CHECK(std::strstr(f.console.pane.html, "ACCESSING...") != nullptr);
    // The pane holds a fresh record, so its icons follow while the reply is
    // pending.
    CHECK(f.console.pane.record == &f.console.pane.profile);
    CHECK(profile_requests.size() == 1 && profile_requests[0].user_id == 3);
    f.r.panes.clear();
    chat_handle_pane_message(&f.console, chat_message::lock_whisper, 3, nullptr);
    CHECK(f.console.whisper_lock_user == 3);
    CHECK(f.console.pane.whisper_icon_value == 4);
    CHECK(f.r.panes == (std::vector<std::string>{"fields"}));
    chat_handle_pane_message(&f.console, chat_message::lock_whisper, 3, nullptr);
    CHECK(f.console.whisper_lock_user == 0);
    CHECK(f.console.pane.whisper_icon_value == 3);
    chat_handle_pane_message(&f.console, chat_message::toggle_muzzle, 3, nullptr);
    CHECK(f.console.pane.silence_value == 4);
    // PAGE (message 300) replaces the input line and focuses it; without a
    // name it does nothing.
    const std::size_t calls = f.r.ui_calls.size();
    chat_handle_pane_message(&f.console, chat_message::go_to_room, 0, "Carol");
    CHECK_STR(f.console.input, "+Page Carol ");
    CHECK(f.r.ui_calls.size() == calls + 1 && f.r.ui_calls.back() == "input");
    chat_handle_pane_message(&f.console, chat_message::go_to_room, 0, "");
    CHECK(f.r.ui_calls.size() == calls + 1);
    f.r.ui_calls.pop_back();
    // The shown player leaving blanks the whisper icon; rejoining puts it
    // back in its first frame (whisper_icon_value is set to 3).
    chat_update_player(&f.console, PlayerListChange::left, 3, nullptr, 0, 0);
    CHECK(f.console.pane.whisper_icon_value == 0);
    chat_update_player(&f.console, PlayerListChange::joined, 3, "Carol", 1, 0x09);
    CHECK(f.console.pane.whisper_icon_value == 3);
    // Carol's reply, through the callback the request passed, fills the pane.
    by_test::ProfileReply carol{
        {"user_id", "3"},
        {"LOGINNAME", "Carol"},
        {"ICON", "9"},
        {"ALLEGIANCE", "C"},
        {"CLAN", "Red Sky"}
    };
    const ProfileFields fields = carol.fields();
    profile_requests[0].on_profile(profile_requests[0].context, true, &fields);
    // The profile reply stops it.
    CHECK(f.r.ui_calls == (std::vector<std::string>{"light on", "light off"}));
    CHECK(!f.r.panes.empty() && f.r.panes.back() == "page");
    CHECK_STR(f.console.pane.name, "Carol");
    CHECK_STR(f.console.pane.rank, "Supreme Commander");
    CHECK(f.console.pane.insignia == insignia_frame::core && f.console.pane.insignia_shown);
    CHECK_STR(f.console.pane.clan, "Red Sky");
    CHECK(f.console.pane.page_value == widget_value::enabled);
    // A player list click passes the profile request whether the row shows the SysOp icon
    // (User_Icons 0x17), not whether it is the player's own: a SysOp's
    // muzzle stays off.
    chat_on_control(&f.console, ChatControl::player_list, 1, 1);
    CHECK(f.console.pane.user_id == 2 && f.console.pane.sysop);
    by_test::ProfileReply bob{{"user_id", "2"}, {"LOGINNAME", "Bob"}, {"ICON", "23"}};
    const ProfileFields bob_fields = bob.fields();
    profile_requests.back().on_profile(profile_requests.back().context, true, &bob_fields);
    CHECK(f.console.pane.page_value == widget_value::enabled);
    CHECK(f.console.pane.silence_value == widget_value::disabled);
    chat_on_control(&f.console, ChatControl::player_list, 1, 0);
    CHECK(f.console.pane.user_id == 1 && !f.console.pane.sysop);
    chat_on_control(&f.console, ChatControl::create_button, 1, 0);
    CHECK(f.console.create_dialog.visible && !f.console.pane.visible);
    // CREATE closes the panes first, so the shown pane's close slot dropped
    // its player and record.
    CHECK(f.console.pane.user_id == 0 && f.console.pane.record == nullptr);
    // Closing the panes closes the player pane, the create dialog and the
    // frequency info, whichever are open.
    f.console.pane.visible = true;
    f.console.pane.user_id = 3;
    f.console.pane.record = &f.console.pane.profile;
    f.console.room_info_visible = true;
    chat_close_panes(&f.console);
    CHECK(
        !f.console.pane.visible && !f.console.create_dialog.visible && !f.console.room_info_visible
    );
    // The pane's close slot frees its record and clears its user id.
    CHECK(f.console.pane.user_id == 0 && f.console.pane.record == nullptr);
    f.console.room_info_visible = true;
    chat_close(&f.console);
    CHECK(!f.console.room_info_visible && !f.console.visible);
}

void test_create_pane() {
    Fixture f;
    f.console.visible = true;
    FrequencyDialog& d = f.console.create_dialog;
    // The create-frequency pane starts hidden and idle, its entries taking input
    // (value 1) and CREATE disabled (value 0).
    CHECK(!d.visible && !d.submitting && d.entry_value == 1 && d.create_value == 0);
    CHECK(d.name[0] == '\0' && d.password[0] == '\0' && d.description[0] == '\0');
    // Opening empties the hidden pane's entries when no request is pending.
    std::snprintf(d.name, sizeof d.name, "%s", "Old");
    std::snprintf(d.description, sizeof d.description, "%s", "stale");
    chat_on_control(&f.console, ChatControl::create_button, 1, 0);
    CHECK(d.visible && d.name[0] == '\0' && d.description[0] == '\0');
    chat_on_control(&f.console, ChatControl::create_button, 1, 0);
    CHECK(!d.visible);
    // ... and keeps them while one is.
    std::snprintf(d.name, sizeof d.name, "%s", "Kept");
    d.submitting = true;
    chat_on_control(&f.console, ChatControl::create_button, 1, 0);
    CHECK(d.visible && std::strcmp(d.name, "Kept") == 0);
    d.visible = false;
    d.submitting = false;

    // +create runs only the pane's open slot, so a shown player pane stays;
    // after the request the pulse disables CREATE
    // and the entries while it is pending.
    f.console.pane.visible = true;
    CHECK(f.submit("+c Den quiet"));
    CHECK(d.visible && f.console.pane.visible && d.submitting);
    CHECK_STR(d.name, "Den");
    CHECK_STR(d.description, "quiet");
    CHECK(d.create_value == 0 && d.entry_value == 0);

    // The pulse also waits on a frequency change (ChatSession.pending_room_id). Once both
    // clear, CREATE goes from 0 to 2 and the entries to 1; a CREATE face
    // other than 0 is left alone, and an empty name disables it again.
    d.submitting = false;
    f.session.pending_room_id = 71;
    chat_create_dialog_pulse(&f.console);
    CHECK(d.create_value == 0 && d.entry_value == 0);
    f.session.pending_room_id = 0;
    chat_create_dialog_pulse(&f.console);
    CHECK(d.create_value == 2 && d.entry_value == 1);
    d.create_value = 1;
    chat_create_dialog_pulse(&f.console);
    CHECK(d.create_value == 1);
    d.name[0] = '\0';
    chat_create_dialog_pulse(&f.console);
    CHECK(d.create_value == 0 && d.entry_value == 1);
}

void test_dispatch_and_population() {
    Fixture f;
    add_room(f, 70, "Lobby", false, 9, 1, true);
    CHECK(chat_find_room(&f.console, 70)->population == f.console.player_count);
    chat_update_player(&f.console, PlayerListChange::joined, 9, "Dan", 1, 3);
    CHECK(chat_find_room(&f.console, 70)->population == 5);
    chat_update_player(&f.console, PlayerListChange::left, 9, nullptr, 0, 0);
    CHECK(chat_find_room(&f.console, 70)->population == 4);
    CHECK(!chat_dispatch_message(&f.console, 17));
    CHECK(chat_dispatch_message(&f.console, 401));
    std::snprintf(f.console.input, sizeof f.console.input, "hey");
    CHECK(chat_dispatch_message(&f.console, chat_message::enter_key));
    CHECK(f.r.chats.empty()); // no focus: the line stays
    f.console.input_focused = true;
    CHECK(chat_dispatch_message(&f.console, chat_message::enter_key));
    CHECK(f.r.chats.size() == 1);
}

// Closing the console leaves the frequency and then frees the people lists, which
// writes them back.
void test_close_releases_people_lists() {
    Fixture f;
    static std::vector<std::string>* order;
    std::vector<std::string> calls;
    order = &calls;
    f.service.leave_room = [](void*, std::uint32_t area, std::uint32_t room) {
        order->push_back("leave " + std::to_string(area) + "/" + std::to_string(room));
        return ServiceResult::ok;
    };
    f.service.peoplelist_release = [](void*) { order->push_back("release"); };
    chat_close(&f.console);
    CHECK(calls.size() == 2);
    CHECK_STR(calls[0].c_str(), "leave 7/70");
    CHECK_STR(calls[1].c_str(), "release");
    CHECK(!f.console.visible);
}

void test_offline_service() {
    ChatSession session{};
    const BoneyardsService service = boneyards_offline_service(nullptr);
    ChatConsole console{};
    chat_console_init(&console, &session, &service, ChatUi{});
    chat_update_player(&console, PlayerListChange::joined, 5, "Solo", 0, 1);
    std::snprintf(console.input, sizeof console.input, "+m solo");
    CHECK(chat_submit_input(&console));
    CHECK(!chat_is_silenced(&console, 5)); // offline people list without storage stays empty
    CHECK(!service.connected(service.user));
    CHECK(service.login(service.user, nullptr) == ServiceResult::not_connected);
}

// A new frequency row puts planetary defaults (type 1) before the first row that
// is not one or whose name the word sort orders at or after the new one, user
// created frequencies (3) before the first battle debriefing (2), and
// debriefings and any other type at the end.
void test_room_order() {
    Fixture f;
    f.session.room_id = 0;

    const struct {
        std::uint32_t id;
        const char* name;
        std::int32_t type;
    } arrivals[] = {
        {1, "Tactics", room_type::planetary_default},
        {2, "Debrief_7", room_type::battle_debriefing},
        {3, "den", room_type::user_created},
        {4, "War_Room", room_type::planetary_default},
        {5, "main", room_type::planetary_default},
        {6, "Lounge", room_type::user_created},
        {7, "WarZone", room_type::planetary_default},
        {8, "Debrief_2", room_type::battle_debriefing},
        {9, "Odd", 4},
    };

    for (const auto& a : arrivals)
        add_room(f, a.id, a.name, false, 1, a.type, false);
    // The word sort puts "main" before "Tactics" and the underscore
    // before letters, where a byte compare would not.
    CHECK_STR(
        room_order(f.console).c_str(),
        "main,Tactics,War_Room,WarZone,den,Lounge,Debrief_7,Debrief_2,Odd"
    );

    // Case only breaks a tie (lower case first); a hyphen is weighed last.
    Fixture g;
    for (const char* name : {"Calm", "co-op", "calm", "coop", "Bay"})
        add_room(
            g,
            static_cast<std::uint32_t>(g.console.room_total + 1),
            name,
            false,
            1,
            room_type::planetary_default,
            false
        );
    CHECK_STR(room_order(g.console).c_str(), "Bay,calm,Calm,coop,co-op");

    // A row keeps a bounded copy of the name (0x18), description (0x80)
    // and tag (0x40); the selected row follows an insertion before it.
    g.console.selected_room = 1;
    const std::string description(0x90, 'd');
    const std::string tag(0x50, 't');
    const RoomListing longest{
        40,
        "Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        description.c_str(),
        true,
        3,
        room_type::planetary_default,
        tag.c_str()
    };
    chat_on_room_listed(&g.console, PlayerListChange::joined, &longest, false);
    const ChatRoom* row = chat_find_room(&g.console, 40);
    CHECK(row == &g.console.rooms[0] && g.console.selected_room == 2);
    CHECK(
        std::strlen(row->name) == 0x17 && std::strlen(row->description) == 0x7f &&
        std::strlen(row->tag) == 0x3f
    );
    CHECK(
        row->password_required && row->population == 3 && row->type == room_type::planetary_default
    );
    const RoomListing gone{5, "Bay", "", false, 0, room_type::planetary_default, ""};
    chat_update_room_row(&g.console, PlayerListChange::left, &gone);
    CHECK(g.console.selected_room == 1 && std::strcmp(g.console.rooms[1].name, "calm") == 0);
}

// Type 0 gets no row: it never lists, refreshes or enters.
void test_unlisted_type_ignored() {
    Fixture f;
    f.session.room_id = 0;
    chat_open_on_planet(&f.console);
    add_room(f, 30, "Hidden", false, 1, room_type::unlisted, false);
    CHECK(f.console.room_total == 0 && chat_find_room(&f.console, 30) == nullptr);
    add_room(f, 30, "Hidden", false, 5, room_type::unlisted, true);
    CHECK(f.console.room_total == 0);
    CHECK_STR(f.r.boxes.back().c_str(), "Unable to enter chat on this planet. Please try another.");
    CHECK(f.r.joins.empty());
}

// The light runs from the opening to the listing's last row, again from the
// default entry to the next listing's end, and from a go-to-room; each last
// row also scrolls the current frequency into view.
void test_activity_light() {
    Fixture f;
    f.session.room_id = 0;
    chat_open_on_planet(&f.console);
    add_room(f, 12, "Private", false, 0, room_type::user_created, false);
    add_room(f, 13, "Calm", false, 2, room_type::planetary_default, true);
    chat_on_join_result(&f.console, JoinResult::entered);
    add_room(f, 12, "Private", false, 0, room_type::user_created, false);
    add_room(f, 13, "Calm", false, 2, room_type::planetary_default, true);
    chat_go_to_room(&f.console, 7, 12, nullptr, "Private");
    const std::vector<std::string> expected{
        "light on",
        "rooms",
        "light off",
        "light on",
        "join 13",
        "rooms",
        "light off",
        "show 0",
        "light on",
        "switch 12",
    };
    CHECK(f.r.ui_calls == expected);
    // A pending entry or the current frequency sends nothing and leaves the
    // light alone.
    f.r.ui_calls.clear();
    CHECK(!chat_go_to_room(&f.console, 7, 13, nullptr, "Calm"));
    f.session.pending_room_id = 0;
    CHECK(!chat_go_to_room(&f.console, 7, 13, nullptr, "Calm"));
    CHECK(f.r.ui_calls.empty());

    // The preferred-room entry starts it too.
    f.session.room_id = 0;
    chat_open_on_planet(&f.console);
    CHECK(f.r.ui_calls == (std::vector<std::string>{"light on", "join 13"}));
}

// A default entry collects, in arrival order, the planetary defaults with fewer
// than 0x1F occupants and enters the one the random draw picks modulo their
// count; with none it reports the planet as unavailable.
void test_default_pick() {
    Fixture f;
    f.session.room_id = 0;
    f.r.rolls = {5, 4};
    chat_open_on_planet(&f.console);
    add_room(f, 21, "Zeta", false, 0x1e, room_type::planetary_default, false);
    add_room(f, 22, "Full", false, 0x1f, room_type::planetary_default, false);
    add_room(f, 23, "Alpha", false, 0, room_type::planetary_default, false);
    add_room(f, 24, "Made", false, 0, room_type::user_created, false);
    add_room(f, 25, "Mid", false, 3, room_type::planetary_default, true);
    // Candidates 21, 23, 25: draw 5 % 3 picks 25.
    CHECK_STR(f.r.joins.back().c_str(), "7/25/-/Mid");
    CHECK(f.session.pending_room_id == 25 && f.session.pending_area_id == 7);
    CHECK_STR(f.session.pending_room_name, "Mid");
    chat_on_join_result(&f.console, JoinResult::failed);
    CHECK(f.console.enter_default_room);
    // The next listing starts a new collection: draw 4 % 3 picks 23.
    add_room(f, 21, "Zeta", false, 0x1e, room_type::planetary_default, false);
    add_room(f, 22, "Full", false, 0x1f, room_type::planetary_default, false);
    add_room(f, 23, "Alpha", false, 0, room_type::planetary_default, false);
    add_room(f, 25, "Mid", false, 3, room_type::planetary_default, true);
    CHECK_STR(f.r.joins.back().c_str(), "7/23/-/Alpha");
    CHECK(f.r.rolls.empty());

    // Only full or non-default frequencies: the box, no entry.
    Fixture g;
    g.session.room_id = 0;
    chat_open_on_planet(&g.console);
    add_room(g, 22, "Full", false, 0x1f, room_type::planetary_default, false);
    add_room(g, 24, "Made", false, 0, room_type::user_created, true);
    CHECK(g.r.joins.empty() && g.r.boxes.size() == 1);
    CHECK_STR(g.r.boxes.back().c_str(), "Unable to enter chat on this planet. Please try another.");
    CHECK(g.r.ui_calls.back() == "light off");
}

// The creation reply ends the dialog's wait; zero ids report the failure, otherwise
// the dialog posts message 300 with its password, which switches
// to the new frequency from the current one.
void test_room_created() {
    Fixture f;
    CHECK(f.submit("+cp Den sesame a quiet corner"));
    CHECK(f.console.create_dialog.submitting);
    chat_on_room_created(&f.console, 0, 80, "Den");
    CHECK(!f.console.create_dialog.submitting);
    CHECK_STR(f.r.boxes.back().c_str(), "Unable to Create New Frequency");
    CHECK(f.r.switches.empty());
    f.console.create_dialog.submitting = true;
    chat_on_room_created(&f.console, 7, 0, "Den");
    CHECK(!f.console.create_dialog.submitting && f.r.boxes.size() == 2 && f.r.switches.empty());

    f.console.create_dialog.submitting = true;
    f.r.ui_calls.clear();
    chat_on_room_created(&f.console, 7, 80, "Den");
    CHECK(!f.console.create_dialog.submitting && f.r.boxes.size() == 2);
    CHECK_STR(f.r.switches.back().c_str(), "7/80/sesame/7/70/Den");
    CHECK(f.r.ui_calls == (std::vector<std::string>{"light on", "switch 80"}));
    CHECK(f.session.pending_room_id == 80);
    CHECK_STR(f.session.pending_room_name, "Den");
    // A second reply while that entry is pending only ends the wait.
    f.console.create_dialog.submitting = true;
    chat_on_room_created(&f.console, 7, 81, "Other");
    CHECK(!f.console.create_dialog.submitting && f.r.switches.size() == 1);

    // The navigator forwards the reply while the console exists.
    Recorder r;
    BoneyardsService service{};
    service.user = &r;
    service.switch_room = svc_switch;
    service.create_room = [](void*, std::uint32_t, const char*, const char*, const char*) {
        return ServiceResult::pending;
    };
    auto* nav = new BoneyardsScreens{};
    boneyards_screens_init(nav, &service, SettingsStore{});
    static int lights;
    lights = 0;
    nav->host = ScreensHost{nullptr, [](void*, bool running) { lights += running ? 1 : -1; }};
    nav->session.area_id = 7;
    boneyards_open(nav, ByScreen::chat_frequencies);
    chat_open_on_planet(&nav->chat);
    nav->session.room_id = 5;
    chat_create_room(&nav->chat, "Den", "", "pw");
    boneyards_on_room_created(nav, 7, 80, "Den");
    CHECK_STR(r.switches.back().c_str(), "7/80/pw/7/5/Den");
    CHECK(lights == 2 && !nav->chat.create_dialog.submitting);
    delete nav;
}

// The room feeds reach the chat console through the navigator only while
// the console exists: from the Chat Frequencies screen's opening until
// another screen replaces it, as in 3.1c.
std::vector<std::string> g_nav_lines;

void test_room_feeds_through_navigator() {
    Recorder r;
    BoneyardsService service{};
    service.user = &r;
    service.join_room = svc_join;
    service.switch_room = svc_switch;
    service.request_room_list = svc_rooms;
    auto* nav = new BoneyardsScreens{};
    boneyards_screens_init(nav, &service, SettingsStore{});
    nav->chat.ui.print_line = [](void*, const char* text, std::int32_t, bool) {
        g_nav_lines.emplace_back(text);
    };
    const ServiceEvents events = boneyards_screens_events(nav);
    nav->session.area_id = 7;
    nav->session.room_id = 5;

    const RoomListing calm{13, "Calm", "quiet", false, 2, room_type::planetary_default, ""};
    boneyards_on_room_listed(nav, 0, &calm, true);
    boneyards_on_room_member(nav, 0, 9, "Dan", 2, 3);
    boneyards_on_chat_message(nav, 9, "hi");
    boneyards_on_room_entered(nav, 0);
    boneyards_on_room_left(nav, false);
    CHECK(nav->chat.room_total == 0 && nav->chat.player_total == 0);
    CHECK(g_nav_lines.empty() && r.joins.empty() && r.room_list_requests == 0);
    CHECK(nav->session.room_id == 5);

    // Making the screen current asks for nothing; opening the console with
    // no preferred room asks for the list, whose last row picks a default,
    // entered through slot 10.
    boneyards_open(nav, ByScreen::chat_frequencies);
    CHECK(r.room_list_requests == 0);
    chat_open_on_planet(&nav->chat);
    CHECK(r.room_list_requests == 1 && nav->chat.enter_default_room);
    events.room_listed(
        nav,
        0,
        7,
        13,
        "Calm",
        "quiet",
        false,
        2,
        room_type::planetary_default,
        "Ann",
        true,
        false,
        0
    );
    CHECK(nav->chat.room_total == 1 && nav->chat.rooms[0].room_id == 13);
    CHECK_STR(nav->chat.rooms[0].name, "Calm");
    CHECK_STR(nav->chat.rooms[0].description, "quiet");
    CHECK_STR(nav->chat.rooms[0].tag, "Ann");
    CHECK(nav->chat.rooms[0].type == room_type::planetary_default);
    CHECK_STR(r.joins.back().c_str(), "7/13/-/Calm");
    CHECK(r.switches.empty());
    events.room_entered(nav, 0);
    CHECK_STR(g_nav_lines.back().c_str(), "[Entered 'Calm']");
    CHECK(nav->session.room_id == 13 && r.room_list_requests == 2);

    events.room_member(nav, 0, 9, "Dan", 2, 3, 12, "C");
    CHECK(nav->chat.player_total == 1 && nav->chat.players[0].user_id == 9);
    CHECK_STR(nav->chat.players[0].name, "Dan");
    CHECK(nav->chat.players[0].latency_icon == 2 && nav->chat.players[0].user_icon == 3);
    CHECK(nav->chat.players[0].rank == 12);
    CHECK_STR(nav->chat.players[0].allegiance, "C");
    const std::size_t entered = g_nav_lines.size();
    events.chat_message(nav, true, 0, "Welcome to Slag, commander.");
    CHECK(g_nav_lines.size() == entered);
    events.chat_message(nav, false, 9, "hi");
    CHECK_STR(g_nav_lines.back().c_str(), "Dan: hi");
    events.room_updated(nav, 0, 7, 14, "Other", "", true, 1, 2, "");
    CHECK(nav->chat.room_total == 1 && nav->chat.rooms[0].password_required);
    events.room_updated(nav, 1, 7, 14, "Other", "", true, 1, 2, "");
    CHECK(nav->chat.room_total == 0);
    events.room_member(nav, 1, 9, "Dan", 2, 3, 12, "C");
    CHECK(nav->chat.player_total == 0);

    // A failed switch reports itself and asks for the list again.
    chat_go_to_room(&nav->chat, 7, 15, nullptr, "Far");
    CHECK_STR(r.switches.back().c_str(), "7/15/-/7/13/Far");
    events.room_entered(nav, 2);
    CHECK_STR(g_nav_lines.back().c_str(), "[Entry to 'Far' failed]");
    CHECK(nav->session.room_id == 13 && nav->session.pending_room_id == 0);

    // Closing leaves the room; its reply still reaches the console.
    chat_close(&nav->chat);
    events.room_left(nav, false);
    CHECK(nav->session.room_id == 0);

    // Another screen replaces the console: the feeds stop.
    boneyards_open(nav, ByScreen::battle_list);
    nav->session.room_id = 13;
    events.room_member(nav, 0, 10, "Eve", 1, 3, 0, "A");
    events.room_listed(
        nav, 0, 7, 13, "Calm", "quiet", false, 2, room_type::planetary_default, "", true, false, 0
    );
    events.room_left(nav, false);
    const std::size_t lines = g_nav_lines.size();
    events.chat_message(nav, false, 10, "late");
    CHECK(nav->chat.player_total == 0 && nav->chat.room_total == 0);
    CHECK(nav->session.room_id == 13 && g_nav_lines.size() == lines);
    delete nav;
}

// The list rows follow the chat as the room and member feeds change them. A
// frequency row is made where its type sorts, with its listed count, or
// the members counted so far for the current frequency; each new
// member adds one to that count and gets a row with its rank
// and latency icons, muzzled from the Silenced list or the guest set and
// green while whispers are locked onto it. Icons outside 1..0x24 get no
// row. The muzzle and lock commands change the rows' flags; a member who
// leaves or a frequency listed as gone loses its row; an entry and the
// close empty the lists.
void test_list_rows() {
    Fixture f;
    const std::vector<std::string> fixture_rows{
        "player+0 Me 1/0",
        "player+1 Bob 23/1",
        "player+2 Carol 9/1",
        "player+3 Guest 2/1",
    };
    CHECK(f.r.rows == fixture_rows);
    f.r.rows.clear();
    add_room(f, 70, "Lobby", false, 9, room_type::planetary_default, false);
    add_room(f, 71, "Arena", true, 2, room_type::planetary_default, false);
    add_room(f, 72, "Debrief", false, 5, room_type::battle_debriefing, true);
    chat_update_player(&f.console, PlayerListChange::joined, 9, "Dan", 1, 3);
    chat_update_player(&f.console, PlayerListChange::joined, 10, "Nobody", 1, 0x25);
    chat_update_player(&f.console, PlayerListChange::joined, 11, "Blank", 1, 0);
    chat_update_player(&f.console, PlayerListChange::joined, 9, "Dan", 2, 4);
    CHECK(f.console.player_total == 5 && f.console.player_count == 5);
    CHECK(
        f.r.rows == (std::vector<std::string>{
                        "room+0 Lobby 4",
                        "room+0 Arena 2",
                        "room+2 Debrief 5",
                        "room=1 Lobby 5",
                        "player+4 Dan 3/1",
                        "player=4 Dan 4/2",
                    })
    );

    f.r.rows.clear();
    f.submit("+m dan");
    f.submit("+wl dan");
    f.submit("+wl bob");
    f.submit("-wl");
    f.submit("-m dan");
    CHECK(
        f.r.rows == (std::vector<std::string>{
                        "player=4 Dan 4/2 m",
                        "player=4 Dan 4/2 m l",
                        "player=4 Dan 4/2 m",
                        "player=1 Bob 23/1 l",
                        "player=1 Bob 23/1",
                        "player=4 Dan 4/2",
                    })
    );

    // A guest muzzled before joining and the locked player joining again
    // after an entry come in with their flags set.
    f.r.rows.clear();
    chat_toggle_silence(&f.console, 0x80000007u);
    f.submit("+wl carol");
    const RoomListing arena{71, "Arena", "", true, 3, room_type::planetary_default, ""};
    chat_update_room_row(&f.console, PlayerListChange::left, &arena);
    chat_update_player(&f.console, PlayerListChange::left, 2, nullptr, 0, 0);
    f.session.pending_room_id = 72;
    chat_on_join_result(&f.console, JoinResult::entered);
    chat_update_player(&f.console, PlayerListChange::joined, 0x80000007u, "Shade", 1, 2);
    chat_update_player(&f.console, PlayerListChange::joined, 3, "Carol", 1, 9);
    CHECK(
        f.r.rows == (std::vector<std::string>{
                        "player=2 Carol 9/1 l",
                        "room-0 Arena",
                        "player-1 Bob",
                        "room=0 Lobby 4",
                        "players cleared",
                        "rooms cleared",
                        "player+0 Shade 2/1 m",
                        "player+1 Carol 9/1 l",
                    })
    );

    f.r.rows.clear();
    // The pane's closing notice (0xC9) drops the player list's selection
    // (the player list's selected row is cleared).
    chat_handle_pane_message(&f.console, chat_message::clear_selection, 0, nullptr);
    chat_close(&f.console);
    CHECK(
        f.r.rows ==
        (std::vector<std::string>{"select player -1", "players cleared", "rooms cleared"})
    );
    CHECK(f.console.player_total == 0 && f.console.room_total == 0);
}

} // namespace

int main() {
    test_plain_chat_and_empty_line();
    test_help_summary();
    test_whisper();
    test_whisper_lock();
    test_muzzle();
    test_receive_colours();
    test_server_line_rank_and_sysop();
    test_page_and_locate();
    test_goto_and_create();
    test_join_results_and_default_room();
    test_controls_and_pane();
    test_create_pane();
    test_dispatch_and_population();
    test_close_releases_people_lists();
    test_offline_service();
    test_room_feeds_through_navigator();
    test_room_order();
    test_unlisted_type_ignored();
    test_activity_light();
    test_default_pick();
    test_room_created();
    test_list_rows();
    return by_test::finish("boneyards chat");
}
