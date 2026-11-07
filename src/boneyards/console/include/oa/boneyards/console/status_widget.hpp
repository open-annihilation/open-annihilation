// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Boneyards status widget: the overlay BY1117a draws across the bottom of
// the Total Annihilation main menu.
//
// After MAINMENU.GUI is loaded the Boneyards client creates a 640x102 panel
// at y=378 painted with Main_Console.Gaf "Main_Menu_Overlay". It carries its
// own Single/Multi/Intro/Exit buttons (on top of the identically placed TA
// gadgets, and answering a click there themselves), an invisible credits
// hotspot, the "Enter Boneyards" button and an eight-lamp activity light.
// The TA-side effects of its buttons (sounds,
// cursor, frontend signals, discs, message boxes) and the online service are
// reached through the host tables below.
#pragma once

#include "oa/boneyards/screens/profile.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "oa/boneyards/console/images.hpp"
#include "oa/boneyards/html_fonts.hpp"
#include "oa/boneyards/service.hpp"
#include "oa/boneyards/toolkit.hpp"
#include "oa/present/surface.h"

namespace oa::boneyards::console {

// Screen geometry of the overlay and its children (absolute pixels).
namespace overlay_layout {
inline constexpr int32_t x = 0, y = 378, width = 640, height = 102;
inline constexpr int32_t menu_button_width = 96, menu_button_height = 20;
inline constexpr int32_t left_column_x = 82, right_column_x = 464;
inline constexpr int32_t top_row_y = 393, bottom_row_y = 430;
inline constexpr int32_t credits_x = 295, credits_y = 454, credits_width = 50, credits_height = 24;
inline constexpr int32_t enter_x = 260, enter_y = 412, enter_width = 45, enter_height = 28;
inline constexpr int32_t light_x = 307, light_y = 418, light_width = 64, light_height = 14;
} // namespace overlay_layout

// Resource names used by the overlay (anims/ and fonts/ archives of by.ccx).
namespace resource {
inline constexpr const char* main_console_gaf = "Main_Console.Gaf";
inline constexpr const char* overlay_sequence = "Main_Menu_Overlay";
inline constexpr const char* single_sequence = "MM_Single_Button";
inline constexpr const char* multi_sequence = "MM_Multi_Button";
inline constexpr const char* intro_sequence = "MM_Intro_Button";
inline constexpr const char* exit_sequence = "MM_Exit_Button";
inline constexpr const char* enter_sequence = "MM_Enter_Button";
inline constexpr const char* activity_light_sequence = "Activity_Light";
// Transparent palette index passed with every console graphic.
inline constexpr uint8_t console_key = 9;
} // namespace resource

// Named sound flags played by the TA frontend (core_set_named_flag names).
namespace menu_sound {
inline constexpr const char* big_button = "BigButton";
inline constexpr const char* small_button = "smlButton";
inline constexpr const char* exit = "exit";
} // namespace menu_sound

// Message box texts shown by the overlay.
namespace menu_message {
inline constexpr const char* multiplayer_disc_required =
    "Please insert the Multiplayer CD (Disc 1) and try again";
inline constexpr const char* fullscreen_required =
    "Debug:  You must be in full-screen mode to play a movie";
inline constexpr const char* game_disc_required =
    "Please insert a Total Annihilation CD and try again";
inline constexpr const char* internet_required =
    "Please connect to the Internet before entering the Boneyards";
inline constexpr const char* rover_missing =
    "Boneyards could not be loaded. The Rover client is missing or corrupted.";
inline constexpr const char* rover_attach_failed =
    "Boneyards could not be attached. Exit other running programs and try again.";
inline constexpr const char* rover_activate_failed =
    "Boneyards could not be activated. Exit other running programs and try again.";
// The box closing the window opens while the Boneyards is up.
inline constexpr const char* exit_to_windows = "Exit to the system?";
} // namespace menu_message

// The sound the Boneyards drones on the alternate route while its background
// sound is on (BYStopThatIncessantDroning unset).
inline constexpr const char* k_background_sound = "BGM";

// What the Boneyards shows when its launch_game reply refuses a battle, by
// the reply's result: 1 to 8, and any other.
namespace launch_message {
inline constexpr const char* wrong_player_type =
    "INTERNAL:  the player i`s not the correct type to start/join the requested type of game in "
    "that battle area";
inline constexpr const char* refused[] = {
    "User already in game",
    "INTERNAL:  bad area id or user in incorrect area",
    "INTERNAL:  failed to create the game info structure or add it to the gaem dictionary",
    "INTERNAL:  some options specified by the user did not match the requested game type",
    "Wrong password for game",
    "Game is full",
    wrong_player_type,
    "Your account has been restricted from this type of game.",
};
inline constexpr const char* unknown = "INTERNAL:  Unknown failure";
} // namespace launch_message

// Frontend values the overlay writes (same numbering as the TA dispatcher).
namespace frontend_value {
inline constexpr uint8_t signal_single_player = 5;
inline constexpr uint8_t signal_multiplayer = 6;
inline constexpr uint8_t signal_exit_application = 8;
inline constexpr uint8_t signal_credits = 9;
inline constexpr uint8_t state_intro = 1;
inline constexpr uint8_t state_main_menu = 2;
inline constexpr uint8_t state_boneyards = 7;
// The connection selection a launched battle goes on in.
inline constexpr uint8_t state_connection_selection = 16;
inline constexpr uint8_t signal_initialize = 0;
inline constexpr int32_t menu_cursor = 20;
// Application flag bit set while running full screen.
inline constexpr uint8_t app_flag_fullscreen = 0x2;
inline constexpr uint32_t disc_campaign = 0;
inline constexpr uint32_t disc_multiplayer = 1;
} // namespace frontend_value

struct StatusWidget;

// TA frontend services the overlay calls. Every entry may be null (no-op /
// false).
struct MainMenuHost {
    void* user;
    // Plays a named frontend sound.
    void (*play_sound)(void* user, const char* name);
    // Registers the sounds/ WAV `file` under the sound name `category`
    // unless the name is taken.
    void (*register_sound)(void* user, const char* category, const char* file);
    // Selects the cursor animation by table index.
    void (*select_cursor)(void* user, int32_t index);
    // Rescans the archives before the multiplayer menu.
    void (*prepare_multiplayer)(void* user);
    // Loads maps/multiplay.tdf; false when it is missing.
    bool (*load_multiplayer_document)(void* user);
    // Called after a successful multiplayer document load.
    void (*after_multiplayer_selection)(void* user);
    // Display/application flag byte (frontend_value::app_flag_*).
    uint8_t (*application_flags)(void* user);
    // True when the given disc is in a drive.
    bool (*find_disc)(void* user, uint32_t disc);
    bool (*shift_key_down)(void* user);
    // Records whether shift was held when the intro was requested.
    void (*store_intro_shift)(void* user, bool held);
    void (*drain_input)(void* user);
    // Writes the pending frontend signal byte.
    void (*set_pending_signal)(void* user, uint8_t signal);
    // Writes both the current and the pending frontend signal bytes.
    void (*set_signal)(void* user, uint8_t signal);
    void (*set_frontend_state)(void* user, uint8_t state);
    // Re-evaluates the TA gadgets after a signal change.
    void (*update_gadgets)(void* user);
    // Opens the TA modal message box with `text` and clears its selection.
    void (*show_message_box)(void* user, const char* text);
    // Reloads the TA main-menu panel (MAINMENU.GUI) after leaving the Boneyards.
    void (*init_main_menu_panel)(void* user);
    // The game's GUI clock, which ticks 30 times a second. Null steps the
    // windows on every tick call.
    uint32_t (*current_tick)(void* user);
    // True while the press being routed belongs to the MAINMENU.GUI gadget
    // under Single, Multi, Intro, Exit or Credits: its TA handler performs
    // the button's action and the overlay only shows the press.
    bool (*press_taken_below)(void* user);
    // The pointer in frame pixels; false leaves the Boneyards cursor undrawn.
    bool (*pointer_position)(void* user, int32_t* x, int32_t* y);
    // Posts a line to the message log of the battle the Boneyards launched
    // and raises bit 0 of Game.gui_flags.
    void (*battle_message)(void* user, const char* text);
    // Receives the battle a launch_game reply starts, with the player's
    // Boneyards name: the game's online launch block is filled from it and
    // the name becomes the frontend Game's nickname.
    void (*battle_launched)(void* user, const LaunchReport* launch, const char* user_name);
    // Null allows the launch. False does not enter the battle; when `text`
    // is non-null it receives the refusal.
    bool (*launch_permitted)(void* user, const char** text) = nullptr;
    // Hears that the battle a launch_game reply started no longer counts as
    // launched (ConsoleSystem.chat_console_pending cleared): the Boneyards
    // took the player back, or its session went back to the main menu.
    void (*launch_ended)(void* user);
    // Runs one pass of the frontend dispatcher once the current event or
    // frame has been handled, so that the state and signal just written take
    // effect (after a launch, the connection selection's initialize).
    void (*run_frontend)(void* user);
    // Tells the Boneyards that the player left the battle it launched.
    void (*left_game)(void* user, uint32_t area_id, const char* game_id);
    // Sets the flags the online client sends with its latency replies.
    void (*set_latency_reply_flags)(void* user, uint32_t flags);
    // Empties the frontend Game's chat ring (Game.chat_head, chat_tail).
    void (*empty_chat_ring)(void* user);
    // Stops every playing sound, the one on the alternate route included.
    void (*stop_sounds)(void* user);
    // Plays a named sound on the alternate route, which streams and repeats
    // it in place of whatever that route plays.
    void (*play_sound_alternate)(void* user, const char* name);
    // Leaves the game and ends the program, showing the disconnect reason
    // first when there is one, as YES in the "Exit to the system?" box does.
    void (*leave_game)(void* user);
    // ENTER, the B key and "-y" ask this instead of opening at once. Null
    // keeps the network check and opens as before. The widget is the one
    // whose ENTER was pressed.
    void (*request_entry)(StatusWidget* widget) = nullptr;
};

// What a battle the Boneyards launches needs of the application that shows
// the Boneyards, which installs it once. Every entry may be null or 0.
struct LaunchBridge {
    void* user{};
    // Fills the online launch block from the launch reply and makes the
    // player's Boneyards name the frontend Game's nickname.
    void (*launched)(void* user, const LaunchReport* launch, const char* user_name){};
    // Null allows the launch. False does not enter the battle
    // (MainMenuHost::launch_permitted).
    bool (*launch_permitted)(void* user, const char** text){};
    // The launched battle no longer counts as launched
    // (MainMenuHost::launch_ended).
    void (*launch_ended)(void* user){};
    // The online launch block's connection type.
    int32_t (*connection_type)(void* user){};
    // Empties the frontend Game's chat ring.
    void (*empty_chat_ring)(void* user){};
    // Posts a line to the launched battle's message log.
    void (*battle_message)(void* user, const char* text){};
    // Leaves the game and ends the program with the disconnect reason
    // (MainMenuHost::leave_game).
    void (*leave_game)(void* user){};
};

// Creates the console screen `screen` (see the Main Frame buttons) inside the
// Main Frame's info area. A null `create` selects the built-in Boneyards
// screen views, which navigate with boneyards_open; a null result leaves the
// area empty.
struct ConsoleScreenFactory {
    void* user;
    Window* (*create)(
        void* user,
        int32_t screen,
        Window* frame,
        int32_t x,
        int32_t y,
        int32_t width,
        int32_t height
    );
};

// Console screen ids selected by the Main Frame buttons. QuickPlay and
// Locator are the consoles those buttons open for an account that sees
// them. none is not a screen.
namespace console_screen {
inline constexpr int32_t update = 0;
inline constexpr int32_t login = 1;
inline constexpr int32_t recon = 3;
inline constexpr int32_t chat = 4;
inline constexpr int32_t battle = 5;
inline constexpr int32_t ladders = 7;
inline constexpr int32_t news = 8;
inline constexpr int32_t profile = 9;
inline constexpr int32_t help = 10;
inline constexpr int32_t quickplay = 11;
inline constexpr int32_t locator = 12;
inline constexpr int32_t none = 13;
} // namespace console_screen

inline constexpr int32_t k_console_button_count = 9;

// Order of the Main Frame's console buttons.
enum class ConsoleButton : int32_t {
    release, // closes the open console screen
    recon,
    chat,
    battle,
    news,
    ladders,
    locator,
    profile,
    help,
};

// Main Frame geometry (absolute pixels).
namespace main_frame_layout {
inline constexpr int32_t exit_x = 19, exit_y = 46, exit_width = 45, exit_height = 28;
inline constexpr int32_t quickplay_x = 532, quickplay_y = 89, quickplay_width = 87,
                         quickplay_height = 20;
inline constexpr int32_t light_x = 66, light_y = 52, light_width = 64, light_height = 14;
inline constexpr int32_t banner_x = 152, banner_y = 18, banner_width = 468, banner_height = 60;
// Console screen area (the galaxy map panel).
inline constexpr int32_t info_x = 23, info_y = 119, info_width = 498, info_height = 328;
// The green Console layer inside the map panel; console screens open in it.
inline constexpr int32_t console_x = 0x17, console_y = 0x7e, console_width = 0x1f2,
                         console_height = 0x139;
} // namespace main_frame_layout

struct StatusWidgetConfig {
    FileReader files; // by.ccx resources, anims/ and fonts/ (and TA's anims/)
    ShadeTables shade;
    // Palette used to map widget RGB colours to indexes; null disables text
    // and colour services (images still draw).
    const Palette* palette;
    // Host text service for markup pages; without one they are set in the
    // FNT fonts.
    html::FontService text_fonts;
    MainMenuHost host;
    BoneyardsService service; // null entries fall back to offline behaviour
    ConsoleScreenFactory screens;
    screens::SettingsStore settings{};
    // The main menu leaves the screen models unbuilt until the player enters.
    // A widget built for a test leaves this false, so the models exist as soon
    // as the widget does.
    bool defer_models = false;
};

struct StatusWidget;

/// Names the analogue atlas file the console reads when the Boneyards window opens.
///
/// OA_FONT_ATLASES_DIR overrides this path. A missing file is reported once
/// and the faces' own glyphs still draw. The main-menu strip does not open it.
///
/// @param path full path of boneyards-analogues.bin; null or empty clears it
void boneyards_status_widget_set_analogue_atlas_path(const char* path);

/// Names the calls that open and close the page fonts with the Boneyards window.
///
/// `acquire` runs as the window opens, before its pages are built. `release`
/// runs once the window's pages have been destroyed. Either may be null.
/// The main menu does not call them.
///
/// @param acquire opens the page fonts for one window
/// @param close_fonts closes them after that window is gone
void boneyards_status_widget_set_font_lifetime(
    void (*acquire)(StatusWidget* widget), void (*close_fonts)(StatusWidget* widget)
);

/// Gives one widget the text service its markup pages open.
///
/// @param widget status widget; null does nothing
/// @param fonts host text service; a service with no `open` leaves the pages in the FNT fonts
void boneyards_status_widget_set_text_fonts(StatusWidget* widget, const html::FontService& fonts);

/// Creates the console state, installs its image service and shows the overlay as the main menu is
/// entered.
///
/// The war-map graphics, the analogue atlas and the page fonts stay unloaded
/// until the Boneyards window opens. The graphics stay null when any of them
/// is missing.
///
/// @param config files, palette, host tables, service and screen factory; null service entries
///     fall back to offline behaviour
/// @return the widget, or null when the overlay graphic cannot be loaded
StatusWidget* boneyards_status_widget_create(const StatusWidgetConfig& config);

/// Releases the Boneyards GUI, detaches from the service's events and frees the widget.
///
/// @param widget widget to destroy; null does nothing
void boneyards_status_widget_destroy(StatusWidget* widget);

/// Releases the Boneyards GUI as the frontend leaves the main menu; the entry path counts as not
/// taken.
///
/// @param[in,out] widget status widget; null does nothing
void boneyards_status_widget_leave_main_menu(StatusWidget* widget);

/// Shows the overlay again, with no connection pending, as the frontend returns to the main menu.
///
/// Both frontend signal bytes become 1 and the entry path counts as not
/// taken.
///
/// @param[in,out] widget status widget; null does nothing
void boneyards_status_widget_return_to_main_menu(StatusWidget* widget);

/// Routes one input message to the Boneyards windows.
///
/// Control, navigation and modifier keys are tracked as held; typed
/// characters are single messages with no release.
///
/// @param[in,out] widget status widget; null or one without a GUI consumes nothing
/// @param message input message
/// @return true when a window consumed it
bool boneyards_status_widget_event(StatusWidget* widget, const Message& message);

/// Gives typed text to the focused edit box.
///
/// The text is inserted as the player typed it. A focus that is not an edit
/// box changes nothing.
///
/// @param[in,out] widget status widget; null does nothing
/// @param text typed text; null does nothing
/// @return true when an edit box had the focus
bool boneyards_status_widget_take_text(StatusWidget* widget, const char* text);

/// Shows characters being composed in the focused edit box.
///
/// They are drawn at the caret and are not part of the field's text. A focus
/// that is not an edit box changes nothing. Null or empty clears them.
///
/// @param[in,out] widget status widget; null does nothing
/// @param text characters being composed; null clears them
void boneyards_status_widget_set_composition(StatusWidget* widget, const char* text);

/// Reports the focused edit box's place and the caret the player sees.
///
/// @param widget status widget; null or one with no edit focused returns false
/// @param[out] x left edge in screen pixels; may be null
/// @param[out] y top edge in screen pixels; may be null
/// @param[out] width field width in pixels; may be null
/// @param[out] height field height in pixels; may be null
/// @param[out] cursor caret distance from the field's left, in pixels; may be null
/// @return true when an edit box has the focus
bool boneyards_status_widget_text_field(
    const StatusWidget* widget,
    int32_t* x,
    int32_t* y,
    int32_t* width,
    int32_t* height,
    int32_t* cursor
);

// What a gamepad button does in the Boneyards. The directions move the focus.
// Accept and Menu press the focused control. Back leaves a screen. Previous
// and Next move as Shift+Tab and Tab.
enum class PadPress : int32_t {
    up,
    down,
    left,
    right,
    accept,
    back,
    menu,
    previous,
    next,
};

/// Moves the focus or presses the focused control the way a gamepad button does.
///
/// With nothing focused, a direction, Accept or Menu lands on the first
/// control and does not press it.
///
/// @param[in,out] widget status widget; null consumes nothing
/// @param press the gamepad button
/// @return true when a control took the press
bool boneyards_status_widget_pad(StatusWidget* widget, PadPress press);

/// Records Shift, Control, Alt and the system key (Command or Windows) as held.
///
/// They stay held until this is called again. The engine's Ctrl, Alt and
/// system modifier values are applied through this.
///
/// @param[in,out] widget status widget; null does nothing
/// @param shift true while Shift is held
/// @param control true while Control is held
/// @param alt true while Alt is held
/// @param system true while the system key is held
void boneyards_status_widget_modifiers(
    StatusWidget* widget, bool shift, bool control, bool alt, bool system
);

/// Holds modifier keys for the next input event, then puts them back.
///
/// A modifier that is already held stays held. One that this call adds is
/// released again after that event, unless the event itself is that key.
///
/// @param[in,out] widget status widget; null does nothing
/// @param shift true when this event's Shift modifier is set
/// @param control true when this event's Control modifier is set
/// @param alt true when this event's Alt modifier is set
/// @param system true when this event's system modifier is set
void boneyards_status_widget_event_modifiers(
    StatusWidget* widget, bool shift, bool control, bool alt, bool system
);

/// Hears when a text field gains or loses the keyboard focus.
///
/// `place` is that field, in screen pixels, so an on-screen keyboard can
/// stand clear of it. Null means the focus left every text field and the
/// keyboard should close.
///
/// @param[in,out] widget status widget; null does nothing
/// @param callback called with the field, or null to close; null stops hearing
/// @param user pointer passed back to `callback`
void boneyards_status_widget_set_screen_keyboard(
    StatusWidget* widget,
    void (*callback)(void* user, const oa::boneyards::TextFieldPlace* place),
    void* user
);

/// Runs the per-frame upkeep: the service's pump, then the windows and the cursor.
///
/// The pump runs every call (its events reach the screens); the windows
/// (activity light, pending enter-button release) and the cursor step once
/// per changed clock tick. Windows marked for destruction are freed.
///
/// @param[in,out] widget status widget; null does nothing
void boneyards_status_widget_tick(StatusWidget* widget);

/// Draws the Boneyards windows and the cursor into `target` while the desktop has any.
///
/// 3.1c draws the cursor into the UI MAIN overlay after composing it, so it
/// reaches the screen with the next frame, under that frame's windows; the
/// native store draws straight into the frame, so the cursor goes first.
///
/// `origin_y` is subtracted from every window and from the cursor for this
/// draw, then put back. A target that holds only the strip, with `origin_y`
/// at the strip's top, receives the pixels the strip writes. A message box
/// sits above the strip, so that draw passes 0 and a full frame.
///
/// @param[in,out] widget status widget; null does nothing
/// @param[in,out] target frame the windows are drawn into
/// @param origin_y rows above the target's top, in screen pixels
void boneyards_status_widget_draw(StatusWidget* widget, Surface* target, int32_t origin_y = 0);

/// Shows a message box over the main-menu overlay and dims the overlay while it is open.
///
/// The host's box is used when it supplies one, otherwise a Boneyards
/// message box.
///
/// @param[in,out] widget status widget; null or one without an overlay does nothing
/// @param text message
void boneyards_status_widget_show_message(StatusWidget* widget, const char* text);

/// Opens the overlay's own message box, without dimming the overlay.
///
/// The box restores the overlay when it closes. A host message hook opens
/// this box; the dimming stays with the overlay.
///
/// @param[in,out] widget status widget; null or one without an overlay does nothing
/// @param text message; null does nothing
void boneyards_status_widget_open_message(StatusWidget* widget, const char* text);

/// Tells whether every host entry the main-menu overlay calls is set.
///
/// @param host the overlay's host table
/// @return true when each entry the overlay calls is set
bool main_menu_overlay_host_connected(const MainMenuHost& host);

/// Tells whether a Boneyards message box is open; it takes all input.
///
/// @param widget status widget; may be null
/// @return true while a box is open
bool boneyards_status_widget_message_open(StatusWidget* widget);

/// Finds the centre of a button of the open Boneyards message box, for checks.
///
/// @param widget status widget; may be null
/// @param name button name (message_box_name's yes, no, ok or cancel)
/// @param[out] x centre in frame pixels
/// @param[out] y centre in frame pixels
/// @return false when no box is open or the open one has no such button
bool boneyards_status_widget_message_button(
    StatusWidget* widget, const char* name, int32_t* x, int32_t* y
);

/// Restores the overlay after the host's message box is dismissed.
///
/// @param[in,out] widget status widget; null does nothing
void boneyards_status_widget_dialog_closed(StatusWidget* widget);

/// Tells whether the main-menu overlay is shown (false after entering the Boneyards).
///
/// @param widget status widget; may be null
/// @return true while the overlay is shown
bool boneyards_status_widget_visible(const StatusWidget* widget);

/// Tells whether the Boneyards Main Frame is shown instead of the overlay.
///
/// @param widget status widget; may be null
/// @return true while the Main Frame is shown
bool boneyards_status_widget_in_boneyards(const StatusWidget* widget);

/// Tells whether a battle launched from the Boneyards is under way (ConsoleSystem.chat_console_pending).
///
/// @param widget status widget; may be null
/// @return true from the battle launch until the return to the Boneyards; false without the widget
bool boneyards_status_widget_launched_game(const StatusWidget* widget);

/// Returns the planet of the battle the Boneyards launched, and that battle's game id.
///
/// The planet is the one the launch reply named. It stays that planet after
/// the Main Frame selects another. Before a launch is accepted it is 0.
///
/// @param widget status widget; may be null
/// @param[out] area_id the launch reply's planet; 0 before a launch is accepted
/// @param[out] game_id the launch reply's game id, "" before any launch
/// @return false without the widget
bool boneyards_status_widget_battle_ids(
    const StatusWidget* widget, uint32_t* area_id, const char** game_id
);

/// Returns the planet's name and the sector's name of the battle the Boneyards launched.
///
/// They are the galactic map's names for the planet the launch reply named
/// and for the sector it showed, read when the reply came, so selecting
/// another planet later does not change them. Each is "" when the map held
/// no such planet, or had no sector loaded.
///
/// @param widget status widget; may be null
/// @param[out] planet the planet's name; "" before a launch is accepted
/// @param[out] sector the sector's name; "" before a launch is accepted
/// @return false without the widget
bool boneyards_status_widget_battle_place(
    const StatusWidget* widget, const char** planet, const char** sector
);

/// Tells whether the network the Boneyards needs is there (the main menu's entry check).
///
/// @param widget status widget; may be null
/// @return the service's answer; false without the widget
bool boneyards_status_widget_network_available(const StatusWidget* widget);

/// What the update screen's checks found. A flag left clear was not performed.
struct UpdateCheckFacts {
    bool services_checked{false};
    bool services_on{false};
    bool files_checked{false};
    std::string mismatched_file{};
    std::vector<std::string> matched_files{};
    bool feed_checked{false};
    bool feed_failed{false};
    bool update_offered{false};
    std::string offered_version{};
    bool update_declined{false};
    bool extension_checked{false};
    /// The fetch's percent, 0 to 100. -1 when no fetch is showing.
    int32_t extension_percent{-1};
    bool extension_installed_now{false};
    bool extension_current{false};
    std::string extension_version{};
    /// A later step failed. `failure` is the sentence the entry produced.
    bool entry_failed{false};
    std::string failure{};
    /// The entry is ready to open. Sets the picture finished without adding
    /// a line for a check that did not run.
    bool entry_ready{false};
};

/// One line of the update screen.
struct UpdateCheckLine {
    std::string text{};
    bool closing{false}; ///< drawn in the bright colour
};

/// The lines and the bar for one moment of the update screen.
struct UpdateCheckPicture {
    std::vector<UpdateCheckLine> lines{};
    int32_t fill{0}; ///< thousandths of the track, 0 to 1000
    bool stop{false};
    bool asking{false};
    bool finished{false};
};

/// Builds the update screen's lines from checks that were actually performed.
///
/// A flag left clear adds no line. Services off is the only line, and later
/// flags are ignored. The lines only grow as later flags become set.
///
/// @param facts the checks that ran
/// @return the lines, the bar, and whether the entry stops, asks or finishes
UpdateCheckPicture boneyards_update_picture(const UpdateCheckFacts& facts);

/// Names the eight sounds the Boneyards' consoles play to the host.
///
/// GREEN_SLIDE, MESSAGE, GALACTIC_BUTTON and the rest each name a WAV of
/// the game's sounds/ folder. The overlay names them once it is built, and
/// the Main Frame build names them again, so a console opened from any path
/// plays them. A name the host already holds keeps its sound.
///
/// @param widget status widget; null does nothing
void boneyards_status_widget_register_sounds(const StatusWidget* widget);

/// Starts the real update screen on this widget, before its console is built.
///
/// The placeholder pass is not used after this. Lines set before the console
/// opens are the ones it shows.
///
/// @param[in,out] widget status widget; null does nothing
void boneyards_status_widget_begin_real_update(StatusWidget* widget);

/// Copies a picture onto the real update screen.
///
/// Does nothing until begin_real_update. The list the console already drew
/// keeps its earlier rows; only new lines are appended on the next pulse.
///
/// @param[in,out] widget status widget; null does nothing
/// @param picture the lines and the bar
void boneyards_status_widget_set_real_update(
    StatusWidget* widget, const UpdateCheckPicture& picture
);

/// Enters the Boneyards as ENTER BONEYARDS does, without its network check: the entry "-y" takes.
///
/// Starts the GUI when it is down and activates the service. When the
/// service cannot be activated, the frontend state stays 2 and the main menu
/// says why; otherwise the frontend state becomes 7, the main menu comes down
/// and the Main Frame opens. Either way the signal becomes 1.
///
/// @param[in,out] widget status widget; null does nothing
void boneyards_status_widget_enter(StatusWidget* widget);

/// Returns from a battle the Boneyards launched to the Boneyards, as the main menu does when it opens.
///
/// Tells the service the player left the battle on the planet the launch
/// named, starts the GUI when it is down and opens the Main Frame while the
/// battle still counts as launched, so a debriefing room reopens the Chat
/// console in that room and otherwise the battle console opens again; then
/// the battle no longer counts as launched, every playing sound stops, the
/// frontend state becomes 7 and the signal 1.
///
/// @param[in,out] widget status widget; null does nothing
void boneyards_status_widget_return_from_battle(StatusWidget* widget);

/// Opens a Boneyards message box on the Boneyards desktop, shown with the Boneyards screens.
///
/// While the GUI is down (the main menu has been left for the game list) the
/// text is kept and opened by the return from the battle, once the console
/// is up again.
///
/// @param[in,out] widget status widget; null does nothing
/// @param text message
void boneyards_status_widget_show_notice(StatusWidget* widget, const char* text);

/// Runs the Boneyards part of the game's idle work, once every frame.
///
/// Pumps the service when `pump` asks. While the GUI is up, a NO in the
/// "Exit to the system?" box is forgotten, or else a YES in it leaves the game
/// through MainMenuHost::leave_game. While the GUI is up and a launched
/// battle has only just started, the frontend Game's chat ring is emptied
/// and the latency reply flags are set to 0; the battle stops counting as
/// just started at the next once-a-second check.
///
/// @param[in,out] widget status widget; null does nothing
/// @param now_ms a millisecond clock
/// @param pump whether to pump the service this frame (the overlay's tick has not)
void boneyards_status_widget_idle(StatusWidget* widget, uint32_t now_ms, bool pump);

/// Lists the controls the widget is showing, for a program driving the game.
///
/// The widget's own services are installed for the call and restored after
/// it, so a caption reads as the player sees it. A null widget lists nothing.
/// The visit must not change the widget.
///
/// @param widget status widget; null does nothing
/// @param context passed to `visit`
/// @param visit called once per shown control; null does nothing
void boneyards_status_widget_visit_driver_controls(
    const StatusWidget* widget, void* context, DriverControlVisitor visit
);

/// Tells whether the Boneyards is up: its GUI runs and its entry path ran (Enter Boneyards, or the
/// return from a battle it launched), until the main menu takes its place again.
///
/// @param widget status widget; may be null
/// @return true while the Boneyards is up; false without the widget
bool boneyards_status_widget_up(const StatusWidget* widget);

/// Opens the Boneyards box "Exit to the system?" with YES and NO, as a request to close the window does
/// while the Boneyards is up outside a battle it launched.
///
/// Each request opens a box of its own. The idle work acts on the answer
/// (boneyards_status_widget_idle): YES leaves the game, NO only closes the
/// box.
///
/// @param[in,out] widget status widget; null or one whose GUI is down opens nothing
/// @return true when the box opened
bool boneyards_status_widget_ask_exit(StatusWidget* widget);

/// Stops every playing sound, as the Boneyards does when a joiner's host is not found.
///
/// @param widget status widget; null does nothing
void boneyards_status_widget_stop_sounds(const StatusWidget* widget);

// Test/inspection access to the overlay's windows.
enum class OverlayPart : int32_t {
    panel,
    single,
    multi,
    intro,
    exit,
    credits,
    enter,
    activity_light,
};

/// Returns one of the overlay's windows, for tests and inspection.
///
/// @param widget status widget; may be null
/// @param part window asked for
/// @return the window, or null without the widget or its overlay
Window* boneyards_status_widget_part(StatusWidget* widget, OverlayPart part);

/// Returns the widget's console image store.
///
/// @param widget status widget; may be null
/// @return the store, or null without the widget
ConsoleImages* boneyards_status_widget_images(StatusWidget* widget);

// A window's bounds in screen pixels.
struct ScreenRect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

inline constexpr std::size_t k_probe_ticker_capacity = 0x100;

// What checks read of the open Main Frame.
struct MainFrameProbe {
    int32_t connection_level; // session level (SessionLevel values)
    // The login name the session holds. Once the dossier has arrived this is
    // the name the server returned, in the server's spelling.
    char user_name[kSessionNameCapacity];
    int32_t console_screen; // console_screen id in the info area
    bool screen_shown;      // that console screen is shown
    // The green layer over the map.
    bool layer_sliding;
    int32_t layer_direction; // 0 slides in, 1 slides out
    bool layer_covering;
    int32_t layer_countdown; // pulses left in the slide
    int32_t slide_steps;
    // The galactic map.
    std::size_t planet_count;
    std::size_t planets_drawn; // visible planets whose viewer anchor shows their sprite
    bool galaxy_loading;
    bool map_locked;
    int32_t selected_planet; // -1 when none
    bool unlock_pending;     // the map unlocks once the galaxy data has loaded
    // Console button states (button_state values).
    int32_t release_button;
    int32_t recon_button;
    int32_t chat_button;
    int32_t battle_button;
    bool activity_light_running;
    // The news ticker's text and what its label shows of it.
    char ticker_text[k_probe_ticker_capacity];
    char ticker_shown[k_probe_ticker_capacity];
    // The planet summary beside the Recon button.
    bool summary_shown;     // its planet name label shows
    int32_t summary_planet; // the planet it summarises, -1 when none
    // Pixels of the summary's picture area right of its labels (x 0x24D..0x26D,
    // y 0x7E..0x9C) in the frame's background that are not the 0xAF fill.
    std::size_t summary_picture_pixels;
    // What the summary's labels read; the wins row shows for a contested
    // planet.
    char summary_name[16];
    char summary_owner[16];
    char summary_battles[12];
    char summary_forming[12];
    bool summary_wins_shown;
    char summary_arm_wins[12];
    char summary_core_wins[12];
    // The selected planet as the war map names it and the allegiance feed
    // assigns it; "" and 0 without a selection.
    char selected_name[16];
    char selected_side;
    ScreenRect console_buttons[k_console_button_count]; // in ConsoleButton order
    uint32_t selected_area_id; // the session's current area; 0 until a planet is picked
    // The selected planet's id and census fields; all 0 when none is selected.
    CensusReport selected_census;
    // Census requests for the current area the Main Frame has made: planet
    // presses, keep-alives and Recon opens.
    uint32_t census_requests;
    // The open Recon console: whether it shows, what its Planet, Gravity and
    // Population fields read, whether it scans a panorama (otherwise the map
    // static) and the panorama's scroll position.
    bool recon_shown;
    char recon_planet_text[16];
    char recon_gravity_text[12];
    int32_t recon_population;
    bool recon_panorama;
    int32_t recon_pan_scroll;
    // Lines in the open Chat console's log and the text of the newest; 0
    // and "" while no Chat console is open.
    int32_t chat_lines;
    char chat_last_line[0x100];
    // The frequency the chat session is in (0 when none) and the members and
    // frequencies the chat console lists.
    uint32_t chat_room_id;
    int32_t chat_members;
    int32_t chat_frequencies;
    // Rows the open Chat console's frequency and player lists show; 0 while
    // no Chat console is open.
    int32_t chat_frequency_rows;
    int32_t chat_player_rows;
    // The frequency an entry or a switch awaits; 0 when none.
    uint32_t chat_pending_room_id;
    // The open Chat console's CREATE button; all 0 while none is open.
    ScreenRect chat_create_button;
    // The chat console's player pane: the player it holds (0 when none) and
    // the name and clan it shows from the player's record ("" until the
    // reply has been read).
    uint32_t chat_pane_user_id;
    char chat_pane_name[0x10];
    char chat_pane_clan[0x40];
    // The open Profile console's Profile.def description of its tab; ""
    // while no Profile console is open.
    char profile_tab_description[0x80];
    // The open Profile console: whether it still waits for the player's
    // record, and the login name the record gives ("" until then).
    bool profile_receiving;
    char profile_login_name[0x10];
    // The open Profile console's view: whether it shows, the rows of its
    // ProfileList, the tab of its selected row (-1 none), its first
    // description line, and its status line; all false, 0 and "" while none
    // is open.
    bool profile_shown;
    int32_t profile_rows;
    int32_t profile_row_tab;
    char profile_description_line[0x40];
    bool profile_status_shown;
    char profile_status[0x40];
    // The open Help, Ladders or News console: the rows its contents list
    // shows, whether its view still shows the waiting page, the locator of
    // the page it opened last (a fetched page's URL) and the images of the
    // shown page not loaded; 0, false and "" while none is open.
    int32_t contents_rows;
    bool contents_waiting;
    char contents_page[0x100];
    int32_t contents_images_missing;
};

/// Fills a probe from the open Main Frame for checks.
///
/// @param widget status widget; may be null
/// @param[out] out probe filled from the Main Frame, its screens and the chat and profile models
/// @return false while no Main Frame is open
bool boneyards_status_widget_probe(const StatusWidget* widget, MainFrameProbe* out);

/// Finds the centre of the PlanetHotSpot over a map planet.
///
/// @param widget status widget; may be null
/// @param planet planet index in the galaxy view
/// @param[out] x centre in frame pixels
/// @param[out] y centre in frame pixels
/// @return false when the planet has no shown hotspot or no Main Frame is open
bool boneyards_status_widget_planet_point(
    const StatusWidget* widget, std::size_t planet, int32_t* x, int32_t* y
);

/// Finds the centre of the PlanetHotSpot over the planet with an area id.
///
/// @param widget status widget; may be null
/// @param area_id planet id
/// @param[out] x centre in frame pixels
/// @param[out] y centre in frame pixels
/// @return false when no planet has that id or it has no shown hotspot
bool boneyards_status_widget_planet_hotspot(
    const StatusWidget* widget, int32_t area_id, int32_t* x, int32_t* y
);

// The open Chat console's lists of the planet's frequencies and of the
// entered frequency's players.
enum class ChatList : int32_t {
    frequencies,
    players,
};

// What a row of one of those lists shows.
struct ChatRowProbe {
    bool shown; // in view, so a press at its centre lands on it
    int32_t x;  // its centre in frame pixels
    int32_t y;
    uint32_t id;   // the frequency's room id or the player's user id
    int32_t type;  // the frequency's room type; 0 for a player
    bool selected; // its list's selected row
    bool current;  // a frequency row circled as the session's frequency
    char name[0x40];
    char count[8]; // a frequency row's user count label
};

/// Reads a row of one of the open Chat console's lists.
///
/// @param widget status widget; may be null
/// @param list frequency or player list
/// @param index row index
/// @param[out] out what the row shows
/// @return false while no Chat console is open or past the list's last row
bool boneyards_status_widget_chat_row(
    const StatusWidget* widget, ChatList list, int32_t index, ChatRowProbe* out
);

/// Reads the text of a row of the open Chat console's log (a wrapped line takes several rows).
///
/// @param widget status widget; may be null
/// @param index row index
/// @param[out] text row text
/// @param capacity size of `text` in bytes
/// @return false while no Chat console is open or past its last row
bool boneyards_status_widget_chat_log_line(
    const StatusWidget* widget, int32_t index, char* text, std::size_t capacity
);

// The open Chat console's side panes.
enum class ChatPane : int32_t {
    create_frequency,
    frequency_info,
    player,
};

inline constexpr int32_t k_chat_pane_entries = 3;

// What one of those panes shows: its entries (the create pane's name,
// password and description; the information pane's password first) and GO
// (the player pane's PAGE) in frame pixels, the information pane's name and
// field values, and the player pane's name, rank and clan.
struct ChatPaneProbe {
    bool shown;
    ScreenRect entries[k_chat_pane_entries];
    bool entry_shown[k_chat_pane_entries];
    ScreenRect go;
    int32_t go_state; // button_state values
    char name[0x20];
    char type[0x20];
    char creator[0x40];
    char population[12];
    char rank[0x20];
    char clan[0x40];
};

/// Reads one of the open Chat console's side panes.
///
/// @param widget status widget; may be null
/// @param pane pane asked for
/// @param[out] out what the pane shows
/// @return false while no Chat console is open or the console has no such pane
bool boneyards_status_widget_chat_pane(
    const StatusWidget* widget, ChatPane pane, ChatPaneProbe* out
);

// One row of the open battle console's list, in the order the list shows it.
struct BattleRowProbe {
    bool shown; // in view, so a press at its centre lands on it
    int32_t x;  // its centre in frame pixels
    int32_t y;
    bool category; // a game-mode row, rather than a battle
    bool selected;
    char name[0x40];
    char game_id[0x28]; // a battle row's id; "" for a mode
};

/// Reads a row of the open battle console's list.
///
/// @param widget status widget; may be null
/// @param index row index
/// @param[out] out what the row shows
/// @return false while no battle console is open, the list's rows are being rebuilt, or past the
///     last row
bool boneyards_status_widget_battle_row(
    const StatusWidget* widget, int32_t index, BattleRowProbe* out
);

// One row of the open Host pane's game-type list.
struct BattleTypeProbe {
    bool shown;
    int32_t x; // its centre in frame pixels
    int32_t y;
    bool selected;
    char name[0x40];
};

/// Reads a row of the open Host pane's game-type list.
///
/// @param widget status widget; may be null
/// @param index row index
/// @param[out] out what the row shows
/// @return false while the Host pane is not open or past the list's last row
bool boneyards_status_widget_battle_type(
    const StatusWidget* widget, int32_t index, BattleTypeProbe* out
);

// The open battle console's HOST, the Host pane's name and START, and the Join summary's JOIN.
struct BattleControlsProbe {
    bool host_button_shown;
    ScreenRect host_button;
    bool host_pane_shown;
    ScreenRect name_entry;
    char name_text[0x20];
    ScreenRect start_button;
    int32_t start_state; // button_state values
    bool join_pane_shown;
    ScreenRect join_button;
    int32_t join_state; // button_state values
};

/// Reads the open battle console's HOST, Host pane and Join summary.
///
/// @param widget status widget; may be null
/// @param[out] out what those controls show
/// @return false while no battle console is open
bool boneyards_status_widget_battle_controls(const StatusWidget* widget, BattleControlsProbe* out);

// One planet, as a program driving the game reads it. `on_viewer` is false
// when the planet has no button, and then the rectangle is empty.
struct DriverPlanetFact {
    int32_t id{};
    char label[48]{};
    char text[96]{};
    bool on_viewer{};
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

/// Fills one fact per planet. Planets past `capacity` are left out.
///
/// @param widget status widget; null fills nothing
/// @param[out] out the facts
/// @param capacity how many facts `out` can hold
/// @return how many facts were written
std::size_t boneyards_status_widget_planet_facts(
    const StatusWidget* widget, DriverPlanetFact* out, std::size_t capacity
);

// English a planet's spoken value is built from. One battle uses the singular
// line. The catalogue uses the same lines, so a translation has to keep these
// conversions.
inline constexpr const char* k_planet_value = "%s, %d battles, %d forming";
inline constexpr const char* k_planet_battle = "%s, %d battle, %d forming";
inline constexpr const char* k_planet_value_chosen = "%s, %d battles, %d forming, chosen";
inline constexpr const char* k_planet_battle_chosen = "%s, %d battle, %d forming, chosen";
inline constexpr const char* k_arm_wins_label = "Arm wins";
inline constexpr const char* k_core_wins_label = "Core wins";

/// The owner word of a planet's spoken value.
///
/// A and C are Arm and Core. I, the contested letter, is contested.
/// Any other letter, including none, is unknown.
///
/// @param owner the allegiance letter
/// @return Arm, Core, contested or unknown
const char* planet_owner_word(char owner);

/// Reads a planet's facts line. The line itself is not changed.
///
/// @param text `id=… owner=… battles=… forming=…`; null is not a facts line
/// @param[out] id the planet's id; null is not written
/// @param[out] owner the allegiance letter; null is not written
/// @param[out] battles the battles; null is not written
/// @param[out] forming the forming count; null is not written
/// @return true when the line has all four
bool planet_parse_facts(const char* text, int* id, char* owner, int* battles, int* forming);

/// The value lines already in the player's language.
///
/// One battle uses `battle` or `battle_chosen`. Any other count uses
/// `battles` or `battles_chosen`.
struct PlanetValuePatterns {
    const char* battles{};
    const char* battle{};
    const char* battles_chosen{};
    const char* battle_chosen{};
};

/// Writes the label and value a planet speaks.
///
/// The label is `name`. The value is the translated template for this battle
/// count and for whether the planet is chosen, filled with `owner_word`.
/// A facts line that does not parse leaves both empty.
///
/// @param[out] label the name; null is ignored
/// @param label_capacity size of `label`, including the terminator
/// @param[out] value the value; null is ignored
/// @param value_capacity size of `value`, including the terminator
/// @param facts the facts line the planet shows
/// @param name the planet's name; null writes an empty label
/// @param chosen the planet is the one the player has chosen
/// @param patterns the four value lines, already translated
/// @param owner_word the owner in the player's language; null is empty
/// @return false when `facts` is not a planet facts line
bool planet_access_words(
    char* label,
    std::size_t label_capacity,
    char* value,
    std::size_t value_capacity,
    const char* facts,
    const char* name,
    bool chosen,
    const PlanetValuePatterns& patterns,
    const char* owner_word
);

/// The words for one of the two win icons beside a contested planet.
///
/// The icon higher on the summary is Arm's. Both icons keep the name
/// SmallArmIcon.
///
/// @param arm true for the higher icon
/// @return "Arm wins" or "Core wins"
const char* summary_win_icon_label(bool arm);

/// Names each win icon from its top.
///
/// One icon is Arm's. Of two or more, the higher is Arm's and the lower is
/// Core's. Tops that match are all Arm's. `labels` is written in the same
/// order as `tops`.
///
/// @param tops each icon's top; null writes nothing
/// @param count how many tops
/// @param[out] labels one English line per top; null writes nothing
void summary_win_icon_labels(const int32_t* tops, std::size_t count, const char** labels);

// The sector name and the map's magnification.
struct DriverMapCaption {
    bool ready{};
    char sector[0x33]{};
    int32_t magnification{};
};

/// Reads the sector name and the magnification.
///
/// @param widget status widget; null is not ready
/// @param[out] out the caption
/// @return false while no Main Frame is open
bool boneyards_status_widget_map_caption(const StatusWidget* widget, DriverMapCaption* out);

// The advert banner a program driving the game can press.
struct DriverBannerFact {
    bool shown{};
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
    char identity[32]{};
};

/// Reads the advert banner while it is shown.
///
/// @param widget status widget; null is not shown
/// @param[out] out the banner
/// @return false while it is not shown
bool boneyards_status_widget_banner(const StatusWidget* widget, DriverBannerFact* out);

/// Sets the function that hears a line, a page, the ticker, an alert, the
/// session and a console opening. Null stops the notes.
///
/// @param note called with a kind and a text; null clears it
void boneyards_console_set_driver_note(void (*note)(const char* kind, const char* text));

/// Posts one note when a function was set.
///
/// @param kind the kind; null or empty is ignored
/// @param text the note; null is empty
void boneyards_console_driver_note(const char* kind, const char* text);

} // namespace oa::boneyards::console
