// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// ui-map-browser: the map browser's model (merging the four sources, every
// filter and their combination, the search, the order, the primary button
// and the cards' words), then its screen (layout_test.cpp).
// Given --frames DIR, it writes the three size classes' pictures there and
// does not run the tests.
#include "oa/test/check.hpp"
#include "oa/ui/map_browser/model.hpp"

#include <cstddef>
#include <cstdio>
#include <optional>
#include <stdint.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// Runs the screen's tests, in layout_test.cpp.
void layout_tests();

/// Writes map-browser-compact.png, map-browser-regular.png and
/// map-browser-large.png into directory, in the modern look when the fonts
/// are beside the test, and returns
/// 0. Returns 1 when a picture could not be written. A missing game
/// directory ends the process as a skipped test.
///
/// @param directory the folder
/// @return 0 when the three pictures were written
int write_map_browser_frames(const char* directory);

namespace {

namespace mb = oa::ui::map_browser;
using Origin = mb::MapCard::Origin;
using Fit = mb::MapCard::Fit;

/// Returns a card.
///
/// @param name the game name
/// @param title the title
/// @param players the most players
/// @param size the size text
/// @return the card
mb::MapCard
card(std::string_view name, std::string_view title, int32_t players, std::string_view size) {
    mb::MapCard made;
    made.name = std::string(name);
    made.title = std::string(title);
    made.players = players;
    made.size = std::string(size);
    return made;
}

/// Returns the shown cards' names, in order.
///
/// @param browser the browser
/// @return the names
std::vector<std::string> shown_names(const mb::Browser& browser) {
    std::vector<std::string> names;
    for (const std::size_t index : browser.shown)
        names.push_back(browser.cards[index].name);
    return names;
}

/// A browser of three game maps, one mod map, two installed and three
/// online, one online map the same as an installed one.
///
/// @return the browser, its filters applied
mb::Browser sample() {
    std::vector<mb::MapCard> game{
        card("Painted Desert", "Painted Desert", 4, "16 x 16"),
        card("Coast To Coast", "Coast to Coast", 2, "20 x 20"),
        card("Big Wars", "Big Wars", 8, "32 x 32"),
    };
    game[0].source_name = "TA 3.1c";
    game[1].source_name = "Core Contingency";
    game[2].source_name = "Battle Tactics";
    std::vector<mb::MapCard> mod{card("ridge_valley", "Ridge Valley", 3, "14 x 14")};
    mod[0].source_name = "Ridge";
    std::vector<mb::MapCard> installed{
        card("tidewater@lowlands", "Tidewater", 4, "24x24"),
        card("fen_crossing@lowlands", "Fen Crossing", 3, "18x18"),
    };
    installed[0].pack_name = "Lowlands";
    installed[1].pack_name = "Lowlands";
    std::vector<mb::MapCard> online{
        card("isle_of_ashes@archipelago", "Isle of Ashes", 4, "16x16"),
        card("tidewater@lowlands", "Tidewater (online)", 4, "24x24"),
        card("black_shoals@archipelago", "Black Shoals", 6, "12x12"),
    };
    online[0].pack_name = "Archipelago Pack";
    online[2].pack_name = "Archipelago Pack";
    mb::Browser browser;
    browser.cards =
        mb::merge_cards(std::move(game), std::move(mod), std::move(installed), std::move(online));
    mb::apply_filters(browser);
    return browser;
}

void merging() {
    const mb::Browser browser = sample();
    // The online copy of an installed map shows once, as installed.
    OA_CHECK(browser.cards.size() == 8);
    std::size_t tidewater = 0;
    for (const mb::MapCard& made : browser.cards)
        if (made.name == "tidewater@lowlands") {
            ++tidewater;
            OA_CHECK(made.origin == Origin::installed);
            OA_CHECK(made.title == "Tidewater");
        }
    OA_CHECK(tidewater == 1);
    // Also when the installed pack came from another registry, and a second
    // registry's online copy shows once, the first's.
    mb::MapCard from_one = card("isle_of_ashes@archipelago", "Isle of Ashes", 4, "16x16");
    from_one.registry_name = "Registry One";
    mb::MapCard from_two = from_one;
    from_two.registry_name = "Registry Two";
    mb::MapCard installed = from_one;
    installed.registry_name = "Registry Three";
    std::vector<mb::MapCard> merged = mb::merge_cards({}, {}, {installed}, {from_one, from_two});
    OA_CHECK(merged.size() == 1);
    OA_CHECK(merged[0].origin == Origin::installed && merged[0].registry_name == "Registry Three");
    merged = mb::merge_cards({}, {}, {}, {from_one, from_two});
    OA_CHECK(merged.size() == 1);
    OA_CHECK(merged[0].origin == Origin::online && merged[0].registry_name == "Registry One");
    // Each card's origin is its list's, whatever it said. A game map wins
    // over a mod map of the same name; the mod map is not dropped otherwise.
    mb::MapCard wrong = card("x", "X", 2, "8x8");
    wrong.origin = Origin::online;
    merged = mb::merge_cards({wrong}, {}, {}, {});
    OA_CHECK(merged.size() == 1 && merged[0].origin == Origin::game);
    mb::MapCard same_game = card("same", "Same", 2, "8x8");
    mb::MapCard same_mod = card("same", "Same Mod", 2, "8x8");
    mb::MapCard other_mod = card("other", "Other", 2, "8x8");
    merged = mb::merge_cards({same_game}, {same_mod, other_mod}, {}, {});
    OA_CHECK(merged.size() == 2);
    OA_CHECK(merged[0].origin == Origin::game && merged[0].title == "Same");
    OA_CHECK(merged[1].origin == Origin::mod && merged[1].name == "other");
}

void order() {
    const mb::Browser browser = sample();
    // One alphabetical list. An online map sits among the others, not after them.
    const std::vector<std::string> expected{
        "Big Wars",
        "black_shoals@archipelago",
        "Coast To Coast",
        "fen_crossing@lowlands",
        "isle_of_ashes@archipelago",
        "Painted Desert",
        "ridge_valley",
        "tidewater@lowlands",
    };
    OA_CHECK(shown_names(browser) == expected);
}

void source_filter() {
    mb::Browser browser = sample();
    browser.filters.source = mb::Filters::Source::game;
    mb::apply_filters(browser);
    OA_CHECK(
        shown_names(browser) ==
        (std::vector<std::string>{"Big Wars", "Coast To Coast", "Painted Desert"})
    );
    browser.filters.source = mb::Filters::Source::mod;
    mb::apply_filters(browser);
    OA_CHECK(shown_names(browser) == (std::vector<std::string>{"ridge_valley"}));
    // Installed and online packs together. The duplicate online map stays out.
    browser.filters.source = mb::Filters::Source::downloads;
    mb::apply_filters(browser);
    OA_CHECK(
        shown_names(browser) == (std::vector<std::string>{
                                    "black_shoals@archipelago",
                                    "fen_crossing@lowlands",
                                    "isle_of_ashes@archipelago",
                                    "tidewater@lowlands",
                                })
    );
    browser.filters.source = mb::Filters::Source::all;
    mb::apply_filters(browser);
    OA_CHECK(browser.shown.size() == 8);
}

void counts_and_overlays() {
    mb::Browser browser = sample();
    mb::SourceCounts counts = mb::source_counts(browser);
    OA_CHECK(counts.all == 8 && counts.game == 3 && counts.mod == 1 && counts.downloads == 4);
    // The source pill does not change the counts. Players 4 does.
    browser.filters.source = mb::Filters::Source::mod;
    browser.filters.players = 4;
    counts = mb::source_counts(browser);
    OA_CHECK(counts.all == 3 && counts.game == 1 && counts.mod == 0 && counts.downloads == 2);
    OA_CHECK(mb::overlay_name(browser.cards[0]).size() > 0);
    for (const mb::MapCard& made : browser.cards) {
        if (made.name == "Painted Desert")
            OA_CHECK(mb::overlay_name(made) == "TA 3.1c");
        if (made.name == "Coast To Coast")
            OA_CHECK(mb::overlay_name(made) == "Core Contingency");
        if (made.name == "Big Wars")
            OA_CHECK(mb::overlay_name(made) == "Battle Tactics");
        if (made.name == "ridge_valley")
            OA_CHECK(mb::overlay_name(made) == "Ridge");
        if (made.name == "fen_crossing@lowlands")
            OA_CHECK(mb::overlay_name(made) == "Lowlands");
        if (made.name == "isle_of_ashes@archipelago")
            OA_CHECK(mb::overlay_name(made) == "Archipelago Pack");
    }
    // A pack named as the map, or as the stem before @, shows Downloaded
    // only once it is installed. The same map from a catalogue says GET.
    mb::MapCard titled = card("tidewater@lowlands", "Tidewater", 4, "24x24");
    titled.origin = Origin::installed;
    titled.pack_name = "tidewater";
    OA_CHECK(mb::overlay_name(titled) == "Downloaded");
    titled.pack_name = "TIDEWATER";
    titled.title = "Tidewater";
    OA_CHECK(mb::overlay_name(titled) == "Downloaded");
    titled.pack_name = "Lowlands";
    OA_CHECK(mb::overlay_name(titled) == "Lowlands");
    titled.origin = Origin::online;
    titled.pack_name = "Tidewater";
    titled.title = "Tidewater";
    OA_CHECK(mb::overlay_name(titled) == "Available");
    OA_CHECK(mb::overlay_name(titled) != "Downloaded");
    titled.title = "The Tide";
    titled.pack_name = "tidewater";
    OA_CHECK(mb::overlay_name(titled) == "Available");
    titled.origin = Origin::installed;
    OA_CHECK(mb::overlay_name(titled) == "Downloaded");
    // The card whose pack is downloading says so, installed or not.
    OA_CHECK(mb::overlay_name(titled, true) == "Downloading…");
    titled.origin = Origin::online;
    OA_CHECK(mb::overlay_name(titled, true) == "Downloading…");
    titled.pack_name = "Lowlands";
    OA_CHECK(mb::overlay_name(titled, true) == "Downloading…");
    OA_CHECK(mb::overlay_name(titled, false) == "Lowlands");
    // A game map keeps its source name even when that name is the title.
    titled.origin = Origin::game;
    titled.pack_name = "Tidewater";
    titled.source_name = "TA 3.1c";
    OA_CHECK(mb::overlay_name(titled) == "TA 3.1c");
    OA_CHECK(mb::overlay_name(titled, true) == "TA 3.1c");
}

/// A card whose only count is `players`, as a pack map's manifest gives it.
///
/// @param players the count
/// @return the card
mb::MapCard one_count(int32_t players) {
    mb::MapCard made;
    made.players = players;
    made.player_counts = {players};
    return made;
}

void numplayers_list() {
    // A listed count is kept, and the start-position count is not used.
    const mb::BaseMapPlayers two_and_four = mb::base_map_players("2, 4", 10);
    OA_CHECK(two_and_four.from_numplayers);
    OA_CHECK(two_and_four.counts == (std::vector<int32_t>{2, 4}));
    OA_CHECK(two_and_four.players == 4);
    const mb::BaseMapPlayers four = mb::base_map_players("4", 10);
    OA_CHECK(four.counts == (std::vector<int32_t>{4}) && four.players == 4);
    const mb::BaseMapPlayers spaced = mb::base_map_players("2,3, 4", 10);
    OA_CHECK(spaced.counts == (std::vector<int32_t>{2, 3, 4}) && spaced.players == 4);
    // Out of order, repeated, and a number outside 2 to 10 dropped.
    const mb::BaseMapPlayers ordered = mb::base_map_players("4, 2, 4, 12", 10);
    OA_CHECK(ordered.counts == (std::vector<int32_t>{2, 4}) && ordered.players == 4);
    // No count in 2 to 10: the start-position count, alone.
    const mb::BaseMapPlayers empty = mb::base_map_players("", 7);
    OA_CHECK(!empty.from_numplayers);
    OA_CHECK(empty.counts == (std::vector<int32_t>{7}) && empty.players == 7);
    const mb::BaseMapPlayers twelve = mb::base_map_players("12", 7);
    OA_CHECK(!twelve.from_numplayers && twelve.counts == (std::vector<int32_t>{7}));
    const mb::BaseMapPlayers junk = mb::base_map_players("junk", 7);
    OA_CHECK(!junk.from_numplayers && junk.players == 7 && junk.counts.size() == 1);
}

void players_filter() {
    OA_CHECK(mb::players_match(2, one_count(2)) && !mb::players_match(2, one_count(3)));
    OA_CHECK(mb::players_match(4, one_count(4)) && !mb::players_match(4, one_count(5)));
    OA_CHECK(mb::players_match(mb::many_players_chip, one_count(5)));
    OA_CHECK(mb::players_match(mb::many_players_chip, one_count(10)));
    OA_CHECK(!mb::players_match(mb::many_players_chip, one_count(4)));
    OA_CHECK(!mb::players_match(mb::many_players_chip, one_count(11)));
    // A card that lists no counts is matched on players alone.
    mb::MapCard lone = card("lone", "Lone", 6, "8x8");
    OA_CHECK(mb::players_match(mb::many_players_chip, lone) && !mb::players_match(4, lone));

    // Counts 2 and 4: chips 2 and 4 find it, chip 3 and 5–10 do not.
    mb::MapCard foursome = card("Acid Foursome", "Acid Foursome", 10, "13 x 13");
    foursome.player_counts = {2, 4};
    OA_CHECK(mb::shown_players(foursome) == 4);
    OA_CHECK(mb::players_match(2, foursome) && mb::players_match(4, foursome));
    OA_CHECK(!mb::players_match(3, foursome));
    OA_CHECK(!mb::players_match(mb::many_players_chip, foursome));
    mb::Browser several;
    several.cards = {foursome};
    several.filters.players = 2;
    mb::apply_filters(several);
    OA_CHECK(shown_names(several) == (std::vector<std::string>{"Acid Foursome"}));
    several.filters.players = 4;
    mb::apply_filters(several);
    OA_CHECK(shown_names(several) == (std::vector<std::string>{"Acid Foursome"}));
    several.filters.players = 3;
    mb::apply_filters(several);
    OA_CHECK(several.shown.empty());
    several.filters.players = mb::many_players_chip;
    mb::apply_filters(several);
    OA_CHECK(several.shown.empty());
    // Any shows it again. Choosing the same count leaves it shown.
    several.filters.players.reset();
    mb::apply_filters(several);
    OA_CHECK(several.shown.size() == 1);
    // 5–10 matches a map that is also for 2, when one count is from 5 to 10.
    several.cards[0].player_counts = {2, 6};
    several.cards[0].players = 6;
    several.filters.players = 2;
    mb::apply_filters(several);
    OA_CHECK(several.shown.size() == 1);
    several.filters.players = mb::many_players_chip;
    mb::apply_filters(several);
    OA_CHECK(several.shown.size() == 1);

    mb::Browser browser = sample();
    browser.filters.players = 4;
    mb::apply_filters(browser);
    OA_CHECK(
        shown_names(browser) ==
        (std::vector<std::string>{
            "isle_of_ashes@archipelago", "Painted Desert", "tidewater@lowlands"
        })
    );
    browser.filters.players = 2;
    mb::apply_filters(browser);
    OA_CHECK(shown_names(browser) == (std::vector<std::string>{"Coast To Coast"}));
    browser.filters.players = mb::many_players_chip;
    mb::apply_filters(browser);
    OA_CHECK(
        shown_names(browser) == (std::vector<std::string>{"Big Wars", "black_shoals@archipelago"})
    );
    browser.filters.players = 3;
    mb::apply_filters(browser);
    OA_CHECK(
        shown_names(browser) == (std::vector<std::string>{"fen_crossing@lowlands", "ridge_valley"})
    );
}

void size_filter() {
    OA_CHECK(mb::map_sides("16 x 16") == (std::pair<int32_t, int32_t>{16, 16}));
    OA_CHECK(mb::map_sides("24x12") == (std::pair<int32_t, int32_t>{24, 12}));
    OA_CHECK(!mb::map_sides("huge"));
    OA_CHECK(!mb::map_sides("16"));
    OA_CHECK(mb::size_letter("10x2") == 'S');
    OA_CHECK(mb::size_letter("11 x 4") == 'M');
    OA_CHECK(mb::size_letter("8x20") == 'M');
    OA_CHECK(mb::size_letter("21x21") == 'L');
    OA_CHECK(!mb::size_letter(""));

    mb::Browser browser = sample();
    browser.filters.size = 'S';
    mb::apply_filters(browser);
    OA_CHECK(shown_names(browser).empty());
    browser.filters.size = 'M';
    mb::apply_filters(browser);
    OA_CHECK(
        shown_names(browser) == (std::vector<std::string>{
                                    "black_shoals@archipelago",
                                    "Coast To Coast",
                                    "fen_crossing@lowlands",
                                    "isle_of_ashes@archipelago",
                                    "Painted Desert",
                                    "ridge_valley",
                                })
    );
    browser.filters.size = 'L';
    mb::apply_filters(browser);
    OA_CHECK(shown_names(browser) == (std::vector<std::string>{"Big Wars", "tidewater@lowlands"}));
    browser.filters.size.reset();
    mb::apply_filters(browser);
    OA_CHECK(browser.shown.size() == 8);
}

void search() {
    mb::Browser browser = sample();
    // Title, ignoring case.
    browser.filters.query = "ISLE";
    mb::apply_filters(browser);
    OA_CHECK(shown_names(browser) == (std::vector<std::string>{"isle_of_ashes@archipelago"}));
    // The name.
    browser.filters.query = "fen_cross";
    mb::apply_filters(browser);
    OA_CHECK(shown_names(browser) == (std::vector<std::string>{"fen_crossing@lowlands"}));
    // The pack's name.
    browser.filters.query = "lowlands";
    mb::apply_filters(browser);
    OA_CHECK(shown_names(browser).size() == 2);
    browser.filters.query = "nothing like it";
    mb::apply_filters(browser);
    OA_CHECK(browser.shown.empty());
}

void combined_filters() {
    mb::Browser browser = sample();
    browser.filters.source = mb::Filters::Source::downloads;
    browser.filters.players = 4;
    browser.filters.size = 'M';
    browser.filters.query = "ashes";
    mb::apply_filters(browser);
    OA_CHECK(shown_names(browser) == (std::vector<std::string>{"isle_of_ashes@archipelago"}));
    browser.filters.source = mb::Filters::Source::game;
    mb::apply_filters(browser);
    OA_CHECK(browser.shown.empty());
    // A selection the filters hide is cleared. The map stays in the list.
    browser = sample();
    const std::size_t cards = browser.cards.size();
    browser.selected = browser.shown.front();
    browser.filters.source = mb::Filters::Source::downloads;
    mb::apply_filters(browser);
    OA_CHECK(!browser.selected);
    OA_CHECK(browser.cards.size() == cards);
}

void primary() {
    mb::Browser browser = sample();
    // No selection: Select, disabled.
    mb::PrimaryAction action = mb::primary_action(browser);
    OA_CHECK(action.kind == mb::Primary::select && !action.enabled);
    const auto select_named = [&](std::string_view name) {
        for (std::size_t index = 0; index < browser.cards.size(); ++index)
            if (browser.cards[index].name == name)
                browser.selected = index;
    };
    // A game map, and an installed one that fits: Select.
    select_named("Painted Desert");
    action = mb::primary_action(browser);
    OA_CHECK(action.kind == mb::Primary::select && action.enabled && action.reason.empty());
    select_named("tidewater@lowlands");
    OA_CHECK(mb::primary_action(browser).enabled);
    // An installed map that does not fit: disabled, with its reason.
    browser.cards[*browser.selected].mod_fit = Fit::does_not_fit;
    browser.cards[*browser.selected].fit_reason = "New names: the feature 'x' is defined already";
    action = mb::primary_action(browser);
    OA_CHECK(action.kind == mb::Primary::select && !action.enabled);
    OA_CHECK(action.reason.find("New names") == 0);
    // An online map that fits, or whose fit is unknown: Download and select.
    select_named("isle_of_ashes@archipelago");
    action = mb::primary_action(browser);
    OA_CHECK(action.kind == mb::Primary::download_and_select && action.enabled);
    browser.cards[*browser.selected].mod_fit = Fit::unknown;
    OA_CHECK(mb::primary_action(browser).enabled);
    // One that does not: disabled.
    browser.cards[*browser.selected].base_fit = Fit::does_not_fit;
    action = mb::primary_action(browser);
    OA_CHECK(action.kind == mb::Primary::download_and_select && !action.enabled);
    // While a download runs: Downloading…, disabled.
    browser.cards[*browser.selected].base_fit = Fit::fits;
    browser.downloading = "isle_of_ashes@archipelago";
    action = mb::primary_action(browser);
    OA_CHECK(action.kind == mb::Primary::downloading && !action.enabled);
}

void words() {
    OA_CHECK(mb::card_word("isle_of_ashes@archipelago") == "isle-of-ashes-archipelago");
    OA_CHECK(mb::card_word("Painted Desert") == "painted-desert");
    OA_CHECK(mb::card_word("a-b") == "a-b");
    OA_CHECK(mb::card_word("A_B") == "a-b");
    // A character of several bytes is one hyphen.
    OA_CHECK(mb::card_word("caf\xc3\xa9") == "caf-");
    OA_CHECK(mb::card_word("") == "map");
    OA_CHECK(mb::card_word(std::string(200, 'x')).size() == mb::most_word_bytes);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        const std::string_view argument = argv[1];
        if (argument == "--frames" && argc > 2)
            return write_map_browser_frames(argv[2]);
        std::fprintf(stderr, "map browser: unexpected argument %s\n", argv[1]);
        return 2;
    }
    merging();
    order();
    source_filter();
    counts_and_overlays();
    numplayers_list();
    players_filter();
    size_filter();
    search();
    combined_filters();
    primary();
    words();
    layout_tests();
    return oa::test::check_exit_status();
}
