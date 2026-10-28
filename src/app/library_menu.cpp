// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Open Annihilation's Library screen on the OA layer (library_menu.hpp).

#include "library_menu.hpp"
#include "game_menu_art.hpp"
#include "menu_paint.hpp"

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace kit = oa::ui::kit;
using namespace menu_paint;

// The screen's measures, in rows of a window 1080 rows high, as the other
// menus' are.

/// The narrowest width, in the same rows, the measures keep their size in;
/// a narrower window shrinks them to fit its width.
constexpr double design_least_columns = 1480.0;
constexpr double content_columns = 1420.0;    ///< the panels' width together
constexpr double panels_top = 180.75;         ///< the panels' top
constexpr double panels_rows = 690.0;         ///< the panels' height
constexpr double tabs_columns = 300.0;        ///< the LIBRARY panel's width
constexpr double details_columns = 350.0;     ///< the DETAILS panel's width
constexpr double panel_gap_columns = 14.0;    ///< between two panels
constexpr double band_rows = 46.0;            ///< a panel's label band
constexpr double rim_columns = 11.0;          ///< from a panel's edge to its label's room
constexpr double label_left_columns = 10.0;   ///< from there to a panel's label
constexpr double label_top_rows = 5.0;        ///< from a panel's top to its label
constexpr double label_rows = 34.0;           ///< a label's height
constexpr double inset_columns = 18.0;        ///< inside a panel, round its content
constexpr double tab_rows = 50.0;             ///< a tab's plate's height
constexpr double tab_gap_rows = 8.0;          ///< between two tabs' plates
constexpr double divider_rows = 26.0;         ///< the room a divider takes
constexpr double icon_columns = 30.0;         ///< a tab's picture's side
constexpr double icon_gap_columns = 14.0;     ///< from a plate's left to its picture, and on to its name
constexpr double foot_rows = 40.0;            ///< the box at the tabs' foot
constexpr double search_rows = 44.0;          ///< the search field's height
constexpr double chip_rows = 38.0;            ///< a filter's plate's height
constexpr double tags_columns = 180.0;        ///< TAGS' width, beside the search
constexpr double gap_rows = 10.0;             ///< between the search, the filters and the rows
constexpr double row_rows = 74.0;             ///< a package's row's height
constexpr double row_gap_rows = 6.0;          ///< between two rows
constexpr double pad_columns = 12.0;          ///< inside a row or a tile, round its parts
constexpr double mark_columns = 40.0;         ///< a row's mark's side
constexpr double version_columns = 160.0;     ///< a row's version and size column
constexpr double bar_rows = 6.0;              ///< a download's bar's height
constexpr double scroll_columns = 20.0;       ///< the scroll bar's width
constexpr double scroll_gap_columns = 8.0;    ///< between the rows and the scroll bar
constexpr double least_thumb_rows = 40.0;     ///< the scroll bar's thumb's least height
constexpr double details_badge_columns = 76.0; ///< the details' badge's side
constexpr double small_button_rows = 44.0;    ///< a small button's height
constexpr double button_gap_columns = 8.0;    ///< between two small buttons
constexpr double tag_rows = 28.0;             ///< a tag's plate's height
constexpr double strip_rows = 30.0;           ///< a tile's or a section's strip's height
constexpr double tile_gap_columns = 12.0;     ///< between two tiles
constexpr double pill_rows = 36.0;            ///< an expansion's row's height
constexpr double pill_gap_rows = 6.0;         ///< between two expansions' rows
constexpr double tile_button_columns = 150.0; ///< a tile's button's width
constexpr double tile_button_rows = 40.0;     ///< a tile's button's height
constexpr double browse_columns = 170.0;      ///< BROWSE MAPS' width
constexpr double card_rows = 124.0;           ///< a registry's card's height
constexpr double card_gap_rows = 10.0;        ///< between two cards
constexpr double add_rows = 96.0;             ///< ADD REGISTRY…'s box's height
constexpr double switch_columns = 132.0;      ///< a registry's switch's and REFRESH's width
constexpr double switch_rows = 38.0;          ///< their height
constexpr double note_rows = 64.0;            ///< the note on review's box
constexpr double chooser_row_rows = 76.0;     ///< a base game's row in the chooser
constexpr double menu_item_rows = 34.0;       ///< an item of TAGS' list
constexpr double menu_columns = 300.0;        ///< TAGS' list's width
constexpr double buttons_gap_rows = 18.0;     ///< between the panels and the buttons
constexpr double button_rows = 58.0;          ///< the buttons' height
constexpr double left_button_columns = 300.0; ///< a button's width on the left
constexpr double close_columns = 240.0;       ///< CLOSE's width
constexpr double footer_gap_columns = 16.0;   ///< between two buttons of a side
/// The points a line of text needs over its font's size.
constexpr int32_t text_room = 8;
/// The most lines the details' summary takes.
constexpr std::size_t most_summary_lines = 6;
/// The most lines a fact's value takes.
constexpr std::size_t most_fact_lines = 3;
/// The most items TAGS' list shows at once.
constexpr std::size_t most_menu_items = 10;
/// A turn of the wheel scrolls the rows by this share of a row's pitch.
constexpr double wheel_rows_per_notch = 1.0;
/// The map tiles' names take this many points of a tile's height.
constexpr int32_t map_name_room = 22;

/// The bars across the registries' tab picture: the plain buttons' steel.
constexpr kit::Colour entry_steel_colour{64, 70, 72};
constexpr kit::Colour chosen_colour{232, 164, 52};       ///< amber: the chosen and the pointed at
constexpr kit::Colour chosen_text_colour{255, 206, 120}; ///< amber words: a status, a chosen state
constexpr kit::Colour well_colour{12, 15, 18};           ///< a bar's ground, and the shade over the screen
constexpr kit::Colour note_colour{150, 152, 144};        ///< faded words
constexpr kit::Colour warn_colour{232, 164, 52};         ///< words to heed, amber
constexpr kit::Colour badge_ink_colour{226, 228, 214};   ///< the speech mark on the language's badge
/// How strongly the row under the pointer is lit amber, in 256ths.
constexpr uint32_t row_light_opacity = 26;
/// How strongly a dimmed row is shaded, in 256ths.
constexpr uint32_t dim_opacity = 110;
/// How strongly the screen is shaded under a switch's line, in 256ths.
constexpr uint32_t switching_dim_opacity = 128;
/// How strongly the screen is shaded under a question, in 256ths.
constexpr uint32_t question_dim_opacity = 128;
/// The most lines a question's paragraph takes.
constexpr std::size_t most_question_lines = 6;
/// A tab's picture on the game's buttons, and the magnifier on its text
/// field: dark, as their bevel's shade.
constexpr kit::Colour game_icon_colour{40, 44, 40};
/// The gap between the game's title and the refresh button right of it, in
/// the picture's pixels.
constexpr int32_t game_refresh_gap = 10;
/// How many times its height the refresh button is wide over the game's
/// art, as the game's narrowest plain button.
constexpr int32_t game_refresh_shape = 4;
/// The window pixels a point of the screen takes, in percent.
constexpr int32_t window_percent = 100;

/// What a press can reach.
enum class Part : uint8_t {
    none,           ///< nothing
    tab,            ///< a tab, by its place
    search,         ///< the search field
    filter,         ///< a filter's plate, by its place
    tags,           ///< TAGS, which opens the list of tags
    tag_item,       ///< an item of that list, by its place; past the tags, every tag
    row,            ///< a package's row, by its place
    row_mark,       ///< a row's mark, by its place
    scroll_up,      ///< the scroll bar's upper arrow
    scroll_down,    ///< its lower arrow
    well,           ///< its well, round its thumb
    thumb,          ///< its thumb
    details_tag,    ///< a tag of the details, by its place
    details_button, ///< a button of the details, by its place
    left_button,    ///< a button left of CLOSE, by its place
    refresh,        ///< the header's refresh
    close,          ///< CLOSE
    tile,           ///< an Active tile, where it has no button, by LibraryMenuTile
    change_base,    ///< CHANGE…
    language,       ///< the language tile's SETTINGS
    expansion,      ///< an expansion's plate, by its place
    choose_mod,     ///< CHOOSE MOD
    no_mod,         ///< NO MOD
    browse_maps,    ///< BROWSE MAPS
    chooser_row,    ///< a base game of the chooser, by its place
    chooser_use,    ///< USE THIS GAME
    chooser_cancel, ///< the chooser's CANCEL
    card,           ///< a registry's card, by its place
    card_off,       ///< its switch's OFF half
    card_on,        ///< its switch's ON half
    card_refresh,   ///< its REFRESH
    add_registry,   ///< the ADD REGISTRY… box
    question_button, ///< a button of the question, by its place
};

/// A part, and which of its kind.
struct Hit {
    Part part{Part::none}; ///< what it is
    std::size_t index{};   ///< which of its kind

    friend bool operator==(const Hit&, const Hit&) = default;
};

/// Where the screen's fixed parts lie, in its points.
struct LibraryLayout {
    double unit{};             ///< the points a row of the 1080-row window takes
    kit::Rect refresh{};       ///< the refresh button, right of the game's title
    kit::Rect tabs{};          ///< the LIBRARY panel
    kit::Rect tab_list{};      ///< inside it, where the plates lie
    kit::Rect foot{};          ///< the box at its foot
    kit::Rect center{};        ///< the chosen tab's panel
    kit::Rect body{};          ///< inside it
    kit::Rect details{};       ///< the DETAILS panel
    kit::Rect about{};         ///< inside it, where its text lies
    kit::Rect close{};         ///< CLOSE
    int32_t left{};            ///< the panels' left edge, where the left buttons start
    int32_t buttons_y{};       ///< the buttons' top
    int32_t button_height{};   ///< the buttons' height
    int32_t left_width{};      ///< a left button's width
    int32_t footer_gap{};      ///< between two buttons of a side
    int32_t tab_height{};      ///< a tab's plate's height
    int32_t tab_gap{};         ///< between two plates
    int32_t divider{};         ///< the room a divider takes
    int32_t icon{};            ///< a tab's picture's side
    int32_t icon_gap{};        ///< round a tab's picture
    int32_t foot_height{};     ///< the box at the tabs' foot
    int32_t search_height{};   ///< the search field's height
    int32_t chip_height{};     ///< a filter's plate's height
    int32_t tags_width{};      ///< TAGS' width, beside the search
    int32_t gap{};             ///< between the search, the filters and the rows
    int32_t row_height{};      ///< a row's height
    int32_t row_gap{};         ///< between two rows
    int32_t pad{};             ///< inside a row or a tile
    int32_t mark{};            ///< a row's mark's side
    int32_t version{};         ///< a row's version column's width
    int32_t bar{};             ///< a download's bar's height
    int32_t scroll{};          ///< the scroll bar's width
    int32_t scroll_gap{};      ///< between the rows and the scroll bar
    int32_t least_thumb{};     ///< the thumb's least height
    int32_t details_badge{};   ///< the details' badge's side
    int32_t small_button{};    ///< a small button's height
    int32_t button_gap{};      ///< between two small buttons
    int32_t tag_height{};      ///< a tag's plate's height
    int32_t strip{};           ///< a tile's strip's height
    int32_t tile_gap{};        ///< between two tiles
    int32_t pill{};            ///< an expansion's row's height
    int32_t pill_gap{};        ///< between two plates
    int32_t tile_button{};     ///< a tile's button's width
    int32_t tile_button_height{}; ///< a tile's button's height
    int32_t browse{};          ///< BROWSE MAPS' width
    int32_t card{};            ///< a card's height
    int32_t card_gap{};        ///< between two cards
    int32_t add{};             ///< ADD REGISTRY…'s box's height
    int32_t switch_width{};    ///< a card's switch's width
    int32_t switch_height{};   ///< its height
    int32_t note{};            ///< the note on review's box's height
    int32_t chooser_row{};     ///< a base game's row's height
    int32_t menu_item{};       ///< an item of TAGS' list
    int32_t menu_width{};      ///< TAGS' list's width
    int32_t inset{};           ///< inside a panel, round its content
    int32_t rim{};             ///< from a panel's edge to its label's room
    int32_t label_left{};      ///< from there to its label
    int32_t label_top{};       ///< from a panel's top to its label
    int32_t label_height{};    ///< a label's height
};

/// Lays the screen's fixed parts out on a screen of a size.
///
/// @param width the screen's width, in points
/// @param height the screen's height, in points
/// @param largest_unit the most points a row of the 1080-row window takes
/// @return where they lie
LibraryLayout lay_out_library_menu(int32_t width, int32_t height, double largest_unit) {
    LibraryLayout placed;
    const double columns = std::max(width, 1);
    const double rows = std::max(height, 1);
    // A point is this many rows of the 1080-row window; a window narrower
    // than design_least_columns of them takes a smaller one, centred down.
    const double unit = std::min({rows / design_rows, columns / design_least_columns, largest_unit});
    const double top = (rows - design_rows * unit) / 2.0;
    placed.unit = unit;
    const double middle = columns / 2.0;
    const auto across = [&](double design) { return nearest(design * unit); };
    const auto down = [&](double design) { return nearest(top + design * unit); };
    const int32_t regular_line = kit::smooth_text_points(kit::FontRole::regular) + text_room;
    const int32_t small_line = kit::smooth_text_points(kit::FontRole::small) + text_room;

    placed.inset = across(inset_columns);
    placed.rim = across(rim_columns);
    placed.label_left = across(label_left_columns);
    placed.label_top = across(label_top_rows);
    placed.label_height = std::max(across(label_rows), small_line);
    placed.tab_height = std::max(across(tab_rows), regular_line);
    placed.tab_gap = std::max(across(tab_gap_rows), 1);
    placed.divider = std::max(across(divider_rows), 5);
    placed.icon = std::max(across(icon_columns), 8);
    placed.icon_gap = std::max(across(icon_gap_columns), 3);
    placed.foot_height = std::max(across(foot_rows), small_line + 4);
    placed.search_height = std::max(across(search_rows), regular_line + 4);
    placed.chip_height = std::max(across(chip_rows), small_line + 4);
    placed.tags_width = std::max(across(tags_columns), 48);
    placed.gap = std::max(across(gap_rows), 2);
    placed.row_height = std::max(across(row_rows), regular_line + small_line + 4);
    placed.row_gap = std::max(across(row_gap_rows), 1);
    placed.pad = std::max(across(pad_columns), 3);
    placed.mark = std::max(across(mark_columns), 12);
    placed.version = std::max(across(version_columns), 40);
    placed.bar = std::max(across(bar_rows), 3);
    placed.scroll = std::max(across(scroll_columns), 8);
    placed.scroll_gap = across(scroll_gap_columns);
    placed.least_thumb = std::max(across(least_thumb_rows), 8);
    placed.details_badge = std::max(across(details_badge_columns), 24);
    placed.small_button = std::max(across(small_button_rows), regular_line + 4);
    placed.button_gap = std::max(across(button_gap_columns), 2);
    placed.tag_height = std::max(across(tag_rows), small_line);
    placed.strip = std::max(across(strip_rows), small_line + 2);
    placed.tile_gap = std::max(across(tile_gap_columns), 3);
    placed.pill = std::max(across(pill_rows), regular_line);
    placed.pill_gap = std::max(across(pill_gap_rows), 1);
    placed.tile_button = std::max(across(tile_button_columns), 40);
    placed.tile_button_height = std::max(across(tile_button_rows), regular_line);
    placed.browse = std::max(across(browse_columns), 48);
    placed.card = std::max(across(card_rows), regular_line + 2 * small_line + 8);
    placed.card_gap = std::max(across(card_gap_rows), 2);
    placed.add = std::max(across(add_rows), regular_line * 2);
    placed.switch_width = std::max(across(switch_columns), 40);
    placed.switch_height = std::max(across(switch_rows), regular_line);
    placed.note = std::max(across(note_rows), 2 * small_line + 4);
    placed.chooser_row = std::max(across(chooser_row_rows), regular_line + small_line + 8);
    placed.menu_item = std::max(across(menu_item_rows), small_line + 4);
    placed.menu_width = std::max(across(menu_columns), 80);
    const int32_t band = std::max(across(band_rows), placed.label_top + placed.label_height + 2);

    const int32_t left = nearest(middle - content_columns * unit / 2.0);
    const int32_t content = across(content_columns);
    const int32_t panels_y = down(panels_top);
    const int32_t panels_height = across(panels_rows);
    const int32_t gap = across(panel_gap_columns);
    placed.tabs = {left, panels_y, across(tabs_columns), panels_height};
    const int32_t details_width = across(details_columns);
    placed.details = {left + content - details_width, panels_y, details_width, panels_height};
    const int32_t center_left = placed.tabs.x + placed.tabs.width + gap;
    placed.center = {center_left, panels_y, placed.details.x - gap - center_left, panels_height};
    const auto inside = [&](const kit::Rect& panel) {
        return kit::Rect{
            panel.x + placed.inset,
            panel.y + band,
            panel.width - 2 * placed.inset,
            panel.height - band - placed.inset
        };
    };
    const kit::Rect tab_room = inside(placed.tabs);
    placed.foot = {
        tab_room.x,
        tab_room.y + tab_room.height - placed.foot_height,
        tab_room.width,
        placed.foot_height
    };
    placed.tab_list = {
        tab_room.x, tab_room.y, tab_room.width, tab_room.height - placed.foot_height - placed.gap
    };
    placed.body = inside(placed.center);
    placed.about = inside(placed.details);

    const double buttons_top = panels_top + panels_rows + buttons_gap_rows;
    placed.buttons_y = down(buttons_top);
    placed.button_height = across(button_rows);
    placed.footer_gap = across(footer_gap_columns);
    placed.left = left;
    placed.left_width = across(left_button_columns);
    const int32_t close = across(close_columns);
    placed.close = {left + content - close, placed.buttons_y, close, placed.button_height};
    return placed;
}

/// Draws a check mark in a square.
///
/// @param canvas where it draws
/// @param box the square
/// @param width how thick it is, in points
/// @param ink its colour
void draw_check(const kit::Canvas& canvas, const kit::Rect& box, int32_t width, kit::Colour ink) {
    const auto at = [&](double left, double top) {
        return kit::Point{box.x + nearest(left * box.width), box.y + nearest(top * box.height)};
    };
    draw_line(canvas, at(0.22, 0.52), at(0.42, 0.72), width, ink);
    draw_line(canvas, at(0.42, 0.72), at(0.78, 0.30), width, ink);
}

/// Draws an arrow pointing up or down in a square: a shaft and a head.
///
/// @param canvas where it draws
/// @param box the square
/// @param down it points down; otherwise up
/// @param width how thick its shaft is, in points
/// @param ink its colour
void draw_shaft_arrow(
    const kit::Canvas& canvas, const kit::Rect& box, bool down, int32_t width, kit::Colour ink
) {
    const int32_t middle = box.x + box.width / 2;
    const int32_t top = box.y + box.height / 5;
    const int32_t bottom = box.y + box.height * 4 / 5;
    draw_line(canvas, {middle, top}, {middle, bottom}, width, ink);
    const int32_t reach = std::max(box.width / 4, 2);
    if (down) {
        draw_line(canvas, {middle - reach, bottom - reach}, {middle, bottom}, width, ink);
        draw_line(canvas, {middle + reach, bottom - reach}, {middle, bottom}, width, ink);
    } else {
        draw_line(canvas, {middle - reach, top + reach}, {middle, top}, width, ink);
        draw_line(canvas, {middle + reach, top + reach}, {middle, top}, width, ink);
    }
}

/// Draws the refresh mark: a ring broken at its top right, with an arrow's
/// head at the break.
///
/// @param canvas where it draws
/// @param box the square it fills
/// @param ink its colour
void draw_refresh_mark(const kit::Canvas& canvas, const kit::Rect& box, kit::Colour ink) {
    const int32_t side = std::min(box.width, box.height);
    if (side < 6)
        return;
    const int32_t width = std::max(side / 9, 1);
    const double radius = side * 0.36;
    const double cx = box.x + box.width / 2.0;
    const double cy = box.y + box.height / 2.0;
    constexpr double pi = 3.14159265358979323846;
    // Round from a little past the top, clockwise, to the top's left.
    kit::Point last{};
    for (int32_t step = 0; step <= 40; ++step) {
        const double angle = -pi / 2.0 + pi / 5.0 + (2.0 * pi - pi / 2.5) * step / 40.0;
        const kit::Point at{nearest(cx + radius * std::cos(angle)), nearest(cy + radius * std::sin(angle))};
        if (step > 0)
            draw_line(canvas, last, at, width, ink);
        last = at;
    }
    const kit::Point tip{nearest(cx + radius * std::cos(-pi / 2.0 + pi / 5.0)),
                         nearest(cy + radius * std::sin(-pi / 2.0 + pi / 5.0))};
    const int32_t head = std::max(side / 5, 2);
    draw_line(canvas, tip, {tip.x - head, tip.y - head / 3}, width, ink);
    draw_line(canvas, tip, {tip.x + head / 4, tip.y - head}, width, ink);
}

/// Draws a padlock: its body and its shackle.
///
/// @param canvas where it draws
/// @param box the square it fills
/// @param ink its colour
void draw_padlock(const kit::Canvas& canvas, const kit::Rect& box, kit::Colour ink) {
    const int32_t s = std::min(box.width, box.height);
    if (s < 6)
        return;
    const int32_t x = box.x + (box.width - s) / 2;
    const int32_t y = box.y + (box.height - s) / 2;
    kit::outline_round_rect(canvas, {x + s / 4, y, s / 2, s * 3 / 5}, s / 4, ink);
    kit::fill_rect(canvas, {x + s / 8, y + s * 2 / 5, s * 3 / 4, s * 3 / 5}, ink);
}

/// Draws a tab's picture: a small line drawing in one colour.
///
/// @param canvas where it draws
/// @param box the square it fills
/// @param icon which picture
/// @param ink its colour
void draw_tab_icon(
    const kit::Canvas& canvas, const kit::Rect& box, LibraryMenuIcon icon, kit::Colour ink
) {
    const int32_t s = std::min(box.width, box.height);
    if (s < 6)
        return;
    const int32_t x = box.x + (box.width - s) / 2;
    const int32_t y = box.y + (box.height - s) / 2;
    const int32_t line = std::max(s / 12, 1);
    const auto at = [&](double left, double top, double wide, double high) {
        return kit::Rect{
            x + nearest(left * s),
            y + nearest(top * s),
            std::max(nearest(wide * s), 1),
            std::max(nearest(high * s), 1)
        };
    };
    const auto point = [&](double left, double top) {
        return kit::Point{x + nearest(left * s), y + nearest(top * s)};
    };
    const auto frame = [&](const kit::Rect& rect) {
        for (int32_t step = 0; step < line; ++step)
            kit::outline_rect(
                canvas,
                {rect.x + step, rect.y + step, rect.width - 2 * step, rect.height - 2 * step},
                ink
            );
    };
    switch (icon) {
    case LibraryMenuIcon::active:
        frame(at(0.04, 0.10, 0.92, 0.62));
        kit::fill_rect(canvas, at(0.44, 0.72, 0.12, 0.12), ink);
        kit::fill_rect(canvas, at(0.24, 0.84, 0.52, 0.08), ink);
        break;
    case LibraryMenuIcon::mods:
        kit::fill_rect(canvas, at(0.10, 0.30, 0.60, 0.60), ink);
        kit::fill_round_rect(canvas, at(0.28, 0.08, 0.24, 0.26), nearest(0.12 * s), ink);
        kit::fill_round_rect(canvas, at(0.66, 0.48, 0.26, 0.24), nearest(0.12 * s), ink);
        break;
    case LibraryMenuIcon::maps:
        draw_line(canvas, point(0.06, 0.20), point(0.34, 0.08), line + 1, ink);
        draw_line(canvas, point(0.34, 0.08), point(0.66, 0.20), line + 1, ink);
        draw_line(canvas, point(0.66, 0.20), point(0.94, 0.08), line + 1, ink);
        draw_line(canvas, point(0.06, 0.92), point(0.34, 0.80), line + 1, ink);
        draw_line(canvas, point(0.34, 0.80), point(0.66, 0.92), line + 1, ink);
        draw_line(canvas, point(0.66, 0.92), point(0.94, 0.80), line + 1, ink);
        draw_line(canvas, point(0.06, 0.20), point(0.06, 0.92), line + 1, ink);
        draw_line(canvas, point(0.34, 0.08), point(0.34, 0.80), line + 1, ink);
        draw_line(canvas, point(0.66, 0.20), point(0.66, 0.92), line + 1, ink);
        draw_line(canvas, point(0.94, 0.08), point(0.94, 0.80), line + 1, ink);
        break;
    case LibraryMenuIcon::updates:
        draw_shaft_arrow(canvas, at(0.10, 0.00, 0.80, 0.80), true, line + 1, ink);
        kit::fill_rect(canvas, at(0.06, 0.84, 0.88, 0.10), ink);
        kit::fill_rect(canvas, at(0.06, 0.64, 0.10, 0.24), ink);
        kit::fill_rect(canvas, at(0.84, 0.64, 0.10, 0.24), ink);
        break;
    case LibraryMenuIcon::registries:
        for (const double top : {0.06, 0.36, 0.66}) {
            kit::fill_round_rect(canvas, at(0.10, top, 0.80, 0.26), nearest(0.12 * s), ink);
            kit::fill_rect(canvas, at(0.18, top + 0.10, 0.64, 0.05), entry_steel_colour);
        }
        break;
    }
}

/// Draws a speech mark: a rounded box with a tail and three dots.
///
/// @param canvas where it draws
/// @param box the square it fills
/// @param ink its colour
void draw_speech_mark(const kit::Canvas& canvas, const kit::Rect& box, kit::Colour ink) {
    const int32_t s = std::min(box.width, box.height);
    if (s < 8)
        return;
    const int32_t x = box.x + (box.width - s) / 2;
    const int32_t y = box.y + (box.height - s) / 2;
    const int32_t line = std::max(s / 16, 1);
    const kit::Rect bubble{x + s / 8, y + s / 6, s * 3 / 4, s / 2};
    for (int32_t step = 0; step < line; ++step)
        kit::outline_round_rect(
            canvas,
            {bubble.x + step, bubble.y + step, bubble.width - 2 * step, bubble.height - 2 * step},
            std::max(s / 5 - step, 0),
            ink
        );
    draw_line(
        canvas,
        {bubble.x + bubble.width / 4, bubble.y + bubble.height - 1},
        {bubble.x + bubble.width / 6, bubble.y + bubble.height + s / 6},
        line,
        ink
    );
    draw_line(
        canvas,
        {bubble.x + bubble.width / 6, bubble.y + bubble.height + s / 6},
        {bubble.x + bubble.width * 5 / 12, bubble.y + bubble.height - 1},
        line,
        ink
    );
    const int32_t dot = std::max(s / 12, 2);
    for (int32_t which = 0; which < 3; ++which)
        kit::fill_round_rect(
            canvas,
            {bubble.x + bubble.width * (which + 1) / 4 - dot / 2,
             bubble.y + bubble.height / 2 - dot / 2, dot, dot},
            dot / 2,
            ink
        );
}

/// Draws a shield: the mark beside the note on review.
///
/// @param canvas where it draws
/// @param box the square it fills
/// @param ink its colour
void draw_shield(const kit::Canvas& canvas, const kit::Rect& box, kit::Colour ink) {
    const int32_t s = std::min(box.width, box.height);
    if (s < 8)
        return;
    const int32_t x = box.x + (box.width - s) / 2;
    const int32_t y = box.y + (box.height - s) / 2;
    const int32_t line = std::max(s / 12, 1);
    const kit::Point top_left{x + s / 6, y + s / 6};
    const kit::Point top_right{x + s * 5 / 6, y + s / 6};
    const kit::Point peak{x + s / 2, y + s / 16};
    const kit::Point bottom{x + s / 2, y + s * 15 / 16};
    draw_line(canvas, top_left, peak, line, ink);
    draw_line(canvas, peak, top_right, line, ink);
    draw_line(canvas, top_left, {top_left.x, y + s / 2}, line, ink);
    draw_line(canvas, top_right, {top_right.x, y + s / 2}, line, ink);
    draw_line(canvas, {top_left.x, y + s / 2}, bottom, line, ink);
    draw_line(canvas, {top_right.x, y + s / 2}, bottom, line, ink);
    for (int32_t row = y + s / 6; row < y + s * 3 / 4; ++row) {
        const int32_t reach = row < y + s / 2 ? s / 3 - line
                                              : (s / 3 - line) * (y + s * 15 / 16 - row) / (s * 7 / 16);
        if (reach > 0)
            kit::fill_rect(canvas, {x + s / 2, row, reach, 1}, ink);
    }
}

/// The side of the game's get mark, in its pixels.
constexpr int32_t get_tray_side = 24;
/// The game's get mark: a green arrow dropping into an open steel tray, one
/// letter a pixel. '.' is clear; 'a', 'b' and 'c' are the arrow's edge, light
/// and body; 'd', 'e' and 'f' are the tray's light, face and shade.
constexpr std::array<std::string_view, get_tray_side> get_tray_pixels{
    "........................",
    ".........aaaaaa.........",
    ".........abbbba.........",
    ".........abccca.........",
    ".........abccca.........",
    ".........abccca.........",
    ".........abccca.........",
    ".........abccca.........",
    ".....aaaaabcccaaaaa.....",
    ".....abbbbbccccccca.....",
    "......abccccccccca......",
    ".......abccccccca.......",
    "........abccccca........",
    ".........abccca.........",
    ".ddd......abca......ddd.",
    ".def.......aa.......def.",
    ".def................def.",
    ".def................def.",
    ".def................def.",
    ".deeddddddddddddddddeef.",
    ".deeeeeeeeeeeeeeeeeeeef.",
    ".deeffffffffffffffffeef.",
    ".efffffffffffffffffffff.",
    "........................",
};
/// The get mark's arrow, as the game's scroll arrows: edge, light, body.
constexpr std::array<kit::Colour, 3> get_arrow_colours{{{35, 91, 15}, {27, 159, 19}, {27, 127, 11}}};
/// The get mark's arrow under the pointer, in the game's amber.
constexpr std::array<kit::Colour, 3> get_arrow_lit_colours{{{105, 81, 29}, {248, 191, 70}, {218, 167, 61}}};
/// The get mark's tray: light, face, shade.
constexpr std::array<kit::Colour, 3> get_tray_colours{{{195, 195, 155}, {123, 123, 123}, {43, 43, 43}}};

/// Draws the game's get mark, its pixels whole multiples of the picture's
/// where the square has room, centred.
///
/// @param canvas where it draws
/// @param box the square it fills
/// @param lit whether its arrow is amber, under the pointer
void draw_get_tray(const kit::Canvas& canvas, const kit::Rect& box, bool lit) {
    const int32_t room = std::min(box.width, box.height);
    if (room < 8)
        return;
    const int32_t side = room >= get_tray_side ? room / get_tray_side * get_tray_side : room;
    const int32_t x = box.x + (box.width - side) / 2;
    const int32_t y = box.y + (box.height - side) / 2;
    const std::array<kit::Colour, 3>& arrow = lit ? get_arrow_lit_colours : get_arrow_colours;
    for (int32_t row = 0; row < get_tray_side; ++row) {
        const int32_t top = y + row * side / get_tray_side;
        const int32_t bottom = y + (row + 1) * side / get_tray_side;
        for (int32_t column = 0; column < get_tray_side; ++column) {
            const char letter = get_tray_pixels[row][column];
            if (letter < 'a' || letter > 'f')
                continue;
            const int32_t left = x + column * side / get_tray_side;
            const int32_t right = x + (column + 1) * side / get_tray_side;
            const kit::Colour ink = letter <= 'c' ? arrow[letter - 'a'] : get_tray_colours[letter - 'd'];
            kit::fill_rect(canvas, {left, top, right - left, bottom - top}, ink);
        }
    }
}

/// Returns a rectangle a picture fits in, centred, keeping its shape.
///
/// @param box where it is drawn
/// @param picture the picture
/// @return the part of the box it fills
kit::Rect fitted_rect(const kit::Rect& box, const LibraryMenuPicture& picture) {
    if (picture.width == 0 || picture.height == 0)
        return box;
    const double across = static_cast<double>(box.width) / picture.width;
    const double down = static_cast<double>(box.height) / picture.height;
    const double scale = std::min(across, down);
    const int32_t width = std::max(nearest(picture.width * scale), 1);
    const int32_t height = std::max(nearest(picture.height * scale), 1);
    return {box.x + (box.width - width) / 2, box.y + (box.height - height) / 2, width, height};
}

/// Draws a picture fitted into a box, keeping its shape.
///
/// @param canvas where it draws
/// @param box where it is drawn
/// @param picture the picture; none draws nothing
/// @return true when it was drawn
bool draw_fitted(const kit::Canvas& canvas, const kit::Rect& box, const LibraryMenuPicture& picture) {
    if (picture.pixels == nullptr || picture.width == 0 || picture.height == 0)
        return false;
    oa::ui::frontend_renderer::RgbaPicture shown;
    shown.width = picture.width;
    shown.height = picture.height;
    shown.pixels = std::span<const uint8_t>(
        picture.pixels, static_cast<std::size_t>(picture.width) * picture.height * 4
    );
    kit::draw_picture(canvas, fitted_rect(box, picture), shown);
    return true;
}

/// Returns a word fit for an automation name: lower case letters, digits
/// and hyphens.
///
/// @param text the word as given
/// @return the word; "item" for one with none of them
std::string word_of(std::string_view text) {
    std::string word;
    for (const char character : text) {
        if (character >= 'A' && character <= 'Z')
            word += static_cast<char>(character - 'A' + 'a');
        else if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9'))
            word += character;
        else if (!word.empty() && word.back() != '-')
            word += '-';
    }
    while (!word.empty() && word.back() == '-')
        word.pop_back();
    return word.empty() ? std::string("item") : word;
}

/// The Library screen as a screen of the OA layer.
///
/// One of its points is a pixel of the window, from the room's top left
/// corner, and the screen covers the room.
class LibraryMenuScreen final : public LayerScreen {
  public:

    /// Makes the screen.
    ///
    /// @param name its name on the OA layer
    /// @param facts reads what the Library shows now
    /// @param actions what its presses do
    /// @param game_art the game's art it is drawn with
    LibraryMenuScreen(
        std::string_view name,
        std::function<LibraryMenuFacts()> facts,
        LibraryMenuActions actions,
        GameMenuArt game_art
    )
        : name_(name), facts_(std::move(facts)), actions_(std::move(actions)),
          painter_(std::move(game_art)) {
        facts_now_ = facts_();
    }

    /// Returns the screen's name.
    ///
    /// @return its name
    [[nodiscard]] std::string_view name() const override { return name_; }

    /// Returns where the screen shows: the whole room, at the window's own
    /// pixels, as the game's art is drawn in whole pixels of the window and
    /// scaled with the window rather than with Interface size; nowhere in a
    /// match.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        if (placed_on.match || placed_on.room.width <= 0 || placed_on.room.height <= 0)
            return {};
        const kit::Point size = points_of(placed_on);
        const kit::Rect at = kit::to_canvas({0, 0, size.x, size.y}, window_percent);
        return {{placed_on.room.x, placed_on.room.y, at.width, at.height}, size.x, size.y, window_percent};
    }

    /// Tells that the screen takes every input while it shows.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that the screen darkens nothing: it covers what lies under it.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Draws the game's picture and title with the refresh button right of
    /// it, the three panels, the buttons, and TAGS' list
    /// while it is open.
    ///
    /// @param canvas where it draws, at the layer's scale
    void draw(const kit::Canvas& canvas) const override {
        painter_.draw_background(canvas);
        draw_refresh_button(canvas);
        draw_tabs(canvas);
        draw_center(canvas);
        draw_details(canvas);
        for (std::size_t at = 0; at < facts_now_.left_buttons.size(); ++at)
            draw_footer(
                canvas, {Part::left_button, at}, left_rect(at), facts_now_.left_buttons[at].caption,
                facts_now_.left_buttons[at].enabled
            );
        draw_footer(canvas, {Part::close, 0}, layout_.close, worded("CLOSE"), true);
        if (tags_open_)
            draw_tags_menu(canvas);
        if (facts_now_.question)
            draw_question(canvas);
        else
            question_buttons_.clear();
        if (!facts_now_.switching.empty())
            draw_switching(canvas);
    }

    /// Follows the view: the room's size in the layer's points.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override {
        if (placed_on.match)
            return;
        const kit::Point size = points_of(placed_on);
        if (size.x == size_.x && size.y == size_.y && laid_out_)
            return;
        size_ = size;
        // The parts are laid out on the game's stage, the room round it the
        // picture's alone.
        const kit::Rect stage = game_menu_stage(size_.x, size_.y);
        layout_ = lay_out_library_menu(
            size_.x, size_.y, std::min(stage.height / design_rows, stage.width / design_least_columns)
        );
        painter_.lay_out(size_.x, size_.y, layout_.unit);
        fit_game_art();
        laid_out_ = true;
        place_content();
        ++revision_;
    }

    /// Places the parts that stand by the game's art: the refresh button
    /// right of the game's title, a small button high and centred on the
    /// title's height, and the big buttons, each as wide as its plate is for
    /// their height.
    void fit_game_art() {
        const kit::Rect& title = painter_.title();
        const int32_t height = layout_.small_button;
        layout_.refresh = {
            title.x + title.width + nearest(painter_.picture_scale() * game_refresh_gap),
            title.y + (title.height - height) / 2, height * game_refresh_shape, height
        };
        const int32_t width = painter_.big_button_width(layout_.button_height);
        const int32_t right = layout_.close.x + layout_.close.width;
        layout_.close = {right - width, layout_.buttons_y, width, layout_.button_height};
        layout_.left_width = width;
    }

    /// Follows the pointer over the parts: a press acts when it is released
    /// over the part it was made on; one on the scroll bar's thumb drags the
    /// rows, and one on its arrows or its well scrolls them at once.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button, 1 for the left
    /// @param at the pointer, in the screen's points
    /// @return close when a press closed the Library; redraw when the look
    ///     changed; none otherwise
    LayerAnswer pointer(
        ScreenInputKind kind, uint8_t button, kit::Point at, int32_t /*finger_reach*/
    ) override {
        const uint64_t before = revision_;
        if (kind == ScreenInputKind::pointer_move) {
            if (dragging_thumb_) {
                drag_thumb(at);
                return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
            }
            set_hovered(hit_at(at));
            return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
        }
        if (button != SDL_BUTTON_LEFT)
            return LayerAnswer::none;
        const Hit over = hit_at(at);
        if (kind == ScreenInputKind::pointer_down) {
            // A press outside TAGS' list closes it and goes no further.
            if (tags_open_ && over.part != Part::tag_item && over.part != Part::tags) {
                tags_open_ = false;
                ++revision_;
                return LayerAnswer::redraw;
            }
            set_hovered(over);
            set_pressed(over);
            if (search_focused_ && over.part != Part::search) {
                search_focused_ = false;
                ++revision_;
            }
            switch (over.part) {
            case Part::thumb:
                dragging_thumb_ = true;
                thumb_grab_ = at.y - thumb_rect().y;
                break;
            case Part::scroll_up:
            case Part::scroll_down:
                scroll_by((over.part == Part::scroll_up ? -1 : 1) * pitch());
                break;
            case Part::well:
                scroll_by((at.y < thumb_rect().y ? -1 : 1) * std::max(view_.height - pitch(), 1));
                break;
            case Part::search:
                if (!search_focused_) {
                    search_focused_ = true;
                    ++revision_;
                }
                break;
            default:
                break;
            }
            return LayerAnswer::redraw;
        }
        if (kind != ScreenInputKind::pointer_up)
            return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
        const Hit held = pressed_;
        const bool dragged = dragging_thumb_;
        dragging_thumb_ = false;
        set_pressed({});
        set_hovered(over);
        if (!dragged && held.part != Part::none && held == over && press(held))
            return LayerAnswer::close;
        return LayerAnswer::redraw;
    }

    /// Takes the Library's own keys before the kit's: Ctrl+F (Cmd+F on
    /// macOS) and `/` outside the search find, Ctrl+Tab and Ctrl+Shift+Tab
    /// move between the tabs, and F5 presses the refresh button.
    ///
    /// @param sdl_key the key's SDL keycode
    /// @param modifiers the modifier keys held
    /// @return redraw when it acted; pass otherwise
    LayerAnswer shortcut(uint32_t sdl_key, uint16_t modifiers) override {
#ifdef SDL_PLATFORM_MACOS
        constexpr uint16_t command_modifier = SDL_KMOD_GUI;
#else
        constexpr uint16_t command_modifier = SDL_KMOD_CTRL;
#endif
        // A question takes every key: they go on to key.
        if (facts_now_.question)
            return LayerAnswer::pass;
        if (sdl_key == SDLK_F5) {
            if (!facts_now_.refresh_enabled)
                return LayerAnswer::none;
            return press({Part::refresh, 0}) ? LayerAnswer::close : LayerAnswer::redraw;
        }
        if (sdl_key == SDLK_TAB && (modifiers & SDL_KMOD_CTRL) != 0) {
            step_tab((modifiers & SDL_KMOD_SHIFT) == 0);
            return LayerAnswer::redraw;
        }
        const bool find = (sdl_key == SDLK_F && (modifiers & command_modifier) != 0) ||
                          (sdl_key == SDLK_SLASH && !search_focused_ &&
                           (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) == 0);
        if (find && facts_now_.page == LibraryMenuPage::list) {
            search_focused_ = true;
            ++revision_;
            return LayerAnswer::redraw;
        }
        return LayerAnswer::pass;
    }

    /// Takes the keys: Up and Down choose the row or the card before or
    /// after, Page Up and Page Down move by the rows shown, Home and End go
    /// to the ends, Tab and Shift+Tab show the next and the previous tab,
    /// Return leaves the search, or presses the details' first button,
    /// Backspace and Delete edit the search, and Escape closes TAGS' list, clears the search, closes
    /// the chooser, or closes the Library.
    ///
    /// @param pressed the key
    /// @return close when the Library closes; redraw when it acted; none otherwise
    LayerAnswer key(kit::Key pressed, uint32_t sdl_key) override {
        if (facts_now_.question)
            return question_key(pressed, sdl_key);
        switch (pressed) {
        case kit::Key::escape:
            if (tags_open_) {
                tags_open_ = false;
                ++revision_;
                return LayerAnswer::redraw;
            }
            if (search_focused_ || !facts_now_.list.search.empty()) {
                if (!facts_now_.list.search.empty() && facts_now_.page == LibraryMenuPage::list)
                    ask({LibraryMenuCommand::search, 0, {}, false});
                search_focused_ = false;
                ++revision_;
                return LayerAnswer::redraw;
            }
            if (facts_now_.page == LibraryMenuPage::active && facts_now_.active.chooser_open)
                return press({Part::chooser_cancel, 0}) ? LayerAnswer::close : LayerAnswer::redraw;
            return press({Part::close, 0}) ? LayerAnswer::close : LayerAnswer::redraw;
        case kit::Key::backspace:
        case kit::Key::delete_forward:
            if (!search_focused_ || facts_now_.list.search.empty())
                return LayerAnswer::none;
            {
                std::string words = facts_now_.list.search;
                // The caret stays at the end: Backspace takes the last character.
                std::size_t cut = words.size() - 1;
                while (cut > 0 && kit::continuation_byte(words[cut]))
                    --cut;
                words.erase(cut);
                ask({LibraryMenuCommand::search, 0, std::move(words), false});
            }
            return LayerAnswer::redraw;
        case kit::Key::up:
        case kit::Key::down:
            move_choice(pressed == kit::Key::down ? 1 : -1);
            return LayerAnswer::redraw;
        case kit::Key::page_up:
        case kit::Key::page_down: {
            const int32_t shown = std::max(view_.height / std::max(pitch(), 1), 1);
            move_choice(pressed == kit::Key::page_down ? shown : -shown);
            return LayerAnswer::redraw;
        }
        case kit::Key::home:
            move_choice(-1000000);
            return LayerAnswer::redraw;
        case kit::Key::end:
            move_choice(1000000);
            return LayerAnswer::redraw;
        case kit::Key::tab:
        case kit::Key::back_tab:
            step_tab(pressed == kit::Key::tab);
            return LayerAnswer::redraw;
        case kit::Key::enter:
        case kit::Key::space: {
            if (pressed == kit::Key::space && search_focused_)
                return LayerAnswer::none;
            if (search_focused_) {
                search_focused_ = false;
                ++revision_;
                return LayerAnswer::redraw;
            }
            const LibraryMenuDetails* told = told_details();
            if (told == nullptr || facts_now_.page == LibraryMenuPage::active)
                return LayerAnswer::none;
            for (std::size_t at = 0; at < told->buttons.size(); ++at)
                if (told->buttons[at].enabled)
                    return press({Part::details_button, at}) ? LayerAnswer::close
                                                             : LayerAnswer::redraw;
            return LayerAnswer::none;
        }
        default:
            return LayerAnswer::none;
        }
    }

    /// Takes text typed into the search while it has the focus.
    ///
    /// @param utf8 the text
    /// @return redraw when it searched; pass otherwise
    LayerAnswer text(std::string_view utf8) override {
        if (facts_now_.question)
            return LayerAnswer::none;
        if (!search_focused_ || facts_now_.page != LibraryMenuPage::list)
            return LayerAnswer::pass;
        std::string words = facts_now_.list.search;
        words += utf8;
        ask({LibraryMenuCommand::search, 0, std::move(words), false});
        return LayerAnswer::redraw;
    }

    /// Returns the search field while it has the focus.
    ///
    /// @return the field; none otherwise
    [[nodiscard]] std::optional<kit::Rect> text_field() const override {
        if (!search_focused_ || facts_now_.page != LibraryMenuPage::list)
            return std::nullopt;
        return search_rect();
    }

    /// Scrolls what is under the pointer: TAGS' list, the rows or the cards.
    ///
    /// @param at the pointer, in the screen's points
    /// @param notches how far the wheel turned, positive away from the player
    /// @return redraw when something scrolled; none otherwise
    LayerAnswer wheel(kit::Point at, float notches) override {
        if (facts_now_.question)
            return LayerAnswer::none;
        if (tags_open_ && kit::contains(tags_menu_rect(), at)) {
            const auto count = static_cast<int32_t>(facts_now_.list.tags.size() + 1);
            const int32_t limit = std::max(count - static_cast<int32_t>(most_menu_items), 0);
            const int32_t next = std::clamp(
                tags_first_ - static_cast<int32_t>(std::lround(notches)), 0, limit
            );
            if (next == tags_first_)
                return LayerAnswer::none;
            tags_first_ = next;
            ++revision_;
            return LayerAnswer::redraw;
        }
        if (!kit::contains(view_, at) && !kit::contains(scroll_bar_, at))
            return LayerAnswer::none;
        const int32_t before = scroll_;
        scroll_by(static_cast<int32_t>(std::lround(-notches * wheel_rows_per_notch * pitch())));
        if (scroll_ == before)
            return LayerAnswer::none;
        set_hovered(hit_at(at));
        return LayerAnswer::redraw;
    }

    /// Returns the parts a press can reach, one named control each, for
    /// automation; only the rows and cards the view shows.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        std::set<std::string> taken;
        const auto add = [&](Hit hit, const kit::Rect& rect, const std::string& name,
                             std::string text, bool checked, kit::ControlKind kind) {
            std::string full = "library." + name;
            if (!taken.insert(full).second) {
                for (int32_t again = 2;; ++again)
                    if (taken.insert(full + "-" + std::to_string(again)).second) {
                        full += "-" + std::to_string(again);
                        break;
                    }
            }
            kit::Control control;
            control.id = control_id(hit);
            control.rect = rect;
            control.enabled = enabled(hit);
            control.name = std::move(full);
            control.kind = kind;
            control.text = std::move(text);
            control.checked = checked;
            list.controls.push_back(std::move(control));
            list.tab_order.push_back(control_id(hit));
        };
        // A question's buttons are all a press reaches while it shows.
        if (facts_now_.question) {
            const LibraryMenuQuestion& asked = *facts_now_.question;
            for (std::size_t button = 0; button < asked.buttons.size() && button < question_buttons_.size();
                 ++button)
                add({Part::question_button, button}, question_buttons_[button],
                    "prompt." + word_of(asked.buttons[button].id), asked.buttons[button].caption, false,
                    kit::ControlKind::button);
            return list;
        }
        if (tags_open_) {
            const std::size_t count = facts_now_.list.tags.size() + 1;
            for (std::size_t item = static_cast<std::size_t>(tags_first_);
                 item < count && item < static_cast<std::size_t>(tags_first_) + most_menu_items; ++item)
                add({Part::tag_item, item}, tags_item_rect(item),
                    item < facts_now_.list.tags.size() ? "tags." + word_of(facts_now_.list.tags[item].id)
                                                       : std::string("tags.every"),
                    item < facts_now_.list.tags.size() ? facts_now_.list.tags[item].caption
                                                       : worded("Every tag"),
                    item < facts_now_.list.tags.size() && facts_now_.list.tags[item].chosen,
                    kit::ControlKind::list_item);
        }
        for (std::size_t tab = 0; tab < facts_now_.tabs.size(); ++tab)
            add({Part::tab, tab}, tab_rect(tab), "tab." + word_of(facts_now_.tabs[tab].id),
                facts_now_.tabs[tab].name, tab == facts_now_.chosen, kit::ControlKind::tab);
        add({Part::refresh, 0}, layout_.refresh, "refresh",
            worded(facts_now_.refreshing ? "Refreshing" : "Refresh"), false, kit::ControlKind::button);
        switch (facts_now_.page) {
        case LibraryMenuPage::list: {
            const LibraryMenuList& shown = facts_now_.list;
            add({Part::search, 0}, search_rect(), "search", shown.search, false,
                kit::ControlKind::text_field);
            for (std::size_t chip = 0; chip < shown.filters.size(); ++chip)
                add({Part::filter, chip}, chip_rect(chip), "filter." + word_of(shown.filters[chip].id),
                    shown.filters[chip].caption, shown.filters[chip].chosen, kit::ControlKind::button);
            if (!shown.tags.empty())
                add({Part::tags, 0}, tags_rect(), "tags", tags_caption(),
                    shown.tag_chosen, kit::ControlKind::choice);
            for (std::size_t row = 0; row < shown.rows.size(); ++row) {
                const kit::Rect whole = row_rect(row);
                if (!wholly_in_view(whole))
                    continue;
                const LibraryMenuRow& item = shown.rows[row];
                add({Part::row, row}, whole, "row." + item.id, item.name,
                    shown.selected && *shown.selected == row, kit::ControlKind::list_item);
                if (item.mark == LibraryMenuMark::get || item.mark == LibraryMenuMark::update)
                    add({Part::row_mark, row}, mark_rect(row),
                        "row." + item.id + (item.mark == LibraryMenuMark::get ? ".get" : ".update"),
                        worded(item.mark == LibraryMenuMark::get ? "GET" : "UPDATE"), false,
                        kit::ControlKind::button);
            }
            break;
        }
        case LibraryMenuPage::active: {
            const LibraryMenuActive& shown = facts_now_.active;
            if (shown.chooser_open) {
                for (std::size_t row = 0; row < shown.chooser_rows.size(); ++row)
                    add({Part::chooser_row, row}, chooser_row_rect(row),
                        "base." + word_of(shown.chooser_rows[row].id), shown.chooser_rows[row].name,
                        shown.chooser_rows[row].marked, kit::ControlKind::list_item);
                add({Part::chooser_use, 0}, chooser_button_rect(true), "base.use",
                    worded("USE THIS GAME"), false, kit::ControlKind::button);
                add({Part::chooser_cancel, 0}, chooser_button_rect(false), "base.cancel",
                    worded("CANCEL"), false, kit::ControlKind::button);
                break;
            }
            add({Part::change_base, 0}, tile_button_rect(LibraryMenuTile::base, 0, 1),
                "active.change", worded("CHANGE…"), false, kit::ControlKind::button);
            add({Part::language, 0}, tile_button_rect(LibraryMenuTile::language, 0, 1),
                "active.language", worded("SETTINGS"), false, kit::ControlKind::button);
            const kit::Rect expansions = tile_inside(LibraryMenuTile::expansions);
            for (std::size_t pill = 0; pill < shown.expansions.size(); ++pill) {
                const kit::Rect plate = expansion_rect(pill);
                if (plate.y + plate.height > expansions.y + expansions.height)
                    break;
                add({Part::expansion, pill}, plate,
                    "active.expansion." + word_of(shown.expansions[pill].id),
                    shown.expansions[pill].name, shown.expansions[pill].on, kit::ControlKind::toggle);
            }
            add({Part::choose_mod, 0}, tile_button_rect(LibraryMenuTile::mod, 1, 2),
                "active.choose-mod", worded("CHOOSE MOD"), false, kit::ControlKind::button);
            add({Part::no_mod, 0}, tile_button_rect(LibraryMenuTile::mod, 0, 2), "active.unload",
                worded("NO MOD"), false, kit::ControlKind::button);
            add({Part::browse_maps, 0}, browse_rect(), "active.browse-maps", worded("BROWSE MAPS"),
                false, kit::ControlKind::button);
            for (std::size_t tile = 0; tile < library_menu_tile_count; ++tile) {
                static constexpr std::array<std::string_view, library_menu_tile_count> words{
                    "base", "language", "expansions", "mod", "maps"
                };
                add({Part::tile, tile}, tile_rect(static_cast<LibraryMenuTile>(tile)),
                    "active." + std::string(words[tile]),
                    tile < shown.details.size() ? shown.details[tile].title : std::string(), false,
                    kit::ControlKind::area);
            }
            break;
        }
        case LibraryMenuPage::registries: {
            const LibraryMenuRegistries& shown = facts_now_.registries;
            for (std::size_t card = 0; card < shown.cards.size(); ++card) {
                const kit::Rect whole = card_rect(card);
                if (!wholly_in_view(whole))
                    continue;
                const LibraryMenuRegistry& item = shown.cards[card];
                const std::string word = "registry." + word_of(item.id);
                add({Part::card, card}, whole, word, item.title,
                    shown.selected && *shown.selected == card, kit::ControlKind::list_item);
                add({Part::card_off, card}, switch_half(card, false), word + ".off",
                    worded("OFF"), !item.on, kit::ControlKind::button);
                add({Part::card_on, card}, switch_half(card, true), word + ".on", worded("ON"),
                    item.on, kit::ControlKind::button);
                add({Part::card_refresh, card}, card_refresh_rect(card), word + ".refresh",
                    worded("REFRESH"), false, kit::ControlKind::button);
            }
            if (wholly_in_view(add_rect()))
                add({Part::add_registry, 0}, add_rect(), "add-registry-box", shown.add, false,
                    kit::ControlKind::button);
            break;
        }
        }
        if (scroll_limit() > 0) {
            add({Part::scroll_up, 0}, scroll_arrow_rect(true), "scroll-up", {}, false,
                kit::ControlKind::button);
            add({Part::scroll_down, 0}, scroll_arrow_rect(false), "scroll-down", {}, false,
                kit::ControlKind::button);
        }
        if (const LibraryMenuDetails* told = told_details()) {
            for (std::size_t tag = 0; tag < told->tags.size() && tag < details_tags_.size(); ++tag)
                add({Part::details_tag, tag}, details_tags_[tag], "detail.tag." + word_of(told->tags[tag]),
                    told->tags[tag], false, kit::ControlKind::button);
            for (std::size_t button = 0; button < told->buttons.size(); ++button)
                add({Part::details_button, button}, details_button_rect(button),
                    "action." + told->buttons[button].id, told->buttons[button].caption, false,
                    kit::ControlKind::button);
        }
        for (std::size_t at = 0; at < facts_now_.left_buttons.size(); ++at)
            add({Part::left_button, at}, left_rect(at), facts_now_.left_buttons[at].id,
                facts_now_.left_buttons[at].caption, false, kit::ControlKind::button);
        add({Part::close, 0}, layout_.close, "close", worded("CLOSE"), false, kit::ControlKind::button);
        return list;
    }

    /// Returns the hovered and pressed parts, and the search while it has
    /// the focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown;
        shown.hovered = hovered_.part != Part::none ? control_id(hovered_) : kit::no_control;
        shown.pressed = pressed_.part != Part::none ? control_id(pressed_) : kit::no_control;
        if (facts_now_.question) {
            shown.focused = control_id({Part::question_button, question_marked_});
            shown.focus_shown = true;
        } else if (search_focused_) {
            shown.focused = control_id({Part::search, 0});
            shown.focus_shown = true;
        }
        return shown;
    }

    /// Closes the screen when it no longer shows, follows what the Library
    /// shows, and tells the host what is in view.
    ///
    /// @return close when the screen no longer shows; redraw when what it
    ///     shows changed; otherwise none
    LayerAnswer tick() override {
        if (actions_.shows && !actions_.shows())
            return LayerAnswer::close;
        const bool changed = refresh();
        tell_view();
        return changed ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Returns a number that changes with what the screen draws.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Tells that the screen draws nothing that moves on its own.
    ///
    /// @return true
    [[nodiscard]] bool revision_covers_drawing() const override { return true; }

    /// Has the host store what the Active tab holds, as the Library leaves.
    void close(bool /*by_key*/) override {
        if (closed_)
            return;
        closed_ = true;
        if (actions_.closed)
            actions_.closed();
    }

  private:

    /// Returns the room's size in the screen's points, a point a pixel of
    /// the window.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the width and height, in points
    [[nodiscard]] static kit::Point points_of(const LayerView& placed_on) noexcept {
        return {placed_on.room.width, placed_on.room.height};
    }

    /// Returns a part's control number.
    ///
    /// @param hit the part
    /// @return its number
    [[nodiscard]] static kit::ControlId control_id(Hit hit) noexcept {
        // Each kind has a hundred thousand numbers.
        return static_cast<kit::ControlId>(
            static_cast<int32_t>(hit.part) * 100000 +
            static_cast<int32_t>(std::min<std::size_t>(hit.index, 99999))
        );
    }

    /// Asks the host for a press, and reads the facts again.
    ///
    /// @param asked the press
    /// @return true when the Library closes
    bool ask(const LibraryMenuPress& asked) {
        const bool closes = actions_.press ? actions_.press(asked) : false;
        ++revision_;
        if (!closes)
            refresh();
        return closes;
    }

    /// Reads the facts again, and places the rows again when they changed.
    ///
    /// @return true when what the screen shows changed
    bool refresh() {
        LibraryMenuFacts now = facts_();
        if (now == facts_now_)
            return false;
        const bool other_tab = now.chosen != facts_now_.chosen || now.page != facts_now_.page;
        const bool other_rows = now.list.rows.size() != facts_now_.list.rows.size() ||
                                now.list.search != facts_now_.list.search;
        const std::optional<std::size_t> was_selected = facts_now_.list.selected;
        const bool other_question = now.question != facts_now_.question;
        facts_now_ = std::move(now);
        // A question asked anew takes the pointer and the keys from what
        // lies under it, its button marked at first the one Enter answers.
        if (other_question) {
            tags_open_ = false;
            search_focused_ = false;
            dragging_thumb_ = false;
            hovered_ = {};
            pressed_ = {};
            question_marked_ = facts_now_.question ? facts_now_.question->marked : 0;
        }
        if (other_tab) {
            scroll_ = 0;
            tags_open_ = false;
            tags_first_ = 0;
            search_focused_ = false;
        } else if (other_rows) {
            scroll_ = 0;
        }
        place_content();
        if (facts_now_.list.selected && facts_now_.list.selected != was_selected)
            reveal(row_rect(*facts_now_.list.selected));
        if (!enabled(hovered_))
            hovered_ = {};
        ++revision_;
        return true;
    }

    /// Tells the host which rows and maps are in view, when they changed.
    void tell_view() {
        LibraryMenuView view;
        if (facts_now_.page == LibraryMenuPage::list && pitch() > 0) {
            const std::size_t count = facts_now_.list.rows.size();
            view.first_row = std::min<std::size_t>(static_cast<std::size_t>(scroll_ / pitch()), count);
            view.end_row = std::min<std::size_t>(
                static_cast<std::size_t>((scroll_ + view_.height) / pitch() + 1), count
            );
        }
        if (facts_now_.page == LibraryMenuPage::active && !facts_now_.active.chooser_open)
            view.maps = map_tiles_shown();
        if (told_view_ && *told_view_ == view)
            return;
        told_view_ = view;
        if (actions_.viewed)
            actions_.viewed(view);
    }

    /// Places the rows or the cards from the top of the view, before it
    /// scrolls, and holds the scroll to what they need.
    void place_content() {
        const kit::Rect& body = layout_.body;
        view_ = {};
        scroll_bar_ = {};
        content_height_ = 0;
        switch (facts_now_.page) {
        case LibraryMenuPage::list: {
            const int32_t top = body.y + layout_.search_height + layout_.gap + layout_.chip_height +
                                layout_.gap;
            const int32_t height = std::max(body.y + body.height - top, 1);
            scroll_bar_ = {body.x + body.width - layout_.scroll, top, layout_.scroll, height};
            view_ = {body.x, top, body.width - layout_.scroll - layout_.scroll_gap, height};
            const auto rows = static_cast<int32_t>(facts_now_.list.rows.size());
            content_height_ = std::max(rows * pitch() - layout_.row_gap, 0);
            break;
        }
        case LibraryMenuPage::registries: {
            const int32_t note = layout_.note + (facts_now_.registries.notice.empty() ? 0 : layout_.strip);
            const int32_t height = std::max(body.height - note - layout_.gap, 1);
            scroll_bar_ = {body.x + body.width - layout_.scroll, body.y, layout_.scroll, height};
            view_ = {body.x, body.y, body.width - layout_.scroll - layout_.scroll_gap, height};
            const auto cards = static_cast<int32_t>(facts_now_.registries.cards.size());
            content_height_ = cards * pitch() + layout_.add;
            break;
        }
        case LibraryMenuPage::active:
            break;
        }
        scroll_ = std::clamp(scroll_, 0, scroll_limit());
    }

    /// Returns the distance from one row or card to the next.
    ///
    /// @return the pitch, in points
    [[nodiscard]] int32_t pitch() const noexcept {
        if (facts_now_.page == LibraryMenuPage::registries)
            return layout_.card + layout_.card_gap;
        return layout_.row_height + layout_.row_gap;
    }

    /// Returns how far the rows or cards scroll at most.
    ///
    /// @return the points; 0 when they fit the view
    [[nodiscard]] int32_t scroll_limit() const noexcept {
        return std::max(content_height_ - view_.height, 0);
    }

    /// Scrolls the rows to an offset, held to what they need.
    ///
    /// @param offset the offset, in points from their top
    void scroll_to(int32_t offset) {
        const int32_t held = std::clamp(offset, 0, scroll_limit());
        if (held == scroll_)
            return;
        scroll_ = held;
        ++revision_;
    }

    /// Scrolls the rows by a distance.
    ///
    /// @param distance the points, positive towards their end
    void scroll_by(int32_t distance) { scroll_to(scroll_ + distance); }

    /// Scrolls the least that shows a rectangle of the view whole.
    ///
    /// @param whole the rectangle, where the view shows it now
    void reveal(const kit::Rect& whole) {
        if (whole.height <= 0)
            return;
        if (whole.y < view_.y)
            scroll_by(whole.y - view_.y);
        else if (whole.y + whole.height > view_.y + view_.height)
            scroll_by(whole.y + whole.height - view_.y - view_.height);
    }

    /// Tells whether a rectangle lies wholly in the view.
    ///
    /// @param whole the rectangle
    /// @return true when it does
    [[nodiscard]] bool wholly_in_view(const kit::Rect& whole) const noexcept {
        return whole.y >= view_.y && whole.y + whole.height <= view_.y + view_.height;
    }

    /// Shows the next or the previous tab.
    ///
    /// @param forward the next; otherwise the previous
    void step_tab(bool forward) {
        const std::size_t count = facts_now_.tabs.size();
        if (count == 0)
            return;
        const std::size_t next =
            forward ? (facts_now_.chosen + 1) % count : (facts_now_.chosen + count - 1) % count;
        press({Part::tab, next});
    }

    /// Moves the choice of a row or a card by a number of places, held to
    /// the list.
    ///
    /// @param by the places, positive towards the end
    void move_choice(int32_t by) {
        std::size_t count = 0;
        std::optional<std::size_t> now;
        Part part = Part::none;
        if (facts_now_.page == LibraryMenuPage::list) {
            count = facts_now_.list.rows.size();
            now = facts_now_.list.selected;
            part = Part::row;
        } else if (facts_now_.page == LibraryMenuPage::registries) {
            count = facts_now_.registries.cards.size();
            now = facts_now_.registries.selected;
            part = Part::card;
        }
        if (count == 0)
            return;
        const int64_t from = now ? static_cast<int64_t>(*now) : (by > 0 ? -1 : static_cast<int64_t>(count));
        const auto next = static_cast<std::size_t>(
            std::clamp<int64_t>(from + by, 0, static_cast<int64_t>(count) - 1)
        );
        press({part, next});
        reveal(part == Part::row ? row_rect(next) : card_rect(next));
    }

    /// Returns a tab's plate.
    ///
    /// @param tab the tab, by its place
    /// @return the plate
    [[nodiscard]] kit::Rect tab_rect(std::size_t tab) const noexcept {
        int32_t y = layout_.tab_list.y;
        for (std::size_t at = 0; at <= tab && at < facts_now_.tabs.size(); ++at) {
            if (facts_now_.tabs[at].after_divider && at > 0)
                y += layout_.divider;
            if (at == tab)
                break;
            y += layout_.tab_height + layout_.tab_gap;
        }
        return {layout_.tab_list.x, y, layout_.tab_list.width, layout_.tab_height};
    }

    /// Returns a button's place left of CLOSE.
    ///
    /// @param at the button, from 0 at the left
    /// @return its place
    [[nodiscard]] kit::Rect left_rect(std::size_t at) const noexcept {
        return {
            layout_.left + static_cast<int32_t>(at) * (layout_.left_width + layout_.footer_gap),
            layout_.buttons_y, layout_.left_width, layout_.button_height
        };
    }

    /// Returns the search field, which leaves room for TAGS at its right
    /// when the list has tags.
    ///
    /// @return the field
    [[nodiscard]] kit::Rect search_rect() const noexcept {
        const kit::Rect& body = layout_.body;
        const int32_t right = facts_now_.list.tags.empty() ? body.x + body.width
                                                           : tags_rect().x - layout_.button_gap;
        return {body.x, body.y, std::max(right - body.x, 1), layout_.search_height};
    }

    /// Returns TAGS, at the search field's right, centred on it and as
    /// wide and tall as the last filter below it.
    ///
    /// @return the plate
    [[nodiscard]] kit::Rect tags_rect() const noexcept {
        const kit::Rect& body = layout_.body;
        const std::size_t filters = facts_now_.list.filters.size();
        const kit::Rect below = filters > 0 ? chip_rect(filters - 1) : kit::Rect{};
        const int32_t width = filters > 0 ? below.width : std::min(layout_.tags_width, body.width);
        const int32_t height = std::min(layout_.chip_height, layout_.search_height);
        const int32_t x = filters > 0 ? below.x : body.x + body.width - width;
        return {x, body.y + (layout_.search_height - height) / 2, width, height};
    }

    /// Returns a filter's plate.
    ///
    /// @param chip the plate, by its place
    /// @return the plate
    [[nodiscard]] kit::Rect chip_rect(std::size_t chip) const noexcept {
        const kit::Rect& body = layout_.body;
        const auto count = static_cast<int32_t>(facts_now_.list.filters.size());
        if (count <= 0)
            return {};
        const int32_t width = (body.width - (count - 1) * layout_.button_gap) / count;
        return {
            body.x + static_cast<int32_t>(chip) * (width + layout_.button_gap),
            body.y + layout_.search_height + layout_.gap, std::max(width, 1), layout_.chip_height
        };
    }

    /// Returns TAGS' caption: the tag shown, or TAGS.
    ///
    /// @return the caption
    [[nodiscard]] std::string tags_caption() const {
        for (const LibraryMenuChip& tag : facts_now_.list.tags)
            if (tag.chosen)
                return tag.caption;
        return worded("TAGS");
    }

    /// Returns TAGS' open list.
    ///
    /// @return its place
    [[nodiscard]] kit::Rect tags_menu_rect() const noexcept {
        const kit::Rect chip = tags_rect();
        const std::size_t count = std::min(facts_now_.list.tags.size() + 1, most_menu_items);
        const int32_t width = std::max(layout_.menu_width, chip.width);
        const int32_t right = layout_.body.x + layout_.body.width;
        return {
            std::min(chip.x, right - width), chip.y + chip.height + 2, width,
            static_cast<int32_t>(count) * layout_.menu_item + 4
        };
    }

    /// Returns an item of TAGS' list.
    ///
    /// @param item the item, by its place
    /// @return its place
    [[nodiscard]] kit::Rect tags_item_rect(std::size_t item) const noexcept {
        const kit::Rect menu = tags_menu_rect();
        const auto shown = static_cast<int32_t>(item) - tags_first_;
        return {menu.x + 2, menu.y + 2 + shown * layout_.menu_item, menu.width - 4, layout_.menu_item};
    }

    /// Returns a package's row where the view shows it.
    ///
    /// @param row the row, by its place
    /// @return the row
    [[nodiscard]] kit::Rect row_rect(std::size_t row) const noexcept {
        return {
            view_.x, view_.y + static_cast<int32_t>(row) * pitch() - scroll_, view_.width,
            layout_.row_height
        };
    }

    /// Returns a row's mark.
    ///
    /// @param row the row, by its place
    /// @return the mark's square
    [[nodiscard]] kit::Rect mark_rect(std::size_t row) const noexcept {
        const kit::Rect whole = row_rect(row);
        return {
            whole.x + whole.width - layout_.pad - layout_.mark,
            whole.y + (whole.height - layout_.mark) / 2, layout_.mark, layout_.mark
        };
    }

    /// Returns a registry's card where the view shows it.
    ///
    /// @param card the card, by its place
    /// @return the card
    [[nodiscard]] kit::Rect card_rect(std::size_t card) const noexcept {
        return {
            view_.x, view_.y + static_cast<int32_t>(card) * pitch() - scroll_, view_.width,
            layout_.card
        };
    }

    /// Returns one half of a card's switch.
    ///
    /// @param card the card, by its place
    /// @param on the ON half; otherwise OFF
    /// @return the half
    [[nodiscard]] kit::Rect switch_half(std::size_t card, bool on) const noexcept {
        const kit::Rect whole = card_rect(card);
        const int32_t left = whole.x + whole.width - layout_.pad - layout_.switch_width;
        const int32_t half = layout_.switch_width / 2;
        return {on ? left + half : left, whole.y + layout_.pad, half, layout_.switch_height};
    }

    /// Returns a card's REFRESH.
    ///
    /// @param card the card, by its place
    /// @return the button
    [[nodiscard]] kit::Rect card_refresh_rect(std::size_t card) const noexcept {
        const kit::Rect whole = card_rect(card);
        return {
            whole.x + whole.width - layout_.pad - layout_.switch_width,
            whole.y + whole.height - layout_.pad - layout_.switch_height, layout_.switch_width,
            layout_.switch_height
        };
    }

    /// Returns the ADD REGISTRY… box, after the cards.
    ///
    /// @return the box
    [[nodiscard]] kit::Rect add_rect() const noexcept {
        const auto cards = static_cast<int32_t>(facts_now_.registries.cards.size());
        return {view_.x, view_.y + cards * pitch() - scroll_, view_.width, layout_.add};
    }

    /// Returns the box of the note on review, under the cards.
    ///
    /// @return the box
    [[nodiscard]] kit::Rect note_rect() const noexcept {
        const kit::Rect& body = layout_.body;
        return {body.x, body.y + body.height - layout_.note, body.width, layout_.note};
    }

    /// Returns where an Active tile lies.
    ///
    /// @param tile the tile
    /// @return its place
    [[nodiscard]] kit::Rect tile_rect(LibraryMenuTile tile) const noexcept {
        const kit::Rect& body = layout_.body;
        const int32_t gap = layout_.tile_gap;
        const int32_t room = body.height - 2 * gap;
        const int32_t first = room * 30 / 100;
        const int32_t second = room * 34 / 100;
        const int32_t third = room - first - second;
        const int32_t base_width = (body.width - gap) * 58 / 100;
        const int32_t expansions_width = base_width;
        switch (tile) {
        case LibraryMenuTile::base:
            return {body.x, body.y, base_width, first};
        case LibraryMenuTile::language:
            return {body.x + base_width + gap, body.y, body.width - base_width - gap, first};
        case LibraryMenuTile::expansions:
            return {body.x, body.y + first + gap, expansions_width, second};
        case LibraryMenuTile::mod:
            return {
                body.x + expansions_width + gap, body.y + first + gap,
                body.width - expansions_width - gap, second
            };
        case LibraryMenuTile::maps:
            return {body.x, body.y + first + second + 2 * gap, body.width, third};
        }
        return {};
    }

    /// Returns the inside of a tile, under its strip.
    ///
    /// @param tile the tile
    /// @return its inside
    [[nodiscard]] kit::Rect tile_inside(LibraryMenuTile tile) const noexcept {
        const kit::Rect whole = tile_rect(tile);
        return {
            whole.x + layout_.pad, whole.y + layout_.strip + layout_.pad, whole.width - 2 * layout_.pad,
            std::max(whole.height - layout_.strip - 2 * layout_.pad, 1)
        };
    }

    /// Returns a tile's button at its bottom right, or one of two side by side.
    ///
    /// @param tile the tile
    /// @param place 0 for the right one, 1 for the one left of it
    /// @param count how many the tile has
    /// @return the button
    [[nodiscard]] kit::Rect
    tile_button_rect(LibraryMenuTile tile, int32_t place, int32_t count) const noexcept {
        const kit::Rect inside = tile_inside(tile);
        const int32_t width =
            count > 1 ? std::min(layout_.tile_button, (inside.width - layout_.button_gap) / 2)
                      : std::min(layout_.tile_button, inside.width);
        return {
            inside.x + inside.width - width - place * (width + layout_.button_gap),
            inside.y + inside.height - layout_.tile_button_height, width,
            layout_.tile_button_height
        };
    }

    /// Returns an expansion's plate.
    ///
    /// @param at the expansion, by its place
    /// @return the plate
    [[nodiscard]] kit::Rect expansion_rect(std::size_t at) const noexcept {
        const kit::Rect inside = tile_inside(LibraryMenuTile::expansions);
        // Two lines of words stay free under the plates; plates that do not
        // fit in one column stand in two, and grow thinner while they do
        // not fit in two.
        const int32_t words = 2 * (kit::smooth_text_points(kit::FontRole::small) + 6);
        const int32_t room = std::max(inside.height - words, layout_.pill);
        const auto count = static_cast<int32_t>(facts_now_.active.expansions.size());
        const int32_t pitch = layout_.pill + layout_.pill_gap;
        const int32_t columns = count * pitch - layout_.pill_gap > room ? 2 : 1;
        const int32_t lines = std::max((count + columns - 1) / columns, 1);
        const int32_t height = std::clamp(
            (room + layout_.pill_gap) / lines - layout_.pill_gap,
            kit::smooth_text_points(kit::FontRole::small) + 4, layout_.pill
        );
        const int32_t width = (inside.width - (columns - 1) * layout_.pill_gap) / columns;
        const auto line = static_cast<int32_t>(at) / columns;
        const auto column = static_cast<int32_t>(at) % columns;
        return {
            inside.x + column * (width + layout_.pill_gap),
            inside.y + line * (height + layout_.pill_gap), width, height
        };
    }

    /// Returns BROWSE MAPS.
    ///
    /// @return the button
    [[nodiscard]] kit::Rect browse_rect() const noexcept {
        const kit::Rect inside = tile_inside(LibraryMenuTile::maps);
        return {
            inside.x + inside.width - layout_.browse,
            inside.y + inside.height - layout_.tile_button_height, layout_.browse,
            layout_.tile_button_height
        };
    }

    /// Returns a map's tile on the maps tile.
    ///
    /// @param at the map, by its place
    /// @return its minimap's square, its name under it
    [[nodiscard]] kit::Rect map_rect(std::size_t at) const noexcept {
        const kit::Rect inside = tile_inside(LibraryMenuTile::maps);
        const int32_t side = std::max(inside.height - map_name_room, 8);
        return {
            inside.x + static_cast<int32_t>(at) * (side + layout_.tile_gap), inside.y, side, side
        };
    }

    /// Returns how many maps fit beside BROWSE MAPS.
    ///
    /// @return the count, at most the maps there are
    [[nodiscard]] std::size_t map_tiles_shown() const noexcept {
        const kit::Rect inside = tile_inside(LibraryMenuTile::maps);
        const int32_t side = std::max(inside.height - map_name_room, 8);
        const int32_t room = inside.width - layout_.browse - layout_.tile_gap;
        const auto fit = static_cast<std::size_t>(std::max((room + layout_.tile_gap) / (side + layout_.tile_gap), 0));
        return std::min(fit, facts_now_.active.maps.size());
    }

    /// Returns a base game's row in the chooser.
    ///
    /// @param at the game, by its place
    /// @return the row
    [[nodiscard]] kit::Rect chooser_row_rect(std::size_t at) const noexcept {
        const kit::Rect& body = layout_.body;
        const int32_t top = body.y + layout_.strip + layout_.gap +
                            kit::smooth_text_points(kit::FontRole::small) + text_room + layout_.gap;
        return {
            body.x, top + static_cast<int32_t>(at) * (layout_.chooser_row + layout_.row_gap),
            body.width, layout_.chooser_row
        };
    }

    /// Returns one of the chooser's buttons at its bottom right.
    ///
    /// @param use USE THIS GAME; otherwise CANCEL
    /// @return the button
    [[nodiscard]] kit::Rect chooser_button_rect(bool use) const noexcept {
        const kit::Rect& body = layout_.body;
        const int32_t width = layout_.tile_button * 3 / 2;
        const int32_t right = body.x + body.width - (use ? 0 : width + layout_.button_gap);
        return {
            right - width, body.y + body.height - layout_.small_button, width, layout_.small_button
        };
    }

    /// Returns the details the panel tells of now.
    ///
    /// @return the details; null for none
    [[nodiscard]] const LibraryMenuDetails* told_details() const noexcept {
        switch (facts_now_.page) {
        case LibraryMenuPage::list:
            return facts_now_.details.title.empty() ? nullptr : &facts_now_.details;
        case LibraryMenuPage::registries: {
            const LibraryMenuRegistries& shown = facts_now_.registries;
            if (shown.selected && *shown.selected < shown.cards.size())
                return &shown.cards[*shown.selected].details;
            return nullptr;
        }
        case LibraryMenuPage::active: {
            const auto& details = facts_now_.active.details;
            if (facts_now_.active.chooser_open)
                return nullptr;
            const auto tile = static_cast<std::size_t>(told_tile_);
            return tile < details.size() && !details[tile].title.empty() ? &details[tile] : nullptr;
        }
        }
        return nullptr;
    }

    /// Returns the details' first line of buttons' top.
    ///
    /// @return the row
    [[nodiscard]] int32_t details_buttons_top() const noexcept {
        const LibraryMenuDetails* told = told_details();
        const kit::Rect& about = layout_.about;
        const std::size_t count = told != nullptr ? told->buttons.size() : 0;
        const auto lines = static_cast<int32_t>((count + 1) / 2);
        return about.y + about.height - lines * layout_.small_button -
               std::max(lines - 1, 0) * layout_.button_gap;
    }

    /// Returns one of the details' buttons: two to a line, a last one alone
    /// across the line.
    ///
    /// @param at the button, by its place
    /// @return the button
    [[nodiscard]] kit::Rect details_button_rect(std::size_t at) const noexcept {
        const LibraryMenuDetails* told = told_details();
        if (told == nullptr || at >= told->buttons.size())
            return {};
        const kit::Rect& about = layout_.about;
        const auto line = static_cast<int32_t>(at / 2);
        const int32_t y = details_buttons_top() + line * (layout_.small_button + layout_.button_gap);
        const bool alone = at % 2 == 0 && at + 1 == told->buttons.size();
        if (alone)
            return {about.x, y, about.width, layout_.small_button};
        const int32_t width = (about.width - layout_.button_gap) / 2;
        return {about.x + (at % 2 == 0 ? 0 : width + layout_.button_gap), y, width, layout_.small_button};
    }

    /// Returns one of the scroll bar's arrows.
    ///
    /// @param up the upper arrow; otherwise the lower
    /// @return the arrow
    [[nodiscard]] kit::Rect scroll_arrow_rect(bool up) const noexcept {
        const kit::Rect& bar = scroll_bar_;
        const int32_t side = bar.width;
        return {bar.x, up ? bar.y : bar.y + bar.height - side, side, side};
    }

    /// Returns the scroll bar's well, between its arrows.
    ///
    /// @return the well
    [[nodiscard]] kit::Rect well_rect() const noexcept {
        const kit::Rect up = scroll_arrow_rect(true);
        const kit::Rect down = scroll_arrow_rect(false);
        const int32_t top = up.y + up.height + 2;
        return {scroll_bar_.x, top, scroll_bar_.width, std::max(down.y - 2 - top, 1)};
    }

    /// Returns the scroll bar's thumb.
    ///
    /// @return the thumb
    [[nodiscard]] kit::Rect thumb_rect() const noexcept {
        const kit::Rect well = well_rect();
        const int32_t content = std::max(content_height_, 1);
        const int32_t height = std::clamp(
            static_cast<int32_t>(static_cast<int64_t>(well.height) * view_.height / content),
            std::min(layout_.least_thumb, well.height), well.height
        );
        const int32_t limit = scroll_limit();
        const int32_t travel = well.height - height;
        const int32_t offset =
            limit > 0 ? static_cast<int32_t>(static_cast<int64_t>(travel) * scroll_ / limit) : 0;
        return {well.x, well.y + offset, well.width, height};
    }

    /// Returns the part under a point.
    ///
    /// @param at the point, in the screen's points
    /// @return the part; none when nothing a press reaches is there
    [[nodiscard]] Hit hit_at(kit::Point at) const noexcept {
        if (facts_now_.question) {
            for (std::size_t button = 0; button < question_buttons_.size(); ++button)
                if (kit::contains(question_buttons_[button], at))
                    return {Part::question_button, button};
            return {};
        }
        if (tags_open_) {
            if (kit::contains(tags_menu_rect(), at)) {
                const std::size_t count = facts_now_.list.tags.size() + 1;
                for (std::size_t item = static_cast<std::size_t>(tags_first_);
                     item < count && item < static_cast<std::size_t>(tags_first_) + most_menu_items; ++item)
                    if (kit::contains(tags_item_rect(item), at))
                        return {Part::tag_item, item};
                return {};
            }
        }
        for (std::size_t tab = 0; tab < facts_now_.tabs.size(); ++tab)
            if (kit::contains(tab_rect(tab), at))
                return {Part::tab, tab};
        if (kit::contains(layout_.refresh, at))
            return {Part::refresh, 0};
        if (kit::contains(layout_.body, at)) {
            if (scroll_limit() > 0 && kit::contains(scroll_bar_, at)) {
                if (kit::contains(scroll_arrow_rect(true), at))
                    return {Part::scroll_up, 0};
                if (kit::contains(scroll_arrow_rect(false), at))
                    return {Part::scroll_down, 0};
                if (kit::contains(thumb_rect(), at))
                    return {Part::thumb, 0};
                if (kit::contains(well_rect(), at))
                    return {Part::well, 0};
                return {};
            }
            switch (facts_now_.page) {
            case LibraryMenuPage::list:
                return list_hit(at);
            case LibraryMenuPage::active:
                return active_hit(at);
            case LibraryMenuPage::registries:
                return registries_hit(at);
            }
            return {};
        }
        if (kit::contains(layout_.about, at)) {
            if (const LibraryMenuDetails* told = told_details()) {
                for (std::size_t button = 0; button < told->buttons.size(); ++button)
                    if (kit::contains(details_button_rect(button), at))
                        return {Part::details_button, button};
                for (std::size_t tag = 0; tag < told->tags.size() && tag < details_tags_.size(); ++tag)
                    if (kit::contains(details_tags_[tag], at))
                        return {Part::details_tag, tag};
            }
            return {};
        }
        for (std::size_t button = 0; button < facts_now_.left_buttons.size(); ++button)
            if (kit::contains(left_rect(button), at))
                return {Part::left_button, button};
        if (kit::contains(layout_.close, at))
            return {Part::close, 0};
        return {};
    }

    /// Returns the part of a list's page under a point.
    ///
    /// @param at the point, in the body
    /// @return the part
    [[nodiscard]] Hit list_hit(kit::Point at) const noexcept {
        if (kit::contains(search_rect(), at))
            return {Part::search, 0};
        const std::size_t filters = facts_now_.list.filters.size();
        for (std::size_t chip = 0; chip < filters; ++chip)
            if (kit::contains(chip_rect(chip), at))
                return {Part::filter, chip};
        if (!facts_now_.list.tags.empty() && kit::contains(tags_rect(), at))
            return {Part::tags, 0};
        if (!kit::contains(view_, at))
            return {};
        const int32_t place = (at.y - view_.y + scroll_) / std::max(pitch(), 1);
        if (place < 0 || static_cast<std::size_t>(place) >= facts_now_.list.rows.size())
            return {};
        const auto row = static_cast<std::size_t>(place);
        if (!kit::contains(row_rect(row), at))
            return {};
        const LibraryMenuRow& item = facts_now_.list.rows[row];
        if (item.mark_pressable && kit::contains(mark_rect(row), at))
            return {Part::row_mark, row};
        return {Part::row, row};
    }

    /// Returns the part of the Active tab under a point.
    ///
    /// @param at the point, in the body
    /// @return the part
    [[nodiscard]] Hit active_hit(kit::Point at) const noexcept {
        const LibraryMenuActive& shown = facts_now_.active;
        if (shown.chooser_open) {
            if (kit::contains(chooser_button_rect(true), at))
                return {Part::chooser_use, 0};
            if (kit::contains(chooser_button_rect(false), at))
                return {Part::chooser_cancel, 0};
            for (std::size_t row = 0; row < shown.chooser_rows.size(); ++row)
                if (kit::contains(chooser_row_rect(row), at))
                    return {Part::chooser_row, row};
            return {};
        }
        if (kit::contains(tile_button_rect(LibraryMenuTile::base, 0, 1), at))
            return {Part::change_base, 0};
        if (kit::contains(tile_button_rect(LibraryMenuTile::language, 0, 1), at))
            return {Part::language, 0};
        if (kit::contains(tile_button_rect(LibraryMenuTile::mod, 0, 2), at))
            return {Part::no_mod, 0};
        if (kit::contains(tile_button_rect(LibraryMenuTile::mod, 1, 2), at))
            return {Part::choose_mod, 0};
        if (kit::contains(browse_rect(), at))
            return {Part::browse_maps, 0};
        const kit::Rect expansions = tile_inside(LibraryMenuTile::expansions);
        for (std::size_t pill = 0; pill < shown.expansions.size(); ++pill) {
            const kit::Rect plate = expansion_rect(pill);
            if (plate.y + plate.height > expansions.y + expansions.height)
                break;
            if (kit::contains(plate, at))
                return {Part::expansion, pill};
        }
        for (std::size_t tile = 0; tile < library_menu_tile_count; ++tile)
            if (kit::contains(tile_rect(static_cast<LibraryMenuTile>(tile)), at))
                return {Part::tile, tile};
        return {};
    }

    /// Returns the part of the Registries tab under a point.
    ///
    /// @param at the point, in the body
    /// @return the part
    [[nodiscard]] Hit registries_hit(kit::Point at) const noexcept {
        if (!kit::contains(view_, at))
            return {};
        const LibraryMenuRegistries& shown = facts_now_.registries;
        for (std::size_t card = 0; card < shown.cards.size(); ++card) {
            if (!kit::contains(card_rect(card), at))
                continue;
            if (kit::contains(switch_half(card, false), at))
                return {Part::card_off, card};
            if (kit::contains(switch_half(card, true), at))
                return {Part::card_on, card};
            if (kit::contains(card_refresh_rect(card), at))
                return {Part::card_refresh, card};
            return {Part::card, card};
        }
        if (kit::contains(add_rect(), at))
            return {Part::add_registry, 0};
        return {};
    }

    /// Tells whether a part takes a press.
    ///
    /// @param hit the part
    /// @return true when it does
    [[nodiscard]] bool enabled(Hit hit) const noexcept {
        const LibraryMenuActive& active = facts_now_.active;
        const LibraryMenuRegistries& registries = facts_now_.registries;
        switch (hit.part) {
        case Part::none:
            return false;
        case Part::tab:
            return hit.index < facts_now_.tabs.size();
        case Part::search:
            return facts_now_.page == LibraryMenuPage::list;
        case Part::filter:
            return facts_now_.page == LibraryMenuPage::list && hit.index < facts_now_.list.filters.size();
        case Part::tags:
            return facts_now_.page == LibraryMenuPage::list && !facts_now_.list.tags.empty();
        case Part::tag_item:
            return tags_open_ && hit.index <= facts_now_.list.tags.size();
        case Part::row:
            return facts_now_.page == LibraryMenuPage::list && hit.index < facts_now_.list.rows.size();
        case Part::row_mark:
            return facts_now_.page == LibraryMenuPage::list && hit.index < facts_now_.list.rows.size() &&
                   facts_now_.list.rows[hit.index].mark_pressable;
        case Part::scroll_up:
        case Part::scroll_down:
        case Part::well:
        case Part::thumb:
            return scroll_limit() > 0;
        case Part::details_tag: {
            const LibraryMenuDetails* told = told_details();
            return told != nullptr && hit.index < told->tags.size();
        }
        case Part::details_button: {
            const LibraryMenuDetails* told = told_details();
            return told != nullptr && hit.index < told->buttons.size() && told->buttons[hit.index].enabled;
        }
        case Part::left_button:
            return hit.index < facts_now_.left_buttons.size() &&
                   facts_now_.left_buttons[hit.index].enabled;
        case Part::refresh:
            return facts_now_.refresh_enabled;
        case Part::close:
            return true;
        case Part::tile:
            return facts_now_.page == LibraryMenuPage::active && !active.chooser_open &&
                   hit.index < library_menu_tile_count;
        case Part::change_base:
            return facts_now_.page == LibraryMenuPage::active && !active.chooser_open && active.change_enabled;
        case Part::language:
        case Part::choose_mod:
        case Part::browse_maps:
            return facts_now_.page == LibraryMenuPage::active && !active.chooser_open;
        case Part::no_mod:
            return facts_now_.page == LibraryMenuPage::active && !active.chooser_open && active.no_mod_enabled;
        case Part::expansion:
            return facts_now_.page == LibraryMenuPage::active && !active.chooser_open &&
                   hit.index < active.expansions.size() && active.expansions[hit.index].enabled;
        case Part::chooser_row:
            return facts_now_.page == LibraryMenuPage::active && active.chooser_open &&
                   hit.index < active.chooser_rows.size();
        case Part::chooser_use:
        case Part::chooser_cancel:
            return facts_now_.page == LibraryMenuPage::active && active.chooser_open;
        case Part::card:
        case Part::card_off:
        case Part::card_on:
            return facts_now_.page == LibraryMenuPage::registries && hit.index < registries.cards.size();
        case Part::card_refresh:
            return facts_now_.page == LibraryMenuPage::registries && hit.index < registries.cards.size() &&
                   registries.cards[hit.index].refresh_enabled;
        case Part::add_registry:
            return facts_now_.page == LibraryMenuPage::registries;
        case Part::question_button:
            return facts_now_.question && hit.index < facts_now_.question->buttons.size() &&
                   facts_now_.question->buttons[hit.index].enabled;
        }
        return false;
    }

    /// Presses a part that takes a press.
    ///
    /// @param hit the part
    /// @return true when the Library closes
    bool press(Hit hit) {
        if (!enabled(hit))
            return false;
        const auto plain = [&](LibraryMenuCommand command) {
            return ask({command, hit.index, {}, false});
        };
        switch (hit.part) {
        case Part::tab:
            told_tile_ = LibraryMenuTile::mod;
            return plain(LibraryMenuCommand::tab);
        case Part::filter:
            return plain(LibraryMenuCommand::filter);
        case Part::tags:
            tags_open_ = !tags_open_;
            tags_first_ = 0;
            ++revision_;
            return false;
        case Part::tag_item:
            tags_open_ = false;
            return plain(LibraryMenuCommand::tag);
        case Part::row:
            return plain(LibraryMenuCommand::select);
        case Part::row_mark:
            return plain(LibraryMenuCommand::row_mark);
        case Part::details_tag:
            return plain(LibraryMenuCommand::details_tag);
        case Part::details_button:
            return plain(LibraryMenuCommand::details_button);
        case Part::left_button:
            return plain(LibraryMenuCommand::left_button);
        case Part::refresh:
            return plain(LibraryMenuCommand::refresh);
        case Part::close:
            return plain(LibraryMenuCommand::close);
        case Part::tile:
            told_tile_ = static_cast<LibraryMenuTile>(hit.index);
            ++revision_;
            return false;
        case Part::change_base:
            return plain(LibraryMenuCommand::change_base);
        case Part::language:
            return plain(LibraryMenuCommand::language);
        case Part::expansion:
            told_tile_ = LibraryMenuTile::expansions;
            return plain(LibraryMenuCommand::expansion);
        case Part::choose_mod:
            return plain(LibraryMenuCommand::choose_mod);
        case Part::no_mod:
            return plain(LibraryMenuCommand::no_mod);
        case Part::browse_maps:
            return plain(LibraryMenuCommand::browse_maps);
        case Part::chooser_row:
            return plain(LibraryMenuCommand::chooser_mark);
        case Part::chooser_use:
            return plain(LibraryMenuCommand::chooser_use);
        case Part::chooser_cancel:
            return plain(LibraryMenuCommand::chooser_cancel);
        case Part::card:
            return plain(LibraryMenuCommand::registry_select);
        case Part::card_off:
        case Part::card_on: {
            ask({LibraryMenuCommand::registry_select, hit.index, {}, false});
            return ask({LibraryMenuCommand::registry_switch, hit.index, {}, hit.part == Part::card_on});
        }
        case Part::card_refresh:
            ask({LibraryMenuCommand::registry_select, hit.index, {}, false});
            return plain(LibraryMenuCommand::registry_refresh);
        case Part::add_registry:
            return plain(LibraryMenuCommand::add_registry);
        case Part::question_button:
            return plain(LibraryMenuCommand::question_button);
        default:
            return false;
        }
    }

    /// Scrolls the rows so that the dragged thumb follows the pointer.
    ///
    /// @param at the pointer, in the screen's points
    void drag_thumb(kit::Point at) {
        const kit::Rect well = well_rect();
        const kit::Rect thumb = thumb_rect();
        const int32_t travel = well.height - thumb.height;
        if (travel <= 0)
            return;
        const int32_t top = std::clamp(at.y - thumb_grab_, well.y, well.y + travel);
        scroll_to(static_cast<int32_t>(static_cast<int64_t>(scroll_limit()) * (top - well.y) / travel));
    }

    /// Sets the part under the pointer, counting a change; an Active tile it
    /// lies in becomes the one DETAILS tells of.
    ///
    /// @param hit the part
    void set_hovered(Hit hit) {
        if (!enabled(hit))
            hit = {};
        if (hit == hovered_)
            return;
        hovered_ = hit;
        if (facts_now_.page == LibraryMenuPage::active) {
            std::optional<LibraryMenuTile> tile;
            switch (hit.part) {
            case Part::tile:
                tile = static_cast<LibraryMenuTile>(hit.index);
                break;
            case Part::change_base:
                tile = LibraryMenuTile::base;
                break;
            case Part::language:
                tile = LibraryMenuTile::language;
                break;
            case Part::expansion:
                tile = LibraryMenuTile::expansions;
                break;
            case Part::choose_mod:
            case Part::no_mod:
                tile = LibraryMenuTile::mod;
                break;
            case Part::browse_maps:
                tile = LibraryMenuTile::maps;
                break;
            default:
                break;
            }
            if (tile)
                told_tile_ = *tile;
        }
        ++revision_;
    }

    /// Sets the part a press holds, counting a change.
    ///
    /// @param hit the part
    void set_pressed(Hit hit) {
        if (hit == pressed_)
            return;
        pressed_ = hit;
        ++revision_;
    }

    /// Returns how many points each pixel of the game's font covers in the
    /// panels: as in an expansion's row, the line the font fills best.
    ///
    /// @return the points a pixel
    [[nodiscard]] int32_t game_square() const { return painter_.text_square(layout_.pill); }

    /// Returns a measure of texts in the game's font.
    ///
    /// @return the measure, in points
    [[nodiscard]] kit::Measure measure() const { return painter_.measure(game_square()); }

    /// Returns a line's height in the game's font, its letters a pixel of
    /// the font apart.
    ///
    /// @return the line's height, in points
    [[nodiscard]] int32_t line() const {
        const int32_t square = game_square();
        return static_cast<int32_t>(painter_.caption("A", Ink::plain).height) * square + square;
    }

    /// Returns the tint of the game's font a colour of the screen is drawn
    /// in: amber for the amber, darkened for the faded, the font's own for
    /// the rest.
    ///
    /// @param ink the colour
    /// @return the tint
    [[nodiscard]] static Ink ink_of(kit::Colour ink) noexcept {
        if (ink == warn_colour || ink == chosen_colour || ink == chosen_text_colour)
            return Ink::lit;
        if (ink == note_colour || ink == caption_faded_colour)
            return Ink::faded;
        return Ink::plain;
    }

    /// Writes a line of text in a box, in the game's font.
    ///
    /// @param canvas where it draws
    /// @param text the text, fitted to the box
    /// @param box the box
    /// @param align where the text sits along the box's width
    /// @param ink its colour, tinted as ink_of tells
    void write(
        const kit::Canvas& canvas, std::string_view text, const kit::Rect& box, kit::Align align, kit::Colour ink
    ) const {
        painter_.draw_text_at(canvas, box, text, align, ink_of(ink), game_square());
    }

    /// Draws a panel's frame, the game's.
    ///
    /// @param canvas where it draws
    /// @param panel the panel
    void draw_frame(const kit::Canvas& canvas, const kit::Rect& panel) const {
        painter_.draw_frame(canvas, panel);
    }

    /// Draws a box a row, a card, a tile or words stand in: the game's list
    /// box.
    ///
    /// @param canvas where it draws
    /// @param box the box
    void draw_box(const kit::Canvas& canvas, const kit::Rect& box) const { painter_.draw_box(canvas, box); }

    /// Marks a row, a card or a plate as chosen: an amber ring round it.
    ///
    /// @param canvas where it draws
    /// @param box what is chosen
    void draw_chosen(const kit::Canvas& canvas, const kit::Rect& box) const {
        draw_ring(canvas, box, std::max(game_square(), 1), game_amber);
    }

    /// Draws a strip with a title: the game's sunk bar.
    ///
    /// @param canvas where it draws
    /// @param strip the strip
    /// @param title its title
    /// @param lit the pointer is over what it heads
    void draw_strip(const kit::Canvas& canvas, const kit::Rect& strip, std::string_view title, bool lit = false) const {
        const kit::Rect words{strip.x + layout_.pad, strip.y, strip.width - 2 * layout_.pad, strip.height};
        painter_.draw_well(canvas, strip);
        painter_.draw_text_at(canvas, words, title, kit::Align::left, lit ? Ink::lit : Ink::plain, game_square());
    }

    /// Draws a panel's label in the game's font, at its top left.
    ///
    /// @param canvas where it draws
    /// @param panel the panel
    /// @param text the label
    void draw_label(const kit::Canvas& canvas, const kit::Rect& panel, std::string_view text) const {
        const int32_t left = panel.x + layout_.rim + layout_.label_left;
        const int32_t reach = panel.x + panel.width - layout_.rim - layout_.inset;
        painter_.draw_text(
            canvas, {left, panel.y + layout_.label_top, reach - left, layout_.label_height}, text,
            kit::Align::left, Ink::plain
        );
    }

    /// Draws a small button with its caption: the game's plain button.
    ///
    /// @param canvas where it draws
    /// @param rect the button
    /// @param hit its part
    /// @param caption its words
    void draw_small_button(
        const kit::Canvas& canvas, const kit::Rect& rect, Hit hit, std::string_view caption
    ) const {
        const bool usable = enabled(hit);
        const bool lit = usable && hovered_ == hit;
        painter_.draw_captioned_button(canvas, rect, caption, usable, lit, lit && pressed_ == hit);
    }

    /// Draws a tile: a list box under a strip with its title.
    ///
    /// @param canvas where it draws
    /// @param whole the tile
    /// @param title its strip's title
    /// @param lit the pointer is over it
    void draw_tile_frame(
        const kit::Canvas& canvas, const kit::Rect& whole, std::string_view title, bool lit
    ) const {
        draw_box(canvas, whole);
        draw_strip(canvas, {whole.x, whole.y, whole.width, layout_.strip}, title, lit);
    }

    /// Draws the header's refresh button: the game's plain button with its
    /// caption, amber while the registries are asked.
    ///
    /// @param canvas where it draws
    void draw_refresh_button(const kit::Canvas& canvas) const {
        const Hit hit{Part::refresh, 0};
        const bool lit = enabled(hit) && hovered_ == hit;
        painter_.draw_captioned_button(
            canvas, layout_.refresh, worded("REFRESH"), enabled(hit), lit || facts_now_.refreshing,
            lit && pressed_ == hit
        );
    }

    /// Draws the LIBRARY panel: a plate for each tab with its picture and its
    /// name, the chosen one held, the dividers, and the box at its foot.
    ///
    /// @param canvas where it draws
    void draw_tabs(const kit::Canvas& canvas) const {
        draw_frame(canvas, layout_.tabs);
        draw_label(canvas, layout_.tabs, worded("LIBRARY"));
        const kit::Measure measured = measure();
        for (std::size_t tab = 0; tab < facts_now_.tabs.size(); ++tab) {
            const LibraryMenuTab& shown = facts_now_.tabs[tab];
            const kit::Rect plate = tab_rect(tab);
            // The divider is the frame's tube, from the frame's left tube to
            // its right.
            if (shown.after_divider && tab > 0)
                painter_.draw_rule(canvas, layout_.tabs, plate.y - (layout_.divider + layout_.tab_gap) / 2);
            const bool chosen = tab == facts_now_.chosen;
            const Hit hit{Part::tab, tab};
            const bool lit = hovered_ == hit;
            const bool held = lit && pressed_ == hit;
            // The game's button stands held for the chosen tab; only the
            // name under the pointer turns amber.
            painter_.draw_plain_button(canvas, plate, chosen || held ? 1 : 0);
            const int32_t icon = std::min(layout_.icon, plate.height - 6);
            const kit::Rect picture{plate.x + layout_.icon_gap, plate.y + (plate.height - icon) / 2, icon, icon};
            draw_tab_icon(canvas, picture, shown.icon, game_icon_colour);
            const int32_t dot = shown.attention ? std::max(plate.height / 4, 6) : 0;
            const int32_t text_left = picture.x + picture.width + layout_.icon_gap;
            const int32_t text_right = plate.x + plate.width - layout_.icon_gap - (dot > 0 ? dot + layout_.icon_gap : 0);
            const kit::Rect words{text_left, plate.y, text_right - text_left, plate.height};
            painter_.draw_text_at(canvas, words, shown.name, kit::Align::left, lit ? Ink::lit : Ink::plain, game_square());
            if (dot > 0)
                kit::fill_round_rect(
                    canvas,
                    {plate.x + plate.width - layout_.icon_gap - dot, plate.y + (plate.height - dot) / 2, dot, dot},
                    dot / 2, game_amber
                );
        }
        // The download queue's line stands over the foot while it has one.
        if (!facts_now_.queue.empty()) {
            kit::WrapRules rules;
            rules.most_lines = 2;
            const int32_t height = line();
            const auto lines = kit::wrap(facts_now_.queue, layout_.foot.width, measured, rules);
            int32_t y = layout_.foot.y - layout_.gap - static_cast<int32_t>(lines.size()) * height;
            for (const std::string& text : lines) {
                write(canvas, text, {layout_.foot.x, y, layout_.foot.width, height}, kit::Align::left, warn_colour);
                y += height;
            }
        }
        painter_.draw_well(canvas, layout_.foot);
        write(
            canvas, kit::fit(facts_now_.installed, layout_.foot.width - 2 * layout_.pad, measured),
            {layout_.foot.x + layout_.pad, layout_.foot.y, layout_.foot.width - 2 * layout_.pad,
             layout_.foot.height},
            kit::Align::left, plate_text_colour
        );
    }

    /// Draws the chosen tab's panel.
    ///
    /// @param canvas where it draws
    void draw_center(const kit::Canvas& canvas) const {
        draw_frame(canvas, layout_.center);
        draw_label(canvas, layout_.center, facts_now_.heading);
        switch (facts_now_.page) {
        case LibraryMenuPage::list:
            draw_list(canvas);
            break;
        case LibraryMenuPage::active:
            if (facts_now_.active.chooser_open)
                draw_chooser(canvas);
            else
                draw_active(canvas);
            break;
        case LibraryMenuPage::registries:
            draw_registries(canvas);
            break;
        }
        if (scroll_limit() > 0)
            draw_scroll_bar(canvas);
    }

    /// Draws a list's page: the search, the filters and the rows.
    ///
    /// @param canvas where it draws
    void draw_list(const kit::Canvas& canvas) const {
        const LibraryMenuList& shown = facts_now_.list;
        const kit::Measure measured = measure();
        // The search: a field with a magnifier, its words or its hint; the
        // game's black list box, as the rows.
        const kit::Rect field = search_rect();
        painter_.draw_box(canvas, field);
        if (search_focused_)
            draw_ring(canvas, field, std::max(game_square(), 1), game_amber);
        const int32_t glass = std::max(field.height / 2, 8);
        const kit::Rect lens{field.x + layout_.pad, field.y + (field.height - glass) / 2, glass * 3 / 4, glass * 3 / 4};
        draw_circle(canvas, lens, std::max(glass / 10, 1), game_icon_colour);
        draw_line(
            canvas, {lens.x + lens.width - 1, lens.y + lens.height - 1},
            {lens.x + glass - 1, lens.y + glass - 1}, std::max(glass / 8, 1), game_icon_colour
        );
        const int32_t text_left = field.x + layout_.pad * 2 + glass;
        const int32_t text_width = field.x + field.width - layout_.pad - text_left;
        const bool hint = shown.search.empty();
        std::string words = hint ? shown.search_hint : shown.search;
        const int32_t caret_room = search_focused_ ? 4 : 0;
        if (!hint && measured(words) > text_width - caret_room) {
            // The end shows, as it is being typed.
            while (!words.empty() && measured("…" + words) > text_width - caret_room)
                words.erase(0, kit::character_bytes(words));
            words = "…" + words;
        }
        write(
            canvas, hint ? kit::fit(words, text_width, measured) : words,
            {text_left, field.y, text_width, field.height}, kit::Align::left,
            hint ? note_colour : caption_lit_colour
        );
        // The caret, after the words typed.
        if (search_focused_) {
            const int32_t caret_height = line();
            kit::fill_rect(
                canvas,
                {text_left + (hint ? 0 : measured(words)) + 1, field.y + (field.height - caret_height) / 2,
                 std::max(game_square(), 2), caret_height},
                game_amber
            );
        }
        // The filters, then TAGS beside the search.
        for (std::size_t chip = 0; chip < shown.filters.size(); ++chip)
            draw_chip(canvas, chip_rect(chip), {Part::filter, chip}, shown.filters[chip].caption,
                      shown.filters[chip].chosen);
        if (!shown.tags.empty())
            draw_chip(canvas, tags_rect(), {Part::tags, 0}, tags_caption() + " ▾",
                      shown.tag_chosen || tags_open_);
        // The rows.
        if (shown.rows.empty()) {
            write(canvas, kit::fit(shown.empty, view_.width, measured), view_, kit::Align::centre, note_colour);
            return;
        }
        const kit::Canvas inside = kit::clipped(canvas, view_);
        for (std::size_t row = 0; row < shown.rows.size(); ++row) {
            const kit::Rect whole = row_rect(row);
            if (whole.y + whole.height < view_.y || whole.y > view_.y + view_.height)
                continue;
            draw_row(inside, row);
        }
    }

    /// Draws a filter's plate: the game's plain button, held while chosen.
    ///
    /// @param canvas where it draws
    /// @param rect the plate
    /// @param hit its part
    /// @param caption its words
    /// @param chosen it is chosen
    void draw_chip(
        const kit::Canvas& canvas, const kit::Rect& rect, Hit hit, std::string_view caption, bool chosen
    ) const {
        const bool lit = hovered_ == hit;
        painter_.draw_captioned_button(canvas, rect, caption, true, lit, chosen || (lit && pressed_ == hit));
    }

    /// Draws a badge: the game's list box with the picture in it, or its
    /// letters.
    ///
    /// @param canvas where it draws
    /// @param box the badge
    /// @param letters the letters while it has no picture
    /// @param picture its picture
    void draw_badge(
        const kit::Canvas& canvas, const kit::Rect& box, std::string_view letters, const LibraryMenuPicture& picture
    ) const {
        painter_.draw_box(canvas, box);
        const int32_t edge = std::max(box.width / 14, 2);
        const kit::Rect inside{box.x + edge, box.y + edge, box.width - 2 * edge, box.height - 2 * edge};
        if (draw_fitted(canvas, inside, picture))
            return;
        painter_.draw_text(canvas, inside, letters, kit::Align::centre, Ink::plain);
    }

    /// Draws a package's row: its badge, its name and line, its version and
    /// size, its mark, and its download's bar.
    ///
    /// @param canvas where it draws, clipped to the view
    /// @param row the row, by its place
    void draw_row(const kit::Canvas& canvas, std::size_t row) const {
        const LibraryMenuRow& shown = facts_now_.list.rows[row];
        const kit::Rect whole = row_rect(row);
        const bool chosen = facts_now_.list.selected && *facts_now_.list.selected == row;
        const bool lit = (hovered_ == Hit{Part::row, row}) || (hovered_ == Hit{Part::row_mark, row});
        draw_box(canvas, whole);
        if (lit && !chosen)
            kit::blend_rect(canvas, whole, game_amber, row_light_opacity);
        const kit::Measure measured = measure();
        const int32_t badge = whole.height - 2 * layout_.pad;
        draw_badge(canvas, {whole.x + layout_.pad, whole.y + layout_.pad, badge, badge}, shown.letters, shown.picture);
        const kit::Rect mark = mark_rect(row);
        const int32_t version_left = mark.x - layout_.pad - layout_.version;
        const int32_t text_left = whole.x + 2 * layout_.pad + badge;
        const int32_t text_width = std::max(version_left - layout_.pad - text_left, 1);
        const int32_t text_line = line();
        const int32_t lines_top = whole.y + (whole.height - 2 * text_line - (shown.progress ? layout_.bar + 4 : 0)) / 2;
        write(
            canvas, kit::fit(shown.name, text_width, measured), {text_left, lines_top, text_width, text_line},
            kit::Align::left, shown.dimmed ? caption_faded_colour : caption_lit_colour
        );
        write(
            canvas, kit::fit(shown.line, text_width, measured),
            {text_left, lines_top + text_line, text_width, text_line}, kit::Align::left,
            shown.line_warns ? warn_colour : note_colour
        );
        if (shown.progress) {
            const kit::Rect track{
                text_left, lines_top + 2 * text_line + 2, std::max(mark.x - layout_.pad - text_left, 1), layout_.bar
            };
            kit::fill_rect(canvas, track, well_colour);
            draw_sunk_edge(canvas, track);
            const double share = std::clamp(*shown.progress, 0.0, 1.0);
            const int32_t filled_width = nearest((track.width - 2) * share);
            if (filled_width > 0)
                kit::fill_rect(canvas, {track.x + 1, track.y + 1, filled_width, track.height - 2}, game_amber);
        }
        const int32_t column_top = whole.y + (whole.height - 2 * text_line) / 2;
        write(
            canvas, kit::fit(shown.version, layout_.version, measured),
            {version_left, column_top, layout_.version, text_line}, kit::Align::right, plate_text_colour
        );
        write(
            canvas, kit::fit(shown.size, layout_.version, measured),
            {version_left, column_top + text_line, layout_.version, text_line}, kit::Align::right, note_colour
        );
        draw_mark(canvas, row, mark);
        if (shown.dimmed)
            kit::blend_rect(canvas, whole, well_colour, dim_opacity);
        if (chosen)
            draw_chosen(canvas, whole);
    }

    /// Draws a row's mark, in the game's font's white and amber: a green
    /// arrow into a tray, an amber up-arrow in a ring or a check in a ring.
    ///
    /// @param canvas where it draws
    /// @param row the row, by its place
    /// @param box the mark's square
    void draw_mark(const kit::Canvas& canvas, std::size_t row, const kit::Rect& box) const {
        const LibraryMenuRow& shown = facts_now_.list.rows[row];
        const Hit hit{Part::row_mark, row};
        const bool lit = shown.mark_pressable && hovered_ == hit;
        const int32_t width = std::max(box.width / 14, 2);
        switch (shown.mark) {
        case LibraryMenuMark::none:
            return;
        case LibraryMenuMark::get:
            draw_get_tray(canvas, box, lit);
            return;
        case LibraryMenuMark::update:
            draw_circle(canvas, box, width, game_amber);
            draw_shaft_arrow(canvas, box, false, width, lit ? chosen_text_colour : game_amber);
            return;
        case LibraryMenuMark::installed:
            draw_circle(canvas, box, width, caption_colour);
            draw_check(canvas, box, width, caption_colour);
            return;
        }
    }

    /// Draws the Active tab's five tiles.
    ///
    /// @param canvas where it draws
    void draw_active(const kit::Canvas& canvas) const {
        const LibraryMenuActive& shown = facts_now_.active;
        const kit::Measure measured = measure();
        const int32_t text_line = line();
        const auto lit_tile = [&](LibraryMenuTile tile) {
            return told_tile_ == tile && hovered_.part != Part::none;
        };
        // A picture, then lines beside it, from the inside's top.
        const auto beside = [&](const kit::Canvas& on, const kit::Rect& inside, int32_t side,
                                std::string_view letters, const LibraryMenuPicture& picture,
                                bool speech, std::string_view title,
                                const std::vector<std::pair<std::string, kit::Colour>>& lines,
                                int32_t bottom) {
            const kit::Rect badge{inside.x, inside.y, side, side};
            if (speech) {
                painter_.draw_box(on, badge);
                draw_speech_mark(on, {badge.x + side / 6, badge.y + side / 6, side * 2 / 3, side * 2 / 3}, badge_ink_colour);
            } else {
                draw_badge(on, badge, letters, picture);
            }
            const int32_t left = badge.x + side + layout_.pad;
            const int32_t width = std::max(inside.x + inside.width - left, 1);
            int32_t y = inside.y;
            write(on, kit::fit(title, width, measured), {left, y, width, text_line}, kit::Align::left, caption_lit_colour);
            y += text_line;
            kit::WrapRules rules;
            rules.most_lines = 2;
            for (const auto& [text, ink] : lines) {
                for (const std::string& text_line_words : kit::wrap(text, width, measured, rules)) {
                    if (y + text_line > bottom)
                        return;
                    write(on, text_line_words, {left, y, width, text_line}, kit::Align::left, ink);
                    y += text_line;
                }
            }
        };

        // BASE GAME.
        {
            const LibraryMenuTile tile = LibraryMenuTile::base;
            draw_tile_frame(canvas, tile_rect(tile), worded("BASE GAME"), lit_tile(tile));
            const kit::Rect inside = tile_inside(tile);
            const kit::Rect button = tile_button_rect(tile, 0, 1);
            const int32_t side = std::min(inside.height, inside.width / 3);
            std::vector<std::pair<std::string, kit::Colour>> lines{{shown.base_version, plate_text_colour}};
            if (!shown.base_check.empty())
                lines.emplace_back(shown.base_check, shown.base_check_good ? note_colour : warn_colour);
            beside(canvas, inside, side, shown.base_letters, shown.base_picture, false, shown.base_name, lines, button.y);
            draw_small_button(canvas, button, {Part::change_base, 0}, worded("CHANGE…"));
        }
        // LANGUAGE.
        {
            const LibraryMenuTile tile = LibraryMenuTile::language;
            draw_tile_frame(canvas, tile_rect(tile), worded("LANGUAGE"), lit_tile(tile));
            const kit::Rect inside = tile_inside(tile);
            const kit::Rect button = tile_button_rect(tile, 0, 1);
            const int32_t side = std::min({inside.height - layout_.tile_button_height - layout_.pad, inside.width / 3, inside.height});
            beside(canvas, inside, std::max(side, 16), shown.language_letters, shown.language_picture,
                   shown.language_letters.empty() && shown.language_picture.pixels == nullptr,
                   shown.language_name, {{shown.language_line, plate_text_colour}}, button.y);
            draw_small_button(canvas, button, {Part::language, 0}, worded("SETTINGS"));
        }
        // EXPANSIONS.
        {
            const LibraryMenuTile tile = LibraryMenuTile::expansions;
            draw_tile_frame(canvas, tile_rect(tile), shown.expansions_heading, lit_tile(tile));
            const kit::Rect inside = tile_inside(tile);
            const kit::Canvas clip = kit::clipped(canvas, inside);
            int32_t y = inside.y;
            for (std::size_t pill = 0; pill < shown.expansions.size(); ++pill) {
                const kit::Rect plate = expansion_rect(pill);
                if (plate.y + plate.height > inside.y + inside.height)
                    break;
                draw_expansion(clip, pill, plate);
                y = std::max(y, plate.y + plate.height + layout_.pill_gap);
            }
            kit::WrapRules rules;
            rules.most_lines = 3;
            if (!shown.expansions_lock.empty()) {
                const int32_t lock = std::max(kit::smooth_text_points(kit::FontRole::small), 6);
                draw_padlock(clip, {inside.x, y + (text_line - lock) / 2, lock, lock}, game_amber);
                for (const std::string& words : kit::wrap(shown.expansions_lock, inside.width - lock - layout_.pad, measured, rules)) {
                    write(clip, words, {inside.x + lock + layout_.pad, y, inside.width - lock - layout_.pad, text_line},
                          kit::Align::left, warn_colour);
                    y += text_line;
                }
            }
            for (const std::string& words : kit::wrap(shown.expansions_hint, inside.width, measured, rules)) {
                write(clip, words, {inside.x, y, inside.width, text_line}, kit::Align::left, note_colour);
                y += text_line;
            }
        }
        // MOD.
        {
            const LibraryMenuTile tile = LibraryMenuTile::mod;
            draw_tile_frame(canvas, tile_rect(tile), worded("MOD"), lit_tile(tile));
            const kit::Rect inside = tile_inside(tile);
            const kit::Rect choose = tile_button_rect(tile, 1, 2);
            const int32_t side = std::max(std::min(choose.y - inside.y - layout_.pad, inside.width / 3), 16);
            std::vector<std::pair<std::string, kit::Colour>> lines{{shown.mod_line, plate_text_colour}};
            if (!shown.mod_state.empty())
                lines.emplace_back(shown.mod_state, chosen_colour);
            beside(canvas, inside, side, shown.mod_letters, shown.mod_picture, false, shown.mod_name, lines, choose.y);
            draw_small_button(canvas, choose, {Part::choose_mod, 0}, worded("CHOOSE MOD"));
            draw_small_button(canvas, tile_button_rect(tile, 0, 2), {Part::no_mod, 0}, worded("NO MOD"));
        }
        // MAPS.
        {
            const LibraryMenuTile tile = LibraryMenuTile::maps;
            draw_tile_frame(canvas, tile_rect(tile), shown.maps_heading, lit_tile(tile));
            const kit::Rect inside = tile_inside(tile);
            const std::size_t count = map_tiles_shown();
            if (shown.maps.empty())
                write(
                    canvas, kit::fit(shown.maps_empty, inside.width - layout_.browse - layout_.pad, measured),
                    {inside.x, inside.y, inside.width - layout_.browse - layout_.pad, inside.height},
                    kit::Align::left, note_colour
                );
            for (std::size_t map = 0; map < count; ++map) {
                const kit::Rect square = map_rect(map);
                painter_.draw_box(canvas, square);
                const int32_t edge = std::max(game_square(), 2);
                draw_fitted(canvas, {square.x + edge, square.y + edge, square.width - 2 * edge, square.height - 2 * edge},
                            shown.maps[map].picture);
                write(
                    canvas, kit::fit(shown.maps[map].name, square.width + layout_.tile_gap - 2, measured),
                    {square.x - layout_.tile_gap / 2, square.y + square.height, square.width + layout_.tile_gap,
                     map_name_room},
                    kit::Align::centre, note_colour
                );
            }
            draw_small_button(canvas, browse_rect(), {Part::browse_maps, 0}, worded("BROWSE MAPS"));
        }
    }

    /// Draws an expansion's row: the game's check box, lit while on, its
    /// name, faded while off, and its arrow or its padlock; the row under
    /// the pointer is washed amber.
    ///
    /// @param canvas where it draws
    /// @param at the expansion, by its place
    /// @param plate the row
    void draw_expansion(const kit::Canvas& canvas, std::size_t at, const kit::Rect& plate) const {
        const LibraryMenuExpansion& shown = facts_now_.active.expansions[at];
        const Hit hit{Part::expansion, at};
        const bool lit = enabled(hit) && hovered_ == hit;
        if (lit)
            kit::blend_rect(canvas, plate, game_amber, row_light_opacity);
        const int32_t side = std::clamp(plate.height * 3 / 4, 6, std::max(plate.height - 2, 6));
        const kit::Rect box{plate.x + layout_.pad / 2, plate.y + (plate.height - side) / 2, side, side};
        painter_.draw_check_box(canvas, box, shown.on);
        const int32_t right = plate.x + plate.width - layout_.pad / 2 - (shown.arrow != LibraryMenuExpansionArrow::none || shown.locked ? side + layout_.pad / 2 : 0);
        const int32_t left = box.x + box.width + layout_.pad;
        painter_.draw_text_at(
            canvas, {left, plate.y, right - left, plate.height}, shown.name, kit::Align::left,
            lit ? Ink::lit : shown.on ? Ink::plain : Ink::faded, game_square()
        );
        const kit::Rect end{plate.x + plate.width - layout_.pad / 2 - side, box.y, side, side};
        const int32_t width = std::max(side / 9, 2);
        const kit::Colour ink = lit ? game_amber : caption_colour;
        switch (shown.arrow) {
        case LibraryMenuExpansionArrow::none:
            if (shown.locked)
                draw_padlock(canvas, end, game_amber);
            break;
        case LibraryMenuExpansionArrow::get:
            draw_shaft_arrow(canvas, end, true, width, ink);
            kit::fill_rect(canvas, {end.x, end.y + end.height - width, end.width, width}, ink);
            break;
        case LibraryMenuExpansionArrow::cancel:
            draw_line(canvas, {end.x + 2, end.y + 2}, {end.x + end.width - 3, end.y + end.height - 3}, width, game_amber);
            draw_line(canvas, {end.x + end.width - 3, end.y + 2}, {end.x + 2, end.y + end.height - 3}, width, game_amber);
            break;
        case LibraryMenuExpansionArrow::retry:
            draw_refresh_mark(canvas, end, game_amber);
            break;
        }
        if (shown.locked || !shown.enabled)
            kit::blend_rect(canvas, plate, well_colour, dim_opacity / 2);
    }

    /// Draws the base game chooser over the Active tab: the games, each with
    /// its check box, badge, name, line and state, then the note, USE THIS
    /// GAME and CANCEL.
    ///
    /// @param canvas where it draws
    void draw_chooser(const kit::Canvas& canvas) const {
        const LibraryMenuActive& shown = facts_now_.active;
        const kit::Rect& body = layout_.body;
        const kit::Measure measured = measure();
        const int32_t text_line = line();
        draw_strip(canvas, {body.x, body.y, body.width, layout_.strip}, worded("BASE GAME"));
        write(
            canvas, kit::fit(shown.chooser_lead, body.width, measured),
            {body.x, body.y + layout_.strip + layout_.gap, body.width, text_line}, kit::Align::left,
            plate_text_colour
        );
        int32_t y = chooser_row_rect(0).y;
        for (std::size_t row = 0; row < shown.chooser_rows.size(); ++row) {
            const LibraryMenuBaseGame& base = shown.chooser_rows[row];
            const kit::Rect whole = chooser_row_rect(row);
            const Hit hit{Part::chooser_row, row};
            draw_box(canvas, whole);
            if (hovered_ == hit && !base.marked)
                kit::blend_rect(canvas, whole, game_amber, row_light_opacity);
            const int32_t radio = std::max(whole.height / 3, 10);
            const kit::Rect dot{whole.x + layout_.pad, whole.y + (whole.height - radio) / 2, radio, radio};
            painter_.draw_check_box(canvas, dot, base.marked);
            const int32_t badge = whole.height - 2 * layout_.pad;
            draw_badge(canvas, {dot.x + radio + layout_.pad, whole.y + layout_.pad, badge, badge}, base.letters, base.picture);
            const int32_t state_width = std::min(measured(base.state) + 2 * layout_.pad, whole.width / 4);
            const int32_t left = dot.x + radio + 2 * layout_.pad + badge;
            const int32_t width = std::max(whole.x + whole.width - 2 * layout_.pad - state_width - left, 1);
            write(
                canvas, kit::fit(base.name, width, measured), {left, whole.y + layout_.pad, width, text_line},
                kit::Align::left, caption_lit_colour
            );
            write(
                canvas, kit::fit(base.detail, width, measured),
                {left, whole.y + layout_.pad + text_line, width, text_line}, kit::Align::left, note_colour
            );
            if (!base.state.empty()) {
                const kit::Rect pill{
                    whole.x + whole.width - layout_.pad - state_width,
                    whole.y + (whole.height - layout_.tag_height) / 2, state_width, layout_.tag_height
                };
                painter_.draw_well(canvas, pill);
                write(canvas, kit::fit(base.state, pill.width - 4, measured), pill, kit::Align::centre,
                      base.marked ? chosen_text_colour : caption_colour);
            }
            if (base.marked)
                draw_ring(canvas, whole, std::max(game_square(), 1), game_amber);
            y = whole.y + whole.height + layout_.gap;
        }
        kit::WrapRules rules;
        rules.most_lines = 3;
        const int32_t bottom = chooser_button_rect(true).y - layout_.gap;
        for (const auto& [text, ink] :
             {std::pair<std::string, kit::Colour>{shown.chooser_note, plate_text_colour},
              std::pair<std::string, kit::Colour>{shown.chooser_problem, warn_colour}}) {
            for (const std::string& words : kit::wrap(text, body.width, measured, rules)) {
                if (y + text_line > bottom)
                    break;
                write(canvas, words, {body.x, y, body.width, text_line}, kit::Align::left, ink);
                y += text_line;
            }
        }
        draw_small_button(canvas, chooser_button_rect(true), {Part::chooser_use, 0}, worded("USE THIS GAME"));
        draw_small_button(canvas, chooser_button_rect(false), {Part::chooser_cancel, 0}, worded("CANCEL"));
    }

    /// Draws the Registries tab: a card for each registry, ADD REGISTRY…,
    /// and the note on review under them.
    ///
    /// @param canvas where it draws
    void draw_registries(const kit::Canvas& canvas) const {
        const LibraryMenuRegistries& shown = facts_now_.registries;
        const kit::Measure measured = measure();
        const int32_t text_line = line();
        const kit::Canvas inside = kit::clipped(canvas, view_);
        for (std::size_t card = 0; card < shown.cards.size(); ++card) {
            const kit::Rect whole = card_rect(card);
            if (whole.y + whole.height < view_.y || whole.y > view_.y + view_.height)
                continue;
            const LibraryMenuRegistry& item = shown.cards[card];
            const bool chosen = shown.selected && *shown.selected == card;
            const bool lit = hovered_.index == card &&
                             (hovered_.part == Part::card || hovered_.part == Part::card_off ||
                              hovered_.part == Part::card_on || hovered_.part == Part::card_refresh);
            draw_box(inside, whole);
            if (lit && !chosen)
                kit::blend_rect(inside, whole, game_amber, row_light_opacity);
            const int32_t badge = whole.height - 2 * layout_.pad;
            draw_badge(inside, {whole.x + layout_.pad, whole.y + layout_.pad, badge, badge}, item.letters, item.picture);
            const int32_t left = whole.x + 2 * layout_.pad + badge;
            const int32_t right = whole.x + whole.width - 2 * layout_.pad - layout_.switch_width;
            const int32_t width = std::max(right - left, 1);
            int32_t y = whole.y + layout_.pad;
            const int32_t pill_width = item.pill.empty() ? 0 : std::min(measured(item.pill) + 2 * layout_.pad, width / 2);
            const std::string title = kit::fit(item.title, width - pill_width - (pill_width > 0 ? layout_.pad : 0), measured);
            write(inside, title, {left, y, width, text_line}, kit::Align::left, caption_lit_colour);
            if (pill_width > 0) {
                const kit::Rect pill{
                    left + measured(title) + layout_.pad, y + (text_line - layout_.tag_height) / 2,
                    pill_width, layout_.tag_height
                };
                painter_.draw_well(inside, pill);
                write(inside, kit::fit(item.pill, pill.width - 4, measured), pill, kit::Align::centre,
                      item.pill_warns ? warn_colour : caption_colour);
            }
            y += text_line;
            write(inside, kit::fit(item.address, width, measured), {left, y, width, text_line}, kit::Align::left,
                  note_colour);
            y += text_line;
            const std::string packages = item.packages + " · ";
            const int32_t packages_width = std::min(measured(packages), width);
            write(inside, kit::fit(packages, packages_width, measured), {left, y, packages_width, text_line},
                  kit::Align::left, plate_text_colour);
            write(inside, kit::fit(item.refreshed, std::max(width - packages_width, 1), measured),
                  {left + packages_width, y, std::max(width - packages_width, 1), text_line},
                  kit::Align::left, item.refresh_warns ? warn_colour : plate_text_colour);
            // The switch: two of the game's plain buttons, the chosen one
            // held.
            for (const bool on : {false, true}) {
                const Hit hit{on ? Part::card_on : Part::card_off, card};
                painter_.draw_captioned_button(
                    inside, switch_half(card, on), worded(on ? "ON" : "OFF"), true, hovered_ == hit, item.on == on
                );
            }
            const Hit refresh_hit{Part::card_refresh, card};
            const bool usable = enabled(refresh_hit);
            const bool refresh_lit = usable && hovered_ == refresh_hit;
            painter_.draw_captioned_button(
                inside, card_refresh_rect(card), worded("REFRESH"), usable, refresh_lit || item.refreshing,
                refresh_lit && pressed_ == refresh_hit
            );
            if (chosen)
                draw_chosen(inside, whole);
        }
        // ADD REGISTRY…: the game's list box with a plus.
        const kit::Rect box = add_rect();
        if (box.y < view_.y + view_.height) {
            const bool lit = hovered_ == Hit{Part::add_registry, 0};
            painter_.draw_box(inside, box);
            const kit::Colour plus_ink = lit ? game_amber : caption_colour;
            const int32_t plus = std::max(box.height / 4, 8);
            const int32_t middle = box.x + box.width / 2;
            const int32_t plus_top = box.y + box.height / 2 - plus - 2;
            const int32_t thick = std::max(plus / 6, 2);
            kit::fill_rect(inside, {middle - plus / 2, plus_top + plus / 2 - thick / 2, plus, thick}, plus_ink);
            kit::fill_rect(inside, {middle - thick / 2, plus_top, thick, plus}, plus_ink);
            const kit::Rect words{box.x, box.y + box.height / 2, box.width, box.height / 2 - 4};
            painter_.draw_text_at(inside, words, shown.add, kit::Align::centre, lit ? Ink::lit : Ink::plain, game_square());
        }
        // The note on review, and a refusal over it.
        const kit::Rect note = note_rect();
        kit::WrapRules rules;
        rules.most_lines = 2;
        if (!shown.notice.empty())
            write(canvas, kit::fit(shown.notice, note.width, measured),
                  {note.x, note.y - layout_.strip, note.width, layout_.strip}, kit::Align::left, warn_colour);
        draw_box(canvas, note);
        const int32_t shield = std::min(note.height - 2 * layout_.pad, layout_.mark);
        draw_shield(canvas, {note.x + layout_.pad, note.y + (note.height - shield) / 2, shield, shield}, caption_colour);
        const int32_t left = note.x + 2 * layout_.pad + shield;
        const int32_t width = std::max(note.x + note.width - layout_.pad - left, 1);
        const auto lines = kit::wrap(shown.note, width, measured, rules);
        int32_t y = note.y + (note.height - static_cast<int32_t>(lines.size()) * text_line) / 2;
        for (const std::string& text : lines) {
            write(canvas, text, {left, y, width, text_line}, kit::Align::left, plate_text_colour);
            y += text_line;
        }
    }

    /// Draws the scroll bar: its arrows, its well and its thumb, the game's.
    ///
    /// @param canvas where it draws
    void draw_scroll_bar(const kit::Canvas& canvas) const {
        const auto held = [this](bool up) {
            const Hit hit{up ? Part::scroll_up : Part::scroll_down, 0};
            return hovered_ == hit && pressed_ == hit;
        };
        painter_.draw_scroll_bar(
            canvas, scroll_bar_, scroll_arrow_rect(true), scroll_arrow_rect(false), well_rect(), thumb_rect(),
            held(true), held(false)
        );
    }

    /// Takes a key while a question shows: Escape and N answer its cancel
    /// button, Y its primary one, Enter and Space the button marked, and
    /// Left, Right, Tab and Shift+Tab move the mark.
    ///
    /// @param pressed the key
    /// @param sdl_key the key's SDL keycode
    /// @return close when the answer closed the Library; redraw when it acted; none otherwise
    LayerAnswer question_key(kit::Key pressed, uint32_t sdl_key) {
        const LibraryMenuQuestion& asked = *facts_now_.question;
        const std::size_t count = asked.buttons.size();
        if (count == 0)
            return LayerAnswer::none;
        const auto answer = [this](std::size_t button) {
            return press({Part::question_button, button}) ? LayerAnswer::close : LayerAnswer::redraw;
        };
        if (pressed == kit::Key::escape || sdl_key == SDLK_N)
            return answer(asked.cancel);
        if (sdl_key == SDLK_Y)
            return answer(asked.primary);
        switch (pressed) {
        case kit::Key::enter:
        case kit::Key::space:
            return answer(std::min(question_marked_, count - 1));
        case kit::Key::left:
        case kit::Key::back_tab:
            question_marked_ = (std::min(question_marked_, count - 1) + count - 1) % count;
            ++revision_;
            return LayerAnswer::redraw;
        case kit::Key::right:
        case kit::Key::tab:
            question_marked_ = (std::min(question_marked_, count - 1) + 1) % count;
            ++revision_;
            return LayerAnswer::redraw;
        default:
            return LayerAnswer::none;
        }
    }

    /// Draws the question over everything: the screen shaded, and a panel
    /// in its middle, framed and labelled as the others, its paragraphs
    /// under the label as DETAILS' text, and its buttons, the big buttons
    /// of the screen's foot, in a row at its foot, right-aligned, an amber
    /// ring round the one marked. It keeps where the buttons were drawn for
    /// the presses.
    ///
    /// @param canvas where it draws
    void draw_question(const kit::Canvas& canvas) const {
        const LibraryMenuQuestion& asked = *facts_now_.question;
        kit::blend_rect(canvas, {0, 0, size_.x, size_.y}, well_colour, question_dim_opacity);
        // Its text stands as far within its frame as DETAILS' does.
        const int32_t side = layout_.about.x - layout_.details.x;
        const int32_t top = layout_.about.y - layout_.details.y;
        const kit::Rect stage = game_menu_stage(size_.x, size_.y);
        const auto count = static_cast<int32_t>(asked.buttons.size());
        const int32_t buttons_width =
            count * layout_.left_width + std::max(count - 1, 0) * layout_.footer_gap;
        const int32_t width = std::min(
            std::max({stage.width * 9 / 20, buttons_width + 2 * side, 1}), std::max(size_.x - 2 * side, 1)
        );
        const int32_t text_width = std::max(width - 2 * side, 1);
        const int32_t text_line = line();
        kit::WrapRules rules;
        rules.most_lines = most_question_lines;
        rules.newlines = true;
        std::vector<std::vector<std::string>> paragraphs;
        int32_t text_height = 0;
        for (const std::string& paragraph : asked.paragraphs) {
            if (!paragraphs.empty())
                text_height += layout_.gap;
            paragraphs.push_back(kit::wrap(paragraph, text_width, measure(), rules));
            text_height += static_cast<int32_t>(paragraphs.back().size()) * text_line;
        }
        const int32_t height = top + text_height + 2 * layout_.gap + layout_.button_height + side;
        const kit::Rect panel{(size_.x - width) / 2, (size_.y - height) / 2, width, height};
        draw_frame(canvas, panel);
        draw_label(canvas, panel, asked.title);
        int32_t y = panel.y + top;
        for (std::size_t at = 0; at < paragraphs.size(); ++at) {
            if (at > 0)
                y += layout_.gap;
            for (const std::string& text : paragraphs[at]) {
                write(canvas, text, {panel.x + side, y, text_width, text_line}, kit::Align::left, plate_text_colour);
                y += text_line;
            }
        }
        question_buttons_.clear();
        int32_t x = panel.x + panel.width - side - buttons_width;
        const int32_t buttons_y = panel.y + panel.height - side - layout_.button_height;
        for (std::size_t button = 0; button < asked.buttons.size(); ++button) {
            const kit::Rect rect{x, buttons_y, layout_.left_width, layout_.button_height};
            question_buttons_.push_back(rect);
            const Hit hit{Part::question_button, button};
            draw_footer(canvas, hit, rect, asked.buttons[button].caption, enabled(hit));
            if (button == question_marked_)
                draw_ring(canvas, rect, std::max(game_square(), 1), game_amber);
            x += layout_.left_width + layout_.footer_gap;
        }
    }

    /// Draws the line a switch of the mod shows while the game starts again
    /// over everything: the screen shaded, and the line in a panel's frame
    /// in its middle, at most three lines of it, in the game's font as large
    /// as the panels' labels.
    ///
    /// @param canvas where it draws
    void draw_switching(const kit::Canvas& canvas) const {
        kit::blend_rect(canvas, {0, 0, size_.x, size_.y}, well_colour, switching_dim_opacity);
        const int32_t square = painter_.text_square(layout_.label_height);
        const kit::Measure measured = painter_.measure(square);
        const int32_t line_height = painter_.line_height(square);
        // As wide as the line, between a quarter and a half of the stage.
        const kit::Rect stage = game_menu_stage(size_.x, size_.y);
        const int32_t margin = layout_.rim + layout_.inset + layout_.pad;
        kit::WrapRules rules;
        rules.most_lines = 3;
        const auto lines =
            kit::wrap(facts_now_.switching, std::max(stage.width / 2 - 2 * margin, 1), measured, rules);
        int32_t widest = stage.width / 4;
        for (const std::string& text : lines)
            widest = std::max(widest, measured(text));
        const int32_t width = widest + 2 * margin;
        const int32_t height = static_cast<int32_t>(lines.size()) * line_height + 2 * margin;
        const kit::Rect panel{(size_.x - width) / 2, (size_.y - height) / 2, width, height};
        draw_frame(canvas, panel);
        int32_t y = panel.y + margin;
        for (const std::string& text : lines) {
            const kit::Rect box{panel.x + margin, y, panel.width - 2 * margin, line_height};
            painter_.draw_text_at(canvas, box, text, kit::Align::centre, Ink::plain, square);
            y += line_height;
        }
    }

    /// Draws TAGS' open list over everything: every tag, then the item that
    /// shows them all; in the game's list box.
    ///
    /// @param canvas where it draws
    void draw_tags_menu(const kit::Canvas& canvas) const {
        const kit::Rect menu = tags_menu_rect();
        painter_.draw_box(canvas, menu);
        const kit::Measure measured = measure();
        const std::size_t count = facts_now_.list.tags.size() + 1;
        for (std::size_t item = static_cast<std::size_t>(tags_first_);
             item < count && item < static_cast<std::size_t>(tags_first_) + most_menu_items; ++item) {
            const kit::Rect rect = tags_item_rect(item);
            const Hit hit{Part::tag_item, item};
            const bool chosen = item < facts_now_.list.tags.size() && facts_now_.list.tags[item].chosen;
            if (hovered_ == hit)
                kit::blend_rect(canvas, rect, game_amber, row_light_opacity * 2);
            const std::string caption = item < facts_now_.list.tags.size() ? facts_now_.list.tags[item].caption
                                                                            : worded("Every tag");
            write(
                canvas, kit::fit(caption, rect.width - 2 * layout_.pad, measured),
                {rect.x + layout_.pad, rect.y, rect.width - 2 * layout_.pad, rect.height}, kit::Align::left,
                chosen ? chosen_text_colour : hovered_ == hit ? caption_lit_colour : caption_colour
            );
        }
    }

    /// Draws the DETAILS panel: the badge and title, the status, the summary,
    /// the tags, the facts, a section, the note and the buttons; with nothing
    /// to tell, how to choose something.
    ///
    /// @param canvas where it draws
    void draw_details(const kit::Canvas& canvas) const {
        draw_frame(canvas, layout_.details);
        draw_label(canvas, layout_.details, worded("DETAILS"));
        details_tags_.clear();
        const kit::Rect& about = layout_.about;
        const kit::Measure measured = measure();
        const int32_t text_line = line();
        const LibraryMenuDetails* told = told_details();
        // The Active tab's note stands in a box at the foot.
        int32_t bottom = told != nullptr && !told->buttons.empty() ? details_buttons_top() - layout_.gap
                                                                    : about.y + about.height;
        if (facts_now_.page == LibraryMenuPage::active && !facts_now_.active.note.empty()) {
            kit::WrapRules rules;
            rules.most_lines = 3;
            const auto lines = kit::wrap(facts_now_.active.note, about.width - 2 * layout_.pad, measured, rules);
            const int32_t height = static_cast<int32_t>(lines.size()) * text_line + 2 * layout_.pad;
            const kit::Rect box{about.x, about.y + about.height - height, about.width, height};
            draw_box(canvas, box);
            int32_t y = box.y + layout_.pad;
            for (const std::string& text : lines) {
                write(canvas, text,
                      {box.x + layout_.pad, y, box.width - 2 * layout_.pad, text_line},
                      kit::Align::left, plate_text_colour);
                y += text_line;
            }
            bottom = box.y - layout_.gap;
        }
        if (!facts_now_.notice.empty()) {
            kit::WrapRules rules;
            rules.most_lines = 3;
            const auto lines = kit::wrap(facts_now_.notice, about.width, measured, rules);
            bottom -= static_cast<int32_t>(lines.size()) * text_line;
            int32_t y = bottom;
            for (const std::string& text : lines) {
                write(canvas, text, {about.x, y, about.width, text_line},
                      kit::Align::left, warn_colour);
                y += text_line;
            }
            bottom -= layout_.gap;
        }
        if (told == nullptr) {
            std::string hint;
            switch (facts_now_.page) {
            case LibraryMenuPage::list:
                hint = facts_now_.list.rows.empty() ? std::string() : worded("Choose a package to read about it.");
                break;
            case LibraryMenuPage::registries:
                hint = worded("Choose a registry to read about it.");
                break;
            case LibraryMenuPage::active:
                hint = facts_now_.active.chooser_open
                           ? facts_now_.active.chooser_lead +
                                 (facts_now_.active.chooser_note.empty()
                                      ? std::string()
                                      : " " + facts_now_.active.chooser_note)
                           : worded("Point at a tile to read about it.");
                break;
            }
            kit::WrapRules rules;
            int32_t y = about.y;
            for (const std::string& text : kit::wrap(hint, about.width, measured, rules)) {
                write(canvas, text, {about.x, y, about.width, text_line},
                      kit::Align::left, plate_text_colour);
                y += text_line;
            }
            return;
        }
        const kit::Canvas clip = kit::clipped(canvas, {about.x, about.y, about.width, std::max(bottom - about.y, 1)});
        int32_t y = about.y;
        // The badge and the title beside it.
        int32_t text_left = about.x;
        int32_t head = 0;
        if (told->badge) {
            const int32_t side = layout_.details_badge;
            draw_badge(clip, {about.x, about.y, side, side}, told->letters, told->picture);
            text_left = about.x + side + layout_.pad;
            head = side;
        }
        const int32_t text_width = std::max(about.x + about.width - text_left, 1);
        kit::WrapRules title_rules;
        title_rules.most_lines = 2;
        int32_t text_y = about.y;
        for (const std::string& text : kit::wrap(told->title, text_width, measured, title_rules)) {
            write(clip, text, {text_left, text_y, text_width, text_line},
                  kit::Align::left, caption_lit_colour);
            text_y += text_line;
        }
        kit::WrapRules status_rules;
        status_rules.most_lines = 2;
        for (const std::string& text : kit::wrap(told->status, text_width, measured, status_rules)) {
            write(clip, text, {text_left, text_y, text_width, text_line},
                  kit::Align::left, told->status_warns ? warn_colour : chosen_text_colour);
            text_y += text_line;
        }
        y = std::max(about.y + head, text_y) + layout_.gap;
        // The summary.
        kit::WrapRules summary_rules;
        summary_rules.most_lines = most_summary_lines;
        summary_rules.newlines = true;
        for (const std::string& text : kit::wrap(told->summary, about.width, measured, summary_rules)) {
            write(clip, text, {about.x, y, about.width, text_line},
                  kit::Align::left, plate_text_colour);
            y += text_line;
        }
        // The tags, as plates that wrap: the game's plain buttons.
        if (!told->tags.empty()) {
            y += layout_.gap / 2;
            int32_t x = about.x;
            for (std::size_t tag = 0; tag < told->tags.size(); ++tag) {
                const int32_t width = std::min(measured(told->tags[tag]) + 2 * layout_.pad, about.width);
                if (x > about.x && x + width > about.x + about.width) {
                    x = about.x;
                    y += layout_.tag_height + layout_.button_gap;
                }
                const kit::Rect plate{x, y, width, layout_.tag_height};
                details_tags_.push_back(plate);
                const Hit hit{Part::details_tag, tag};
                painter_.draw_plain_button(clip, plate, hovered_ == hit && pressed_ == hit ? 1 : 0);
                painter_.draw_text_at(clip, plate, told->tags[tag], kit::Align::centre,
                                      hovered_ == hit ? Ink::lit : Ink::plain, game_square(),
                                      TextEdge::smoothed);
                x += width + layout_.button_gap;
            }
            y += layout_.tag_height + layout_.gap;
        }
        // The facts: a label, a bar, the value.
        if (!told->facts.empty()) {
            // The facts stand under the frame's tube, across the panel from
            // its left tube to its right.
            const int32_t tube = std::max(nearest(painter_.picture_scale() * painter_.art().rule.height), 1);
            painter_.draw_rule(canvas, layout_.details, y + tube / 2);
            y += tube + layout_.gap;
            const int32_t label_width = about.width * 36 / 100;
            const int32_t value_left = about.x + label_width + layout_.pad;
            const int32_t value_width = std::max(about.x + about.width - value_left, 1);
            kit::WrapRules value_rules;
            value_rules.most_lines = most_fact_lines;
            for (const LibraryMenuFact& fact : told->facts) {
                const auto lines = kit::wrap(fact.value, value_width, measured, value_rules);
                const int32_t height = std::max(static_cast<int32_t>(lines.size()), 1) * text_line;
                if (y + text_line > bottom)
                    break;
                write(clip, kit::fit(fact.label, label_width, measured),
                      {about.x, y, label_width, text_line}, kit::Align::left, note_colour);
                int32_t line_y = y;
                for (const std::string& text : lines) {
                    write(clip, text, {value_left, line_y, value_width, text_line},
                          kit::Align::left, fact.warns ? warn_colour : caption_colour);
                    line_y += text_line;
                }
                y += height + 2;
            }
        }
        // A section under its strip, such as the signing keys.
        if (!told->section.empty()) {
            y += layout_.gap;
            draw_strip(clip, {about.x, y, about.width, layout_.strip}, told->section);
            y += layout_.strip + layout_.gap / 2;
            if (!told->section_lines.empty()) {
                const int32_t height = static_cast<int32_t>(told->section_lines.size()) * text_line + layout_.pad;
                const kit::Rect box{about.x, y, about.width, height};
                draw_box(clip, box);
                int32_t line_y = y + layout_.pad / 2;
                for (const std::string& text : told->section_lines) {
                    write(clip, kit::fit(text, box.width - 2 * layout_.pad, measured),
                          {box.x + layout_.pad, line_y, box.width - 2 * layout_.pad, text_line},
                          kit::Align::left, caption_colour);
                    line_y += text_line;
                }
                y += height + layout_.gap / 2;
            }
            if (!told->section_note.empty()) {
                write(clip, kit::fit(told->section_note, about.width, measured),
                      {about.x, y, about.width, text_line}, kit::Align::centre, note_colour);
                y += text_line;
            }
        }
        // The note over the buttons.
        if (!told->note.empty()) {
            kit::WrapRules rules;
            rules.most_lines = 4;
            const auto lines = kit::wrap(told->note, about.width, measured, rules);
            int32_t line_y = bottom - static_cast<int32_t>(lines.size()) * text_line;
            line_y = std::max(line_y, y + layout_.gap);
            for (const std::string& text : lines) {
                write(canvas, text, {about.x, line_y, about.width, text_line},
                      kit::Align::left, warn_colour);
                line_y += text_line;
            }
        }
        for (std::size_t button = 0; button < told->buttons.size(); ++button)
            draw_small_button(canvas, details_button_rect(button), {Part::details_button, button},
                              told->buttons[button].caption);
    }

    /// Draws one of the big buttons: the game's big button on its plate,
    /// as the Options screen's.
    ///
    /// @param canvas where it draws
    /// @param hit its part
    /// @param rect the button
    /// @param caption its words
    /// @param usable it takes a press
    void draw_footer(
        const kit::Canvas& canvas, Hit hit, const kit::Rect& rect, std::string_view caption, bool usable
    ) const {
        const bool lit = usable && hovered_ == hit;
        painter_.draw_big_button(canvas, rect, caption, usable, lit, lit && pressed_ == hit);
    }

    std::string name_{};                         ///< its name on the OA layer
    std::function<LibraryMenuFacts()> facts_{};  ///< reads what the Library shows
    LibraryMenuActions actions_{};               ///< what its presses do
    LibraryMenuFacts facts_now_{};               ///< what it shows, as last read
    kit::Point size_{};                          ///< the screen's size, in points
    LibraryLayout layout_{};                     ///< where its fixed parts lie
    bool laid_out_{};                            ///< layout_ is for size_
    kit::Rect view_{};                           ///< where the rows or the cards scroll
    kit::Rect scroll_bar_{};                     ///< right of them, its arrows included
    int32_t content_height_{};                   ///< the rows' or the cards' height together
    int32_t scroll_{};                           ///< how far they are scrolled, in points
    Hit hovered_{};                              ///< the part under the pointer
    Hit pressed_{};                              ///< the part a press holds
    bool dragging_thumb_{};                      ///< a press drags the scroll bar's thumb
    int32_t thumb_grab_{};                       ///< where the press took the thumb, from its top
    bool search_focused_{};                      ///< typed text goes to the search
    bool tags_open_{};                           ///< TAGS' list is open
    int32_t tags_first_{};                       ///< its first item shown
    LibraryMenuTile told_tile_{LibraryMenuTile::mod}; ///< the Active tile DETAILS tells of
    mutable std::vector<kit::Rect> details_tags_{};   ///< the details' tags as last drawn
    mutable std::vector<kit::Rect> question_buttons_{}; ///< the question's buttons as last drawn
    std::size_t question_marked_{};              ///< the question's button Enter answers
    std::optional<LibraryMenuView> told_view_{};      ///< what the host was last told is in view
    bool closed_{};                              ///< the host was told the Library left
    GameMenuPainter painter_;                    ///< draws with the game's art
    uint64_t revision_{1};                       ///< counts the changes to what it draws
};

} // namespace

std::unique_ptr<LayerScreen> make_library_menu(
    std::string_view name,
    std::function<LibraryMenuFacts()> facts,
    LibraryMenuActions actions,
    GameMenuArt game_art
) {
    return std::make_unique<LibraryMenuScreen>(name, std::move(facts), std::move(actions), std::move(game_art));
}

} // namespace oa::app
