// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Network play's command line in oa-game: the long options of the loopback,
// host-not-found and demo runs, the 3.1c network switches, and the parts of
// the extension table that need no running app; and a recording the engine
// hands over to replay.
#pragma once

#include "oa/app/extension.hpp"
#include "oa/app/command_line.hpp"
#include "oa/netgame/frontend/multiplayer_states.hpp"
#include "oa/app/netgame/launch_file.hpp"
#include "oa/app/netgame/launch_switches.hpp"
#include "oa/app/netgame/opened_launch.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/screen_registry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace fs = std::filesystem;

constexpr std::size_t kDefaultDemoTicks = 600; // headless --play-demo without --match-ticks

// When --check-opened-launch opens its file in the game, as the system opens one.
enum class OpenedLaunchCheck : uint8_t {
    none,  // no check
    start, // as the game starts, before its window opens
    menu,  // once the main menu has settled
};

struct NetOptions {
    // Headless in-process host/join over loopback: networked ticks to run.
    std::optional<std::size_t> net_loopback_ticks;
    bool net_loopback_watcher = false; // the loopback joiner only watches
    // The loopback host seats a computer player, which plays the joiner, or
    // the host while the joiner watches.
    bool net_loopback_computer = false;
    // The loopback host and the joiner each seat one computer player. Implies
    // the computer-player match, and plays its recording back when --net-record
    // names a file.
    bool net_loopback_each_ai = false;
    // The loopback host fixes the start positions, as the battle room's
    // option does.
    bool net_loopback_fixed_starts = false;
    // The loopback host makes one finished unit of this type beside its
    // commander, which fires at the ground beside the commander's aim point.
    // Empty: the check makes none.
    std::string net_loopback_shooter;
    // A join whose host is not found, played through the main loop (with
    // --frames and "-n1:<address>" naming an address where no game is
    // hosted): a launch counts as active until the game list begins to wait
    // for the host, and no longer after, the one way to "Host not found.
    // Exiting...". Standard output reports that text and the leave it leads
    // to, with the reason and the exit status it ends the run with.
    bool check_host_not_found = false;
    // The DirectPlay enumeration port this game hosts on and asks for games
    // at, in place of the standard one; its stream and datagram ports are
    // then any the system has free. Checks that host or search give each run
    // its own, so they can run side by side.
    std::optional<uint16_t> dplay_port;
    // The DirectPlay application GUID this game hosts under and looks for,
    // in wire byte order (--dplay-guid). Unset: Total Annihilation's own GUID,
    // so a game stays visible to 3.1c and to a game started without the option.
    std::optional<std::array<uint8_t, 16>> dplay_guid;
    // .tad recording to replay; match_ticks bounds a headless replay.
    fs::path play_demo;
    // Replay even when the recording's unit table differs from the installed
    // one (diagnostic: definition indices then name the wrong units).
    bool demo_ignore_unit_table = false;
    // Hand the engine's recording hook (Extension::open_recording) bytes that
    // are no recording and a recording's truncated header, then the
    // --play-demo recording, which it replays through the replay hooks.
    bool check_recording_hook = false;
    // Record the next network game to this file, whatever the rules; empty
    // records only under the recorder's rules, to the mod's folder in the
    // player's own folder's Recordings (recording_file, user_folder.hpp).
    fs::path net_record;
    // Present the replay version bytes (identity.replay-network-version), as a
    // game that joins a replayer's session to watch a recording does.
    bool replay_viewer = false;
    // --host or --join ADDRESS, with --player-name, --game-name and
    // --game-password: the TCP/IP game the multiplayer screens host or join
    // as soon as the game starts, as the player would through them. The
    // preferences keep what they hold.
    oa::ui::frontend_multiplayer::DirectGame direct_game{};
    // --launch FILE, or "-" for standard input: a launch file, an .oalaunch
    // or TA Forever's INI, as -c names one. Read once the other options are
    // known, so an option beside the file replaces one field.
    std::string launch_path{};
    // The launch settings' options, such as --metal 2000, in command-line
    // order: the option as written and its value (empty for one that takes
    // none). Each replaces its field of the file's plan, or makes a plan of
    // its own with --host or --join.
    std::vector<std::pair<std::string, std::string>> launch_options{};
    // The program, not a test, may read a launch piped in on standard input
    // when no launch file is named (allow_piped_launch).
    bool piped_launch_allowed = false;
    // --list-content with --json: print the installed maps, mods and
    // expansions and leave before a window opens.
    bool list_content = false;
    bool json_requested = false;
    // A launch was planned, from a file, from standard input or from the
    // launch settings' options. The battle room applies it, and the log names
    // the map, the rules and the seats.
    bool launch_planned = false;
    std::string launch_source{};
    // Where a later refusal of a field points (launch_place): the option
    // that replaced it, or the key of an INI that filled it, written
    // "taforever.ini: mission".
    std::vector<std::pair<std::string, std::string>> launch_replaced{};
    // Which of the direct-game options the command line itself named, so a
    // launch file does not replace them.
    bool cli_player_name = false;
    bool cli_game_name = false;
    bool cli_password = false;
    bool cli_dplay_port = false;
    bool cli_dplay_guid = false;
    netgame::LaunchContentChoice launch_content{};
    netgame::LaunchStatusChoice launch_status{};
    // The launch's content is what this run plays, rather than --content.
    bool launch_content_applied = false;
    // The launch names a mod the run has not resolved to a folder yet.
    bool launch_mod_pending = false;
    // A launch file check_options refused. The message is raised after the
    // hook returns, so the process exits 2. Empty when the file was accepted.
    std::string launch_refusal{};
    // --check-opened-launch start|menu FILE: the file is opened in the game as
    // the system opens one (a drop of the file on the game), at that moment.
    OpenedLaunchCheck opened_launch_check = OpenedLaunchCheck::none;
    std::string opened_launch_check_file{};
};

// A recording the engine handed network play to replay
// (Extension::open_recording), held until start_demo_playback takes it in
// place of the --play-demo file.
struct StagedRecording {
    std::string name;           // the recording's name as the engine gave it
    std::vector<uint8_t> bytes; // the whole recording
    bool strict{};              // refuse a unit table that differs from the installed one
};

// The extension's context: what the command line gave network play, and the
// multiplayer frontend states the dispatcher runs.
struct NetgameContext {
    NetOptions options{};
    oa::app::netgame::launch::LaunchSwitches launch{};
    oa::app::command_line::SwitchHandler switches{};
    // Its session routines are all null: no provider resolves and no session
    // starts through the dispatcher; the multiplayer screens drive the network.
    oa::netgame::frontend::MultiplayerFrontend frontend{};
    // Tells whether a launch is active, which the frontend's entry follows;
    // null answers false.
    bool (*launch_active)(void* context){};
    // The recording the next start_demo_playback replays in place of
    // --play-demo's; empty otherwise.
    std::optional<StagedRecording> staged_recording{};
};

/// Returns the process's network play context, which oa_extension_init_netgame hands the table.
///
/// @return The one context, created empty on first use.
[[nodiscard]] NetgameContext& netgame_context();

/// Returns the long options network play took from the command line.
///
/// @return netgame_context().options.
[[nodiscard]] inline const NetOptions& net_options() {
    return netgame_context().options;
}

/// Returns the 3.1c network switches network play took from the command line.
///
/// @return netgame_context().launch.
[[nodiscard]] inline const oa::app::netgame::launch::LaunchSwitches& net_launch_switches() {
    return netgame_context().launch;
}

/// Fills the extension hooks that need no running app.
///
/// They cover the long options and their checks as a whole, the 3.1c
/// network switches (the handler is bound to context.launch here), the usage
/// text, the frontend's entry, the multiplayer frontend states and the launch
/// values of a new match's Game block.
///
/// @param[in,out] table Extension table; context, take_option, check_options,
///                      switch_handler, text, frontend_entry, frontend_states
///                      and match_game are set.
/// @param[in,out] context Context the hooks read and write; becomes `table.context`.
void fill_option_hooks(Extension& table, NetgameContext& context);

struct Options;

/// Lets this run read a launch piped in on standard input.
///
/// The program calls it before it parses the command line; a test that
/// parses one does not, so it never waits on its own standard input. When
/// no launch file is named, check_options then reads a launch from standard
/// input if it is a pipe or a file whose first bytes are `oalaunch:`,
/// waiting at most oa::app::netgame::kPipedLaunchWait for the first byte.
void allow_piped_launch() noexcept;

/// Returns where a refusal of one of the launch's fields points.
///
/// @param field the dotted field, such as map
/// @return the option that replaced it, such as --map, the INI's key that
///         filled it, such as "taforever.ini: mission", or the launch's
///         source and the field, such as "host.oalaunch: map"
[[nodiscard]] std::string launch_place(std::string_view field);

/// Names where check_options looks for a launch file named without a folder, after the current folder.
///
/// The engine names the game folder while it parses the command line, and
/// names none again after.
///
/// @param folders the game folder's finder; an empty one looks in the current folder alone
void find_launch_files_in(const netgame::LaunchFolders& folders) noexcept;

/// Keeps a line about the command line for the game's log.
///
/// The lines are written, each after "open-annihilation: ", when the game's
/// first frontend starts, once the log is open. check_options drops those of
/// an earlier parse.
///
/// @param line the line, such as "-l has no effect"
void keep_start_note(std::string line);

/// Returns the lines kept for the game's log and not written yet.
///
/// @return the lines, in the order they were kept
[[nodiscard]] const std::vector<std::string>& start_notes() noexcept;

/// Keeps a line for the log about each engine switch of the 3.1c command line that has no effect here.
///
/// The developer switches, -f and -l have none; -w turns the sound off,
/// where 3.1c plays it through the system's sound player.
///
/// @param options the parsed command line
void note_game_switches(const Options& options);

/// Raises a launch-file refusal that check_options kept.
///
/// check_options runs as a hook. A hook's exception is reported as
/// std::runtime_error, and the process would exit 1. The refusal is kept
/// and raised here, after the hook returns, so the process exits 2.
void raise_launch_refusal();

/// Copies a launch file's content onto the run when the command line did not name --content.
///
/// The mod's folder is resolved later, once the game folder is known. A unit
/// test that parses a local context leaves the process's context alone, so
/// this reads the process's context: the game's own extension uses that one.
/// A base game or expansion the launch names that the game does not know is
/// refused with LaunchRefusal, naming the file and the field.
///
/// @param[in,out] options the parsed command line
void adopt_launch_run(Options& options);

/// Puts a launch in place once the command line was checked, as check_options puts a -c file in place.
///
/// The plan goes into the launch block and the game to host or join
/// (apply_launch_plan, which keeps the fields the command line named), and
/// a launch that hosts or joins has the multiplayer screens take TCP/IP at
/// once, as check_options does. A launch file the system
/// opened the game with is put in place this way before the runtime starts,
/// and one opened while the game runs before the game goes to its battle
/// room. A refusal puts nothing in place: the context is as it was.
///
/// @param[in,out] netgame the context the launch goes into
/// @param plan the launch, as read_opened_launch_plan reads it
/// @throws LaunchRefusal a field the battle room cannot use
void put_launch_in_place(NetgameContext& netgame, netgame::LaunchPlan plan);

/// Binds the multiplayer frontend states' answers about joining and leaving a session to the dispatcher (query_register).
///
/// The states' session routines are null (NetgameContext::frontend): the
/// multiplayer screens open the provider's session, join and leave, and
/// start the match report as the match launches. So joining never waits for
/// a match report to start (init_score_reporting answers 0), and every game plays
/// over TCP/IP (transport_kind answers transport_kind::tcpip), so leaving the
/// battle room returns to the game list. open_service_session keeps the
/// dispatcher's 0 for a query no handler takes: the states ask it only once
/// the connection selection proceeds with a provider other than the modem,
/// serial and TCP/IP ones, or once a connection dialog has an address, and
/// nothing leads them there while the screens choose the provider.
///
/// @param[in,out] registry The runtime's registry.
void register_frontend_state_queries(ScreenRegistry* registry) noexcept;

} // namespace oa::app
