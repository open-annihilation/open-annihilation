// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Profile.def read through oa-rover's record parser into the template
// tables: ladder records with their pages, category names and descriptions,
// the byinfo and popup pages, the one-time read and the Rover session's
// service binding; also the shipped BY1117a Profile.def when present, with
// a profile and its ladder pages built from its templates.
#include "oa/boneyards/profile_templates.hpp"
#include "oa/boneyards/rover_calls.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/profile.hpp"
#include "oa/boneyards_test/installs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

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
#define CHECK_STR(a, b) check((a) != nullptr && std::strcmp((a), (b)) == 0, #a " == " #b, __LINE__)

void write_file(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// An install directory whose Boneyards folder is capitalised, as shipped;
// the definitions name their files with the same capitals.
fs::path fresh_install(const char* name) {
    const fs::path root = fs::temp_directory_path() / name;
    fs::remove_all(root);
    const fs::path profile = root / "Boneyards" / "Profile";
    write_file(
        profile / "Profile.def",
        "{[Profile<S:command=ladder_template><I:sort=2><S:file=Boneyards/Profile/Core.htm>"
        "<I:type=4><I:subtype=2>]}\r\n"
        "{[Profile<S:command=ladder_template><I:sort=3><S:file=Boneyards/Profile/Missing.htm>"
        "<I:type=1><I:subtype=0>]}\r\n"
        "{[Profile<S:command=ladder_template><I:sort=4><S:file=Boneyards/Profile/Empty.htm>]}\r\n"
        "{[Profile<S:command=profile_category_info><S:id=1><S:name=Personal>"
        "<S:description=About you.>]}\r\n"
        "{[Profile<S:command=profile_category_info><S:id=6><S:name=Local>]}\r\n"
        "{[Profile<S:command=profile_category_info><S:id=7><S:name=Beyond>"
        "<S:description=Dropped.>]}\r\n"
        "{[Profile<S:command=headline><S:file=Boneyards/Profile/Core.htm>]}\r\n"
        "{[Profile<S:command=profile_byinfo_template><S:file=Boneyards/Profile/Info.htm>]}\r\n"
        "{[Profile<S:command=profile_popup_template><S:file=Boneyards/Profile/Popup.htm>]}\r\n"
    );
    write_file(profile / "Core.htm", "<html>\r\nCORE\r\n</html>\r\n");
    write_file(profile / "Empty.htm", "");
    write_file(profile / "Info.htm", "info\rpage\r\n\x1a trailing");
    write_file(profile / "Popup.htm", "popup\n");
    rover_session_configure(root.string().c_str(), "");
    return root;
}

void bind_rover() {
    rover_module_unload();
    rover_module_set_source(rover_get_interface);
    CHECK(rover_module_load());
}

// Type and subtype come from the record's bag (0 when absent),
// the sort from atoi of its text; a ladder whose page cannot be read is
// dropped, an empty one kept. Categories take a copy of the name and of the
// bag's description ("" when absent). Pages are read in text mode.
void test_profile_definitions() {
    fresh_install("oa-profile-templates");
    bind_rover();
    profile_templates_release();
    const ProfileTemplates* t = profile_templates_load();
    CHECK(t->loaded);
    CHECK(t->ladder_count == 2);
    if (t->ladder_count == 2) {
        const LadderTemplate* core = t->ladders[0];
        CHECK(core->type == 4 && core->subtype == 2 && core->sort == 2);
        CHECK_STR(core->page, "<html>\nCORE\n</html>\n");
        const LadderTemplate* empty = t->ladders[1];
        CHECK(empty->type == 0 && empty->subtype == 0 && empty->sort == 4);
        CHECK_STR(empty->page, "");
    }
    CHECK_STR(t->category_names[1], "Personal");
    CHECK_STR(t->category_descriptions[1], "About you.");
    CHECK_STR(t->category_names[6], "Local");
    CHECK_STR(t->category_descriptions[6], "");
    CHECK(t->category_names[0] == nullptr && t->category_descriptions[0] == nullptr);
    CHECK_STR(t->byinfo_page, "info\rpage\n");
    CHECK_STR(t->popup_page, "popup\n");

    // Read once: a later request returns the same tables without parsing.
    const fs::path def = fs::temp_directory_path() / "oa-profile-templates" / "Boneyards" /
                         "Profile" / "Profile.def";
    fs::remove(def);
    CHECK(profile_templates_load() == t && t->ladder_count == 2);

    // The Rover session's service hands the screens the same tables.
    const BoneyardsService service = rover_session_service();
    CHECK(service.profile_templates != nullptr);
    CHECK(service.profile_templates(service.user) == t);

    // A missing file still counts as read.
    profile_templates_release();
    CHECK(!t->loaded && t->ladder_count == 0 && t->byinfo_page == nullptr);
    CHECK(profile_templates_load()->loaded);
    CHECK(t->ladder_count == 0 && t->category_names[1] == nullptr);
    profile_templates_release();
    rover_module_unload();
}

// The ladder template stores its fields and reads the page, null for a missing file.
void test_ladder_template_init() {
    fresh_install("oa-profile-ladder");
    LadderTemplate ladder{};
    CHECK(ladder_template_init(&ladder, 4, 1, 1, "Boneyards/Profile/Core.htm") == &ladder);
    CHECK(ladder.type == 4 && ladder.subtype == 1 && ladder.sort == 1);
    CHECK_STR(ladder.page, "<html>\nCORE\n</html>\n");
    std::free(ladder.page);
    ladder_template_init(&ladder, 1, 0, 3, "Boneyards/Profile/Absent.htm");
    CHECK(ladder.page == nullptr && ladder.sort == 3);
}

// A reply's fields for the record readers.
struct Reply {
    std::map<std::string, std::string> values;
};

bool reply_string(void* user, const char* key, char* out, std::size_t capacity) {
    const auto& values = static_cast<Reply*>(user)->values;
    const auto it = values.find(key);
    if (it == values.end())
        return false;
    std::snprintf(out, capacity, "%s", it->second.c_str());
    return true;
}

bool reply_int(void* user, const char* key, int32_t* out) {
    const auto& values = static_cast<Reply*>(user)->values;
    const auto it = values.find(key);
    if (it == values.end())
        return false;
    *out = std::atoi(it->second.c_str());
    return true;
}

void reply_set(void* user, const char* key, const char* value) {
    static_cast<Reply*>(user)->values[key] = value;
}

screens::ProfileFields reply_fields(Reply* reply) {
    return screens::ProfileFields{reply, reply_string, reply_int, nullptr, reply_set};
}

bool contains(const char* text, const char* part) {
    return text != nullptr && std::strstr(text, part) != nullptr;
}

// The shipped templates filled from a reply: a Duel ladder page arriving
// before the Arm Galactic War one still follows it (sorts 3 and 1), missing
// ladder fields show their defaults, and the pop-up and Boneyards-info pages
// show the derived preference texts and the defaults.
void test_shipped_profile(const ProfileTemplates* t) {
    screens::ProfileRecord record{};
    screens::profile_record_init(&record);
    Reply duel;
    duel.values = {{"RANK", "12"}, {"SCORE", "1500"}};
    screens::ProfileFields f = reply_fields(&duel);
    CHECK(screens::ladder_profile_apply(&record, t, 1, 0, &f));
    Reply arm;
    arm.values = {{"WS", "40"}};
    f = reply_fields(&arm);
    CHECK(screens::ladder_profile_apply(&record, t, 4, 1, &f));
    CHECK(!screens::ladder_profile_apply(&record, t, 4, 3, &f));
    Reply reply;
    reply.values = {
        {"USER_STATUS", "1"},
        {"PLAYERSKILL", "3"},
        {"NUMPLAYERS", "1"},
        {"ONCOMDEATH", "0"},
        {"TAFORUM_ALIAS", " "}
    };
    f = reply_fields(&reply);
    screens::profile_from_fields(&record, t, &f);
    CHECK(contains(
        record.ladder_html,
        "<html><BODY><b><font size=\"+1\">Galactic War Service "
        "Record<BR>Fighting for Arm"
    ));
    CHECK(contains(record.ladder_html, "<TD><b>Total Wins:</b></TD> <TD>40</TD>"));
    CHECK(contains(record.ladder_html, "<TD><b>Current Rank:</b></TD> <TD>12</TD>"));
    CHECK(contains(record.ladder_html, "<TD><b>Ladder Score:</b></TD> <TD>1500</TD>"));
    CHECK(contains(record.ladder_html, "<TD><b>Wins:</b></TD> <TD>0 (0 / 0 / 0)</TD>"));
    CHECK(
        std::strstr(record.ladder_html, "Fighting for Arm") <
        std::strstr(record.ladder_html, ">Duel<")
    );
    CHECK(contains(record.general_html, "<TD><b>Player Skill:</b></TD> <TD>Expert</TD>"));
    CHECK(
        contains(record.general_html, "<TD><b>Number of Players:</b></TD> <TD>Doesn't Matter</TD>")
    );
    CHECK(contains(record.general_html, "<TD><b>On Commander Death:</b></TD> <TD>Continues</TD>"));
    CHECK(contains(record.general_html, "<TD><b>TA Forum Alias:</b></TD> <TD></TD>"));
    CHECK(contains(record.general_html, "<TD><b>ICQ Number:</b></TD> <TD>Not Specified</TD>"));
    CHECK(contains(record.info_html, "<TD><b>Clan:</b></TD> <TD>None</TD>"));
    CHECK(contains(record.info_html, "<TD><b>Boneyards Time:</b></TD> <TD>0 minutes</TD>"));
    CHECK(record.awards_html == nullptr);
    screens::profile_record_release(&record);
}

// The shipped definitions: four ladders, the seven category descriptions
// the Profile console shows, and both player-page templates.
void test_shipped_definitions() {
    const fs::path game = oa::boneyards_test::require_by_install(
        "the shipped profile definitions", {"Boneyards/Profile/Profile.def"}
    );
    rover_session_configure(game.string().c_str(), "");
    bind_rover();
    profile_templates_release();
    const ProfileTemplates* t = profile_templates_load();
    CHECK(t->ladder_count == 4);
    if (t->ladder_count == 4) {
        static constexpr int32_t kShipped[4][3] = {{4, 1, 1}, {4, 2, 2}, {1, 0, 3}, {2, 0, 4}};
        for (int32_t i = 0; i < 4; ++i) {
            const LadderTemplate* ladder = t->ladders[i];
            CHECK(ladder->type == kShipped[i][0] && ladder->subtype == kShipped[i][1]);
            CHECK(ladder->sort == kShipped[i][2]);
            CHECK(ladder->page != nullptr && std::strlen(ladder->page) > 0);
        }
    }
    CHECK_STR(t->category_names[0], "Login");
    CHECK_STR(t->category_descriptions[0], "Private user account information.");
    CHECK_STR(t->category_names[4], "Ladder Standings");
    CHECK_STR(t->category_descriptions[4], "Scrolling summary of ladder standings. ");
    CHECK_STR(t->category_descriptions[6], " Machine specific configuration options.");
    CHECK(t->byinfo_page != nullptr && t->popup_page != nullptr);
    if (t->ladder_count == 4 && t->byinfo_page != nullptr && t->popup_page != nullptr)
        test_shipped_profile(t);
    profile_templates_release();
    rover_module_unload();
}

} // namespace

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv)) {
        test_shipped_definitions();
    } else {
        test_ladder_template_init();
        test_profile_definitions();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("profile templates: ok");
    return 0;
}
