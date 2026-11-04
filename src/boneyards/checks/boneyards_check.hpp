// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The headless Boneyards checks, which drive the running game through the
// engine's check host (oa/app/check_host.hpp), their exit codes and the
// helpers they share.
#pragma once

#include "oa/boneyards/console/status_widget.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oa::app {

struct CheckHost;
struct Options;
class Runtime;

/// Runs --check-boneyards-login: logs into Central Command as the login screen does, then logs out.
///
/// The account comes from OA_BY_USER / OA_BY_PASSWORD (a guest without a
/// password; with --boneyards-replay and no account, the guest oa_check)
/// and the server list from <game-dir>/boneyards or --boneyards-server.
/// The session is pumped once a frame until the dossier arrives, the
/// attempt ends or the time limit passes; a loaded dossier is followed by
/// the galactic map, whose graphics come from the game's files.
///
/// @param host the running game's check host
/// @return 0 when logged in with the dossier and map loaded; 1 when the
///         login failed, 2 without an account, 3 when the dossier did not
///         arrive, 4 when the galactic map was unavailable.
int check_boneyards_login(const CheckHost& host);

/// Applies one unit limit from the Profile and reads the engine's preference back.
///
/// The Boneyards' own UnitLimit is stored through a stand-in settings store.
/// The preference file is the one the run was given.
///
/// @param runtime the running app
/// @param limit units per player, already within 21..500
/// @return 0 when the preference holds `limit`; 1 otherwise
int check_profile_unit_limit(Runtime& runtime, int32_t limit);

/// Runs --enter-boneyards: presses Enter Boneyards and runs idle passes on a clock stepped one frontend tick per pass.
///
/// Enter Boneyards goes in as the B key through the overlay; after "-y" the
/// main menu's first update must have entered instead, within a few passes,
/// and no key is pressed. Without --boneyards-login the passes run, nothing
/// logs in, and a request to close the window must open the Boneyards box
/// "Exit to the system?", which NO closes, while the run goes on; with it, check_boneyards_post_login
/// runs with `ticks` as its pass limit. The frame is then composed and
/// written to --snapshot when given, and the Rover session is closed
/// before returning.
///
/// @param host the running game's check host
/// @param options the game's command-line options, for --snapshot
/// @param ticks Idle passes to run.
/// @return kEntryPassed (0), kEntryNotEntered when Enter Boneyards did not
///         reach frontend state 7, kEntryCloseNotAsked when the close
///         request was not answered with the box, or
///         check_boneyards_post_login's result.
int check_boneyards_entry(const CheckHost& host, const Options& options, std::size_t ticks);

/// Runs --boneyards-login: logs in through the login console within `passes` idle passes and follows the transition to the galactic map.
///
/// The map must load its planets and draw them, the heard sounds must
/// resolve to WAVs of the install and a press on a planet must show it in
/// the planet summary; the --boneyards-pick, --boneyards-open and
/// --boneyards-chat steps follow on that map.
///
/// @param host the running game's check host
/// @param options the game's command-line options, for --snapshot
/// @param passes Most idle passes to run.
/// @return kEntryPassed (0) or the failing kEntry* code below.
int check_boneyards_post_login(const CheckHost& host, const Options& options, std::size_t passes);

/// Runs --boneyards-pick and --boneyards-open on the galactic map the login check reached.
///
/// @param host the running game's check host
/// @param options the game's command-line options, for --snapshot
/// @param sounds What the Boneyards has played so far; grows as passes run.
/// @param pass Idle passes the login check has used.
/// @param passes Most idle passes in all.
/// @return kEntryPassed (0), also when neither option is given, or the
///         failing kEntry* code below.
int check_boneyards_screens(
    const CheckHost& host,
    const Options& options,
    const std::vector<std::string>& sounds,
    std::size_t pass,
    std::size_t passes
);

/// Runs --boneyards-chat on the planet check_boneyards_screens picked.
///
/// Checks the news ticker, entering a frequency with its rows and log, a
/// said line, switching frequencies, the player pane, creating a
/// frequency (--boneyards-create) and Release, against the replay.
///
/// @param host the running game's check host
/// @param options the game's command-line options, for --snapshot
/// @param sounds What the Boneyards has played so far; grows as passes run.
/// @param pass Idle passes used so far.
/// @param passes Most idle passes in all.
/// @return kEntryPassed (0) or the failing kEntry* code below.
int check_boneyards_chat(
    const CheckHost& host,
    const Options& options,
    const std::vector<std::string>& sounds,
    std::size_t pass,
    std::size_t passes
);

/// Sends SDL pointer input for the Boneyards checks in canvas coordinates (CheckHost::pointer).
///
/// @param host the running game's check host
/// @param type SDL_EVENT_MOUSE_MOTION, or a left button press or release.
/// @param x Canvas x.
/// @param y Canvas y.
/// @param clicks Click count SDL reports for a press or release in its run.
void boneyards_pointer(const CheckHost& host, uint32_t type, int32_t x, int32_t y, uint8_t clicks);

/// Sends a key press and release for the Boneyards checks.
///
/// @param host the running game's check host
/// @param code SDL key code.
void boneyards_key(const CheckHost& host, uint32_t code);

/// Runs one idle pass of the Boneyards checks.
///
/// Steps the held clock one frontend tick, runs a pass of the screen
/// packages and composes the frame (written to --boneyards-frames when
/// given).
///
/// @param host the running game's check host, its clock held
/// @return The composed frame's checksum.
std::uint64_t boneyards_pass(const CheckHost& host);

} // namespace oa::app

namespace oa::app::boneyards_check {

// Exit codes of --enter-boneyards; with --boneyards-login each stage of the
// login, of the transition to the galactic map and of the planet and
// console steps fails with its own.
inline constexpr int kEntryPassed = 0;
inline constexpr int kEntryNotEntered = 1;
inline constexpr int kEntryNoLoginConsole = 2;
// The dossier (connection level 4) never arrived.
inline constexpr int kEntryNoDossier = 3;
// The first pass at level 4 left the login console up.
inline constexpr int kEntryLoginNotClosed = 4;
inline constexpr int kEntryChatOpened = 5;
// The frame barely changed while the green slid out.
inline constexpr int kEntryStillSlide = 6;
// The slide did not hand the map back.
inline constexpr int kEntryMapNotReturned = 7;
inline constexpr int kEntryMapNotLoaded = 8;
inline constexpr int kEntryNoAccount = 9;
// Too few planet sprites in the map panel's viewer.
inline constexpr int kEntryMapNotDrawn = 10;
// A console sound names no WAV in the install.
inline constexpr int kEntrySoundMissing = 11;
// A planet press left the planet summary empty, or under --boneyards-pick
// it does not read the planet and its census.
inline constexpr int kEntrySummaryNotShown = 12;
// --boneyards-pick: no hotspot for the planet in the map view; hovering and
// pressing it did not select the planet; its census did not arrive, or not
// as the replay has it.
inline constexpr int kEntryPickMissing = 13;
inline constexpr int kEntryPickNotSelected = 14;
inline constexpr int kEntryPickNoCensus = 15;
// --boneyards-open: the console button did not bring its console up; Escape
// did not hand the map back, or under a replay did not leave the chat
// console's frequency.
inline constexpr int kEntryOpenTimeout = 16;
inline constexpr int kEntryReleaseFailed = 17;
// --boneyards-open recon: the console did not show the picked planet's name,
// environment and census population, or its panorama did not scroll.
inline constexpr int kEntryReconWrong = 18;
// --boneyards-say: no line reached the chat log after the typed one was
// sent, or under a replay a plain line did not reach Rover as a say to the
// entered frequency.
inline constexpr int kEntrySayNoReply = 19;
// --boneyards-open chat on a planet whose rooms the replay records: the
// console entered none, or does not list the room's members and every
// frequency the replay lists, or announced nothing.
inline constexpr int kEntryChatNotEntered = 20;
// --boneyards-open profile: the console shows no Profile.def description
// for its account tab.
inline constexpr int kEntryProfileNoDescription = 21;
// --boneyards-chat: the entered frequency's rows, entry line or the replay's
// lines are not as the replay has them; the typed line did not reach Rover
// as a say to the frequency or was not echoed; a double click on the other
// planetary default did not switch to it; Release did not leave the
// frequency.
inline constexpr int kEntryChatRowsWrong = 22;
inline constexpr int kEntryChatSayNotSent = 23;
inline constexpr int kEntryChatNotSwitched = 24;
inline constexpr int kEntryChatNotLeft = 25;
// --boneyards-create: CREATE did not open the create-frequency pane, GO did
// not come on for the typed name or send the request, or the frequency the
// replay made was not entered.
inline constexpr int kEntryChatNotCreated = 26;
// --boneyards-chat: a click on the second player row, a member whose profile
// the replay records, did not show the player pane with its name and clan.
inline constexpr int kEntryChatPaneEmpty = 27;
// --boneyards-open profile: the console's request for the player's own
// record was not answered with a login name.
inline constexpr int kEntryProfileNotReceived = 28;
// --boneyards-chat or --boneyards-open profile: the answer arrived, but the
// activity light did not run while the profile request was out or still
// runs after it.
inline constexpr int kEntryActivityLightWrong = 29;
// --boneyards-chat: the news ticker does not hold the replay's ticker line
// with its spacer, or its label does not show a window of it; a page the
// replay brings with the entered frequency is not in the chat log.
inline constexpr int kEntryChatTickerWrong = 30;
inline constexpr int kEntryChatPageMissing = 31;
// --boneyards-open help, ladders or news: the console's contents list stayed
// empty, its view kept the waiting page or the activity light kept running;
// a News or Ladders page is not a URL on the content server of the
// console's kind, or under a replay a download did not arrive or its
// temporary file was left behind.
inline constexpr int kEntryContentsNotLoaded = 32;
// --boneyards-open profile: the console does not show seven category rows
// with the account row selected, the start of the account description in
// its first description line, or keeps its status line up once the record
// has arrived.
inline constexpr int kEntryProfileViewWrong = 33;
// A request to close the window over the Boneyards ended the run, opened no
// "Exit to the system?" box, or NO left the box open.
inline constexpr int kEntryCloseNotAsked = 34;

inline constexpr int kSlideOut = 1;
inline constexpr const char* kGreenSlideSound = "GREEN_SLIDE";
inline constexpr const char* kBigButtonSound = "BIGBUTTON";
// The Main Frame's planet console buttons play it; the galactic ones play
// GALACTIC_BUTTON.
inline constexpr const char* kPlanetButtonSound = "PLANET_BUTTON";
// A planet plays it as the pointer enters its hotspot.
inline constexpr const char* kMoreSound = "MORE";

/// Tests whether a sound was heard from an index of the heard list on.
///
/// @param sounds sound names in the order they played
/// @param from first index searched
/// @param name sound name to find
/// @return true when `name` is at or after `from`
bool heard_since(const std::vector<std::string>& sounds, std::size_t from, const char* name);

/// Prints one line of the Main Frame console state for the login check.
///
/// @param when label of the moment the probe was taken
/// @param probe console state read from the session
void print_probe(const char* when, const oa::boneyards::console::MainFrameProbe& probe);

} // namespace oa::app::boneyards_check
