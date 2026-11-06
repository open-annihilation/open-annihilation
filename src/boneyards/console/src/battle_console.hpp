// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle console the Battle button opens: the planet's battles grouped
// by game mode on the left with HOST under them, the planet intelligence log
// on the right, and the Host, Join summary and Scoreboard panes it opens
// over the log. The battles themselves are the navigation's
// screens::BattleList, which these windows show and hand what the player
// does.
#pragma once

#include "console_state.hpp"
#include "oa/boneyards/screens/battle_list.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"

namespace oa::boneyards::console {

struct MatchControlPanel;

// Battle console (BattleList console): the "BATTLES" title over the
// BattleList, HOST under it, "PLANET INTELLIGENCE" over the PlanetIntelList
// log, and with matchmaking enabled a MatchControlPanel under the list.
struct BattleConsole {
    Window base;
    MainFrame* frame;
    Window* battle_button;            // the Main Frame's Battle button
    screens::ByListBox* list;         // BattleList
    Window* host_button;              // HOST
    Window* intel_title;              // "PLANET INTELLIGENCE"
    screens::ByListBox* intel;        // PlanetIntelList
    MatchControlPanel* match_control; // null without matchmaking
    InfoPane* panes[3];               // by screens::BattlePanel
    StatusWidget* owner;
};

// Host pane (HOST BATTLE): the game name and password entries, the
// game-type list, the Player Skill, Number of Players and On Commander Death
// radio groups and START, enabled only while the game has a name.
struct HostBattlePane {
    InfoPane pane;
    SingleEdit* game_name;
    SingleEdit* password;
    screens::ByListBox* types;
    screens::ByRadioGroup* skill;
    screens::ByRadioGroup* players;
    screens::ByRadioGroup* commander_death;
    Window* start;
    // Set while the pane is hidden, so the next time it shows the game name
    // takes the keyboard once. A later pulse leaves the keyboard where it is.
    uint8_t awaiting_keyboard = 0;
};

inline constexpr int32_t k_battle_seats = 4;

// Join summary (BATTLE INTELLIGENCE of a forming battle): the battle's name,
// its game type, the host and up to three more players with their rank
// icons, the skill and commander-death fields, a password entry for a
// password game, and JOIN.
struct JoinSummaryPane {
    InfoPane pane;
    SingleEdit* password;
    Window* password_label;
    Window* name;
    screens::LobbyField* game_type;
    Window* seat_captions[k_battle_seats]; // Host:, Player 2: to Player 4:
    Window* seat_icons[k_battle_seats];
    Window* seat_names[k_battle_seats];
    screens::LobbyField* skill;
    screens::LobbyField* commander_death;
    Window* join;
    Handle blocked_icon; // User_Icons frame 1, on a seat the game does not have
    // Set while the pane is hidden, so a password entry takes the keyboard
    // once when it appears. A later pulse leaves the keyboard where it is.
    uint8_t password_pending = 0;
};

// One player's row of the Scoreboard: the rank icon, the name and the
// Kills, Losses, Energy, Metal and Score columns.
struct ScoreboardSeat {
    Window* icon;
    Window* name;
    Window* kills;
    Window* losses;
    Window* energy;
    Window* metal;
    Window* score;
};

// Scoreboard (BATTLE INTELLIGENCE of a battle in progress): the battle's
// name over the four players' seats, ruled into columns.
struct ScoreboardPane {
    InfoPane pane;
    Window* name;
    ScoreboardSeat seats[k_battle_seats];
    // The list report messages go to as "Cartman" rows; the pane never
    // builds it, so they are not shown.
    screens::ByListBox* messages;
};

/// Creates the battle console in the green layer's rectangle; it builds itself hidden.
///
/// Makes the battle list the current Boneyards screen and has its
/// intelligence lines, report messages and panel changes reach this console.
///
/// @param owner status widget
/// @param frame the Main Frame window
/// @param x left edge in frame pixels
/// @param y top edge in frame pixels
/// @param width console width in pixels
/// @param height console height in pixels
/// @return the console window
Window* battle_console_create(
    StatusWidget* owner, Window* frame, int32_t x, int32_t y, int32_t width, int32_t height
);

/// Opens the battle console (its open slot).
///
/// Takes the Main Frame's Host, Scoreboard and Join summary panes, each
/// making room for itself by moving the intelligence log and its title
/// down, shows the console, opens again whichever of those panes shows,
/// lights the Battle button and enters the planet's battles with the
/// activity light running.
///
/// @param console the battle console
void battle_console_open(Window* console);

/// Closes the battle console (its close slot).
///
/// Closes the shown pane (the Host pane, else the Scoreboard, else the Join
/// summary), puts the Battle button back on its normal face, empties the
/// mode categories and the intelligence log, hides the console, marks the
/// rows for a rebuild and leaves the planet's battles with the activity
/// light running.
///
/// @param console the battle console
void battle_console_close(Window* console);

/// Builds the Host pane hidden in the Main Frame.
///
/// @param[in,out] frame Main Frame
/// @param title pane title
/// @return the pane
InfoPane* host_battle_pane_create(MainFrame* frame, const char* title);

/// Builds the Join summary pane hidden in the Main Frame, showing no battle.
///
/// @param[in,out] frame Main Frame
/// @param title pane title
/// @return the pane
InfoPane* join_summary_pane_create(MainFrame* frame, const char* title);

/// Builds the Scoreboard pane hidden in the Main Frame, showing no battle.
///
/// @param[in,out] frame Main Frame
/// @param title pane title
/// @return the pane
InfoPane* scoreboard_pane_create(MainFrame* frame, const char* title);

/// Adds a report message of the battle the Scoreboard shows as a two-label row with "Cartman" as
/// its first label, in the Scoreboard's message list; without that list, which the pane never
/// builds, nothing is shown.
///
/// @param[in,out] pane the Scoreboard
/// @param message the report message
void scoreboard_pane_add_message(ScoreboardPane* pane, const char* message);

// Faces of the matchmaking panel, as its state record keeps them.
namespace match_face {
inline constexpr int32_t minimized = 0;
inline constexpr int32_t active = 1;
inline constexpr int32_t deactivated = 2;
} // namespace match_face

// Matchmaking panel under the battle list (MatchControlPanel): the
// MATCHMAKING title with its minimize button, DEACTIVATE (ACTIVATE on the
// active face), SETTINGS and a content list, in one of three faces the
// navigation's matchmaking state keeps between consoles.
struct MatchControlPanel {
    Window base;
    screens::MatchmakingState* state;
    Handle images[3]; // by face: its background, ruled along the top
    int32_t heights[3];
    Window* title;    // MATCHMAKING
    Window* minimize; // Minimize_Button, Minimize_Button_180
    Window* activate; // DEACTIVATE or ACTIVATE
    Window* settings; // SETTINGS
    screens::ByListBox* content;
    StatusWidget* owner;
};

/// Creates the matchmaking panel along the bottom of the battle console and builds it in the face
/// its state keeps.
///
/// @param owner status widget
/// @param state the navigation's matchmaking state
/// @param console the battle console
/// @return the panel
MatchControlPanel*
match_control_panel_create(StatusWidget* owner, screens::MatchmakingState* state, Window* console);

/// Shows the panel's face: minimized (0x123 rows below the console's top), active (0x10b) or
/// deactivated (0xd6).
///
/// @param[in,out] panel the matchmaking panel
/// @param face match_face value
void match_control_panel_show(MatchControlPanel* panel, int32_t face);

} // namespace oa::boneyards::console
