// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/reporter.hpp"

#include "oa/data/campaign/campaign_file.hpp"
#include "oa/sim/simulation_state.hpp"
#include "oa/ui/frontend_multiplayer/connect.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace oa::boneyards::reporter {
namespace {

namespace mp = oa::ui::frontend_multiplayer;

constexpr const char* kSettingNames[setting_count] = {
    "connect",     "creator",  "registeredonly", "playerlimit", "timelimit",  "provider",
    "session",     "mission",  "password",       "player",      "pfs",        "address",
    "lockOptions", "maxunits", "energy",         "metal",       "deathopt",   "los",
    "cheats",      "location", "mapmode",        "watching",    "tournament",
};

constexpr const char* kScoreLabels[score_line_count] = {
    "Kills",
    "Losses",
    "Energy Produced",
    "Metal Produced",
    "Excess Energy",
    "Excess Metal",
    "Commanders Killed",
    "Commanders Lost",
    "I am Winner",
};

// The side names are stored directly before the team names, so a side index
// past Core reads a team name.
constexpr const char* kSideNames[] = {
    "Arm", "Core", "diamond", "x", "equals", "triangle", "slug", ""
};
constexpr uint32_t kSideNameCount = sizeof kSideNames / sizeof kSideNames[0];
constexpr const char* const* kTeamNames = kSideNames + 2;

constexpr const char* kProductName = "Total Annihilation";
constexpr uint32_t kVersionMajorMask = 0xffff0000u;
constexpr int32_t kLibraryInitializeFailed = 4;
constexpr uint32_t kTicksPerScoreStep = 60;
constexpr int32_t kDefaultPlayerLimit = 10;
constexpr int32_t kDefaultPfs = 5;
constexpr uint32_t kSessionTextLimit = setting_text_bytes - 1;
constexpr uint32_t kSessionTrimFirst = session_name_bytes - 2; // index 15; index 0 is never trimmed
constexpr uint16_t kVictoryFlag = 0x10;                        // Game.outcome_flags
constexpr uint8_t kRoleHost = 0x01;                            // PlayerSetupInfo.role
constexpr uint8_t kInfoStateDefeated = 2;
constexpr uint32_t kSendsScores = 0x3;
constexpr uint32_t kSendsChat = 0x8;
constexpr uint32_t kSendsAll = 0xffffffffu;
constexpr uint16_t kLockOptionsBit = 0x0001; // Game.setup_options
constexpr std::size_t kPasswordOffset = offsetof(PlayerSetupInfo, password);

// Provider indices of the "connect" setting.
constexpr int32_t kProviderTcpip = 1;
constexpr int32_t kProviderIpx = 2;
constexpr int32_t kProviderModem = 3;
constexpr int32_t kProviderSerial = 4;
constexpr int32_t kProviderUnknown = 5;

// LOS modes of the "los" setting.
constexpr int32_t kLosTrue = 1;
constexpr int32_t kLosCircular = 2;
constexpr int32_t kLosPermanent = 3;

const PlayerSetupInfo kNoInfo{};

const PlayerSetupInfo& info_of(const World& world, const Player& player) noexcept {
    const PlayerSetupInfo* info = world_player_info(&world, &player);
    return info != nullptr ? *info : kNoInfo;
}

bool watcher(const World& world, const Player& player) noexcept {
    return (info_of(world, player).options & OA_SETUP_OPTION_WATCHER) != 0;
}

// Remote slot whose lobby record reports it defeated (the lobby's
// slot_remote_defeated test over the canonical records).
bool remote_defeated(const World& world, const Player& player) noexcept {
    return player.in_use != 0 && player.status == OA_PLAYER_STATUS_MIRRORED &&
           info_of(world, player).state == kInfoStateDefeated;
}

// Truncates toward zero to a 64-bit integer and keeps the low 32 bits; NaN
// and values outside the 64-bit range give 0 (the low half of INT64_MIN).
int32_t truncated_low32(double value) noexcept {
    if (!(value > -9.2233720368547758e18 && value < 9.2233720368547758e18))
        return 0;
    return static_cast<int32_t>(
        static_cast<uint32_t>(static_cast<uint64_t>(static_cast<int64_t>(value)))
    );
}

// Copies at most size - 1 characters and always terminates; `limit` bounds
// the source record.
void copy_text(char* out, uint32_t size, const char* text, std::size_t limit = SIZE_MAX) noexcept {
    uint32_t length = 0;
    while (length + 1 < size && length < limit && text[length] != '\0') {
        out[length] = text[length];
        ++length;
    }
    out[length] = '\0';
}

void format_decimal(char* out, uint32_t value) noexcept {
    std::snprintf(out, setting_text_bytes, "%u", value);
}

char* text_of(Reporter& reporter, Setting setting) noexcept {
    return reporter.setting_text[static_cast<uint32_t>(setting)];
}

int32_t& value_of(Reporter& reporter, Setting setting) noexcept {
    return reporter.setting_values[static_cast<uint32_t>(setting)];
}

/// Fills one setting from a number, written as an unsigned decimal.
///
/// @param[in,out] reporter Reporter whose setting is filled.
/// @param setting Setting to fill.
/// @param value Value to store.
/// @quirk A negative value shows as its unsigned 32-bit form, as in 3.1c.
void set_number(Reporter& reporter, Setting setting, int32_t value) noexcept {
    value_of(reporter, setting) = value;
    format_decimal(text_of(reporter, setting), static_cast<uint32_t>(value));
}

void set_choice(Reporter& reporter, Setting setting, int32_t value, const char* text) noexcept {
    value_of(reporter, setting) = value;
    copy_text(text_of(reporter, setting), setting_text_bytes, text);
}

const char* side_name(uint8_t side) noexcept {
    return side < kSideNameCount ? kSideNames[side] : "";
}

// Team code the player upload takes from the first letter of the
// team name, in either case.
int32_t team_code(const char* team) noexcept {
    switch (team != nullptr ? team[0] : '\0') {
    case 'd':
    case 'D':
        return 1;
    case 'x':
    case 'X':
        return 2;
    case 'e':
    case 'E':
        return 3;
    case 't':
    case 'T':
        return 4;
    case 's':
    case 'S':
        return 5;
    default:
        return 0;
    }
}

bool uploads_online(const Hooks& hooks) noexcept {
    return hooks.uploads_online != nullptr && hooks.uploads_online(hooks.context);
}

/// Tells whether this simulation may be reported. A null hook answers yes.
///
/// @param hooks Platform hooks.
/// @return false when the hook refuses the simulation
bool simulation_permitted(const Hooks& hooks) noexcept {
    return hooks.simulation_permitted == nullptr || hooks.simulation_permitted(hooks.context);
}

/// Returns the online launch block's options.
///
/// @param hooks Platform hooks.
/// @return The options, or null when the game was not started through an online launch.
const OnlineOptions* online_options(const Hooks& hooks) noexcept {
    return hooks.online_options != nullptr ? hooks.online_options(hooks.context) : nullptr;
}

void release_library(Reporter& reporter, const Hooks& hooks, bool terminate) noexcept {
    if (terminate && reporter.library.terminate != nullptr)
        reporter.library.terminate(reporter.library.context);
    if (hooks.close_library != nullptr)
        hooks.close_library(hooks.context);
    reporter.library = {};
    reporter.library_loaded = false;
}

bool library_complete(const Library& library) noexcept {
    return library.get_version != nullptr && library.initialize != nullptr &&
           library.report != nullptr && library.report_chat != nullptr &&
           library.interval_timer != nullptr && library.terminate != nullptr;
}

void fill_player(Reporter& reporter, const World& world, uint32_t index) noexcept {
    const Player& player = world.game.players[index];
    const PlayerSetupInfo& info = info_of(world, player);
    const bool watching = watcher(world, player);
    ReportPlayer& out = reporter.players[index];
    out.name = player.name;
    uint32_t flags = 0;
    if (player.player_id != 0xffffffffu) {
        out.net_id = player.player_id;
        if (oa::sim::simulation_state::player_participating(player) &&
            (player.in_use == 0 || !watching))
            flags = player_flag::playing;
    }
    if ((player.in_use != 0 && player.status == OA_PLAYER_STATUS_COMPUTER) ||
        remote_defeated(world, player))
        flags |= player_flag::computer_or_out;
    if (player.in_use != 0) {
        if (watching && player.units_created == 0)
            flags |= player_flag::watching;
        if ((info.role & kRoleHost) != 0)
            flags |= player_flag::host;
    }
    out.flags = flags;
    out.side = side_name(info.side);
    const uint8_t team = player.team < OA_PLAYER_NO_TEAM ? player.team : OA_PLAYER_NO_TEAM;
    out.team = kTeamNames[team];

    int32_t allies = 0;
    for (uint32_t other = 0; other < OA_PLAYER_COUNT; ++other)
        if (other != index && player.alliance[other] != 0)
            out.allies[allies++] = &reporter.players[other];
    out.ally_count = allies;
    for (uint32_t rest = static_cast<uint32_t>(allies); rest < OA_PLAYER_COUNT; ++rest)
        out.allies[rest] = nullptr;

    int32_t members = 0;
    if (team != OA_PLAYER_NO_TEAM) {
        for (uint32_t other = 0; other < OA_PLAYER_COUNT; ++other) {
            const Player& mate = world.game.players[other];
            if (other == index || mate.team != team || mate.in_use == 0)
                continue;
            if (oa::sim::simulation_state::player_active(mate) || watcher(world, mate) ||
                static_cast<int32_t>(mate.units_created) > 0)
                out.team_members[members++] = &reporter.players[other];
        }
    }
    out.team_member_count = members;
    for (uint32_t rest = static_cast<uint32_t>(members); rest < OA_PLAYER_COUNT; ++rest)
        out.team_members[rest] = nullptr;
}

void fill_board(
    Reporter& reporter, const World& world, uint32_t index, bool final_report
) noexcept {
    const Game& game = world.game;
    const Player& player = game.players[index];
    ScoreBoard& board = reporter.boards[index];
    const float time_multiplier =
        reporter.options != nullptr ? reporter.options->time_multiplier : 0.0f;
    const float kill_multiplier =
        reporter.options != nullptr ? reporter.options->kill_multiplier : 0.0f;
    const auto steps = static_cast<double>(game.tick / kTicksPerScoreStep);
    board.score = static_cast<int32_t>(
        static_cast<uint32_t>(truncated_low32(steps * static_cast<double>(time_multiplier))) +
        static_cast<uint32_t>(truncated_low32(
            static_cast<double>(player.kills) * static_cast<double>(kill_multiplier)
        ))
    );
    board.line_count = static_cast<int32_t>(score_line_count);
    for (uint32_t line = 0; line < score_line_count; ++line)
        board.lines[line].label = kScoreLabels[line];
    board.lines[score_line::kills].value = player.kills;
    board.lines[score_line::losses].value = player.losses;
    board.lines[score_line::energy_produced].value = truncated_low32(player.energy_produced_total);
    board.lines[score_line::metal_produced].value = truncated_low32(player.metal_produced_total);
    board.lines[score_line::excess_energy].value = truncated_low32(player.energy_wasted_total);
    board.lines[score_line::excess_metal].value = truncated_low32(player.metal_wasted_total);
    board.lines[score_line::commanders_killed].value = player.commanders_killed;
    board.lines[score_line::commanders_lost].value = player.commanders_lost;
    // Only the local winner is marked; other rows keep their last value.
    if (!final_report || (game.outcome_flags & kVictoryFlag) == 0)
        board.lines[score_line::winner].value = 0;
    else if (index == game.local_player_index)
        board.lines[score_line::winner].value = 1;
}

// The session name without its trailing spaces, at most 63 characters.
void fill_session_setting(Reporter& reporter, const Game& game) noexcept {
    char* out = text_of(reporter, Setting::session);
    const char* name = session_name(game);
    std::size_t length = strnlen(name, setting_text_bytes);
    if (length > kSessionTextLimit)
        length = kSessionTextLimit;
    if (length == 0) {
        out[0] = '\0';
        return;
    }
    const char* last = name + length - 1;
    while (last > name && *last == ' ')
        --last;
    const auto kept = static_cast<std::size_t>(last - name) + 1;
    std::memmove(out, name, kept);
    out[kept] = '\0';
}

void fill_settings(Reporter& reporter, const World& world, const Hooks& hooks) noexcept {
    const Game& game = world.game;
    const OnlineOptions* online = online_options(hooks);
    const Player& local =
        game.players[game.local_player_index < OA_PLAYER_COUNT ? game.local_player_index : 0];
    const PlayerSetupInfo& info = info_of(world, local);
    const auto* info_bytes = reinterpret_cast<const char*>(&info);

    if (local.in_use != 0 && (info.role & kRoleHost) != 0)
        set_choice(reporter, Setting::creator, 1, "creator");
    else
        set_choice(reporter, Setting::creator, 0, "joiner");
    set_choice(reporter, Setting::registered_only, 1, "registered only");

    int32_t player_limit = kDefaultPlayerLimit;
    if (online != nullptr && online->player_limit > 1 && online->player_limit < kDefaultPlayerLimit)
        player_limit = online->player_limit;
    set_number(reporter, Setting::player_limit, player_limit);
    set_number(
        reporter,
        Setting::time_limit,
        online != nullptr && online->time_limit >= 1 ? online->time_limit : 0
    );
    if (online != nullptr && online->provider != nullptr && online->provider[0] != '\0')
        copy_text(text_of(reporter, Setting::provider), setting_text_bytes, online->provider);
    else
        text_of(reporter, Setting::provider)[0] = '\0';

    fill_session_setting(reporter, game);
    copy_text(reporter.mission, mission_bytes, info.map_name, sizeof info.map_name);
    copy_text(
        text_of(reporter, Setting::password),
        setting_text_bytes,
        info_bytes + kPasswordOffset,
        sizeof(PlayerSetupInfo) - kPasswordOffset
    );
    copy_text(
        text_of(reporter, Setting::player), setting_text_bytes, local.name, sizeof local.name
    );
    set_number(
        reporter, Setting::pfs, online != nullptr && online->pfs != 0 ? online->pfs : kDefaultPfs
    );
    if (online != nullptr) {
        if (online->address != nullptr && online->address[0] != '\0')
            copy_text(text_of(reporter, Setting::address), setting_text_bytes, online->address);
        else
            text_of(reporter, Setting::address)[0] = '\0';
    }

    const bool locked = (game.setup_options & kLockOptionsBit) != 0;
    set_choice(reporter, Setting::lock_options, locked ? 1 : 0, locked ? "locked" : "not locked");
    set_number(reporter, Setting::max_units, info.max_units);
    set_number(reporter, Setting::energy, info.energy_hundreds * 100);
    set_number(reporter, Setting::metal, info.metal_hundreds * 100);

    const uint16_t options = info.options;
    const uint32_t commander =
        (options & OA_SETUP_OPTION_COMMANDER_MASK) >> OA_SETUP_OPTION_COMMANDER_SHIFT;
    static constexpr const char* kDeathOptions[] = {
        "game continues", "game ends", "deathmatch", "unknown"
    };
    set_choice(
        reporter,
        Setting::death_option,
        static_cast<int32_t>(commander) + 1,
        kDeathOptions[commander]
    );

    int32_t los = kLosPermanent;
    if ((options & OA_SETUP_OPTION_LOS_LIMITED) != 0)
        los = (options & OA_SETUP_OPTION_LOS_TRUE) != 0 ? kLosTrue : kLosCircular;
    set_choice(
        reporter,
        Setting::los,
        los,
        los == kLosTrue       ? "true"
        : los == kLosCircular ? "circular"
                              : "permanent"
    );

    const bool cheats = (options & OA_SETUP_OPTION_CHEATS_ALLOWED) != 0;
    set_choice(reporter, Setting::cheats, cheats ? 2 : 1, cheats ? "allowed" : "disallowed");
    const bool fixed = (options & OA_SETUP_OPTION_FIXED_LOCATIONS) != 0;
    set_choice(reporter, Setting::location, fixed ? 1 : 2, fixed ? "fixed" : "random");
    // The report calls the set bit "mapped".
    const bool mapped = (options & OA_SETUP_OPTION_UNMAPPED) != 0;
    set_choice(reporter, Setting::map_mode, mapped ? 1 : 2, mapped ? "mapped" : "unmapped");
    const bool watching = (options & OA_SETUP_OPTION_WATCHING_ALLOWED) != 0;
    set_choice(reporter, Setting::watching, watching ? 2 : 1, watching ? "allowed" : "disallowed");
    set_number(
        reporter,
        Setting::tournament,
        online != nullptr && online->tournament != 0 ? online->tournament : 0
    );
}

SettingsUpload settings_upload(Reporter& reporter) noexcept {
    SettingsUpload upload{};
    upload.lock_options = value_of(reporter, Setting::lock_options);
    upload.mission = reporter.settings[static_cast<uint32_t>(Setting::mission)].text;
    upload.player_limit = value_of(reporter, Setting::player_limit);
    upload.death_option = value_of(reporter, Setting::death_option);
    upload.energy = value_of(reporter, Setting::energy);
    upload.metal = value_of(reporter, Setting::metal);
    upload.location = value_of(reporter, Setting::location);
    upload.los = value_of(reporter, Setting::los);
    upload.map_mode = value_of(reporter, Setting::map_mode);
    upload.max_units = value_of(reporter, Setting::max_units);
    upload.watching = value_of(reporter, Setting::watching);
    upload.cheats = value_of(reporter, Setting::cheats);
    upload.tournament = value_of(reporter, Setting::tournament);
    return upload;
}

ScoreUpload score_upload(const Reporter& reporter, uint32_t index, bool final_report) noexcept {
    const ScoreBoard& board = reporter.boards[index];
    ScoreUpload upload{};
    upload.net_id = reporter.players[index].net_id;
    for (uint32_t line = 0; line < score_line::winner; ++line)
        upload.lines[line] = board.lines[line].value;
    upload.score = board.score;
    upload.winner = board.lines[score_line::winner].value != 0;
    upload.final_report = final_report;
    return upload;
}

// The upload half of an event: settings and players when the host
// launches, scores periodically and for the local winner at the end.
void upload_event(
    Reporter& reporter, World& world, int32_t event, int32_t count, const Hooks& hooks
) noexcept {
    if (event == event::battleroom_opened || event == event::game_started ||
        event == event::game_launched || event == event::game_ended)
        reporter.upload_sends = kSendsAll;
    if ((reporter.upload_sends & kSendsScores) == 0 || !uploads_online(hooks))
        return;
    const uint8_t local =
        world.game.local_player_index < OA_PLAYER_COUNT ? world.game.local_player_index : 0;
    const bool host = (reporter.players[local].flags & player_flag::host) != 0;
    if (event == event::game_launched && host) {
        if (hooks.upload_settings != nullptr)
            hooks.upload_settings(hooks.context, settings_upload(reporter));
        for (int32_t index = 0; index < count; ++index) {
            const ReportPlayer& player = reporter.players[index];
            if (hooks.upload_player != nullptr)
                hooks.upload_player(
                    hooks.context,
                    PlayerUpload{player.net_id, player.name, player.side, team_code(player.team)}
                );
        }
        reporter.upload_countdown = upload_interval;
    }
    const bool winner = reporter.boards[local].lines[score_line::winner].value != 0;
    if ((event == event::interval && host) || (event == event::game_ended && winner))
        for (int32_t index = 0; index < count; ++index)
            if (hooks.upload_score != nullptr)
                hooks.upload_score(
                    hooks.context,
                    score_upload(reporter, static_cast<uint32_t>(index), event == event::game_ended)
                );
    if (event == event::game_ended || event == event::session_closed) {
        reporter.upload_countdown = upload_off;
        world.game.password[0] = '\0';
    }
}

} // namespace

const char* session_name(const Game& game) noexcept {
    return reinterpret_cast<const char*>(game.session_description);
}

void set_session_name(Game& game, const char* name) noexcept {
    copy_text(
        reinterpret_cast<char*>(game.session_description),
        session_name_bytes,
        name != nullptr ? name : ""
    );
}

int32_t alloc_report_tables(Reporter& reporter) noexcept {
    for (auto& player : reporter.players)
        player = {};
    for (auto& board : reporter.boards)
        board = {};
    std::memset(reporter.setting_text, 0, sizeof reporter.setting_text);
    for (uint32_t index = 0; index < setting_count; ++index) {
        reporter.setting_values[index] = setting_unset;
        reporter.settings[index] = {
            kSettingNames[index], reporter.setting_text[index], &reporter.setting_values[index]
        };
    }
    reporter.settings[static_cast<uint32_t>(Setting::mission)].text = reporter.mission;
    reporter.setting_total = static_cast<int32_t>(setting_count);
    reporter.buffers = true;
    return result::ok;
}

void shutdown_reporter(Reporter& reporter, const Hooks& hooks) noexcept {
    reporter.active = 0;
    if (reporter.library_loaded) {
        if (hooks.close_session != nullptr)
            hooks.close_session(hooks.context);
        release_library(reporter, hooks, true);
    }
    reporter.buffers = false;
    reporter.setting_total = 0;
}

int32_t init_game_reporter(
    Reporter& reporter,
    Game& game,
    const oa::data::campaign::CampaignFile* options,
    int32_t* status,
    const Hooks& hooks
) noexcept {
    shutdown_reporter(reporter, hooks);
    if (!simulation_permitted(hooks))
        return result::refused;
    reporter.options = options;
    const OnlineOptions* online = online_options(hooks);
    const bool named_provider =
        online != nullptr && online->provider != nullptr && online->provider[0] != '\0';
    const mp::ProviderKind kind = mp::provider_kind(game.provider_guid);
    if (!named_provider && (kind == mp::ProviderKind::ipx || kind == mp::ProviderKind::serial ||
                            kind == mp::ProviderKind::modem))
        return result::unavailable;
    alloc_report_tables(reporter);
    int32_t provider = kProviderUnknown;
    const char* provider_text = "unknown provider";
    switch (kind) {
    case mp::ProviderKind::tcpip:
        provider = kProviderTcpip;
        provider_text = "tcpip";
        break;
    case mp::ProviderKind::ipx:
        provider = kProviderIpx;
        provider_text = "ipx";
        break;
    case mp::ProviderKind::modem:
        provider = kProviderModem;
        provider_text = "modem";
        break;
    case mp::ProviderKind::serial:
        provider = kProviderSerial;
        provider_text = "serial";
        break;
    default:
        break;
    }
    copy_text(text_of(reporter, Setting::connect), setting_text_bytes, provider_text);
    value_of(reporter, Setting::connect) = provider;
    reporter.active = 1;
    *status = 0;
    if (reporter.library_loaded)
        return result::ok;
    Library library{};
    if (hooks.open_library == nullptr || !hooks.open_library(hooks.context, &library))
        return result::unavailable;
    reporter.library = library;
    if (!library_complete(library)) {
        release_library(reporter, hooks, false);
        return result::library_failed;
    }
    if ((library.get_version(library.context, kProductName) & kVersionMajorMask) != 0) {
        release_library(reporter, hooks, true);
        return result::version_mismatch;
    }
    if (library.initialize(library.context, status) == kLibraryInitializeFailed) {
        release_library(reporter, hooks, true);
        return result::unavailable;
    }
    reporter.library_loaded = true;
    return result::ok;
}

int32_t build_score_report(
    Reporter& reporter, const World& world, bool full, bool final_report, const Hooks& hooks
) noexcept {
    int32_t count = 0;
    for (uint32_t index = 0; index < OA_PLAYER_COUNT; ++index) {
        const Player& player = world.game.players[index];
        if (player.in_use == 0 && player.units_created == 0)
            continue;
        ++count;
        fill_player(reporter, world, index);
        fill_board(reporter, world, index, final_report);
    }
    if (full)
        fill_settings(reporter, world, hooks);
    return count;
}

int32_t report_event(
    Reporter& reporter, World& world, int32_t event, uint8_t routes, const Hooks& hooks
) noexcept {
    if (!simulation_permitted(hooks))
        return result::refused;
    if (reporter.active == 0 && !reporter.library_loaded)
        return result::unavailable;
    if (!reporter.buffers)
        return result::no_buffers;
    uint8_t guid[16]{};
    if (hooks.session_guid != nullptr) {
        if (const uint8_t* source = hooks.session_guid(hooks.context))
            std::memcpy(guid, source, sizeof guid);
    }
    const uint32_t session_value =
        hooks.session_value != nullptr ? hooks.session_value(hooks.context) : 0;
    if (event == event::battleroom_opened) {
        std::memcpy(reporter.session_name, session_name(world.game), session_name_bytes);
        for (uint32_t at = kSessionTrimFirst; at > 0 && reporter.session_name[at] == ' '; --at)
            reporter.session_name[at] = '\0';
    }
    const bool full =
        event < event::game_ended || event == event::game_launched || event == event::interval;
    const int32_t count =
        build_score_report(reporter, world, full, event == event::game_ended, hooks);
    if (reporter.active != 0 && (routes & route_uploads) != 0)
        upload_event(reporter, world, event, count, hooks);
    if (reporter.library_loaded && (routes & route_library) != 0) {
        LibraryReport report{};
        report.event = event;
        report.session_guid = guid;
        report.provider_guid = world.game.provider_guid;
        report.session_value = session_value;
        report.session_name = reporter.session_name;
        report.map_name = reporter.mission;
        report.local_player = world.game.local_player_index;
        report.player_count = count;
        report.reporter = &reporter;
        reporter.library.report(reporter.library.context, report);
    }
    return result::ok;
}

int32_t
report_chat(Reporter& reporter, World& world, const char* line, const Hooks& hooks) noexcept {
    if (!simulation_permitted(hooks))
        return result::refused;
    const bool uploads = reporter.active != 0 && (reporter.upload_sends & kSendsChat) != 0;
    if (!uploads && !reporter.library_loaded)
        return result::unavailable;
    build_score_report(reporter, world, false, false, hooks);
    const uint8_t local =
        world.game.local_player_index < OA_PLAYER_COUNT ? world.game.local_player_index : 0;
    // Already UTF-8. The report record converts when it is written.
    if (uploads && uploads_online(hooks) && hooks.upload_chat != nullptr)
        hooks.upload_chat(hooks.context, reporter.players[local].net_id, line);
    if (!reporter.library_loaded)
        return result::ok;
    return reporter.library.report_chat(reporter.library.context, reporter.players[local], line);
}

void step_reporter(Reporter& reporter, World& world, const Hooks& hooks) noexcept {
    if (!simulation_permitted(hooks))
        return;
    if (reporter.library_loaded)
        reporter.library.interval_timer(reporter.library.context);
    if (!uploads_online(hooks) || reporter.upload_countdown < 0)
        return;
    if (reporter.upload_countdown == 0) {
        report_event(reporter, world, event::interval, route_all, hooks);
        reporter.upload_countdown = upload_interval;
        return;
    }
    --reporter.upload_countdown;
}

bool reporter_library_loaded(const Reporter& reporter) noexcept {
    return reporter.library_loaded;
}

} // namespace oa::boneyards::reporter
