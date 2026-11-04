// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The profile record readers: the profile_info fields into the record, the
// derived preference texts written back for the templates, the awards page,
// the pop-up and Boneyards-info pages, and the ladder pages in sort order.
#include "oa/boneyards/screens/profile.hpp"

#include "check.hpp"

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

using namespace oa::boneyards;
using namespace oa::boneyards::screens;

namespace {

// A reply's fields: texts, with the integer readers parsing them.
struct Bag {
    std::map<std::string, std::string> values;
    int writes = 0;
};

bool get_string(void* user, const char* key, char* out, std::size_t capacity) {
    const auto& values = static_cast<Bag*>(user)->values;
    const auto it = values.find(key);
    if (it == values.end())
        return false;
    std::snprintf(out, capacity, "%s", it->second.c_str());
    return true;
}

bool get_int(void* user, const char* key, int32_t* out) {
    const auto& values = static_cast<Bag*>(user)->values;
    const auto it = values.find(key);
    if (it == values.end())
        return false;
    *out = static_cast<int32_t>(std::strtol(it->second.c_str(), nullptr, 10));
    return true;
}

bool get_uint(void* user, const char* key, uint32_t* out) {
    const auto& values = static_cast<Bag*>(user)->values;
    const auto it = values.find(key);
    if (it == values.end())
        return false;
    *out = static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10));
    return true;
}

void set_string(void* user, const char* key, const char* value) {
    auto* bag = static_cast<Bag*>(user);
    bag->values[key] = value;
    ++bag->writes;
}

ProfileFields fields_of(Bag* bag) {
    return ProfileFields{bag, get_string, get_int, get_uint, set_string};
}

const char* field(const Bag& bag, const char* key) {
    const auto it = bag.values.find(key);
    return it == bag.values.end() ? nullptr : it->second.c_str();
}

char g_popup[] = "<$TAG|no tag$>|<$HAS_CC|Not Specified$>|<$PLAYERSKILL_TEXT|Not Specified$>|"
                 "<$NUMPLAYERS_TEXT|Not Specified$>|<$ONCMDDEATH_TEXT|Not Specified$>|"
                 "<$TAFORUM_ALIAS|Not Specified$>";
char g_byinfo[] = "<$CLAN|None$> <$BONETIME|0$> minutes";

ProfileTemplates page_templates() {
    ProfileTemplates t{};
    t.loaded = true;
    t.popup_page = g_popup;
    t.byinfo_page = g_byinfo;
    return t;
}

Bag member_bag() {
    Bag bag;
    bag.values = {
        {"user_id", "77"},
        {"LOGINNAME", "SentinelOfTheYards"},
        {"ICON", "18"},
        {"ALLEGIANCE", "arm"},
        {"CLAN", "Iron Legion"},
        {"USER_STATUS", "5"},
        {"EMAIL", "s@example.com"},
        {"TAG", " "},
        {"HAS_CC", "Yes"},
        {"HAS_BT", "yes"},
        {"TAFORUM_ALIAS", "sentinel"},
        {"EMAIL_ALIAS", " "},
        {"ICQ_ALIAS", "12345"},
        {"NUMPLAYERS", "3"},
        {"PLAYERSKILL", "2"},
        {"ONCOMDEATH", "2"},
        {"FAVORITEMAP", "Coast to Coast"},
        {"AWARD_1", "Iron Cross"},
        {"AWARD_2", "Yard Dog of the Month"},
        {"AWARD_4", "never read"},
        {"BONETIME", "4000"},
    };
    return bag;
}

// profile_from_fields on a full reply: the login name is cut to 15 characters, the
// side comes from the first letter in either case, a one-space text reads
// and is written back empty, HAS_CC/HAS_BT compare case-sensitively with
// "Yes", the preference codes become their radio texts and are written back
// for the templates, and AWARD_n are read up to the first gap.
void test_member_record() {
    const ProfileTemplates t = page_templates();
    Bag bag = member_bag();
    const ProfileFields f = fields_of(&bag);
    ProfileRecord r{};
    profile_record_init(&r);
    profile_from_fields(&r, &t, &f);

    CHECK(r.user_id == 77);
    CHECK_STR(r.name, "SentinelOfTheYa");
    CHECK(r.user_icon == 18);
    CHECK(r.allegiance == 'A');
    CHECK_STR(r.clan, "Iron Legion");
    CHECK(r.status == static_cast<int32_t>(ProfileStatus::beta_tester));
    CHECK_STR(r.email, "s@example.com");
    CHECK_STR(r.tag_line, "");
    CHECK_STR(field(bag, "TAG"), "");
    CHECK(r.has_core_contingency && r.core_contingency_set);
    CHECK(!r.has_battle_tactics && r.battle_tactics_set);
    CHECK_STR(r.forum_alias, "sentinel");
    CHECK_STR(r.email_alias, "");
    CHECK_STR(field(bag, "EMAIL_ALIAS"), "");
    CHECK_STR(r.icq, "12345");
    CHECK_STR(r.preferred_players, "3");
    CHECK_STR(field(bag, "NUMPLAYERS_TEXT"), "3");
    CHECK_STR(r.preferred_skill, "Seasoned");
    CHECK_STR(field(bag, "PLAYERSKILL_TEXT"), "Seasoned");
    CHECK_STR(r.preferred_death, "Deathmatch");
    CHECK_STR(field(bag, "ONCMDDEATH_TEXT"), "Deathmatch");
    CHECK_STR(r.favorite_map, "Coast to Coast");
    CHECK(r.ladder_html == nullptr);
    CHECK_STR(
        r.awards_html,
        "<html><BODY><font size=+1>Iron Cross</font><BR><P>"
        "<font size=+1>Yard Dog of the Month</font><BR><P><P></BODY></html>"
    );
    CHECK_STR(r.general_html, "|Yes|Seasoned|3|Deathmatch|sentinel");
    CHECK_STR(r.info_html, "Iron Legion 4000 minutes");
    CHECK(bag.writes == 5);
    profile_record_release(&r);
    CHECK(r.clan == nullptr && r.general_html == nullptr && r.awards_html == nullptr);
    CHECK(r.allegiance == 'U' && r.status == static_cast<int32_t>(ProfileStatus::active));
}

// An empty reply: the name is empty, icon and status 0, no side, the
// optional texts stay unset, the preferences read as no preference without
// writing anything back, no awards page, and the templates use their
// defaults. The user id keeps the record's value.
void test_empty_reply() {
    const ProfileTemplates t = page_templates();
    Bag bag;
    const ProfileFields f = fields_of(&bag);
    ProfileRecord r{};
    profile_record_init(&r);
    r.user_id = -5;
    profile_from_fields(&r, &t, &f);

    CHECK(r.user_id == -5);
    CHECK_STR(r.name, "");
    CHECK(r.user_icon == 0);
    CHECK(r.allegiance == 'U');
    CHECK(r.status == 0);
    CHECK_STR(profile_status_label(&r), "Guest");
    CHECK(r.clan == nullptr && r.email == nullptr && r.tag_line == nullptr);
    CHECK(r.forum_alias == nullptr && r.email_alias == nullptr && r.icq == nullptr);
    CHECK(r.favorite_map == nullptr);
    CHECK(!r.has_core_contingency && !r.core_contingency_set);
    CHECK(!r.has_battle_tactics && !r.battle_tactics_set);
    CHECK_STR(r.preferred_players, "Doesn't Matter");
    CHECK_STR(r.preferred_skill, "Don't Know");
    CHECK_STR(r.preferred_death, "Doesn't Matter");
    CHECK(bag.writes == 0);
    CHECK(r.ladder_html == nullptr && r.awards_html == nullptr);
    CHECK_STR(
        r.general_html,
        "no tag|Not Specified|Not Specified|Not Specified|Not Specified|Not Specified"
    );
    CHECK_STR(r.info_html, "None 0 minutes");
    profile_record_release(&r);

    // Without templates the two pages stay null.
    profile_record_init(&r);
    profile_from_fields(&r, nullptr, &f);
    CHECK(r.general_html == nullptr && r.info_html == nullptr);
    profile_record_release(&r);
}

struct PreferenceCase {
    const char* players;
    const char* skill;
    const char* death;
    const char* players_text;
    const char* skill_text;
    const char* death_text;
};

// The preference codes: NUMPLAYERS "1" means no preference (and is written
// back as such), PLAYERSKILL is read as a number, ONCOMDEATH by its first
// character.
void test_preference_codes() {
    static constexpr PreferenceCase kCases[] = {
        {"1", "1", "0", "Doesn't Matter", "Novice", "Continues"},
        {"2", "3", "1", "2", "Expert", "Ends"},
        {"4", "03", "2x", "4", "Expert", "Deathmatch"},
        {"", "4", "3", "", "Don't Know", "Doesn't Matter"},
        {"Doesn't Matter", "x", "", "Doesn't Matter", "Don't Know", "Doesn't Matter"},
    };
    for (const PreferenceCase& c : kCases) {
        Bag bag;
        bag.values = {{"NUMPLAYERS", c.players}, {"PLAYERSKILL", c.skill}, {"ONCOMDEATH", c.death}};
        const ProfileFields f = fields_of(&bag);
        ProfileRecord r{};
        profile_record_init(&r);
        profile_from_fields(&r, nullptr, &f);
        CHECK_STR(r.preferred_players, c.players_text);
        CHECK_STR(r.preferred_skill, c.skill_text);
        CHECK_STR(r.preferred_death, c.death_text);
        CHECK_STR(field(bag, "NUMPLAYERS_TEXT"), c.players_text);
        CHECK_STR(field(bag, "PLAYERSKILL_TEXT"), c.skill_text);
        CHECK_STR(field(bag, "ONCMDDEATH_TEXT"), c.death_text);
        profile_record_release(&r);
    }
}

char g_arm[] = "A<$RANK|Below 500$>;";
char g_core[] = "B<$SCORE|0$>;";
char g_duel[] = "C<$WS|0$>;";
char g_arm_again[] = "never used";
char g_wta[] = "D;";

// Each ladder reply takes the first template of its type and subtype; one
// without a template adds nothing. profile_from_fields then joins the
// pages in template sort order, arrival order among equal sorts, unless
// the player is an admin.
void test_ladder_pages() {
    LadderTemplate arm{4, 1, 2, g_arm};
    LadderTemplate core{4, 2, 1, g_core};
    LadderTemplate duel{1, 0, 3, g_duel};
    LadderTemplate arm_again{4, 1, 0, g_arm_again};
    LadderTemplate wta{2, 0, 1, g_wta};
    LadderTemplate* ladders[] = {&arm, &core, &duel, &arm_again, &wta};
    ProfileTemplates t = page_templates();
    t.ladders = ladders;
    t.ladder_count = 5;

    for (int32_t status : {1, 4}) {
        ProfileRecord r{};
        profile_record_init(&r);
        Bag duel_bag;
        duel_bag.values = {{"WS", "7"}};
        ProfileFields f = fields_of(&duel_bag);
        CHECK(ladder_profile_apply(&r, &t, 1, 0, &f));
        Bag arm_bag;
        arm_bag.values = {{"RANK", "12"}};
        f = fields_of(&arm_bag);
        CHECK(ladder_profile_apply(&r, &t, 4, 1, &f));
        Bag empty;
        f = fields_of(&empty);
        CHECK(!ladder_profile_apply(&r, &t, 7, 0, &f));
        CHECK(!ladder_profile_apply(&r, nullptr, 2, 0, &f));
        CHECK(ladder_profile_apply(&r, &t, 2, 0, &f));
        Bag core_bag;
        core_bag.values = {{"SCORE", "900"}};
        f = fields_of(&core_bag);
        CHECK(ladder_profile_apply(&r, &t, 4, 2, &f));
        CHECK(r.ladder_page_count == 4);
        CHECK(r.ladder_pages[0].sort == 3);
        CHECK_STR(r.ladder_pages[0].html, "C7;");
        CHECK(r.ladder_pages[1].sort == 2);
        CHECK_STR(r.ladder_pages[1].html, "A12;");

        Bag reply;
        reply.values = {{"USER_STATUS", std::to_string(status)}};
        f = fields_of(&reply);
        profile_from_fields(&r, &t, &f);
        if (status == 4) {
            CHECK(r.ladder_html == nullptr);
        } else {
            CHECK_STR(r.ladder_html, "<html><BODY>D;B900;A12;C7;<P></BODY></html>");
        }
        profile_record_release(&r);
        CHECK(r.ladder_pages == nullptr && r.ladder_page_count == 0 && r.ladder_html == nullptr);
    }
}

} // namespace

int main() {
    test_member_record();
    test_empty_reply();
    test_preference_codes();
    test_ladder_pages();
    return by_test::finish("boneyards-profile-record");
}
