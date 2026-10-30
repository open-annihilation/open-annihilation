// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A launch fills one plan from Open Annihilation's .oalaunch file, from TA
// Forever's INI, from the options that name each launch setting, or from a
// launch piped in on standard input. -c and --launch read either file by
// what it holds, from a path with spaces in it too, an option beside the
// file replaces its field, and every refusal names the field or the option.
// The INI is read as 3.1c reads it:
// every key of its table, a Windows settings file's rules, hosting and joining
// from connect, creator and address, -h, -n and -b around -c, and the lines
// the start keeps for the log.
#include "net_options.hpp"
#include "oa/app/app.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/netgame/launch_file.hpp"
#include "oa/netgame/dplay.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace mp = oa::ui::frontend_multiplayer;
namespace nl = oa::ui::frontend_multiplayer::launch;
namespace launch_file = oa::app::netgame;

namespace {

int failures = 0;

void expect(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what.c_str());
        ++failures;
    }
}

/// Removes the scratch directory when the checks end.
struct Scratch {
    fs::path dir;

    Scratch() {
        dir = fs::temp_directory_path() / "G104-launch-file";
        std::error_code failure;
        fs::remove_all(dir, failure);
        fs::create_directories(dir);
    }

    ~Scratch() {
        std::error_code failure;
        fs::remove_all(dir, failure);
    }

    /// Writes one scratch file and returns its path as UTF-8.
    ///
    /// @param name the file's name
    /// @param text the bytes
    /// @return the path
    std::string write(const char* name, std::string_view text) const {
        const fs::path file = dir / name;
        std::ofstream output(file, std::ios::binary);
        expect(static_cast<bool>(output), "the scratch file opens");
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        expect(static_cast<bool>(output), "the scratch file is written");
        return oa::app::path_to_utf8(file);
    }
};

constexpr std::string_view kIni = R"([totala]
watching=2
mapmode=2
location=2
cheats=1
los=1
deathopt=2
metal=1000
energy=1000
lockOptions=0
playerlimit=4
pfs=0
session=Example game
provider=TAForever
mission=Acid Foursome
timelimit=0
registeredonly=0
tournment=1
tournament=1
)";

// Every launch setting, each with a value other than the battle room's own.
constexpr std::string_view kLaunch = R"(oalaunch: 1
game:
  role: host
  name: Example game
  password: secret
  player-limit: 6
  launcher-name: Ladder
  tournament: off
  closed: off
  directplay:
    application-guid: "{99797420-F5F5-11CF-9827-00A0241496C8}"
    port: 47624
player:
  name: Commander
  side: core
  team: 3
  colour: 5
map: Acid Foursome
rules:
  commander-death: ends
  metal: 1200
  energy: 1500
  unit-limit: 250
  line-of-sight: circular
  mapping: unmapped
  start-positions: fixed
  cheats: on
  watchers: off
  time-limit: 30
  lock-options: true
  unit-restrictions:
    CORKROG: 0
    ARMFLASH: no-limit
    armmav: 5
computer-players:
  - side: core
    team: 2
    colour: 7
    difficulty: medium
  - side: arm
content:
  base-game: totala
  expansions: [core-contingency]
  mod: ridge
  mutators:
    - wide-sight
    - id: fast-builders
      version: "1.0"
      options:
        factor: 2
        rush: on
status:
  standard-output: true
  file: status.jsonl
  quit-after-game: after-score
)";

// A player who joins, and watches.
constexpr std::string_view kJoin = R"(oalaunch: 1
game:
  role: join
  address: 192.0.2.10
player:
  side: watcher
)";

// The same settings as kLaunch's network play reads, as options.
const std::vector<const char*> kLaunchOptions = {
    "--host",
    "--game-name",
    "Example game",
    "--game-password",
    "secret",
    "--player-limit",
    "6",
    "--launcher-name",
    "Ladder",
    "--tournament=off",
    "--closed=off",
    "--dplay-guid",
    "{99797420-F5F5-11CF-9827-00A0241496C8}",
    "--dplay-port",
    "47624",
    "--player-name",
    "Commander",
    "--side",
    "core",
    "--team",
    "3",
    "--colour",
    "5",
    "--map",
    "Acid Foursome",
    "--commander-death",
    "ends",
    "--metal",
    "1200",
    "--energy",
    "1500",
    "--unit-limit",
    "250",
    "--line-of-sight",
    "circular",
    "--mapping",
    "unmapped",
    "--start-positions",
    "fixed",
    "--cheats",
    "on",
    "--watchers",
    "off",
    "--time-limit",
    "30",
    "--lock-options",
    "--unit-restriction",
    "CORKROG=0",
    "--unit-restriction",
    "ARMFLASH=no-limit",
    "--unit-restriction",
    "armmav=5",
    "--computer-player",
    "core:2:7:medium",
    "--computer-player",
    "arm",
};

// A host with the room's own values, which each option below replaces.
constexpr std::string_view kPlainHost = R"(oalaunch: 1
game:
  role: host
  name: Plain
player:
  side: arm
rules:
  watchers: on
)";

bool same_text(const char* field, const char* expected) {
    return std::strcmp(field, expected) == 0;
}

/// Tells whether two launch blocks hold the same launch.
///
/// @param left one block
/// @param right the other
/// @return true when every field matches
bool same_block(const nl::LaunchBlock& left, const nl::LaunchBlock& right) {
    bool same =
        left.connection_type == right.connection_type && left.hosting == right.hosting &&
        left.player_limit == right.player_limit && left.time_limit == right.time_limit &&
        left.pfs == right.pfs && left.lock_options == right.lock_options &&
        left.max_units == right.max_units && left.energy == right.energy &&
        left.metal == right.metal && left.death_option == right.death_option &&
        left.los == right.los && left.cheats == right.cheats && left.location == right.location &&
        left.mapping == right.mapping && left.watching == right.watching &&
        left.tournament == right.tournament && left.faction == right.faction &&
        left.team == right.team && left.color == right.color && left.watcher == right.watcher &&
        left.computer_count == right.computer_count && left.closed == right.closed &&
        left.restriction_count == right.restriction_count &&
        same_text(left.faction_name, right.faction_name) &&
        same_text(left.provider_label, right.provider_label) &&
        same_text(left.session_name, right.session_name) &&
        same_text(left.mission, right.mission) && same_text(left.password, right.password) &&
        same_text(left.user_name, right.user_name) && same_text(left.address, right.address);
    for (int32_t at = 0; same && at < left.computer_count && at < nl::kLaunchComputerLimit; ++at) {
        const auto& one = left.computers[at];
        const auto& other = right.computers[at];
        same = one.faction == other.faction && one.team == other.team && one.color == other.color &&
               one.difficulty == other.difficulty && same_text(one.brain, other.brain) &&
               same_text(one.faction_name, other.faction_name);
    }
    for (int32_t at = 0; same && at < left.restriction_count && at < nl::kLaunchRestrictionLimit;
         ++at)
        same = same_text(left.restrictions[at].unit, right.restrictions[at].unit) &&
               left.restrictions[at].limit == right.restrictions[at].limit;
    return same;
}

/// The message a launch text is refused with, or empty when it is accepted.
///
/// @param text the file's bytes
/// @return the refusal
std::string refused_text(std::string_view text) {
    try {
        (void)launch_file::read_launch_text("launch.oalaunch", text);
    } catch (const launch_file::LaunchRefusal& error) {
        return error.what();
    }
    return {};
}

struct Parsed {
    oa::app::NetOptions net;
    oa::app::Options app;
    oa::app::netgame::launch::LaunchSwitches launch;
};

/// Parses one command line through the game's option hooks.
///
/// @param arguments the arguments after the program name
/// @return the options and the launch switches
Parsed parse(const std::vector<const char*>& arguments) {
    std::vector<std::string> storage{"open-annihilation"};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : storage)
        argv.push_back(argument.data());
    oa::app::NetgameContext context{};
    oa::app::Extension table{};
    oa::app::fill_option_hooks(table, context);
    auto app = oa::app::parse_options(static_cast<int>(argv.size()), argv.data(), table);
    return {context.options, std::move(app), context.launch};
}

/// The message a command line is refused with, or empty when it is accepted.
///
/// @param arguments the arguments after the program name
/// @return the refusal
std::string rejection(const std::vector<const char*>& arguments) {
    try {
        (void)parse(arguments);
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    return {};
}

/// Joins two argument lists.
///
/// @param first the first
/// @param second the second
/// @return first, then second
std::vector<const char*>
joined(const std::vector<const char*>& first, const std::vector<const char*>& second) {
    std::vector<const char*> both = first;
    both.insert(both.end(), second.begin(), second.end());
    return both;
}

/// TA Forever's file from issue 19, unchanged, fills every field it names,
/// reads registeredonly and leaves tournment and tournament unread.
///
/// @param path the example file
void check_ini(const std::string& path) {
    const auto plan = launch_file::read_launch_file(path);
    const auto& block = plan.block;
    expect(plan.ini && !plan.connection_named, "TA Forever's file names no connection");
    expect(
        plan.role == launch_file::LaunchRole::host && block.hosting == 1 &&
            block.connection_type == nl::connection_type::tcpip,
        "a file that names no connection hosts over TCP/IP"
    );
    expect(block.watching == 2, "watching=2 allows watchers");
    expect(block.mapping == 2, "mapmode=2 starts mapped");
    expect(block.location == 2, "location=2 starts at random");
    expect(block.cheats == 1, "cheats=1 disallows the cheat codes");
    expect(block.los == 1, "los=1 is true line of sight");
    expect(block.death_option == 2, "deathopt=2 ends the game");
    expect(block.metal == 1000 && block.energy == 1000, "metal and energy");
    expect(block.lock_options == 0, "lockOptions");
    expect(block.player_limit == 4, "playerlimit");
    expect(block.pfs == 0 && block.time_limit == 0, "pfs and timelimit");
    expect(block.tournament == 0, "tournment and tournament are not read");
    expect(same_text(block.session_name, "Example game"), "session");
    expect(same_text(block.provider_label, "TAForever"), "provider");
    expect(same_text(block.mission, "Acid Foursome"), "mission");
    expect(plan.game_name == "Example game", "the game's name");
    expect(
        block.password[0] == '\0' && block.user_name[0] == '\0' && plan.password.empty() &&
            plan.player_name.empty() && block.faction == 0 && block.team == 0 &&
            block.max_units == 0 && block.computer_count == 0 && block.address[0] == '\0',
        "registeredonly and the unnamed fields add nothing"
    );
    expect(
        plan.notes.size() == 1 && plan.notes[0].ends_with(
                                      "taforever.ini: tournment, tournament: not read, as 3.1c "
                                      "does not read them"
                                  ),
        "the two keys not read are noted once, by their names in the file"
    );
    expect(
        launch_file::launch_place(plan, "map").ends_with("taforever.ini: mission") &&
            launch_file::launch_place(plan, "game.name").ends_with("taforever.ini: session"),
        "a refusal of a field the file filled names its key"
    );
    // The table holds the 22 keys 3.1c reads, then the two it never reads;
    // each read key names a launch field, or none.
    const auto keys = launch_file::launch_ini_keys();
    expect(keys.size() == 24, "the INI's table has 24 keys");
    std::size_t read = 0;
    for (const auto& key : keys) {
        read += key.read ? 1 : 0;
        expect(
            key.field.empty() ? key.key == "connect" || key.key == "registeredonly" ||
                                    key.key == "pfs" || !key.read
                              : launch_file::find_launch_field(key.field) != nullptr,
            std::string(key.key) + " names a launch field"
        );
    }
    expect(read == 22, "3.1c reads 22 keys");
}

/// The plan an INI's `[TotalA]` lines give.
///
/// @param lines the section's lines
/// @return the plan
launch_file::LaunchPlan ini_plan(std::string_view lines) {
    return launch_file::read_launch_text("k.ini", "[TotalA]\n" + std::string(lines));
}

/// The block an INI's `[TotalA]` lines give.
///
/// @param lines the section's lines
/// @return the block
nl::LaunchBlock ini_block(std::string_view lines) {
    return ini_plan(lines).block;
}

/// Each key of the INI's table, with the values 3.1c reads and the odd ones it never refuses.
void check_ini_keys() {
    // connect: a word of three letters or more over the shorter length, in
    // 3.1c's order, else a number.
    const std::pair<const char*, int32_t> transports[] = {
        {"tcpip", 1},
        {"TCPIP", 1},
        {"tcp", 1},
        {"internet", 1},
        {"internetwork", 1},
        {"ipx", 2},
        {"novell", 2},
        {"modem", 3},
        {"serial", 4},
        {"ser", 4},
        {"1", 1},
        {"2", 2},
        {"7", 7},
        {"tc", 0},
        {"0", 0},
        {"\"tcpip\"", 1},
        {"4 cables", 4},
        {"satellite", 0},
    };
    for (const auto& [value, transport] : transports)
        expect(
            ini_block(std::string("connect=") + value + "\n").connection_type == transport,
            std::string("connect=") + value
        );
    const auto absent = ini_plan("creator=0\n");
    expect(
        absent.block.connection_type == 0 && absent.connection_named,
        "connect is 0 when absent from a file that names creator"
    );
    expect(
        !ini_plan("").connection_named && ini_block("").connection_type == 1,
        "a file that names none of connect, creator and address hosts over TCP/IP"
    );
    // creator: any whole number but 0 hosts.
    expect(ini_block("creator=1\n").hosting == 1, "creator=1 hosts");
    expect(ini_block("creator=5\n").hosting == 1, "creator=5 hosts");
    expect(ini_block("creator=-1\n").hosting == 1, "creator=-1 hosts");
    expect(ini_block("creator=0\n").hosting == 0, "creator=0 joins");
    // registeredonly: read, and nothing comes of it.
    expect(
        same_block(ini_block("registeredonly=1\n"), ini_block("registeredonly=0\n")),
        "registeredonly changes nothing"
    );
    // playerlimit: 6 when absent; 1 or less the game's own; above 10 is 10.
    const std::pair<const char*, int32_t> limits[] = {
        {"", 6},
        {"playerlimit=2\n", 2},
        {"playerlimit=10\n", 10},
        {"playerlimit=11\n", 10},
        {"playerlimit=99\n", 10},
        {"playerlimit=1\n", 0},
        {"playerlimit=0\n", 0},
        {"playerlimit=-3\n", 0},
        {"playerlimit=\n", 6},
    };
    for (const auto& [lines, limit] : limits)
        expect(ini_block(lines).player_limit == limit, std::string("player limit: ") + lines);
    // timelimit: passed on; below 0 is 0.
    expect(ini_block("timelimit=30\n").time_limit == 30, "timelimit");
    expect(ini_block("timelimit=-5\n").time_limit == 0, "a negative timelimit is 0");
    // pfs: kept for the send rate (check_ini_hosting applies it).
    expect(ini_block("pfs=12\n").pfs == 12 && ini_block("pfs=-1\n").pfs == -1, "pfs");
    // Texts: quotes dropped, cut to their fields, and the names and the
    // password to what the game keeps of them.
    const std::string long_text(80, 'x');
    const auto texts = ini_plan(
        "provider=" + long_text + "\nsession=" + long_text + "\nmission=" + long_text +
        "\npassword=" + long_text + "\nplayer=" + long_text + "\naddress=" + long_text + "\n"
    );
    expect(
        std::string_view(texts.block.provider_label).size() == 15 &&
            std::string_view(texts.block.session_name).size() == launch_file::kGameNameLimit &&
            std::string_view(texts.block.mission).size() == 63 &&
            std::string_view(texts.block.password).size() == launch_file::kPasswordLimit &&
            std::string_view(texts.block.user_name).size() == launch_file::kPlayerNameLimit &&
            std::string_view(texts.block.address).size() == 63,
        "a long text is cut to what the game keeps, never refused"
    );
    expect(
        texts.game_name.size() == launch_file::kGameNameLimit &&
            texts.password.size() == launch_file::kPasswordLimit &&
            texts.player_name.size() == launch_file::kPlayerNameLimit && texts.address.size() == 63,
        "the plan's names are cut as the block's are"
    );
    const auto named = ini_plan(
        "session=\"Friday game\"\nplayer='Commander'\npassword=\"secret\"\nprovider=\"Ladder\n"
        "mission=Acid Foursome\naddress=192.0.2.10\n"
    );
    expect(
        named.game_name == "Friday game" && same_text(named.block.user_name, "Commander") &&
            named.password == "secret" && same_text(named.block.provider_label, "\"Ladder") &&
            same_text(named.block.mission, "Acid Foursome") &&
            same_text(named.block.address, "192.0.2.10"),
        "quotes around a value are dropped, an unmatched one is kept"
    );
    // lockOptions: any whole number but 0 locks.
    expect(ini_block("lockOptions=1\n").lock_options == 1, "lockOptions=1 locks");
    expect(ini_block("lockoptions=7\n").lock_options == 1, "lockOptions=7 locks");
    expect(ini_block("LOCKOPTIONS=-1\n").lock_options == 1, "lockOptions=-1 locks");
    expect(ini_block("lockOptions=0\n").lock_options == 0, "lockOptions=0 unlocks");
    // maxunits: 0 leaves the room's; kept from 20 through 6553.
    const std::pair<const char*, int32_t> units[] = {
        {"maxunits=0\n", 0},
        {"maxunits=250\n", 250},
        {"maxunits=5\n", 20},
        {"maxunits=-10\n", 20},
        {"maxunits=9999\n", 6553},
    };
    for (const auto& [lines, limit] : units)
        expect(ini_block(lines).max_units == limit, std::string("unit limit: ") + lines);
    // energy and metal: 0 leaves the room's 1000; kept from 0 through 10000.
    const std::pair<const char*, int32_t> amounts[] = {
        {"0", 0},
        {"500", 500},
        {"10000", 10000},
        {"20000", 10000},
        {"40000", 10000},
        {"-5", 0},
    };
    for (const auto& [value, amount] : amounts) {
        expect(ini_block(std::string("energy=") + value).energy == amount, value);
        expect(ini_block(std::string("metal=") + value).metal == amount, value);
    }
    // The battle room's choices: 0, and an unknown code, leave the room's own.
    const std::tuple<const char*, int32_t nl::LaunchBlock::*, int32_t> choices[] = {
        {"deathopt=1", &nl::LaunchBlock::death_option, 1},
        {"deathopt=2", &nl::LaunchBlock::death_option, 2},
        {"deathopt=3", &nl::LaunchBlock::death_option, 3},
        {"deathopt=4", &nl::LaunchBlock::death_option, 0},
        {"deathopt=-1", &nl::LaunchBlock::death_option, 0},
        {"los=1", &nl::LaunchBlock::los, 1},
        {"los=2", &nl::LaunchBlock::los, 2},
        {"los=3", &nl::LaunchBlock::los, 3},
        {"los=0", &nl::LaunchBlock::los, 0},
        {"los=9", &nl::LaunchBlock::los, 0},
        {"cheats=2", &nl::LaunchBlock::cheats, 2},
        {"cheats=1", &nl::LaunchBlock::cheats, 1},
        {"cheats=5", &nl::LaunchBlock::cheats, 1},
        {"cheats=0", &nl::LaunchBlock::cheats, 0},
        {"location=1", &nl::LaunchBlock::location, 1},
        {"location=2", &nl::LaunchBlock::location, 2},
        {"location=7", &nl::LaunchBlock::location, 2},
        {"location=0", &nl::LaunchBlock::location, 0},
        {"mapmode=1", &nl::LaunchBlock::mapping, 1},
        {"mapmode=2", &nl::LaunchBlock::mapping, 2},
        {"mapmode=3", &nl::LaunchBlock::mapping, 2},
        {"mapmode=0", &nl::LaunchBlock::mapping, 0},
        {"watching=2", &nl::LaunchBlock::watching, 2},
        {"watching=1", &nl::LaunchBlock::watching, 1},
        {"watching=9", &nl::LaunchBlock::watching, 1},
        {"watching=0", &nl::LaunchBlock::watching, 0},
    };
    for (const auto& [line, field, code] : choices)
        expect(ini_block(std::string(line) + "\n").*field == code, line);
    // tournament and tournment: not read, whatever they hold.
    const auto unread = ini_plan("tournament=1\ntournment=1\n");
    expect(unread.block.tournament == 0, "tournament and tournment are not read");
    expect(
        unread.notes.size() == 1 &&
            unread.notes[0] == "k.ini: tournament, tournment: not read, as 3.1c does not read them",
        "they are noted"
    );
}

/// The INI is read as a Windows settings file is, and nothing in it is refused.
void check_ini_reader() {
    expect(
        launch_file::read_launch_text("k.ini", "[TOTALA]\nmetal=300\n").block.metal == 300 &&
            launch_file::read_launch_text("k.ini", "[totala]\nmetal=300\n").block.metal == 300,
        "the section's name is read in any case"
    );
    expect(
        launch_file::read_launch_text("k.ini", "[Other]\nmetal=5\n[TotalA]\nmetal=7\n")
                .block.metal == 7,
        "another section is skipped"
    );
    const auto first = launch_file::read_launch_text(
        "k.ini", "[TotalA]\nmetal=100\n[Other]\nenergy=5\n[TotalA]\nmetal=200\nenergy=300\n"
    );
    expect(
        first.block.metal == 100 && first.block.energy == 0,
        "only the first [TotalA] section is read"
    );
    expect(ini_block("metal=100\nMETAL=200\n").metal == 100, "the first copy of a key counts");
    expect(ini_block("MeTaL=500\n").metal == 500, "a key is read in any case");
    expect(ini_block("  metal  =   700   \r\n").metal == 700, "spaces around key and value");
    expect(ini_block("metal=12abc\n").metal == 12, "a number is what the value begins with");
    expect(ini_block("metal=abc\n").metal == 0, "a value without a number is 0");
    expect(ini_block("metal=+300\n").metal == 300, "a sign may come first");
    expect(ini_block("metal=\"800\"\n").metal == 800, "a quoted number");
    expect(ini_block("metal=\n").metal == 0, "an empty value is the default");
    expect(ini_block(";metal=900\n").metal == 0, "a line that begins ; is a comment");
    expect(
        ini_block("junk line without a value\nmetal=400\n").metal == 400,
        "a line without = is no setting and is not refused"
    );
    const auto noted = ini_plan("foo=1\nFOO=2\nbar=3\nmetal=1\n");
    expect(
        noted.notes.size() == 1 &&
            noted.notes[0] == "k.ini: foo, bar: not read, as 3.1c does not read them",
        "a key 3.1c does not read is noted once, by its first name"
    );
    expect(
        launch_file::read_launch_text("k.ini", "\xef\xbb\xbf[TotalA]\nmetal=600\n").block.metal ==
            600,
        "a byte order mark is skipped"
    );
    // A file of odd values throughout is read, never refused.
    std::string odd;
    for (const auto& key : launch_file::launch_ini_keys())
        odd += std::string(key.key) + "=\"-99999999999 not a number\n";
    expect(refused_text("[TotalA]\n" + odd).empty(), "no value of the INI is refused");
}

/// Who hosts or joins, from connect, creator and address, with -h, -n and the long options.
///
/// @param scratch the directory the files are written in
void check_ini_hosting(const Scratch& scratch) {
    using Kind = mp::DirectGame::Kind;
    const std::string taforever = scratch.write("G105-taforever.ini", kIni);
    const auto file = [&scratch](const char* name, std::string_view lines) {
        return scratch.write(name, "[TotalA]\n" + std::string(lines));
    };
    const auto plain = parse({"-c", taforever.c_str()});
    expect(
        plain.net.launch_planned && plain.net.direct_game.kind == Kind::host &&
            plain.launch.block.hosting == 1 &&
            plain.launch.block.connection_type == nl::connection_type::tcpip,
        "TA Forever's file hosts over TCP/IP"
    );
    const auto host = parse(
        {"-c", file("G105-host.ini", "connect=tcpip\ncreator=1\naddress=127.0.0.1\n").c_str()}
    );
    expect(
        host.net.direct_game.kind == Kind::host && host.launch.block.hosting == 1,
        "connect, creator and address host at once"
    );
    const auto join =
        parse({"-c", file("G105-join.ini", "connect=1\ncreator=0\naddress=192.0.2.10\n").c_str()});
    expect(
        join.net.direct_game.kind == Kind::join && join.net.direct_game.address == "192.0.2.10" &&
            join.launch.block.hosting == 0,
        "connect and address without creator join the game there"
    );
    const auto panel = parse({"-c", file("G105-panel.ini", "connect=1\ncreator=1\n").c_str()});
    expect(
        panel.net.launch_planned && panel.net.direct_game.kind == Kind::none &&
            panel.launch.block.connection_type == 1 && panel.launch.block.hosting == 1,
        "TCP/IP without an address leaves the address to the screens"
    );
    const auto menus = parse({"-c", file("G105-menus.ini", "creator=1\n").c_str()});
    expect(
        menus.net.launch_planned && menus.net.direct_game.kind == Kind::none &&
            menus.launch.block.connection_type == 0,
        "creator alone leaves the menus as they are"
    );
    const auto ipx =
        parse({"-c", file("G105-ipx.ini", "connect=ipx\ncreator=1\naddress=a\n").c_str()});
    const auto& ipx_notes = oa::app::start_notes();
    expect(
        ipx.net.direct_game.kind == Kind::none && ipx.launch.block.connection_type == 2 &&
            std::any_of(
                ipx_notes.begin(),
                ipx_notes.end(),
                [](const std::string& note) {
                    return note.ends_with(
                        "G105-ipx.ini: connect names IPX, which this game does not have; the "
                        "multiplayer screens open on the provider list"
                    );
                }
            ),
        "a transport this game does not have opens the provider list, and is noted"
    );
    const auto with_name =
        parse({"--player-name", "Commander", "-c", file("G105-name.ini", "creator=1\n").c_str()});
    expect(
        with_name.net.launch_planned && same_text(with_name.launch.block.user_name, "Commander"),
        "--player-name names the player of a launch the screens choose for"
    );

    // -h and -n after -c decide as the file's keys would; before it they are lost.
    const auto named = parse({"-c", taforever.c_str(), "-hFriday"});
    expect(
        named.net.direct_game.kind == Kind::none && named.launch.block.hosting == 1 &&
            same_text(named.launch.block.session_name, "Friday") &&
            named.launch.block.connection_type == 0,
        "-h after -c names the game and the host, and the menus choose the transport"
    );
    const auto joined = parse({"-c", taforever.c_str(), "-n1:192.0.2.20"});
    expect(
        joined.net.direct_game.kind == Kind::join && joined.net.direct_game.address == "192.0.2.20",
        "-n1:ADDRESS after -c joins the game there"
    );
    const auto hosted = parse({"-c", taforever.c_str(), "-n1:192.0.2.20", "-hFriday"});
    expect(
        hosted.net.direct_game.kind == Kind::host && hosted.net.direct_game.game_name == "Friday",
        "-n1:ADDRESS and -h after -c host"
    );
    const auto lost = parse({"-n1:192.0.2.20", "-hEarly", "-c", taforever.c_str()});
    const auto& lost_notes = oa::app::start_notes();
    expect(
        lost.net.direct_game.kind == Kind::host &&
            same_text(lost.launch.block.session_name, "Example game") &&
            std::find(
                lost_notes.begin(),
                lost_notes.end(),
                "-c clears the launch, as in 3.1c: what -h, -n set before it is lost"
            ) != lost_notes.end(),
        "-h and -n before -c are lost, and noted"
    );
    // The long options win whatever their order.
    const std::string join_file =
        file("G105-join-long.ini", "connect=1\ncreator=0\naddress=192.0.2.10\nsession=Ini\n");
    const auto long_host = parse({"--host", "-c", join_file.c_str()});
    expect(long_host.net.direct_game.kind == Kind::host, "--host wins over the file's join");
    const auto long_join = parse(
        {"-c",
         file("G105-host-long.ini", "connect=1\ncreator=1\naddress=a\n").c_str(),
         "--join",
         "192.0.2.30"}
    );
    expect(
        long_join.net.direct_game.kind == Kind::join &&
            long_join.net.direct_game.address == "192.0.2.30",
        "--join wins over the file's host"
    );
    const auto long_name = parse({"--game-name", "Long", "-c", join_file.c_str(), "-hShort"});
    expect(
        long_name.net.direct_game.game_name == "Long" &&
            same_text(long_name.launch.block.session_name, "Long"),
        "--game-name wins over -h and the file"
    );
    // pfs sets the send rate in place of -p, whichever comes first.
    const std::string paced = file("G105-pfs.ini", "pfs=12\n");
    const auto pfs_after = parse({"-p5", "-c", paced.c_str()});
    const auto pfs_before = parse({"-c", paced.c_str(), "-p5"});
    expect(
        pfs_after.launch.send_pacing == 12 && pfs_after.launch.send_pacing_set == 1 &&
            pfs_before.launch.send_pacing == 12,
        "pfs replaces -p"
    );
    const auto unpaced = parse({"-p5", "-c", file("G105-pfs0.ini", "pfs=0\n").c_str()});
    expect(unpaced.launch.send_pacing == 5, "pfs=0 leaves -p's rate");
    // A refusal of the map names the key.
    expect(
        plain.net.launch_replaced.end() !=
            std::find_if(
                plain.net.launch_replaced.begin(),
                plain.net.launch_replaced.end(),
                [](const auto& entry) {
                    return entry.first == "map" &&
                           entry.second.ends_with("G105-taforever.ini: mission");
                }
            ),
        "a refusal of the INI's map names mission"
    );
    const auto map_option = parse({"-c", taforever.c_str(), "--map", "Coast To Coast"});
    expect(
        same_text(map_option.launch.block.mission, "Coast To Coast") &&
            std::any_of(
                map_option.net.launch_replaced.begin(),
                map_option.net.launch_replaced.end(),
                [](const auto& entry) { return entry.first == "map" && entry.second == "--map"; }
            ),
        "--map replaces mission, and a refusal names --map"
    );
}

/// "-b WORD" sets a battle room option of the launch, after -c, below the long options.
///
/// @param scratch the directory the files are written in
void check_battle_words(const Scratch& scratch) {
    const std::string taforever = scratch.write("G105-battle.ini", kIni);
    const auto after =
        parse({"-c", taforever.c_str(), "-b", "deathmatch", "-bpermlos", "-b", "lock"});
    expect(
        after.launch.block.death_option == 3 && after.launch.block.los == 3 &&
            after.launch.block.lock_options == 1,
        "-b after -c replaces the file's options"
    );
    const auto before = parse({"-b", "deathmatch", "-c", taforever.c_str()});
    expect(before.launch.block.death_option == 2, "-b before -c is lost");
    const auto option = parse({"-c", taforever.c_str(), "-b", "cheating", "--cheats", "off"});
    expect(option.launch.block.cheats == 1, "--cheats wins over -b cheating");
    const auto host = parse(
        {"--host",
         "-b",
         "watching",
         "-b",
         "fixedloc",
         "-b",
         "mapping",
         "-b",
         "circlos",
         "-b",
         "deathplays"}
    );
    expect(
        host.net.launch_planned && host.launch.block.watching == 2 &&
            host.launch.block.location == 1 && host.launch.block.mapping == 2 &&
            host.launch.block.los == 2 && host.launch.block.death_option == 1,
        "-b beside --host makes a launch"
    );
    const auto alone = parse({"-b", "deathends"});
    const auto& notes = oa::app::start_notes();
    expect(
        !alone.net.launch_planned && alone.app.launch.language[0] == '\0' &&
            std::any_of(
                notes.begin(),
                notes.end(),
                [](const std::string& note) {
                    return note.starts_with("-b names battle room options");
                }
            ),
        "-b with no launch does nothing and is noted"
    );
}

/// -c with no file, a missing file, and a name with no folder.
///
/// @param scratch the directory the files are written in
void check_c_file(const Scratch& scratch) {
    const auto alone = parse({"-c"});
    expect(
        !alone.net.launch_planned && alone.app.launch.skip_intro == 1 &&
            std::find(
                oa::app::start_notes().begin(),
                oa::app::start_notes().end(),
                "-c names no file: it skips the intro and starts no launch, as in 3.1c"
            ) != oa::app::start_notes().end(),
        "-c alone starts no launch, skips the intro and is noted"
    );
    const std::string launch = scratch.write("G105-c-launch.oalaunch", kLaunch);
    const auto beside = parse({"-c", "--launch", launch.c_str()});
    expect(
        beside.net.launch_planned && beside.launch.block.computer_count == 2,
        "-c with no file leaves --launch's file"
    );
    const std::string missing = (scratch.dir / "G105-missing.ini").string();
    expect(
        rejection({"-c", missing.c_str()}).ends_with("G105-missing.ini: the file: cannot be read"),
        "a file that cannot be read is refused, naming it"
    );
    // A bare name is looked for in the game folder after the current one.
    const fs::path game = scratch.dir / "G105-game";
    fs::create_directories(game);
    {
        std::ofstream output(game / "G105-bare.ini", std::ios::binary);
        output << "[TotalA]\nmetal=4000\n";
    }
    const std::string game_dir = oa::app::path_to_utf8(game);
    const auto bare = parse({"--game-dir", game_dir.c_str(), "-c", "G105-bare.ini"});
    expect(
        bare.net.launch_planned && bare.launch.block.metal == 4000 &&
            std::any_of(
                oa::app::start_notes().begin(),
                oa::app::start_notes().end(),
                [](const std::string& note) {
                    return note.starts_with("G105-bare.ini was read from the game folder");
                }
            ),
        "a bare name is read from the game folder, and noted"
    );
    expect(
        rejection({"--game-dir", game_dir.c_str(), "-c", "G105-not-there.ini"}) ==
            "G105-not-there.ini: the file: cannot be read from the current folder or the game "
            "folder",
        "a bare name in neither folder is refused"
    );
    expect(launch_file::names_bare_file("a.ini"), "a.ini is a bare name");
    expect(
        !launch_file::names_bare_file("dir/a.ini") && !launch_file::names_bare_file("dir\\a.ini") &&
            !launch_file::names_bare_file("C:a.ini"),
        "a name with a folder or a drive is not bare"
    );
}

/// -c reads a file of either kind whose path has a space in it, as -c, -C,
/// /c or /C, with the file apart or attached, and a word after it is the
/// language.
///
/// @param scratch the directory the files are written in
void check_spaced_paths(const Scratch& scratch) {
    const fs::path folder = scratch.dir / "G113 Launch Files";
    fs::create_directories(folder);
    const auto write = [&folder](const char* name, std::string_view text) {
        const fs::path file = folder / name;
        std::ofstream output(file, std::ios::binary);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        expect(static_cast<bool>(output), "the spaced scratch file is written");
        return oa::app::path_to_utf8(file);
    };
    const std::string launch = write("host game.oalaunch", kLaunch);
    const std::string ini = write("TA Forever.ini", kIni);
    const auto read = [](const std::vector<const char*>& arguments) -> std::optional<Parsed> {
        try {
            return parse(arguments);
        } catch (const std::runtime_error& error) {
            std::fprintf(stderr, "refused: %s\n", error.what());
            return std::nullopt;
        }
    };
    const auto read_launch = [&read](const std::vector<const char*>& arguments) {
        const auto taken = read(arguments);
        return taken && taken->net.launch_planned && taken->launch.block.computer_count == 2 &&
               same_text(taken->launch.block.provider_label, "Ladder") &&
               taken->app.launch.language[0] == '\0';
    };
    const auto read_ini = [&read](const std::vector<const char*>& arguments) {
        const auto taken = read(arguments);
        return taken && taken->net.launch_planned && taken->launch.block.watching == 2 &&
               same_text(taken->launch.block.provider_label, "TAForever") &&
               taken->app.launch.language[0] == '\0';
    };
    for (const char* letter : {"-c", "-C", "/c", "/C"}) {
        const std::string attached_launch = letter + launch;
        const std::string attached_ini = letter + ini;
        expect(
            read_launch({letter, launch.c_str()}) && read_launch({attached_launch.c_str()}),
            std::string(letter) + " reads an .oalaunch file whose path has a space in it"
        );
        expect(
            read_ini({letter, ini.c_str()}) && read_ini({attached_ini.c_str()}),
            std::string(letter) + " reads TA Forever's file whose path has a space in it"
        );
    }
    const auto with_language = read({"-c", ini.c_str(), "german", "-b", "permlos"});
    expect(
        with_language && with_language->launch.block.watching == 2 &&
            std::string_view(with_language->app.launch.language) == "german" &&
            with_language->launch.block.los == 3,
        "a word after the spaced file is the language, and -b after it still applies"
    );
}

/// The engine's switches with no effect here are noted, and the notes go to the log once.
void check_switch_notes() {
    (void)parse({"-f", "-l", "-w", "-memfussy", "-gonzo", "-y"});
    const auto& notes = oa::app::start_notes();
    const auto noted = [&notes](std::string_view start) {
        return std::any_of(notes.begin(), notes.end(), [start](const std::string& note) {
            return note.starts_with(start);
        });
    };
    expect(noted("-f looks for 3.1c's disc"), "-f is noted");
    expect(noted("-l has no effect"), "-l is noted");
    expect(noted("-w plays 3.1c's sounds"), "-w is noted");
    expect(
        noted("-memfussy and 1 more are 3.1c's developer switches"), "developer switches are noted"
    );
    expect(noted("-y opens 3.1c's built-in internet lobby"), "-y is noted");
    expect(notes.size() == 5, "one note each");
    (void)parse({"-s", "-t60"});
    expect(oa::app::start_notes().empty(), "switches with their effect are not noted");

    // The frontend's start writes the notes and drops them.
    oa::app::NetgameContext context{};
    oa::app::Extension table{};
    oa::app::fill_option_hooks(table, context);
    oa::app::keep_start_note("G105 note");
    oa::ui::frontend_state::StateHandler handler{};
    table.frontend_states(table.context, handler);
    expect(oa::app::start_notes().empty(), "the notes are written once");
}

/// The .oalaunch example fills every field of the plan.
void check_launch() {
    const auto plan = launch_file::read_launch_text("launch.oalaunch", kLaunch);
    const auto& block = plan.block;
    expect(!plan.ini && plan.role == launch_file::LaunchRole::host, "the example hosts");
    expect(plan.source == "launch.oalaunch", "the refusal names the file it was given");
    expect(block.hosting == 1, "a host creates the game");
    expect(block.connection_type == nl::connection_type::tcpip, "a host plays over TCP/IP");
    expect(
        plan.game_name == "Example game" && same_text(block.session_name, "Example game"),
        "game.name"
    );
    expect(plan.password == "secret" && same_text(block.password, "secret"), "game.password");
    expect(block.player_limit == 6, "game.player-limit");
    expect(same_text(block.provider_label, "Ladder"), "game.launcher-name");
    expect(block.tournament == 0, "game.tournament");
    expect(block.closed == 0, "game.closed");
    std::array<uint8_t, 16> guid{};
    expect(
        oa::netgame::parse_application_guid(
            "{99797420-F5F5-11CF-9827-00A0241496C8}", guid.data()
        ) && plan.dplay_guid &&
            *plan.dplay_guid == guid,
        "game.directplay.application-guid"
    );
    expect(plan.dplay_port && *plan.dplay_port == 47624, "game.directplay.port");
    expect(
        plan.player_name == "Commander" && same_text(block.user_name, "Commander"), "player.name"
    );
    expect(block.faction == nl::faction::core && block.watcher == 0, "player.side");
    expect(block.team == 3, "player.team");
    expect(block.color == 5, "player.colour");
    expect(same_text(block.mission, "Acid Foursome"), "map");
    expect(block.death_option == 2, "rules.commander-death");
    expect(block.metal == 1200 && block.energy == 1500, "rules.metal and rules.energy");
    expect(block.max_units == 250, "rules.unit-limit");
    expect(block.los == 2, "rules.line-of-sight");
    expect(block.mapping == 1 && block.location == 1, "rules.mapping and rules.start-positions");
    expect(block.cheats == 2 && block.watching == 1, "rules.cheats and rules.watchers");
    expect(block.time_limit == 30, "rules.time-limit");
    expect(block.lock_options == 1, "rules.lock-options");
    expect(
        block.restriction_count == 3 && same_text(block.restrictions[0].unit, "CORKROG") &&
            block.restrictions[0].limit == 0 && same_text(block.restrictions[1].unit, "ARMFLASH") &&
            block.restrictions[1].limit == nl::kLaunchNoLimit &&
            same_text(block.restrictions[2].unit, "armmav") && block.restrictions[2].limit == 5,
        "rules.unit-restrictions"
    );
    expect(
        block.computer_count == 2 && block.computers[0].faction == nl::faction::core &&
            block.computers[0].team == 2 && block.computers[0].color == 7 &&
            block.computers[0].difficulty == nl::difficulty::medium &&
            block.computers[1].faction == nl::faction::arm && block.computers[1].team == 0 &&
            block.computers[1].color == 0 && block.computers[1].difficulty == nl::difficulty::none,
        "computer-players"
    );
    expect(
        plan.content.named && plan.content.base_game == "totala" &&
            plan.content.expansions.size() == 1 &&
            plan.content.expansions[0] == "core-contingency" && plan.content.mod == "ridge" &&
            plan.content.mutators_named && plan.content.mutators.size() == 2 &&
            plan.content.mutators[0].id == "wide-sight" &&
            plan.content.mutators[0].version.empty() &&
            plan.content.mutators[1].id == "fast-builders" &&
            plan.content.mutators[1].version == "1.0" &&
            plan.content.mutators[1].options.size() == 2 &&
            plan.content.mutators[1].options[0] ==
                std::pair<std::string, std::string>{"factor", "2"} &&
            plan.content.mutators[1].options[1] ==
                std::pair<std::string, std::string>{"rush", "true"},
        "content"
    );
    expect(
        plan.status.standard_output_named && plan.status.standard_output &&
            plan.status.file == "status.jsonl" &&
            plan.status.quit_after == launch_file::QuitAfter::after_score,
        "status"
    );
    // Every field of the table is one the examples name, so a new setting
    // needs a value here.
    for (const auto& field : launch_file::launch_fields()) {
        const auto last = field.field.rfind('.');
        const std::string key(field.field.substr(last == std::string_view::npos ? 0 : last + 1));
        expect(
            kLaunch.find(key + ":") != std::string_view::npos ||
                kJoin.find(key + ":") != std::string_view::npos,
            std::string(field.field) + " is in the examples"
        );
    }
    const auto joiner = launch_file::read_launch_text("join.oalaunch", kJoin);
    expect(
        joiner.role == launch_file::LaunchRole::join && joiner.block.hosting == 0 &&
            joiner.address == "192.0.2.10" && same_text(joiner.block.address, "192.0.2.10") &&
            joiner.block.watcher == 1,
        "a joiner's file names the address and may watch"
    );
}

/// The file and the options fill the same plan, and an option beside the file replaces its field.
void check_options_match_the_file(const std::string& launch, const std::string& plain) {
    const auto from_file = parse({"--launch", launch.c_str()});
    const auto from_options = parse(kLaunchOptions);
    expect(
        same_block(from_file.launch.block, from_options.launch.block),
        "the options fill the battle room's block as the file does"
    );
    expect(
        from_file.net.direct_game.game_name == from_options.net.direct_game.game_name &&
            from_file.net.direct_game.player_name == from_options.net.direct_game.player_name &&
            from_file.net.direct_game.password == from_options.net.direct_game.password &&
            from_file.net.dplay_port == from_options.net.dplay_port &&
            from_file.net.dplay_guid == from_options.net.dplay_guid &&
            from_file.net.direct_game.kind == from_options.net.direct_game.kind,
        "the options name the game as the file does"
    );
    expect(
        from_file.net.launch_planned && from_options.net.launch_planned &&
            from_options.app.skip_intro,
        "both plan a launch and skip the intro"
    );

    // Each of network play's launch options replaces the plain host's field,
    // before or after the file.
    struct Replacement {
        std::vector<const char*> option;
        bool (*holds)(const Parsed&);
    };

    const Replacement replacements[] = {
        {{"--player-limit", "8"}, [](const Parsed& p) { return p.launch.block.player_limit == 8; }},
        {{"--launcher-name", "Tourney"},
         [](const Parsed& p) { return same_text(p.launch.block.provider_label, "Tourney"); }},
        {{"--tournament"}, [](const Parsed& p) { return p.launch.block.tournament == 1; }},
        {{"--closed"}, [](const Parsed& p) { return p.launch.block.closed == 1; }},
        {{"--unit-restriction", "CORKROG=3"},
         [](const Parsed& p) {
             const auto& block = p.launch.block;
             return block.restriction_count == 1 &&
                    same_text(block.restrictions[0].unit, "CORKROG") &&
                    block.restrictions[0].limit == 3;
         }},
        {{"--side", "watcher"},
         [](const Parsed& p) {
             return p.launch.block.watcher == 1 && p.launch.block.faction == 0;
         }},
        {{"--team", "none"}, [](const Parsed& p) { return p.launch.block.team == 0; }},
        {{"--colour", "10"}, [](const Parsed& p) { return p.launch.block.color == 10; }},
        {{"--map", "Coast To Coast"},
         [](const Parsed& p) { return same_text(p.launch.block.mission, "Coast To Coast"); }},
        {{"--commander-death", "continues"},
         [](const Parsed& p) { return p.launch.block.death_option == 1; }},
        {{"--metal", "0"}, [](const Parsed& p) { return p.launch.block.metal == 0; }},
        {{"--energy", "10000"}, [](const Parsed& p) { return p.launch.block.energy == 10000; }},
        {{"--unit-limit", "6553"},
         [](const Parsed& p) { return p.launch.block.max_units == 6553; }},
        {{"--line-of-sight", "permanent"}, [](const Parsed& p) { return p.launch.block.los == 3; }},
        {{"--mapping", "mapped"}, [](const Parsed& p) { return p.launch.block.mapping == 2; }},
        {{"--start-positions", "random"},
         [](const Parsed& p) { return p.launch.block.location == 2; }},
        {{"--cheats", "off"}, [](const Parsed& p) { return p.launch.block.cheats == 1; }},
        {{"--watchers", "off"}, [](const Parsed& p) { return p.launch.block.watching == 1; }},
        {{"--time-limit", "45"}, [](const Parsed& p) { return p.launch.block.time_limit == 45; }},
        {{"--lock-options=on"}, [](const Parsed& p) { return p.launch.block.lock_options == 1; }},
        {{"--computer-player", "core:none:4:easy"}, [](const Parsed& p) {
             const auto& block = p.launch.block;
             return block.computer_count == 1 && block.computers[0].faction == nl::faction::core &&
                    block.computers[0].team == 0 && block.computers[0].color == 4 &&
                    block.computers[0].difficulty == nl::difficulty::easy;
         }},
    };
    std::set<std::string> replaced;
    for (const auto& replacement : replacements) {
        const std::string name(replacement.option.front());
        replaced.insert(name.substr(0, name.find('=')));
        const auto file_first = joined({"--launch", plain.c_str()}, replacement.option);
        const auto option_first = joined(replacement.option, {"--launch", plain.c_str()});
        expect(replacement.holds(parse(file_first)), name + " replaces the file's field");
        expect(
            replacement.holds(parse(option_first)), name + " replaces the field before the file"
        );
    }
    const auto restricted =
        parse({"--launch", launch.c_str(), "--unit-restriction", "corkrog=no-limit"}).launch.block;
    expect(
        restricted.restriction_count == 1 && restricted.restrictions[0].limit == nl::kLaunchNoLimit,
        "the first --unit-restriction replaces the file's restrictions"
    );
    const auto twice =
        parse({"--host", "--unit-restriction", "CORKROG=1", "--unit-restriction", "corkrog=2"})
            .launch.block;
    expect(
        twice.restriction_count == 1 && twice.restrictions[0].limit == 2,
        "a later restriction of the same unit replaces the earlier"
    );
    const auto listed =
        parse({"--launch", launch.c_str(), "--computer-player", "arm"}).launch.block;
    expect(
        listed.computer_count == 1 && listed.computers[0].faction == nl::faction::arm &&
            listed.computers[0].difficulty == nl::difficulty::none,
        "the first --computer-player replaces the file's list"
    );
    expect(
        parse({"--launch", plain.c_str(), "--computer-player", "arm", "--computer-player", "core"})
                .launch.block.computer_count == 2,
        "each --computer-player adds one"
    );
    for (const auto& field : launch_file::launch_fields())
        if (field.reader == launch_file::LaunchOptionReader::network_play) {
            const std::string option(field.option.substr(0, field.option.find_first_of(" [")));
            expect(replaced.contains(option), option + " has a replacement case");
        }
}

/// The fifth part and the group's brain are read, an empty part is the built-in
/// player, a bad id is refused at its place, and a sixth part is refused.
void check_computer_brains() {
    const auto named =
        parse({"--host", "--computer-player", "core:2::hard:steady-commander"}).launch.block;
    expect(
        named.computer_count == 1 && named.computers[0].faction == nl::faction::core &&
            named.computers[0].team == 2 && named.computers[0].color == 0 &&
            named.computers[0].difficulty == nl::difficulty::hard &&
            same_text(named.computers[0].brain, "steady-commander"),
        "the fifth part names the brain"
    );
    const auto empty_tail = parse({"--host", "--computer-player", "core:2::hard:"}).launch.block;
    expect(
        empty_tail.computer_count == 1 &&
            empty_tail.computers[0].difficulty == nl::difficulty::hard &&
            empty_tail.computers[0].brain[0] == '\0',
        "an empty fifth part seats the built-in player"
    );
    const auto four = parse({"--host", "--computer-player", "core:none:4:easy"}).launch.block;
    expect(
        four.computer_count == 1 && four.computers[0].brain[0] == '\0' &&
            four.computers[0].team == 0 && four.computers[0].difficulty == nl::difficulty::easy,
        "four parts leave the brain empty"
    );
    const std::string bad_option =
        rejection({"--host", "--computer-player", "core:1::hard:Steady"});
    expect(
        bad_option == "--computer-player brain: is not a kebab-case brain id",
        "a bad id names --computer-player brain: " + bad_option
    );
    const std::string too_many =
        rejection({"--host", "--computer-player", "core:1:2:hard:steady-commander:extra"});
    expect(
        too_many == "--computer-player: expected SIDE[:TEAM[:COLOUR[:DIFFICULTY[:BRAIN]]]]",
        "more than five parts are refused: " + too_many
    );
    const std::string file = refused_text(
        "oalaunch: 1\n"
        "game:\n"
        "  role: host\n"
        "computer-players:\n"
        "  - side: arm\n"
        "  - side: core\n"
        "    team: 2\n"
        "    difficulty: hard\n"
        "    brain: steady-commander\n"
    );
    expect(file.empty(), "a group's brain is read: " + file);
    const auto grouped = launch_file::read_launch_text(
        "launch.oalaunch",
        "oalaunch: 1\n"
        "game:\n"
        "  role: host\n"
        "computer-players:\n"
        "  - side: arm\n"
        "    brain: \"\"\n"
        "  - side: core\n"
        "    team: 2\n"
        "    difficulty: hard\n"
        "    brain: steady-commander\n"
    );
    expect(
        grouped.block.computer_count == 2 && grouped.block.computers[0].brain[0] == '\0' &&
            same_text(grouped.block.computers[1].brain, "steady-commander") &&
            grouped.block.computers[1].difficulty == nl::difficulty::hard,
        "the group's brain is stored, and an empty one is the built-in player"
    );
    const std::string bad_file = refused_text(
        "oalaunch: 1\n"
        "game:\n"
        "  role: host\n"
        "computer-players:\n"
        "  - side: arm\n"
        "  - side: core\n"
        "    brain: Nope\n"
    );
    expect(
        bad_file == "launch.oalaunch: computer-players[2].brain: is not a kebab-case brain id",
        "a bad id names computer-players[2].brain: " + bad_file
    );
}

/// The names and the password a launch takes are as long as the game keeps
/// them, one limit for each: the file's field and its option refuse one
/// character more, with or without a launch beside the option, the manual's
/// table and --help say that limit, and TA Forever's file keeps that many.
void check_name_limits() {
    struct Limited {
        const char* field;
        const char* option;
        const char* key;
        std::size_t limit;
    };

    const Limited limited[] = {
        {"player.name", "--player-name", "player", launch_file::kPlayerNameLimit},
        {"game.name", "--game-name", "session", launch_file::kGameNameLimit},
        {"game.password", "--game-password", "password", launch_file::kPasswordLimit},
    };
    expect(
        launch_file::kPlayerNameLimit == 16 && launch_file::kGameNameLimit == 16 &&
            launch_file::kPasswordLimit == 10,
        "a name keeps 16 characters and a password 10, as the battle room does"
    );
    for (const auto& one : limited) {
        const std::string longest(one.limit, 'n');
        const std::string longer(one.limit + 1, 'n');
        const std::string why = "is longer than " + std::to_string(one.limit) + " characters";
        const std::string field(one.field);
        const std::string group = field.substr(0, field.find('.'));
        const std::string key = field.substr(field.find('.') + 1);
        const auto file = [&](const std::string& value) {
            const std::string head = "oalaunch: 1\ngame:\n  role: host\n";
            const std::string text = group == "game"
                                         ? head + "  " + key + ": " + value + "\n"
                                         : head + group + ":\n  " + key + ": " + value + "\n";
            return refused_text(text);
        };
        expect(
            file(longest).empty(), field + " takes " + std::to_string(one.limit) + " characters"
        );
        expect(
            file(longer) == "launch.oalaunch: " + field + ": " + why,
            field + " refuses one more: " + file(longer)
        );
        const std::string option(one.option);
        expect(
            rejection({"--host", one.option, longest.c_str()}).empty(),
            option + " takes " + std::to_string(one.limit) + " characters"
        );
        expect(
            rejection({"--host", one.option, longer.c_str()}) == option + ": " + why,
            option + " refuses one more without a launch: " +
                rejection({"--host", one.option, longer.c_str()})
        );
        expect(
            rejection({"--host", "--metal", "1000", one.option, longer.c_str()}) ==
                option + ": " + why,
            option + " refuses one more beside a launch"
        );
        const launch_file::LaunchField* setting = launch_file::find_launch_field(field);
        const std::string said = "at most " + std::to_string(one.limit) + " characters";
        expect(
            setting != nullptr && setting->meaning.find(said) != std::string_view::npos,
            field + "'s row says " + said
        );
        const std::string kept = "its first " + std::to_string(one.limit) + " characters kept";
        bool row_found = false;
        for (const auto& row : launch_file::launch_ini_keys())
            if (row.key == one.key) {
                row_found = true;
                expect(
                    row.meaning.find(kept) != std::string_view::npos,
                    std::string(one.key) + "'s row says " + kept
                );
            }
        expect(row_found, std::string(one.key) + " is a key of the -c file");
    }
}

/// A side is a faction, a number from 0, or a SIDEDATA name.
void check_side_values() {
    const auto numbered = launch_file::read_launch_text(
        "launch.oalaunch", "oalaunch: 1\ngame:\n  role: host\nplayer:\n  side: 6\n"
    );
    expect(
        numbered.block.faction == 7 && numbered.block.watcher == 0 &&
            numbered.block.faction_name[0] == '\0',
        "side 6 is faction 7"
    );
    const auto named = launch_file::read_launch_text(
        "launch.oalaunch", "oalaunch: 1\ngame:\n  role: host\nplayer:\n  side: Ridge6\n"
    );
    expect(
        named.block.faction == nl::faction::none && named.block.watcher == 0 &&
            same_text(named.block.faction_name, "Ridge6"),
        "Ridge6 is the side's name"
    );
    const auto by_option = parse({"--host", "--side", "6"});
    expect(
        by_option.launch.block.faction == 7 && by_option.launch.block.faction_name[0] == '\0',
        "--side 6 is faction 7"
    );
    const auto computer = parse({"--host", "--computer-player", "Ridge6:2"});
    expect(
        computer.launch.block.computer_count == 1 &&
            computer.launch.block.computers[0].faction == nl::faction::none &&
            computer.launch.block.computers[0].team == 2 &&
            same_text(computer.launch.block.computers[0].faction_name, "Ridge6"),
        "--computer-player Ridge6:2 names the side and the team"
    );
    const auto arm = launch_file::read_launch_text(
        "launch.oalaunch", "oalaunch: 1\ngame:\n  role: host\nplayer:\n  side: arm\n"
    );
    expect(
        arm.block.faction == nl::faction::arm && arm.block.watcher == 0 &&
            arm.block.faction_name[0] == '\0',
        "arm is faction 1"
    );
    const auto core = launch_file::read_launch_text(
        "launch.oalaunch", "oalaunch: 1\ngame:\n  role: host\nplayer:\n  side: core\n"
    );
    expect(
        core.block.faction == nl::faction::core && core.block.watcher == 0 &&
            core.block.faction_name[0] == '\0',
        "core is faction 2"
    );
    const auto watcher = launch_file::read_launch_text(
        "launch.oalaunch",
        "oalaunch: 1\ngame:\n  role: host\nrules:\n  watchers: on\nplayer:\n  side: watcher\n"
    );
    expect(
        watcher.block.faction == nl::faction::none && watcher.block.watcher == 1 &&
            watcher.block.faction_name[0] == '\0',
        "watcher watches"
    );
    const std::string empty =
        refused_text("oalaunch: 1\ngame:\n  role: host\nplayer:\n  side: \"\"\n");
    expect(
        empty.find("expected arm, core, watcher, a side's number from 0 to 31 or its name") !=
            std::string::npos,
        "an empty side is refused: " + empty
    );
}

/// Each refusal names the field in the file, or the option, and why.
///
/// @param scratch the directory the files are written in
void check_refusals(const Scratch& scratch) {
    struct Refused {
        const char* option;
        const char* value; // null for an option that takes none
        const char* file;  // the same value in the file: a dotted field, then its YAML
        const char* why;
    };

    const Refused refused[] = {
        {"--player-limit", "11", "game:\n  player-limit: 11\n", "11 is above 10"},
        {"--launcher-name",
         "SixteenLettersXX",
         "game:\n  launcher-name: SixteenLettersXX\n",
         "is longer than 15 characters"},
        {"--tournament=maybe", nullptr, "game:\n  tournament: maybe\n", "expected on or off"},
        {"--closed=maybe", nullptr, "game:\n  closed: maybe\n", "expected on or off"},
        {"--unit-restriction",
         "CORKROG=101",
         "rules:\n  unit-restrictions:\n    CORKROG: 101\n",
         "101 is above 100"},
        {"--side",
         "32",
         "player:\n  side: 32\n",
         "expected arm, core, watcher, a side's number from 0 to 31 or its name"},
        {"--side",
         "012345678901234567890123456789",
         "player:\n  side: \"012345678901234567890123456789\"\n",
         "expected arm, core, watcher, a side's number from 0 to 31 or its name"},
        {"--team", "6", "player:\n  team: 6\n", "expected 1 to 5 or none"},
        {"--colour", "11", "player:\n  colour: 11\n", "11 is above 10"},
        {"--map",
         "A map whose name is far longer than the sixty-three characters a launch keeps",
         "map: A map whose name is far longer than the sixty-three characters a launch keeps\n",
         "is longer than 63 characters"},
        {"--commander-death",
         "sometimes",
         "rules:\n  commander-death: sometimes\n",
         "expected continues, ends or deathmatch"},
        {"--metal", "10001", "rules:\n  metal: 10001\n", "10001 is above 10000"},
        {"--energy", "-1", "rules:\n  energy: -1\n", "-1 is below 0"},
        {"--unit-limit", "19", "rules:\n  unit-limit: 19\n", "19 is below 20"},
        {"--line-of-sight",
         "false",
         "rules:\n  line-of-sight: false\n",
         "expected true, circular or permanent"},
        {"--mapping", "half", "rules:\n  mapping: half\n", "expected mapped or unmapped"},
        {"--start-positions",
         "near",
         "rules:\n  start-positions: near\n",
         "expected fixed or random"},
        {"--cheats", "yes", "rules:\n  cheats: yes\n", "expected on or off"},
        {"--watchers", "1", "rules:\n  watchers: 1\n", "expected on or off"},
        {"--time-limit", "-5", "rules:\n  time-limit: -5\n", "-5 is below 0"},
        {"--lock-options=sometimes",
         nullptr,
         "rules:\n  lock-options: sometimes\n",
         "expected on or off"},
        {"--computer-player",
         "32",
         "computer-players:\n  - side: 32\n",
         "expected arm, core, a side's number from 0 to 31 or its name"},
        {"--computer-player",
         "012345678901234567890123456789",
         "computer-players:\n  - side: \"012345678901234567890123456789\"\n",
         "expected arm, core, a side's number from 0 to 31 or its name"},
    };
    std::set<std::string> covered;
    for (const auto& case_of : refused) {
        const std::string option(case_of.option);
        const std::string name = option.substr(0, option.find('='));
        covered.insert(name);
        std::vector<const char*> arguments{"--host", case_of.option};
        if (case_of.value != nullptr)
            arguments.push_back(case_of.value);
        const std::string by_option = rejection(arguments);
        expect(
            by_option.starts_with(name) && by_option.find(case_of.why) != std::string::npos,
            name + " is refused naming the option: " + by_option
        );
        const std::string text = std::string("oalaunch: 1\ngame:\n  role: host\n") + case_of.file;
        // A group the file names twice is refused for that; join it to the host's.
        std::string merged = text;
        if (std::string_view(case_of.file).starts_with("game:\n"))
            merged = std::string("oalaunch: 1\ngame:\n  role: host\n") +
                     std::string(case_of.file).substr(std::strlen("game:\n"));
        const std::string by_file = refused_text(merged);
        expect(
            by_file.starts_with("launch.oalaunch: ") &&
                by_file.find(case_of.why) != std::string::npos,
            name + " is refused in the file naming the field: " + by_file
        );
    }
    for (const auto& field : launch_file::launch_fields())
        if (field.reader == launch_file::LaunchOptionReader::network_play) {
            const std::string option(field.option.substr(0, field.option.find_first_of(" [")));
            expect(covered.contains(option), option + " has a refusal case");
        }

    const std::string unknown = refused_text("oalaunch: 1\nrules:\n  nope: 1\n");
    expect(
        unknown == "launch.oalaunch: rules.nope: is not a launch setting",
        "an unknown key names its field: " + unknown
    );
    const std::string legacy = refused_text("oalaunch: 1\nrules:\n  los: 1\n");
    expect(
        legacy == "launch.oalaunch: rules.los: is not a launch setting",
        "a legacy name is not a field: " + legacy
    );
    const std::string metal =
        refused_text("oalaunch: 1\ngame:\n  password: secret\nrules:\n  metal: 250000\n");
    expect(
        metal == "launch.oalaunch: rules.metal: 250000 is above 10000",
        "metal above the slider names the field and not the password: " + metal
    );
    expect(
        refused_text("oalaunch: 1\nrules:\n  metal: lots\n") ==
            "launch.oalaunch: rules.metal: expected a whole number",
        "a wrong type names the field"
    );
    expect(refused_text("oalaunch: 2\n") == "launch.oalaunch: oalaunch: expected 1", "oalaunch: 1");
    constexpr std::string_view password = "G104-password-that-must-stay-out";
    const std::string long_password =
        refused_text("oalaunch: 1\ngame:\n  password: G104-password-that-must-stay-out\n");
    expect(
        long_password == "launch.oalaunch: game.password: is longer than 10 characters" &&
            long_password.find(password) == std::string::npos,
        "a long password is refused without being repeated"
    );
    const std::string option_password =
        rejection({"--host", "--metal", "1000", "--game-password", password.data()});
    expect(
        option_password == "--game-password: is longer than 10 characters",
        "a long --game-password beside a launch names the option: " + option_password
    );
    expect(
        refused_text("oalaunch: 1\ncontent:\n  expansions: [core-contingency]\n") ==
            "launch.oalaunch: content.base-game: expected a base game",
        "expansions need a base game, wherever the file writes them"
    );

    // The plan is checked as a whole, naming the field or the option.
    const std::string deathmatch = refused_text(
        "oalaunch: 1\nrules:\n  commander-death: deathmatch\ncomputer-players:\n  - side: arm\n"
    );
    std::string checked;
    try {
        launch_file::check_launch_plan(
            launch_file::read_launch_text(
                "launch.oalaunch",
                "oalaunch: 1\nrules:\n  commander-death: deathmatch\ncomputer-players:\n  - side: "
                "arm\n"
            )
        );
    } catch (const launch_file::LaunchRefusal& error) {
        checked = error.what();
    }
    expect(
        deathmatch.empty() &&
            checked.starts_with("launch.oalaunch: computer-players: a deathmatch"),
        "a deathmatch seats no computer players: " + checked
    );
    const std::string tournament =
        rejection({"--host", "--tournament", "--computer-player", "arm"});
    expect(
        tournament.starts_with("--computer-player: a tournament game"),
        "a tournament seats no computer players: " + tournament
    );
    const std::string crowded = rejection(
        {"--host", "--player-limit", "2", "--computer-player", "arm", "--computer-player", "core"}
    );
    expect(
        crowded.starts_with("--computer-player: 2 computer players and the player are more"),
        "the computer players fit the player limit: " + crowded
    );
    const std::string watching = rejection({"--host", "--watchers", "off", "--side", "watcher"});
    expect(
        watching == "--side: a host who watches needs rules.watchers on",
        "a host who watches allows watchers: " + watching
    );
    const std::string roleless = rejection({"--metal", "2000"});
    expect(roleless == "--metal needs --host or --join", "options alone need a role: " + roleless);
    const std::string address = rejection(
        {"--launch", scratch.write("join.oalaunch", "oalaunch: 1\ngame:\n  role: join\n").c_str()}
    );
    expect(
        address.ends_with(
            "join.oalaunch: game.address: a player who joins needs the game's address"
        ),
        "a join needs an address: " + address
    );
    const std::string unknown_option = rejection({"--host", "--metal=5"});
    expect(
        unknown_option == "unknown option: --metal=5",
        "only --tournament and --lock-options take =: " + unknown_option
    );
}

/// -c and --launch read either kind of file by what it holds, whatever its name.
///
/// @param scratch the directory the files are written in
void check_either_file(const Scratch& scratch) {
    const std::string launch_named_ini = scratch.write("launch.ini", kLaunch);
    const std::string ini_named_launch = scratch.write("totala.oalaunch", kIni);
    const std::string neither = scratch.write("notes.txt", "hello\n");
    expect(
        launch_file::launch_file_kind(kLaunch) == launch_file::LaunchFileKind::open_annihilation &&
            launch_file::launch_file_kind(kIni) == launch_file::LaunchFileKind::ta_forever &&
            launch_file::launch_file_kind("# a comment\n\noalaunch: 1\n") ==
                launch_file::LaunchFileKind::open_annihilation &&
            launch_file::launch_file_kind("\xef\xbb\xbfoalaunch: 1\n") ==
                launch_file::LaunchFileKind::open_annihilation &&
            launch_file::launch_file_kind("; TA Forever\n[TotalA]\nmetal=1\n") ==
                launch_file::LaunchFileKind::ta_forever &&
            launch_file::launch_file_kind("game:\n  role: host\noalaunch: 1\n") ==
                launch_file::LaunchFileKind::neither &&
            launch_file::launch_file_kind("hello\n") == launch_file::LaunchFileKind::neither,
        "a launch file is told by what it holds"
    );
    const auto c_launch = parse({"-c", launch_named_ini.c_str()});
    const auto option_ini = parse({"--launch", ini_named_launch.c_str()});
    expect(
        c_launch.net.launch_planned && c_launch.launch.block.computer_count == 2 &&
            same_text(c_launch.launch.block.provider_label, "Ladder"),
        "-c reads an .oalaunch file, whatever its name"
    );
    expect(
        option_ini.net.launch_planned && option_ini.launch.block.watching == 2 &&
            same_text(option_ini.launch.block.provider_label, "TAForever"),
        "--launch reads TA Forever's INI, whatever its name"
    );
    const std::string refused = rejection({"-c", neither.c_str()});
    expect(
        refused.find("notes.txt: the file: is neither a launch file") != std::string::npos,
        "a file that is neither is refused: " + refused
    );
    expect(
        rejection({"-c", ini_named_launch.c_str(), "--launch", launch_named_ini.c_str()}) ==
            "-c and --launch cannot both be given",
        "-c and --launch cannot both be given"
    );
    expect(rejection({"-c"}).empty(), "-c without a file is not refused");

    // The options the direct game took before launches keep their place.
    const std::string ini = scratch.write("taforever.ini", kIni);
    const auto join_first = parse({"--join", "192.0.2.10", "-c", ini.c_str()});
    const auto file_join = parse({"-c", ini.c_str(), "--join", "192.0.2.10"});
    const auto is_join = [](const Parsed& taken) {
        return taken.net.direct_game.kind == mp::DirectGame::Kind::join &&
               taken.launch.block.hosting == 0 && taken.net.direct_game.address == "192.0.2.10" &&
               same_text(taken.launch.block.address, "192.0.2.10");
    };
    expect(is_join(join_first) && is_join(file_join), "--join replaces -c's host");
    const auto renamed = parse({"--game-name", "Override", "-c", launch_named_ini.c_str()});
    expect(
        same_text(renamed.launch.block.session_name, "Override") &&
            renamed.net.direct_game.game_name == "Override",
        "--game-name replaces the file's name"
    );
    expect(
        rejection({"--list-content"}) == "--list-content needs --json" &&
            rejection({"--json"}) == "--json needs --list-content",
        "--list-content and --json go together"
    );
}

/// The status block is kept for the status stream, from any launch.
///
/// @param scratch the directory the files are written in
void check_kept_status(const Scratch& scratch) {
    const std::string launch = scratch.write("status.oalaunch", kLaunch);
    (void)parse({"-c", launch.c_str()});
    const auto& kept = launch_file::kept_launch_status();
    expect(
        kept.standard_output_named && kept.standard_output && kept.file == "status.jsonl" &&
            kept.quit_after == launch_file::QuitAfter::after_score,
        "the launch's status block is kept for the status stream"
    );
    (void)parse({"--host"});
    expect(
        !launch_file::kept_launch_status().standard_output_named &&
            launch_file::kept_launch_status().file.empty(),
        "a run without a launch keeps no status block"
    );
}

#if defined(_WIN32)
using Writer = HANDLE;

/// Makes a pipe: the reading end as an input handle and the writing end.
///
/// @param[out] reader the end the launch is read from
/// @param[out] writer the end the test writes
/// @return true when the pipe was made
bool make_pipe(launch_file::InputHandle& reader, Writer& writer) {
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (CreatePipe(&read_end, &write_end, nullptr, 0) == FALSE)
        return false;
    reader = read_end;
    writer = write_end;
    return true;
}

/// Writes all of a text to the pipe's writing end.
///
/// @param writer the writing end
/// @param text the bytes
void write_all(Writer writer, std::string_view text) {
    DWORD written = 0;
    expect(
        WriteFile(writer, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) !=
                FALSE &&
            written == text.size(),
        "the pipe takes the text"
    );
}

/// Closes a writing end.
///
/// @param writer the end
void close_writer(Writer writer) {
    CloseHandle(writer);
}

/// Closes a reading end.
///
/// @param reader the end
void close_reader(launch_file::InputHandle reader) {
    CloseHandle(static_cast<HANDLE>(reader));
}

/// Opens a file to read as an input.
///
/// @param path the file
/// @return the input, or an invalid handle
launch_file::InputHandle open_input(const std::string& path) {
    const std::wstring wide = fs::path(oa::app::path_from_utf8(path)).wstring();
    return CreateFileW(
        wide.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
}
#else
using Writer = int;

bool make_pipe(launch_file::InputHandle& reader, Writer& writer) {
    int ends[2] = {-1, -1};
    if (::pipe(ends) != 0)
        return false;
    reader = ends[0];
    writer = ends[1];
    return true;
}

void write_all(Writer writer, std::string_view text) {
    expect(
        ::write(writer, text.data(), text.size()) == static_cast<ssize_t>(text.size()),
        "the pipe takes the text"
    );
}

void close_writer(Writer writer) {
    ::close(writer);
}

void close_reader(launch_file::InputHandle reader) {
    ::close(reader);
}

launch_file::InputHandle open_input(const std::string& path) {
    return ::open(path.c_str(), O_RDONLY);
}
#endif

/// What a look at a pipe found, and how long it took.
struct Looked {
    launch_file::PipedLaunch piped{};
    std::string refusal{};
    std::chrono::milliseconds took{};
};

/// Looks at a pipe holding a text, its writing end left open or closed.
///
/// @param text the bytes written first
/// @param close_after true to close the writing end, as a finished pipe is
/// @param first_wait how long to wait for the first byte
/// @param rest_wait how long the rest may take
/// @return what was found
Looked look_at_pipe(
    std::string_view text,
    bool close_after,
    std::chrono::milliseconds first_wait,
    std::chrono::milliseconds rest_wait
) {
    Looked looked;
    launch_file::InputHandle reader{};
    Writer writer{};
    if (!make_pipe(reader, writer)) {
        expect(false, "a pipe is made");
        return looked;
    }
    if (!text.empty())
        write_all(writer, text);
    if (close_after)
        close_writer(writer);
    const auto started = std::chrono::steady_clock::now();
    try {
        looked.piped = launch_file::read_piped_launch(reader, first_wait, rest_wait);
    } catch (const launch_file::LaunchRefusal& error) {
        looked.refusal = error.what();
    }
    looked.took = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started
    );
    if (!close_after)
        close_writer(writer);
    close_reader(reader);
    return looked;
}

/// A launch piped in is read; an idle pipe, another program's input and an
/// empty one are left after at most the wait.
///
/// @param scratch the directory the files are written in
void check_standard_input(const Scratch& scratch) {
    using std::chrono::milliseconds;
    // A margin for a busy machine: the waits are what the check is about,
    // not this machine's scheduling.
    const milliseconds margin{1500};
    const auto piped =
        look_at_pipe(kLaunch, true, launch_file::kPipedLaunchWait, milliseconds{2000});
    expect(
        piped.piped.found && piped.piped.text == kLaunch, "a launch piped in is read to its end"
    );
    const auto plan = launch_file::read_launch_text("standard input", piped.piped.text);
    expect(plan.block.computer_count == 2, "the piped launch is the file's");
    const auto marked = look_at_pipe(
        std::string("\xef\xbb\xbf") + std::string(kLaunch),
        true,
        milliseconds{250},
        milliseconds{2000}
    );
    expect(marked.piped.found, "a byte order mark before the header is read");

    const auto idle = look_at_pipe({}, false, launch_file::kPipedLaunchWait, milliseconds{2000});
    expect(!idle.piped.found && idle.refusal.empty(), "an idle pipe holds no launch");
    expect(
        idle.took >= launch_file::kPipedLaunchWait - milliseconds{20} &&
            idle.took < launch_file::kPipedLaunchWait + margin,
        "an idle pipe is left after the wait: " + std::to_string(idle.took.count()) + " ms"
    );
    const auto other =
        look_at_pipe("hello, other program\n", false, milliseconds{250}, milliseconds{2000});
    expect(
        !other.piped.found && other.refusal.empty() && other.took < margin,
        "another program's input is left at once"
    );
    const auto empty = look_at_pipe({}, true, milliseconds{250}, milliseconds{2000});
    expect(!empty.piped.found && empty.took < margin, "an empty pipe is left at once");
    const auto stalled = look_at_pipe("oalaunch: 1\n", false, milliseconds{250}, milliseconds{100});
    expect(
        !stalled.piped.found &&
            stalled.refusal == "standard input: the file: did not end within 100 milliseconds",
        "a launch that does not end is refused: " + stalled.refusal
    );

    const std::string file = scratch.write("piped.oalaunch", kLaunch);
    const auto input = open_input(file);
    const auto from_file =
        launch_file::read_piped_launch(input, milliseconds{250}, milliseconds{2000});
    close_reader(input);
    expect(from_file.found && from_file.text == kLaunch, "a file given as standard input is read");
    const std::string blank = scratch.write("blank.txt", "");
    const auto empty_input = open_input(blank);
    expect(
        !launch_file::read_piped_launch(empty_input, milliseconds{250}, milliseconds{2000}).found,
        "an empty file holds no launch"
    );
    close_reader(empty_input);
}

/// content.mutators keeps order, normalises options, and refuses a bad list.
void check_mutators() {
    const auto both = launch_file::read_launch_text(
        "launch.oalaunch",
        "oalaunch: 1\n"
        "content:\n"
        "  mutators:\n"
        "    - wide-sight\n"
        "    - id: fast-builders\n"
        "      version: \"1.0\"\n"
        "      options:\n"
        "        factor: 2\n"
        "        rush: on\n"
        "        shown: true\n"
        "        quiet: off\n"
        "        hidden: false\n"
    );
    expect(!both.content.named && both.content.mutators_named, "mutators do not name content");
    expect(both.content.mutators.size() == 2, "both forms are kept");
    expect(
        both.content.mutators[0].id == "wide-sight" && both.content.mutators[0].options.empty() &&
            both.content.mutators[1].id == "fast-builders" &&
            both.content.mutators[1].version == "1.0",
        "the order and the version are kept"
    );
    const auto& options = both.content.mutators[1].options;
    expect(
        options.size() == 5 && options[0].second == "2" && options[1].second == "true" &&
            options[2].second == "true" && options[3].second == "false" &&
            options[4].second == "false",
        "on, off, true and false are normalised"
    );
    const auto empty =
        launch_file::read_launch_text("launch.oalaunch", "oalaunch: 1\ncontent:\n  mutators: []\n");
    expect(
        empty.content.mutators_named && empty.content.mutators.empty() && !empty.content.named,
        "an empty list names no mutators"
    );
    expect(
        refused_text("oalaunch: 1\ncontent:\n  mutators: wide-sight\n") ==
            "launch.oalaunch: content.mutators: expected a list",
        "option text is not a list"
    );
    expect(
        refused_text("oalaunch: 1\ncontent:\n  mutators: [1]\n") ==
            "launch.oalaunch: content.mutators[1]: expected a mutator id or a group",
        "a number is not a mutator"
    );
    expect(
        refused_text("oalaunch: 1\ncontent:\n  mutators:\n  - version: \"1.0\"\n") ==
            "launch.oalaunch: content.mutators[1]: expected an id",
        "a group needs an id"
    );
    expect(
        refused_text(
            "oalaunch: 1\ncontent:\n  mutators:\n  - wide-sight\n  - id: fast-builders\n"
            "    colour: 2\n"
        ) == "launch.oalaunch: content.mutators[2].colour: is not a mutator setting",
        "another key is not a mutator setting"
    );
    expect(
        refused_text("oalaunch: 1\ncontent:\n  mutators: [Wide-Sight]\n") ==
            "launch.oalaunch: content.mutators[1]: expected a mutator id",
        "an id the manifest would refuse"
    );
    expect(
        refused_text(
            "oalaunch: 1\ncontent:\n  mutators:\n  - id: wide-sight\n    options:\n"
            "        Factor: 1\n"
        ) == "launch.oalaunch: content.mutators[1].options.Factor: expected an option id",
        "an option id the manifest would refuse"
    );
    expect(
        refused_text(
            "oalaunch: 1\ncontent:\n  mutators:\n  - wide-sight\n  - id: fast-builders\n"
            "    options: [factor]\n"
        ) == "launch.oalaunch: content.mutators[2].options: expected a group of options",
        "options are a group"
    );
    expect(
        refused_text(
            "oalaunch: 1\ncontent:\n  mutators:\n  - id: wide-sight\n    options:\n"
            "        factor: [2]\n"
        ) == "launch.oalaunch: content.mutators[1].options.factor: expected text, a number, on or "
             "off",
        "an option value is not a list"
    );
    expect(
        refused_text(
            "oalaunch: 1\ncontent:\n  mutators:\n  - id: wide-sight\n    options:\n"
            "        health: \"1000,extra=true\"\n"
        ) == "launch.oalaunch: content.mutators[1].options.health: expected a value of letters, "
             "digits, _, - or .",
        "a value cannot carry another option"
    );
    expect(
        refused_text(
            "oalaunch: 1\ncontent:\n  mutators:\n  - id: wide-sight\n    options:\n"
            "        health: \"1000,health=3000\"\n"
        ) == "launch.oalaunch: content.mutators[1].options.health: expected a value of letters, "
             "digits, _, - or .",
        "a value cannot repeat an option"
    );
    expect(
        refused_text(
            "oalaunch: 1\ncontent:\n  mutators:\n  - id: wide-sight\n    options:\n"
            "        health: \"two thousand\"\n"
        ) == "launch.oalaunch: content.mutators[1].options.health: expected a value of letters, "
             "digits, _, - or .",
        "a value is an identifier"
    );
    expect(
        refused_text("oalaunch: 1\ncontent:\n  mutators: [wide-sight, wide-sight]\n") ==
            "launch.oalaunch: content.mutators[2]: \"wide-sight\" is named twice",
        "the same id twice"
    );
    std::string many = "oalaunch: 1\ncontent:\n  mutators:\n";
    for (int index = 0; index < 32; ++index)
        many += "  - wide-" + std::to_string(index) + "\n";
    const auto taken = launch_file::read_launch_text("launch.oalaunch", many);
    expect(taken.content.mutators.size() == 32, "32 mutators are taken");
    many += "  - wide-32\n";
    expect(
        refused_text(many) == "launch.oalaunch: content.mutators[33]: expected at most 32 mutators",
        "33 mutators are refused"
    );
}

} // namespace

int main() {
    const Scratch scratch;
    check_ini(scratch.write("taforever.ini", kIni));
    check_ini_keys();
    check_ini_reader();
    check_ini_hosting(scratch);
    check_battle_words(scratch);
    check_c_file(scratch);
    check_spaced_paths(scratch);
    check_switch_notes();
    check_launch();
    check_mutators();
    check_options_match_the_file(
        scratch.write("launch.oalaunch", kLaunch), scratch.write("plain.oalaunch", kPlainHost)
    );
    check_refusals(scratch);
    check_side_values();
    check_computer_brains();
    check_name_limits();
    check_either_file(scratch);
    check_kept_status(scratch);
    check_standard_input(scratch);
    if (failures != 0)
        std::fprintf(stderr, "%d launch checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
