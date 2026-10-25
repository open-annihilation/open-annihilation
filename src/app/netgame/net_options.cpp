// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Network play's long options, their usage text and checks, the 3.1c network
// switches, and the values they hand the frontend and a new match.
#include "net_options.hpp"

#include "oa/app/app.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/base/text.hpp"
#include "oa/core/game_state.h"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/netgame/dplay.hpp"
#include "oa/platform/log_files.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

using netgame::LaunchPlan;
using netgame::LaunchRefusal;
using netgame::LaunchRole;

/// The refusal the last check_options kept.
///
/// The combined option table's context is not these options, and a unit test
/// parses a context of its own, so the message is kept here until parsing
/// raises it outside the hook.
///
/// @return the message, empty when the file was accepted
std::string& held_launch_refusal() {
    static std::string held;
    return held;
}

/// The lines about the command line kept for the game's log (keep_start_note).
///
/// @return the lines not written yet
std::vector<std::string>& held_start_notes() {
    static std::vector<std::string> held;
    return held;
}

/// Where a launch file named without a folder is looked for after the current folder.
///
/// @return the finder the engine named (find_launch_files_in)
netgame::LaunchFolders& launch_folders() {
    static netgame::LaunchFolders folders;
    return folders;
}

/// Names a transport the launch block holds that this game does not have.
///
/// @param transport the block's connection type
/// @return "IPX", "a modem", "a serial cable", or the number
std::string transport_name(int32_t transport) {
    namespace transports = oa::app::netgame::launch::connection_type;
    switch (transport) {
    case transports::ipx:
        return "IPX";
    case transports::modem:
        return "a modem";
    case transports::serial:
        return "a serial cable";
    default:
        return "transport " + std::to_string(transport);
    }
}

/// Parses a UDP port number.
///
/// Anything but a whole number from 1 through 65535 throws std::runtime_error
/// "--dplay-port expects a port from 1 through 65535".
///
/// @param text The option's value.
/// @return The port.
[[nodiscard]] uint16_t parse_port(std::string_view text) {
    uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0 ||
        value > 65535)
        throw std::runtime_error("--dplay-port expects a port from 1 through 65535");
    return static_cast<uint16_t>(value);
}

/// Parses an application GUID.
///
/// Anything but eight-four-four-four-twelve hexadecimal digits, any case,
/// with or without braces, throws std::runtime_error
/// "--dplay-guid expects a GUID, with or without braces".
///
/// @param text The option's value.
/// @return The GUID in wire byte order.
[[nodiscard]] std::array<uint8_t, 16> parse_guid(std::string_view text) {
    std::array<uint8_t, 16> bytes{};
    if (!oa::netgame::parse_application_guid(text, bytes.data()))
        throw std::runtime_error("--dplay-guid expects a GUID, with or without braces");
    return bytes;
}

/// Turns a content choice the launch named into a refusal that names the field.
///
/// @param source the launch's file, as a refusal names it
/// @param problem the message `--content` would have reported
/// @return the launch refusal, naming the file and the field
std::string launch_content_error(std::string_view source, std::string_view problem) {
    const std::string prefix = std::string(source) + ": ";
    auto line = [&](std::string_view field, std::string_view id, std::string_view why) {
        return prefix + std::string(field) + ": \"" + std::string(id) + "\" " + std::string(why);
    };
    constexpr std::string_view unknown_base = "--content names an unknown base game: ";
    constexpr std::string_view expansion_as_base =
        "--content names an expansion as the base game: ";
    constexpr std::string_view unknown_expansion = "--content names an unknown expansion: ";
    constexpr std::string_view base_as_expansion = "--content names a base game as an expansion: ";
    if (problem.starts_with(unknown_base))
        return line(
            "content.base-game",
            problem.substr(unknown_base.size()),
            "is not an installed base game"
        );
    if (problem.starts_with(expansion_as_base))
        return line(
            "content.base-game",
            problem.substr(expansion_as_base.size()),
            "is not an installed base game"
        );
    if (problem.starts_with(unknown_expansion))
        return line(
            "content.expansions",
            problem.substr(unknown_expansion.size()),
            "is not an installed expansion"
        );
    if (problem.starts_with(base_as_expansion))
        return line(
            "content.expansions",
            problem.substr(base_as_expansion.size()),
            "is not an installed expansion"
        );
    return prefix + "content.base-game: " + std::string(problem);
}

/// Returns the NetgameContext behind an extension context pointer.
///
/// @param context The extension table's context.
/// @return The context it points to.
NetgameContext& context_of(void* context) {
    return *static_cast<NetgameContext*>(context);
}

/// Takes one of network play's long options (Extension::take_option).
///
/// --net-loopback-check makes the run an unattended headless one (the check
/// runs only in a headless run; without it the run would sit in the menus),
/// --check-host-not-found an unattended run that skips the intro,
/// --check-recording-hook an unattended headless run that skips the intro,
/// --host, --join, --launch and the launch settings' options a run that skips
/// the intro, and --list-content the same for a content list.
/// --check-opened-launch takes when (start or menu) and the file, and skips
/// the intro too. --json is
/// taken only beside --list-content. A launch setting's option is kept with
/// its value and checked with the launch. A missing or malformed value
/// throws std::runtime_error.
///
/// @param context The NetgameContext whose options receive the value.
/// @param name Option name with its dashes.
/// @param values Arguments that follow the option.
/// @param[in,out] effects Gains the option_effect bits the option implies.
/// @return False when the option is not network play's.
bool take_option(void* context, const char* name, const OptionValues& values, uint32_t& effects) {
    auto& result = context_of(context).options;
    const std::string_view argument(name);
    auto value = [&values]() -> std::string_view { return values.next(values.arguments); };
    if (argument == "--net-loopback-check") {
        result.net_loopback_ticks = parse_count(value());
        effects |= option_effect::headless_check | option_effect::unattended;
    } else if (argument == "--net-loopback-watcher")
        result.net_loopback_watcher = true;
    else if (argument == "--net-loopback-computer")
        result.net_loopback_computer = true;
    else if (argument == "--net-loopback-each-ai")
        result.net_loopback_each_ai = true;
    else if (argument == "--net-loopback-fixed-starts")
        result.net_loopback_fixed_starts = true;
    else if (argument == "--check-host-not-found") {
        result.check_host_not_found = true;
        effects |= option_effect::unattended | option_effect::skip_intro;
    } else if (argument == "--dplay-port") {
        result.dplay_port = parse_port(value());
        result.cli_dplay_port = true;
    } else if (argument == "--dplay-guid") {
        result.dplay_guid = parse_guid(value());
        result.cli_dplay_guid = true;
    } else if (argument == "--launch") {
        result.launch_path = std::string(value());
        effects |= option_effect::skip_intro;
    } else if (argument == "--list-content") {
        result.list_content = true;
        effects |= option_effect::skip_intro;
    } else if (argument == "--check-opened-launch") {
        const std::string_view when = value();
        if (when == "start")
            result.opened_launch_check = OpenedLaunchCheck::start;
        else if (when == "menu")
            result.opened_launch_check = OpenedLaunchCheck::menu;
        else
            throw std::runtime_error("--check-opened-launch expects start or menu, then a file");
        result.opened_launch_check_file = std::string(value());
        effects |= option_effect::skip_intro;
    } else if (argument == "--json")
        result.json_requested = true;
    else if (argument == "--play-demo")
        result.play_demo = path_from_utf8(value());
    else if (argument == "--check-recording-hook") {
        result.check_recording_hook = true;
        effects |=
            option_effect::headless_check | option_effect::unattended | option_effect::skip_intro;
    } else if (argument == "--host" || argument == "--join") {
        using Kind = oa::ui::frontend_multiplayer::DirectGame::Kind;
        const Kind kind = argument == "--host" ? Kind::host : Kind::join;
        auto& game = result.direct_game;
        if (game.kind != Kind::none && game.kind != kind)
            throw std::runtime_error("--host and --join cannot be used together");
        game.kind = kind;
        if (kind == Kind::join)
            game.address = std::string(value());
        effects |= option_effect::skip_intro;
    } else if (argument == "--player-name") {
        result.direct_game.player_name = std::string(value());
        result.cli_player_name = true;
    } else if (argument == "--game-name") {
        result.direct_game.game_name = std::string(value());
        result.cli_game_name = true;
    } else if (argument == "--game-password") {
        result.direct_game.password = std::string(value());
        result.cli_password = true;
    } else if (argument == "--net-record")
        result.net_record = path_from_utf8(value());
    else if (argument == "--replay-viewer")
        result.replay_viewer = true;
    else if (argument == "--demo-unit-table") {
        const auto mode = std::string(value());
        if (mode != "strict" && mode != "ignore")
            throw std::runtime_error("--demo-unit-table expects strict or ignore");
        result.demo_ignore_unit_table = mode == "ignore";
    } else if (netgame::find_launch_option(argument) != nullptr) {
        std::string given;
        if (netgame::launch_option_takes_value(argument))
            given = std::string(value());
        result.launch_options.emplace_back(std::string(argument), std::move(given));
        effects |= option_effect::skip_intro;
    } else
        return false;
    return true;
}

/// Copies an option's text into a launch field, or refuses when it does not fit.
///
/// The message names the option and the limit. It does not repeat the text,
/// so a password is never written out.
///
/// @param field the field, including room for a terminating NUL
/// @param text the text
/// @param option the option that gave it
void store_launch_text(std::span<char> field, std::string_view text, std::string_view option) {
    if (field.empty() || text.size() >= field.size()) {
        const auto limit = field.empty() ? std::size_t{0} : field.size() - 1;
        throw LaunchRefusal(
            std::string(option) + ": is longer than " + std::to_string(limit) + " characters"
        );
    }
    oa::base::text::copy_terminated(field, text);
}

/// Refuses a name or a password the command line gives a game to host or join that is longer than the game keeps.
///
/// The message names the option and the limit (kPlayerNameLimit,
/// kGameNameLimit, kPasswordLimit). It does not repeat the text, so a
/// password is never written out.
///
/// @param options the parsed options, a game to host or join among them
void check_direct_game_texts(const NetOptions& options) {
    const auto refuse_longer = [](std::string_view text,
                                  std::size_t limit,
                                  std::string_view option) {
        if (text.size() > limit)
            throw LaunchRefusal(
                std::string(option) + ": is longer than " + std::to_string(limit) + " characters"
            );
    };
    const auto& game = options.direct_game;
    if (options.cli_player_name)
        refuse_longer(game.player_name, netgame::kPlayerNameLimit, "--player-name");
    if (options.cli_game_name)
        refuse_longer(game.game_name, netgame::kGameNameLimit, "--game-name");
    if (options.cli_password)
        refuse_longer(game.password, netgame::kPasswordLimit, "--game-password");
}

/// Puts a launch plan into the context, then puts the command line's own fields back.
///
/// --host, --join, --player-name, --game-name, --game-password, --dplay-guid
/// and --dplay-port keep the field they named, wherever they stand. "-h" and
/// "-n" after the last "-c" replace the file's game name, host flag,
/// transport and address, as in 3.1c. The role then follows the first that
/// applies: --host or --join; an INI that, with "-h" and "-n", names none of
/// the transport, the host flag and the address hosts over TCP/IP; an INI
/// otherwise decides as 3.1c's screens do (role_from_record), which may
/// leave the choice to the multiplayer screens; "-h" hosts an .oalaunch
/// file's game; the file's game.role. The plan is then checked as a whole. A
/// nonzero pfs sets the send rate in place of "-p". The password is stored
/// and not written out.
///
/// @param[in,out] netgame the context the plan and the command line share
/// @param plan the launch, from a file, standard input, the launch settings'
///             options or "-b"
/// @return false when the plan holds "-b" words alone, which a run without a
///         launch does not apply; nothing is planned then
bool apply_launch_plan(NetgameContext& netgame, LaunchPlan plan) {
    namespace nl = oa::app::netgame::launch;
    using Kind = oa::ui::frontend_multiplayer::DirectGame::Kind;
    auto& options = netgame.options;
    auto& switches = netgame.launch;

    const Kind cli_kind = options.direct_game.kind;
    if (cli_kind == Kind::none && plan.source.empty() && options.launch_options.empty()) {
        keep_start_note(
            "-b names battle room options, which only a launch applies: -c FILE, --launch FILE, "
            "or --host or --join; it does nothing here"
        );
        return false;
    }
    const std::string cli_address = options.direct_game.address;
    const std::string cli_player = options.direct_game.player_name;
    const std::string cli_game = options.direct_game.game_name;
    const std::string cli_password = options.direct_game.password;
    const bool cli_player_set = options.cli_player_name;
    const bool cli_game_set = options.cli_game_name;
    const bool cli_password_set = options.cli_password;
    const auto cli_port = options.dplay_port;
    const auto cli_guid = options.dplay_guid;
    const bool from_session = switches.session_from_switch;
    const std::string saved_session(switches.block.session_name);
    const bool from_type = switches.connection_type_from_switch;
    const int32_t saved_type = switches.block.connection_type;
    const bool from_address = switches.address_from_switch;
    const std::string saved_address(switches.block.address);
    const bool switch_named = from_session || from_type || from_address;

    switches.block = plan.block;
    // An INI that names none of the three hosts over TCP/IP. When a switch
    // names one, the record decides instead, from the file's own values.
    const bool record_decides = plan.ini && (plan.connection_named || switch_named);
    if (plan.ini && !plan.connection_named && switch_named) {
        switches.block.connection_type = nl::connection_type::none;
        switches.block.hosting = 0;
    }
    if (from_session) {
        store_launch_text(switches.block.session_name, saved_session, "-h");
        switches.block.hosting = 1;
    }
    if (from_type)
        switches.block.connection_type = saved_type;
    if (from_address)
        store_launch_text(switches.block.address, saved_address, "-n");

    LaunchRole role = plan.role;
    std::string join_address = from_address ? saved_address : plan.address;
    if (record_decides) {
        role = netgame::role_from_record(switches.block);
        join_address = switches.block.address;
    } else if (from_session) {
        role = LaunchRole::host;
    }
    if (cli_kind == Kind::host) {
        options.direct_game.kind = Kind::host;
        options.direct_game.address.clear();
        switches.block.hosting = 1;
    } else if (cli_kind == Kind::join) {
        options.direct_game.kind = Kind::join;
        options.direct_game.address = cli_address;
        switches.block.hosting = 0;
        if (!from_address)
            store_launch_text(switches.block.address, cli_address, "--join");
    } else if (role == LaunchRole::host) {
        options.direct_game.kind = Kind::host;
        options.direct_game.address.clear();
        switches.block.hosting = 1;
        if (!from_type)
            switches.block.connection_type = nl::connection_type::tcpip;
    } else if (role == LaunchRole::join) {
        options.direct_game.kind = Kind::join;
        options.direct_game.address = join_address;
        switches.block.hosting = 0;
        if (!from_type)
            switches.block.connection_type = nl::connection_type::tcpip;
    } else if (plan.ini) {
        // The multiplayer screens choose, as 3.1c's menus do, with the
        // record's transport, host flag and address.
        options.direct_game.kind = Kind::none;
    } else if (!plan.source.empty()) {
        throw LaunchRefusal(
            plan.source + ": game.role: expected host or join, or --host or --join beside the file"
        );
    } else {
        throw LaunchRefusal(options.launch_options.front().first + " needs --host or --join");
    }
    plan.role = options.direct_game.kind == Kind::host   ? LaunchRole::host
                : options.direct_game.kind == Kind::join ? LaunchRole::join
                                                         : LaunchRole::unset;
    plan.address = options.direct_game.address;

    if (cli_game_set) {
        options.direct_game.game_name = cli_game;
        store_launch_text(
            std::span<char>(switches.block.session_name).first(netgame::kGameNameLimit + 1),
            cli_game,
            "--game-name"
        );
    } else if (from_session) {
        options.direct_game.game_name = saved_session;
    } else {
        options.direct_game.game_name = plan.game_name;
    }

    if (cli_player_set) {
        options.direct_game.player_name = cli_player;
        store_launch_text(
            std::span<char>(switches.block.user_name).first(netgame::kPlayerNameLimit + 1),
            cli_player,
            "--player-name"
        );
    } else {
        options.direct_game.player_name = plan.player_name;
    }

    if (cli_password_set) {
        options.direct_game.password = cli_password;
        store_launch_text(
            std::span<char>(switches.block.password).first(netgame::kPasswordLimit + 1),
            cli_password,
            "--game-password"
        );
    } else {
        options.direct_game.password = plan.password;
    }

    if (options.cli_dplay_port)
        options.dplay_port = cli_port;
    else
        options.dplay_port = plan.dplay_port;
    if (options.cli_dplay_guid)
        options.dplay_guid = cli_guid;
    else
        options.dplay_guid = plan.dplay_guid;

    // pfs sets the send rate as the connection starts, in place of -p's.
    if (switches.block.pfs != 0) {
        switches.send_pacing = switches.block.pfs;
        switches.send_pacing_set = 1;
    }

    netgame::check_launch_plan(plan);
    options.launch_content = plan.content;
    options.launch_status = plan.status;
    options.launch_planned = true;
    options.launch_source = plan.source;
    options.launch_replaced = plan.replaced;
    for (const auto& [field, key] : plan.ini_keys)
        if (std::none_of(
                plan.replaced.begin(), plan.replaced.end(), [&field](const auto& replaced) {
                    return replaced.first == field;
                }
            ))
            options.launch_replaced.emplace_back(field, plan.source + ": " + key);
    netgame::keep_launch_status(plan.status);
    for (std::string& note : plan.notes)
        keep_start_note(std::move(note));
    return true;
}

/// Reads the launch the command line names: a file, standard input, "-b" and the launch settings' options.
///
/// "-c FILE" and --launch FILE name one file of either kind, and cannot both
/// name one; a "-c" with no file names none, as in 3.1c. Without either, a
/// launch piped in on standard input is read when the program allows it.
/// The battle room options "-b" named after the last "-c" then replace the
/// file's, and each launch setting's option replaces its field, in
/// command-line order. "-b" and the options alone make a plan of their own.
///
/// @param netgame the context the command line filled
/// @return the plan, or nothing when the command line names no launch
std::optional<LaunchPlan> read_named_launch(const NetgameContext& netgame) {
    const auto& options = netgame.options;
    const auto& switches = netgame.launch;
    const bool c_file = switches.launch_seen && !switches.launch_path.empty();
    if (c_file && !options.launch_path.empty())
        throw LaunchRefusal("-c and --launch cannot both be given");
    if (switches.launch_seen && !c_file)
        keep_start_note("-c names no file: it skips the intro and starts no launch, as in 3.1c");
    const std::string& path = c_file ? switches.launch_path : options.launch_path;
    std::optional<LaunchPlan> plan;
    if (!path.empty()) {
        plan = netgame::read_launch_file(path, launch_folders());
    } else if (options.piped_launch_allowed && !switches.launch_seen) {
        const auto piped = netgame::read_piped_launch(
            netgame::standard_input(), netgame::kPipedLaunchWait, netgame::kPipedLaunchRest
        );
        if (piped.found)
            plan = netgame::read_launch_text("standard input", piped.text);
    }
    if (!plan && (!options.launch_options.empty() || switches.battle.named()))
        plan.emplace();
    if (plan)
        oa::app::netgame::launch::apply_battle_words(switches.battle, plan->block);
    for (const auto& [option, given] : options.launch_options)
        netgame::replace_launch_field(*plan, *netgame::find_launch_option(option), option, given);
    return plan;
}

/// Keeps a note for a transport the launch block holds that this game does not have.
///
/// The multiplayer screens then open the provider list, as 3.1c's do when
/// the transport is not installed.
///
/// @param netgame the context the command line filled
void note_missing_transport(const NetgameContext& netgame) {
    namespace transports = oa::app::netgame::launch::connection_type;
    const int32_t transport = netgame.launch.block.connection_type;
    if (transport == transports::none || transport == transports::tcpip)
        return;
    const auto& options = netgame.options;
    const std::string who = netgame.launch.connection_type_from_switch || !options.launch_planned
                                ? std::string("-n")
                                : options.launch_source + ": connect";
    keep_start_note(
        who + " names " + transport_name(transport) +
        ", which this game does not have; the multiplayer screens open on the provider list"
    );
}

/// Checks the network game options as a whole (Extension::check_options).
///
/// A launch is read here, after the long options and the game switches, and
/// each field the command line named is put back. --player-name,
/// --game-name and --game-password need --host or --join, or a launch, and
/// are refused when longer than the game keeps. A
/// game to host or join has the frontend open multiplayer over TCP/IP at
/// once, whatever connection "-n" named. --list-content needs --json, and
/// --json needs --list-content. A refused combination throws
/// std::runtime_error. A launch that cannot be used is kept, and
/// raise_launch_refusal raises it after the hook returns. The notes of the
/// switches and the launch are kept for the log (keep_start_note), and a
/// transport this game does not have is noted.
///
/// @param context The NetgameContext whose options are checked.
void check_options(void* context) {
    auto& netgame = context_of(context);
    auto& options = netgame.options;
    options.launch_refusal.clear();
    held_launch_refusal().clear();
    held_start_notes() = netgame.launch.notes;
    netgame::keep_launch_status({});
    try {
        if (options.list_content && !options.json_requested)
            throw std::runtime_error("--list-content needs --json");
        if (options.json_requested && !options.list_content)
            throw std::runtime_error("--json needs --list-content");
        if (std::optional<LaunchPlan> plan = read_named_launch(netgame))
            (void)apply_launch_plan(netgame, std::move(*plan));
        const auto& game = options.direct_game;
        if (game.kind == oa::ui::frontend_multiplayer::DirectGame::Kind::none) {
            note_missing_transport(netgame);
            // A launch the multiplayer screens choose for takes the names.
            if (options.launch_planned)
                return;
            if (!game.player_name.empty())
                throw std::runtime_error("--player-name needs --host or --join");
            if (!game.game_name.empty())
                throw std::runtime_error("--game-name needs --host or --join");
            if (!game.password.empty())
                throw std::runtime_error("--game-password needs --host or --join");
            return;
        }
        check_direct_game_texts(options);
        oa::app::netgame::launch::set_connection_type(
            netgame.launch, oa::app::netgame::launch::connection_type::tcpip
        );
    } catch (const LaunchRefusal& refusal) {
        options.launch_refusal = refusal.what();
        held_launch_refusal() = options.launch_refusal;
    }
}

/// Returns the handler of the 3.1c network switches (Extension::switch_handler).
///
/// @param context The NetgameContext holding the handler.
/// @return The handler fill_option_hooks bound to the context's LaunchSwitches.
const oa::app::command_line::SwitchHandler* switch_handler(void* context) {
    return &context_of(context).switches;
}

/// Returns network play's options for the run options' line of the usage.
///
/// The launch settings network play reads come from the table of launch
/// settings, in its order, so --help names each with the value it takes.
///
/// @return the text, made once
const std::string& usage_runs() {
    static const std::string runs = [] {
        std::string listed = "[--launch FILE|-] ";
        for (const netgame::LaunchField& field : netgame::launch_fields()) {
            if (field.reader != netgame::LaunchOptionReader::network_play &&
                field.reader != netgame::LaunchOptionReader::direct_game)
                continue;
            // --join ADDRESS, the address's option, is already part of the role's.
            if (listed.find(field.option) != std::string::npos)
                continue;
            listed += "[" + std::string(field.option) + "] ";
        }
        listed += "[--list-content --json] "
                  "[--net-loopback-check TICKS] [--net-loopback-watcher] "
                  "[--net-loopback-computer] [--net-loopback-each-ai] "
                  "[--net-loopback-fixed-starts] [--check-host-not-found] "
                  "[--check-opened-launch start|menu FILE] "
                  "[--play-demo FILE.tad] "
                  "[--demo-unit-table strict|ignore] [--net-record FILE] [--replay-viewer] ";
        return listed;
    }();
    return runs;
}

/// Returns network play's wording of the usage text and of the line -r ends the start with (Extension::text).
///
/// @param context Extension context (unused).
/// @param which Text asked for.
/// @return The text; "" for no usage note.
const char* text(void* /*context*/, ExtensionText which) {
    switch (which) {
    case ExtensionText::usage_checks:
        return "[--check-recording-hook] ";
    case ExtensionText::usage_runs:
        return usage_runs().c_str();
    case ExtensionText::usage_switches:
        return "-b WORD -c FILE|- -e PERCENT -h NAME -n TYPE[:ADDRESS] -p RATE -t SECONDS -y ";
    case ExtensionText::usage_note:
        return "";
    case ExtensionText::register_switch:
        return "-r registers the game as a DirectPlay application, which this game does not "
               "need; it ends here, as 3.1c does after -r";
    }
    return nullptr;
}

/// Hands the frontend the launch's names (Extension::frontend_entry).
///
/// The game name is the launch block's session name, which "-h" and
/// a launch write. While a launch is active the launch's
/// user name is the nickname.
///
/// @param context The NetgameContext holding the switches.
/// @param[out] entry Receives game_name, and nickname under an active launch.
void frontend_entry(void* context, FrontendEntry& entry) {
    const auto& netgame = context_of(context);
    const auto& block = netgame.launch.block;
    entry.game_name = block.session_name;
    const bool launched = netgame.launch_active != nullptr && netgame.launch_active(nullptr);
    if (launched)
        entry.nickname = block.user_name;
}

/// Starts the multiplayer frontend states with the fields the network switches set (Extension::frontend_states).
///
/// A connection type ("-n", or a launch's) opens multiplayer from the main
/// menu at once and a setup request (request_setup, which a launching
/// extension makes) asks for step setup_requested on the first main-menu
/// update. The lines kept about the command line are written here, the
/// first time: the game's log is open by now.
///
/// @param context The NetgameContext holding the switches and the states.
/// @param[out] handler Receives the handler that runs the states.
void frontend_states(void* context, oa::ui::frontend_state::StateHandler& handler) {
    auto& netgame = context_of(context);
    for (const std::string& line : held_start_notes())
        oa::platform::log_files::write_line("open-annihilation: " + line);
    held_start_notes().clear();
    netgame.frontend.state.connection_type = netgame.launch.block.connection_type;
    netgame.frontend.state.setup_request = netgame.launch.netsetup;
    handler = oa::netgame::frontend::state_handler(netgame.frontend);
}

/// Answers init_score_reporting: 0, as joining never waits for a match report to start.
///
/// The report starts as the battle room's match launches instead
/// (NetworkPlay::start_reporter).
///
/// @param ctx Unused.
/// @param state Unused.
/// @return 0.
uint32_t query_score_reporting(ScreenContext* /*ctx*/, void* /*state*/) noexcept {
    return 0;
}

/// Answers transport_kind: TCP/IP, over which every game plays.
///
/// @param ctx Unused.
/// @param state Unused.
/// @return oa::netgame::frontend::transport_kind::tcpip.
uint32_t query_transport_kind(ScreenContext* /*ctx*/, void* /*state*/) noexcept {
    return oa::netgame::frontend::transport_kind::tcpip;
}

/// Writes the -t and -e values into a new match's Game block (Extension::match_game).
///
/// @param context The NetgameContext holding the switches.
/// @param[out] game Receives player_timeout_seconds and send_error_percent.
void match_game(void* context, oa::Game& game) {
    const auto& launch = context_of(context).launch;
    game.player_timeout_seconds = launch.net_timeout_seconds;
    game.send_error_percent = launch.send_error_percent;
}

} // namespace

void allow_piped_launch() noexcept {
    netgame_context().options.piped_launch_allowed = true;
}

std::string launch_place(std::string_view field) {
    const auto& net = netgame_context().options;
    for (const auto& [replaced, option] : net.launch_replaced)
        if (replaced == field)
            return option;
    if (net.launch_source.empty())
        return std::string(field);
    return net.launch_source + ": " + std::string(field);
}

void find_launch_files_in(const netgame::LaunchFolders& folders) noexcept {
    launch_folders() = folders;
}

void keep_start_note(std::string line) {
    held_start_notes().push_back(std::move(line));
}

const std::vector<std::string>& start_notes() noexcept {
    return held_start_notes();
}

void note_game_switches(const Options& options) {
    const auto& switches = options.launch;
    if (switches.developer_switches == 1)
        keep_start_note(
            std::string(switches.developer_switch) +
            " is one of 3.1c's developer switches, which have no effect"
        );
    else if (switches.developer_switches > 1)
        keep_start_note(
            std::string(switches.developer_switch) + " and " +
            std::to_string(switches.developer_switches - 1) +
            " more are 3.1c's developer switches, which have no effect"
        );
    if (switches.forced_disc_drive != 0)
        keep_start_note(
            "-f looks for 3.1c's disc on drive H:; this game reads the installed game, so -f "
            "has no effect"
        );
    if (switches.l_switch_flag == 0)
        keep_start_note("-l has no effect, as in 3.1c");
    if (switches.system_sound != 0)
        keep_start_note(
            "-w plays 3.1c's sounds through the system's sound player; this game has no second "
            "player, so -w turns the sound off, as -s does"
        );
}

void raise_launch_refusal() {
    if (held_launch_refusal().empty())
        return;
    const std::string refusal = std::move(held_launch_refusal());
    held_launch_refusal().clear();
    throw netgame::LaunchRefusal(refusal);
}

NetgameContext& netgame_context() {
    static NetgameContext context{};
    return context;
}

void register_frontend_state_queries(ScreenRegistry* registry) noexcept {
    namespace query = oa::netgame::frontend::query;
    (void)query_register(registry, query::init_score_reporting, query_score_reporting, nullptr);
    (void)query_register(registry, query::transport_kind, query_transport_kind, nullptr);
}

void adopt_launch_run(Options& options) {
    auto& net = netgame_context().options;
    if (!net.launch_planned)
        return;
    if (net.launch_content.named && options.content.empty()) {
        std::string text = net.launch_content.base_game;
        if (!net.launch_content.expansions.empty()) {
            if (!text.empty())
                text.push_back(':');
            for (std::size_t at = 0; at < net.launch_content.expansions.size(); ++at) {
                if (at != 0)
                    text.push_back(',');
                text += net.launch_content.expansions[at];
            }
        }
        if (!text.empty()) {
            const oa::data::content_bundles::ContentChoice named =
                oa::data::content_bundles::parse_content_argument(text);
            if (!named.error.empty())
                throw LaunchRefusal(launch_content_error(net.launch_source, named.error));
            options.content = std::move(text);
            net.launch_content_applied = true;
        }
    }
    if (!net.launch_content.mod.empty() && options.mod_file.empty() && options.mod_dir.empty() &&
        !options.base_game)
        net.launch_mod_pending = true;
    // The first --mutator replaces the file's whole list. A headless run
    // takes the list here too: adopt_launch_run runs before the match.
    if (net.launch_content.mutators_named && options.mutators.empty()) {
        options.mutators.reserve(net.launch_content.mutators.size());
        options.mutator_fields.reserve(net.launch_content.mutators.size());
        options.mutator_versions.reserve(net.launch_content.mutators.size());
        for (std::size_t index = 0; index < net.launch_content.mutators.size(); ++index) {
            const netgame::LaunchMutator& one = net.launch_content.mutators[index];
            oa::data::mutators::MutatorChoice choice;
            choice.target = one.id;
            choice.options = one.options;
            options.mutators.push_back(oa::data::mutators::format_choice(choice));
            options.mutator_fields.push_back("content.mutators[" + std::to_string(index + 1) + "]");
            options.mutator_versions.push_back(one.version);
        }
    }
}

void put_launch_in_place(NetgameContext& netgame, LaunchPlan plan) {
    // A refusal half way leaves nothing behind: a launch block left with a
    // connection type would open multiplayer at the main menu's next start.
    const NetOptions options = netgame.options;
    const oa::app::netgame::launch::LaunchSwitches switches = netgame.launch;
    const netgame::LaunchStatusChoice status = netgame::kept_launch_status();
    try {
        // As check_options does: a launch that hosts or joins plays over
        // TCP/IP; one that leaves the choice to the multiplayer screens keeps
        // the transport it names. A file never holds "-b" words alone.
        if (apply_launch_plan(netgame, std::move(plan)) &&
            netgame.options.direct_game.kind !=
                oa::ui::frontend_multiplayer::DirectGame::Kind::none)
            oa::app::netgame::launch::set_connection_type(
                netgame.launch, oa::app::netgame::launch::connection_type::tcpip
            );
    } catch (...) {
        netgame.options = options;
        netgame.launch = switches;
        netgame::keep_launch_status(status);
        throw;
    }
}

void fill_option_hooks(Extension& table, NetgameContext& context) {
    context.switches = oa::app::netgame::launch::launch_switch_handler(&context.launch);
    table.context = &context;
    table.take_option = take_option;
    table.check_options = check_options;
    table.switch_handler = switch_handler;
    table.text = text;
    table.frontend_entry = frontend_entry;
    table.frontend_states = frontend_states;
    table.match_game = match_game;
}

} // namespace oa::app
