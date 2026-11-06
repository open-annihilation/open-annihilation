// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Entry points for the QuickPlay console, the Locator console and the
// Matchmaking dialog. The Main Frame, the Chat console and the battle
// console's matchmaking panel call them for an account that sees the
// feature. Each body is the empty console of that name until the screen
// that replaces it is in place; the names, the rectangle and the searching
// face stay.
#pragma once

#include <cstdint>

#include "oa/boneyards/toolkit.hpp"

namespace oa::boneyards::screens {
struct QuickPlay;
struct Locator;
} // namespace oa::boneyards::screens

namespace oa::boneyards::console {

struct StatusWidget;

// Window names of the three consoles. A check finds an open one by name.
inline constexpr const char* k_quickplay_console_name = "QuickPlay";
inline constexpr const char* k_locator_console_name = "Locator";
inline constexpr const char* k_matchmaking_dialog_name = "Matchmaking";

// Matchmaking dialog faces. 0 is the settings dialog, 1 the searching one.
inline constexpr int32_t k_matchmaking_settings = 0;
inline constexpr int32_t k_matchmaking_searching = 1;

// The dialog's place over Chat or Battle, in screen pixels.
namespace matchmaking_dialog_layout {
inline constexpr int32_t x = 150;
inline constexpr int32_t y = 140;
inline constexpr int32_t width = 340;
inline constexpr int32_t height = 250;
} // namespace matchmaking_dialog_layout

/// Builds the QuickPlay console in the Main Frame's info area.
///
/// The window is named QuickPlay and starts hidden. The Main Frame shows it
/// when the green covers the map, and shows the same window again the next
/// time. When gui/quickplay.gui cannot be read, an Ok box says QuickPlay
/// needs the Boneyards mod and no window is opened.
///
/// @param owner status widget the console belongs to
/// @param frame Main Frame window, the console's parent
/// @param x left edge in screen pixels
/// @param y top edge in screen pixels
/// @param width console width in pixels
/// @param height console height in pixels
/// @return the console window, or null when gui/quickplay.gui cannot be read
Window* quickplay_console_create(
    StatusWidget* owner, Window* frame, int32_t x, int32_t y, int32_t width, int32_t height
);

/// Returns the choices and the search behind a QuickPlay console.
///
/// @param console window from quickplay_console_create; another window returns null
/// @return the card, or null
screens::QuickPlay* quickplay_console_model(Window* console);

/// Steps the mocked search by milliseconds and updates the scanner, the timer and the planet list.
///
/// @param console QuickPlay console; null or another window does nothing
/// @param ms milliseconds to add; zero or negative leaves the clock
void quickplay_console_advance(Window* console, int32_t ms);

/// Ends a running QuickPlay search because this console is no longer showing.
///
/// An open offer is answered No, then the search is cancelled the way CANCEL
/// SEARCH cancels it, and the card shows FIND BATTLE. A Yes that is launching
/// a battle is left alone. A console that is not searching is left alone.
///
/// @param console QuickPlay console; null or another window does nothing
void quickplay_console_end_search(Window* console);

/// Builds the Locator console in the Main Frame's info area.
///
/// The window is the screen in gui/locator.gui. It is named Locator and starts
/// hidden. The file places the five searches, the name field and SEARCH, the
/// results list, and RELOCATE with PAGE. The console wires those controls to
/// the search. The Main Frame shows the same window again the next time.
/// When gui/locator.gui cannot be read, an Ok box says Locator needs the
/// Boneyards mod and no window is opened.
///
/// @param owner status widget the console belongs to; null builds the chrome only
/// @param frame Main Frame window, the console's parent
/// @param x left edge in screen pixels
/// @param y top edge in screen pixels
/// @param width console width in pixels
/// @param height console height in pixels
/// @return the console window, or null when gui/locator.gui cannot be read
Window* locator_console_create(
    StatusWidget* owner, Window* frame, int32_t x, int32_t y, int32_t width, int32_t height
);

/// Returns the search behind a Locator console.
///
/// @param console console from locator_console_create; null has none
/// @return the search, or null
screens::Locator* locator_console_state(Window* console);

/// True while the locator wants frequency rows delivered to it.
///
/// @param console locator console; null is false
/// @return whether a room-list row should come here instead of chat
bool locator_console_takes_rooms(const Window* console);

/// True while the selected battle's intel should be delivered here.
///
/// @param console locator console; null is false
/// @return whether intel should update the locator's detail
bool locator_console_takes_intel(const Window* console);

/// Delivers one frequency row to the locator.
///
/// @param console locator console; null is ignored
/// @param change 0 adds the row
/// @param area_id planet the row belongs to
/// @param room_id frequency id
/// @param name frequency name; null is skipped
/// @param needs_password whether the frequency asks for a password
/// @param users commanders in the frequency
/// @param type frequency type
/// @param last true for the planet's last row
/// @param global_known true when this row carried global
/// @param global the field's value; 1 is the sector-wide frequency
void locator_console_on_room(
    Window* console,
    int32_t change,
    uint32_t area_id,
    uint32_t room_id,
    const char* name,
    bool needs_password,
    int32_t users,
    int32_t type,
    bool last,
    bool global_known,
    int32_t global
);

/// Fills the selected battle's detail from its intel.
///
/// @param console locator console; null is ignored
/// @param game_id battle the intel describes
/// @param host_name the host's name; null reads as unknown
/// @param skill 0 novice, 1 seasoned, 2 expert
/// @param failed true leaves the detail as it was
void locator_console_on_intel(
    Window* console, const char* game_id, const char* host_name, int32_t skill, bool failed
);

/// Continues a server transfer once logout has finished.
///
/// @param console locator console; null is ignored
void locator_console_on_login_reset(Window* console);

/// Opens the Matchmaking dialog over the Main Frame.
///
/// The dialog shows the game type, skill, side and planet choices, the queue
/// meter and ACTIVATE. `searching` selects the settings face or the searching
/// face. Opening the settings face asks how many commanders are queued.
/// Opening the searching face joins the queue. A dialog already built is
/// raised, shown and given that face; opening settings does not leave the
/// queue. When gui/matchmaking.gui cannot be read, the dialog stays hidden
/// and an Ok box says Matchmaking needs the Boneyards mod.
/// Close hides it and, while a search is queued, MATCHMAKING blinks. The
/// window is a child of the Main Frame, so it closes with it.
///
/// @param owner status widget; null, or one without a Main Frame, opens nothing
/// @param searching k_matchmaking_settings or k_matchmaking_searching
/// @return the dialog, or null when it cannot be opened
Window* matchmaking_dialog_open(StatusWidget* owner, int32_t searching);

/// Returns the Matchmaking dialog, shown or hidden.
///
/// @param owner status widget; may be null
/// @return the dialog, or null when none has been opened
Window* matchmaking_dialog_find(const StatusWidget* owner);

/// Returns the face the Matchmaking dialog is showing.
///
/// @param dialog dialog from matchmaking_dialog_find; null reads as settings
/// @return k_matchmaking_searching or k_matchmaking_settings
int32_t matchmaking_dialog_searching(const Window* dialog);

/// Returns whether a search is still queued.
///
/// Closing the dialog leaves the search running. Opening the settings face
/// does not cancel it. DEACTIVATE leaves the queue.
///
/// @param dialog dialog from matchmaking_dialog_find; null is not queued
/// @return true while a search is queued
bool matchmaking_dialog_queued(const Window* dialog);

/// Shows the opponent-found box from the offer stored on the dialog.
///
/// Yes and No hide the box and answer the offer. The search itself opens this
/// box when an opponent is found.
///
/// @param dialog dialog from matchmaking_dialog_find; null shows nothing
void matchmaking_dialog_show_offer(Window* dialog);

/// Returns how many look, join, cancel and answer records the dialog has sent.
///
/// A record counts once, when the service accepts it. A service that is offline
/// or refuses the record does not count. Hosting and joining the battle do not count.
///
/// @param dialog dialog from matchmaking_dialog_find; null reads as 0
/// @return the number of records sent
int32_t matchmaking_dialog_requests(const Window* dialog);

/// Keeps a registered member with no side on the side prompt.
///
/// Called from the login console's pulse once the dossier has loaded. Opens
/// gui/side_prompt.gui when ALLEGIANCE is 'U' and the login is not a guest,
/// and leaves the Main Frame closed until a refreshed dossier names Arm or
/// Core. Choosing sends set_profile ALLEGIANCE and waits for the player's own
/// profile. EXIT returns to the main menu. A prompt already open stays up.
/// When the GUI file cannot be read, the login console stays and no other box
/// is offered.
///
/// @param owner status widget; null does not hold the login
/// @return true when the Main Frame must not open on this pulse
bool side_prompt_holds_login(StatusWidget* owner);

} // namespace oa::boneyards::console
