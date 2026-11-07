// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The map browser's screen, built only from the UI kit's components: the
// window at the size class's dialog size with its header and footer, the
// filter bar (sources, players, sizes and the search), the grid of map
// cards in 3, 4 or 6 columns, and the selected map's details, fit and
// buttons; its drawing; and the pointer, finger, keys, typed text and wheel
// that drive it. Nothing here hosts the screen, fetches or installs: a host
// lays it out for its frame, draws it, passes its input in and acts on what
// it returns. Every player word is looked up in the interface catalogue.
#pragma once

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/map_browser/model.hpp"
#include "oa/ui/map_previews/map_previews.hpp"

#include <cstddef>
#include <optional>
#include <stdint.h>
#include <string_view>
#include <vector>

namespace oa::ui::map_browser {

/// The browser's name on the OA layer; its controls' dialog is oa. and it.
inline constexpr std::string_view screen_name = "map-browser";

// The controls' automation names, which journeys press as oa. and the name.

/// The source pill All.
inline constexpr std::string_view name_filter_all = "maps-filter-all";
/// The source pill Game.
inline constexpr std::string_view name_filter_game = "maps-filter-game";
/// The source pill Mod.
inline constexpr std::string_view name_filter_mod = "maps-filter-mod";
/// The source pill Downloads.
inline constexpr std::string_view name_filter_downloads = "maps-filter-downloads";
/// The players strip. Its parts are any, 2, 3, 4 and 5-10.
inline constexpr std::string_view name_players = "maps-players";
/// The size strip. Its parts are any, s, m and l.
inline constexpr std::string_view name_sizes = "maps-size";
/// The search field.
inline constexpr std::string_view name_search = "maps-search";
/// What every card's name starts with; card_word of the map's game name follows.
inline constexpr std::string_view name_card_prefix = "maps-card-";
/// Cancel.
inline constexpr std::string_view name_cancel = "maps-cancel";
/// The primary button: Select, Download and select, or Downloading….
inline constexpr std::string_view name_select = "maps-select";
/// The header's close mark. A press closes the browser as Cancel does.
inline constexpr std::string_view name_close = "maps-close";
/// The source pills as one stop: All, Game, Mod and Downloads.
inline constexpr std::string_view name_filters = "maps-filters";
/// The map cards as one stop.
inline constexpr std::string_view name_cards = "maps-cards";

/// The columns of the card grid at Compact.
inline constexpr std::size_t compact_columns = 3;
/// The columns of the card grid at Regular.
inline constexpr std::size_t regular_columns = 4;
/// The columns of the card grid at Large.
inline constexpr std::size_t large_columns = 6;

/// Where a card's picture comes from: the host's preview cache. The layout
/// asks for the pictures of the cards in the grid's view and the row after
/// it, and of no other card.
struct PreviewHooks {
    void* context{}; ///< passed back to each function
    /// Returns a card's picture when it is decoded, and asks for it
    /// otherwise (PreviewCache::ask). Null shows no picture. The picture
    /// stays valid until the host next decodes (PreviewCache::pump).
    const oa::ui::map_previews::Thumbnail* (*ask)(void* context, const MapCard& card){};
    /// Tells whether a card's picture could not be had: its card shows "No
    /// preview" and is not asked for. Null tells that none failed.
    bool (*failed)(void* context, const MapCard& card){};
};

/// What kind of pointer event the screen is handed.
enum class PointerKind : uint8_t {
    move, ///< the pointer moved
    down, ///< a button was pressed, or a finger touched
    up,   ///< it was released, or the finger lifted
};

/// A press in the grid, which becomes a drag that scrolls it once the
/// pointer moves farther than its reach.
struct Drag {
    bool held{};       ///< a press in the grid is held
    kit::Point from{}; ///< where it pressed, in points
    int32_t offset{};  ///< how far the grid was scrolled when it pressed, in points
    bool scrolling{};  ///< it moved past its reach: it scrolls and presses nothing
};

/// What the browser's screen keeps between its inputs, beside the model.
struct ScreenState {
    kit::Interaction interaction{}; ///< the hover, the press and the focus
    int32_t scroll{};               ///< how far the grid is scrolled, in points
    kit::WheelCarry wheel{};        ///< the wheel's turn the grid has not scrolled yet
    /// The search field's text and caret. Its text is the filters' query:
    /// the input keeps them the same.
    kit::TextField search{};
    Drag drag{};        ///< a press in the grid
    bool bar_held{};    ///< the pointer holds the grid's scroll bar
    int32_t bar_grab{}; ///< where on the thumb the pointer took hold, from its top
};

/// What the browser asks of its host.
enum class BrowserAction : uint8_t {
    none,   ///< nothing: the screen did all of it itself
    cancel, ///< Cancel or Escape: close the browser and change nothing
    choose, ///< the primary button on the selected card: Select, or Download and select
};

/// What one input asked of the host.
struct ScreenResult {
    BrowserAction action{BrowserAction::none}; ///< what the host is to do
    bool redraw{}; ///< what the screen shows changed: lay it out and draw it again
};

/// Returns the window's size for a frame: the size class's dialog size
/// (kit::metrics_of), no larger than the frame's area.
///
/// @param frame the frame the layer gives
/// @return the width and height, in points
[[nodiscard]] kit::Point window_size(const kit::Frame& frame) noexcept;

/// Returns the card grid's columns at a size class: compact_columns,
/// regular_columns or large_columns.
///
/// @param size_class the class
/// @return the columns
[[nodiscard]] std::size_t grid_columns(kit::SizeClass size_class) noexcept;

/// Returns the cards whose pictures the layout asks for: those whose cells
/// meet the grid's view at its scroll, and those of the row after the last
/// of them.
///
/// @param browser the browser
/// @param state the screen's state
/// @param frame the frame
/// @param fonts the fonts the layout measures in
/// @return the cards, as indices into the browser's cards, in grid order
[[nodiscard]] std::vector<std::size_t> cards_in_view(
    const Browser& browser,
    const ScreenState& state,
    const kit::Frame& frame,
    const kit::Fonts& fonts
);

/// Lays the browser out for a frame.
///
/// The window, from its top left corner: the header titled "Maps" with the
/// picker and the mod being played at its right, and a close mark; the
/// filter bar, one line at Regular and Large and two at Compact (the source
/// pills and the search, then PLAYERS and SIZE); the grid of cards in
/// grid_columns columns, scrolled; and the footer with the selected map's
/// title and details, its fit chips and the reason it does not fit, a
/// progress bar in their place while a download runs, Cancel and the
/// primary button. Every control is named (screen.hpp's names, and one
/// name_card_prefix name per card that shows, "-2", "-3"… after a word
/// already taken in grid order). The players and size strips name each
/// segment as a part. Tab's order is the source pills (one stop), the
/// players strip, the sizes strip, the search, the cards (one stop, entered
/// at the selected card, else the first), Cancel, the primary button and the
/// close mark last. The arrows move inside the source pills and, both ways,
/// inside the card grid; Left and Right step a strip's choice.
/// Asks the hooks for the pictures of cards_in_view alone.
///
/// @param browser the browser
/// @param state the screen's state
/// @param frame the frame the layer gives
/// @param fonts the fonts the texts are measured in
/// @param previews where the cards' pictures come from
/// @return the display list, in the window's points
[[nodiscard]] kit::DisplayList browser_layout(
    const Browser& browser,
    const ScreenState& state,
    const kit::Frame& frame,
    const kit::Fonts& fonts,
    const PreviewHooks& previews
);

/// Draws the browser: the window's face, the display list, then the
/// window's edge (the kit's chrome), from the canvas's origin.
///
/// @param canvas where it is drawn
/// @param list what browser_layout gave
/// @param size the window's size, as window_size gave it
void draw_browser(const kit::Canvas& canvas, const kit::DisplayList& list, kit::Point size);

/// Takes a pointer's move, press or release, or a finger's.
///
/// A press on a source pill chooses that source; on a segment of the
/// players or size strip it chooses that one segment, and choosing the
/// segment already chosen leaves it chosen; on the search it gives it the
/// focus; on a card it selects the card, and on the selected card it acts
/// as the primary button; on Cancel, the close mark or the primary button
/// it asks for them. A press in the grid that moves farther than its reach
/// (a finger's, or a few points for a mouse) scrolls the grid instead, and
/// presses nothing. A press on the grid's scroll bar holds it: on the thumb
/// it grabs the thumb at the row pressed, on the well the thumb's middle
/// jumps to the pointer, and the thumb then follows the pointer until the
/// release.
///
/// @param[in,out] browser the browser
/// @param[in,out] state the screen's state
/// @param list the display list it was laid out to
/// @param kind move, down or up
/// @param button the pointer's button, 1 for the left; 0 for a move
/// @param at the pointer, in the window's points
/// @param finger_reach how far a finger's press may lie from a control, in points; 0 for a mouse
/// @return what the host is to do
ScreenResult browser_pointer(
    Browser& browser,
    ScreenState& state,
    const kit::DisplayList& list,
    PointerKind kind,
    uint8_t button,
    kit::Point at,
    int32_t finger_reach
);

/// Takes a key.
///
/// The arrows move the focus by where the controls lie (kit::focus_toward),
/// within the grid and between the bands, and the grid scrolls to show the
/// focused card. Left and Right on a players or size strip step its choice
/// instead, and stay on Any or the last segment at the ends. Tab and
/// Shift+Tab follow the declared order, entering the grid at the selected
/// card, else the first; Enter or Space on a source pill chooses it, on a
/// card selects it, and on the selected card or the primary button acts;
/// Enter or Space on a strip leaves its choice as it is; Enter in the
/// search goes to the grid; Escape cancels; Page Up, Page Down, Home and
/// End scroll the grid; the editing keys edit the focused search.
///
/// @param[in,out] browser the browser
/// @param[in,out] state the screen's state
/// @param list the display list it was laid out to
/// @param pressed the key
/// @return what the host is to do
ScreenResult
browser_key(Browser& browser, ScreenState& state, const kit::DisplayList& list, kit::Key pressed);

/// Takes text typed into the focused search: it is inserted at the caret
/// and the cards are filtered again as it goes.
///
/// @param[in,out] browser the browser
/// @param[in,out] state the screen's state
/// @param list the display list it was laid out to
/// @param utf8 the text
/// @return what the host is to do; nothing while the search has no focus
ScreenResult browser_text(
    Browser& browser, ScreenState& state, const kit::DisplayList& list, std::string_view utf8
);

/// Takes a turn of the wheel, or the controller's right stick: it scrolls
/// the grid.
///
/// @param[in,out] browser the browser
/// @param[in,out] state the screen's state
/// @param list the display list it was laid out to
/// @param notches how far it turned; positive towards the top
/// @return what the host is to do
ScreenResult
browser_wheel(Browser& browser, ScreenState& state, const kit::DisplayList& list, float notches);

/// Gives the search field the focus, as the controller's Y and a press on
/// the field do, so that typing searches.
///
/// @param[in,out] state the screen's state
/// @param list the display list it was laid out to
/// @return what the host is to do: a redraw; nothing without a search field
ScreenResult browser_find(ScreenState& state, const kit::DisplayList& list);

/// Returns the search field while it has the focus.
///
/// @param state the screen's state
/// @param list the display list it was laid out to
/// @return the field's rectangle, in the window's points; none otherwise
[[nodiscard]] std::optional<kit::Rect>
focused_field(const ScreenState& state, const kit::DisplayList& list) noexcept;

} // namespace oa::ui::map_browser
