// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/profile.hpp"

#include "check.hpp"
#include "oa/boneyards/screens/chat.hpp"
#include "profile_reply.hpp"

#include <cstring>
#include <map>
#include <string>

using namespace oa::boneyards;
using namespace oa::boneyards::screens;

namespace {

struct Store {
    std::map<std::string, int32_t> values;
};

bool save_number(void* user, const char* key, int32_t value) {
    static_cast<Store*>(user)->values[key] = value;
    return true;
}

int32_t read_number(void* user, const char* key, int32_t fallback) {
    const auto& values = static_cast<Store*>(user)->values;
    const auto it = values.find(key);
    return it == values.end() ? fallback : it->second;
}

void box(void*, const char*) {
}

// Profile.def's tables as the service reads them (a category description,
// the Boneyards-info template and a Duel ladder template); counts the
// requests.
char g_personal_description[] = "Personal details";
char g_byinfo[] = "<html>info <$BONETIME|0$></html>";
char g_duel_page[] = "Duel rank <$RANK|Below 500$>";
LadderTemplate g_duel{1, 0, 3, g_duel_page};
LadderTemplate* g_ladders[] = {&g_duel};
ProfileTemplates g_templates{};
int g_template_requests = 0;

const ProfileTemplates* read_templates(void*) {
    ++g_template_requests;
    g_templates.loaded = true;
    g_templates.category_descriptions[1] = g_personal_description;
    g_templates.byinfo_page = g_byinfo;
    g_templates.ladders = g_ladders;
    g_templates.ladder_count = 1;
    g_templates.ladder_capacity = 1;
    return &g_templates;
}

// Sentinel's profile_info reply: an Arm Supreme Commander and beta tester.
by_test::ProfileReply member_reply() {
    return {
        {"user_id", "42"},
        {"LOGINNAME", "Sentinel"},
        {"ICON", "18"},
        {"ALLEGIANCE", "Arm"},
        {"USER_STATUS", "5"},
        {"EMAIL", "s@example.com"},
        {"TAG", "Never surrender"},
        {"HAS_CC", "Yes"},
        {"HAS_BT", "No"},
        {"TAFORUM_ALIAS", "sentinel"},
        {"EMAIL_ALIAS", "sentinel@forum"},
        {"ICQ_ALIAS", "12345"},
        {"PLAYERSKILL", "3"},
        {"NUMPLAYERS", "3"},
        {"ONCOMDEATH", "2"},
        {"FAVORITEMAP", "Coast to Coast"},
        {"BONETIME", "4000"},
    };
}

void receive(ProfileConsole* c, by_test::ProfileReply reply) {
    const ProfileFields fields = reply.fields();
    profile_on_received(c, true, &fields);
}

ChatSession g_session{};

void test_status_and_ranks() {
    ProfileRecord r{};
    r.user_id = -1;
    CHECK_STR(profile_status_label(&r), "Guest");
    r.user_id = 1;
    r.status = 1;
    CHECK_STR(profile_status_label(&r), "Active");
    r.status = 4;
    CHECK_STR(profile_status_label(&r), "Admin");
    r.status = 5;
    CHECK_STR(profile_status_label(&r), "Beta Tester");
    r.status = 2;
    CHECK_STR(profile_status_label(&r), "Unknown");

    // A record awaiting its reply has no side and reads as Active.
    profile_record_init(&r);
    CHECK(r.allegiance == 'U' && r.user_id == 0 && r.name[0] == '\0' && r.clan == nullptr);
    CHECK_STR(profile_status_label(&r), "Active");

    CHECK_STR(rank_title(1, false), "Core Private");
    CHECK_STR(rank_title(1, true), "Private");
    CHECK_STR(rank_title(9, false), "Core Supreme Commander");
    CHECK_STR(rank_title(10, false), "Arm Private");
    CHECK_STR(rank_title(0x12, true), "Supreme Commander");
    CHECK_STR(rank_title(0x1e, false), "Core Supreme Commander"); // repeated officer range
    CHECK_STR(rank_title(0x1f, false), "Arm Lieutenant");
    CHECK_STR(rank_title(0x24, false), "Arm Supreme Commander");
    CHECK_STR(rank_title(0x15, false), "Newsbot");
    CHECK_STR(rank_title(0x16, true), "System Commander");
    CHECK_STR(rank_title(0x17, false), "Yard Dog");
    CHECK_STR(rank_title(0x18, false), "Core System Commander");
    CHECK_STR(rank_title(0x13, false), "");
    CHECK_STR(rank_title(0, true), "");
}

void test_member_tabs() {
    Store store;
    store.values["UnitLimit"] = 300;
    store.values["BYStopThatIncessantDroning"] = 1;
    BoneyardsService service = boneyards_offline_service(nullptr);
    service.profile_templates = read_templates;
    static ProfileConsole c;
    profile_console_init(
        &c,
        &service,
        &g_session,
        SettingsStore{&store, save_number, read_number},
        nullptr,
        box,
        nullptr
    );
    // The console's record and the one it starts receiving each read the tables.
    CHECK(c.templates == &g_templates && g_template_requests == 2);
    CHECK_STR(c.tab_description, "");

    // The account tab opens on a record in its defaults and hides while
    // receiving.
    CHECK(c.tab_created[0] && c.current_tab == 0 && c.tab_list_row == 0 && !c.tab_visible[0]);
    CHECK(c.record == &c.profile && c.profile.allegiance == 'U');
    CHECK_STR(c.tabs.login_name, "");
    CHECK_STR(c.tabs.status, "Active");
    CHECK(c.tabs.change_value == widget_value::enabled && !c.tabs.change_shown);
    CHECK(c.tabs.password.mask == '*' && c.tabs.confirm.mask == '*');
    CHECK(c.tabs.password.max_length == 0xf && !c.tabs.password.shown && !c.tabs.confirm.shown);

    // The Duel standing arrives before the profile and becomes its ladder page.
    by_test::ProfileReply duel{{"user_id", "42"}, {"RANK", "12"}};
    const ProfileFields duel_fields = duel.fields();
    profile_on_ladder(&c, 1, 0, &duel_fields);
    CHECK(c.profile.ladder_page_count == 1);
    receive(&c, member_reply());
    CHECK(c.tab_visible[0]);
    CHECK_STR(c.tabs.login_name, "Sentinel");
    CHECK_STR(c.tabs.status, "Beta Tester");
    CHECK_STR(c.tabs.email, "s@example.com");
    CHECK(c.tabs.change_shown && !c.password_fields_shown);

    // CHANGE swaps itself for the password fields.
    profile_on_control(&c, ProfileControl::change_password_button, 0);
    CHECK(c.password_fields_shown && c.tabs.password.shown && c.tabs.confirm.shown);
    CHECK(!c.tabs.change_shown);

    profile_select_tab(&c, 1);
    CHECK_STR(c.tab_description, "Personal details");
    CHECK(c.tab_list_row == 1);
    CHECK(!c.tabs.password.shown && !c.tabs.confirm.shown && !c.tabs.change_shown);
    CHECK_STR(c.tabs.tag_line.text, "Never surrender");
    CHECK(c.tabs.tag_line.value == kEntryInitialValue && c.tabs.tag_line.mask == 0);
    CHECK(c.tabs.tag_line.max_length == 0xff && c.tabs.tag_line.shown);
    CHECK(c.tabs.icq.max_length == 0xd && c.tabs.forum_alias.max_length == 0xf);
    CHECK(c.tabs.core_contingency_value == widget_value::icon_on);
    CHECK(c.tabs.battle_tactics_value == widget_value::icon_off);
    CHECK_STR(c.tabs.forum_alias.text, "sentinel");
    CHECK_STR(c.tabs.icq.text, "12345");
    CHECK(c.tabs.display_email_alias_value == widget_value::icon_on); // an email alias is set

    profile_select_tab(&c, 2);
    CHECK(!c.tabs.tag_line.shown);
    CHECK(c.tabs.user_icon == 0x12);
    CHECK_STR(c.tabs.user_name, "Sentinel");
    CHECK(c.tabs.name_color == SideColor::arm && c.tabs.logo_frame == insignia_frame::arm);
    CHECK_STR(c.tabs.side_name, "Arm");
    CHECK_STR(c.tabs.rank, "Supreme Commander");
    CHECK(c.tabs.logo_shown && c.tabs.side_shown && c.tabs.rank_shown);
    CHECK(!c.tabs.join_shown && !c.tabs.radios_shown);
    CHECK_STR(c.tabs.allegiance_html, "<html>info 4000</html>");

    profile_select_tab(&c, 3);
    CHECK(!c.tabs.logo_shown && !c.tabs.side_shown && !c.tabs.rank_shown);
    CHECK(
        c.tabs.skill.count == 4 && c.tabs.players.count == 4 && c.tabs.commander_death.count == 4
    );
    CHECK(c.tabs.skill.selected == 2);
    CHECK(c.tabs.players.selected == 1);
    CHECK(c.tabs.commander_death.selected == 2);
    CHECK_STR(c.tabs.commander_death.members[0].name, "Ends");
    CHECK_STR(c.tabs.players.members[3].name, "Doesn't Matter");
    CHECK_STR(c.tabs.favorite_map.text, "Coast to Coast");
    CHECK(c.tabs.favorite_map.max_length == 0x3f && c.tabs.favorite_map.shown);

    profile_select_tab(&c, 4);
    CHECK_STR(c.tabs.ladder_html, "<html><BODY>Duel rank 12<P></BODY></html>");
    CHECK(!c.tabs.favorite_map.shown);
    profile_select_tab(&c, 5);
    CHECK_STR(c.tabs.awards_html, kDefaultReportHtml);

    profile_select_tab(&c, 6);
    CHECK(c.unit_limit == 300 && c.slide_steps == 20 && c.sound_off);
    CHECK_STR(c.unit_limit_text, "300");
    CHECK_STR(c.slide_steps_text, "20");
    CHECK(c.sound_off_checked);
    CHECK(c.current_tab == 6 && c.tab_visible[6] && !c.tab_visible[5]);
    CHECK_STR(c.tab_description, "");

    // Re-selecting a created tab does not rebuild its radio groups.
    c.tabs.skill.selected = 0;
    profile_select_tab(&c, 3);
    CHECK(c.tabs.skill.count == 4 && c.tabs.skill.selected == 0);

    // A reload replaces the record with a fresh one until the reply.
    profile_begin_receive(&c);
    CHECK(c.busy && c.record == &c.profile);
    CHECK(c.profile.name[0] == '\0' && c.profile.ladder_page_count == 0);
    CHECK(c.profile.ladder_html == nullptr && c.profile.info_html == nullptr);
}

void test_long_tag_line() {
    BoneyardsService service = boneyards_offline_service(nullptr);
    static ProfileConsole c;
    profile_console_init(&c, &service, &g_session, SettingsStore{}, nullptr, box, nullptr);
    by_test::ProfileReply reply = member_reply();
    reply.values["TAG"] = std::string(200, 'x');
    receive(&c, reply);
    profile_select_tab(&c, 1);
    CHECK(std::strlen(c.tabs.tag_line.text) == 200);
}

void test_guest_and_unaligned_tabs() {
    BoneyardsService service = boneyards_offline_service(nullptr);
    static ProfileConsole c;
    profile_console_init(&c, &service, &g_session, SettingsStore{}, nullptr, box, nullptr);
    by_test::ProfileReply guest = member_reply();
    guest.values["user_id"] = "-1";
    for (int tab = 0; tab <= 3; ++tab)
        profile_select_tab(&c, tab);
    receive(&c, guest);
    CHECK_STR(c.tabs.status, "Guest");
    CHECK(c.tabs.change_value == widget_value::disabled);
    CHECK(c.tabs.tag_line.value == widget_value::disabled);
    CHECK(c.tabs.core_contingency_value == widget_value::disabled);
    CHECK(c.tabs.display_email_alias_value == widget_value::disabled);
    CHECK(!c.tabs.logo_shown && !c.tabs.side_shown && !c.tabs.rank_shown);
    CHECK(
        c.tabs.arm_value == widget_value::disabled && c.tabs.core_value == widget_value::disabled
    );
    CHECK(c.tabs.skill.selected == -1 && c.tabs.skill.members[0].value == widget_value::disabled);
    CHECK(c.tabs.favorite_map.value == widget_value::disabled);

    // A member without a side can join one: the tab shows the join radios.
    by_test::ProfileReply undecided = member_reply();
    undecided.values["ALLEGIANCE"] = "Undecided";
    receive(&c, undecided);
    profile_select_tab(&c, 2);
    CHECK(c.tabs.name_color == SideColor::no_side_tab && c.tabs.logo_frame == insignia_frame::none);
    CHECK(c.tabs.arm_value == widget_value::enabled && c.tabs.core_value == widget_value::enabled);
    CHECK(!c.tabs.logo_shown && !c.tabs.side_shown && !c.tabs.rank_shown);
    CHECK(c.tabs.join_shown && c.tabs.radios_shown);

    // Joining a side swaps the radios for the side labels, and a reload
    // without a side brings the radios back.
    receive(&c, member_reply());
    CHECK(c.tabs.logo_shown && c.tabs.side_shown && c.tabs.rank_shown);
    CHECK(!c.tabs.join_shown && !c.tabs.radios_shown);
    receive(&c, undecided);
    CHECK(c.tabs.join_shown && c.tabs.radios_shown && !c.tabs.logo_shown);

    // Local settings without a store use the defaults.
    profile_select_tab(&c, 6);
    CHECK(c.unit_limit == kUnitLimitDefault && c.slide_steps == kSlideStepsDefault && !c.sound_off);
    CHECK_STR(c.unit_limit_text, "250");
}

} // namespace

int main() {
    test_status_and_ranks();
    test_member_tabs();
    test_long_tag_line();
    test_guest_and_unaligned_tabs();
    return by_test::finish("boneyards-profile-tabs");
}
