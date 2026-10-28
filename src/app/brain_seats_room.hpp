// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the OA parts of the game's rooms share: the room's art, a picture
// of room parts drawn on a screen of the OA layer at the room's own place,
// the frame pixel a point lies on, and a computer seat's brain menu, drawn
// as the room's list box under the seat's button. The skirmish setup and
// the new-campaign screen (runtime_brain_seats_screen.cpp), the battle room
// (runtime_brain_room.cpp), the rooms' Mutators button
// (runtime_mutator_button.cpp) and a guest's Mutators in the battle room
// (runtime_get_mutators.cpp) draw through it, and the Mutators button and
// its card are shared here too.
#pragma once

#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/data/brain_library/seats.hpp"
#include "oa/ui/brain_picker/model.hpp"
#include "oa/ui/frontend_renderer/room_parts.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

/// The frame pixel a point of the OA layer lies on, through the picture's
/// place in the window: the inverse of frame_to_points.
///
/// @param seen what the layer's screens are placed on
/// @param layer_point the point, in the layer's points
/// @return the frame pixel
[[nodiscard]] oa::ui::kit::Point frame_point(const LayerView& seen, oa::ui::kit::Point layer_point);

/// Returns a rectangle of frame pixels in a screen's own points.
///
/// @param seen what the layer's screens are placed on
/// @param bounds the screen's place, in the layer's points
/// @param frame the rectangle, in frame pixels
/// @return the rectangle, in the screen's points
[[nodiscard]] oa::ui::kit::Rect frame_rect_local(
    const LayerView& seen,
    const oa::ui::kit::Rect& bounds,
    const oa::ui::frontend_renderer::SourceRect& frame
);

/// Returns the frame pixels a rectangle of the layer's points covers: the
/// inverse of frame_rect_points.
///
/// @param seen what the layer's screens are placed on
/// @param layer_points the rectangle, in the layer's points
/// @return the rectangle, in frame pixels
[[nodiscard]] oa::ui::frontend_renderer::SourceRect
points_rect_frame(const LayerView& seen, const oa::ui::kit::Rect& layer_points);

/// The files the battle room is drawn from: its background, which holds
/// its palette, the GUI palette, its own sprites and COMMONGUI.GAF.
///
/// @return the names
[[nodiscard]] inline oa::ui::frontend_renderer::ScreenAssetNames battle_room_art_names() {
    return {
        "",
        "bitmaps/battleroom.pcx",
        "palettes/guipal.pal",
        "anims/lounge2.gaf",
        "anims/commongui.gaf"
    };
}

/// Frame pixels round a part that its focus marker lights.
inline constexpr int32_t room_focus_margin = 7;

/// Returns a rectangle of frame pixels in the layer's points.
///
/// @param seen what the layer's screens are placed on
/// @param frame the rectangle, in frame pixels
/// @return the rectangle, in the layer's points
[[nodiscard]] oa::ui::kit::Rect
frame_rect_points(const LayerView& seen, const oa::ui::frontend_renderer::SourceRect& frame);

/// Draws a picture of room parts on a screen's canvas, over the room at the
/// parts' own place: the covered pixels alone, scaled as the room is.
///
/// @param canvas the screen's canvas
/// @param seen what the layer's screens are placed on
/// @param bounds the screen's place, in the layer's points
/// @param parts the parts
void paint_room_parts(
    const oa::ui::kit::Canvas& canvas,
    const LayerView& seen,
    const oa::ui::kit::Rect& bounds,
    const oa::ui::frontend_renderer::RoomCanvas& parts
);

/// What one row of a seat's brain menu is.
enum class BrainMenuKind : uint8_t {
    heading,    ///< the menu's title
    built_in,   ///< the built-in computer player
    brain,      ///< an installed brain
    options,    ///< "Options...", which opens the brain picker on the seat's options
    every_seat, ///< "Use for every computer seat", a check box
    get_brains, ///< "Get more brains…", which opens the Library at Brains
};

/// One row of a seat's brain menu.
struct BrainMenuRow {
    BrainMenuKind kind{};                    ///< what the row is
    std::string id{};                        ///< the brain's id; empty for every other row
    std::string words{};                     ///< the row's words, in UTF-8
    std::string level_id{};                  ///< the level the row plays at; empty for none
    std::string level_words{};               ///< that level's name, in UTF-8
    std::vector<std::string> level_ids{};    ///< the brain's levels, in its order
    std::vector<std::string> level_names{};  ///< their names, in UTF-8
    bool current{};                          ///< the seat plays it now
    bool unfit{};                            ///< it cannot play this game, and cannot be chosen
    oa::ui::brain_picker::SeatShown shown{}; ///< what its card reads
};

/// A seat's brain menu while it is open.
struct BrainMenu {
    int32_t seat{-1};                 ///< the seat it is open on; -1 while it is closed
    std::vector<BrainMenuRow> rows{}; ///< its rows, top to bottom
    int32_t selected{-1};             ///< the row the pointer or the keys are on; -1 for none
    bool every{};                     ///< "Use for every computer seat" is ticked
    /// The seat's options, kept while it keeps its brain.
    std::vector<std::pair<std::string, std::string>> options{};
    oa::ui::frontend_renderer::SourceRect rect{}; ///< the list box, in frame pixels
};

/// What a press or a key on the menu asks for.
struct BrainMenuAnswer {
    /// What to do.
    enum class Kind : uint8_t {
        none,       ///< nothing; the menu takes the input
        close,      ///< close the menu
        choose,     ///< write `choice` and close
        level,      ///< write `choice` and keep the menu open
        every,      ///< write `choice` onto every computer seat and keep the menu open
        options,    ///< close and open the brain picker on the seat's choice and its options
        get_brains, ///< close and open the Library at Brains
    } kind{};
    oa::ui::brain_picker::Choice choice{}; ///< the choice to write
    bool every{};                          ///< write it onto every computer seat
};

/// Words of the brain menu.
namespace brain_menu_words {
/// The menu's title over a seat.
inline constexpr std::string_view seat_heading = "Computer player for this seat";
/// The menu's title on the new-campaign screen.
inline constexpr std::string_view campaign_heading = "Computer player in the missions";
/// The check box that writes the seat's brain onto every computer seat.
inline constexpr std::string_view every_seat = "Use for every computer seat";
/// The row that opens the Library at Brains.
inline constexpr std::string_view get_brains = "Get more brains...";
/// The row that opens the brain picker on the seat's brain and its options.
inline constexpr std::string_view options = "Options...";
} // namespace brain_menu_words

/// Pixels of the value column, where a row's level stands.
inline constexpr int32_t brain_menu_value_width = 84;

/// Builds the room's list rows for the menu, in the room's bytes.
///
/// @param menu the menu
/// @return one list row a menu row
[[nodiscard]] std::vector<oa::ui::frontend_renderer::RoomListRow>
brain_menu_list_rows(const BrainMenu& menu);

/// Places the menu under a button, inside the room: left-aligned with it,
/// above it when there is no room below, and moved in from the room's
/// edges.
///
/// @param art the room's art
/// @param[in,out] menu the menu; its rectangle is set
/// @param button the button it opens from, in frame pixels
/// @param room the room's frame, in frame pixels
void place_brain_menu(
    const oa::ui::frontend_renderer::RoomArt& art,
    BrainMenu& menu,
    const oa::ui::frontend_renderer::SourceRect& button,
    const oa::ui::frontend_renderer::SourceRect& room
);

/// The row of the menu a frame pixel lies on.
///
/// @param art the room's art
/// @param menu the menu
/// @param at the pixel
/// @return the row, or -1
[[nodiscard]] int32_t brain_menu_row_at(
    const oa::ui::frontend_renderer::RoomArt& art, const BrainMenu& menu, oa::ui::kit::Point at
);

/// The level arrows of a row: the left one steps to the level before, the
/// right one to the level after. Empty for a row with one level or none.
struct BrainMenuArrows {
    oa::ui::frontend_renderer::SourceRect previous{}; ///< the left arrow, in frame pixels
    oa::ui::frontend_renderer::SourceRect next{};     ///< the right arrow, in frame pixels
};

/// Returns where a row's level arrows lie: the current row, a brain's or
/// the built-in player's, when it has more than one level.
///
/// @param art the room's art
/// @param menu the menu
/// @param row the row
/// @return the arrows; empty rectangles without them
[[nodiscard]] BrainMenuArrows brain_menu_arrows(
    const oa::ui::frontend_renderer::RoomArt& art, const BrainMenu& menu, int32_t row
);

/// Answers a press at a frame pixel: a row is chosen, an arrow steps the
/// level, the check box turns, the Library row goes to the Library, and a
/// press outside the menu closes it.
///
/// @param art the room's art
/// @param[in,out] menu the menu; the check box and the selected row change
/// @param at the pixel
/// @return what to do
[[nodiscard]] BrainMenuAnswer brain_menu_press(
    const oa::ui::frontend_renderer::RoomArt& art, BrainMenu& menu, oa::ui::kit::Point at
);

/// Answers a key: Up and Down move the selection over the rows that can be
/// chosen, Left and Right step the current row's level on its row, Return
/// and Space press the selected row, Escape closes.
///
/// @param[in,out] menu the menu
/// @param pressed the key
/// @return what to do; none for a key the menu does not use, which it takes all the same
[[nodiscard]] BrainMenuAnswer brain_menu_key(BrainMenu& menu, oa::ui::kit::Key pressed);

/// Adds the menu's controls to a display list for automation and the
/// pointer: brain-menu.row.<id> for each brain (brain-menu.row.built-in for
/// the built-in player), brain-menu.level.previous and brain-menu.level.next
/// on the current row, brain-menu.options, brain-menu.every-seat, checked
/// while ticked, and brain-menu.get-brains. A row's text is its words, and its level's
/// after " · ".
///
/// @param list the list
/// @param art the room's art
/// @param menu the menu
/// @param seen what the layer's screens are placed on
/// @param bounds the screen's place, in the layer's points
/// @param first_id the first control id to use
void add_brain_menu_controls(
    oa::ui::kit::DisplayList& list,
    const oa::ui::frontend_renderer::RoomArt& art,
    const BrainMenu& menu,
    const LayerView& seen,
    const oa::ui::kit::Rect& bounds,
    oa::ui::kit::ControlId first_id
);

/// A room's Mutators button's text for automation, in the language shown:
/// "Mutators · off" while none is on, or "Mutators · 4 on" with four. Its
/// first word is the button's word as draw_mutators_switch draws it, ended
/// with "…" where the button cuts it.
///
/// @param art the room's art
/// @param frame the button, in frame pixels
/// @param count how many mutators are on
/// @return the text
[[nodiscard]] std::string mutators_automation_text(
    const oa::ui::frontend_renderer::RoomArt& art,
    const oa::ui::frontend_renderer::SourceRect& frame,
    std::size_t count
);

/// One mutator the Mutators card names.
struct MutatorsCardItem {
    std::string name{}; ///< its name, or its id
    bool lacking{};     ///< this machine does not have the host's release
};

/// The most mutators the Mutators card names one by one; past them it
/// reads "and N more".
inline constexpr std::size_t mutators_card_rows = 6;

/// The Mutators button's card: a title that counts the mutators on, each by
/// name in the order they apply (one this machine lacks marked "not
/// installed"), "and N more" past mutators_card_rows, and a line on what a
/// press does.
///
/// @param items the mutators on, in the order they apply
/// @param host the player can change the set; otherwise a press reads it
/// @return the card, its arrow not yet pointed
[[nodiscard]] oa::ui::kit::HoverCardLook
mutators_card(std::span<const MutatorsCardItem> items, bool host);

/// Draws a room's Mutators button: the room's on and off button with the OA
/// stamp at its left, the word "Mutators" in the language shown, the lamp
/// asked for and, while it holds the focus, the room's focus marker round
/// it. The word is drawn in the room's caption font, else its label font,
/// and cut with "..." only when even that does not fit.
///
/// @param parts the canvas, which reaches round the button for the marker
/// @param art the room's art
/// @param frame the button, in frame pixels
/// @param lamp the lamp
/// @param condition pressed or grayed draws it so
/// @param focused the button holds the focus
void draw_mutators_switch(
    oa::ui::frontend_renderer::RoomCanvas& parts,
    const oa::ui::frontend_renderer::RoomArt& art,
    const oa::ui::frontend_renderer::SourceRect& frame,
    oa::ui::frontend_renderer::RoomLamp lamp,
    oa::ui::frontend_renderer::ButtonCondition condition,
    bool focused
);

/// Returns the lamp a Mutators button shows in a picture of the window, for
/// the checks: its lamps found in the room's art, where the button stands.
///
/// @param runtime the runtime
/// @param seen what the layer's screens are placed on
/// @param button the button, in the window's pixels (its automation place)
/// @param battle true in the battle room, whose art differs from the setups'
/// @param picture the window's picture
/// @return red, green or unlit
[[nodiscard]] oa::ui::frontend_renderer::RoomLamp mutators_lamp_seen(
    Runtime& runtime,
    const LayerView& seen,
    const oa::ui::kit::Rect& button,
    bool battle,
    const oa::ui::frontend_renderer::Surface& picture
);

/// Reads the runtime fields the seat screen's helpers need, and writes a
/// choice onto computer seats. The helpers are not members.
struct BrainSeatsAccess {
    /// Tells whether the seat buttons show.
    ///
    /// @param runtime the runtime
    /// @return true on SKIRMISH, NEWGAME or Any Mission, with no match, no dialog, fonts, and a brain installed
    static bool shows(Runtime& runtime);

    /// Writes a choice onto one computer seat, or onto every computer seat.
    ///
    /// A slot that is not a computer seat is left as it is, including a seat
    /// stored for a slot that is no longer a computer.
    ///
    /// @param runtime the runtime
    /// @param slot the seat the menu was opened on
    /// @param choice the choice
    /// @param every true when the choice is for every computer seat
    static void
    write(Runtime& runtime, int32_t slot, const oa::ui::brain_picker::Choice& choice, bool every);

    /// Writes a choice onto the campaign's computer player, at place 1.
    ///
    /// The skirmish slots are not the mission's players while NEWGAME shows,
    /// so this does not ask whether a slot is a computer.
    ///
    /// @param runtime the runtime
    /// @param choice the choice
    static void write_campaign(Runtime& runtime, const oa::ui::brain_picker::Choice& choice);

    /// Returns the art the shown front-end screen draws with, and its palette.
    ///
    /// @param runtime the runtime
    /// @return the art; its resources are the screen's own
    static oa::ui::frontend_renderer::RoomArt screen_art(const Runtime& runtime);

    /// Returns the front end's frame as last drawn.
    ///
    /// @param runtime the runtime
    /// @return the frame, the 640 by 480 picture the room is drawn in
    static const oa::ui::frontend_renderer::Surface& frame(const Runtime& runtime);

    /// Loads the art of a room the runtime does not hold, such as the battle
    /// room's, through the runtime's game files.
    ///
    /// @param runtime the runtime
    /// @param names the room's background, palette and sprites
    /// @return the art; nothing when a file is missing or cannot be read
    static std::optional<oa::ui::frontend_renderer::ScreenResources>
    load_art(Runtime& runtime, const oa::ui::frontend_renderer::ScreenAssetNames& names);

    /// Leaves the setup for the main menu, where the Library opens at
    /// Brains: the Library opens over the main menu alone.
    ///
    /// @param runtime the runtime
    static void get_more_brains(Runtime& runtime);

    /// Builds a seat's brain menu: its title, the built-in player, every brain
    /// installed (each at the level the seat plays it at, or the level the
    /// setup's difficulty gives), "Options..." while the seat's brain has
    /// options, "Use for every computer seat" and "Get more brains…" where
    /// the room offers them.
    ///
    /// @param runtime the runtime
    /// @param seat the seat it opens on
    /// @param current the seat's choice
    /// @param difficulty the setup's difficulty
    /// @param built_in_level the built-in player's level, in the language shown
    /// @param built_in_levels the built-in player's levels the menu steps
    ///     through, each its id and its name in the language shown, easiest
    ///     first; empty where the room's Difficulty sets the level
    /// @param heading the title, in English
    /// @param every_row offer "Use for every computer seat"
    /// @param library_row offer "Get more brains…"
    /// @return the menu, open, its current row selected
    static BrainMenu menu(
        Runtime& runtime,
        int32_t seat,
        const oa::ui::brain_picker::Choice& current,
        oa::data::brain_library::Difficulty difficulty,
        std::string_view built_in_level,
        const std::vector<std::pair<std::string, std::string>>& built_in_levels,
        std::string_view heading,
        bool every_row,
        bool library_row
    );
};

} // namespace oa::app
