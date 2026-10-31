// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The map browser's layout at Compact, Regular and Large, made only of the
// UI kit's components: the window's header and footer, the filter bar, the
// grid of cards and the selected map's details, every control named for
// automation. It reads the model and decides none of its rules.
#include "screen_controls.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/map_browser/model.hpp"
#include "oa/ui/map_browser/screen.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <stdint.h>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::ui::map_browser {

namespace controls {

std::string word(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

std::string filled(
    std::string_view english, const std::vector<std::pair<std::string_view, std::string>>& values
) {
    const std::string pattern = word(english);
    std::string text;
    std::size_t at = 0;
    while (at < pattern.size()) {
        const std::size_t open = pattern.find('{', at);
        if (open == std::string::npos) {
            text.append(pattern, at, std::string::npos);
            break;
        }
        const std::size_t closing = pattern.find('}', open);
        if (closing == std::string::npos) {
            text.append(pattern, at, std::string::npos);
            break;
        }
        text.append(pattern, at, open - at);
        const std::string_view name(pattern.data() + open + 1, closing - open - 1);
        const auto found = std::find_if(values.begin(), values.end(), [&](const auto& value) {
            return value.first == name;
        });
        if (found != values.end())
            text += found->second;
        else
            text.append(pattern, open, closing - open + 1);
        at = closing + 1;
    }
    return text;
}

} // namespace controls

namespace {

namespace ids = controls;

// The browser's own sizes; every other size is a kit metric.

/// The narrowest the search field may be and still share one line with the chips.
constexpr int32_t least_search_width = 120;
/// The widest the search field grows.
constexpr int32_t most_search_width = 240;
/// A full progress bar, in the bar's own units.
constexpr int32_t progress_whole = 1000;
/// Bytes in a kilobyte and a megabyte, as the download size is shown.
constexpr uint64_t kilobyte = 1024;
/// Bytes in a megabyte.
constexpr uint64_t megabyte = kilobyte * 1024;

// The words, in English as the interface catalogue keys them.

constexpr std::string_view word_title = "OPEN ANNIHILATION";
constexpr std::string_view word_title_second = "MAPS";
constexpr std::string_view word_skirmish_mod = "Skirmish · playing {mod}";
constexpr std::string_view word_skirmish_game = "Skirmish · No Mod";
constexpr std::string_view word_battle_room_mod = "Battle room · playing {mod}";
constexpr std::string_view word_battle_room_game = "Battle room · No Mod";
constexpr std::array<std::string_view, 4> word_sources{"All", "Game", "Mod", "Downloads"};
constexpr std::string_view word_filter_count = "{filter} ({count})";
constexpr std::string_view word_players_heading = "PLAYERS";
constexpr std::string_view word_size_heading = "SIZE";
constexpr std::string_view word_any = "Any";
constexpr std::array<std::string_view, 4> word_players{"2", "3", "4", "5–10"};
constexpr std::array<std::string_view, 3> word_sizes{"S", "M", "L"};
constexpr std::string_view word_search = "Search maps";
constexpr std::string_view word_loading = "Loading preview";
constexpr std::string_view word_no_preview = "No preview";
constexpr std::string_view word_downloaded = "Downloaded";
constexpr std::string_view word_available = "Available";
constexpr std::string_view word_ta = "TA 3.1c";
constexpr std::string_view word_close = "Close";
constexpr std::string_view word_does_not_fit_chip = "Doesn't fit";
constexpr std::string_view word_card_line = "{players} · {size}";
constexpr std::string_view word_players_one = "1 player";
constexpr std::string_view word_players_many = "{count} players";
constexpr std::string_view word_megabytes = "{size} MB";
constexpr std::string_view word_kilobytes = "{size} KB";
constexpr std::string_view word_game_short = "No Mod";
constexpr std::string_view word_fits = "Fits {game} ✓";
constexpr std::string_view word_does_not_fit = "Doesn't fit {game}";
constexpr std::string_view word_fit_later = "Fit with {game} is checked when it's installed";
constexpr std::string_view word_cancel = "Cancel";
constexpr std::string_view word_select = "Select";
constexpr std::string_view word_download_select = "Download and select";
constexpr std::string_view word_downloading = "Downloading…";
constexpr std::string_view word_none_selected = "Choose a map";
constexpr std::string_view word_no_maps = "No map matches these filters";

/// The automation names of the source pills, in their order.
constexpr std::array<std::string_view, 4> source_names{
    name_filter_all, name_filter_game, name_filter_mod, name_filter_downloads
};

/// The window's rows and columns at a size class.
struct Window {
    kit::Point size{};       ///< the window's width and height, in points
    int32_t padding{};       ///< Kit 2's window padding, as Settings keeps it
    int32_t left{};          ///< the content's left column
    int32_t right{};         ///< the column after the content's last
    int32_t header_height{}; ///< the header's band: Kit 2's window header, Compact's at Compact
    int32_t body_top{};      ///< the first row under the header's rule
    int32_t footer_rule{};   ///< the row of the line over the footer
    int32_t footer_top{};    ///< the footer's first row
    int32_t footer_height{}; ///< the footer's rows under its line: Kit 2's window footer
    /// The footer's buttons' top row: in the footer's middle, or at Compact
    /// a space under its line, so the byline's line fits under them.
    int32_t button_top{};
    int32_t control_height{}; ///< Kit 2's control height: the buttons, the strips and the search
    int32_t gap{};            ///< the rows between stacked parts
    bool compact{};           ///< the class is Compact
    const kit::Metrics* metrics{}; ///< the class's metrics
};

/// Returns the window's rows and columns for a frame, in the sizes Settings
/// gives its window: Kit 2's window header at Regular and Large and
/// Compact's band at Compact, Kit 2's window padding, and Kit 2's window
/// footer with its controls. Compact's buttons sit at the footer's top, the
/// title beside them and the byline and the fit chips on the line under them.
///
/// @param frame the frame
/// @return the window
Window window_of(const kit::Frame& frame) {
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    Window window;
    window.metrics = &metrics;
    window.size = window_size(frame);
    window.compact = frame.size_class == kit::SizeClass::compact;
    window.padding = metrics.window_padding;
    window.left = metrics.edge + window.padding;
    window.right = window.size.x - metrics.edge - window.padding;
    window.header_height = window.compact ? metrics.header_height : metrics.window_header_height;
    window.body_top = metrics.edge + window.header_height + 1;
    window.control_height = metrics.control_height;
    window.footer_height = metrics.window_footer_height;
    window.footer_rule = window.size.y - metrics.edge - window.footer_height - 1;
    window.footer_top = window.footer_rule + 1;
    window.button_top =
        window.compact ? window.footer_top + metrics.space
                       : window.footer_top + (window.footer_height - window.control_height) / 2;
    window.gap = window.compact ? metrics.space : metrics.row_gap;
    return window;
}

/// Where the filter bar's parts lie.
struct Bar {
    std::array<kit::Rect, 4> sources{}; ///< the source pills
    kit::Rect players{};                ///< the players strip
    kit::Rect sizes{};                  ///< the size strip
    int32_t players_level_width{};      ///< one players segment, inside the border
    int32_t sizes_level_width{};        ///< one size segment, inside the border
    kit::Rect players_heading{};        ///< PLAYERS
    kit::Rect sizes_heading{};          ///< SIZE
    kit::Rect search{};                 ///< the search field
    int32_t bottom{};                   ///< the row after the bar
};

/// Where the grid lies and how far it scrolls.
struct Grid {
    kit::Rect view{};               ///< what the cards are seen through
    kit::Rect well{};               ///< the scroll bar's well, beside the cards
    std::size_t columns{};          ///< the columns
    std::vector<kit::Rect> cells{}; ///< each shown card's cell, unscrolled, in grid order
    int32_t content_height{};       ///< the cells' height, from the view's top
    int32_t limit{};                ///< the greatest offset
    int32_t offset{};               ///< the offset, held to 0 to the limit
};

/// Where everything lies.
struct Plan {
    Window window{}; ///< the window's rows and columns
    Bar bar{};       ///< the filter bar
    Grid grid{};     ///< the grid
};

/// Returns one source pill's caption: the filter's word and its count.
///
/// @param english the filter's word, in English
/// @param count how many cards it would show
/// @return the caption, looked up
std::string source_caption(std::string_view english, std::size_t count) {
    return ids::filled(
        word_filter_count, {{"filter", ids::word(english)}, {"count", std::to_string(count)}}
    );
}

/// Returns the four source captions, in the pills' order.
///
/// @param browser the browser
/// @return All, Game, Mod and Downloads, each with its count
std::array<std::string, 4> source_captions(const Browser& browser) {
    const SourceCounts counts = source_counts(browser);
    const std::array<std::size_t, 4> numbers{counts.all, counts.game, counts.mod, counts.downloads};
    std::array<std::string, 4> captions;
    for (std::size_t index = 0; index < captions.size(); ++index)
        captions[index] = source_caption(word_sources[index], numbers[index]);
    return captions;
}

/// Places Kit 2's filter chips along a row from a column, chip_gap apart,
/// each centred down it, as the key editor places its filter pills: the
/// class's filter chip height, each pill widened by the height it gains
/// over the kit's pill so its words stay clear of the rounder ends.
///
/// @param fonts the fonts
/// @param metrics the class's metrics
/// @param captions the pills' captions, already looked up
/// @param at the first pill's left column
/// @param row the row's top
/// @param row_height the row's height
/// @param[out] rects each pill's rectangle
/// @return the column after the last pill
int32_t place_pills(
    const kit::Fonts& fonts,
    const kit::Metrics& metrics,
    const std::array<std::string, 4>& captions,
    int32_t at,
    int32_t row,
    int32_t row_height,
    std::array<kit::Rect, 4>& rects
) {
    const int32_t height = metrics.filter_chip_height;
    const int32_t taller = std::max(height - kit::pill_height(), int32_t{0});
    int32_t x = at;
    for (std::size_t index = 0; index < captions.size(); ++index) {
        const int32_t width = kit::pill_width(fonts, kit::PillLook{captions[index]}) + taller;
        rects[index] = {x, row + (row_height - height) / 2, width, height};
        x += width + (index + 1 < captions.size() ? metrics.chip_gap : 0);
    }
    return x;
}

/// Returns the widest of a strip's captions in the small font.
///
/// @param fonts the fonts
/// @param captions the segments' words, already looked up
/// @return the width, in points
int32_t widest_caption(const kit::Fonts& fonts, const std::vector<std::string>& captions) {
    int32_t widest = 0;
    for (const std::string& caption : captions)
        widest = std::max(widest, kit::text_width(fonts, kit::FontRole::small, caption));
    return widest;
}

/// Returns how wide one segment of a strip is: the widest caption, and the
/// room a Settings button keeps round its words, two spaces either side.
///
/// @param fonts the fonts
/// @param metrics the class's metrics
/// @param captions the segments' words, already looked up
/// @return the width, in points
int32_t segment_width(
    const kit::Fonts& fonts, const kit::Metrics& metrics, const std::vector<std::string>& captions
) {
    return widest_caption(fonts, captions) + 4 * metrics.space;
}

/// Returns how wide one segment of a strip is where the room is short: the
/// widest caption and the padding a chip keeps either side of its word.
///
/// @param fonts the fonts
/// @param metrics the class's metrics
/// @param captions the segments' words, already looked up
/// @return the width, in points
int32_t narrow_segment_width(
    const kit::Fonts& fonts, const kit::Metrics& metrics, const std::vector<std::string>& captions
) {
    return widest_caption(fonts, captions) + 2 * metrics.chip_padding;
}

/// Returns a strip's rectangle: one border either side, then one segment each.
///
/// @param at the strip's left column
/// @param row the row's top
/// @param row_height the row's height; the strip fills it
/// @param segments how many segments
/// @param level_width one segment's columns, inside the border
/// @return the strip
kit::Rect
strip_rect(int32_t at, int32_t row, int32_t row_height, std::size_t segments, int32_t level_width) {
    const int32_t width = 2 + level_width * static_cast<int32_t>(segments);
    return {at, row, width, row_height};
}

/// Returns a heading's rectangle at a column, centred down a row.
///
/// @param fonts the fonts
/// @param english the heading's word, in English
/// @param at its left column
/// @param row the row's top
/// @param row_height the row's height
/// @return the rectangle
kit::Rect heading_rect(
    const kit::Fonts& fonts, std::string_view english, int32_t at, int32_t row, int32_t row_height
) {
    const kit::Metrics& metrics = kit::compact_metrics;
    const int32_t width = kit::tracked_width(
        fonts, kit::FontRole::small, ids::word(english), metrics.heading_tracking
    );
    return {at, row + (row_height - metrics.small_line) / 2, width, metrics.small_line};
}

/// Returns the players strip's captions, already looked up: Any, then 2, 3,
/// 4 and 5–10.
///
/// @return the captions
std::vector<std::string> player_captions() {
    std::vector<std::string> captions;
    captions.reserve(ids::player_levels.size());
    captions.push_back(ids::word(word_any));
    for (const std::string_view english : word_players)
        captions.push_back(ids::word(english));
    return captions;
}

/// Returns the size strip's captions, already looked up: Any, then S, M and L.
///
/// @return the captions
std::vector<std::string> size_captions() {
    std::vector<std::string> captions;
    captions.reserve(ids::size_levels.size());
    captions.push_back(ids::word(word_any));
    for (const std::string_view english : word_sizes)
        captions.push_back(ids::word(english));
    return captions;
}

/// Places the players and size groups along a row from a column: each
/// heading, then its strip, the groups a padding apart. A strip is as tall
/// as the row, Kit 2's control height as Settings' strips are, so it stays
/// inside the bar. Its segments keep a button's room round their words, or
/// a chip's where that would run the groups past the content's right.
///
/// @param fonts the fonts
/// @param window the window
/// @param at the first heading's left column
/// @param row the row's top
/// @param row_height the row's height
/// @param[in,out] bar receives the places
/// @return the column after the size strip
int32_t place_groups(
    const kit::Fonts& fonts,
    const Window& window,
    int32_t at,
    int32_t row,
    int32_t row_height,
    Bar& bar
) {
    const kit::Metrics& metrics = *window.metrics;
    const std::vector<std::string> players = player_captions();
    const std::vector<std::string> sizes = size_captions();
    const auto strip_width = [](std::size_t segments, int32_t level_width) {
        return 2 + level_width * static_cast<int32_t>(segments);
    };
    const int32_t headings = heading_rect(fonts, word_players_heading, 0, 0, 0).width +
                             heading_rect(fonts, word_size_heading, 0, 0, 0).width +
                             2 * metrics.label_gap + window.padding;
    bar.players_level_width = segment_width(fonts, metrics, players);
    bar.sizes_level_width = segment_width(fonts, metrics, sizes);
    if (at + headings + strip_width(players.size(), bar.players_level_width) +
            strip_width(sizes.size(), bar.sizes_level_width) >
        window.right) {
        bar.players_level_width = narrow_segment_width(fonts, metrics, players);
        bar.sizes_level_width = narrow_segment_width(fonts, metrics, sizes);
    }
    bar.players_heading = heading_rect(fonts, word_players_heading, at, row, row_height);
    int32_t x = bar.players_heading.x + bar.players_heading.width + metrics.label_gap;
    bar.players = strip_rect(x, row, row_height, players.size(), bar.players_level_width);
    x = bar.players.x + bar.players.width;
    bar.sizes_heading = heading_rect(fonts, word_size_heading, x + window.padding, row, row_height);
    x = bar.sizes_heading.x + bar.sizes_heading.width + metrics.label_gap;
    bar.sizes = strip_rect(x, row, row_height, sizes.size(), bar.sizes_level_width);
    return bar.sizes.x + bar.sizes.width;
}

/// Places the filter bar, a row of Kit 2's control height: on one line when
/// the search keeps least_search_width there, which Compact never tries;
/// else the sources and the search, then the players and the sizes. The
/// search sits at the right, at most most_search_width wide, as the
/// Library's does.
///
/// @param fonts the fonts
/// @param window the window
/// @param browser the browser, for the pills' counts
/// @return the places
Bar bar_of(const kit::Fonts& fonts, const Window& window, const Browser& browser) {
    const kit::Metrics& metrics = *window.metrics;
    const int32_t row_height = window.control_height;
    const int32_t first_row = window.body_top + window.gap;
    Bar bar;
    const std::array<std::string, 4> captions = source_captions(browser);
    const int32_t after_sources =
        place_pills(fonts, metrics, captions, window.left, first_row, row_height, bar.sources);
    if (!window.compact) {
        Bar one = bar;
        const int32_t after_sizes =
            place_groups(fonts, window, after_sources + window.padding, first_row, row_height, one);
        const int32_t room = window.right - (after_sizes + window.padding);
        if (room >= least_search_width) {
            const int32_t width = std::min(room, most_search_width);
            one.search = {window.right - width, first_row, width, row_height};
            one.bottom = first_row + row_height;
            return one;
        }
    }
    const int32_t search_left = after_sources + window.padding;
    const int32_t search_width =
        std::min(std::max(window.right - search_left, int32_t{0}), most_search_width);
    bar.search = {window.right - search_width, first_row, search_width, row_height};
    const int32_t second_row = first_row + row_height + window.gap;
    std::ignore = place_groups(fonts, window, window.left, second_row, row_height, bar);
    bar.bottom = second_row + row_height;
    return bar;
}

/// Places everything for a browser's state at a frame.
///
/// @param browser the browser
/// @param state the screen's state
/// @param frame the frame
/// @param fonts the fonts
/// @return the plan
Plan plan_of(
    const Browser& browser,
    const ScreenState& state,
    const kit::Frame& frame,
    const kit::Fonts& fonts
) {
    Plan plan;
    plan.window = window_of(frame);
    const Window& window = plan.window;
    const kit::Metrics& metrics = *window.metrics;
    plan.bar = bar_of(fonts, window, browser);
    Grid& grid = plan.grid;
    const int32_t top = plan.bar.bottom + window.gap;
    grid.view = {
        window.left,
        top,
        std::max(window.right - window.left, int32_t{0}),
        std::max(window.footer_rule - window.gap - top, int32_t{0})
    };
    const int32_t well_width = metrics.scroll_thumb_width + 2;
    grid.columns = grid_columns(frame.size_class);
    const kit::Rect area{
        grid.view.x,
        grid.view.y,
        std::max(grid.view.width - well_width - metrics.button_gap, int32_t{1}),
        0
    };
    grid.cells = kit::card_cells(area, browser.shown.size(), grid.columns);
    // The grid keeps a gap over the footer's rule, unless a card would then
    // never show whole: Compact's view runs to the rule instead.
    if (!grid.cells.empty() && grid.view.height < grid.cells.front().height)
        grid.view.height = std::max(
            std::min(grid.cells.front().height, window.footer_rule - top), grid.view.height
        );
    grid.well = {
        grid.view.x + grid.view.width - well_width, grid.view.y, well_width, grid.view.height
    };
    // The cells of the wider columns on the left are the taller, so the
    // content ends at the lowest foot of any cell.
    for (const kit::Rect& cell : grid.cells)
        grid.content_height = std::max(grid.content_height, cell.y + cell.height - grid.view.y);
    grid.limit = kit::ScrollArea::limit_for(grid.content_height, grid.view.height);
    grid.offset = std::clamp(state.scroll, int32_t{0}, grid.limit);
    return plan;
}

/// Returns the places of the cards whose pictures are asked for: those
/// whose cells meet the view at the grid's offset, and the row after them.
///
/// @param grid the grid
/// @return the places, from 0, in grid order
std::vector<std::size_t> places_in_view(const Grid& grid) {
    std::vector<std::size_t> places;
    if (grid.cells.empty() || grid.columns == 0)
        return places;
    std::optional<std::size_t> last_row;
    for (std::size_t place = 0; place < grid.cells.size(); ++place) {
        kit::Rect cell = grid.cells[place];
        cell.y -= grid.offset;
        if (kit::intersect(cell, grid.view).height <= 0)
            continue;
        places.push_back(place);
        last_row = place / grid.columns;
    }
    if (!last_row)
        return places;
    const std::size_t next_first = (*last_row + 1) * grid.columns;
    for (std::size_t place = next_first;
         place < grid.cells.size() && place < next_first + grid.columns;
         ++place)
        places.push_back(place);
    return places;
}

/// What a layout is built into: the items in drawing order, the controls in
/// groups tried in a fixed order, Tab's order and the names given.
struct Building {
    const Browser* browser{};            ///< the model
    const ScreenState* state{};          ///< the screen's state
    const kit::Fonts* fonts{};           ///< the fonts texts are measured in
    const Plan* plan{};                  ///< where everything lies
    kit::DisplayList list{};             ///< the items; its controls go to the groups
    std::vector<kit::Control> top{};     ///< the chips and the search
    std::vector<kit::Control> cards{};   ///< the cards
    std::vector<kit::Control> footer{};  ///< Cancel and the primary button
    std::vector<kit::ControlId> order{}; ///< Tab's order, as declared
    std::vector<std::string> names{};    ///< the names given so far
};

/// Returns a name no control of the layout has yet: the name itself, or
/// with "-2", "-3" and so on after it, and records it.
///
/// @param[in,out] building the layout
/// @param name the name wanted
/// @return the name given
std::string unique_name(Building& building, std::string name) {
    std::string given = name;
    for (int32_t count = 2;
         std::find(building.names.begin(), building.names.end(), given) != building.names.end();
         ++count)
        given = name + "-" + std::to_string(count);
    building.names.push_back(given);
    return given;
}

/// Returns a text cut with "..." to a width in the layout's fonts.
///
/// @param building the layout
/// @param role the font
/// @param piece the text, in UTF-8
/// @param width the room, in points
/// @return the text, whole when it fits
std::string
cut(const Building& building, kit::FontRole role, std::string_view piece, int32_t width) {
    return kit::cut_to_width(*building.fonts, role, piece, std::max(width, int32_t{0}));
}

/// Moves the controls a kit function appended to the list into a group.
///
/// @param[in,out] list the display list
/// @param from how many controls the list held before
/// @param[in,out] group the group they go to
void move_controls(kit::DisplayList& list, std::size_t from, std::vector<kit::Control>& group) {
    for (std::size_t index = from; index < list.controls.size(); ++index)
        group.push_back(std::move(list.controls[index]));
    list.controls.resize(from);
}

/// Appends an item.
///
/// @param[in,out] list the display list
/// @param role what it draws
/// @param rect where
/// @param clip what drawing may touch; empty for the window
/// @return the item, to fill in
kit::Item&
add_item(kit::DisplayList& list, kit::Role role, const kit::Rect& rect, const kit::Rect& clip) {
    kit::Item& added = list.items.emplace_back();
    added.role = role;
    added.rect = rect;
    added.clip = clip;
    return added;
}

/// Appends a text in a box.
///
/// @param[in,out] building the layout
/// @param role the font
/// @param drawn the text, as drawn
/// @param box the box
/// @param ink its colour
void add_text(
    Building& building, kit::FontRole role, std::string drawn, const kit::Rect& box, kit::Colour ink
) {
    if (drawn.empty())
        return;
    kit::Item& added = add_item(building.list, kit::Role::text, box, {});
    added.text = std::move(drawn);
    added.font = role;
    added.align = kit::Align::left;
    added.colour = ink;
}

/// Adds a control to Tab's order when it takes the focus.
///
/// @param[in,out] building the layout
/// @param control the control
void tab_stop(Building& building, const kit::Control& control) {
    if (control.enabled && control.focusable)
        building.order.push_back(control.id);
}

/// Tells whether the pointer is over a control.
///
/// @param building the layout
/// @param id the control
/// @return true while it is hovered
bool hovered(const Building& building, kit::ControlId id) {
    return building.state->interaction.hovered == id;
}

/// Tells whether a press is held on a control with the pointer over it.
///
/// @param building the layout
/// @param id the control
/// @return true while it is held
bool held(const Building& building, kit::ControlId id) {
    const kit::Interaction& interaction = building.state->interaction;
    return interaction.pressed == id && interaction.hovered == id;
}

/// Returns the header's close mark, the right-hand square in the middle of
/// the header's band, where Settings and the Library keep theirs.
///
/// @param window the window
/// @return the button
kit::Rect header_close_rect(const Window& window) {
    const kit::Metrics& compact = kit::compact_metrics;
    const int32_t side = compact.button_height;
    return {
        window.size.x - compact.edge - compact.padding - side,
        compact.edge + (window.header_height - side) / 2,
        side,
        side,
    };
}

/// Appends the header as Settings and the Library draw theirs: the OA mark,
/// "OPEN ANNIHILATION" then "MAPS" in the header's second colour, and at
/// the right the picker and the mod being played, cut so they stop short of
/// the close mark. The title is cut first when the window is too narrow for
/// both words.
///
/// @param[in,out] building the layout
/// @param window the window
void add_header(Building& building, const Window& window) {
    const Browser& browser = *building.browser;
    const kit::Metrics& compact = kit::compact_metrics;
    const bool skirmish = browser.opener == Browser::Opener::skirmish;
    std::string right;
    if (browser.mod_name.empty())
        right = ids::word(skirmish ? word_skirmish_game : word_battle_room_game);
    else
        right = ids::filled(
            skirmish ? word_skirmish_mod : word_battle_room_mod, {{"mod", browser.mod_name}}
        );
    const int32_t tracking = window.metrics->heading_tracking;
    // The kit's header keeps Compact's places from the window's edges. The
    // close mark keeps a square at the right, and the subtitle stops a gap
    // short of it.
    const int32_t title_left = compact.padding + compact.mark_side + compact.header_gap;
    const kit::Rect close = header_close_rect(window);
    const int32_t right_edge = close.x - compact.header_gap;
    kit::HeaderLook look;
    look.width = window.size.x;
    look.height = window.header_height;
    look.tracking = tracking;
    // The close mark sits in the padding the header otherwise gives the
    // subtitle, so the subtitle ends a gap short of that square.
    look.right_inset = compact.button_height + compact.header_gap;
    look.second = ids::word(word_title_second);
    look.second_width =
        kit::tracked_width(*building.fonts, kit::FontRole::regular, look.second, tracking);
    const int32_t title_room =
        std::max(right_edge - title_left - compact.header_gap - look.second_width, int32_t{0});
    look.title = ids::word(word_title);
    if (kit::tracked_width(*building.fonts, kit::FontRole::regular, look.title, tracking) >
        title_room)
        look.title =
            cut(building,
                kit::FontRole::regular,
                look.title,
                title_room - tracking * static_cast<int32_t>(kit::character_count(look.title)));
    look.title_width =
        kit::tracked_width(*building.fonts, kit::FontRole::regular, look.title, tracking);
    const int32_t version_room = std::max(
        right_edge - (title_left + look.title_width + compact.header_gap + look.second_width +
                      compact.header_gap),
        int32_t{0}
    );
    look.version = cut(building, kit::FontRole::small, right, version_room);
    look.version_width = kit::text_width(*building.fonts, kit::FontRole::small, look.version);
    const int32_t header_rule = compact.edge + window.header_height;
    add_item(building.list, kit::Role::header, {0, 0, window.size.x, header_rule + 1}, {}).look =
        std::move(look);
}

/// Appends the header's close mark. Pressing it asks for the same close as
/// Cancel. Tab's order gains it last, after every other stop.
///
/// @param[in,out] building the layout
/// @param window the window
void add_close_button(Building& building, const Window& window) {
    const kit::Rect rect = header_close_rect(window);
    kit::ButtonLook look;
    look.style = kit::ButtonStyle::quiet;
    look.hovered = hovered(building, ids::close);
    look.held = held(building, ids::close);
    look.mark = kit::ButtonMark::close;
    const std::size_t before = building.list.controls.size();
    kit::add_button(
        building.list, rect, look, ids::close, unique_name(building, std::string(name_close))
    );
    move_controls(building.list, before, building.top);
    building.top.back().text = ids::word(word_close);
}

/// Appends one source pill as a control of the bar.
///
/// @param[in,out] building the layout
/// @param rect the pill
/// @param caption its words, already looked up
/// @param on it is the chosen source
/// @param id its control
/// @param name its automation name
void add_source_pill(
    Building& building,
    const kit::Rect& rect,
    const std::string& caption,
    bool on,
    kit::ControlId id,
    std::string_view name
) {
    const kit::Interaction& interaction = building.state->interaction;
    kit::PillLook look;
    look.text = caption;
    look.state = on ? kit::PillState::active : kit::PillState::inactive;
    look.hovered = hovered(building, id);
    look.focused = interaction.focus_shown && interaction.focused == id;
    const std::size_t before = building.list.controls.size();
    kit::add_pill(building.list, rect, look, id, unique_name(building, std::string(name)));
    move_controls(building.list, before, building.top);
    tab_stop(building, building.top.back());
}

/// Appends a players or size strip. Its parts are set after the kit adds it,
/// which does not copy them. Tab stops on the strip once.
///
/// @param[in,out] building the layout
/// @param rect the strip
/// @param captions each segment's words, already looked up
/// @param level_width one segment's columns, inside the border
/// @param chosen the chosen segment, from 0
/// @param parts each segment's automation word
/// @param id its control
/// @param name its automation name
void add_strip(
    Building& building,
    const kit::Rect& rect,
    std::vector<std::string> captions,
    int32_t level_width,
    std::size_t chosen,
    std::span<const std::string_view> parts,
    kit::ControlId id,
    std::string_view name
) {
    kit::LevelsLook look;
    look.captions = std::move(captions);
    look.level_width = level_width;
    look.chosen = chosen;
    look.offered = look.captions.size();
    look.hovered = hovered(building, id);
    const std::size_t before = building.list.controls.size();
    kit::add_levels(building.list, rect, look, id, unique_name(building, std::string(name)));
    move_controls(building.list, before, building.top);
    kit::Control& control = building.top.back();
    control.parts.reserve(parts.size());
    for (const std::string_view part : parts)
        control.parts.emplace_back(part);
    tab_stop(building, control);
}

/// Appends a heading of the bar.
///
/// @param[in,out] building the layout
/// @param rect its box
/// @param english its word, in English
void add_heading(Building& building, const kit::Rect& rect, std::string_view english) {
    add_item(building.list, kit::Role::heading, rect, {}).look =
        kit::HeadingLook{ids::word(english)};
}

/// Appends the filter bar: the source pills, the players and size strips,
/// their headings and the search field.
///
/// @param[in,out] building the layout
void add_bar(Building& building) {
    const Bar& bar = building.plan->bar;
    const Filters& filters = building.browser->filters;
    const std::array<std::string, 4> captions = source_captions(*building.browser);
    for (std::size_t index = 0; index < ids::sources.size(); ++index)
        add_source_pill(
            building,
            bar.sources[index],
            captions[index],
            filters.source == ids::sources[index],
            ids::first_source + static_cast<kit::ControlId>(index),
            source_names[index]
        );
    add_heading(building, bar.players_heading, word_players_heading);
    add_strip(
        building,
        bar.players,
        player_captions(),
        bar.players_level_width,
        ids::players_index(filters),
        ids::player_parts,
        ids::players,
        name_players
    );
    add_heading(building, bar.sizes_heading, word_size_heading);
    add_strip(
        building,
        bar.sizes,
        size_captions(),
        bar.sizes_level_width,
        ids::size_index(filters),
        ids::size_parts,
        ids::sizes,
        name_sizes
    );
    kit::SearchLook look;
    look.field = building.state->search;
    const kit::Interaction& interaction = building.state->interaction;
    look.focused = interaction.focus_shown && interaction.focused == ids::search;
    look.hovered = hovered(building, ids::search);
    look.placeholder =
        cut(building,
            kit::FontRole::regular,
            ids::word(word_search),
            kit::field_text_rect(bar.search, look).width);
    const std::size_t before = building.list.controls.size();
    kit::add_search(
        building.list,
        bar.search,
        look,
        ids::search,
        unique_name(building, std::string(name_search))
    );
    move_controls(building.list, before, building.top);
    tab_stop(building, building.top.back());
}

/// Returns a map's width and height as a card and the footer show them:
/// "16×16", or its size text as written when it cannot be read.
///
/// @param card the card
/// @return the text
std::string sides_text(const MapCard& card) {
    if (const auto sides = map_sides(card.size))
        return std::to_string(sides->first) + "×" + std::to_string(sides->second);
    return card.size;
}

/// Returns the line under a card's title: the most players it is for, then
/// "<W>×<H>".
///
/// @param card the card
/// @return the line
std::string card_line(const MapCard& card) {
    return ids::filled(
        word_card_line,
        {{"players", std::to_string(shown_players(card))}, {"size", sides_text(card)}}
    );
}

/// Returns a card's overlay as the player reads it. "Downloaded", "Available",
/// "Downloading…" and "TA 3.1c" are looked up. A bundle's name, a mod's
/// name and a pack's name are shown as the card gives them.
///
/// @param card the card
/// @param downloading true while this card's pack is downloading
/// @return the overlay
std::string overlay_text(const MapCard& card, bool downloading = false) {
    const std::string english = overlay_name(card, downloading);
    if (english == word_downloaded || english == word_available || english == word_downloading ||
        english == word_ta)
        return ids::word(english);
    return english;
}

/// Returns a card's source chip: the overlay, coloured by where the map is.
///
/// A map not installed yet whose pack is named for it says "Available". While
/// that card's pack downloads, the chip says "Downloading…". "Downloaded"
/// is only an installed pack named for its map.
///
/// @param card the card
/// @param downloading true while this card's pack is downloading
/// @return the chip
kit::ChipLook source_chip(const MapCard& card, bool downloading = false) {
    kit::ChipLook chip;
    chip.text = overlay_text(card, downloading);
    switch (card.origin) {
    case MapCard::Origin::game:
        chip.state = kit::ChipState::get;
        break;
    case MapCard::Origin::mod:
        chip.state = kit::ChipState::playing;
        break;
    case MapCard::Origin::installed:
        chip.state = kit::ChipState::installed;
        break;
    case MapCard::Origin::online:
        chip.state = kit::ChipState::online;
        break;
    }
    return chip;
}

/// Tells whether a card's picture could not be had.
///
/// @param previews the hooks
/// @param card the card
/// @return true when it has none, or it failed
bool no_picture(const PreviewHooks& previews, const MapCard& card) {
    return card.preview_key.empty() || previews.ask == nullptr ||
           (previews.failed != nullptr && previews.failed(previews.context, card));
}

/// Appends the grid: a card item for each card in the view, a control for
/// every card that shows, the scroll bar while the cards do not fit, and
/// the words when no card shows. Asks for the pictures of the cards in the
/// view and the row after it alone.
///
/// @param[in,out] building the layout
/// @param previews where the pictures come from
void add_grid(Building& building, const PreviewHooks& previews) {
    const Browser& browser = *building.browser;
    const Grid& grid = building.plan->grid;
    const kit::Metrics& metrics = *building.plan->window.metrics;
    if (browser.shown.empty()) {
        add_text(
            building,
            kit::FontRole::small,
            cut(building, kit::FontRole::small, ids::word(word_no_maps), grid.view.width),
            {grid.view.x, grid.view.y, grid.view.width, metrics.small_line},
            kit::colour::hint
        );
        return;
    }
    const std::vector<std::size_t> asked = places_in_view(grid);
    for (std::size_t place = 0; place < browser.shown.size() && place < grid.cells.size();
         ++place) {
        const std::size_t index = browser.shown[place];
        const MapCard& card = browser.cards[index];
        kit::Rect cell = grid.cells[place];
        cell.y -= grid.offset;
        const kit::ControlId id = ids::card_control(place);
        const bool in_view = kit::intersect(cell, grid.view).height > 0;
        kit::CardLook look;
        look.title = card.title.empty() ? card.name : card.title;
        look.selected = browser.selected && *browser.selected == index;
        if (in_view) {
            const bool asks = std::find(asked.begin(), asked.end(), place) != asked.end() &&
                              !no_picture(previews, card);
            const oa::ui::map_previews::Thumbnail* thumbnail =
                asks ? previews.ask(previews.context, card) : nullptr;
            if (thumbnail != nullptr && thumbnail->width > 0 && thumbnail->height > 0)
                look.picture = {
                    static_cast<uint32_t>(thumbnail->width),
                    static_cast<uint32_t>(thumbnail->height),
                    std::span<const uint8_t>(thumbnail->rgba)
                };
            look.placeholder =
                ids::word(no_picture(previews, card) ? word_no_preview : word_loading);
            look.subtitle = card_line(card);
            const bool fetching = browser.downloading && *browser.downloading == card.name;
            look.chips.push_back(source_chip(card, fetching));
            if (!fits(card)) {
                kit::ChipLook problem;
                problem.text = ids::word(word_does_not_fit_chip);
                problem.state = kit::ChipState::problem;
                look.chips.push_back(std::move(problem));
            }
            look.hovered = hovered(building, id);
            look.greyed = !fits(card);
        } else if (
            std::find(asked.begin(), asked.end(), place) != asked.end() &&
            !no_picture(previews, card)
        ) {
            // The row after the view: asked for, not drawn.
            std::ignore = previews.ask(previews.context, card);
        }
        const std::size_t before = building.list.controls.size();
        const std::size_t items_before = building.list.items.size();
        kit::add_card(
            building.list,
            cell,
            look,
            id,
            unique_name(building, std::string(name_card_prefix) + card_word(card.name))
        );
        if (in_view)
            building.list.items.back().clip = grid.view;
        else
            building.list.items.resize(items_before);
        move_controls(building.list, before, building.cards);
        kit::Control& control = building.cards.back();
        control.clip = grid.view;
        control.group = ids::grid_group;
        control.text = card.name;
        tab_stop(building, control);
    }
    if (grid.limit > 0) {
        kit::ScrollArea scroll;
        scroll.view = grid.view;
        scroll.well = grid.well;
        scroll.page_step = grid.view.height;
        scroll.content_height = grid.content_height;
        scroll.limit = grid.limit;
        scroll.offset = grid.offset;
        add_item(building.list, kit::Role::scroll_bar, grid.well, {}).look =
            kit::ScrollBarLook{scroll.thumb(metrics.least_thumb_height), building.state->bar_held};
    }
}

/// Returns a download's size as the footer shows it: "2.3 MB", or "640 KB"
/// below a megabyte.
///
/// @param bytes the size, in bytes
/// @return the text
std::string download_text(uint64_t bytes) {
    if (bytes < megabyte) {
        const uint64_t kilobytes = std::max<uint64_t>((bytes + kilobyte - 1) / kilobyte, 1);
        return ids::filled(word_kilobytes, {{"size", std::to_string(kilobytes)}});
    }
    const uint64_t tenths = (bytes * 10 + megabyte / 2) / megabyte;
    return ids::filled(
        word_megabytes, {{"size", std::to_string(tenths / 10) + "." + std::to_string(tenths % 10)}}
    );
}

/// Returns the Compact footer's byline: the pack, or the source a game or
/// mod map shows, and the player count. The version, the registry, the size
/// and the download stay off this line so it and the fit chips both fit.
///
/// @param card the card
/// @return the line
std::string compact_byline(const MapCard& card) {
    std::vector<std::string> parts;
    if (card.origin == MapCard::Origin::game || card.origin == MapCard::Origin::mod) {
        const std::string source = overlay_text(card);
        if (!source.empty())
            parts.push_back(source);
    } else if (!card.pack_name.empty()) {
        parts.push_back(card.pack_name);
    }
    const int32_t players = shown_players(card);
    parts.push_back(
        players == 1 ? ids::word(word_players_one)
                     : ids::filled(word_players_many, {{"count", std::to_string(players)}})
    );
    std::string line;
    for (const std::string& part : parts) {
        if (!line.empty())
            line += " · ";
        line += part;
    }
    return line;
}

/// Returns the selected map's details line. A game or mod map leads with the
/// same name its overlay shows. A pack map leads with its pack, version and
/// registry, even when the overlay says Downloaded. Then the most players
/// it is for, its size and, online, its download.
///
/// @param card the card
/// @return the line
std::string details_line(const MapCard& card) {
    std::vector<std::string> parts;
    if (card.origin == MapCard::Origin::game || card.origin == MapCard::Origin::mod) {
        const std::string source = overlay_text(card);
        if (!source.empty())
            parts.push_back(source);
    } else {
        std::string pack = card.pack_name;
        if (!card.pack_version.empty())
            pack += (pack.empty() ? "" : " ") + card.pack_version;
        if (!pack.empty())
            parts.push_back(std::move(pack));
        if (!card.registry_name.empty())
            parts.push_back(card.registry_name);
    }
    const int32_t players = shown_players(card);
    parts.push_back(
        players == 1 ? ids::word(word_players_one)
                     : ids::filled(word_players_many, {{"count", std::to_string(players)}})
    );
    parts.push_back(sides_text(card));
    if (card.origin == MapCard::Origin::online && card.download_bytes != 0)
        parts.push_back(download_text(card.download_bytes));
    std::string line;
    for (const std::string& part : parts) {
        if (!line.empty())
            line += " · ";
        line += part;
    }
    return line;
}

/// Returns a fit chip: "Fits <game> ✓", "Doesn't fit <game>", or that its
/// fit is checked once it is installed.
///
/// @param fit the fit
/// @param game the game's or the mod's name
/// @return the chip
kit::ChipLook fit_chip(MapCard::Fit fit, const std::string& game) {
    kit::ChipLook chip;
    switch (fit) {
    case MapCard::Fit::fits:
        chip.text = ids::filled(word_fits, {{"game", game}});
        chip.state = kit::ChipState::installed;
        break;
    case MapCard::Fit::does_not_fit:
        chip.text = ids::filled(word_does_not_fit, {{"game", game}});
        chip.state = kit::ChipState::problem;
        break;
    case MapCard::Fit::unknown:
        chip.text = ids::filled(word_fit_later, {{"game", game}});
        chip.state = kit::ChipState::get;
        break;
    }
    return chip;
}

/// Returns the primary button's words.
///
/// @param kind what it does
/// @return the words, looked up
std::string primary_caption(Primary kind) {
    switch (kind) {
    case Primary::select:
        return ids::word(word_select);
    case Primary::download_and_select:
        return ids::word(word_download_select);
    case Primary::downloading:
        return ids::word(word_downloading);
    }
    return ids::word(word_select);
}

/// Returns a button's width as Settings sizes its buttons (Cancel, OK):
/// its caption in the regular font with two spaces either side, and at
/// least twice the control height.
///
/// @param building the layout
/// @param caption the caption
/// @return the width, in points
int32_t button_width(const Building& building, std::string_view caption) {
    const kit::Metrics& metrics = *building.plan->window.metrics;
    return std::max(
        kit::text_width(*building.fonts, kit::FontRole::regular, caption) + 4 * metrics.space,
        2 * metrics.control_height
    );
}

/// Appends a footer button, one of Settings' buttons: its caption in the
/// regular font, at the control height.
///
/// @param[in,out] building the layout
/// @param rect where it lies
/// @param caption its words, looked up
/// @param accent drawn as the main button while it is enabled
/// @param enabled it takes a press
/// @param id its control
/// @param name its automation name
void add_button(
    Building& building,
    const kit::Rect& rect,
    const std::string& caption,
    bool accent,
    bool enabled,
    kit::ControlId id,
    std::string_view name
) {
    kit::ButtonLook look;
    look.caption = cut(building, kit::FontRole::regular, caption, rect.width - 2);
    look.style = accent && enabled ? kit::ButtonStyle::accent : kit::ButtonStyle::plain;
    look.hovered = hovered(building, id);
    look.held = held(building, id);
    look.enabled = enabled;
    look.font = kit::FontRole::regular;
    const std::size_t before = building.list.controls.size();
    kit::add_button(building.list, rect, look, id, unique_name(building, std::string(name)));
    move_controls(building.list, before, building.footer);
    building.footer.back().text = caption;
    tab_stop(building, building.footer.back());
}

/// Places chips along a row from a column, chip_gap apart, as items alone.
///
/// @param[in,out] building the layout
/// @param chips the chips
/// @param at the first chip's left column
/// @param row the chips' top row
/// @return the column after the last chip
int32_t add_chip_items(
    Building& building, const std::vector<kit::ChipLook>& chips, int32_t at, int32_t row
) {
    const kit::Metrics& metrics = kit::compact_metrics;
    int32_t x = at;
    for (const kit::ChipLook& chip : chips) {
        const int32_t width = kit::chip_width(*building.fonts, chip);
        kit::add_chip(
            building.list, {x, row, width, metrics.chip_height}, chip, kit::no_control, {}
        );
        x += width + metrics.chip_gap;
    }
    return chips.empty() ? at : x - metrics.chip_gap;
}

/// Returns the total width of chips placed chip_gap apart.
///
/// @param building the layout
/// @param chips the chips
/// @return the width, in points
int32_t chips_width(const Building& building, const std::vector<kit::ChipLook>& chips) {
    int32_t width = 0;
    for (const kit::ChipLook& chip : chips)
        width += kit::chip_width(*building.fonts, chip) + kit::compact_metrics.chip_gap;
    return chips.empty() ? 0 : width - kit::compact_metrics.chip_gap;
}

/// Appends the footer: its band, the selected map's title and details, its
/// fit chips and the reason it does not fit or the download's failure, or
/// a progress bar while a download runs, and Cancel and the primary button
/// at the right.
///
/// @param[in,out] building the layout
void add_footer(Building& building) {
    const Browser& browser = *building.browser;
    const Window& window = building.plan->window;
    const kit::Metrics& metrics = *window.metrics;
    add_item(
        building.list,
        kit::Role::footer_band,
        {0, window.footer_rule, window.size.x, metrics.footer_height + 1},
        {}
    )
        .look = kit::FooterBandLook{window.size.x, window.footer_rule};
    if (window.footer_height > metrics.footer_height)
        add_item(
            building.list,
            kit::Role::fill,
            {metrics.edge,
             window.footer_top + metrics.footer_height,
             window.size.x - 2 * metrics.edge,
             window.footer_height - metrics.footer_height},
            {}
        )
            .colour = kit::colour::band;

    // Cancel and the primary button, at the right, two spaces apart as
    // Settings keeps Cancel and OK; the primary button is as wide as its
    // widest words, so it never moves as they change.
    const PrimaryAction primary = primary_action(browser);
    int32_t primary_width = 0;
    for (const Primary kind : {Primary::select, Primary::download_and_select, Primary::downloading})
        primary_width = std::max(primary_width, button_width(building, primary_caption(kind)));
    const std::string cancel_caption = ids::word(word_cancel);
    const int32_t cancel_width = button_width(building, cancel_caption);
    const kit::Rect primary_rect{
        window.right - primary_width, window.button_top, primary_width, window.control_height
    };
    const kit::Rect cancel_rect{
        primary_rect.x - 2 * metrics.space - cancel_width,
        window.button_top,
        cancel_width,
        window.control_height
    };
    add_button(building, cancel_rect, cancel_caption, false, true, ids::cancel, name_cancel);
    add_button(
        building,
        primary_rect,
        primary_caption(primary.kind),
        true,
        primary.enabled,
        ids::primary,
        name_select
    );

    // The selected map's fit chips, and the reason or the failure under them.
    std::vector<kit::ChipLook> chips;
    std::string reason;
    kit::Colour reason_ink = kit::colour::danger;
    const MapCard* card = browser.selected && *browser.selected < browser.cards.size()
                              ? &browser.cards[*browser.selected]
                              : nullptr;
    if (card != nullptr) {
        chips.push_back(fit_chip(card->base_fit, ids::word(word_game_short)));
        if (!browser.mod_name.empty())
            chips.push_back(fit_chip(card->mod_fit, browser.mod_name));
        if (!fits(*card))
            reason = primary.reason.empty() ? card->fit_reason : primary.reason;
    }
    if (!browser.error.empty())
        reason = browser.error;
    const bool downloading = browser.downloading.has_value();
    const int32_t room_right = cancel_rect.x - window.padding;
    const int32_t room = std::max(room_right - window.left, int32_t{0});

    int32_t text_right = room_right;
    // The selected map's title over its details, centred down the footer at
    // the left; at Compact the title in the buttons' middle, and the byline
    // on the line under them.
    const int32_t block = metrics.regular_line + metrics.small_line;
    const int32_t title_top =
        window.compact ? window.button_top + (window.control_height - metrics.regular_line) / 2
                       : window.footer_top + (window.footer_height - block) / 2;
    // Compact draws its byline here, on the line under the buttons, with the
    // chips after it across the whole footer. The title stays left of the
    // buttons.
    bool compact_byline_drawn = false;
    if (window.compact) {
        const int32_t row = window.button_top + window.control_height;
        const int32_t line_right = window.right;
        const int32_t line_width = std::max(line_right - window.left, int32_t{0});
        if (downloading) {
            kit::add_progress(
                building.list,
                {window.left,
                 row + (metrics.small_line - metrics.progress_bar_height) / 2,
                 line_width,
                 metrics.progress_bar_height},
                static_cast<int32_t>(std::clamp(browser.progress, 0.0F, 1.0F) * progress_whole),
                progress_whole
            );
        } else if (card != nullptr) {
            const std::string byline = compact_byline(*card);
            const int32_t byline_width =
                kit::text_width(*building.fonts, kit::FontRole::small, byline);
            const int32_t chip_span = chips_width(building, chips);
            const int32_t gap = chips.empty() ? 0 : metrics.label_gap;
            int32_t chip_x = window.left + byline_width + gap;
            if (!chips.empty() && chip_x + chip_span > line_right)
                chip_x = std::max(line_right - chip_span, window.left);
            const int32_t byline_room =
                chips.empty() ? line_width : std::max(chip_x - gap - window.left, int32_t{0});
            add_text(
                building,
                kit::FontRole::small,
                cut(building, kit::FontRole::small, byline, byline_room),
                {window.left, row, byline_room, metrics.small_line},
                kit::colour::hint
            );
            if (!chips.empty()) {
                const int32_t chip_row =
                    row + std::max((metrics.small_line - metrics.chip_height) / 2, int32_t{0});
                const int32_t after = add_chip_items(building, chips, chip_x, chip_row);
                if (!reason.empty()) {
                    const int32_t reason_left = after + metrics.label_gap;
                    add_text(
                        building,
                        kit::FontRole::small,
                        cut(building, kit::FontRole::small, reason, line_right - reason_left),
                        {reason_left,
                         row,
                         std::max(line_right - reason_left, int32_t{0}),
                         metrics.small_line},
                        reason_ink
                    );
                }
            }
            compact_byline_drawn = true;
        }
    } else {
        // Beside the title: the chips over the reason, or the progress bar,
        // ending a padding before Cancel.
        const int32_t chip_room = chips_width(building, chips);
        const int32_t reason_room =
            reason.empty() ? 0 : kit::text_width(*building.fonts, kit::FontRole::small, reason);
        // The title and details keep the room they need, up to half of it;
        // the chips and the reason, or the bar, take the rest they need.
        int32_t text_needed =
            kit::text_width(*building.fonts, kit::FontRole::small, ids::word(word_none_selected));
        if (card != nullptr)
            text_needed = std::max(
                kit::text_width(
                    *building.fonts,
                    kit::FontRole::regular,
                    card->title.empty() ? card->name : card->title
                ),
                kit::text_width(*building.fonts, kit::FontRole::small, details_line(*card))
            );
        const int32_t fit_room =
            std::max(room - std::min(text_needed, room / 2) - window.padding, int32_t{0});
        int32_t fit_width =
            downloading ? fit_room : std::min(std::max(chip_room, reason_room), fit_room);
        fit_width = std::max(fit_width, std::min(chip_room, room));
        const int32_t fit_left = room_right - fit_width;
        text_right = fit_left - window.padding;
        if (downloading) {
            kit::add_progress(
                building.list,
                {fit_left,
                 window.footer_top + (window.footer_height - metrics.progress_bar_height) / 2,
                 fit_width,
                 metrics.progress_bar_height},
                static_cast<int32_t>(std::clamp(browser.progress, 0.0F, 1.0F) * progress_whole),
                progress_whole
            );
        } else {
            const int32_t lines = reason.empty() ? 1 : 2;
            const int32_t fit_block =
                metrics.chip_height + (lines - 1) * (metrics.hint_gap + metrics.small_line);
            const int32_t chip_row = window.footer_top + (window.footer_height - fit_block) / 2;
            std::ignore =
                add_chip_items(building, chips, fit_left + fit_width - chip_room, chip_row);
            if (!reason.empty())
                add_text(
                    building,
                    kit::FontRole::small,
                    cut(building, kit::FontRole::small, reason, fit_width),
                    {fit_left,
                     chip_row + metrics.chip_height + metrics.hint_gap,
                     fit_width,
                     metrics.small_line},
                    reason_ink
                );
        }
    }

    // The selected map's title over its details, at the left.
    const int32_t text_width_room = std::max(text_right - window.left, int32_t{0});
    if (card == nullptr) {
        const int32_t line_top =
            window.compact ? window.button_top + (window.control_height - metrics.small_line) / 2
                           : window.footer_top + (window.footer_height - metrics.small_line) / 2;
        add_text(
            building,
            kit::FontRole::small,
            cut(building, kit::FontRole::small, ids::word(word_none_selected), text_width_room),
            {window.left, line_top, text_width_room, metrics.small_line},
            kit::colour::hint
        );
        return;
    }
    add_text(
        building,
        kit::FontRole::regular,
        cut(building,
            kit::FontRole::regular,
            card->title.empty() ? card->name : card->title,
            text_width_room),
        {window.left, title_top, text_width_room, metrics.regular_line},
        kit::colour::text
    );
    if (compact_byline_drawn)
        return;
    add_text(
        building,
        kit::FontRole::small,
        cut(building, kit::FontRole::small, details_line(*card), text_width_room),
        {window.left, title_top + metrics.regular_line, text_width_room, metrics.small_line},
        kit::colour::hint
    );
}

/// Appends the focus ring round the control that has the focus, unless it
/// is the search field, whose border shows it.
///
/// @param[in,out] building the layout
void add_focus_ring(Building& building) {
    const kit::Interaction& interaction = building.state->interaction;
    if (!interaction.focus_shown || interaction.focused == kit::no_control ||
        interaction.focused == ids::search)
        return;
    for (const auto* group : {&building.top, &building.cards, &building.footer})
        for (const kit::Control& control : *group) {
            if (control.id != interaction.focused || !control.focusable)
                continue;
            add_item(building.list, kit::Role::focus_ring, control.rect, control.clip).look =
                kit::FocusRingLook{true};
            return;
        }
}

} // namespace

kit::Point window_size(const kit::Frame& frame) noexcept {
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    kit::Point size{metrics.dialog_width, metrics.dialog_height};
    if (frame.area.width > 0)
        size.x = std::min(size.x, frame.area.width);
    if (frame.area.height > 0)
        size.y = std::min(size.y, frame.area.height);
    return size;
}

std::size_t grid_columns(kit::SizeClass size_class) noexcept {
    switch (size_class) {
    case kit::SizeClass::compact:
        return compact_columns;
    case kit::SizeClass::regular:
        return regular_columns;
    case kit::SizeClass::large:
        return large_columns;
    }
    return compact_columns;
}

std::vector<std::size_t> cards_in_view(
    const Browser& browser,
    const ScreenState& state,
    const kit::Frame& frame,
    const kit::Fonts& fonts
) {
    const Plan plan = plan_of(browser, state, frame, fonts);
    std::vector<std::size_t> indices;
    for (const std::size_t place : places_in_view(plan.grid))
        if (place < browser.shown.size())
            indices.push_back(browser.shown[place]);
    return indices;
}

kit::DisplayList browser_layout(
    const Browser& browser,
    const ScreenState& state,
    const kit::Frame& frame,
    const kit::Fonts& fonts,
    const PreviewHooks& previews
) {
    const Plan plan = plan_of(browser, state, frame, fonts);
    Building building;
    building.browser = &browser;
    building.state = &state;
    building.fonts = &fonts;
    building.plan = &plan;
    add_header(building, plan.window);
    add_close_button(building, plan.window);
    add_bar(building);
    add_grid(building, previews);
    add_footer(building);
    add_focus_ring(building);
    kit::DisplayList list = std::move(building.list);
    list.controls.clear();
    for (auto* group : {&building.top, &building.cards, &building.footer})
        for (kit::Control& control : *group)
            list.controls.push_back(std::move(control));
    building.order.push_back(ids::close);
    list.tab_order = std::move(building.order);
    return list;
}

} // namespace oa::ui::map_browser
