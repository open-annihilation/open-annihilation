// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/campaign/briefing_text.hpp"
#include "oa/ui/campaign/campaign.hpp"
#include "oa/ui/campaign/single_player.hpp"

#include "oa/data/campaign/campaign_assets.hpp"
#include "oa/core/unit_def.h"
#include "oa/core/world.h"
#include "oa/test/game_assets.hpp"
#include "oa/base/text.hpp"

#include "oa/formats/hpi.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace oa::ui::campaign;

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

// Every character is 6 pixels wide; CR and LF measure nothing.
int32_t measure(void*, const char* text) {
    int32_t width = 0;
    for (; *text != '\0'; ++text)
        if (*text != '\r' && *text != '\n')
            width += 6;
    return width;
}

struct Recorder {
    std::vector<std::string> log;
    bool disc = true;
    uint8_t signal = 0;
};

Recorder* recorder(void* context) {
    return static_cast<Recorder*>(context);
}

FrontendHost recording_host(Recorder& r) {
    FrontendHost host{};
    host.context = &r;
    host.play_sound = [](void* c, const char* n) {
        recorder(c)->log.push_back(std::string("sound ") + n);
    };
    host.select_cursor_animation = [](void* c, int32_t i) {
        recorder(c)->log.push_back("cursor " + std::to_string(i));
    };
    host.message_box = [](void* c, const char* t, int32_t) {
        recorder(c)->log.push_back(std::string("message ") + t);
    };
    host.disc_present = [](void* c) { return recorder(c)->disc; };
    host.discover_archives = [](void* c) { recorder(c)->log.push_back("discover"); };
    host.signal = [](void* c, uint8_t s) {
        recorder(c)->signal = s;
        recorder(c)->log.push_back("signal " + std::to_string(s));
    };
    host.clear_selection = [](void* c) { recorder(c)->log.push_back("clear"); };
    host.set_control_value = [](void* c, const char* n, int32_t v) {
        recorder(c)->log.push_back(std::string("value ") + n + "=" + std::to_string(v));
    };
    host.select_group = [](void* c, const char* n) {
        recorder(c)->log.push_back(std::string("group ") + n);
    };
    host.set_list = [](void* c, const char* n, const char*, int32_t count) {
        recorder(c)->log.push_back(std::string("list ") + n + "=" + std::to_string(count));
    };
    host.stop_narration = [](void* c) { recorder(c)->log.push_back("stop narration"); };
    host.play_narration = [](void* c, const char* p, uint32_t delay) {
        recorder(c)->log.push_back(std::string("narrate ") + p + " after " + std::to_string(delay));
    };
    host.mark_dirty = [](void* c) { recorder(c)->log.push_back("dirty"); };
    host.clear_attributes = [](void* c, const char* n, uint32_t bits) {
        recorder(c)->log.push_back(std::string("attributes ") + n + "&~" + std::to_string(bits));
    };
    return host;
}

bool logged(const Recorder& r, const std::string& entry) {
    return std::find(r.log.begin(), r.log.end(), entry) != r.log.end();
}

void wrap_tests() {
    char out[256];
    wrap_text("alpha beta gamma delta", 61, measure, nullptr, out, sizeof(out));
    expect(std::strcmp(out, "alpha beta\r\ngamma delta") == 0, "wrap at space before width");
    wrap_text("one\ntwo three four five", 61, measure, nullptr, out, sizeof(out));
    expect(std::strcmp(out, "one\ntwo three\r\nfour five") == 0, "newline restarts line width");
    wrap_text("anti-aircraft battery", 61, measure, nullptr, out, sizeof(out));
    expect(std::strcmp(out, "anti\r\naircraft battery") == 0, "hyphen break drops the hyphen");
    wrap_text("supercalifragilistic word", 30, measure, nullptr, out, sizeof(out));
    expect(std::strcmp(out, "supercalifragilistic word") == 0, "long word stays whole");
    // Rows the modern fonts draw keep the hyphen they break at, in Latin and
    // in Bengali; a space at a break is still dropped, and the game font's
    // rows lose the hyphen as above. A Bengali letter takes three bytes, 18
    // pixels here.
    wrap_text("anti-aircraft battery", 61, measure, nullptr, out, sizeof(out), true);
    expect(std::strcmp(out, "anti-\r\naircraft battery") == 0, "a kept hyphen ends its row");
    wrap_text("Empyrrean-এ আছে", 62, measure, nullptr, out, sizeof(out), true);
    expect(std::strcmp(out, "Empyrrean-\r\nএ আছে") == 0, "a kept hyphen ends its Bengali row");
    wrap_text("Empyrrean-এ আছে", 62, measure, nullptr, out, sizeof(out));
    expect(std::strcmp(out, "Empyrrean\r\nএ আছে") == 0, "the game font's row loses the hyphen");
    wrap_text("alpha beta gamma delta", 61, measure, nullptr, out, sizeof(out), true);
    expect(std::strcmp(out, "alpha beta\r\ngamma delta") == 0, "a space at a break is dropped");
    // A hyphen that would carry its row to the width stays with its word.
    wrap_text("ab abcdefg-klm xy", 61, measure, nullptr, out, sizeof(out), true);
    expect(
        std::strcmp(out, "ab\r\nabcdefg-\r\nklm xy") == 0,
        "a hyphen that does not fit moves its word"
    );
    wrap_text("ab abcdefg-klm xy", 61, measure, nullptr, out, sizeof(out));
    expect(std::strcmp(out, "ab abcdefg\r\nklm xy") == 0, "3.1c breaks there and loses the hyphen");
    wrap_text("alpha beta gamma", 60, measure, nullptr, out, sizeof(out));
    expect(std::strcmp(out, "alpha\r\nbeta gamma") == 0, "a line reaching the width breaks");
    // Chinese breaks between its characters, never before a full-width
    // comma, and keeps a highlight's markers with the words they mark.
    wrap_text("建造完成，单位已就绪", 61, measure, nullptr, out, sizeof(out));
    expect(std::strcmp(out, "建造完\r\n成，单\r\n位已就\r\n绪") == 0, "Chinese breaks by kinsoku");
    wrap_text("我们&R敌人&北方", 61, measure, nullptr, out, sizeof(out));
    expect(std::strcmp(out, "我们\r\n&R敌人&\r\n北方") == 0, "markers stay with their words");
    // Korean is written with spaces between its words: a row breaks at a
    // space and keeps each word whole, 18 pixels a syllable here, and a
    // word wider than a row breaks inside only then.
    wrap_text("스파이더 한 대는 반드시 살아남아야", 121, measure, nullptr, out, sizeof(out));
    expect(
        std::strcmp(out, "스파이더 한\r\n대는 반드시\r\n살아남아야") == 0,
        "Korean breaks at its spaces"
    );
    wrap_text("살아남아야합니다 반드시", 121, measure, nullptr, out, sizeof(out));
    expect(
        std::strcmp(out, "살아남아야합\r\n니다 반드시") == 0,
        "a Korean word wider than a row breaks inside"
    );
    // Out of room, the text stops before a character it cannot hold whole.
    wrap_text("指挥官", 100, measure, nullptr, out, 10);
    expect(std::strcmp(out, "指挥") == 0, "Chinese cut between characters");

    char reflowed[128];
    reflow_span_text("go &Rred\r\nzone& now", reflowed, sizeof(reflowed));
    expect(std::strcmp(reflowed, "go &Rred&\r\n&Rzone& now") == 0, "span reopened after break");
    reflow_span_text(
        "a\xff"
        "b",
        reflowed,
        sizeof(reflowed)
    );
    expect(std::strcmp(reflowed, "a") == 0, "0xFF ends the text");
}

/// Lays out the first page of a one-row briefing.
///
/// @param text the briefing
/// @return the page
const BriefingPage& first_page(const std::string& text) {
    BriefingPager pager{};
    briefing_pager_reset(&pager, text.c_str());
    BriefingRegion region{10, 20, 30, 10};
    static BriefingPage page;
    briefing_next_page(&pager, &region, measure, nullptr, &page);
    return page;
}

/// Repeats a text.
///
/// @param text the text
/// @param count how many times
/// @return the text `count` times
std::string repeated(std::string_view text, int count) {
    std::string out;
    for (int i = 0; i < count; ++i)
        out += text;
    return out;
}

void chinese_row_tests() {
    // A row holds as many characters as a row of the game's code page, 127,
    // whatever their bytes: 50 hanzi, 150 bytes, are kept whole, and of 130
    // the first 127 are, never part of one.
    expect(first_page(repeated("中", 50)).rows[0].text == repeated("中", 50), "50 hanzi kept");
    expect(first_page(repeated("中", 130)).rows[0].text == repeated("中", 127), "127 hanzi kept");
    // Cyrillic letters take two bytes each: a row of 120 is kept whole.
    expect(
        first_page(repeated("ї", 120)).rows[0].text == repeated("ї", 120),
        "120 Cyrillic letters kept"
    );
    // A highlight holds as many as its row.
    const BriefingPage& marked = first_page("&G" + repeated("ж", 100) + "&");
    expect(
        marked.highlight_count == 1 && marked.highlights[0].text == repeated("ж", 100),
        "a highlight keeps 100 Cyrillic letters"
    );
    // The game's own one-byte text ends at 127 bytes, as in 3.1c, whether
    // its bytes are ASCII or the code page's own (0x92, a closing quote).
    expect(first_page(repeated("a", 130)).rows[0].text == repeated("a", 127), "127 ASCII bytes");
    expect(
        first_page(repeated("\x92", 130)).rows[0].text == repeated("\x92", 127),
        "127 code page bytes"
    );
}

/// Wraps and lays out a briefing as a screen does and gives the rows of its
/// first page.
///
/// @param text the briefing
/// @param width the row width the briefing is wrapped to, in pixels
/// @param[out] highlights the highlighted words of the page
/// @return the page's rows up to the last with text, each without the CR that
///         ends it
std::vector<std::string>
laid_out_rows(const std::string& text, int32_t width, std::vector<std::string>& highlights) {
    static char wrapped[kBriefingTextBytes];
    static char reflowed[kBriefingTextBytes + kReflowSlack];
    wrap_text(text.c_str(), width, measure, nullptr, wrapped, sizeof(wrapped));
    reflow_span_text(wrapped, reflowed, sizeof(reflowed));
    BriefingPager pager{};
    briefing_pager_reset(&pager, reflowed);
    const BriefingRegion region{0, 0, 10 * 12, 10};
    static BriefingPage page;
    briefing_next_page(&pager, &region, measure, nullptr, &page);
    std::vector<std::string> rows;
    for (uint32_t i = 0; i < page.row_count; ++i) {
        std::string row = page.rows[i].text;
        if (!row.empty() && row.back() == '\r')
            row.pop_back();
        rows.push_back(row);
    }
    // The rows after the briefing's last are empty.
    while (!rows.empty() && rows.back().empty())
        rows.pop_back();
    highlights.clear();
    for (uint32_t i = 0; i < page.highlight_count; ++i)
        highlights.emplace_back(page.highlights[i].text);
    return rows;
}

/// Tells whether every row of a page is narrower than the width it was
/// wrapped to, as laid out on the screen, markers gone.
///
/// @param rows the rows
/// @param width the width, in pixels
/// @return true when each row fits
bool rows_fit(const std::vector<std::string>& rows, int32_t width) {
    return std::all_of(rows.begin(), rows.end(), [&](const std::string& row) {
        return measure(nullptr, row.c_str()) < width;
    });
}

// A highlighted line stands on its row whatever its line breaks are, and
// every row of a page stays inside the width it was wrapped to. A line
// break alone after a highlight's closing marker, as a pack's briefings can
// have, once joined the line after it to the highlight's row, a row of both
// lines' width.
void highlight_row_tests() {
    const int32_t width = 150; // 24 characters of 6 pixels fit
    const std::string lines =
        "Mission 1~~&Y*Hold the line*&~CORE strikes the base from the north and the south~End";
    const std::vector<std::string> expected{
        "Mission 1",
        "",
        "*Hold the line*",
        "CORE strikes the base",
        "from the north and the",
        "south",
        "End"
    };
    std::vector<std::string> highlights;
    for (const char* ending : {"\n", "\r\n"}) {
        std::string text;
        for (const char c : lines)
            text += c == '~' ? std::string(ending) : std::string(1, c);
        const auto rows = laid_out_rows(text, width, highlights);
        const std::string what = std::strlen(ending) == 1 ? "line feeds" : "CR LF line ends";
        expect(rows == expected, ("the rows of a highlighted line with " + what).c_str());
        if (rows != expected)
            for (const auto& row : rows)
                std::cerr << "  row [" << row << "]\n";
        expect(rows_fit(rows, width), ("each row fits the width, with " + what).c_str());
        expect(
            highlights == std::vector<std::string>{"*Hold the line*"},
            ("the highlight stays on its row, with " + what).c_str()
        );
    }

    // A highlight followed by body text on its own row, and one that opens
    // the row, break at the width as the rest of the text does.
    const auto rows = laid_out_rows(
        "&G*Go*& to the beacon and build a base there&R*Now*&\nThen wait", width, highlights
    );
    expect(rows_fit(rows, width), "a row with a highlight and body text fits the width");
    expect(
        highlights.size() == 2 && highlights[0] == "*Go*" && highlights[1] == "*Now*",
        "both highlights are laid out"
    );
    expect(rows.back() == "Then wait", "the line after a highlight's row starts its own row");
    if (rows.back() != "Then wait")
        for (const auto& row : rows)
            std::cerr << "  row [" << row << "]\n";

    // A highlight left open across a blank line, in a briefing with CR LF
    // line ends, as 3.1c's German ArmCampaign22 has one: the reflow writes a
    // closing and an opening marker at each break, which leaves the pager's
    // open and closed state one off, so a later opening marker takes a CR for
    // its colour code, and the line feed after it is not a line feed alone.
    // These rows are as 3.1c draws them (the last holds three lines and two
    // line feeds, each line with a stray colour letter) and must not change.
    const std::vector<std::string> open_highlight{
        "Mission 1", "Do this", "&", "RNext line\nREnd\nR"
    };
    const auto open_rows =
        laid_out_rows("Mission 1\r\n&RDo this\r\n\r\nNext line\r\nEnd\r\n", width, highlights);
    expect(
        open_rows == open_highlight, "an open highlight across a blank line lays out as in 3.1c"
    );
    if (open_rows != open_highlight)
        for (const auto& row : open_rows)
            std::cerr << "  row [" << row << "]\n";
}

void page_tests() {
    const char* text = "l1\nl2\nl3\nl4\nl5";
    expect(find_text_page(text, 2, 0) == text, "page 0");
    expect(std::strcmp(find_text_page(text, 2, 1), "l3\nl4\nl5") == 0, "page 1");
    expect(std::strcmp(find_text_page(text, 2, 2), "l5") == 0, "page 2");
    expect(find_text_page(text, 2, 3) == nullptr, "past end");
    expect(find_text_page("", 2, 1) == nullptr, "empty text");

    BriefingPager pager{};
    briefing_pager_reset(&pager, "Go to &Gthe beacon& now\r\nsecond\r\nthird\r\nfourth\r\nfifth");
    BriefingRegion region{10, 20, 30, 10}; // 3 rows
    static BriefingPage page;
    briefing_next_page(&pager, &region, measure, nullptr, &page);
    expect(page.page == 0 && page.row_count == 3, "first page rows");
    expect(std::strcmp(page.rows[0].text, "Go to the beacon now\r") == 0, "markers stripped");
    expect(page.rows[0].x == 15 && page.rows[0].y == 25 && page.rows[1].y == 35, "row placement");
    expect(
        page.highlight_count == 1 && std::strcmp(page.highlights[0].text, "the beacon") == 0,
        "highlight text"
    );
    expect(
        page.highlights[0].x == 15 + 6 * 6 && page.highlights[0].color == HighlightColor::green,
        "highlight position and colour"
    );
    expect(
        page.more == MoreLabel::more && std::strcmp(more_label_text(page.more), "MORE...") == 0,
        "more label"
    );
    briefing_next_page(&pager, &region, measure, nullptr, &page);
    expect(page.page == 1 && std::strncmp(page.rows[0].text, "fourth", 6) == 0, "second page");
    expect(page.more == MoreLabel::back_to_start, "back to start label");
    briefing_next_page(&pager, &region, measure, nullptr, &page);
    expect(page.page == 0, "wraps to first page");

    // A region 148 rows high with 21-row lines, as a Korean or Chinese
    // briefing's are, has seven whole lines. Rows start half a line down,
    // and the modern fonts' line reaches 18 rows below each row's pen row:
    // the seventh row's would end at 10 + 6 * 21 + 18 = 154, past the
    // region, so the page holds six, and the next page starts at the
    // seventh line.
    briefing_pager_reset(&pager, "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8");
    BriefingRegion reaching{24, 17, 148, 21, 18};
    briefing_next_page(&pager, &reaching, measure, nullptr, &page);
    expect(page.row_count == 6, "rows whose line ends inside the region");
    expect(
        page.rows[5].y - reaching.y + reaching.row_reach <= reaching.height,
        "the last row's line ends inside the region"
    );
    briefing_next_page(&pager, &reaching, measure, nullptr, &page);
    expect(page.page == 1 && std::strncmp(page.rows[0].text, "7", 1) == 0, "the next page");
    // Without a reach, the game font's rows: as many as the height has lines.
    briefing_pager_reset(&pager, "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8");
    BriefingRegion font_rows{24, 17, 148, 21};
    briefing_next_page(&pager, &font_rows, measure, nullptr, &page);
    expect(page.row_count == 7, "the game font's rows fill the region's lines");
    // A row that cannot fit still shows.
    briefing_pager_reset(&pager, "1\r\n2");
    BriefingRegion short_region{24, 17, 30, 21, 40};
    briefing_next_page(&pager, &short_region, measure, nullptr, &page);
    expect(page.row_count == 1, "at least one row");
}

void single_player_tests() {
    Recorder r;
    const auto host = recording_host(r);
    auto* setup = new CampaignSetup;
    campaign_setup_init(setup);
    single_player_click(setup, &host, "NewCamp");
    expect(
        r.signal == signal::new_campaign && logged(r, "sound BigButton") && logged(r, "cursor 20"),
        "NewCamp raises signal 10"
    );
    r = {};
    r.disc = false;
    single_player_click(setup, &host, "AnyMsn");
    expect(
        r.signal == 0 && logged(r, std::string("message ") + kCampaignDiscMessage) &&
            logged(r, "clear"),
        "AnyMsn without disc"
    );
    r = {};
    single_player_click(setup, &host, "PrevMenu");
    expect(r.signal == signal::back, "PrevMenu goes back");

    r = {};
    check_cheat_code(setup, &host, "DRDEATH");
    expect(
        (setup->unlock_flags & kAllMissionsUnlocked) != 0 && logged(r, "value AnyMsn=1"),
        "cheat unlocks"
    );
    check_cheat_code(setup, &host, "DRDEATH");
    expect((setup->unlock_flags & kAllMissionsUnlocked) == 0, "cheat toggles back");
    check_cheat_code(setup, &host, "DRDEATX");
    expect((setup->unlock_flags & kAllMissionsUnlocked) == 0, "wrong code ignored");

    // ai.difficulty-names: the radio label is the name the difficulty carries.
    r = {};
    show_difficulty(0, &host);
    expect(logged(r, "group Easy"), "difficulty 0 shows Easy");
    using Names = oa::data::match_rules::AiDifficultyNamesNames;
    oa::data::match_rules::AiDifficultyNames swapped{};
    swapped.names = {Names::hard, Names::medium, Names::easy};
    r = {};
    show_difficulty(0, &host, swapped);
    expect(logged(r, "group Hard") && !logged(r, "group Easy"), "swapped: 0 shows Hard");
    r = {};
    show_difficulty(1, &host, swapped);
    expect(logged(r, "group Medium"), "swapped: 1 shows Medium");

    r = {};
    setup->difficulty = 2;
    campaign_setup_click(setup, &host, "Difficulty");
    expect(setup->difficulty == 0 && logged(r, "sound SmlButton"), "difficulty cycles");
    campaign_setup_click(setup, &host, "Side1");
    expect(
        setup->side == 1 && setup->player_side[0] == 1 && setup->player_side[1] == 0 &&
            logged(r, "group Core") && logged(r, "sound SideSelect2"),
        "side 1 selection"
    );

    const char names[2][kSideNameBytes] = {"ARM", "CORE"};
    char list[64];
    build_side_name_list(names, 2, list, sizeof(list));
    expect(std::strcmp(list, "Arm") == 0 && std::strcmp(list + 4, "Core") == 0, "side name list");
    campaign_setup_free(setup);
    delete setup;
}

// NEWGAME.GUI: two campaign files make the side buttons choose the campaign
// over newcampaign4x with Difficulty focused; three list the campaigns over
// newcampaign4; any mission moves the lists over playanygame4.
int32_t g_camps = 0;

void new_game_tests() {
    Recorder r;
    auto host = recording_host(r);
    host.load_background = [](void* c, const char* n) {
        recorder(c)->log.push_back(std::string("background ") + n);
    };
    host.set_control_y = [](void* c, const char* n, uint8_t type, int16_t y) {
        recorder(c)->log.push_back(
            std::string("y ") + n + "/" + std::to_string(type) + "=" + std::to_string(y)
        );
    };
    host.set_control_height = [](void* c, const char* n, uint8_t type, int16_t height) {
        recorder(c)->log.push_back(
            std::string("height ") + n + "/" + std::to_string(type) + "=" + std::to_string(height)
        );
    };
    host.zero_sequence_origins = [](void* c, const char* n) {
        recorder(c)->log.push_back(std::string("hotspots ") + n);
    };
    host.focus_control = [](void* c, const char* n) {
        recorder(c)->log.push_back(std::string("focus ") + n);
    };
    oa::data::campaign::CampaignFiles files{};
    files.count = [](void*, const char*) { return g_camps; };
    auto* setup = new CampaignSetup;
    campaign_setup_init(setup);
    setup->env.files = &files;

    g_camps = 2;
    new_game_enter(setup, &host, false);
    expect(
        logged(r, "background newcampaign4x") && setup->fixed_side_campaign &&
            logged(r, "focus Difficulty") && !logged(r, "value Campaign=1"),
        "two campaigns: side buttons pick the campaign"
    );
    expect(
        logged(r, "hotspots Side0") && logged(r, "hotspots Side1") && logged(r, "group Arm") &&
            logged(r, "cursor 19"),
        "side hotspots, side group and the ready cursor"
    );

    r = {};
    g_camps = 3;
    new_game_enter(setup, &host, false);
    expect(
        logged(r, "background newcampaign4") && !setup->fixed_side_campaign &&
            logged(r, "value Campaign=1") && logged(r, "value CampaignKnob=1") &&
            logged(r, "list Campaign=0") && logged(r, "focus Campaign") &&
            !logged(r, "y Campaign/2=308"),
        "three campaigns: the campaign list"
    );

    r = {};
    g_camps = 1;
    new_game_enter(setup, &host, true);
    expect(
        logged(r, "background playanygame4") && !setup->fixed_side_campaign && setup->any_mission &&
            logged(r, "y Campaign/2=308") && logged(r, "height Campaign/2=48") &&
            logged(r, "y CampaignKnob/4=308") && logged(r, "height Missions/2=62") &&
            logged(r, "value Missions=1") && logged(r, "focus Missions"),
        "any mission: lists moved, missions focused"
    );
    campaign_setup_free(setup);
    delete setup;
}

// Two archives offer one campaign file, and each offers campaigns of its own.
// The shared file is listed once, in the top layer's spelling, and that
// layer's mission file is the one loaded. A different file is listed even
// when its mission title matches.
void layered_campaign_list_tests() {
    namespace fs = std::filesystem;

    struct Scratch {
        fs::path path;

        Scratch() {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path = fs::temp_directory_path() / ("oa-campaign-layers-" + std::to_string(stamp));
            fs::create_directories(path);
        }

        ~Scratch() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }

        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;
    } scratch;

    const auto tdf = [](std::string_view side, std::string_view title, std::string_view mission) {
        const std::string text = std::string("[HEADER]\n{\ncampaignside=") + std::string(side) +
                                 ";\n}\n[MISSION0]\n{\nmissionname=" + std::string(title) +
                                 ";\nmissionfile=" + std::string(mission) + ";\n}\n";
        return std::vector<uint8_t>(text.begin(), text.end());
    };
    const auto write_archive = [&](const char* name, const std::vector<oa::HpiWriteFile>& files) {
        const auto bytes = oa::write_hpi(files);
        std::ofstream stream(scratch.path / name, std::ios::binary | std::ios::trunc);
        stream.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
        expect(static_cast<bool>(stream), "layered: archive written");
    };
    write_archive(
        "top.hpi",
        {{"camps/Arm Campaign.tdf", tdf("ARM", "Shared Title", "TOP.ota")},
         {"camps/Second Campaign.tdf", tdf("ARM", "Second", "SECOND.ota")}}
    );
    write_archive(
        "bottom.hpi",
        {{"camps/ARM CAMPAIGN.tdf", tdf("ARM", "Shared Title", "BOT.ota")},
         {"camps/Arm Campaign - Core Contingency.tdf", tdf("ARM", "Contingency", "CC.ota")},
         {"camps/Ridge Campaign.tdf", tdf("ARM", "Shared Title", "RIDGE.ota")},
         {"camps/Core Campaign.tdf", tdf("CORE", "Core", "CORE.ota")},
         {"camps/CORE CAMPAIGN.tdf", tdf("CORE", "Core", "CORE-LOWER.ota")}}
    );

    oa::AssetStore store(scratch.path);
    store.mount(scratch.path / "top.hpi");
    store.mount(scratch.path / "bottom.hpi");
    const auto files = oa::data::campaign::campaign_asset_files(store);

    const auto listed = [&](const char* side) {
        char packed[4096];
        const int32_t count =
            oa::data::campaign::campaign_load_names(&files, side, packed, sizeof(packed));
        std::vector<std::string> names;
        for (const char* cursor = packed; *cursor != '\0'; cursor += std::strlen(cursor) + 1)
            names.emplace_back(cursor);
        expect(static_cast<int32_t>(names.size()) == count, "layered: name count matches");
        return names;
    };
    const auto arm = listed("ARM");
    const auto count_named = [](const std::vector<std::string>& names, const char* wanted) {
        return std::count(names.begin(), names.end(), wanted);
    };
    expect(arm.size() == 4, "layered: four Arm campaigns");
    expect(count_named(arm, "Arm Campaign") == 1, "layered: the shared file is listed once");
    expect(count_named(arm, "ARM CAMPAIGN") == 0, "layered: the top layer's spelling is kept");
    expect(
        count_named(arm, "Arm Campaign - Core Contingency") == 1,
        "layered: a different campaign stays"
    );
    expect(count_named(arm, "Ridge Campaign") == 1, "layered: a matching title stays");
    expect(count_named(arm, "Second Campaign") == 1, "layered: the top layer's own campaign stays");
    expect(count_named(arm, "Core Campaign") == 0, "layered: the other side stays off the list");

    const auto core = listed("CORE");
    expect(core.size() == 1 && core[0] == "Core Campaign", "layered: Core's shared file once");

    auto* campaign = new oa::data::campaign::CampaignFile;
    oa::data::campaign::campaign_file_init(campaign);
    campaign->kind = oa::data::campaign::SessionKind::campaign;
    const oa::data::campaign::CampaignEnv env{&files, nullptr, 0, 0};
    expect(
        oa::data::campaign::campaign_load_file(campaign, &env, "Arm Campaign"),
        "layered: the shared campaign loads"
    );
    char mission[128];
    expect(
        oa::data::campaign::campaign_mission_file(campaign, 0, mission, sizeof(mission)) &&
            std::strcmp(mission, "TOP.ota") == 0,
        "layered: the top layer's mission file wins"
    );
    oa::data::campaign::campaign_file_free(campaign);
    delete campaign;
}

// ---- installed data: briefing panel over the installed campaigns ----

int32_t fixed_random(void*) {
    return 12345;
}

// The campaign screens over the installed game's campaigns, read through its
// store as the game reads them.
void installed_data_tests(const oa::AssetStore& assets) {
    const oa::data::campaign::CampaignFiles files =
        oa::data::campaign::campaign_asset_files(assets);
    Recorder r;
    const auto host = recording_host(r);
    auto* setup = new CampaignSetup;
    campaign_setup_init(setup);
    auto* campaign = new oa::data::campaign::CampaignFile;
    oa::data::campaign::campaign_file_init(campaign);
    setup->campaign = campaign;
    setup->env = {&files, nullptr, 0, 0};
    oa::base::text::copy_terminated(setup->side_names[0], "ARM");
    oa::base::text::copy_terminated(setup->side_names[1], "CORE");
    setup->side_count = 2;

    // main -> single player -> new campaign -> briefing -> mission start
    single_player_click(setup, &host, "NewCamp");
    expect(r.signal == signal::new_campaign, "installed: new campaign signal");
    setup->fixed_side_campaign = true;
    campaign_setup_click(setup, &host, "Side0");
    expect(setup->campaign_count >= 4, "installed: ARM campaigns listed");
    r = {};
    campaign_setup_click(setup, &host, "Start");
    expect(r.signal == signal::campaign_briefing, "installed: Start raises the briefing signal");
    expect(
        std::strcmp(campaign->campaign_name, "Arm Campaign") == 0 && campaign->mission_index == 0,
        "installed: Arm Campaign mission 0 bound"
    );
    expect(campaign->unit_count > 0, "installed: mission units parsed");
    // The checks below read the bound mission; without one they end here.
    if (r.signal != signal::campaign_briefing || campaign->unit_count <= 0)
        return;

    // Unit availability over camps/useonly/AC01.TDF: every record past slot 0 loses
    // the bit, then each listed [UNIT] (case-insensitive) regains it.
    auto* table = new oa::UnitDef[6]{};
    const char* names[] = {"", "ARMCOM", "armpw", "ARMCK", "CORCOM", "CORAK"};
    for (int i = 0; i < 6; ++i) {
        oa::base::text::copy_terminated(table[i].unit_name, names[i]);
        table[i].flags = OA_UNIT_DEF_FLAG_AVAILABLE | OA_UNIT_DEF_FLAG_HAS_WEAPONS;
    }
    expect(
        load_unit_availability(table, 6, campaign, &files), "installed: AC01 use-only file loads"
    );
    const bool available[] = {true, true, true, false, false, true};
    for (int i = 0; i < 6; ++i) {
        expect(((table[i].flags & OA_UNIT_DEF_FLAG_AVAILABLE) != 0) == available[i], names[i]);
        expect((table[i].flags & OA_UNIT_DEF_FLAG_HAS_WEAPONS) != 0, "installed: other flags kept");
    }
    auto* unbound = new oa::data::campaign::CampaignFile;
    oa::data::campaign::campaign_file_init(unbound);
    table[3].flags = OA_UNIT_DEF_FLAG_AVAILABLE;
    expect(
        !load_unit_availability(table, 6, unbound, &files) &&
            table[3].flags == OA_UNIT_DEF_FLAG_AVAILABLE,
        "installed: no use-only file leaves the table"
    );
    oa::data::campaign::campaign_file_free(unbound);
    delete unbound;
    delete[] table;

    auto* panel = new BriefingPanel;
    BriefingRegion region{20, 60, 200, 14};
    briefing_panel_enter(
        panel, campaign, 0, &region, 300, measure, nullptr, fixed_random, nullptr, &host, true
    );
    expect(
        panel->page.row_count > 0 && panel->page.rows[0].text[0] != '\0',
        "installed: briefing text laid out"
    );
    expect(
        panel->wind_walk >= campaign->min_wind && panel->wind_walk <= campaign->max_wind,
        "installed: wind in range"
    );
    // The narration is heard two seconds after it is asked for.
    const auto narrate =
        std::string("narrate ") +
        oa::data::campaign::campaign_path(campaign, oa::data::campaign::CampaignPath::narration) +
        " after 60";
    expect(logged(r, narrate), "installed: narration started");
    r = {};
    briefing_click(panel, campaign, &host, "Start", &region, measure, nullptr, true);
    expect(
        r.signal == signal::start_mission && logged(r, "stop narration"), "installed: mission start"
    );

    // SHUTUP stops the narration; clicked again it plays the narration anew,
    // from its start. PrevMenu stops it and goes back.
    r = {};
    briefing_click(panel, campaign, &host, "SHUTUP", &region, measure, nullptr, true);
    expect(
        !panel->narration_on && logged(r, "stop narration") && !logged(r, narrate),
        "installed: SHUTUP off stops the narration"
    );
    r = {};
    briefing_click(panel, campaign, &host, "SHUTUP", &region, measure, nullptr, true);
    expect(
        panel->narration_on && logged(r, narrate) && !logged(r, "stop narration"),
        "installed: SHUTUP on plays the narration again"
    );
    r = {};
    briefing_click(panel, campaign, &host, "PrevMenu", &region, measure, nullptr, true);
    expect(
        r.signal == signal::back && logged(r, "stop narration"),
        "installed: PrevMenu stops the narration and goes back"
    );
    // The ticker turns SHUTUP off once the narration has played out.
    r = {};
    panel->ticker_next_ms = 0;
    briefing_ticker(panel, 0, 0, true, 0, &host);
    expect(panel->narration_on, "installed: SHUTUP stays on while the narration plays");
    briefing_ticker(panel, kTickerIntervalMs, 0, false, 0, &host);
    expect(
        !panel->narration_on && logged(r, "value SHUTUP=0"),
        "installed: SHUTUP turns off once the narration has played out"
    );
    // Where narration may not play, as while a match runs, SHUTUP plays nothing.
    r = {};
    briefing_click(panel, campaign, &host, "SHUTUP", &region, measure, nullptr, false);
    expect(
        panel->narration_on && !logged(r, narrate),
        "installed: SHUTUP plays nothing where narration may not play"
    );
    // Every briefing opens with SHUTUP on and its narration playing, whatever
    // the last one was left at.
    r = {};
    panel->narration_on = false;
    briefing_panel_enter(
        panel, campaign, 0, &region, 300, measure, nullptr, fixed_random, nullptr, &host, true
    );
    expect(
        panel->narration_on && logged(r, narrate),
        "installed: a briefing opens with its narration on"
    );

    // BRIEFING.GUI over the paused mission.
    auto* ingame = new BriefingPanel;
    r = {};
    ingame_briefing_enter(ingame, campaign, &region, 300, measure, nullptr, &host);
    expect(
        logged(r, "attributes MOREBAR&~16") && logged(r, "attributes TextRegion&~16"),
        "installed: in-game briefing clears the text_list attribute"
    );
    expect(
        ingame->page.row_count > 0 && ingame->page.page == 0,
        "installed: in-game briefing first page"
    );
    r = {};
    ingame_briefing_click(ingame, &host, "MOREBAR", &region, measure, nullptr);
    expect(
        logged(r, "sound Options") && logged(r, "dirty") && logged(r, "clear"),
        "installed: MOREBAR turns the page"
    );
    expect(ingame->page.page == 1 || ingame->page.more != MoreLabel::more, "installed: next page");
    r = {};
    ingame_briefing_click(ingame, &host, "OK", &region, measure, nullptr);
    expect(logged(r, "sound Options") && !logged(r, "clear"), "installed: OK only plays its sound");
    ingame_briefing_click(ingame, &host, nullptr, &region, measure, nullptr);
    expect(ingame->pager.text == nullptr, "installed: closing drops the page layout");
    delete ingame;

    // Pagination over every installed briefing.
    std::size_t pages = 0;
    std::size_t briefs = 0;
    for (const auto& name : assets.list_effective("camps/briefs", ".txt")) {
        const auto bytes = oa::test::read_game_file(assets, name);
        const std::string text(bytes.begin(), bytes.end());
        static char wrapped[kBriefingTextBytes];
        static char reflowed[kBriefingTextBytes + kReflowSlack];
        wrap_text(text.c_str(), 300, measure, nullptr, wrapped, sizeof(wrapped));
        reflow_span_text(wrapped, reflowed, sizeof(reflowed));
        BriefingPager pager{};
        briefing_pager_reset(&pager, reflowed);
        static BriefingPage page;
        for (int i = 0; i < 64; ++i) {
            briefing_next_page(&pager, &region, measure, nullptr, &page);
            ++pages;
            if (page.more != MoreLabel::more)
                break;
        }
        ++briefs;
    }
    expect(briefs > 50, "installed: briefings paged");
    std::cout << "campaign UI installed: " << briefs << " briefings, " << pages << " pages\n";

    delete panel;
    oa::data::campaign::campaign_file_free(campaign);
    delete campaign;
    campaign_setup_free(setup);
    delete setup;
}

// The SOLARSYSTEM lines and the panorama's scroll: gravity reads as a
// multiple of the standard 112, the wind stays in the mission's range, and
// the panorama steps one pixel every third tick and wraps at its strip.
// An empty use-only list, as The Lost Legacy Campaign's missions 13 and 14
// ship, leaves every unit type available: 3.1c's TDF loader refuses it.
void empty_use_only_tests() {
    auto* campaign = new oa::data::campaign::CampaignFile;
    oa::data::campaign::campaign_file_init(campaign);
    oa::base::text::copy_terminated(
        campaign->paths[static_cast<uint32_t>(oa::data::campaign::CampaignPath::use_only)],
        "camps/useonly/missiontll13.tdf"
    );

    struct Empty {
        static int32_t size(void*, const char*) { return 0; }

        static int32_t read(void*, const char*, char*, uint32_t) { return 0; }
    };

    oa::data::campaign::CampaignFiles files{};
    files.size = Empty::size;
    files.read = Empty::read;
    auto* table = new oa::UnitDef[3]{};
    const char* names[] = {"", "TLLCOM", "ARMCOM"};
    for (int i = 0; i < 3; ++i) {
        oa::base::text::copy_terminated(table[i].unit_name, names[i]);
        table[i].flags = OA_UNIT_DEF_FLAG_AVAILABLE;
    }
    expect(
        !load_unit_availability(table, 3, campaign, &files),
        "empty use-only: the empty file is refused"
    );
    for (int i = 0; i < 3; ++i)
        expect(
            (table[i].flags & OA_UNIT_DEF_FLAG_AVAILABLE) != 0,
            "empty use-only: every unit type stays available"
        );
    delete[] table;
    oa::data::campaign::campaign_file_free(campaign);
    delete campaign;
}

void solar_system_tests() {
    auto* campaign = new oa::data::campaign::CampaignFile;
    campaign->min_wind = 100;
    campaign->max_wind = 2500;
    campaign->gravity = 112;
    auto* panel = new BriefingPanel;
    constexpr int32_t strip = 4;
    briefing_solar_system_tick(panel, campaign, nullptr, nullptr, 1, strip, nullptr);
    expect(std::string(panel->gravity_label) == "Gravity : 1.0", "solar: standard gravity");
    expect(
        panel->wind_walk >= campaign->min_wind && panel->wind_walk <= campaign->max_wind,
        "solar: wind within the mission's range"
    );
    expect(panel->panorama_scroll == 1, "solar: the first update scrolls a pixel");
    campaign->gravity = 168;
    for (const uint32_t tick : {2U, 3U})
        briefing_solar_system_tick(panel, campaign, nullptr, nullptr, tick, strip, nullptr);
    expect(std::string(panel->gravity_label) == "Gravity : 1.5", "solar: gravity of 168");
    expect(panel->panorama_scroll == 1, "solar: no step until its deadline has passed");
    for (const uint32_t tick : {4U, 7U, 10U})
        briefing_solar_system_tick(panel, campaign, nullptr, nullptr, tick, strip, nullptr);
    expect(panel->panorama_scroll == 0, "solar: a step every third tick wraps at the strip");
    delete panel;
    delete campaign;
}

} // namespace

/// Answers campaign names as a language's text table does: 3.1c's German
/// table names the Arm campaign, and a pack names a Core Contingency one by
/// its name without the space its file ends in.
///
/// @param text the text
/// @return its translation, or null for any other text
const char* translated_campaigns(void*, const char* text) {
    if (std::strcmp(text, "Arm Campaign") == 0)
        return "Arm Missionen";
    if (std::strcmp(text, "Core Campaign - Core Contingency") == 0)
        return "Core-Feldzug - Core Contingency";
    return nullptr;
}

// The Campaign list shows a campaign's name from the text table, and the
// file's own name where the table has none or nothing translates.
void campaign_name_tests() {
    FrontendHost host{};
    expect(shown_campaign_name(&host, "Arm Campaign") == "Arm Campaign", "no table keeps the name");
    expect(
        shown_campaign_name(nullptr, "Arm Campaign") == "Arm Campaign", "no host keeps the name"
    );
    host.translate = translated_campaigns;
    expect(
        shown_campaign_name(&host, "Arm Campaign") == "Arm Missionen", "a listed name is translated"
    );
    expect(
        shown_campaign_name(&host, "Core Campaign - Core Contingency ") ==
            "Core-Feldzug - Core Contingency",
        "a name with an end space is looked up without it"
    );
    expect(
        shown_campaign_name(&host, "Battle Tactics - Arm Very Short") ==
            "Battle Tactics - Arm Very Short",
        "a name the table lacks shows as the file is named"
    );
    expect(
        shown_campaign_name(&host, "Arm Campaign - Core Contingency ") ==
            "Arm Campaign - Core Contingency ",
        "an untranslated name keeps its end space"
    );
}

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv)) {
        installed_data_tests(
            oa::test::require_game_assets("the campaign screens over the installed campaigns")
        );
    } else {
        wrap_tests();
        page_tests();
        chinese_row_tests();
        highlight_row_tests();
        single_player_tests();
        campaign_name_tests();
        new_game_tests();
        layered_campaign_list_tests();
        empty_use_only_tests();
        solar_system_tests();
    }
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "campaign UI tests passed\n";
    return 0;
}
