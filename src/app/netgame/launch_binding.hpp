// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The launch block as the running game binds it: where a launch written
// into the block lands, whether one is active, the answers read from the
// block (its connection type, a tournament game), the multiplayer screens'
// link to it, and the hooks an extension built on network play set
// (oa/app/netgame/extension_api.hpp, which forwards to these).
#pragma once

#include "close_handlers.hpp"

#include "oa/app/netgame/extension_api.hpp"
#include "oa/app/netgame/launch_switches.hpp"
#include "oa/ui/frontend_multiplayer/launch_block.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace oa::app {

// Where a launch written into the launch block lands in the running game.
// Every entry may be null.
struct LaunchBinding {
    // The game's launch block, which the "-n" and "-h" switches and a
    // launch write.
    oa::ui::frontend_multiplayer::launch::LaunchBlock* block{};
    // The game switches' values, which a setup request ("-y") marks
    // (request_setup); null marks nothing.
    oa::app::netgame::launch::LaunchSwitches* switches{};
    // A launch sets it; the connection selection's initialize applies it.
    uint8_t* launch_pending{};
    // The application mode a return from a launched game asks the next
    // frontend pass to set (MultiplayerState::pending_app_mode); null asks
    // nothing.
    int32_t* app_mode_pending{};
    // Leaves the game and ends the program (LaunchLink::leave_game);
    // null leaves nothing.
    void* leave_context{};
    void (*leave_game)(void* context, bool with_reason){};
    // The running game's close handler, which a launch sets to the exit
    // confirmation; null sets nothing.
    CloseHandlers* close_handlers{};
    // Asked each time whether a launch is active, while one is: true ends
    // it, as end_launch does. Null ends none this way.
    bool (*launch_ended)(){};
};

/// Installs where a launch lands; the answers below read it. Whether a launch is active is kept.
///
/// @param binding the block, the switches, the pending flag, the leave, the close handler and the end
void bind_launch(const LaunchBinding& binding) noexcept;

/// Returns the bound launch block.
///
/// @return the block; null before one is bound
[[nodiscard]] oa::ui::frontend_multiplayer::launch::LaunchBlock* launch_block() noexcept;

/// Tells whether a launch is active: applied and not ended since, or marked active (set_launch_active).
///
/// While one is, the bound LaunchBinding::launch_ended is asked first, and
/// ends it when it answers true.
///
/// @param context unused
/// @return whether a launch is active
[[nodiscard]] bool launch_active(void* context) noexcept;

/// Marks a launch active, or ended, without applying one.
///
/// @param active whether a launch is active from now on
void set_launch_active(bool active) noexcept;

/// Applies a launch written into the block.
///
/// The block's user name becomes the frontend Game's nickname, 16
/// characters at most; the connection selection is asked to select the
/// multiplayer map list (LaunchBinding::launch_pending); a request to end
/// the program opens the exit confirmation (CloseHandler::exit_confirm)
/// until the game next sets an application mode; and the launch is active
/// until end_launch.
void apply_launch() noexcept;

/// Ends the active launch; nothing changes without one.
void end_launch() noexcept;

/// Asks the main menu's first update for step setup_requested, as "-y" does (LaunchBinding::switches).
void request_setup() noexcept;

/// Asks the next frontend pass to set an application mode (LaunchBinding::app_mode_pending).
///
/// @param context unused
/// @param mode the mode
void request_app_mode(void* context, int32_t mode) noexcept;

/// Leaves the game and ends the program through the bound leave (LaunchBinding::leave_game).
///
/// @param context unused
/// @param with_reason whether the disconnect reason shows first
void leave_launched_game(void* context, bool with_reason) noexcept;

/// Returns the launch link the multiplayer screens read.
///
/// The link reads the bound block and launch_active; a return's application
/// mode goes to the bound pending mode, and the leave to
/// LaunchBinding::leave_game. A join the launch asked for that failed goes to
/// the extension's hook (extension_hooks().join_failed); without one nothing
/// is shown or left.
///
/// @return the link over the bound block
[[nodiscard]] oa::ui::frontend_multiplayer::LaunchLink launch_link() noexcept;

/// Returns the hooks an extension built on network play set (oa::app::netgame::extension_api::set_hooks).
///
/// @return the hooks; every entry null until an extension sets them
[[nodiscard]] const oa::app::netgame::extension_api::Hooks& extension_hooks() noexcept;

/// What an extension says of the game being recorded, as a replay's description carries it.
struct RecordedGameFacts {
    /// The kind of game: `game.kind`.
    std::string kind{};
    /// The object under `game` named for that kind, as compact JSON.
    std::string object_json{};
};

/// Asks the extension built on network play what it says of the game being recorded
/// (oa::app::netgame::extension_api::Hooks::recorded_game).
///
/// The members it names become the members of one JSON object, in its order,
/// each a string. A kind that is empty or is not lower-case letters, digits
/// and hyphens, and a member whose name is not letters, digits and
/// underscores, are refused: the answer is then none.
///
/// @return the kind and the object; none when no extension answers, or it
///         says the game is not its own, or what it says is refused
[[nodiscard]] std::optional<RecordedGameFacts> recorded_game_facts();

/// Tells whether the launch marks the running game as a tournament game (TeamPanelHost::tournament_game).
///
/// The launch block's tournament field is written by each launch and never
/// cleared, so a game hosted after a tournament battle counts as one too,
/// as in 3.1c and as the battle room takes it.
///
/// @return true when a launch block is bound and its tournament field is not 0
[[nodiscard]] bool launch_tournament() noexcept;

/// Returns the launch block's connection type (MultiplayerHost::connection_type).
///
/// @param context unused
/// @return the type; 0 without a bound block
int32_t launch_connection_type(void* context) noexcept;

} // namespace oa::app
