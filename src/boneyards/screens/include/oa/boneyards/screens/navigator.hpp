// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards screens' models and the client events that drive them. The
// console package enters a screen's model through boneyards_open as it
// builds the screen's view; the Main Frame decides which screen is open.
#pragma once

#include "oa/boneyards/screens/battle_list.hpp"
#include "oa/boneyards/screens/chat.hpp"
#include "oa/boneyards/screens/galaxy.hpp"
#include "oa/boneyards/screens/login.hpp"
#include "oa/boneyards/screens/profile.hpp"
#include "oa/boneyards/service.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::boneyards::screens {

// Screen ids in the application registry range reserved for these screens.
enum class ByScreen : uint16_t {
    none = 0,
    login = 0x700,
    chat_frequencies = 0x701,
    profile_console = 0x702,
    battle_list = 0x703,
    game_setup = 0x704,
};

// The battle console's matchmaking panel, kept between consoles: the face
// it shows (0 minimized, 1 active, 2 deactivated) and the flag its SETTINGS
// button flips.
struct MatchmakingState {
    int32_t face;
    bool settings_flag;
};

// Frontend services the screens reach outside their own models; every
// entry may be null.
struct ScreensHost {
    void* user;
    // Starts (running) or stops the Main Frame's activity light.
    void (*activity_light)(void* user, bool running);
};

struct BoneyardsScreens {
    BoneyardsService service;
    ScreensHost host; // set after boneyards_screens_init
    ChatSession session;
    char allegiance;              // SessionRecord.allegiance: 'A', 'C' or 'U'
    char allegiance_previous = 0; // 'A' or 'C' from the dossier, 0 when it names none
    ByScreen current;
    char last_message[0x100]; // most recent message box text (also shown as a
                              // Boneyards message box while a desktop is installed)
    int32_t message_count;
    LoginDialog login;
    ChatConsole chat;
    ProfileConsole profile;
    BattleList battles;
    GameSetupPanel setup;
    GalaxyView galaxy; // the war map behind the Main Frame
    MatchmakingState matchmaking;
};

/// Initialises every screen against the service.
///
/// Frees what an earlier initialisation held first, so the screens must start
/// zeroed the first time. The session's background sound is on unless the
/// BYStopThatIncessantDroning setting is set.
///
/// @param[in,out] screens screens to reset
/// @param service client service; null or null entries become offline answers
/// @param settings settings store the Profile console reads and writes
void boneyards_screens_init(
    BoneyardsScreens* screens, const BoneyardsService* service, SettingsStore settings
);

/// Frees what the screens hold on the heap: the battle list's records and rows and the Profile
/// console's record.
///
/// @param[in,out] screens screens to empty
void boneyards_screens_release(BoneyardsScreens* screens);

/// Enters a screen's model and makes it the current screen.
///
/// Chat Frequencies is only made current: its console enters a frequency
/// when the green has slid over the map and it opens.
///
/// @param[in,out] screens Boneyards screens
/// @param screen screen to open; none leaves the current one
/// @return the current screen
ByScreen boneyards_open(BoneyardsScreens* screens, ByScreen screen);

/// Ends a login attempt on the dialog.
///
/// A completed login leaves no screen current: the Main Frame closes the
/// login console once the dossier has loaded.
///
/// @param[in,out] screens Boneyards screens
/// @param connected whether the login succeeded
void boneyards_on_login_result(BoneyardsScreens* screens, bool connected);

/// Shows a client status line in the login dialog.
///
/// @param[in,out] screens Boneyards screens
/// @param text status line
void boneyards_on_login_status(BoneyardsScreens* screens, const char* text);

/// Returns the login dialog to idle after the client ended the attempt.
///
/// @param[in,out] screens Boneyards screens
void boneyards_on_login_reset(BoneyardsScreens* screens);

/// Updates the identity the chat session uses from a session report.
///
/// Once the dossier has loaded during a login attempt, the login completes.
/// The record's area is the home planet from login. It becomes the current
/// planet only while the map has none selected; a later record does not
/// replace a planet the player has already chosen.
///
/// @param[in,out] screens Boneyards screens
/// @param session session record; the account flags are the top byte of its user id
void boneyards_on_session(BoneyardsScreens* screens, const SessionRecord* session);

/// Shows a client notice as a message box.
///
/// @param[in,out] screens Boneyards screens
/// @param text notice text
void boneyards_on_notice(BoneyardsScreens* screens, const char* text);

/// Shows a message-box alert as a message box; the console's Main Frame handles every kind.
///
/// @param[in,out] screens Boneyards screens
/// @param alert system alert; other kinds are ignored here
void boneyards_on_system_alert(BoneyardsScreens* screens, const AlertReport* alert);

/// Applies the planets' allegiances to the galaxy view.
///
/// @param[in,out] screens Boneyards screens
/// @param allegiances one allegiance letter per planet
void boneyards_on_allegiances(BoneyardsScreens* screens, const char* allegiances);

/// Applies the planets' activity digits to the galaxy view.
///
/// @param[in,out] screens Boneyards screens
/// @param digits one activity digit per planet
void boneyards_on_activity(BoneyardsScreens* screens, const char* digits);

/// Applies an area's census to the galaxy view.
///
/// A census for the current area also refreshes the territory summary.
///
/// @param[in,out] screens Boneyards screens
/// @param report census of one area
/// @return whether the planet is on the map
bool boneyards_on_census(BoneyardsScreens* screens, const CensusReport* report);

/// Applies a room-list row to the Chat Frequencies console while it exists.
///
/// The console exists from its opening until another Boneyards screen
/// replaces it; the callbacks return early while it does not, as in 3.1c.
///
/// @param[in,out] screens Boneyards screens
/// @param change PlayerListChange value
/// @param room the row
/// @param last whether the row ends the listing
void boneyards_on_room_listed(
    BoneyardsScreens* screens, int32_t change, const RoomListing* room, bool last
);

/// Applies a row of the entered room's area to the Chat Frequencies console while it exists.
///
/// @param[in,out] screens Boneyards screens
/// @param change PlayerListChange value
/// @param room the row
void boneyards_on_room_updated(BoneyardsScreens* screens, int32_t change, const RoomListing* room);

/// Applies the answer to entering a room to the Chat Frequencies console while it exists.
///
/// @param[in,out] screens Boneyards screens
/// @param result 0 entered, anything else failed
void boneyards_on_room_entered(BoneyardsScreens* screens, int32_t result);

/// Applies a member of the entered room to the Chat Frequencies console while it exists.
///
/// @param[in,out] screens Boneyards screens
/// @param change PlayerListChange value
/// @param user_id member's user id
/// @param name member's name
/// @param latency member's latency
/// @param icon member's user icon, the rank icon's User_Icons frame
/// @param rank rank reported with the member
/// @param allegiance allegiance reported with the member; null keeps none
void boneyards_on_room_member(
    BoneyardsScreens* screens,
    int32_t change,
    uint32_t user_id,
    const char* name,
    int32_t latency,
    int32_t icon,
    int32_t rank = 0,
    const char* allegiance = nullptr
);

/// Applies a line said in the entered room to the Chat Frequencies console while it exists.
///
/// A line from the server names no member and is not shown. A listed speaker's line is shown,
/// with the [SysOp] tag when that member's rank icon is the SysOp frame.
///
/// @param[in,out] screens Boneyards screens
/// @param user_id speaker; 0 for a line from the server
/// @param text the line
/// @param from_server true when the line names no user
void boneyards_on_chat_message(
    BoneyardsScreens* screens, uint32_t user_id, const char* text, bool from_server = false
);

/// Resets the room counts of the Chat Frequencies console, while it exists, once a room is left.
///
/// @param[in,out] screens Boneyards screens
/// @param failed whether the leave failed; not read
void boneyards_on_room_left(BoneyardsScreens* screens, bool failed);

/// Hands a created frequency to the create-frequency dialog while the Chat Frequencies console
/// exists.
///
/// @param[in,out] screens Boneyards screens
/// @param area_id area of the new frequency; 0 when refused
/// @param room_id room id of the new frequency; 0 when refused
/// @param name frequency name
void boneyards_on_room_created(
    BoneyardsScreens* screens, uint32_t area_id, uint32_t room_id, const char* name
);

/// Stops the activity light once entering or leaving the battle list is answered.
///
/// @param[in,out] screens Boneyards screens
/// @param failed whether the request failed; not read
void boneyards_on_battles_done(BoneyardsScreens* screens, bool failed);

/// Adds, refreshes or removes a battle of the current area in the battle console's list.
///
/// @param[in,out] screens Boneyards screens
/// @param change list change code
/// @param game_id battle id
/// @param name battle name
/// @param type game type
/// @param forming whether the battle is still forming
/// @param host_code host code the list reports
void boneyards_on_battle_info(
    BoneyardsScreens* screens,
    int32_t change,
    const char* game_id,
    const char* name,
    int32_t type,
    int32_t forming,
    int32_t host_code
);

/// Adds an intelligence line to the battle console's list.
///
/// @param[in,out] screens Boneyards screens
/// @param game_id battle the line is about; not read
/// @param text the line
void boneyards_on_intel(BoneyardsScreens* screens, const char* game_id, const char* text);

/// Applies a battle's intelligence to the battle console's list.
///
/// @param[in,out] screens Boneyards screens
/// @param intel the battle's intelligence
void boneyards_on_battle_intel(BoneyardsScreens* screens, const BattleIntel* intel);

/// Applies one line of a battle's report to the battle console's list.
///
/// @param[in,out] screens Boneyards screens
/// @param report the report line
void boneyards_on_battle_report(BoneyardsScreens* screens, const BattleReport* report);

/// Readies the Host panel's game setup for the session, then lists its game types.
///
/// Takes the current area, the player's allegiance and the selected planet's
/// side, player limit and commander death. 3.1c reads the selected planet
/// and the allegiance as the panel builds its type list and applies a type;
/// the panel keeps them from its opening.
///
/// @param[in,out] screens Boneyards screens
void boneyards_prepare_game_setup(BoneyardsScreens* screens);

/// Builds an event sink that forwards the client events to the functions above.
///
/// @param screens Boneyards screens the events reach
/// @return the event sink
ServiceEvents boneyards_screens_events(BoneyardsScreens* screens);

} // namespace oa::boneyards::screens
