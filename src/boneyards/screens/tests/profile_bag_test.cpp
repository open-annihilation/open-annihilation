// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Profile console's APPLY against a recording service: the fields of the
// set_profile property bag each tab's changes put in it, the user id it goes
// for, and nothing sent (and no transmitting) for an empty bag.
#include "oa/boneyards/screens/navigator.hpp"

#include "check.hpp"
#include "profile_reply.hpp"

#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>
#include <cstdio>

using namespace oa::boneyards;
using namespace oa::boneyards::screens;

namespace {

struct Sent {
    uint32_t user_id = 0;
    std::map<std::string, std::string> fields;
    std::vector<std::string> order;
    ProfileUpdatedCallback updated = nullptr;
    void* context = nullptr;
};

std::vector<Sent> g_sent;

ServiceResult record_submit(
    void*,
    uint32_t user_id,
    const ProfileEdits* edits,
    ProfileUpdatedCallback updated,
    void* context
) {
    Sent sent;
    sent.user_id = user_id;
    for (std::size_t i = 0; i < edits->count; ++i) {
        sent.fields[edits->fields[i].key] = edits->fields[i].value;
        sent.order.emplace_back(edits->fields[i].key);
    }
    sent.updated = updated;
    sent.context = context;
    g_sent.push_back(sent);
    return ServiceResult::pending;
}

ServiceResult ignore_request(void*, uint32_t, ProfileCallback, LadderCallback, void*) {
    return ServiceResult::pending;
}

std::vector<bool> g_lights;

ProfileConsole g_console;
ChatSession g_session;
BoneyardsService g_service;

// A member with no side: an email but no alias, a tag and a forum alias,
// Core Contingency, no Battle Tactics answer, three players and Seasoned
// given, no commander death given, and a favourite map.
ProfileConsole* console_with_record(const char* user_id = "12") {
    g_sent.clear();
    g_lights.clear();
    g_service = boneyards_offline_service(nullptr);
    g_service.submit_profile = record_submit;
    g_service.request_profile = ignore_request;
    g_session = ChatSession{};
    g_session.self_user_id = 0x2a;
    profile_console_init(
        &g_console,
        &g_service,
        &g_session,
        SettingsStore{},
        nullptr,
        nullptr,
        [](void*, bool running) { g_lights.push_back(running); }
    );
    by_test::ProfileReply reply{
        {"user_id", user_id},
        {"ALLEGIANCE", "U"},
        {"EMAIL", "cmdr@example.net"},
        {"TAG", "Onward"},
        {"HAS_CC", "Yes"},
        {"TAFORUM_ALIAS", "forum"},
        {"NUMPLAYERS", "3"},
        {"PLAYERSKILL", "2"},
        {"FAVORITEMAP", "Tundra"},
    };
    const ProfileFields fields = reply.fields();
    profile_on_received(&g_console, true, &fields);
    for (int32_t tab = 0; tab < static_cast<int32_t>(kProfileTabCount); ++tab)
        profile_create_tab(&g_console, tab);
    g_lights.clear();
    return &g_console;
}

void mark_dirty(ProfileConsole* c, std::initializer_list<ProfileTab> tabs) {
    for (ProfileTab tab : tabs)
        c->tab_dirty[static_cast<int32_t>(tab)] = true;
}

bool has(const Sent& sent, const char* key) {
    return sent.fields.count(key) != 0;
}

// Dirty tabs whose controls still show the record send only what the record
// lacks: HAS_BT (never answered), ALLEGIANCE ('U' for a member of no side)
// and ONCOMDEATH (not given, its default "Doesn't Matter" as "3").
void test_unchanged_fields() {
    ProfileConsole* c = console_with_record();
    mark_dirty(c, {ProfileTab::personal, ProfileTab::allegiance, ProfileTab::game_preferences});
    profile_apply(c);
    CHECK(g_sent.size() == 1);
    if (g_sent.empty())
        return;
    const Sent& sent = g_sent[0];
    CHECK(sent.user_id == 0x2a && sent.context == c && sent.updated != nullptr);
    CHECK((sent.order == std::vector<std::string>{"HAS_BT", "ALLEGIANCE", "ONCOMDEATH"}));
    CHECK(sent.fields.at("HAS_BT") == "No");
    CHECK(sent.fields.at("ALLEGIANCE") == "U");
    CHECK(sent.fields.at("ONCOMDEATH") == "3");
    CHECK(c->busy && std::strcmp(c->status, "TRANSMITTING DATA...") == 0);
    CHECK((g_lights == std::vector<bool>{true}));
    // The answer ends the transmission and reloads the record.
    sent.updated(sent.context, true);
    CHECK((g_lights == std::vector<bool>{true, false, true}));
}

// Changed texts go as they are, emptied ones as a single space; the checks
// go as "Yes" or "No"; the email alias as the record's email or a space.
void test_personal_changes() {
    ProfileConsole* c = console_with_record();
    ProfileTabControls* t = &c->tabs;
    std::snprintf(t->tag_line.text, sizeof t->tag_line.text, "%s", "");
    std::snprintf(t->forum_alias.text, sizeof t->forum_alias.text, "%s", "new alias");
    std::snprintf(t->icq.text, sizeof t->icq.text, "%s", "12345");
    t->core_contingency_value = widget_value::icon_off;
    t->battle_tactics_value = widget_value::icon_on;
    t->display_email_alias_value = widget_value::icon_on;
    mark_dirty(c, {ProfileTab::personal});
    profile_apply(c);
    CHECK(g_sent.size() == 1);
    if (g_sent.empty())
        return;
    const Sent& sent = g_sent[0];
    CHECK(
        (sent.order == std::vector<std::string>{
                           "TAG", "HAS_CC", "HAS_BT", "TAFORUM_ALIAS", "ICQ_ALIAS", "EMAIL_ALIAS"
                       })
    );
    CHECK(sent.fields.at("TAG") == " ");
    CHECK(sent.fields.at("HAS_CC") == "No");
    CHECK(sent.fields.at("HAS_BT") == "Yes");
    CHECK(sent.fields.at("TAFORUM_ALIAS") == "new alias");
    CHECK(sent.fields.at("ICQ_ALIAS") == "12345");
    CHECK(sent.fields.at("EMAIL_ALIAS") == "cmdr@example.net");

    // Unchecking a record's alias sends a space; checking one without an
    // email sends a space too.
    c = console_with_record();
    c->profile.email_alias = nullptr;
    std::free(const_cast<char*>(c->profile.email));
    c->profile.email = nullptr;
    c->tabs.display_email_alias_value = widget_value::icon_on;
    mark_dirty(c, {ProfileTab::personal});
    profile_apply(c);
    CHECK(!g_sent.empty() && g_sent[0].fields.at("EMAIL_ALIAS") == " ");
    c = console_with_record();
    c->profile.email_alias = "shown";
    c->tabs.display_email_alias_value = widget_value::icon_off;
    mark_dirty(c, {ProfileTab::personal});
    profile_apply(c);
    CHECK(!g_sent.empty() && g_sent[0].fields.at("EMAIL_ALIAS") == " ");
    c->profile.email_alias = nullptr;
}

// ALLEGIANCE: 'ARM' or 'CORE' by the radio (Core when both), only from a
// member of no side; a member of a side or a guest sends none.
void test_allegiance() {
    ProfileConsole* c = console_with_record();
    c->tabs.arm_value = widget_value::selected;
    mark_dirty(c, {ProfileTab::allegiance});
    profile_apply(c);
    CHECK(!g_sent.empty() && g_sent[0].fields.at("ALLEGIANCE") == "ARM");
    c = console_with_record();
    c->tabs.arm_value = widget_value::selected;
    c->tabs.core_value = widget_value::selected;
    mark_dirty(c, {ProfileTab::allegiance});
    profile_apply(c);
    CHECK(!g_sent.empty() && g_sent[0].fields.at("ALLEGIANCE") == "CORE");
    c = console_with_record();
    c->profile.allegiance = 'A';
    mark_dirty(c, {ProfileTab::allegiance});
    profile_apply(c);
    CHECK(g_sent.empty() && !c->busy);
    c = console_with_record("-7");
    mark_dirty(c, {ProfileTab::allegiance});
    profile_apply(c);
    CHECK(g_sent.empty());
}

// The preferences: "Doesn't Matter" players as "1", the skill and death
// codes, and a choice equal to the record but not given by the reply sent.
void test_preferences() {
    ProfileConsole* c = console_with_record();
    ProfileTabControls* t = &c->tabs;
    radio_select_by_name(&t->players, "Doesn't Matter");
    radio_select_by_name(&t->skill, "Expert");
    radio_select_by_name(&t->commander_death, "Continues");
    std::snprintf(t->favorite_map.text, sizeof t->favorite_map.text, "%s", "Lava Run");
    mark_dirty(c, {ProfileTab::game_preferences});
    profile_apply(c);
    CHECK(g_sent.size() == 1);
    if (g_sent.empty())
        return;
    CHECK(
        (g_sent[0].order ==
         std::vector<std::string>{"NUMPLAYERS", "PLAYERSKILL", "ONCOMDEATH", "FAVORITEMAP"})
    );
    CHECK(g_sent[0].fields.at("NUMPLAYERS") == "1");
    CHECK(g_sent[0].fields.at("PLAYERSKILL") == "3");
    CHECK(g_sent[0].fields.at("ONCOMDEATH") == "0");
    CHECK(g_sent[0].fields.at("FAVORITEMAP") == "Lava Run");

    const struct {
        const char* skill;
        const char* skill_code;
        const char* death;
        const char* death_code;
    } codes[] = {
        {"Novice", "1", "Ends", "1"},
        {"Don't Know", "0", "Deathmatch", "2"},
    };

    for (const auto& code : codes) {
        c = console_with_record();
        radio_select_by_name(&c->tabs.skill, code.skill);
        radio_select_by_name(&c->tabs.commander_death, code.death);
        mark_dirty(c, {ProfileTab::game_preferences});
        profile_apply(c);
        CHECK(!g_sent.empty() && g_sent[0].fields.at("PLAYERSKILL") == code.skill_code);
        CHECK(!g_sent.empty() && g_sent[0].fields.at("ONCOMDEATH") == code.death_code);
        CHECK(!g_sent.empty() && !has(g_sent[0], "NUMPLAYERS") && !has(g_sent[0], "FAVORITEMAP"));
    }

    // Three players are the record's and given: not sent; a reply that did
    // not give them sends them even unchanged.
    c = console_with_record();
    c->profile.players_given = false;
    mark_dirty(c, {ProfileTab::game_preferences});
    profile_apply(c);
    CHECK(!g_sent.empty() && g_sent[0].fields.at("NUMPLAYERS") == "3");
}

// A new password joins the same bag as the other changes; an empty bag sends
// nothing and does not start transmitting.
void test_password_and_empty_bag() {
    ProfileConsole* c = console_with_record();
    std::snprintf(c->tabs.password.text, sizeof c->tabs.password.text, "%s", "secret");
    std::snprintf(c->tabs.confirm.text, sizeof c->tabs.confirm.text, "%s", "secret");
    std::snprintf(c->tabs.tag_line.text, sizeof c->tabs.tag_line.text, "%s", "Changed");
    mark_dirty(c, {ProfileTab::account, ProfileTab::personal});
    profile_apply(c);
    CHECK(g_sent.size() == 1);
    CHECK(
        !g_sent.empty() && g_sent[0].order.front() == "PASSWORD" &&
        g_sent[0].fields.at("PASSWORD") == "secret" && g_sent[0].fields.at("TAG") == "Changed"
    );

    c = console_with_record();
    c->profile.battle_tactics_set = true;
    mark_dirty(c, {ProfileTab::account, ProfileTab::personal});
    profile_apply(c);
    CHECK(g_sent.empty() && !c->busy && c->status_visible == false);
    CHECK(g_lights.empty());
}

} // namespace

int main() {
    test_unchanged_fields();
    test_personal_changes();
    test_allegiance();
    test_preferences();
    test_password_and_empty_bag();
    profile_record_release(&g_console.profile);
    return by_test::finish("profile_bag_test");
}
