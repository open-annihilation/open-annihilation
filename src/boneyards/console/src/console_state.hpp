// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Records shared by the console's main-menu overlay and entry path.
#pragma once

#include "oa/boneyards/screens/lobby_widgets.hpp"

#include <memory>
#include <string>
#include <vector>

#include "galaxy_graphics.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/maia_update.hpp"
#include "oa/boneyards/screens/update_pass.hpp"
#include "oa/boneyards/screens/navigator.hpp"

namespace oa::boneyards {
struct MessageBox;
struct FontAtlas;
} // namespace oa::boneyards

namespace oa::boneyards::console {

/// Names a folder of Help pages that replace the expansion's pages.
///
/// While the folder is set, a Help page is read from it when the file is
/// there. Otherwise the page shipped beside the program is used when that
/// file is there. The expansion's own file is read only when neither has
/// the page, and it is never written.
///
/// @param directory folder holding Help.def and the HelpNN.htm pages, or null
void boneyards_console_set_help_override_directory(const char* directory);

inline constexpr int32_t k_activity_lamp_count = 8;

// Not a message. A pending notice equal to this has already opened its box.
inline constexpr char k_pending_notice_shown_mark = '\0';
inline const char* const k_pending_notice_shown = &k_pending_notice_shown_mark;
// The news ticker's text (MainFrame.ticker) and the pulses it skips
// between steps.
inline constexpr std::size_t k_ticker_capacity = 0x100;
inline constexpr int32_t k_ticker_period = 5;

// Eight-lamp "busy" indicator.
struct ActivityLight {
    Window base;
    uint8_t running;   // lamps advance while set
    int32_t countdown; // cycles until the next lamp
    Window* lamps[k_activity_lamp_count];
    int32_t lit;           // index of the lit lamp
    ConsoleImages* images; // lamp graphics source
};

struct StatusWidget;

// Main-menu overlay.
struct MainMenuOverlay {
    Window base;
    uint8_t enable_pending; // re-enable the enter button once offline
    Window* single_button;
    Window* multi_button;
    Window* intro_button;
    Window* exit_button;
    // The part of each main-menu button the strip does not cover. A click
    // there is that button, as it was before the strip moved the columns.
    Window* single_former = nullptr;
    Window* multi_former = nullptr;
    Window* intro_former = nullptr;
    Window* exit_former = nullptr;
    Window* credits_button;
    Window* enter_button;
    ActivityLight* activity_light;
    Handle background{}; // overlay graphic, dimmed while a message box is open
    StatusWidget* owner; // host tables and image store
};

// Main Frame console button, a Button. Its
// unpressed state shows the normal face, or the lit face while its console
// is open.
struct ConsoleNavButton {
    Window base;
    Handle lit{k_no_handle};    // frame 0
    Handle normal{k_no_handle}; // frame 2
};

struct MainFrame;
struct ConsoleLayer;
struct InfoPane;
struct HtmlView;

// Where an info pane's parent makes room for it: a list of the parent that
// shrinks under the open pane and a second window that moves, each by its
// offset below the parent's top.
struct InfoPaneLayout {
    ListBox* list;
    int32_t list_closed_dy;
    int32_t list_closed_height;
    int32_t list_open_dy;
    int32_t list_open_height;
    Window* second;
    int32_t second_closed_dy;
    int32_t second_open_dy;
};

// The open and close handlers of an info pane class.
struct InfoPaneSlots {
    void (*open)(InfoPane* pane);
    void (*close)(InfoPane* pane);
};

// Side pane of the Boneyards consoles: a hidden 0x176 by 0x9d panel at (0x93,
// 0x7e) over the info pane image, its title centred in the big font and a Close_Button in its top
// right corner. The Main Frame keeps one of each kind for the consoles.
struct InfoPane {
    Window base;
    Window* close_button;
    InfoPaneLayout layout;
    const InfoPaneSlots* slots; // the class's open and close slots
    StatusWidget* owner;
};

// The panes the Main Frame keeps, by the kind its pane getter takes.
enum class PaneKind : int32_t {
    host_battle, // game setup
    join_summary,
    scoreboard,
    player,
    create_frequency,
    frequency_info,
};
inline constexpr int32_t k_pane_kind_count = 6;

// Notification codes a pane sends its parent as it opens and closes.
namespace pane_notify {
inline constexpr int32_t opened = 200;
inline constexpr int32_t closed = 0xc9;
} // namespace pane_notify

inline constexpr int32_t k_map_scroller_count = 8;
inline constexpr int32_t k_map_corner_count = 4;

// Galaxy map panel filling the info area.
struct GalaxyPanel {
    Window base;
    MainFrame* frame;
    Handle background{};                          // GMap_BG, drawn under the war map
    ConsoleLayer* console{};                      // green layer over the map
    Window* corners[k_map_corner_count];          // top-left, top-right, bottom-left, bottom-right
    Window* hotspots[screens::kMaxGalaxyPlanets]; // one per planet of the war map
};

// Screen-edge zone that scrolls the map while the pointer rests on it
struct MapScroller {
    Window base;
    uint8_t hovered;
    int32_t dx; // scroller steps per cycle
    int32_t dy;
    MainFrame* frame;
};

// Invisible button over a planet sprite.
struct PlanetHotspot {
    Window base;
    int32_t planet; // index of the planet in the galaxy model
    MainFrame* frame;
    // What a program driving the game reads: id, owner, battles and forming.
    char facts[96]{};
};

// Selected planet's summary under the war map's top right corner. Its
// planet is the galaxy model's territory.planet.
struct BattleSummary {
    MainFrame* frame;
    Surface background; // over the Main Frame's background image
    Handle arm_logo;    // Widgets.gaf ArmCoreLogos frame 0
    Handle core_logo;   // frame 1
    Window* name;
    Window* owner;
    Window* battles_caption;
    Window* battles;
    Window* forming_caption;
    Window* forming;
    Window* arm_icon;
    Window* arm_wins;
    Window* core_icon;
    Window* core_wins;
};

// Top-level Boneyards screen.
struct MainFrame {
    Window base;
    int32_t console_screen;         // console_screen id shown in the info area
    uint8_t unlock_pending;         // the map unlocks once the galaxy data has loaded
    int32_t login_countdown;        // cycles before the login screen opens
    int32_t keepalive_countdown;    // cycles between service keep-alives
    char ticker[k_ticker_capacity]; // the news ticker's text, which the ticker label scrolls
    ConsoleLayer* console{};        // the map panel's green layer; screens open in its rect
    BattleSummary* battle_summary{};
    Window* active_screen; // open console screen, or null
    // Consoles kept after they close, indexed by console_screen id. The
    // login console is not kept: each opening builds a new one. Closing
    // hides a kept console; destroying the Main Frame destroys it too.
    Window* kept_screens[console_screen::none];
    // The update console's auto-update state. The session fetches nothing.
    MaiaUpdateConsole update_console;
    // The placeholder pass the update console plays before the login console.
    screens::UpdatePass update_pass;
    Window* exit_button;
    Window* quickplay_button;
    Window* console_buttons[k_console_button_count];
    ActivityLight* activity_light;
    Window* ad_banner;
    Slider* mag_slider;
    GalaxyPanel* map_panel;
    Window* ticker_label;
    MessageBox* notice_box; // the box the last message-box alert opened
    MapScroller* scrollers[k_map_scroller_count];
    InfoPane* panes[k_pane_kind_count]; // by PaneKind, made on first use
    // Text of the notice the next cycle opens. Null when there is none.
    // k_pending_notice_shown once that box is open: Ok then asks the
    // following cycle to leave for the main menu.
    const char* pending_notice;
    bool return_requested; // the open notice's Ok asks the next cycle to leave
    StatusWidget* owner;
    // Census requests for the current area made by planet presses and
    // keep-alives, which checks count.
    uint32_t census_requests{};
    // The open screen's own open and close handlers; null where the screen
    // has none.
    void (*screen_open)(Window* screen){};
    void (*screen_close)(Window* screen){};
    // The contents console fetched documents arrive at, the last one built
    // or opened; null once it is gone.
    Window* contents_console{};
};

struct BannerReel;

// Rotating advertisement panel. The record is allocated with calloc and released with free, so
// every field stays a plain value. `reel` is owned on its own and deleted from the destroy slot.
struct AdBanner {
    Window base;
    MainFrame* frame;
    Handle banners[3];
    int32_t next;      // banner shown at the next switch
    int32_t shown{};   // local sample on screen; 0 until the first switch
    int32_t countdown; // cycles until the next switch
    ConsoleImages* images;
    bool remote{};          // the strip is showing the ad server's list
    bool fetching{};        // a catalogue or a banner file is in flight
    bool gave_up{};         // this url failed once; it is not fetched again
    int32_t waiting_kind{}; // 1 catalogue, 2 banner file, 0 nothing
    char tried[260]{};      // ad-server url already attempted
    char waiting_url[640]{};
    BannerReel* reel{};
};

/// Writes which banner is on screen.
///
/// A local strip writes "sample N". A remote strip writes the slot id.
///
/// @param banner the banner; null writes nothing
/// @param[out] out the text
/// @param capacity size of `out` in bytes
void ad_banner_shown_text(const AdBanner* banner, char* out, std::size_t capacity);

// Bytes the launch_game reply's game id keeps: 36 characters and a
// terminator.
inline constexpr std::size_t k_game_id_capacity = 0x25;

// The fields of the Boneyards system object that the console uses.
struct ConsoleSystem {
    // A battle the Boneyards launched is under way. When the Main Frame next
    // opens, a debriefing room shows the Chat console in that room;
    // otherwise the battle console opens again.
    uint8_t chat_console_pending;
    uint8_t gui_ready;      // GUI manager is initialised
    uint8_t cursor_ready{}; // cursornormal is loaded; the menu waits until it shows
    MainMenuOverlay* main_menu;
    MainFrame* main_frame;
    uint8_t pump_enabled;        // the idle tick pumps the client while set
    uint8_t session_started;     // set when the Boneyards entry path ran
    int32_t connection_level;    // 0 offline, 1 client module active, >2 connected
    uint8_t matchmaking_enabled; // 1 when the account is Admin or Beta Tester
    screens::HatFonts hat_fonts{};
    bool fonts_loaded{};
    Handle info_pane_image{k_no_handle}; // loaded with the fonts: every info pane's image
    Handle profile_image{k_no_handle};   // loaded with the fonts: the Profile console's image
    // The news ticker's pulses until its next step and how far it has
    // scrolled.
    int32_t ticker_countdown{k_ticker_period};
    int32_t ticker_offset{};
    // The launched battle has only just started: the idle work empties the
    // chat ring and clears the latency reply flags until the next
    // once-a-second check.
    uint8_t just_launched{};
    // The launch reply's game id, and the one the debriefing of the battle
    // names ("" until it arrives). The planet is the one that reply named,
    // kept with the game id; 0 until a launch is accepted.
    char launched_game_id[k_game_id_capacity]{};
    char debriefing_game_id[k_game_id_capacity]{};
    uint32_t launched_area_id{};
    // The launched planet's name and its sector's name, as the galactic map
    // named them when the launch reply came; "" when the map held no such
    // planet or no sector was loaded.
    char launched_planet[screens::kPlanetNameCapacity]{};
    char launched_sector[kSectorNameCapacity]{};
    // When the idle work's once-a-second check last ran.
    uint32_t last_check_ms{};
    // The player's name as the session record last gave it.
    char user_name[kSessionNameCapacity]{};
    // The answers of the "Exit to the system?" boxes: YES and NO set them, and
    // the idle work clears them as it acts on them.
    bool exit_accepted{};
    bool exit_declined{};
};

struct StatusWidget {
    StatusWidgetConfig config;
    ConsoleImages* images;
    ImageService image_service;
    Toolkit* toolkit; // text, font, colour, input and caret services, or null
    // Analogue glyphs for characters a face does not cover. Null when the
    // atlas file is not beside the program. The toolkit does not own it.
    oa::boneyards::FontAtlas* analogues{};
    Desktop desktop;
    Services services;
    ConsoleSystem system;
    screens::BoneyardsScreens* navigation; // model behind the built-in views
    // War-map graphics for the galaxy view; null until the Boneyards window
    // opens, and null again when any of them is missing.
    std::unique_ptr<GalaxyGraphics> galaxy_graphics;
    // The page fonts were opened for a window that has since closed, and
    // close once that window's pages have been destroyed.
    bool window_fonts_pending = false;
    uint32_t last_tick{}; // the clock at the previous tick call
    // Modifier keys held for the next input event only. A key already held
    // stays held; these add the ones that arrived as modifier bits.
    uint8_t event_shift{};
    uint8_t event_control{};
    uint8_t event_alt{};
    uint8_t event_system{};
    // A notice that arrived while the GUI was down (the game list, after the
    // main menu was left). The return from the battle opens it.
    std::string held_notice{};
    // Hears when a text field gains or loses the keyboard. Null place closes
    // the on-screen keyboard.
    void (*screen_keyboard)(void* user, const TextFieldPlace* place){};
    void* screen_keyboard_user{};

    // The real update check, when an entry started one. The placeholder pass
    // is used while `real` is clear. The lines only grow.
    struct UpdateDrive {
        bool real{false};
        bool finished{false};
        bool stopped{false};
        bool hold{false}; ///< the finished picture has been left up for one cycle
        int32_t fill{0};
        std::vector<UpdateCheckLine> lines{};
    };

    UpdateDrive update_drive{};
};

// Installs the widget's service set for the duration of one call.
struct ScopedServices {
    explicit ScopedServices(StatusWidget* widget) : previous(services_install(widget->services)) {}

    ~ScopedServices() { services_install(previous); }

    ScopedServices(const ScopedServices&) = delete;
    ScopedServices& operator=(const ScopedServices&) = delete;
    Services previous;
};

/// Builds the main-menu overlay panel, its six buttons and the activity light.
///
/// The enter button's disabled frame is a darkened copy of its normal
/// frame.
///
/// @param owner status widget whose host tables and image store the overlay uses
/// @return the overlay, hidden
MainMenuOverlay* main_menu_overlay_create(StatusWidget* owner);

/// Disables the enter button and runs the activity light until the online client reports it is
/// offline.
///
/// @param[in,out] overlay main-menu overlay
void main_menu_overlay_start_activity(MainMenuOverlay* overlay);

/// Opens a message box over the overlay, disabling every button and darkening the overlay graphic.
///
/// Hides the Boneyards cursor first. 3.1c uses the game's modal box; without
/// a host box the Boneyards message box takes its place and restores the
/// overlay on close.
///
/// @param[in,out] overlay main-menu overlay
/// @param text message
void main_menu_overlay_show_message(MainMenuOverlay* overlay, const char* text);

/// Restores the overlay after a message box.
///
/// Shows the Boneyards cursor again, re-enables the buttons, lightens the
/// overlay graphic back and returns the keyboard focus to the overlay. The
/// dim/undim round trip goes through the palette tables, so colours need not
/// come back exactly.
///
/// @param[in,out] overlay main-menu overlay
void main_menu_overlay_release_dialog(MainMenuOverlay* overlay);

/// Creates the overlay on first use, optionally starts the busy indicator, shows it and gives it
/// the keyboard focus.
///
/// @param[in,out] widget status widget
/// @param connection_pending whether the client is still going offline (runs the activity light)
void console_activate_main_menu(StatusWidget* widget, bool connection_pending);

/// Starts the GUI when needed and shows the main-menu overlay.
///
/// @param[in,out] widget status widget
/// @param connection_pending whether the client is still going offline (runs the activity light)
void console_show_main_menu(StatusWidget* widget, bool connection_pending);

/// Hides the overlay and marks it for destruction on the next GUI cycle.
///
/// @param[in,out] system the console's system record
void console_release_main_menu(ConsoleSystem* system);

/// Runs "Enter Boneyards": starts the online client and leaves the main menu for the Main Frame.
///
/// When the client cannot start, the frontend stays on the main menu. Either
/// way the frontend signal becomes 1 and the entry path counts as taken.
///
/// @param[in,out] widget status widget
void console_enter_boneyards(StatusWidget* widget);

/// Creates the Main Frame on first use and shows it.
///
/// The first creation loads and tints the Hat fonts and makes the info pane
/// and Profile images. A battle launched from the Boneyards shows the battle
/// console again, or the Chat console in the debriefing room when one was
/// named.
///
/// @param[in,out] widget status widget
void console_open_main_frame(StatusWidget* widget);

/// Hides the Main Frame and marks it for destruction on the next cycle.
///
/// @param[in,out] system the console's system record
void console_close_main_frame(ConsoleSystem* system);

/// Stops every playing sound through the host.
///
/// @param widget status widget
void console_stop_sounds(const StatusWidget* widget);

/// Plays k_background_sound on the alternate route while the chat session's background sound is on.
///
/// @param widget status widget
void console_play_background(const StatusWidget* widget);

/// Logs off (or starts logging off), closes the Main Frame and returns to the main menu with the
/// overlay.
///
/// The overlay waits for the log-off to finish before enabling "Enter
/// Boneyards" again.
///
/// @param[in,out] widget status widget
void console_reset_to_main_menu(StatusWidget* widget);

/// Allocates the Main Frame record with its counters, then creates the full-screen "BYMainFrame"
/// window, which builds the tree.
///
/// @param owner status widget the frame belongs to
/// @return the Main Frame
MainFrame* main_frame_create(StatusWidget* owner);

/// Returns the dispatch table of the eight-lamp activity light.
///
/// @return the table
const WindowOps* main_menu_activity_light_ops();

/// Enables the console buttons.
///
/// Release, Recon, Chat, Battle, News, Ladders, Help, Profile, QuickPlay and
/// Locator, for every account. Recon, Chat and Battle stay disabled while the
/// galaxy data loads or no planet is selected.
///
/// @param[in,out] frame Main Frame
void main_frame_enable_buttons(MainFrame* frame);

/// Opens console screen `screen` in the Main Frame's info area.
///
/// @param[in,out] frame Main Frame
/// @param screen console_screen id
void main_frame_show_screen(MainFrame* frame, int32_t screen);

/// Asks the service for the current area's census, counting the request.
///
/// @param[in,out] frame Main Frame
void main_frame_request_census(MainFrame* frame);

/// Handles a Central Command system alert for the Main Frame.
///
/// A page from a guest or from a user on the Silenced people list, and a
/// notification of any type but PROMOTION, are dropped. The rest sound
/// MESSAGE (a promotion PROMOTED) and, while a battle launched from the
/// Boneyards runs, go to the battle's messages. Otherwise a ticker alert and
/// a promotion replace the news ticker's text, a message-box alert closes
/// the box the last one opened and opens its own, and a page is printed in
/// the Chat console's log while that is shown or opens a message box. The
/// engine closes the last box only while the desktop still holds it.
///
/// @param[in,out] frame Main Frame
/// @param alert system alert
void main_frame_on_alert(MainFrame* frame, const AlertReport* alert);

/// Returns the Main Frame's pane of a kind, building it the first time it is asked for.
///
/// The pane gets the layout that makes room for it and becomes a child of
/// `parent`.
///
/// @param[in,out] frame Main Frame
/// @param kind pane kind
/// @param[in,out] parent window the pane opens in
/// @param layout where the parent makes room for the pane
/// @return the pane, or null for a kind no console builds yet
InfoPane*
main_frame_pane(MainFrame* frame, PaneKind kind, Window* parent, const InfoPaneLayout& layout);

/// Returns the info pane base's dispatch table, whose notify slot closes the pane from its
/// Close_Button.
///
/// A pane class's table starts from it; the class's builder allocates the
/// record with that table and info_pane_init builds the base.
///
/// @return the table
WindowOps info_pane_ops();

/// Builds an info pane's base hidden at (0x93, 0x7e) in its parent.
///
/// Adds the info pane image, the title centred in the big font over all but
/// the Close_Button's column, and the Close_Button. Nothing makes room for
/// the pane yet.
///
/// @param[out] pane pane record allocated with the class's table
/// @param title pane title, also its window name
/// @param[in,out] parent window the pane is built in
/// @param owner status widget whose fonts, images and host the pane uses
void info_pane_init(InfoPane* pane, const char* title, Window* parent, StatusWidget* owner);

/// Gives a pane the layout that makes room for it in its parent.
///
/// @param[in,out] pane info pane
/// @param layout the parent's list and second window with their open and closed places
void info_pane_set_layout(InfoPane* pane, const InfoPaneLayout& layout);

/// Opens a pane (the base open slot).
///
/// Shows the pane, shrinks the list to its open height at its open offset
/// and scrolls it on nine rows, moves the second window to its open offset
/// and tells the parent (200).
///
/// @param[in,out] pane info pane
void info_pane_open(InfoPane* pane);

/// Closes a pane (the base close slot).
///
/// Hides the pane, gives the list back its closed height and offset,
/// scrolling it back by as many of the rows below its view as nine allows,
/// puts the second window back and tells the parent (0xC9).
///
/// @param[in,out] pane info pane
void info_pane_close(InfoPane* pane);

/// Handles a press on the pane's Close_Button: INFO_PANE_CLOSE, then the class's close slot.
///
/// @param[in,out] pane info pane
/// @param notify control notice; may be null
/// @return false for any other source
bool info_pane_on_close_click(InfoPane* pane, const Notify* notify);

/// Creates the image every info pane shows: keyed, with a rule along its bottom.
///
/// @return the image
Handle info_pane_image_create();

/// Starts the lamps from the first one, unless they already run.
///
/// @param[in,out] light activity light
void activity_light_start(ActivityLight* light);

/// Darkens the lit lamp and stops the sequence.
///
/// @param[in,out] light activity light
void activity_light_stop(ActivityLight* light);

/// Adds the Mag_Slider to the Main Frame, at the map's zoom level.
///
/// @param[in,out] frame Main Frame
/// @return the slider
Slider* main_frame_create_mag_slider(MainFrame* frame);

/// Adds the map panel and the eight MapScroller edge zones to the Main Frame.
///
/// @param[in,out] frame Main Frame
void main_frame_create_map(MainFrame* frame);

/// Creates, moves or drops the PlanetHotSpot buttons to match the map model.
///
/// Every planet the model marks gets a button over its view rectangle; the
/// rest lose theirs. The button's name is the planet's name, or that name
/// plus its id when another planet shares the name.
///
/// @param[in,out] frame Main Frame
void galaxy_panel_sync_hotspots(MainFrame* frame);

/// Writes the name a planet's button shows to a program driving the game.
///
/// A name no other planet uses is written as it is. A shared name is
/// followed by the id. An empty name is the id alone, after a '#'.
///
/// @param galaxy the war map
/// @param index planet index
/// @param[out] out the name
/// @param capacity size of `out` in bytes
void galaxy_panel_planet_label(
    const screens::GalaxyView& galaxy, std::size_t index, char* out, std::size_t capacity
);

/// Counts the planets whose anchor in the map panel's viewer, as the last frame drew it, shows
/// their sprite.
///
/// @param frame Main Frame
/// @return the count (galaxy_count_drawn_planets); 0 without a viewer
std::size_t galaxy_panel_planets_drawn(const MainFrame* frame);

/// Builds the planet summary over the Main Frame, all hidden and with no planet.
///
/// Adds the planet name and owner, the Battles and Forming captions and
/// counts, and the Arm and Core logos (ArmCoreLogos frames 0 and 1, both
/// icons named SmallArmIcon) with their win counts; the picture is painted
/// into a view of the frame's background image.
///
/// @param[in,out] frame Main Frame
/// @return the summary, or null when the record cannot be allocated
BattleSummary* battle_summary_create(MainFrame* frame);

/// Releases the summary's two logo frames; the caller frees the record.
///
/// @param[in,out] summary planet summary
void battle_summary_release(BattleSummary* summary);

/// Refreshes the planet summary's labels from the selected planet and shows them.
///
/// Without a planet the labels are hidden; with one its name, owner (kept
/// for an unknown side), Battles and Forming counts are shown, and the win
/// counts and logos only while it is contested.
///
/// @param[in,out] summary planet summary
void battle_summary_update(BattleSummary* summary);

/// Paints the selected planet's picture into the Main Frame's background.
///
/// Clears the picture area to 0xAF and, with a planet selected, paints its
/// current frame at the 50 size there, clipped to the area.
///
/// @param[in,out] summary planet summary
void battle_summary_draw(BattleSummary* summary);

/// Ignores map input and disables the Mag_Slider while a screen covers the map.
///
/// No planet stays under the pointer.
///
/// @param[in,out] frame Main Frame
void main_frame_lock_map(MainFrame* frame);

/// Accepts map input again and enables the Mag_Slider.
///
/// @param[in,out] frame Main Frame
void main_frame_unlock_map(MainFrame* frame);

/// Enters the model of the Boneyards screen behind a console screen and builds its view.
///
/// Help, Ladders and News are contents consoles, and Recon, Chat, Profile,
/// Login, Battle and Update have their own views; the other console screens
/// without a Boneyards screen get an empty panel. Used when the config
/// supplies no screen factory.
///
/// @param widget status widget
/// @param screen console_screen id
/// @param frame the Main Frame window
/// @param x left edge in frame pixels
/// @param y top edge in frame pixels
/// @param width view width in pixels
/// @param height view height in pixels
/// @return the view window
Window* console_create_screen_view(
    StatusWidget* widget,
    int32_t screen,
    Window* frame,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
);

// Create Frequency dialog: the Frequency Name, Password and Description entries under their
// captions and GO. Its request flag and kept password are the chat's create_dialog.submitting and
// create_dialog.password.
struct CreateFrequencyPane {
    InfoPane pane;
    SingleEdit* name;
    SingleEdit* password;
    SingleEdit* description;
    Window* go;
};

inline constexpr int32_t k_info_description_lines = 3;

// Frequency information pane: the frequency's name, its Type, Created by and
// Population fields, three lines of its description and, for a frequency that needs one, a password
// entry beside GO.
struct FrequencyInfoPane {
    InfoPane pane;
    uint32_t room_id; // screens::kNoRoomInfo at first
    char name[screens::kRoomNameCapacity];
    uint8_t needs_password;
    uint32_t population;
    Window* name_label;
    screens::LobbyField* type;
    screens::LobbyField* creator;
    screens::LobbyField* population_field;
    Window* description[k_info_description_lines];
    Window* password_label;
    SingleEdit* password;
    Window* go;
};

inline constexpr int32_t k_player_pane_tabs = 3;

// Player pane: the player's rank icon and name, rank title, side insignia and clan down the left
// with the Whisper Lock and Muzzle check boxes and PAGE, and the General, Ladder and Awards tabs
// over an HTML view of the chosen page on the right. The player, the sysop flag, the record and
// the tab are the chat's screens::PlayerPane, which these windows show.
struct PlayerPaneView {
    InfoPane pane;
    Handle arm_insignia;  // Insignia frame 1
    Handle core_insignia; // Insignia frame 0
    Window* name;
    Window* rank;
    Window* rank_icon; // User_Icons
    Window* insignia;
    Window* clan;
    Window* page; // PAGE
    CheckBox* whisper_lock;
    CheckBox* muzzle;
    HtmlView* html;
    Window* tabs[k_player_pane_tabs]; // GeneralTabBtn, LadderTabBtn, AwardsTabBtn
    Window* tab_labels[k_player_pane_tabs];
    int32_t rank_frame; // User_Icons frame the rank icon holds, -1 none
};

/// Builds the player pane hidden in the Main Frame.
///
/// Adds the rank icon (hidden) with the name beside it, the rank title, the
/// insignia (hidden) and the clan in the small font; the Whisper Lock and
/// Muzzle boxes in the dim small font and PAGE under them; the General,
/// Ladder and Awards captions in the button font, each under an invisible
/// button; and the HTML view of the page. It keeps Insignia frames 1 (Arm)
/// and 0 (Core) for the insignia.
///
/// @param[in,out] frame Main Frame
/// @param title pane title
/// @return the pane
InfoPane* player_pane_create(MainFrame* frame, const char* title);

/// Builds the create-frequency pane hidden in the Main Frame.
///
/// Adds the Frequency Name, Password and Description entries (framed,
/// scrolling, 23, 15 and 127 characters) with their captions ending just
/// left of them in the medium font, and GO under them, disabled until a
/// name is typed.
///
/// @param[in,out] frame Main Frame
/// @param title pane title
/// @return the pane
InfoPane* create_frequency_pane_create(MainFrame* frame, const char* title);

/// Builds the frequency information pane hidden in the Main Frame, showing no frequency.
///
/// Adds the name row, the Type, Created by and Population fields and three
/// description rows in the small font, and the medium-font "Password:"
/// caption with the password entry (framed, scrolling, 15 characters)
/// beside it and GO.
///
/// @param[in,out] frame Main Frame
/// @param title pane title
/// @return the pane
InfoPane* frequency_info_pane_create(MainFrame* frame, const char* title);

/// Shows the chat's player pane in the view.
///
/// Sets the rank icon from User_Icons while it has one, the name and rank
/// title in the side's colour, the side's insignia, the clan, the states of
/// PAGE, the two boxes and the tab buttons, the chosen tab's caption in the
/// lit button font and the others in the normal one.
///
/// @param[in,out] view player pane view
/// @param pane the chat's player pane
/// @param page whether to show its page too
void player_pane_show(PlayerPaneView* view, const screens::PlayerPane& pane, bool page);

/// Shows a frequency in the information pane, then runs its pulse.
///
/// Sounds GREEN_BUTTON when it is another frequency than the one shown, and
/// sets its name, its type with its creator (the System for a planetary
/// default or a battle debriefing, the tag of a user-created one, "Unknown"
/// for any other type), its population, the password caption and entry only
/// when it needs one, and its description wrapped to the width of the rows
/// into at most three of them.
///
/// @param[in,out] pane frequency information pane
/// @param room the frequency's row
void frequency_info_pane_show(FrequencyInfoPane* pane, const screens::ChatRoom* room);

// Chat Frequencies console: the frequency list, CREATE and the player list in the left column,
// with MATCHMAKING under them when ConsoleSystem.matchmaking_enabled is set, and the ChatList log
// with the input line under it on the right. The chat's state is the navigation's
// screens::ChatConsole, which also says which of the panes is shown.
struct ChatView {
    Window base;
    MainFrame* frame;
    Window* chat_button; // the Main Frame's Chat button
    screens::ByListBox* room_list;
    Window* create_button;
    screens::ByListBox* player_list;
    Window* matchmaking;     // Minimize_Button, null without system
    screens::ByListBox* log; // ChatList
    SingleEdit* input;
    PlayerPaneView* player_pane;
    CreateFrequencyPane* create_pane;
    FrequencyInfoPane* info_pane;
    StatusWidget* owner;
};

/// Opens or closes the Chat console's player, create-frequency and frequency information panes to
/// match the chat, closing before opening.
///
/// @param[in,out] view Chat console
void chat_panes_sync(ChatView* view);

/// Creates the Chat console in the green layer's rectangle; it builds itself hidden.
///
/// Makes the console the one the chat prints to and keeps its lists in, and
/// Chat Frequencies the current Boneyards screen, so the room feeds reach
/// the chat from here on. The chat's line
/// colours are set to the session's tinted small fonts on the way.
///
/// @param owner status widget
/// @param frame the Main Frame window
/// @param x left edge in frame pixels
/// @param y top edge in frame pixels
/// @param width console width in pixels
/// @param height console height in pixels
/// @return the console window
Window* chat_console_create(
    StatusWidget* owner, Window* frame, int32_t x, int32_t y, int32_t width, int32_t height
);

/// Opens the Chat console (its open slot).
///
/// Takes the Main Frame's player, create-frequency and frequency
/// information panes, each making room for itself by moving the ChatList
/// down, lights the Chat button, has the chat start the activity light and
/// enter the planet's debriefing, preferred or default frequency, gives the
/// input line the focus and shows the console.
///
/// @param console the Chat console
void chat_console_open(Window* console);

/// Closes the Chat console (its close slot).
///
/// Puts the Chat button back on its normal face, then closes the chat (both
/// lists, whisper lock, panes and the entered frequency) and hides the
/// console.
///
/// @param console the Chat console
void chat_console_close(Window* console);

/// Returns the open Chat console, which the chat prints to and keeps its lists in.
///
/// @return the console, or null while none is open
ChatView* console_open_chat();

/// Follows "Use modern fonts for game text" and Text size into the open chat.
///
/// On, the log and the entry line use the modern faces at the in-battle
/// message-log size and the log makes room for the taller entry. Off, both
/// keep the Boneyards' fonts. A change that leaves the line the same is
/// ignored. Nothing is open yet when this runs from startup.
///
/// @param modern_fonts Use modern fonts for game text is on
/// @param text_size Text size, in percent
void chat_set_modern_text(bool modern_fonts, int32_t text_size);

/// Creates a Help, Ladders or News console in the green layer's rectangle.
///
/// Sets up the record (its contents file, its Main Frame button and its
/// content server kind), makes it the console arriving documents go to, and
/// creates its window, which builds it.
///
/// @param owner status widget
/// @param screen console_screen id: help, ladders or news (anything else builds Help)
/// @param frame the Main Frame window
/// @param x left edge in frame pixels
/// @param y top edge in frame pixels
/// @param width console width in pixels
/// @param height console height in pixels
/// @return the console window
Window* contents_console_create(
    StatusWidget* owner,
    int32_t screen,
    Window* frame,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
);

/// Opens a contents console (its open slot).
///
/// Lights the console's Main Frame button (the lit face in its unpressed
/// state), shows the console and makes it the one arriving documents go to.
///
/// @param console the contents console
void contents_console_open(Window* console);

/// Closes a contents console (its close slot): its Main Frame button goes back to its normal face
/// and the console hides.
///
/// @param console the contents console
void contents_console_close(Window* console);

/// Hands a fetched document to a contents console; a downloaded temporary file is deleted.
///
/// @param console contents console the document is for; may be null
/// @param url locator the document was asked for
/// @param path file holding the document
/// @param status fetch status
void contents_console_file_arrived(
    Window* console, const char* url, const char* path, int32_t status
);

/// Fills the probe's contents fields from an open contents console.
///
/// @param console the contents console
/// @param[in,out] out probe receiving the rows, waiting flag, page locator and missing images
void contents_console_probe(const Window* console, MainFrameProbe* out);

/// Puts a console's Main Frame button back to its normal face and hides the console.
///
/// The close slot of the contents and Profile consoles.
///
/// @param[in,out] console the console window
/// @param console_button the console's Main Frame button
void console_screen_close(Window* console, Window* console_button);

// Recon console: the selected planet's dossier. A bordered scan box
// shows the planet's panorama scrolling by (or the map static without one), eight labelled fields
// its map and census values, and a list the participation notice and the planet's description.
struct ReconConsole {
    Window base;
    // The wind, pan strip and static state and the planet.
    screens::ReconState state;
    // In screens::planet_info_row order.
    screens::LobbyField* fields[screens::kPlanetInfoRowCount];
    screens::ByListBox* text;  // notice and description rows
    Handle image{k_no_handle}; // the console's own image
    Window* recon_button;      // the Main Frame's Recon button
    StatusWidget* owner;
};

/// Creates the Recon console in the green layer's rectangle; it builds itself hidden.
///
/// The record holds the Main Frame's Recon button.
///
/// @param owner status widget
/// @param frame the Main Frame window
/// @param x left edge in frame pixels
/// @param y top edge in frame pixels
/// @param width console width in pixels
/// @param height console height in pixels
/// @return the console window
Window* recon_console_create(
    StatusWidget* owner, Window* frame, int32_t x, int32_t y, int32_t width, int32_t height
);

/// Opens the Recon console on the selected planet (its open slot).
///
/// Lights the Recon button and starts the activity light, asks for the
/// current area's census, fills the fields for the selected planet (its map
/// OTA read on first use, the wind sampled between its limits), loads its
/// panorama when the map is installed and starts the scan at the strip's
/// left edge, refills the list with the participation notice of a planet
/// whose map is missing and the planet's description, and shows the
/// console.
///
/// @param console the Recon console
void recon_console_open(Window* console);

/// Closes the Recon console (its close slot): the Recon button goes back to its normal face, the
/// list empties and the console hides.
///
/// @param console the Recon console
void recon_console_close(Window* console);

/// Updates the open Recon console's Population from a census event for its planet.
///
/// The census is already stored on the war map.
///
/// @param console the Recon console
/// @param report census of one area; other planets' are ignored
void recon_console_on_census(Window* console, const CensusReport* report);

inline constexpr int32_t k_profile_description_lines = 5;

// Profile console: the PROFILE title over the ProfileList of the seven
// Profile.def categories, five lines of the selected category's description
// below the rows, APPLY and RESET, and a status line. Its state (the record,
// the fields and the tab controls) is the navigation's screens::ProfileConsole.
struct ProfileView {
    Window base;
    MainFrame* frame;
    Window* profile_button;   // the Main Frame's Profile button
    Handle arm_insignia;      // Insignia frame 1
    Handle core_insignia;     // Insignia frame 0
    screens::ByListBox* list; // ProfileList
    Window* apply;
    Window* reset;
    Window* status;
    Window* description[k_profile_description_lines];
    // The tab whose description the labels show (-1 none), and the owner.
    int32_t description_tab;
    StatusWidget* owner;
};

/// Creates the Profile console in the green layer's rectangle; it builds itself hidden.
///
/// The record keeps its Main Frame, that frame's Profile button and Insignia
/// frames 1 (Arm) and 0 (Core). Building makes the Profile console the
/// current Boneyards screen, which asks for the player's own record.
///
/// @param owner status widget
/// @param frame the Main Frame window
/// @param x left edge in frame pixels
/// @param y top edge in frame pixels
/// @param width console width in pixels
/// @param height console height in pixels
/// @return the console window
Window* profile_console_create(
    StatusWidget* owner, Window* frame, int32_t x, int32_t y, int32_t width, int32_t height
);

/// Opens the Profile console (its open slot): lights the Profile button and shows the console.
///
/// @param console the Profile console
void profile_console_open(Window* console);

/// Closes the Profile console (its close slot): the Profile button goes back to its normal face and
/// the console hides.
///
/// @param console the Profile console
void profile_console_close(Window* console);

/// Creates the image every Profile console shows: a blank in Widgets.gaf's key with a 0x5D rule
/// down the ProfileList's right edge.
///
/// The Main Frame's first opening makes it with the fonts.
///
/// @return the image
Handle profile_image_create();

// Open and close slots of the built-in view for console screen `screen`.
struct ConsoleScreenSlots {
    void (*open)(Window* screen);
    void (*close)(Window* screen);
};

/// Returns the open and close slots of the built-in view for a console screen.
///
/// @param screen console_screen id
/// @return the slots; both null for a screen without them
ConsoleScreenSlots console_screen_slots(int32_t screen);

} // namespace oa::boneyards::console
