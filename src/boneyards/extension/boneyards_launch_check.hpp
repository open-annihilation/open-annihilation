// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-boneyards-launch: a battle the Boneyards launches, played in the
// running game as a player plays it, by the host or by the joiner.
// --check-boneyards-features: the Phase 3 screens, clicked through as a
// Beta Tester and as an ordinary account.
#pragma once

#include "oa/ui/screen_registry.hpp"

#include <cstdint>

namespace oa::app {

// Which side of the launched battle the check plays.
enum class LaunchCheckRole : uint8_t {
    host, // hosts the battle, starts it and wins it
    join, // joins the battle and loses it
};

// The battle both sides' replays launch: its area, game id and password.
inline constexpr uint32_t kLaunchCheckArea = 0x709;
inline constexpr const char* kLaunchCheckGame = "0c0ffee0d15ea5e0";
inline constexpr const char* kLaunchCheckPassword = "LAUNCHCHECKPASS1";
// The debriefing room the replays name, which the return enters.
inline constexpr uint32_t kLaunchCheckDebriefingRoom = 5;

// Exit codes of --check-boneyards-launch: each stage that runs past its time
// fails with its own, and a run that found anything wrong on the way with
// `wrong`.
namespace launch_check_exit {
inline constexpr int passed = 0;
inline constexpr int no_main_menu = 40;
inline constexpr int no_login = 41;
inline constexpr int no_galactic_map = 42;
inline constexpr int no_launch = 43;
inline constexpr int no_battle_room = 44;
inline constexpr int not_started = 45;
inline constexpr int no_match = 46;
inline constexpr int no_end = 47;
inline constexpr int no_return = 48;
inline constexpr int wrong = 49;
} // namespace launch_check_exit

// Which account --check-boneyards-features logs in as.
enum class FeaturesCheckRole : uint8_t {
    beta,   // a Beta Tester: Matchmaking is offered
    player, // an ordinary account: Matchmaking is not offered
};

// Which tier --check-boneyards-overlay asks the run to show the strip on.
// The flag is the engine's --hardware-acceleration. On the dummy video
// driver every card renderer refuses, so the processor presents the frame;
// the check still requires the strip on that frame.
enum class OverlayCheckTier : uint8_t {
    basic, // the card, where one is used, scales the interface
    full,  // the card, where one is used, also draws the battlefield
};

// Exit codes of --check-boneyards-overlay. The strip's colour has to be on
// the composed main menu and on the frame the window shows.
namespace overlay_check_exit {
inline constexpr int passed = 0;
inline constexpr int no_strip = 60;
inline constexpr int presented_differs = 61;
inline constexpr int no_window = 62;
inline constexpr int wrong_tier = 64;
inline constexpr int bad_scale = 65;
} // namespace overlay_check_exit

// Exit codes of --check-boneyards-features. A screen that runs past its time
// fails with its own, and a run that found anything wrong on the way with
// `wrong`.
namespace features_check_exit {
inline constexpr int passed = 0;
inline constexpr int no_main_menu = 50;
inline constexpr int no_registration = 51;
inline constexpr int no_login = 52;
inline constexpr int no_map = 53;
inline constexpr int no_locator = 54;
inline constexpr int no_quickplay = 55;
inline constexpr int no_matchmaking = 56;
inline constexpr int wrong = 57;
} // namespace features_check_exit

/// Registers the overlay that plays --check-boneyards-launch, ticked on every screen.
///
/// From the main menu the check enters the Boneyards and logs in as the guest
/// its replay confirms (oa_host or oa_joiner), waits for the galactic map and
/// presses the planet of area kLaunchCheckArea. It opens that planet's battle
/// list. The host presses HOST, names the battle, picks Galactic War (or For
/// Fun, with --boneyards-launch-for-fun) and
/// presses START; the joiner presses the forming battle and JOIN. The replay
/// answers with the launch_game record the Boneyards sends; the console takes
/// it and the frontend's pass leads through the connection selection to the
/// battle room,
/// the host printing "the battle room is hosted" once there; the room must show
/// the launch: the host's role and map, the side from the allegiance, the
/// options locked and "Boneyards" on PREVMENU. Both sides mark themselves ready
/// once the other is in; the host starts the battle when START comes on. In the
/// battle each side sends a chat line; the joiner then selects its commander
/// (Ctrl+C) and self-destructs it (Ctrl+D), which ends the battle, as a loss
/// for it and a win for the host. Each side presses through the end-of-game
/// screen to its MainMenu button, which reads "Boneyards", and the main menu
/// returns to the Boneyards, whose chat console enters the debriefing room.
///
/// It then checks the records Rover queued for the Boneyards, each naming the
/// battle's area and, but for the host request, its game: the host or join
/// request; for the host the battle's settings (the options locked and
/// watching disallowed, as the launch leaves them), both players with their
/// sides, a score report while the battle ran and the final one naming it
/// the winner, and from the joiner none of these; the side's own chat line
/// as a score report message; and the left_game of the return. The run ends through
/// ScreenServices::quit with a launch_check_exit code, after printing
/// "boneyards launch check: passed" or what went wrong. Keys and pointer
/// presses reach the game through SDL's event queue, as a player's do.
///
/// @param[in,out] registry the runtime's registry
/// @param role the side the check plays
/// @param for_fun the host picks For Fun; otherwise it picks Galactic War
void register_boneyards_launch_check(
    ScreenRegistry* registry, LaunchCheckRole role, bool for_fun = false
);

/// The after-battle notice is up and has not been pictured yet.
///
/// @return true for a few frames of the launch check's return
bool boneyards_launch_notice_pending();

/// The presented frame has pictured the after-battle notice.
void boneyards_launch_notice_taken();

/// Registers the overlay that plays --check-boneyards-features, ticked on every screen.
///
/// From the main menu the check enters the Boneyards. Before it logs in it
/// opens registration: the web page for both accounts, and for a Beta Tester
/// the in-game form, which this server answers it does not take yet. It then
/// logs in as the account its replay names. Either account runs each working
/// search on the Locator and starts and cancels QuickPlay. A Beta Tester then
/// opens Matchmaking from Chat, joining its mocked queue and leaving it; an
/// ordinary account finds no Matchmaking on the battle list or in Chat. The run ends
/// through ScreenServices::quit with a features_check_exit code, after
/// printing "boneyards features check: passed" or what went wrong.
///
/// @param[in,out] registry the runtime's registry
/// @param role the account the check logs in as
void register_boneyards_features_check(ScreenRegistry* registry, FeaturesCheckRole role);

struct CheckHost;
struct Options;

/// Registers the overlay that samples --check-boneyards-overlay on the main menu.
///
/// The sample runs after the status strip has drawn. It finds a pixel of that
/// strip on the composed frame. The presented frame is read from the window
/// on the frame hook that follows the present.
///
/// @param[in,out] registry the runtime's registry
/// @param tier the tier the run was asked to show
void register_boneyards_overlay_check(ScreenRegistry* registry, OverlayCheckTier tier);

/// Reads the presented frame for --check-boneyards-overlay and ends the run.
///
/// Called from the extension's presented frame hook, after the frame is
/// shown. A tier other than the one asked, a missing strip or a presented
/// pixel that is not the strip's colour ends the run with an
/// overlay_check_exit code. A match prints
/// "boneyards overlay check: basic: the strip is on the presented frame"
/// (or "full") and the renderer's name, and ends the run with 0. Further
/// calls do nothing.
///
/// @param host the running game's check host
/// @param options the command line, whose hardware acceleration is the tier asked
void present_boneyards_overlay(const CheckHost& host, const Options& options);

} // namespace oa::app
