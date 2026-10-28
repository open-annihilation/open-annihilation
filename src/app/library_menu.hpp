// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library screen on the OA layer, drawn with the game's own art over
// the game's own main menu (runtime_library_menu.cpp opens it
// from the main menu). It covers the whole window: under a LIBRARY title
// with the refresh button at its right, a LIBRARY panel of tabs, the chosen
// tab's panel and a DETAILS panel, then the tab's buttons on the left and
// CLOSE on the right.
//
// The tabs are Active, then Mods, Maps and Updates, then Registries. Active
// shows the player's setup in five tiles: BASE GAME, LANGUAGE, EXPANSIONS,
// MOD and MAPS. Mods, Maps and Updates show a search field, the filters and
// the packages, one row each with its badge, name, maker and summary, its
// version and size and its state's mark; DETAILS tells of the row chosen,
// with its buttons. Registries shows a card for each registry with its
// switch and REFRESH, ADD REGISTRY… and the note on review; DETAILS tells
// of the card chosen.
//
// The screen stands on the bare picture the game's menus stand on, the
// LIBRARY title in the game's font, unsmoothed, where the Options screen has
// its OPTIONS, and the refresh button, the game's plain button, right of
// it. The three panels stand in the frame the game draws round its score
// board, labelled in the game's font, and the tube of its frame parts the
// tabs. The tabs, the filters, the
// switches and the small buttons are the game's plain buttons, held while
// chosen; the rows, the cards, the tiles, the badges and the boxes are its
// list box; the strips, the foot and the pills its sunk bar; the search its
// text field; the expansions' and base games' marks its check box; and the
// scroll bar its own. The big buttons stand on the Options screen's plates.
// Every text is in the game's font, amber for what lies under the pointer
// and what to heed.
//
// The screen is modal and draws nothing that moves on its own. What the
// presses do is its host's: the screen asks for a press (LibraryMenuPress)
// and reads what the Library shows again after it.
#pragma once

#include "game_menu_art.hpp"
#include "oa_layer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

/// A picture the Library shows: a badge, a registry's icon or a minimap,
/// 8-bit RGBA, rows top to bottom. Its pixels are the host's and stay until
/// the host's next facts.
struct LibraryMenuPicture {
    const uint8_t* pixels{}; ///< width * height * 4 bytes; null for none
    uint32_t width{};        ///< in pixels
    uint32_t height{};       ///< in pixels

    friend bool operator==(const LibraryMenuPicture&, const LibraryMenuPicture&) = default;
};

/// The picture beside a tab's name.
enum class LibraryMenuIcon : uint8_t {
    active,     ///< a screen: what is played now
    mods,       ///< a puzzle piece
    maps,       ///< a folded map
    updates,    ///< a download arrow
    registries, ///< a stack of discs
};

/// One tab of the LIBRARY panel.
struct LibraryMenuTab {
    std::string id{};        ///< its word, for automation
    std::string name{};      ///< its name and count, as shown
    LibraryMenuIcon icon{};  ///< its picture
    bool after_divider{};    ///< a divider stands above it
    bool attention{};        ///< an amber dot: something waits there, such as an update

    friend bool operator==(const LibraryMenuTab&, const LibraryMenuTab&) = default;
};

/// The mark at a package row's right.
enum class LibraryMenuMark : uint8_t {
    none,      ///< nothing
    get,       ///< a download arrow: not installed
    update,    ///< an amber up-arrow in a ring: an update waits
    installed, ///< a check in a ring: installed, or played
};

/// One package of the list.
struct LibraryMenuRow {
    std::string id{};               ///< its word, for automation
    std::string letters{};          ///< the badge's letters while it has no picture
    LibraryMenuPicture picture{};   ///< its badge; none draws the letters
    std::string name{};             ///< its name
    std::string line{};             ///< its maker and summary, or its download's state
    bool line_warns{};              ///< the line is amber: blocked, failed or downloading
    std::string version{};          ///< its version, as shown
    std::string size{};             ///< its size, as shown
    LibraryMenuMark mark{};         ///< its mark
    bool mark_pressable{};          ///< the mark asks for GET or UPDATE when pressed
    bool dimmed{};                  ///< it cannot be got or updated now
    /// The share downloaded, 0 to 1, drawn as a bar under its line; none
    /// while it is not downloading.
    std::optional<double> progress{};

    friend bool operator==(const LibraryMenuRow&, const LibraryMenuRow&) = default;
};

/// One fact of the details: a label and its value.
struct LibraryMenuFact {
    std::string label{}; ///< what it is
    std::string value{}; ///< what it says
    bool warns{};        ///< the value is amber

    friend bool operator==(const LibraryMenuFact&, const LibraryMenuFact&) = default;
};

/// One button of the details.
struct LibraryMenuButton {
    std::string id{};      ///< its word, for automation
    std::string caption{}; ///< its words
    bool enabled{true};    ///< it takes a press

    friend bool operator==(const LibraryMenuButton&, const LibraryMenuButton&) = default;
};

/// What the DETAILS panel tells.
struct LibraryMenuDetails {
    std::string letters{};                   ///< the badge's letters while it has no picture
    LibraryMenuPicture picture{};            ///< its badge; none draws the letters
    bool badge{true};                        ///< a badge shows beside the title
    std::string title{};                     ///< its name
    std::string status{};                    ///< the line under the name
    bool status_warns{};                     ///< the status is amber
    std::string summary{};                   ///< what it is, wrapped
    std::vector<std::string> tags{};         ///< its tags, as small plates that choose the tag
    std::vector<LibraryMenuFact> facts{};    ///< its facts, one to a line
    std::string section{};                   ///< a strip's title over the section's lines; empty for none
    std::vector<std::string> section_lines{}; ///< the lines under the strip, such as fingerprints
    std::string section_note{};              ///< a faded line under them
    std::string note{};                      ///< an amber note over the buttons
    std::vector<LibraryMenuButton> buttons{}; ///< its buttons, two to a line

    friend bool operator==(const LibraryMenuDetails&, const LibraryMenuDetails&) = default;
};

/// One plate of the filters' row.
struct LibraryMenuChip {
    std::string id{};      ///< its word, for automation
    std::string caption{}; ///< its words and count
    bool chosen{};         ///< it is the filter shown

    friend bool operator==(const LibraryMenuChip&, const LibraryMenuChip&) = default;
};

/// What Mods, Maps and Updates show.
struct LibraryMenuList {
    std::string search{};                  ///< the search's words
    std::string search_hint{};             ///< what the empty search says
    std::vector<LibraryMenuChip> filters{}; ///< the filters, then TAGS
    std::vector<LibraryMenuChip> tags{};    ///< the tags TAGS lists, with their counts
    bool tag_chosen{};                     ///< a tag hides the rows without it
    std::vector<LibraryMenuRow> rows{};    ///< the packages, in the order shown
    std::optional<std::size_t> selected{}; ///< the row chosen
    std::string empty{};                   ///< what an empty list says

    friend bool operator==(const LibraryMenuList&, const LibraryMenuList&) = default;
};

/// How an expansion's plate is drawn.
enum class LibraryMenuExpansionArrow : uint8_t {
    none,   ///< no arrow
    get,    ///< a download arrow
    cancel, ///< the download runs: pressing cancels it
    retry,  ///< the download failed: pressing tries again
};

/// One expansion of the base game, on the Active tab.
struct LibraryMenuExpansion {
    std::string id{};                      ///< its bundle id
    std::string name{};                    ///< its words
    bool on{};                             ///< on in the choice shown
    bool enabled{};                        ///< a press does something
    bool locked{};                         ///< it stays on: the command line's mod needs it
    LibraryMenuExpansionArrow arrow{};     ///< the arrow at its right

    friend bool operator==(const LibraryMenuExpansion&, const LibraryMenuExpansion&) = default;
};

/// One installed map, on the Active tab.
struct LibraryMenuMap {
    std::string name{};           ///< its title
    LibraryMenuPicture picture{}; ///< its minimap; none while it is not decoded

    friend bool operator==(const LibraryMenuMap&, const LibraryMenuMap&) = default;
};

/// One base game of the chooser.
struct LibraryMenuBaseGame {
    std::string id{};             ///< its id
    std::string letters{};        ///< the badge's letters while it has no picture
    LibraryMenuPicture picture{}; ///< its icon
    std::string name{};           ///< its name and version
    std::string state{};          ///< its state's words: in use, copied, not added
    std::string detail{};         ///< the line under its name
    bool marked{};                ///< the radio marks it

    friend bool operator==(const LibraryMenuBaseGame&, const LibraryMenuBaseGame&) = default;
};

/// The tiles of the Active tab.
enum class LibraryMenuTile : uint8_t { base, language, expansions, mod, maps };

/// How many tiles the Active tab has.
inline constexpr std::size_t library_menu_tile_count = 5;

/// What the Active tab shows.
struct LibraryMenuActive {
    std::string base_letters{};                    ///< the base game's letters
    LibraryMenuPicture base_picture{};             ///< its icon
    std::string base_name{};                       ///< "Total Annihilation"
    std::string base_version{};                    ///< "3.1c · from GOG"
    std::string base_check{};                      ///< "All files match"; empty for none
    bool base_check_good{true};                    ///< the check is not amber
    bool change_enabled{};                         ///< CHANGE… takes a press
    std::string language_letters{};                ///< the pack's letters; empty draws a speech mark
    LibraryMenuPicture language_picture{};         ///< the pack's badge
    std::string language_name{};                   ///< the language's name in itself
    std::string language_line{};                   ///< "Built in", or the pack's version
    std::string expansions_heading{};              ///< "EXPANSIONS · 2 OF 3 ON"
    std::vector<LibraryMenuExpansion> expansions{}; ///< the pills
    std::string expansions_hint{};                 ///< under the pills
    std::string expansions_lock{};                 ///< why locked pills stay on, amber
    std::string mod_letters{};                     ///< the mod's letters
    LibraryMenuPicture mod_picture{};              ///< its badge
    std::string mod_name{};                        ///< its name, or No Mod
    std::string mod_line{};                        ///< its release and maker, or "as it was made"
    std::string mod_state{};                       ///< "Playing"; empty for none
    bool no_mod_enabled{};                         ///< NO MOD takes a press
    std::string maps_heading{};                    ///< "MAPS · 214"
    std::vector<LibraryMenuMap> maps{};            ///< the maps, in the order shown
    std::string maps_empty{};                      ///< what the tile says with no map
    /// What DETAILS tells of each tile, in LibraryMenuTile's order.
    std::vector<LibraryMenuDetails> details{};
    std::string note{}; ///< the line at DETAILS' foot: what happens as the Library closes
    bool chooser_open{};                            ///< the base game chooser shows
    std::string chooser_lead{};                     ///< the chooser's line over the games
    std::vector<LibraryMenuBaseGame> chooser_rows{}; ///< its games
    std::string chooser_note{};                     ///< its note under them
    std::string chooser_problem{};                  ///< a copy that failed, amber

    friend bool operator==(const LibraryMenuActive&, const LibraryMenuActive&) = default;
};

/// One registry's card.
struct LibraryMenuRegistry {
    std::string id{};             ///< its id
    std::string letters{};        ///< its initials while it has no picture
    LibraryMenuPicture picture{}; ///< its icon
    std::string title{};          ///< its heading
    std::string pill{};           ///< "Built in" or "not reviewed"; empty for none
    bool pill_warns{};            ///< the pill is amber
    std::string address{};        ///< where it is read from
    std::string packages{};       ///< "214 packages"
    std::string refreshed{};      ///< "refreshed 5 min ago"
    bool refresh_warns{};         ///< the refresh line is amber: it could not be reached
    bool on{};                    ///< its switch is on
    bool refresh_enabled{};       ///< REFRESH takes a press
    bool refreshing{};            ///< it is being asked now
    LibraryMenuDetails details{}; ///< what DETAILS tells of it

    friend bool operator==(const LibraryMenuRegistry&, const LibraryMenuRegistry&) = default;
};

/// What the Registries tab shows.
struct LibraryMenuRegistries {
    std::vector<LibraryMenuRegistry> cards{}; ///< the registries, in the order shown
    std::optional<std::size_t> selected{};    ///< the card chosen
    std::string add{};                        ///< ADD REGISTRY…'s words
    std::string note{};                       ///< the note on review under the cards
    std::string notice{};                     ///< why the last change was refused, amber

    friend bool operator==(const LibraryMenuRegistries&, const LibraryMenuRegistries&) = default;
};

/// A question the Library asks over itself, and a notice of one button: in a
/// panel's frame over the dimmed screen, its title over a divider, its
/// paragraphs and its buttons in a row at its foot, right-aligned. It takes
/// every press and key while it shows.
struct LibraryMenuQuestion {
    std::string title{};                      ///< its title
    std::vector<std::string> paragraphs{};    ///< its text, top to bottom
    std::vector<LibraryMenuButton> buttons{}; ///< its buttons, left to right
    std::size_t cancel{};                     ///< the button Escape and N answer
    std::size_t primary{};                    ///< the button Y answers
    std::size_t marked{};                     ///< the button Enter answers at first

    friend bool operator==(const LibraryMenuQuestion&, const LibraryMenuQuestion&) = default;
};

/// Which kind of tab is shown.
enum class LibraryMenuPage : uint8_t { active, list, registries };

/// What the Library screen shows now.
struct LibraryMenuFacts {
    std::vector<LibraryMenuTab> tabs{};   ///< the tabs, top to bottom
    std::size_t chosen{};                 ///< the tab shown
    LibraryMenuPage page{};               ///< what it is
    std::string heading{};                ///< the tab's panel's label
    std::string installed{};              ///< "Installed: 7 packages", at the tabs' foot
    bool refresh_enabled{};               ///< the header's refresh takes a press
    bool refreshing{};                    ///< the registries are being asked now
    LibraryMenuActive active{};           ///< the Active tab, while it shows
    LibraryMenuList list{};               ///< Mods, Maps or Updates, while one shows
    LibraryMenuRegistries registries{};   ///< Registries, while it shows
    /// The details of the row chosen on a list; empty title for none.
    LibraryMenuDetails details{};
    std::vector<LibraryMenuButton> left_buttons{}; ///< the buttons left of CLOSE
    std::string queue{};   ///< the download queue's line, over the buttons; empty for none
    std::string notice{};  ///< an amber line in DETAILS, such as a folder that did not open
    /// The line a switch of the mod made here shows while the game starts
    /// again, in a frame over the dimmed screen; empty for none.
    std::string switching{};
    /// The question asked over the screen; none for none.
    std::optional<LibraryMenuQuestion> question{};

    friend bool operator==(const LibraryMenuFacts&, const LibraryMenuFacts&) = default;
};

/// What a press asks the host for.
enum class LibraryMenuCommand : uint8_t {
    tab,              ///< show a tab, by its place
    filter,           ///< show a filter, by its chip's place
    tag,              ///< show a tag, by its place among the tags; past them, every tag
    search,           ///< search for the words given
    select,           ///< choose a row, by its place
    row_mark,         ///< a row's mark: its GET or UPDATE, by its place
    details_tag,      ///< a tag of the details, by its place
    details_button,   ///< a button of the details, by its place
    left_button,      ///< a button left of CLOSE, by its place
    refresh,          ///< the header's refresh
    close,            ///< CLOSE
    change_base,      ///< CHANGE… on the base game tile
    language,         ///< SETTINGS on the language tile
    expansion,        ///< an expansion's plate, by its place
    choose_mod,       ///< CHOOSE MOD on the mod tile
    no_mod,           ///< NO MOD on the mod tile
    browse_maps,      ///< BROWSE MAPS on the maps tile
    chooser_mark,     ///< a base game of the chooser, by its place
    chooser_use,      ///< USE THIS GAME
    chooser_cancel,   ///< CANCEL on the chooser
    registry_select,  ///< choose a registry's card, by its place
    registry_switch,  ///< a registry's switch, by its place; on says which half
    registry_refresh, ///< a registry's REFRESH, by its place
    add_registry,     ///< ADD REGISTRY…
    question_button,  ///< a button of the question, by its place
};

/// One press of the Library screen.
struct LibraryMenuPress {
    LibraryMenuCommand command{}; ///< what it asks for
    std::size_t index{};          ///< the place it names
    std::string text{};           ///< the search's words
    bool on{};                    ///< the switch's half
};

/// The rows and maps the screen shows now, whose pictures the host fetches.
struct LibraryMenuView {
    std::size_t first_row{}; ///< the first list row in view
    std::size_t end_row{};   ///< after the last list row in view
    std::size_t maps{};      ///< how many of the Active tab's maps show

    friend bool operator==(const LibraryMenuView&, const LibraryMenuView&) = default;
};

/// What the Library screen's presses do. The screen reads the facts again
/// after each.
struct LibraryMenuActions {
    /// Tells whether the screen shows; it closes when it does not.
    std::function<bool()> shows{};
    /// Carries out a press: true when the screen closes.
    std::function<bool(const LibraryMenuPress&)> press{};
    /// Tells the host the rows and maps in view.
    std::function<void(const LibraryMenuView&)> viewed{};
    /// The screen leaves: what the Active tab holds is stored.
    std::function<void()> closed{};
};

/// Makes the Library screen.
///
/// @param name the screen's name on the OA layer
/// @param facts reads what the Library shows now
/// @param actions what its presses do
/// @param game_art the game's art it is drawn with
/// @return the screen
[[nodiscard]] std::unique_ptr<LayerScreen> make_library_menu(
    std::string_view name,
    std::function<LibraryMenuFacts()> facts,
    LibraryMenuActions actions,
    GameMenuArt game_art
);

} // namespace oa::app
