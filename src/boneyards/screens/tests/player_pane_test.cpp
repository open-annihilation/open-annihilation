// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/player_pane.hpp"

#include "check.hpp"
#include "profile_reply.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace oa::boneyards;
using namespace oa::boneyards::screens;

namespace {

std::vector<std::string> g_lines;
std::vector<by_test::ProfileRequestSeen> g_profile_requests;

void print_line(void*, const char* text, int32_t, bool) {
    g_lines.emplace_back(text);
}

// The Main Frame's activity light as the pane starts and stops it.
std::vector<bool> g_lights;

void activity(void*, bool running) {
    g_lights.push_back(running);
}

// The pane's change notices to its view: true when it has a new page.
std::vector<bool> g_pane_pages;

void pane_changed(void*, bool page) {
    g_pane_pages.push_back(page);
}

ServiceResult request_profile(
    void*, uint32_t user_id, ProfileCallback on_profile, LadderCallback on_ladder, void* context
) {
    g_profile_requests.push_back({user_id, on_profile, on_ladder, context});
    return ServiceResult::pending;
}

// Profile.def's pop-up template and a Duel ladder template (type 1, subtype 0).
char g_popup[] = "<html>general <$TAG|no tag$></html>";
char g_duel[] = "<b>Duel</b> rank <$RANK|Below 500$>";
LadderTemplate g_duel_ladder{1, 0, 3, g_duel};
LadderTemplate* g_ladders[] = {&g_duel_ladder};

const ProfileTemplates* templates(void*) {
    static ProfileTemplates t{};
    t.loaded = true;
    t.popup_page = g_popup;
    t.ladders = g_ladders;
    t.ladder_count = 1;
    t.ladder_capacity = 1;
    return &t;
}

struct Fixture {
    ChatSession session{};
    OfflineServiceState people{};
    BoneyardsService service = boneyards_offline_service(&people);
    ChatConsole console{};

    Fixture() {
        session.self_user_id = 7;
        session.arm_font = 252;
        session.core_font = 253;
        service.request_profile = request_profile;
        service.profile_templates = templates;
        ChatUi ui{};
        ui.print_line = print_line;
        ui.activity = activity;
        ui.pane_changed = pane_changed;
        g_lights.clear();
        g_pane_pages.clear();
        chat_console_init(&console, &session, &service, ui);
        chat_update_player(&console, PlayerListChange::joined, 7, "Me", 1, 0x13);
        chat_update_player(&console, PlayerListChange::joined, 42, "Sentinel", 2, 0x12);
        chat_update_player(&console, PlayerListChange::joined, 43, "Drone", 1, 0x19);
    }
};

// Sentinel's profile_info reply: an Arm Supreme Commander of Iron Legion.
by_test::ProfileReply sentinel() {
    return {
        {"user_id", "42"},
        {"LOGINNAME", "Sentinel"},
        {"ICON", "18"},
        {"ALLEGIANCE", "Arm"},
        {"CLAN", "Iron Legion"},
        {"TAG", "Never surrender"},
    };
}

void test_build() {
    Fixture f;
    const PlayerPane* p = &f.console.pane;
    CHECK(!p->visible && p->user_id == 0 && p->record == nullptr);
    CHECK(p->tab == static_cast<int32_t>(PaneTab::general));
    CHECK(p->page_value == widget_value::enabled);
    CHECK(!p->user_icon_shown && !p->insignia_shown);
    for (bool highlighted : p->tab_highlighted)
        CHECK(!highlighted);
}

void test_open_and_render() {
    Fixture f;
    ChatConsole* c = &f.console;
    CHECK(c->player_total == 3);
    CHECK_STR(c->players[1].name, "Sentinel");
    CHECK(c->players[1].user_icon == 0x12 && c->players[1].latency_icon == 2);

    // Selecting a row opens the pane on a fresh record, controls disabled,
    // and requests that player's profile (slot 63 with the user and the
    // pane's profile and ladder callbacks).
    g_profile_requests.clear();
    chat_on_control(c, ChatControl::player_list, kListActivate, 1);
    CHECK(c->pane.visible && c->pane.user_id == 42 && !c->pane.sysop);
    CHECK(c->pane.record == &c->pane.profile && c->pane.profile.allegiance == 'U');
    CHECK(c->pane.tab == 0 && c->pane.page_value == widget_value::disabled);
    for (int32_t value : c->pane.tab_values)
        CHECK(value == widget_value::disabled);
    CHECK(!c->pane.user_icon_shown && !c->pane.insignia_shown);
    CHECK(g_profile_requests.size() == 1);
    const by_test::ProfileRequestSeen request = g_profile_requests.at(0);
    CHECK(request.user_id == 42 && request.context == c);
    CHECK(request.on_profile == chat_pane_on_profile && request.on_ladder == chat_pane_on_ladder);
    CHECK(std::strstr(c->pane.html, "ACCESSING") != nullptr);
    // The Main Frame's light runs while the request is out.
    CHECK(g_lights == (std::vector<bool>{true}));

    // The reply's ladder standing, then its profile, fill the pane; the
    // profile stops the light, the ladder standing does not.
    by_test::ProfileReply duel{{"user_id", "42"}, {"RANK", "12"}};
    const ProfileFields duel_fields = duel.fields();
    request.on_ladder(request.context, 1, 0, &duel_fields);
    CHECK(g_lights == (std::vector<bool>{true}));
    by_test::ProfileReply reply = sentinel();
    const ProfileFields fields = reply.fields();
    request.on_profile(request.context, true, &fields);
    CHECK(g_lights == (std::vector<bool>{true, false}));
    CHECK(c->pane.record == &c->pane.profile && c->pane.profile.user_id == 42);
    CHECK(c->pane.user_icon == 0x12 && c->pane.user_icon_shown);
    CHECK_STR(c->pane.name, "Sentinel");
    CHECK_STR(c->pane.rank, "Supreme Commander");
    CHECK(c->pane.name_color == SideColor::arm);
    CHECK(c->pane.insignia == insignia_frame::arm && c->pane.insignia_shown);
    CHECK_STR(c->pane.clan, "Iron Legion");
    CHECK_STR(c->pane.html, "<html>general Never surrender</html>");
    CHECK(c->pane.tab_highlighted[0] && !c->pane.tab_highlighted[1] && !c->pane.tab_highlighted[2]);
    CHECK(c->pane.page_value == widget_value::enabled);
    CHECK(c->pane.whisper_icon_value == widget_value::icon_off);
    CHECK(c->pane.silence_value == widget_value::icon_off);
    for (int32_t value : c->pane.tab_values)
        CHECK(value == widget_value::enabled);

    // The view hears of the rendered page last.
    CHECK(!g_pane_pages.empty() && g_pane_pages.back());

    // Tab buttons switch the page and the label highlight; the awards page
    // falls back to the default report.
    g_pane_pages.clear();
    CHECK(chat_pane_on_click(c, PaneControl::ladder_tab));
    CHECK(g_pane_pages == (std::vector<bool>{true}));
    CHECK(c->pane.tab == 1 && c->pane.tab_highlighted[1] && !c->pane.tab_highlighted[0]);
    CHECK_STR(c->pane.html, "<html><BODY><b>Duel</b> rank 12<P></BODY></html>");
    CHECK(!chat_pane_on_click(c, PaneControl::ladder_tab));
    CHECK(g_pane_pages.size() == 1);
    CHECK(chat_pane_on_click(c, PaneControl::awards_tab));
    CHECK_STR(c->pane.html, kDefaultReportHtml);

    // PAGE, the whisper icon and the muzzle icon post to the console.
    CHECK(chat_pane_on_click(c, PaneControl::page_button));
    CHECK_STR(c->input, "+Page Sentinel ");
    CHECK(chat_pane_on_click(c, PaneControl::whisper_icon));
    CHECK(c->whisper_lock_user == 42 && c->pane.whisper_icon_value == widget_value::icon_on);
    CHECK(chat_pane_on_click(c, PaneControl::silence_icon));
    CHECK(chat_is_silenced(c, 42) && c->pane.silence_value == widget_value::icon_on);

    // A re-render keeps the Awards page and the console's icon states.
    chat_pane_render(c, true);
    CHECK(c->pane.silence_value == widget_value::icon_on);
    CHECK(c->pane.whisper_icon_value == widget_value::icon_on);
    CHECK_STR(c->pane.html, kDefaultReportHtml);

    // A failed reply shows "No Data", keeps the held record and drops the
    // whisper lock on the player.
    by_test::ProfileReply empty;
    const ProfileFields no_fields = empty.fields();
    chat_pane_on_profile(c, false, &no_fields);
    CHECK_STR(c->pane.html, "<html><BODY>No Data<P></BODY></html>");
    CHECK(c->pane.record == &c->pane.profile);
    CHECK_STR(c->pane.profile.name, "Sentinel");
    CHECK(c->whisper_lock_user == 0);

    // Another player blanks the pane, keeps the page choice and replaces the
    // record with a fresh one. The clan label keeps the last player's clan
    // until the reply.
    chat_on_control(c, ChatControl::player_list, kListActivate, 2);
    CHECK(c->pane.user_id == 43 && c->pane.record == &c->pane.profile);
    CHECK(c->pane.profile.name[0] == '\0' && c->pane.profile.ladder_page_count == 0);
    CHECK(c->pane.profile.ladder_html == nullptr && c->pane.profile.clan == nullptr);
    CHECK_STR(c->pane.clan, "Iron Legion");
    CHECK(g_profile_requests.size() == 2 && g_profile_requests[1].user_id == 43);
    CHECK(c->pane.name[0] == '\0' && !c->pane.insignia_shown && !c->pane.user_icon_shown);
    CHECK(c->pane.tab == 2 && !c->pane.tab_highlighted[2]);
    CHECK(c->pane.page_value == widget_value::disabled);
    CHECK(c->pane.silence_value == widget_value::disabled);

    // Hidden, the pane blanks; reopened, it is the same pane.
    chat_on_control(c, ChatControl::create_button, 1, 0);
    CHECK(!c->pane.visible);
    chat_pane_reset(c);
    CHECK(c->pane.html == kPaneEmptyPage);
    chat_on_control(c, ChatControl::player_list, kListActivate, 1);
    CHECK(c->pane.visible && c->pane.tab == 2);
}

void test_self_and_core() {
    Fixture f;
    ChatConsole* c = &f.console;
    chat_on_control(c, ChatControl::player_list, kListActivate, 0);
    CHECK(!c->pane.sysop && c->pane.user_id == 7);
    by_test::ProfileReply me{
        {"user_id", "7"}, {"LOGINNAME", "Me"}, {"ICON", "0"}, {"ALLEGIANCE", "CORE"}
    };
    const ProfileFields fields = me.fields();
    chat_pane_on_profile(c, true, &fields);
    CHECK(!c->pane.user_icon_shown);
    CHECK(c->pane.name_color == SideColor::core);
    CHECK(c->pane.insignia == insignia_frame::core && c->pane.insignia_shown);
    CHECK_STR(c->pane.clan, "");
    CHECK_STR(c->pane.rank, "");
    CHECK(c->pane.page_value == widget_value::disabled); // own profile: no PAGE
    CHECK(c->pane.silence_value == widget_value::disabled);

    // Without a side the pane draws in its own default colour, no insignia.
    c->pane.profile.allegiance = 'U';
    chat_pane_render(c, true);
    CHECK(c->pane.name_color == SideColor::no_side_pane && !c->pane.insignia_shown);

    // Closing the pane frees its record, and its page text no longer points
    // into it.
    CHECK(c->pane.html != nullptr && std::strstr(c->pane.html, "general") != nullptr);
    chat_close_panes(c);
    CHECK(c->pane.record == nullptr && c->pane.profile.name[0] == '\0');
    CHECK(c->pane.html == kPaneEmptyPage);
    CHECK_STR(c->pane.html, "<html><BODY><P></BODY></html>");
}

// A reply for a pane that holds no record (it closed while the request was
// out) adds no ladder page and shows "No Data"; the pane window still
// exists, so the light stops.
void test_reply_after_close() {
    Fixture f;
    ChatConsole* c = &f.console;
    chat_on_control(c, ChatControl::player_list, kListActivate, 1);
    chat_close_panes(c);
    by_test::ProfileReply duel{{"user_id", "42"}, {"RANK", "3"}};
    const ProfileFields duel_fields = duel.fields();
    chat_pane_on_ladder(c, 1, 0, &duel_fields);
    CHECK(c->pane.profile.ladder_page_count == 0);
    by_test::ProfileReply reply = sentinel();
    const ProfileFields fields = reply.fields();
    chat_pane_on_profile(c, true, &fields);
    CHECK(c->pane.profile.name[0] == '\0');
    CHECK_STR(c->pane.html, "<html><BODY>No Data<P></BODY></html>");
    CHECK(g_lights == (std::vector<bool>{true, false}));
}

} // namespace

int main() {
    test_build();
    test_open_and_render();
    test_self_and_core();
    test_reply_after_close();
    return by_test::finish("boneyards-player-pane");
}
