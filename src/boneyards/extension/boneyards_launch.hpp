// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards' part of a network game, built on network play's API
// (oa/app/netgame/extension_api.hpp): how its launch reply fills network play's
// launch block and what it applies, the frontend's questions about a
// launched battle, the bridge its console calls for a launched battle, the
// match report's uploads to the Boneyards, the answers to network play's
// hooks (the console's Page, a failed launched join) and the engine's
// questions after a Boneyards launch (the label the match's return names,
// the stored password).
#pragma once

#include "boneyards_exit_prompt.hpp"

#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/service.hpp"
#include "oa/app/netgame/extension_api.hpp"
#include "oa/boneyards/reporter.hpp"
#include "oa/ui/frontend_multiplayer/launch_block.hpp"
#include "oa/ui/screen_registry.hpp"

#include <cstdint>
#include <string_view>

namespace oa::boneyards {
struct OverlayLaunch;
}

namespace oa::app {

// The Boneyards overlay's status widget (console_screens.cpp); null before
// the overlay is created.
oa::boneyards::console::StatusWidget* boneyards_console_status_widget();

/// Returns the faction an allegiance names by its first letter: 'A' Arm, 'C' Core.
///
/// @param allegiance the launch reply's allegiance; null names none
/// @return an oa::ui::frontend_multiplayer::launch::faction value; faction::none for any other
[[nodiscard]] int32_t boneyards_faction_of_allegiance(const char* allegiance) noexcept;

// The provider label a Boneyards launch writes into the launch block, which
// the match's return names (boneyards_return_label).
inline constexpr const char* kBoneyardsProviderLabel = "Boneyards";

/// Fills network play's launch block from a Boneyards launch reply.
///
/// Both connection type fields become TCP/IP, hosting follows the creator
/// flag, the time limit and pfs become 0, the provider label "Boneyards"
/// (kBoneyardsProviderLabel), and the faction comes from the allegiance's
/// first letter (boneyards_faction_of_allegiance). The session name,
/// mission and address keep at most 63 characters, the password and user
/// name 31; a null text stores an empty one. The reply's game id and result
/// are not stored.
///
/// @param[in,out] block block to fill
/// @param launch the launch_game reply
/// @param user_name the player's Boneyards name; null stores an empty one
void online_launch_fill_boneyards(
    oa::ui::frontend_multiplayer::launch::LaunchBlock& block,
    const oa::boneyards::LaunchReport& launch,
    const char* user_name
) noexcept;

// What the Boneyards' part of a launched battle reads and keeps besides
// network play's launch block.
struct BoneyardsLaunch {
    // Reads the planet of the launched battle and its game id; null reads
    // them from the Boneyards console's status widget. The planet is the one
    // the launch reply named, not the planet the Main Frame has selected.
    bool (*battle_ids)(uint32_t* area_id, const char** game_id){};
    // Reads the planet's name and the sector's name of the launched battle,
    // as the galactic map named them when the launch reply came; null reads
    // them from the Boneyards console's status widget.
    bool (*battle_place)(const char** planet, const char** sector){};
    // The Boneyards box "Exit to the system?", which a launch removes; null
    // removes nothing.
    ExitPrompt* exit_prompt{};
};

/// Installs what the Boneyards' part of a launched battle reads; the hooks below read it.
///
/// @param launch where the area and game ids come from, and the box a launch removes
void bind_boneyards_launch(const BoneyardsLaunch& launch) noexcept;

/// Fills network play's launch block from a launch reply and applies it, as the Boneyards does before the battle.
///
/// A simulation the Boneyards does not launch (boneyards_current_data_verdict)
/// is not applied: a message says so (boneyards_data_refusal_message) and
/// the block is left as it was. A launch already applied keeps its report.
/// Otherwise the block (oa::app::netgame::extension_api::launch_block) takes
/// the reply's values and the player's Boneyards name
/// (online_launch_fill_boneyards), then the launch is applied
/// (oa::app::netgame::extension_api::apply_launch): the name becomes the
/// frontend Game's nickname, the connection selection is asked to select the
/// multiplayer map list, a request to end the program opens the exit
/// confirmation until the game next sets an application mode, and the launch
/// is active until the Boneyards ends it. The Boneyards box is removed
/// (BoneyardsLaunch::exit_prompt).
///
/// @param context unused
/// @param launch the launch_game reply; null fills nothing
/// @param player_name the player's Boneyards name
void on_boneyards_launch(
    void* context, const oa::boneyards::LaunchReport* launch, const char* player_name
) noexcept;

/// Returns the bridge the Boneyards console calls for a launched battle.
///
/// Before the battle is entered the console asks whether this simulation
/// may be launched (LaunchBridge::launch_permitted). A launch reply then
/// fills the block and applies the launch (on_boneyards_launch); the end of
/// the launched battle ends it
/// (oa::app::netgame::extension_api::end_launch); the connection type is the
/// block's; the Boneyards' lines go to the battle
/// (oa::app::netgame::extension_api::post_battle_line), and the chat ring empties.
/// YES in the Boneyards box "Exit to the system?" leaves the game with the
/// disconnect reason (oa::app::netgame::extension_api::leave_game).
///
/// @return the bridge
[[nodiscard]] oa::boneyards::console::LaunchBridge boneyards_launch_bridge() noexcept;

/// Answers a join the launch asked for that failed (oa::app::netgame::extension_api::Hooks::join_failed).
///
/// The text shows in a Boneyards message box and every playing sound stops.
/// The leave is not sent here: the player is still joining. The return to
/// the Boneyards sends it once, as the battle room's back button does.
///
/// @param context unused
/// @param text the host-not-found text
void boneyards_join_failed(void* context, const char* text) noexcept;

/// Carries this battle through the relay when the launch names an overlay.
///
/// A launch without an overlay installs nothing. A route already installed
/// is removed first. When the relay cannot be opened, the status line
/// "Transmision Relay Failure" is shown and no route is installed.
///
/// @param launch the overlay fields of this battle
void boneyards_install_overlay_route(const oa::boneyards::OverlayLaunch& launch) noexcept;

/// Removes the route installed for the battle, if one is installed.
void boneyards_remove_overlay_route() noexcept;

/// Drops the battle tunnel because the player has left the battle.
///
/// A player who is still watching a battle that goes on does not take this path.
/// Neither does a battle that is still being set up: tearing down whatever
/// match was there, including none, is how a new battle starts.
void boneyards_match_ended() noexcept;

/// Tells whether a match event is the player leaving the battle.
///
/// `finished` and `left` are. `torn_down` is not: a new battle's setup
/// destroys any match already there, including none, before that battle
/// starts, and a real leave has already sent `left`. `watching_kept` is not.
///
/// @param event a MatchEvent value
/// @return true for finished and left
[[nodiscard]] bool boneyards_match_event_leaves(uint8_t event) noexcept;

/// Drops the battle tunnel because the Boneyards connection was lost.
///
/// A battle still on keeps its tunnel, from the battle room through the load
/// into the match and while that match is played. Losing the Boneyards does
/// not end a battle the player has not left.
///
/// @param match_still_played true while that battle has not been left
void boneyards_overlay_lost(bool match_still_played) noexcept;

/// Drops the battle tunnel as the runtime that installed it is released.
void boneyards_runtime_released() noexcept;

/// Advances the relay connection without waiting.
void boneyards_pump_overlay_route() noexcept;

/// Pages a user through Rover (oa::app::netgame::extension_api::Hooks::page).
///
/// @param context unused
/// @param user the user paged
/// @param text the message
void boneyards_page(void* context, const char* user, const char* text) noexcept;

/// Binds the frontend's questions about a battle the Boneyards launched to the dispatcher (query_register).
///
/// Whether a launch is active (oa::app::netgame::extension_api::launch_active)
/// answers context_flags, the Boneyards' network check
/// adapter_lookup_needed, and the lobby-launch check
/// (generate_default_game_name) answers 0, as no lobby program launches the
/// native game. Network play answers the states' questions about joining
/// and leaving a session.
///
/// @param[in,out] registry the runtime's registry
void register_boneyards_queries(oa::app::ScreenRegistry* registry) noexcept;

/// Fills the match report's uploads to the Boneyards (boneyards_reporter_hooks).
///
/// Settings, players, scores and chat lines go to Rover with the Boneyards'
/// area id and the launch's game id. The other entries of the hooks
/// (whether the uploads are online, the launch's options) are left as they
/// are.
///
/// @param context unused
/// @param[in,out] hooks the score report's hooks
void boneyards_report_hooks(void* context, oa::boneyards::reporter::Hooks& hooks) noexcept;

/// Tells whether a report of the running game may be sent.
///
/// A launch this call applied stays reportable until it ends, including
/// after Developer Mode changes the simulation the next launch would play.
/// With no such launch, the data playing now is judged
/// (boneyards_current_data_verdict).
///
/// @return true when the report may be sent
[[nodiscard]] bool boneyards_report_simulation_allowed() noexcept;

/// Tells whether the Boneyards launched the running game, as its overlay's status widget records.
///
/// @return true from a launch reply until the Boneyards takes the player back
[[nodiscard]] bool boneyards_launched_game() noexcept;

/// Says what the Boneyards knows of the battle whose replay is being made
/// (oa::app::netgame::extension_api::Hooks::recorded_game).
///
/// A game the Boneyards launched has the kind `boneyards`, and its object
/// under `game` holds the battle's game id (`game_id`), the planet's name
/// (`planet`) and the sector's name (`sector`); a member whose text is empty
/// is left out. A launch that is not active, and a battle with no game id,
/// are not one the Boneyards launched.
///
/// @param context unused
/// @param[out] game the kind and members, set only when this returns true
/// @return true when the Boneyards launched the game
[[nodiscard]] bool boneyards_recorded_game(
    void* context, oa::app::netgame::extension_api::RecordedGame* game
) noexcept;

/// The kind a Boneyards battle's replay names in its description (game.kind),
/// and its object under `game` (game.boneyards).
inline constexpr std::string_view kBoneyardsReplayKind = "boneyards";

/// The title the Library's Replays section shows that kind by, in its kind
/// menu and on its rows: the English, which languages/boneyards.tdf translates.
inline constexpr std::string_view kBoneyardsReplayTitle = "Boneyards";

/// Registers the Boneyards' kind of replay with the Library's Replays section
/// (oa::data::replays::register_kind), so its kind menu offers the Boneyards'
/// battles and their rows show the title with the battle's planet. Called as
/// the extension's table is filled.
void register_boneyards_replay_kind();

/// Returns the label the match's return names after a Boneyards launch (Extension::return_label).
///
/// @return the launch block's provider label ("Boneyards") while the
///         Boneyards launched the game and the launch is active;
///         null otherwise
[[nodiscard]] const char* boneyards_return_label() noexcept;

} // namespace oa::app
