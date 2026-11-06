// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Network play's API for the engine extensions that build on it: 3.1c's
// launch block, which the game switches and a launching extension write;
// applying and ending a launch; the application mode the next frontend pass
// sets; leaving the game; the lines posted to a launched battle; and the
// hooks through which such an extension adds to network play (the match
// report a network game keeps, the console's Page, a join the launch asked
// for that failed). An extension that links oa-app-netgame-sdk builds on network play:
// the engine lists it after network play's extension, so where the engine
// calls every extension in turn network play's hook runs first, and where
// it asks the extension built on the others first, that extension answers
// before network play. Every function is called on the thread that runs
// main(); none throws.
#pragma once

#include "oa/core/world.h"
#include "oa/netgame/overlay_route.hpp"
#include "oa/ui/frontend_multiplayer/launch_block.hpp"

#include <cstdint>

/// The version of network play's API an extension is built against. An
/// extension checks oa::app::netgame::extension_api::api_version, its typed copy,
/// with static_assert.
///
/// Raise it by one in the change that alters the contract: a function or
/// hook added, removed or renamed, or its parameters, its effect or when
/// network play calls it. A change to wording alone keeps it.
#define OA_NET_EXTENSION_API_VERSION 3

namespace oa::data::campaign {
struct CampaignFile;
}

namespace oa::app::netgame::extension_api {

/// OA_NET_EXTENSION_API_VERSION, typed.
inline constexpr uint32_t api_version = OA_NET_EXTENSION_API_VERSION;

/// Returns 3.1c's launch block, which the "-n" and "-h" switches and a launching extension write.
///
/// Network play binds the block as its extension's init runs; the block
/// lives as long as the process.
///
/// @return the block
[[nodiscard]] oa::ui::frontend_multiplayer::launch::LaunchBlock& launch_block() noexcept;

/// Asks the main menu's first update for step setup_requested, as 3.1c's "-y" switch does.
///
/// Called while the game switches are parsed; the frontend's states read the
/// request as they start.
void request_setup() noexcept;

/// Applies a launch written into the block.
///
/// The block's user name becomes the frontend Game's nickname, 16
/// characters at most; the connection selection is asked to select the
/// multiplayer map list (the launch-pending flag); a request to end the
/// program opens the exit confirmation until the game next sets an
/// application mode; and the launch is active (launch_active) until
/// end_launch.
void apply_launch() noexcept;

/// Ends the active launch: its battle is over and the extension has taken the player back.
///
/// Nothing changes when no launch is active.
void end_launch() noexcept;

/// Tells whether a launch is active: applied (apply_launch) and not ended since.
///
/// While one is, the block's launch is the launch the multiplayer
/// screens, the session and the match report follow.
///
/// @return true while a launch is active
[[nodiscard]] bool launch_active() noexcept;

/// Asks the next frontend pass to set an application mode, as the returns from a launched game do.
///
/// @param mode the application mode (an oa::ui::frontend_state::mode_id value)
void request_app_mode(int32_t mode) noexcept;

/// Leaves the game and ends the program with exit status 0.
///
/// The session closes; with `with_reason` the local player's disconnect
/// reason, when there is one, shows first, and without it an open score
/// report hears the closed session as it closes. Nothing happens before the
/// runtime has finished its start.
///
/// @param with_reason whether the disconnect reason shows first
void leave_game(bool with_reason) noexcept;

/// Posts a line to the running battle's message log, or with no battle into the battle room's chat ring.
///
/// The line is cut into parts as the message log's notices are (63
/// characters at most, broken at a blank within the last 12), and bit 0 of
/// the Game's gui flags (the message flag) rises.
///
/// @param text the line; null posts nothing
void post_battle_line(const char* text) noexcept;

/// Empties the battle room's chat ring (the frontend Game's chat head and tail).
void empty_chat_ring() noexcept;

/// Returns the connection type the launch block holds, which the connection selection takes.
///
/// @return an oa::ui::frontend_multiplayer::launch::connection_type value; 0 for none
[[nodiscard]] int32_t connection_type() noexcept;

// Events of a network game's match report (Hooks::report_event), named for
// when network play reports them.
namespace report_event {
inline constexpr int32_t battleroom_opened = 1;
inline constexpr int32_t roster_changed = 2;
inline constexpr int32_t player_removed = 3;
inline constexpr int32_t player_changed = 4; // a defeated player keeps watching
inline constexpr int32_t map_changed = 5;
inline constexpr int32_t game_started = 6;   // the match's first frame
inline constexpr int32_t game_ended = 7;     // the end-of-game screen opened
inline constexpr int32_t session_closed = 8; // the session closed; the report ends after it
inline constexpr int32_t game_launched = 10; // the host pressed Start
inline constexpr int32_t interval = 99;
} // namespace report_event

/// What an extension adds to network play; with every hook null it adds nothing.
struct Hooks {
    /// Passed back to every hook; owned by the extension, which keeps it
    /// valid until the process exits.
    void* context{};
    /// A network game's match report starts: the battle room's match has
    /// launched and its world is built, with the battle room's provider and
    /// game name (Game.session_description) copied into its Game. Network
    /// play reports nothing itself; the report the extension keeps lives
    /// until report_close. Null keeps no report.
    ///
    /// @param context Hooks::context
    /// @param[in,out] world the match's world
    /// @param options the game's options file, as the game read it
    void (*report_start)(
        void* context, oa::World& world, const oa::data::campaign::CampaignFile* options
    ){};
    /// An event of the started report (report_event values), with the world
    /// the report reads: the running match's, else the end-of-game screen's.
    /// Null reports nothing.
    ///
    /// @param context Hooks::context
    /// @param[in,out] world the world the report reads
    /// @param event a report_event value
    void (*report_event)(void* context, oa::World& world, int32_t event){};
    /// A chat line of the match: one sent to every player or to allies, or
    /// one received. Null reports nothing.
    ///
    /// @param context Hooks::context
    /// @param[in,out] world the world the report reads
    /// @param line the line; may be null
    void (*report_chat)(void* context, oa::World& world, const char* line){};
    /// The report's step, once each frame of the running match. Null steps
    /// nothing.
    ///
    /// @param context Hooks::context
    /// @param[in,out] world the running match's world
    void (*report_step)(void* context, oa::World& world){};
    /// The started report ends: called after report_event with
    /// report_event::session_closed, as the session closes or the match's
    /// results are released. Null ends nothing.
    ///
    /// @param context Hooks::context
    void (*report_close)(void* context){};
    /// The connection selection opens, which releases whatever the
    /// extension's report holds open beside the game. Null releases nothing.
    ///
    /// @param context Hooks::context
    void (*report_release)(void* context){};
    /// Sends the console's Page to a user during an active launch. Null
    /// leaves the console's own refusal, which asks for a launch, in place
    /// of every page.
    ///
    /// @param context Hooks::context
    /// @param user the user paged, 15 characters at most
    /// @param text the message, 255 bytes at most
    void (*page)(void* context, const char* user, const char* text){};
    /// A join the active launch asked for failed: the game list found no
    /// host in time. Called before the frontend returns to the main menu;
    /// the hook shows `text` and leaves the extension's battle. Null leaves
    /// network play's own handling, the return alone.
    ///
    /// @param context Hooks::context
    /// @param text the host-not-found text to show
    void (*join_failed)(void* context, const char* text){};
};

/// Sets the hooks; called from the extension's init. A second call replaces the first.
///
/// @param hooks the hooks, copied
void set_hooks(const Hooks& hooks) noexcept;

/// Installs a route for one battle, or removes it when route is null.
///
/// The route is copied. Datagrams and streams whose destination it claims
/// are carried by it instead of by the system. Installing one clears the
/// direct-packet count. Removing one leaves that count as it is.
///
/// @param route the route, or null
void set_overlay_route(const oa::netgame::sock::OverlayRoute* route) noexcept;

/// Returns how many datagrams and streams the sockets sent or received
/// directly, to or from a claimed address or this machine's loopback, while
/// a route was installed.
///
/// @return the count since the route was installed
[[nodiscard]] uint64_t overlay_direct_packets() noexcept;

} // namespace oa::app::netgame::extension_api
