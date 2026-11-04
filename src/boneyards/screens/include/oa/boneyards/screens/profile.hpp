// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Profile console (account, personal, allegiance, game preferences and local
// settings tabs), the battle game-mode list and the host-game setup panel.
#pragma once

#include "oa/boneyards/screens/widgets.hpp"
#include "oa/boneyards/service.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::boneyards::screens {

struct ChatSession;

// Persistent client settings (3.1c writes them to TotalA.ini).
struct SettingsStore {
    void* user;
    // Returns false when the value could not be stored.
    bool (*save_number)(void* user, const char* key, int32_t value);
    // Reads a stored value; null (or a missing key) yields the fallback.
    int32_t (*read_number)(void* user, const char* key, int32_t fallback);
    bool (*read_string)(void* user, const char* key, char* out, std::size_t capacity){};
    bool (*save_string)(void* user, const char* key, const char* value){};
};

inline constexpr const char* kUnitLimitKey = "UnitLimit";
inline constexpr const char* kConsoleSlideKey = "BYConsoleSlide";
inline constexpr const char* kSoundOffKey = "BYStopThatIncessantDroning";

inline constexpr int32_t kUnitLimitMin = 21;
inline constexpr int32_t kUnitLimitMax = 500;
inline constexpr int32_t kSlideStepsMin = 1;
inline constexpr int32_t kSlideStepsMax = 40;
inline constexpr int32_t kUnitLimitDefault = 250;
inline constexpr int32_t kSlideStepsDefault = 20;

inline constexpr std::size_t kProfileTabCount = 7;
static_assert(kProfileTabCount == kProfileCategoryCount, "a tab per Profile.def category");
inline constexpr std::size_t kProfileFieldCapacity = 0x80;
inline constexpr int32_t kProfileNameCharacters = kPlayerNameCharacters;
inline constexpr std::size_t kProfileNameCapacity =
    oa::boneyards::utf8::bytes_for(static_cast<std::size_t>(kProfileNameCharacters));
// The longest entry limit is the tag line's 0xff characters.
inline constexpr int32_t kProfileEntryCharacters = 0xff;
inline constexpr std::size_t kProfileEntryCapacity =
    oa::boneyards::utf8::bytes_for(static_cast<std::size_t>(kProfileEntryCharacters));

// Tabs of the Profile console list.
enum class ProfileTab : int32_t {
    account = 0,  // login name, status, email, password change
    personal = 1, // tag line, expansions, forum alias, ICQ
    allegiance = 2,
    game_preferences = 3,
    ladder = 4,
    awards = 5,
    local_settings = 6, // unit limit, console slide, background sound
};

// Account status codes (ProfileRecord.status).
enum class ProfileStatus : int32_t {
    active = 1,
    admin = 4,
    beta_tester = 5,
};

// One ladder standing of a profile: its template's Profile.def
// sort order and the template expanded with the ladder's fields.
struct LadderPage {
    int32_t sort;
    char* html; // null when no template matched or the expansion failed
};

// A player's profile, filled from the profile_info reply; string fields
// are null when the reply omitted them. The texts and
// pages profile_from_fields and ladder_profile_apply store are heap copies
// the record owns until profile_record_release.
struct ProfileRecord {
    LadderPage* ladder_pages; // list, in arrival order until the ladder page joins them
    int32_t ladder_page_count;
    int32_t ladder_page_capacity;
    const char* ladder_html;
    const char* awards_html;
    const char* general_html;        // pop-up template expansion
    const char* info_html;           // Boneyards-info template expansion
    int32_t user_id;                 // negative for a guest
    char name[kProfileNameCapacity]; // LOGINNAME
    int32_t user_icon;               // ICON, a User_Icons frame (rank)
    char allegiance;                 // 'A', 'C' or 'U'
    // ALLEGIANCE_LOCKED: -1 when the reply omitted it (the choice closes once
    // a side is set), 0 while the side can still change, 1 once it cannot.
    int32_t allegiance_locked = 0;
    const char* clan;              // CLAN
    int32_t status;                // ProfileStatus, USER_STATUS
    const char* email;             // EMAIL
    const char* tag_line;          // TAG
    bool has_core_contingency;     // HAS_CC is "Yes"
    bool core_contingency_set;     // HAS_CC is non-empty
    bool has_battle_tactics;       // HAS_BT is "Yes"
    bool battle_tactics_set;       // HAS_BT is non-empty
    const char* forum_alias;       // TAFORUM_ALIAS
    const char* email_alias;       // EMAIL_ALIAS
    const char* icq;               // ICQ_ALIAS
    const char* preferred_skill;   // PLAYERSKILL_TEXT
    const char* preferred_players; // NUMPLAYERS_TEXT
    const char* preferred_death;   // ONCMDDEATH_TEXT
    const char* favorite_map;      // FAVORITEMAP
    // The reply gave NUMPLAYERS, PLAYERSKILL or ONCOMDEATH; without it the
    // text above is a default.
    bool players_given;
    bool skill_given;
    bool death_given;
};

/// Sets the defaults a record holds until a reply fills it: every field empty, no side, the lock
/// omitted, Active status.
///
/// Profile.def is read through the service's profile_templates, which the
/// screens ask for where they build a record.
///
/// @param[out] record record to reset
void profile_record_init(ProfileRecord* record);

/// Frees the texts, pages and ladder list the record owns and returns it to its defaults.
///
/// @param[in,out] record record to empty
void profile_record_release(ProfileRecord* record);

// Longest field text the readers take from a reply, terminator included.
inline constexpr std::size_t kProfileTextCapacity = 0x400;

// A profile reply's fields, as the reply's property bag holds them.
struct ProfileFields {
    void* user;
    // Copies a field's text, truncated to capacity; false when absent.
    bool (*get_string)(void* user, const char* key, char* out, std::size_t capacity);
    bool (*get_int)(void* user, const char* key, int32_t* out);
    bool (*get_uint)(void* user, const char* key, uint32_t* out);
    // Stores a field so the page templates read it; may be null.
    void (*set_string)(void* user, const char* key, const char* value);
};

/// Fills a record from the profile_info reply and builds its pages.
///
/// Reads the user id, login name, rank icon, side, clan, status, email, tag
/// line, expansions, aliases and game preferences (writing their display
/// texts back for the templates), then builds the ladder page from the
/// ladder pages already added (none for an admin), the awards page from
/// AWARD_1.., and the pop-up and Boneyards-info templates expanded with the
/// reply. A record serves one request: release it before the next request's
/// ladder pages are added, or the old ones join them. Partial: the session's
/// refresh from the player's own record is the caller's.
///
/// @param[in,out] record record to fill
/// @param templates Profile.def's tables; may be null
/// @param fields the reply's fields; blank-able text fields holding a single space are written back
///     empty
void profile_from_fields(
    ProfileRecord* record, const ProfileTemplates* templates, const ProfileFields* fields
);

/// Adds a ladder_profile reply's page to the record's ladder list.
///
/// The page is the first Profile.def ladder template of that type and
/// subtype expanded with the ladder's fields.
///
/// @param[in,out] record record receiving the page
/// @param templates Profile.def's tables; may be null
/// @param ladder_id ladder type
/// @param ladder_subid ladder subtype
/// @param fields the ladder standing's fields
/// @return false when no template matched or the expansion failed
bool ladder_profile_apply(
    ProfileRecord* record,
    const ProfileTemplates* templates,
    int32_t ladder_id,
    int32_t ladder_subid,
    const ProfileFields* fields
);

/// Returns Profile.def's tables from the service, which reads them the first time a profile record
/// is built.
///
/// @param service client service; may be null
/// @return the tables, or null when the service has none
const ProfileTemplates* profile_read_templates(const BoneyardsService* service);

/// Labels a record's account status: "Guest" for a guest, else Active, Admin, Beta Tester or
/// Unknown.
///
/// @param record profile record
/// @return static label text
const char* profile_status_label(const ProfileRecord* record);

// User_Icons frames: the player's rank, or a staff role.
namespace user_icon {
inline constexpr int32_t core_private = 1; // Core ranks run 1..9, Private to Supreme Commander
inline constexpr int32_t core_high_commander = 8;
inline constexpr int32_t core_supreme_commander = 9;
inline constexpr int32_t arm_private = 0xa; // Arm ranks run 0xa..0x12
inline constexpr int32_t arm_high_commander = 0x11;
inline constexpr int32_t arm_supreme_commander = 0x12;
inline constexpr int32_t newsbot = 0x15;
inline constexpr int32_t arm_system_commander = 0x16;
inline constexpr int32_t sysop = 0x17; // titled "Yard Dog"
inline constexpr int32_t core_system_commander = 0x18;
// Frames 0x19..0x1e and 0x1f..0x24 repeat the Core and Arm ranks from
// Lieutenant (the fourth) up.
inline constexpr int32_t core_repeat_first = 0x19;
inline constexpr int32_t core_repeat_high_commander = 0x1d;
inline constexpr int32_t core_repeat_supreme_commander = 0x1e;
inline constexpr int32_t arm_repeat_first = 0x1f;
inline constexpr int32_t arm_repeat_high_commander = 0x23;
inline constexpr int32_t arm_repeat_supreme_commander = 0x24;
inline constexpr int32_t repeated_rank_offset = 3;
inline constexpr int32_t limit = 0x25; // valid icons are 1..0x24
} // namespace user_icon

/// Maps a User_Icons frame to its rank title.
///
/// @param user_icon User_Icons frame
/// @param short_form true for the short form ("Private"), false for the side-qualified form
///     ("Core Private")
/// @return static title text; empty for unranked icons
const char* rank_title(int32_t user_icon, bool short_form);

// The page a report view shows for a record page the reply omitted.
inline constexpr const char* kDefaultReportHtml =
    "<html><BODY><CENTER><b><font size=+2>Nothing to Report</b></font></CENTER><P></BODY></html>";

/// Returns the page a report view shows for a record page.
///
/// @param html the record's page; null when the reply omitted it
/// @return `html`, or kDefaultReportHtml for a null page
inline const char* profile_report_html(const char* html) {
    return html != nullptr ? html : kDefaultReportHtml;
}

// Colour of a name or rank label: the session font it is drawn in.
enum class SideColor : int32_t {
    no_side_tab = 0x00,  // Allegiance tab name of a member with no side, in small_light
    no_side_pane = 0x04, // player pane name and rank with no side, in small
    arm = 0x14,          // small_blue
    core = 0x18,         // small_red
};

// Insignia frames of widgets.gaf, the side logos.
namespace insignia_frame {
inline constexpr int32_t core = 0;
inline constexpr int32_t arm = 1;
inline constexpr int32_t none = -1; // no image
} // namespace insignia_frame

// Value an edit field is constructed with.
inline constexpr int32_t kEntryInitialValue = 1;

// A labelled edit field of a profile tab.
struct ProfileEntry {
    char text[kProfileEntryCapacity];
    int32_t max_length; // characters the edit accepts
    int32_t value;      // widget value
    bool shown;
    char mask; // '*' for password fields, 0 otherwise
};

/// Creates a labelled edit field, shown with the edit's initial value.
///
/// @param[out] entry field to create
/// @param max_length characters the edit accepts
/// @param masked whether the edit echoes '*'
void profile_entry_init(ProfileEntry* entry, int32_t max_length, bool masked);

// Contents of the tab controls, refreshed from the record.
struct ProfileTabControls {
    // Account tab.
    char login_name[kProfileNameCapacity]; // label
    const char* status;                    // label
    char email[kProfileFieldCapacity];     // label
    ProfileEntry password;
    ProfileEntry confirm;
    int32_t change_value; // CHANGE button
    bool change_shown;
    // Personal tab.
    ProfileEntry tag_line;
    int32_t core_contingency_value; // check box
    int32_t battle_tactics_value;
    ProfileEntry forum_alias;
    ProfileEntry icq;
    int32_t display_email_alias_value; // check box, set while EMAIL_ALIAS is non-empty
    // Allegiance tab.
    int32_t user_icon; // User_Icons frame
    char user_name[kProfileNameCapacity];
    SideColor name_color;
    int32_t logo_frame; // insignia_frame
    bool logo_shown;
    const char* side_name;
    bool side_shown;
    const char* rank;
    bool rank_shown;
    bool join_shown;    // "Join Forces of:"
    int32_t arm_value;  // radio
    int32_t core_value; // radio
    bool radios_shown;
    const char* allegiance_html;
    // Game preferences tab.
    RadioGroup skill;
    RadioGroup players;
    RadioGroup commander_death;
    ProfileEntry favorite_map;
    // Ladder and awards tabs.
    const char* ladder_html;
    const char* awards_html;
};

/// True while the Allegiance tab may still change the side.
///
/// A guest never may. When the reply omitted ALLEGIANCE_LOCKED the old rule
/// stays: only a member whose side is unset. 0 leaves the choice open after
/// a side is set. 1 closes it.
///
/// @param record profile record; null is closed
/// @return whether Arm or Core can be sent
inline bool profile_allegiance_open(const ProfileRecord* record) {
    if (record == nullptr || record->user_id < 0)
        return false;
    if (record->allegiance_locked < 0)
        return record->allegiance == 'U';
    return record->allegiance_locked == 0;
}

/// Shows the side mark, the side name and the rank, and the join radios, from the record.
///
/// Radios stay visible while the choice is open, and while it is locked, so a
/// locked side is shown with the choices disabled. A reply that omitted the
/// lock hides the radios once a side is set.
///
/// @param[in,out] tabs allegiance controls
/// @param record profile record
inline void profile_allegiance_show(ProfileTabControls* tabs, const ProfileRecord* record) {
    const bool guest = record->user_id < 0;
    const bool open = profile_allegiance_open(record);
    const bool side = !guest && (record->allegiance == 'A' || record->allegiance == 'C');
    const bool locked = !guest && record->allegiance_locked > 0;
    tabs->join_shown = open;
    tabs->radios_shown = open || locked;
    tabs->logo_shown = side;
    tabs->side_shown = side;
    tabs->rank_shown = side;
}

// Controls of the Profile console that post messages.
enum class ProfileControl : int32_t {
    tab_list, // item value is the tab index
    apply_button,
    reset_button,
    change_password_button,
};

struct ProfileConsole {
    const BoneyardsService* service;
    // The player's own user id is asked for, and APPLY sets its background
    // sound.
    ChatSession* session;
    SettingsStore settings;
    void* ui_user;
    void (*message_box)(void* user, const char* text);
    // Starts (running) or stops the Main Frame's activity light, which runs
    // while a request or submission is outstanding.
    void (*activity)(void* user, bool running);
    // Gives the running console layer new slide steps as soon as APPLY stores
    // them; null leaves it with the ones it read as it was made. Set after
    // profile_console_init, which clears it.
    void* slide_user;
    void (*set_slide_steps)(void* user, int32_t steps);
    // Stops every playing sound as APPLY turns the background sound off
    // (`on` false), and plays it on the alternate route as APPLY turns it on;
    // null does neither. Set after profile_console_init, which clears it.
    void* sound_user;
    void (*background_sound)(void* user, bool on);

    bool tab_created[kProfileTabCount];
    int32_t current_tab;              // -1 before the first selection
    bool busy;                        // transmitting or receiving
    bool tab_dirty[kProfileTabCount]; // per tab, account to local settings: edits not yet applied
    int32_t unit_limit;               // stored value
    int32_t slide_steps;
    bool sound_off;
    int32_t tab_list_value; // widget value
    int32_t tab_list_row;   // current row, by the tab it lists
    int32_t apply_value;
    int32_t reset_value;
    char status[0x40]; // label text
    bool status_visible;
    const ProfileRecord* record; // &profile from the first request
    ProfileRecord profile;       // what the reply to the console's request fills
    bool password_fields_shown;
    bool tab_visible[kProfileTabCount];
    ProfileTabControls tabs;
    // Counts each tab's refreshes from the record, so the windows showing
    // the controls know when to take their contents again.
    uint32_t tab_revision[kProfileTabCount];
    // Profile.def's tables: the profile_category_info description of the
    // current tab is wrapped into the five description labels.
    const ProfileTemplates* templates;
    const char* tab_description;

    // Field contents.
    char unit_limit_text[kProfileFieldCapacity];
    char slide_steps_text[kProfileFieldCapacity];
    bool sound_off_checked;
    ProfileEdits edits;
    int32_t selected_field; // edit field whose text was selected after an error, -1 none

    // The limit APPLY last gave the game. The engine's unit limit preference is
    // set through profile_on_unit_limit; this records the same value.
    int16_t game_unit_limit;
};

/// Hands APPLY's stored unit limit to the game.
///
/// Null, the state until an extension installs one, does nothing. APPLY calls
/// it only after the Boneyards' own UnitLimit value has been stored. The call
/// is process-wide: opening the console does not clear it.
///
/// @param apply called with the stored limit, in units per player; null clears it
void profile_on_unit_limit(void (*apply)(int32_t units_per_player));

/// Binds the Profile console, opens the account tab and requests the player's own record.
///
/// The record a previous opening left is freed first.
///
/// @param[in,out] console console to bind
/// @param service service the requests go to
/// @param session session record: the player's own user id is asked for, and APPLY sets its
///     background sound
/// @param settings persistent client settings
/// @param ui_user context handed to `message_box` and `activity`
/// @param message_box shows a message box; may be null
/// @param activity starts or stops the Main Frame's activity light; may be null
void profile_console_init(
    ProfileConsole* console,
    const BoneyardsService* service,
    ChatSession* session,
    SettingsStore settings,
    void* ui_user,
    void (*message_box)(void* user, const char* text),
    void (*activity)(void* user, bool running)
);

/// Shows a tab's controls.
///
/// The account tab shows CHANGE, or the password fields once opened. The
/// allegiance tab shows the side when one is set, and the Arm and Core
/// choices while the side can still change or once it is locked.
///
/// @param[in,out] console Profile console
/// @param tab ProfileTab index; out-of-range tabs are ignored
void profile_show_tab(ProfileConsole* console, int32_t tab);

/// Hides every control of a tab.
///
/// @param[in,out] console Profile console
/// @param tab ProfileTab index; out-of-range tabs are ignored
void profile_hide_tab(ProfileConsole* console, int32_t tab);

/// Switches to a tab, creating its controls on first use; the tab list row and category description
/// follow.
///
/// @param[in,out] console Profile console
/// @param tab ProfileTab index; the current tab or an out-of-range one changes nothing
void profile_select_tab(ProfileConsole* console, int32_t tab);

/// Creates a tab's controls the first time it is shown, all shown, then refreshes them.
///
/// @param[in,out] console Profile console
/// @param tab ProfileTab index; out-of-range or already created tabs are ignored
void profile_create_tab(ProfileConsole* console, int32_t tab);

/// Refreshes a created tab's controls from the record and counts the refresh in its revision.
///
/// Guests get disabled fields and hidden allegiance controls; the local
/// settings tab rereads its values from the settings store.
///
/// @param[in,out] console Profile console
/// @param tab ProfileTab index; out-of-range or uncreated tabs are ignored
void profile_update_tab(ProfileConsole* console, int32_t tab);

/// Validates and saves the local settings, then sends the account and profile changes as one
/// property bag.
///
/// The unit limit must be 21..500 and the slide steps 1..40; a rejected
/// entry shows a message, switches to its tab and selects the field. A stored
/// unit limit is given to the game (profile_on_unit_limit). New slide steps
/// reach the running console layer at once, and the background sound check
/// sets the session's background sound.
///
/// The bag, from the tabs with changes: PASSWORD when either password field
/// holds text, which needs a matching confirmation without a ' character;
/// the personal tab's TAG, TAFORUM_ALIAS and ICQ_ALIAS when they differ from
/// the record (an emptied one as a single space), HAS_CC and HAS_BT ("Yes"
/// or "No") when the check differs from the record or the reply did not give
/// it, and EMAIL_ALIAS when its check differs from whether the record has an
/// alias (the record's email, or a single space); ALLEGIANCE while the side
/// can still change ('U', 'ARM' or 'CORE', and a different side when one is
/// already set and ALLEGIANCE_LOCKED is 0); and the game preferences' NUMPLAYERS
/// ("Doesn't Matter" as "1"), PLAYERSKILL (Novice to Expert as "1" to "3",
/// else "0") and ONCOMDEATH (Continues, Ends and Deathmatch as "0" to "2",
/// else "3") unless they are the record's and the reply gave them, and
/// FAVORITEMAP as a text field. Only a bag with a field is sent; once it is
/// on its way the console shows "TRANSMITTING DATA..." with the activity
/// light running, and the answer reaches profile_on_submitted. A stored
/// change of the background sound stops the playing sounds as it goes off
/// and plays it as it comes on (ProfileConsole::background_sound).
///
/// @param[in,out] console Profile console
void profile_apply(ProfileConsole* console);

/// Recomputes a created tab's dirty flag from its controls.
///
/// A tab is dirty when either password field holds text, an open allegiance
/// choice differs from the record, a personal or preference field of a
/// member differs from the record, or the local settings differ from the
/// stored values. Ladder and awards are never dirty.
///
/// @param[in,out] console Profile console
/// @param tab ProfileTab index; out-of-range or uncreated tabs are ignored
void profile_check_tab(ProfileConsole* console, int32_t tab);

/// Rechecks the current tab, then enables APPLY and RESET while any tab is dirty and no transfer is
/// running.
///
/// @param[in,out] console Profile console
void profile_update_buttons(ProfileConsole* console);

/// Handles the tab list, APPLY, RESET and CHANGE (password) controls.
///
/// @param[in,out] console Profile console
/// @param control control clicked
/// @param value the selected tab, for the tab list
void profile_on_control(ProfileConsole* console, ProfileControl control, int32_t value);

/// Replaces the record with a fresh one and requests the player's own profile into it.
///
/// Shows "RECEIVING DATA..." with the activity light running.
///
/// @param[in,out] console Profile console
void profile_begin_receive(ProfileConsole* console);

/// Adds a ladder standing of the console's profile reply to its record, when it has one.
///
/// @param console the ProfileConsole
/// @param ladder_id ladder type
/// @param ladder_subid ladder subtype
/// @param fields the standing's fields
void profile_on_ladder(
    void* console, int32_t ladder_id, int32_t ladder_subid, const ProfileFields* fields
);

/// Takes the console's profile reply.
///
/// Stops the activity light, then fills the record and refreshes every
/// created tab from it, or shows "ACCESS FAILED" when it could not be read.
///
/// @param console the ProfileConsole
/// @param ok whether the request succeeded
/// @param fields the profile's fields
void profile_on_received(void* console, bool ok, const ProfileFields* fields);

/// Ends a profile submission, reporting a failure, and reloads the record.
///
/// @param[in,out] console Profile console
/// @param ok whether every change was stored
void profile_on_submitted(ProfileConsole* console, bool ok);

// Battle game modes and their wire ids.
enum class GameMode : int32_t {
    for_fun = 0,
    duel = 1,
    winner_takes_all = 2,
    galactic_war = 4,
    war_training = 7,
};

struct GameModeEntry {
    const char* name;
    GameMode mode;
};

inline constexpr std::size_t kGameModeCount = 5;

/// Lists the battle game modes in display order: For Fun, Duel, Winner Takes All, Galactic War, War
/// Training.
///
/// @param[out] out receives the entries
/// @param capacity entries `out` holds
/// @return number of entries written
std::size_t game_mode_list(GameModeEntry* out, std::size_t capacity);

// The host-game setup panel.
struct GameSetupPanel {
    const BoneyardsService* service;
    void* ui_user;
    void (*message_box)(void* user, const char* text);
    uint32_t area_id;
    char player_allegiance;         // SessionRecord.allegiance: 'A', 'C' or 'U'
    char planet_affiliation;        // the selected planet's side, 0 when none
    int32_t planet_players_limit;   // the selected planet's player_limit
    int32_t planet_commander_death; // the selected planet's commander_death
    bool has_planet;
    char game_name[0x20];
    char password[0x20];
    GameModeEntry types[kGameModeCount]; // the game-type list
    std::size_t type_count;
    int32_t selected_type;
    RadioGroup skill;
    RadioGroup players;
    RadioGroup commander_death;
    bool visible;
};

/// Builds the host-game setup panel's radio groups (Player Skill, Number of Players, On Commander
/// Death) and its game types.
///
/// @param[out] panel panel to build
/// @param service service the host request goes to
/// @param ui_user context handed to `message_box`
/// @param message_box shows a message box; may be null
void game_setup_init(
    GameSetupPanel* panel,
    const BoneyardsService* service,
    void* ui_user,
    void (*message_box)(void* user, const char* text)
);

/// Rebuilds the game-type list for the selected planet and the player's allegiance.
///
/// The three open modes are always listed, plus Galactic War on a contested
/// planet for a member of a side, or War Training on a planet of the
/// player's side.
///
/// @param[in,out] panel setup panel
void game_setup_update_types(GameSetupPanel* panel);

/// Applies a game-type choice to the player-count and commander-death groups.
///
/// A Galactic War or War Training choice without a selected planet shows a
/// message instead.
///
/// @param[in,out] panel setup panel
/// @param index game-type list index; out-of-range indices are ignored
void game_setup_select_type(GameSetupPanel* panel, int32_t index);

/// Sends the host-game request built from the panel (START) and closes the panel.
///
/// @param[in,out] panel setup panel
/// @return false when no game type is selected
bool game_setup_start(GameSetupPanel* panel);

} // namespace oa::boneyards::screens
