// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's screens, built only from the UI kit's components: the list
// and details laid out at the three size classes (a list page and a details
// page of its own at Compact, list and details side by side at Regular, and
// filters, list and details at Large), their drawing, and the pointer,
// finger, keys, typed text, wheel and controller that drive them. Nothing
// here hosts the screen, fetches or installs: a host lays it out for its
// frame, draws it, passes its input in and acts on the actions it returns.
// Its words are in text.hpp.
#pragma once

#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/library/library.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::library {

/// One of the Library's drop-downs, which can be open.
enum class MenuKind : uint8_t {
    none,   ///< no drop-down is open
    filter, ///< Compact's filter: the filters, then each tag of the tab (library.filter)
    tags,   ///< the tags that do not fit as chips or in the filter pane (library.tags)
    more,   ///< the Compact details page's actions after its first three (library.action)
};

/// One of the Library's panes that scroll.
enum class Pane : uint8_t {
    none, ///< none of them
    list, ///< the list of entries
    details, ///< the selected entry's details: the pane at Regular and Large, the page's body at Compact
    maps, ///< the Active tab's maps, under the MAPS heading; the mod and the language stay put
};

/// A finger's press in a pane, which becomes a drag that scrolls the pane
/// once the finger moves farther than its reach.
struct Drag {
    Pane pane{Pane::none}; ///< the pane the finger pressed in; none while no finger presses one
    kit::Point from{};     ///< where the finger pressed, in points
    int32_t offset{};      ///< how far the pane was scrolled when it pressed, in points
    bool scrolling{};      ///< it moved past the finger's reach: it scrolls and presses nothing
};

/// A press on the tab strip, which becomes a drag that scrolls the strip
/// sideways once it moves far enough.
struct TabDrag {
    bool held{};      ///< a press on the strip is held
    int32_t from_x{}; ///< where it pressed, in points
    int32_t scroll{}; ///< how far the strip was scrolled when it pressed, in points
    bool scrolling{}; ///< it moved far enough: it scrolls the strip and presses nothing
};

/// What the Library's screen keeps between its inputs, beside the model.
struct ScreenState {
    kit::Interaction interaction{}; ///< the hover, the press and the focus
    /// How far the list is scrolled, in points. The layout writes it when
    /// reveal_row names a row to bring into view.
    mutable int32_t list_scroll{};
    /// How far the tab strip is scrolled, in points from the first tab. The
    /// layout writes it clamped to the strip, and so the open tab stays
    /// wholly in view until the player scrolls the strip.
    mutable int32_t tab_scroll{};
    /// The player scrolled the tab strip with an arrow, the wheel or a drag
    /// since the open tab changed, so the strip stays where they left it.
    mutable bool tab_scrolled{};
    /// The tab the strip was last laid out with open; none before the first
    /// layout. Opening another brings it into view again.
    mutable std::optional<Tab> tab_laid_out{};
    TabDrag tab_drag{};       ///< a press on the tab strip
    int32_t details_scroll{}; ///< how far the details are scrolled, in points
    /// A row of the list to bring into view on the next layout, its place
    /// among the visible entries from 0. The layout scrolls to it and clears
    /// this. -1 leaves the scroll as it is. A From link sets it so the pack
    /// it names is in view once the Maps tab is laid out.
    mutable int32_t reveal_row{-1};
    kit::WheelCarry list_wheel{};    ///< the wheel's turn the list has not scrolled yet
    kit::WheelCarry details_wheel{}; ///< the wheel's turn the details have not scrolled yet
    /// The search field's text and caret. Its text is the model's query: the
    /// input keeps them the same.
    kit::TextField search{};
    /// The Active tab's maps search, above that pane. It is not the catalogue's
    /// query. Its text stays while the Library is open, on every tab.
    kit::TextField maps_search{};
    /// How far the Active tab is scrolled, in points: its maps pane, or the
    /// whole page when the tiles leave the maps fewer than four rows.
    int32_t maps_scroll{};
    kit::WheelCarry maps_wheel{}; ///< the wheel's turn that scroll has not taken yet
    /// The pane whose scroll bar the pointer holds; none while it holds none.
    /// The Registries tab's bar and the Compact details page's are the
    /// details'.
    Pane bar_held{Pane::none};
    int32_t bar_grab{};            ///< where on the thumb the pointer took hold, from its top
    MenuKind menu{MenuKind::none}; ///< the open drop-down
    int32_t menu_first{};          ///< the open drop-down's first item shown, from 0
    int32_t menu_marked{}; ///< the open drop-down's item the keys or the pointer mark, from 0
    /// Compact shows the selected entry's details page. It shows while this
    /// and the model's details_open are both set: a refresh that closes the
    /// model's details closes the page too.
    bool details_page{};
    Drag drag{}; ///< a finger's press in a pane
    /// How far a turning refresh mark has turned, in eighths of a turn, 0 to 7.
    /// step_refresh_marks advances it from the time spent turning.
    uint8_t refresh_turn{};
    /// How long the header's check has been showing, in milliseconds. Zero
    /// while the arrow rests or turns. The check lasts kit::refresh_check_ms.
    uint16_t refresh_check_ms{};
    /// Milliseconds the marks have been turning, since the latest turn began.
    uint32_t refresh_spin_ms{};
    /// A mark was turning on the previous step.
    bool refresh_spinning{};
    /// The header's refresh was running on the previous step.
    bool refresh_was_checking{};
    /// This turn of the header's refresh has read a list.
    bool refresh_read_list{};

    /// One registry's refresh mark, between steps.
    struct RegistryMark {
        std::string id{};      ///< the registry's id
        bool was_refreshing{}; ///< it was being asked on the previous step
        uint16_t check_ms{};   ///< how long its check has shown; 0 while it has not
    };

    /// The registries whose marks are turning or showing a check.
    std::vector<RegistryMark> registry_marks{};
};

/// One registry's refresh, as the inputs name it for one step of the marks.
struct RegistryPulse {
    std::string_view id{}; ///< the registry's id
    bool refreshing{};     ///< it is being asked now
    bool failed{};         ///< the last ask found nothing usable
};

/// Advances every refresh mark by the time since the previous step.
///
/// While a refresh runs, the shared turn is the eighth kit::refresh_eighth
/// names for the milliseconds spent turning, so the place after a given time
/// does not depend on how many frames made that time up. When the header's
/// refresh stops after a list was read, or a registry's does and that ask
/// did not fail, its mark shows the check; a failure leaves the arrow. A
/// refresh that starts again clears that mark's check at once. The return is
/// false while nothing a mark draws has changed, so a host can leave the
/// screen as it is.
///
/// @param[in,out] state the marks, and what the previous step left
/// @param checking the header's refresh is running
/// @param registries each registry's refresh this step
/// @param elapsed_ms the milliseconds since the previous step
/// @return true when a mark's arrow, turn or check would draw differently
[[nodiscard]] bool step_refresh_marks(
    ScreenState& state,
    bool checking,
    std::span<const RegistryPulse> registries,
    uint32_t elapsed_ms
);

/// What one input asked of the host.
struct ScreenResult {
    /// What the host is to do: an entry's action, UPDATE ALL, the header's
    /// refresh or CLOSE. None when the screen did all of it itself, as it
    /// does for browsing, DETAILS and the way back from the details page.
    std::optional<Action> action{};
    EntryId entry{}; ///< the entry an entry's action is for; empty for the others
    /// The registry a Registries-tab action names, or the registry an expansion's
    /// download uses.
    std::string registry{};
    /// The switch's new state, for Action::set_registry.
    bool registry_on{};
    /// The expansion ADD FROM…, a download arrow, or the expansion-off question names.
    std::string expansion{};
    /// The base game USE THIS GAME asked the host to copy.
    std::string base_game{};
    bool redraw{}; ///< what the screen shows changed: lay it out and draw it again
};

/// What a pointer did, as the OA layer passes it to a screen.
enum class PointerKind : uint8_t {
    move, ///< it moved
    down, ///< a button or a finger pressed
    up,   ///< a button or a finger let go
};

/// The Library's own keys and controller buttons, beyond the kit's keys. A
/// host maps the platform's keys and buttons to these. The controller's
/// D-pad and left stick are the kit's arrows, its A is Space, and its right
/// stick is library_stick.
enum class ScreenCommand : uint8_t {
    find,         ///< Ctrl+F (Cmd+F on macOS), `/` outside the search, and the controller's Y
    next_tab,     ///< Ctrl+Tab and the controller's RB
    previous_tab, ///< Ctrl+Shift+Tab and the controller's LB
    back,         ///< the controller's B: from the details page back to the list, else close
    /// The controller's X: the selected entry's first action when it is GET,
    /// UPDATE, CANCEL or RETRY and enabled.
    first_action,
    /// F5 and the header's refresh icon, when a registry can be asked.
    refresh,
};

/// Returns the Library's window size for a frame: its size class's dialog
/// size (480 by 324, 720 by 486 or 960 by 600 points), no larger than the
/// frame's area. A host places the window by it.
///
/// @param frame the room the screen lays out in
/// @return the width and height, in points
[[nodiscard]] kit::Point window_size(const kit::Frame& frame) noexcept;

/// Lays the Library out for a frame, from the window's top left corner.
///
/// Compact shows the list page, or the selected entry's details page
/// (details_page_layout) while the state and the model both say it shows.
/// Regular shows the list and the details side by side, and Large the
/// filters, the list and the details. Every control carries its automation
/// name (library.…). The Keys tab is `library.tab.keys`, between Brains and
/// Languages. The Active panel's layout line is `library.active.keys`, and
/// its CHANGE… button is `library.active.keys.change`. The layout reads the
/// model's visible entries, its entries' actions, facts, status lines and
/// bylines, the playing note and the queue lines, and decides no rule of
/// its own.
///
/// @param library the model; the display list refers to its badges' pixels,
///     and to the base game's badge scaled for this layout, until the next
///     layout or refresh
/// @param state the hover, press, focus, scroll, search and open drop-down
/// @param frame the room, its size class and its scale
/// @param fonts the fonts its texts are measured in
/// @return what the screen draws and where its controls are
[[nodiscard]] kit::DisplayList library_layout(
    const Library& library,
    const ScreenState& state,
    const kit::Frame& frame,
    const kit::Fonts& fonts
);

/// Lays out Compact's details page of the selected entry: its name and
/// version in the header; the status line, the byline, the summary, the
/// short facts and the playing note; and the first three actions, MORE ▾
/// with the rest and BACK in the footer. BACK returns to the list.
///
/// @param library the model, with an entry selected
/// @param state the hover, press, focus, scroll and open drop-down
/// @param frame the room; the page takes its class's window size
/// @param fonts the fonts its texts are measured in
/// @return what the page draws and where its controls are
[[nodiscard]] kit::DisplayList details_page_layout(
    const Library& library,
    const ScreenState& state,
    const kit::Frame& frame,
    const kit::Fonts& fonts
);

/// Draws a laid-out Library through the kit's painter (kit::paint).
///
/// @param canvas where it is drawn, from the window's top left corner
/// @param list the display list library_layout made
void draw_library(const kit::Canvas& canvas, const kit::DisplayList& list);

/// How many rows past each edge of the list have their pictures asked for,
/// so a short scroll meets pictures already on the way.
inline constexpr std::size_t picture_margin = 3;

/// What the list asks for the pictures of its rows.
struct PictureSource {
    void* context{}; ///< passed back to each function
    /// Tells whether this entry's pictures were already asked for, are held
    /// or were refused. Null asks every time. The selected row is still
    /// offered, so its picture can keep the first place.
    bool (*known)(void* context, const Entry& entry){};
    /// Asks for one entry's pictures. `lead` is the selected row, asked
    /// before the others.
    void (*request)(void* context, const Entry& entry, bool lead){};
};

/// Returns the places, among the visible entries, whose pictures are asked
/// for. The selected entry is first. Then every row the list shows, and
/// picture_margin rows before the first of those and after the last.
///
/// @param library the model
/// @param list the display list the screen shows
/// @return the places, from 0, the selected entry's first when it shows
[[nodiscard]] std::vector<std::size_t>
picture_places(const Library& library, const kit::DisplayList& list);

/// Asks the source for the pictures of picture_places. An entry the source
/// already knows is skipped, except the selected row, which is always
/// offered first.
///
/// @param library the model
/// @param list the display list the screen shows
/// @param source where the pictures are asked for; a null request asks nothing
void request_visible_pictures(
    const Library& library, const kit::DisplayList& list, const PictureSource& source
);

/// What the Registries tab, and a selected entry's From line, ask for a
/// registry's icon. The same rules as a badge: one ask, and a picture
/// already held is not asked for again.
struct RegistryPictureSource {
    void* context{}; ///< passed back to each function
    /// Tells whether this registry's icon was already asked for, is held or
    /// was refused. Null asks whenever the registry names an address and
    /// has no picture yet. A shipped logo is never offered.
    bool (*known)(void* context, const Registry& registry){};
    /// Asks for one registry's icon.
    void (*request)(void* context, const Registry& registry){};
};

/// Asks the source for each registry icon the screen is showing.
///
/// On the Registries tab, every registry that names an address and has no
/// picture yet. On any tab, the selected entry's registry, for its From
/// line. A shipped logo is never asked for. A registry is offered once in
/// one call. Nothing is asked when request is null.
///
/// @param library the model
/// @param source where the icons are asked for
void request_registry_icons(const Library& library, const RegistryPictureSource& source);

/// Tells whether an entry's row is on the list the screen shows, or is the
/// selected entry.
///
/// @param library the model
/// @param list the display list the screen shows
/// @param id the entry
/// @return true when its row is shown, or it is selected
[[nodiscard]] bool
entry_row_shown(const Library& library, const kit::DisplayList& list, const EntryId& id);

/// Returns the letters a badge shows when its package has no picture: a
/// name of one word gives its first three characters, a name of more its
/// words' first characters, at most three; letters a to z in capitals.
///
/// @param name the name, in UTF-8
/// @return the letters, such as "RID" for "Ridge" and "TZ" for "TA Zero"
[[nodiscard]] std::string badge_letters(std::string_view name);

/// Returns the search field while it has the focus, over which a host keeps
/// the system's text input started. On the Active tab that field is the maps
/// search; on the catalogue tabs it is the catalogue search.
///
/// @param state the focus
/// @param list the display list
/// @return the field's rectangle, in points; none while it has not the focus
[[nodiscard]] std::optional<kit::Rect>
focused_field(const ScreenState& state, const kit::DisplayList& list) noexcept;

/// Takes a pointer's move, press or release, in the window's points.
///
/// A press on a row selects it, and at Compact opens its details page; no
/// press on a row gets, updates or installs anything. A finger's press
/// reaches the nearest control within its reach; one in the list or the
/// details that moves farther than its reach scrolls that pane instead. A
/// press on a pane's scroll bar, the mouse's or a finger's, holds it: on the
/// thumb it grabs the thumb at the row pressed, on the well the thumb's
/// middle jumps to the pointer, and the thumb then follows the pointer until
/// the release. A press outside an open drop-down closes it.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param kind a move, a press or a release
/// @param button the pointer's button, 1 for the left; 0 for a move
/// @param at the pointer, in the window's points; it may lie outside the window
/// @param finger_reach how far a finger's press may lie from a control, in
///     points; 0 for a mouse
/// @return what the host is to do
ScreenResult library_pointer(
    Library& library,
    ScreenState& state,
    const kit::DisplayList& list,
    PointerKind kind,
    uint8_t button,
    kit::Point at,
    int32_t finger_reach
);

/// Takes a key, as the kit names it.
///
/// Tab and Shift+Tab follow the declared order: the header's refresh icon
/// when it can be pressed, then the tabs, the search, the filters, the
/// list, the details' actions and the footer's buttons. The
/// arrows move by where controls sit. On the list, Up and Down move the
/// selection and keep the focus there, but at its first or last entry the
/// focus leaves it; Page Up and Page Down move by the rows shown, and Home
/// and End go to its ends. Enter on the list opens the details page at
/// Compact, and moves the focus to the first enabled action at Regular and
/// Large. Escape clears a search that has the focus and holds words, else
/// leaves the details page, else closes. An open drop-down takes Up, Down,
/// Home, End, Enter, Space and Escape.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param pressed the key
/// @return what the host is to do
ScreenResult
library_key(Library& library, ScreenState& state, const kit::DisplayList& list, kit::Key pressed);

/// Takes typed text into the search while it has the focus, and searches
/// for it as it is typed.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param utf8 the typed text, in UTF-8
/// @return what the host is to do
ScreenResult library_text(
    Library& library, ScreenState& state, const kit::DisplayList& list, std::string_view utf8
);

/// Takes a turn of the wheel: it scrolls the pane under the pointer, or the
/// open drop-down's items.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param at the pointer, in the window's points
/// @param notches the turn; positive away from the player, towards the top
/// @return what the host is to do
ScreenResult library_wheel(
    Library& library, ScreenState& state, const kit::DisplayList& list, kit::Point at, float notches
);

/// Takes one of the Library's own keys or controller buttons.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param command what was pressed
/// @return what the host is to do
ScreenResult library_command(
    Library& library, ScreenState& state, const kit::DisplayList& list, ScreenCommand command
);

/// Takes the controller's right stick: it scrolls the pane that holds the
/// focus, the details while the focus is on one of their controls, the Active
/// tab's maps while that tab shows, and the list otherwise.
///
/// @param[in,out] library the model
/// @param[in,out] state the screen's state
/// @param list the display list the screen shows now
/// @param notches the stick's turn, as notches of the wheel; positive towards the top
/// @return what the host is to do
ScreenResult
library_stick(Library& library, ScreenState& state, const kit::DisplayList& list, float notches);

} // namespace oa::ui::library
