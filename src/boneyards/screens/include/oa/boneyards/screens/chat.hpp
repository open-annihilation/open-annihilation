// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Chat Frequencies console: player and frequency lists, the chat command
// language, whisper lock, muzzling, the locator and frequency entry. The
// console's state (ChatConsole) reads and writes the fields of the shared
// session record it needs (ChatSession).
#pragma once

#include "oa/boneyards/screens/chat_room.hpp"
#include "oa/boneyards/screens/profile.hpp"
#include "oa/boneyards/service.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::boneyards::screens {

// A typed user name is rejected past this many characters. The field holds them.
inline constexpr int32_t kUserNameCharacters = kPlayerNameCharacters;
inline constexpr std::size_t kUserNameCapacity =
    oa::boneyards::utf8::bytes_for(static_cast<std::size_t>(kUserNameCharacters));
inline constexpr std::size_t kRoomNameMax = static_cast<std::size_t>(kRoomNameCharacters);
inline constexpr std::size_t kRoomPasswordMax = 0x0f;
inline constexpr std::size_t kRoomDescriptionMax =
    static_cast<std::size_t>(kRoomDescriptionCharacters);
// The input line keeps at most 255 characters.
inline constexpr int32_t kChatLineCharacters = 0xff;
inline constexpr std::size_t kChatLineCapacity =
    oa::boneyards::utf8::bytes_for(static_cast<std::size_t>(kChatLineCharacters));
inline constexpr std::size_t kLobbyMessageMax = 0x190; // chat text appended to a lobby line
inline constexpr std::size_t kMaxChatPlayers = 128;
inline constexpr std::size_t kMaxChatRooms = 64;
inline constexpr std::size_t kMaxGuestSilenced = 64;
inline constexpr std::size_t kMaxAreaRooms = 32;

// Colour argument meaning "use the widget's default colour".
inline constexpr int32_t kDefaultTextColor = -1;

// Account flag bit set for unregistered (guest) accounts.
inline constexpr uint8_t kAccountUnregistered = 0x80;

// User ids with the top bit set are guests; their muzzle state is kept in the
// console instead of the persistent people list.
inline constexpr uint32_t kGuestUserBit = 0x80000000u;

inline constexpr std::size_t kPlayerNameCapacity = 0x100;

// Chat console message codes routed through the screen's message handler.
namespace chat_message {
inline constexpr int32_t enter_key = 0x0d;
inline constexpr int32_t clear_selection = 0xc9;
inline constexpr int32_t go_to_room = 300;
inline constexpr int32_t toggle_muzzle = 0x12d;
inline constexpr int32_t lock_whisper = 0x12e;
inline constexpr int32_t profile_shown = 0x12f;
inline constexpr int32_t unlock_whisper = 0x130;
} // namespace chat_message

// Fields of the shared session record read by the chat screens.
struct ChatSession {
    int32_t system_font; // status lines and SysOp chat
    int32_t rank_0x15_font;
    int32_t arm_font;        // Arm names and senior Arm rank icons 0x11-0x12, 0x16, 0x23-0x24
    int32_t core_font;       // Core names and senior Core rank icons 8-9, 0x18, 0x1d-0x1e
    bool debriefing_pending; // enter the battle debriefing room next
    uint32_t self_user_id;
    uint8_t account_flags;
    uint32_t area_id; // current planet
    bool background_sound;
    uint32_t room_id; // current frequency, 0 when none
    uint32_t debrief_area_id;
    uint32_t debrief_room_id;
    uint32_t pending_area_id; // frequency being entered
    uint32_t pending_room_id;
    char pending_room_name[kRoomNameCapacity];
};

// Allegiance text kept with a member of a frequency. The report is a short word.
inline constexpr std::size_t kMemberAllegianceCapacity = 16;

// One player list row.
struct ChatPlayer {
    char name[kPlayerNameCapacity];
    uint32_t user_id;
    int32_t user_icon;    // User_Icons frame of the rank icon; also selects the chat colour
    int32_t latency_icon; // Latency_Icons frame
    int32_t rank;         // rank reported with the member
    char allegiance[kMemberAllegianceCapacity];
    bool whisper_locked;
    bool silenced;
};

// One frequency list row.
struct ChatRoom {
    char name[kRoomNameCapacity];
    char description[kRoomDescriptionCapacity];
    char tag[kRoomTagCapacity]; // shown for a user-created frequency
    uint32_t room_id;
    bool password_required;
    int32_t population; // count label
    int32_t type;       // room_type
};

// Remembered preferred room per planet (ChatConsole.area_rooms).
struct AreaRoom {
    uint32_t area_id;
    uint32_t room_id;
};

// Frequency id of the information pane before it shows one.
inline constexpr uint32_t kNoRoomInfo = 0xffffffff;

// The create-frequency dialog.
struct FrequencyDialog {
    bool visible;
    bool submitting;                            // request sent, waiting for the reply
    char name[kRoomNameCapacity];               // Frequency Name entry
    char password[kUserNameCapacity];           // Password entry
    char description[kRoomDescriptionCapacity]; // Description entry
    int32_t create_value;                       // CREATE: widget_value enabled or disabled
    int32_t entry_value;                        // the three entries: 1 takes input, 0 disabled
};

// Pages of the player pane.
enum class PaneTab : int32_t {
    general = 0,
    ladder = 1,
    awards = 2,
};

inline constexpr std::size_t kPaneTabCount = 3;

// The empty page the player pane shows as it opens. An array, so every
// source file that names it sees the one page at the same address.
inline constexpr char kPaneEmptyPage[] = "<html><BODY><P></BODY></html>";

// The player information pane beside the lists, built once with the console.
struct PlayerPane {
    bool visible;
    uint32_t user_id;
    bool sysop;                 // the row showed the SysOp icon: the muzzle stays off
    int32_t whisper_icon_value; // Whisper Lock check box
    int32_t silence_value;      // Muzzle check box
    // Profile view text: a page of the record, or a fixed page; the empty
    // page once the pane closes and its record goes.
    const char* html;
    const ProfileRecord* record; // &profile from a request until the pane closes
    ProfileRecord profile;       // what the reply to the pane's request fills
    int32_t tab;                 // PaneTab shown
    int32_t user_icon;           // User_Icons frame
    bool user_icon_shown;
    char name[kProfileNameCapacity]; // label
    const char* rank;                // label
    SideColor name_color;            // name and rank labels
    int32_t insignia;                // image, an insignia_frame
    bool insignia_shown;
    // Label: the CLAN of the last profile shown, kept while the next one is
    // requested.
    char clan[kProfileTextCapacity];
    int32_t page_value;                // PAGE button
    int32_t tab_values[kPaneTabCount]; // General, Ladder, Awards buttons
    // Tab labels: the button hover colour when set, else the normal one.
    bool tab_highlighted[kPaneTabCount];
};

// How a row of the frequency or player list changed.
enum class RowChange : int32_t {
    added,   // a new row now at the index
    updated, // the row at the index shows new values
    removed, // the row that was at the index is gone
};

// Screen output boundary. Every callback is optional.
struct ChatUi {
    void* user;
    // Wraps a line into the chat list, playing GREEN_BUTTON first unless quiet.
    void (*print_line)(void* user, const char* text, int32_t color, bool quiet);
    // Opens a modal message box.
    void (*message_box)(void* user, const char* text);
    // Random number source for choosing a default frequency (the shared rand15 stream).
    uint32_t (*random)(void* user);
    // Starts (running) or stops the Main Frame's activity light, which runs
    // while a frequency entry or listing or the player pane's profile
    // request is outstanding.
    void (*activity)(void* user, bool running);
    // Scrolls the frequency list just far enough to bring row `row` into
    // view; the selection stays.
    void (*show_room)(void* user, std::size_t row);
    // A frequency row was added, had its count changed, or was removed (then
    // `room` is what the row held). Rows keep the chat's order, so `index`
    // is the list row.
    void (*room_row)(void* user, RowChange change, const ChatRoom* room, std::size_t index);
    // A player row was added, had its icons, muzzle or whisper lock changed,
    // or was removed.
    void (*player_row)(void* user, RowChange change, const ChatPlayer* player, std::size_t index);
    // Every frequency row, or every player row, was dropped.
    void (*rooms_cleared)(void* user);
    void (*players_cleared)(void* user);
    // Makes frequency row `row` the list's selected row; -1 selects none.
    void (*select_room)(void* user, int32_t row);
    // The player pane's texts, icons or controls changed; `page` when it
    // also has a new page to show (pane.html).
    void (*pane_changed)(void* user, bool page);
    // Makes player row `row` the list's selected row; -1 selects none.
    void (*select_player)(void* user, int32_t row);
    // The chat replaced the input line (`input`) and gave it the focus.
    void (*show_input)(void* user);
};

struct ChatConsole {
    ChatSession* session;
    const BoneyardsService* service;
    ChatUi ui;
    char input[kChatLineCapacity]; // input line
    bool input_focused;
    uint32_t whisper_lock_user;
    uint32_t guest_silenced[kMaxGuestSilenced]; // user ids of the silenced guests
    std::size_t guest_silenced_count;
    bool enter_default_room;
    AreaRoom area_rooms[kMaxAreaRooms];
    std::size_t area_room_count;
    uint32_t default_candidates[kMaxChatRooms]; // rooms eligible as a default
    std::size_t default_candidate_count;
    int32_t player_count;
    ChatPlayer players[kMaxChatPlayers]; // player list rows
    std::size_t player_total;
    ChatRoom rooms[kMaxChatRooms]; // frequency list rows
    std::size_t room_total;
    int32_t selected_room; // frequency list selection, -1 none
    PlayerPane pane;
    FrequencyDialog create_dialog;
    bool room_info_visible;
    uint32_t room_info_room_id; // kNoRoomInfo until a frequency is shown
    bool visible;
};

/// Binds a console to its session, service and output, clears its lists and builds its player,
/// create-frequency and frequency information panes.
///
/// @param[out] console console to build
/// @param session shared session record the console reads and writes
/// @param service service the requests go to
/// @param ui the console's output hooks
void chat_console_init(
    ChatConsole* console, ChatSession* session, const BoneyardsService* service, ChatUi ui
);

/// Prints one console line: wraps it into the chat list, playing GREEN_BUTTON first unless quiet.
///
/// @param console chat console
/// @param text the line
/// @param color font or colour of the line; kDefaultTextColor for the list's default
/// @param quiet whether to skip the sound
void chat_print_line(ChatConsole* console, const char* text, int32_t color, bool quiet);

/// Finds a player row by user id.
///
/// @param console chat console
/// @param user_id user looked for
/// @return the row, or null when the player is not listed
ChatPlayer* chat_find_player(ChatConsole* console, uint32_t user_id);

/// Looks up a listed player's user id by name, compared case-insensitively.
///
/// @param console chat console
/// @param name player name
/// @return the user id, or 0 when no listed player has the name
uint32_t chat_find_player_id_by_name(const ChatConsole* console, const char* name);

/// Finds a frequency row by room id.
///
/// @param console chat console
/// @param room_id frequency looked for
/// @return the row, or null when the frequency is not listed
ChatRoom* chat_find_room(ChatConsole* console, uint32_t room_id);

/// Finds a frequency row by name, compared case-insensitively.
///
/// @param console chat console
/// @param name frequency name
/// @return the row, or null when no listed frequency has the name
ChatRoom* chat_find_room_by_name(ChatConsole* console, const char* name);

/// Adds a delta to a listed frequency's population label.
///
/// @param[in,out] console chat console
/// @param room_id frequency whose count changes; an unlisted one is ignored
/// @param delta amount added
void chat_add_room_population(ChatConsole* console, uint32_t room_id, int32_t delta);

/// Writes a listed player's name, or "UNK" when the id is not listed.
///
/// @param console chat console
/// @param user_id player looked up
/// @param[out] out name text
/// @param capacity size of `out` in bytes
/// @return whether the player is listed
bool chat_format_player_name(
    const ChatConsole* console, uint32_t user_id, char* out, std::size_t capacity
);

/// Tells whether a user is muzzled: guests are kept in the console, others in the Silenced people
/// list.
///
/// @param console chat console
/// @param user_id user asked about
/// @return true while muzzled
bool chat_is_silenced(const ChatConsole* console, uint32_t user_id);

/// Toggles a user's muzzle and mirrors it into the player row and the pane.
///
/// @param[in,out] console chat console
/// @param user_id user toggled; a guest id is kept in the console
/// @return the new state: true when now muzzled
bool chat_toggle_silence(ChatConsole* console, uint32_t user_id);

/// Locks whispering onto a user or clears the lock, and prints the matching notice.
///
/// Locking marks the player's row and the pane's whisper icon and unmarks
/// the previous target; a player who is not listed cannot be locked onto.
///
/// @param[in,out] console chat console
/// @param user_id user locked onto, or whose lock ends
/// @param lock true to lock, false to clear
void chat_set_whisper_lock(ChatConsole* console, uint32_t user_id, bool lock);

/// Runs the input line: plain chat, a '+'/'-' command, or a whisper to the locked user.
///
/// The input line is emptied first. A whisper to a locked user who has left
/// clears the lock and puts the line back.
///
/// @param[in,out] console chat console
/// @return false only for an empty line
bool chat_submit_input(ChatConsole* console);

/// Routes a console message code.
///
/// Enter runs the input line while it has the focus; list scroll codes (400,
/// 401, 403, 405) are taken as handled.
///
/// @param[in,out] console chat console
/// @param code message code
/// @return whether the code was handled
bool chat_dispatch_message(ChatConsole* console, int32_t code);

/// Prints an incoming chat line prefixed with the sender's name.
///
/// Muzzled senders are dropped (never a SysOp or the player); the line is
/// coloured by the sender's rank icon. A speaker with no row is not shown.
/// A line from the server names no member, so it is not shown.
///
/// @param[in,out] console chat console
/// @param sender_id speaker; an unlisted sender is dropped
/// @param text the line; at most kLobbyMessageMax characters are kept
/// @param from_server true when the line names no user
void chat_receive_message(
    ChatConsole* console, uint32_t sender_id, const char* text, bool from_server = false
);

// Player list change kinds reported by the service.
enum class PlayerListChange : int32_t { joined = 0, left = 1 };

/// Adds or refreshes a player row, with its muzzle and whisper-lock state, or removes it.
///
/// A joining player returns the pane's whisper icon to its first frame; a
/// leaving one ends a whisper lock on it and disables the pane's whisper
/// icon. The current frequency's population follows.
///
/// @param[in,out] console chat console
/// @param change joined or left
/// @param user_id the player
/// @param name player name
/// @param latency_icon Latency_Icons frame
/// @param user_icon User_Icons frame of the rank icon; a joined row with one outside 1..limit
///     is ignored
/// @param rank rank reported with the member
/// @param allegiance allegiance reported with the member; null keeps none
void chat_update_player(
    ChatConsole* console,
    PlayerListChange change,
    uint32_t user_id,
    const char* name,
    int32_t latency_icon,
    int32_t user_icon,
    int32_t rank = 0,
    const char* allegiance = nullptr
);

/// Formats the locator's answer about where a user is as a console line.
///
/// @param report locator reply
/// @param[out] out line text
/// @param capacity size of `out` in bytes
void chat_format_locate_result(const LocateReport* report, char* out, std::size_t capacity);

/// Prints a locator reply (the service's locate callback).
///
/// @param context the ChatConsole
/// @param report locator reply
void chat_on_locate_result(void* context, const LocateReport* report);

// Frequency join outcomes reported by the service.
enum class JoinResult : int32_t {
    entered = 0,
    failed = 1, // any non-zero reply
};

/// Handles the reply to a frequency entry.
///
/// An entry records the new room, empties the player list, remembers the
/// room for the planet and closes the create dialog; a failure explains
/// itself (and with no current room asks for a default). Either way the
/// frequency list is requested again.
///
/// @param[in,out] console chat console
/// @param result entered or failed
void chat_on_join_result(ChatConsole* console, JoinResult result);

// One frequency row reported by the room-list query.
struct RoomListing {
    uint32_t room_id;
    const char* name;
    const char* description;
    bool password_required;
    int32_t population;
    int32_t type; // room_type
    const char* tag;
};

// Planetary defaults with more occupants than this are not picked as the
// default frequency.
inline constexpr int32_t kDefaultRoomPopulationMax = 0x1e;

/// Adds, refreshes or removes a frequency row of a listing.
///
/// The last row of a listing stops the activity light and scrolls the
/// current frequency into view. While a default entry is pending the listing
/// collects the planetary defaults with room to spare and its last row
/// enters a random one.
///
/// @param[in,out] console chat console
/// @param change joined (added or refreshed) or left (removed)
/// @param room the row
/// @param last whether the row ends the listing
void chat_on_room_listed(
    ChatConsole* console, PlayerListChange change, const RoomListing* room, bool last
);

/// Creates, refreshes or removes one frequency row.
///
/// New rows are kept in the list's order: planetary defaults by name, then
/// user-created frequencies, then battle debriefings; an unlisted type gets
/// no row. The current frequency shows the console's player count.
///
/// @param[in,out] console chat console
/// @param change joined (added or refreshed) or left (removed)
/// @param room the row
void chat_update_room_row(ChatConsole* console, PlayerListChange change, const RoomListing* room);

/// Moves to a frequency, leaving the current one once it is entered.
///
/// The activity light runs until the entry's listing ends.
///
/// @param[in,out] console chat console
/// @param area_id area of the frequency
/// @param room_id frequency to enter
/// @param password frequency password; null for none
/// @param name frequency name shown in the entry notices; null for none
/// @return false when an entry is already pending or the frequency is current
bool chat_go_to_room(
    ChatConsole* console, uint32_t area_id, uint32_t room_id, const char* password, const char* name
);

// Controls of the Chat Frequencies screen that post commands.
enum class ChatControl : int32_t {
    room_list,
    create_button,
    player_list,
    matchmaking_button,
};

// Message code of a list double-click.
inline constexpr int32_t kListActivate = 2;

/// Handles a click or activation on the frequency list, CREATE, the player list or matchmaking.
///
/// Activating an open frequency goes to it; any other frequency click shows
/// the frequency information pane. CREATE toggles the create-frequency
/// pane, a player click opens the player pane on the player's profile.
///
/// @param[in,out] console chat console
/// @param control control clicked
/// @param code message code (kListActivate for a double click)
/// @param index list row for the list controls
void chat_on_control(ChatConsole* console, ChatControl control, int32_t code, std::size_t index);

/// Opens the console on the current planet with the activity light running.
///
/// Enters the battle debriefing room, the remembered preferred room, or
/// requests the room list to pick a default.
///
/// @param[in,out] console chat console
void chat_open_on_planet(ChatConsole* console);

/// Blanks the player pane's texts, icons, controls and profile view while it is hidden.
///
/// @param[in,out] console chat console
void chat_pane_reset(ChatConsole* console);

/// Shows "ACCESSING..." in the player pane and requests a player's profile into a fresh record.
///
/// Starts the activity light; building the record reads Profile.def the
/// first time.
///
/// @param[in,out] console chat console
/// @param user_id player whose profile is requested
/// @param sysop whether the player's row shows the SysOp icon
void chat_pane_begin_profile(ChatConsole* console, uint32_t user_id, bool sysop);

/// Handles a message from the player pane: page prefill, muzzle toggle, whisper lock and the pane's
/// icon refresh.
///
/// @param[in,out] console chat console
/// @param code chat_message code
/// @param user_id player the message is about
/// @param user_name player name, for a page; may be null
void chat_handle_pane_message(
    ChatConsole* console, int32_t code, uint32_t user_id, const char* user_name
);

/// Builds the create-frequency pane: hidden, no request sent, its entries empty and taking input,
/// CREATE disabled.
///
/// @param[in,out] console chat console
void chat_create_dialog_build(ChatConsole* console);

/// Builds the frequency information pane: hidden, holding no frequency.
///
/// @param[in,out] console chat console
void chat_room_info_build(ChatConsole* console);

/// Runs the create-frequency pane's pulse while it is shown.
///
/// Enables CREATE when the name entry holds text and neither this request
/// nor a frequency change is pending, and lets the three entries take input
/// only while nothing is pending.
///
/// @param[in,out] console chat console
void chat_create_dialog_pulse(ChatConsole* console);

/// Opens the create-frequency dialog and sends the request, recording the dialog fields.
///
/// @param[in,out] console chat console
/// @param name frequency name; null for none
/// @param description frequency description; null for none
/// @param password frequency password; null for an open frequency
/// @return false when a request or a frequency change is already pending
bool chat_create_room(
    ChatConsole* console, const char* name, const char* description, const char* password
);

/// Ends the create-frequency request.
///
/// A refusal (area or room 0) is reported; otherwise the console goes to the
/// new frequency with the dialog's password.
///
/// @param[in,out] console chat console
/// @param area_id area of the new frequency; 0 when refused
/// @param room_id the new frequency; 0 when refused
/// @param name frequency name
void chat_on_room_created(
    ChatConsole* console, uint32_t area_id, uint32_t room_id, const char* name
);

/// Clears the current room id and the player population count.
///
/// @param[in,out] console chat console
void chat_reset_room_counts(ChatConsole* console);

/// Closes whichever of the player pane, the create-frequency dialog and the frequency information
/// pane is open.
///
/// @param[in,out] console chat console
void chat_close_panes(ChatConsole* console);

/// Closes the console.
///
/// Empties the player and frequency lists, drops the whisper lock, closes
/// the panes, leaves the current frequency and writes the people lists back.
///
/// @param[in,out] console chat console
void chat_close(ChatConsole* console);

} // namespace oa::boneyards::screens
