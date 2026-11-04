// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/contents.hpp"
#include "oa/boneyards/screens/widgets.hpp"

#include "check.hpp"

#include <cstring>
#include <string>
#include <cstdio>

using namespace oa::boneyards;
using namespace oa::boneyards::screens;

namespace {

void test_radio_group() {
    RadioGroup g{};
    g.selected = -1;
    CHECK(radio_add(&g, "Arm", widget_value::enabled));
    CHECK(radio_add(&g, "Core", widget_value::enabled));
    CHECK(radio_add(&g, "Off", widget_value::disabled));
    radio_select_by_name(&g, "Core");
    CHECK(g.selected == 1);
    CHECK(g.members[0].value == widget_value::enabled);
    CHECK(g.members[1].value == widget_value::selected);
    CHECK(g.members[2].value == widget_value::disabled); // disabled members stay disabled
    radio_refresh(&g);
    CHECK(g.members[2].value == widget_value::enabled);
    radio_disable_others(&g);
    CHECK(g.members[0].value == 0 && g.members[1].value == 1 && g.members[2].value == 0);
    radio_disable_by_name(&g, "Core");
    CHECK(g.members[1].value == 0);
    radio_select_by_name(&g, "missing");
    CHECK(g.selected == 1);
    radio_clear(&g);
    CHECK(g.selected == -1);
    for (int i = 0; i < 2; ++i)
        CHECK(radio_add(&g, "x", 2));
    CHECK(!radio_add(&g, "overflow", 2));
}

void test_count_labels_and_classes() {
    CountLabel label{};
    count_label_set(&label, 7);
    CHECK_STR(label.text, "7");
    count_label_add(&label, -9);
    CHECK_STR(label.text, "-2");
    count_label_set(&label, 0xffffffffu);
    CHECK_STR(label.text, "-1");
    CHECK(is_list_item_class(ListItemClass::player, "BYPlayerListItem"));
    CHECK(is_list_item_class(ListItemClass::circle, "BYCircleLstItem"));
    CHECK(!is_list_item_class(ListItemClass::chat_room, "byChatRoomListItem"));
    CHECK(!is_list_item_class(ListItemClass::toc, nullptr));
    CHECK(is_list_item_class(ListItemClass::toc_category, "BYTOCCategoryListItem"));
}

bool lookup(void*, const char* tag, const char** value) {
    if (std::strcmp(tag, "NAME") == 0) {
        *value = "Kane";
        return true;
    }
    return false;
}

void test_template_tags() {
    const char* text = "Hi <$NAME|nobody$>, rank <$RANK|none$>!";
    std::size_t size = 0;
    CHECK(expand_template_tags(nullptr, &size, text, lookup, nullptr));
    CHECK(size == std::strlen("Hi Kane, rank none!") + 1);
    char out[64];
    size = sizeof out;
    CHECK(expand_template_tags(out, &size, text, lookup, nullptr));
    CHECK_STR(out, "Hi Kane, rank none!");
    size = 5;
    std::memset(out, 'z', sizeof out);
    CHECK(!expand_template_tags(out, &size, text, lookup, nullptr));
    CHECK(out[0] == 'z');
    size = sizeof out;
    CHECK(expand_template_tags(out, &size, "<$NAME|d$>", nullptr, nullptr));
    CHECK_STR(out, "d");
    CHECK(!expand_template_tags(out, &size, nullptr, nullptr, nullptr));
}

void test_contents() {
    static Contents contents{};
    ContentsField cat_b{"category", "2", "Rules", "20", nullptr, nullptr, nullptr};
    ContentsField cat_a{"category", "1", "News", "10", nullptr, nullptr, nullptr};
    ContentsField head2{"headline", "5", nullptr, "2", "1", "Second", "#Remote#/n2.htm"};
    ContentsField head1{"headline", "4", nullptr, "1", "1", "First", "#Remote#/n1.htm"};
    ContentsField orphan{"headline", "6", nullptr, "1", "9", "Lost", "x"};
    contents_apply_field(&contents, &cat_b);
    contents_apply_field(&contents, &cat_a);
    contents_apply_field(&contents, &head2);
    contents_apply_field(&contents, &head1);
    contents_apply_field(&contents, &orphan);
    CHECK(contents.category_count == 2);
    CHECK_STR(contents.categories[0].name, "News");
    CHECK(contents.categories[0].headline_count == 2);
    CHECK_STR(contents.categories[0].headlines[0].title, "First");
    ContentsRow rows[8];
    char first[0x100] = {};
    const std::size_t n = contents_build_rows(&contents, true, rows, 8, first, sizeof first);
    CHECK(n == 4);
    CHECK(rows[0].is_category && !rows[1].is_category && rows[3].is_category);
    CHECK_STR(first, "#Remote#/n1.htm");

    BoneyardsService offline = boneyards_offline_service(nullptr);
    char locator[0x100];
    std::snprintf(locator, sizeof locator, "%s", "#Remote#/n1.htm");
    CHECK(
        resolve_document_locator(&offline, kContentServerNews, locator, sizeof locator, true) ==
        LocatorKind::unavailable
    );
    CHECK_STR(locator, "#Remote#/n1.htm");
    std::snprintf(locator, sizeof locator, "%s", "help.htm");
    CHECK(
        resolve_document_locator(&offline, kContentServerNews, locator, sizeof locator, true) ==
        LocatorKind::local
    );
    std::snprintf(locator, sizeof locator, "%s", "http://by.example/n1.htm");
    CHECK(
        resolve_document_locator(&offline, kContentServerNews, locator, sizeof locator, true) ==
        LocatorKind::unavailable
    );
    static std::string fetched;
    static std::int32_t asked_type = -2;
    BoneyardsService online = offline;
    online.content_server =
        [](void*, std::int32_t type, char* host, std::size_t cap, std::uint16_t* port) {
            asked_type = type;
            std::snprintf(host, cap, "by.example");
            *port = 8080;
            return true;
        };
    online.fetch_file = [](void*, const char* url, char* dest, std::size_t cap) {
        fetched = url;
        std::snprintf(dest, cap, "0BY1.TMP");
        return ServiceResult::pending;
    };
    // The console's kind picks the server; the URL is
    // "http://%s:%d%s" of its host, port and the path after "#Remote#".
    std::snprintf(locator, sizeof locator, "%s", "#Remote#/ladder/top1.htm");
    CHECK(
        resolve_document_locator(&online, kContentServerLadders, locator, sizeof locator, true) ==
        LocatorKind::remote
    );
    CHECK(asked_type == kContentServerLadders);
    CHECK_STR(locator, "http://by.example:8080/ladder/top1.htm");
    CHECK_STR(fetched.c_str(), "http://by.example:8080/ladder/top1.htm");
    std::snprintf(locator, sizeof locator, "%s", "#Remote#/boneyards/ta/news/news.def");
    CHECK(
        resolve_document_locator(&online, kContentServerNews, locator, sizeof locator, false) ==
        LocatorKind::remote
    );
    CHECK(asked_type == kContentServerNews);
    CHECK_STR(locator, "#Remote#/boneyards/ta/news/news.def");
    CHECK_STR(fetched.c_str(), "http://by.example:8080/boneyards/ta/news/news.def");
    // An http URL is fetched as it is; the server is not asked.
    asked_type = -2;
    std::snprintf(locator, sizeof locator, "%s", "http://img.example/a.png");
    CHECK(
        resolve_document_locator(&online, kContentServerNews, locator, sizeof locator, true) ==
        LocatorKind::remote
    );
    CHECK(asked_type == -2);
    CHECK_STR(fetched.c_str(), "http://img.example/a.png");

    char page[0x100];
    ladder_page_locator("#Remote#/ladder/top1.htm", 250, page, sizeof page);
    CHECK_STR(page, "#Remote#/ladder/top3.htm");
    ladder_page_locator("#Remote#/ladder/top1.htm", 100, page, sizeof page);
    CHECK_STR(page, "#Remote#/ladder/top1.htm");
    ladder_not_listed_message("Kane", page, sizeof page);
    CHECK_STR(page, "User \x93Kane\x94 is not listed in this ladder.");
}

void test_planet_text() {
    CHECK_STR(planet_affiliation_label('a'), "Arm");
    CHECK_STR(planet_affiliation_label('C'), "Core");
    CHECK_STR(planet_affiliation_label('i'), "Contested");
    CHECK(planet_affiliation_label('x') == nullptr);
    const char* lines[kMaxRequirementLines];
    CHECK(gw_requirement_lines(9, lines) == 3);
    CHECK_STR(lines[2], "");
    CHECK(gw_requirement_lines(3, lines) == 5);
    CHECK_STR(lines[3], "  Download the specified GW Map from our web site at www.coreprime.net.");
    CHECK(gw_requirement_lines(5, lines) == 6);
    CHECK_STR(lines[3], "  Have Battle Tactics Installed, and");
    CHECK_STR(lines[4], "  Download the specified GW Map from our web site at www.coreprime.net.");
}

} // namespace

int main() {
    test_radio_group();
    test_count_labels_and_classes();
    test_template_tags();
    test_contents();
    test_planet_text();
    return by_test::finish("boneyards widgets");
}
