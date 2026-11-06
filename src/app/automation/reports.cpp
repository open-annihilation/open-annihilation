// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the endpoint reports of a match and of the battle room (reports.hpp).
#include "reports.hpp"

#include "oa/core/game_state.h"
#include "oa/core/player.h"
#include "oa/core/player_setup.h"
#include "oa/core/world.h"
#include "oa/sim/scenario/outcome.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

namespace oa::app::automation {
namespace {

namespace mp = oa::ui::frontend_multiplayer;
namespace outcome_flag = oa::sim::scenario::outcome_flag;

// Game.sim_run_flags: the match is paused.
constexpr uint16_t kRunFlagPaused = 0x0001;
// The outcome flags a decided match holds, one of them: won or lost.
constexpr uint16_t kDecidedFlags =
    outcome_flag::victory_transition | outcome_flag::defeat_transition;
// The battle room's commander rules, by the stage of its COMMANDER button.
constexpr std::string_view kCommanderRules[] = {"continues", "ends", "deathmatch"};

/// Returns the text of a fixed field: its bytes up to the first zero byte.
///
/// @param bytes the field
/// @param size the field's size in bytes
/// @return the text
std::string_view field_text(const char* bytes, size_t size) noexcept {
    const auto* end = static_cast<const char*>(std::memchr(bytes, 0, size));
    return {bytes, end != nullptr ? static_cast<size_t>(end - bytes) : size};
}

/// Writes a player's status as the protocol names it.
///
/// @param[in,out] json the JSON being written
/// @param status the player's Player.status
void write_status(JsonWriter& json, uint8_t status) {
    switch (status) {
    case OA_PLAYER_STATUS_LOCAL:
        json.string("local");
        break;
    case OA_PLAYER_STATUS_COMPUTER:
        json.string("computer");
        break;
    case OA_PLAYER_STATUS_MIRRORED:
        json.string("remote");
        break;
    default:
        json.null();
        break;
    }
}

/// Writes a side's index and name as the members side and side_name; the
/// name is null when `side` is null or names nothing.
///
/// @param[in,out] json the object being written
/// @param side the side record; null when the game holds none
/// @param side_index the side's index
void write_side(JsonWriter& json, const oa::Side* side, uint8_t side_index) {
    json.key("side");
    json.integer(side_index);
    json.key("side_name");
    const std::string name =
        side != nullptr ? game_text(side->name, sizeof side->name) : std::string{};
    if (!name.empty())
        json.string(name);
    else
        json.null();
}

/// Writes a team, null for none.
///
/// @param[in,out] json the JSON being written
/// @param team the team, OA_PLAYER_NO_TEAM for none
void write_team(JsonWriter& json, uint8_t team) {
    if (team < OA_PLAYER_NO_TEAM)
        json.integer(team);
    else
        json.null();
}

// The simulation's tick rate, which the in-game clock divides by.
constexpr uint32_t kTicksPerSecond = 30;

/// Writes one real number as JSON, in the units the resource bar reads.
///
/// A value that is not finite is null. Trailing zeros after the decimal
/// are dropped, and a negative zero is written as 0.
///
/// @param[in,out] json the JSON being written
/// @param value the number
void write_real(JsonWriter& json, double value) {
    if (!std::isfinite(value)) {
        json.null();
        return;
    }
    char text[128];
    const int written = std::snprintf(text, sizeof text, "%.6f", value);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof text) {
        json.null();
        return;
    }
    for (char* cursor = text; *cursor != '\0'; ++cursor) {
        if (*cursor == ',')
            *cursor = '.';
    }
    char* finish = text + written;
    char* point = nullptr;
    for (char* cursor = text; cursor != finish; ++cursor) {
        if (*cursor == '.')
            point = cursor;
    }
    if (point != nullptr) {
        while (finish > point + 1 && finish[-1] == '0')
            --finish;
        if (finish > point && finish[-1] == '.')
            --finish;
        *finish = '\0';
    }
    if (text[0] == '-' && text[1] == '0' && text[2] == '\0') {
        json.raw("0");
        return;
    }
    json.raw(text);
}

/// Writes the clock the match shows: the tick, the whole seconds, and h:mm:ss.
///
/// @param[in,out] json the JSON being written
/// @param tick the match's tick
void write_clock(JsonWriter& json, uint32_t tick) {
    const uint32_t whole_seconds = tick / kTicksPerSecond;
    const unsigned hours = whole_seconds / 3600u;
    const unsigned minutes = (whole_seconds / 60u) % 60u;
    const unsigned remain = whole_seconds % 60u;
    char display[32];
    const int written =
        std::snprintf(display, sizeof display, "%02u:%02u:%02u", hours, minutes, remain);
    json.begin_object();
    json.key("tick");
    json.integer(static_cast<int64_t>(tick));
    json.key("seconds");
    json.integer(static_cast<int64_t>(whole_seconds));
    json.key("display");
    if (written > 0 && static_cast<std::size_t>(written) < sizeof display)
        json.string(display);
    else
        json.null();
    json.end_object();
}

/// Tells whether a seat's economy is shown.
///
/// The local player is shown, and so is a player allied with the local
/// player. all shows every seat.
///
/// @param game the match
/// @param slot the seat
/// @param shown whose economy is written
/// @return true when the economy is written
bool economy_visible(const oa::Game& game, uint8_t slot, EconomyView shown) {
    if (shown == EconomyView::all)
        return true;
    const uint8_t local = game.local_player_index;
    if (slot == local)
        return true;
    if (local >= OA_PLAYER_COUNT)
        return false;
    return game.players[local].allied_by[slot] != 0;
}

/// Writes one resource: amount, storage, the shared store when it is shared,
/// the bar's income and expense, and the totals.
///
/// storage is the player's store as the bar reads it. shared_storage is
/// written beside it when the player shares allies' storage, and is not
/// added into storage.
///
/// @param[in,out] json the JSON being written
/// @param amount the store
/// @param storage the storage the bar reads
/// @param shared the allies' shared storage
/// @param shares the player shares allies' storage
/// @param income produced per second
/// @param expense requested per second
/// @param produced the produced total
/// @param requested the requested total
/// @param wasted the wasted total
void write_store(
    JsonWriter& json,
    float amount,
    float storage,
    float shared,
    bool shares,
    float income,
    float expense,
    double produced,
    double requested,
    double wasted
) {
    json.begin_object();
    json.key("amount");
    write_real(json, amount);
    json.key("storage");
    write_real(json, storage);
    if (shares) {
        json.key("shared_storage");
        write_real(json, shared);
    }
    json.key("income");
    write_real(json, income);
    json.key("expense");
    write_real(json, expense);
    json.key("produced_total");
    write_real(json, produced);
    json.key("requested_total");
    write_real(json, requested);
    json.key("wasted_total");
    write_real(json, wasted);
    json.end_object();
}

/// Writes a player's metal and energy.
///
/// @param[in,out] json the JSON being written
/// @param player the player
void write_economy(JsonWriter& json, const oa::Player& player) {
    const bool shares = (player.resource_flags & OA_PLAYER_RESOURCE_SHARES_STORAGE) != 0;
    json.begin_object();
    json.key("metal");
    write_store(
        json,
        player.metal,
        player.metal_storage,
        player.shared_metal_storage,
        shares,
        player.metal_produced,
        player.metal_requested,
        player.metal_produced_total,
        player.metal_requested_total,
        player.metal_wasted_total
    );
    json.key("energy");
    write_store(
        json,
        player.energy,
        player.energy_storage,
        player.shared_energy_storage,
        shares,
        player.energy_produced,
        player.energy_requested,
        player.energy_produced_total,
        player.energy_requested_total,
        player.energy_wasted_total
    );
    json.end_object();
}

/// Returns the setup the match's rules are read from: the local player's,
/// or the first seated player's when the local player has none.
///
/// @param world the match
/// @return the setup; null when no seated player has one
const oa::PlayerSetupInfo* match_setup(const oa::World& world) {
    const oa::Game& game = world.game;
    if (game.local_player_index < OA_PLAYER_COUNT) {
        if (const oa::PlayerSetupInfo* info =
                oa::world_player_info(&world, &game.players[game.local_player_index]))
            return info;
    }
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        if (!player_seated(game.players[slot]))
            continue;
        if (const oa::PlayerSetupInfo* info = oa::world_player_info(&world, &game.players[slot]))
            return info;
    }
    return nullptr;
}

/// Writes one player of a match.
///
/// @param[in,out] json the JSON being written
/// @param world the match
/// @param slot the player's slot
/// @param shown whose economy is written
void write_match_player(JsonWriter& json, const oa::World& world, uint8_t slot, EconomyView shown) {
    const oa::Game& game = world.game;
    const oa::Player& player = game.players[slot];
    const oa::PlayerSetupInfo* info = oa::world_player_info(&world, &player);
    json.begin_object();
    json.key("slot");
    json.integer(slot);
    json.key("name");
    json.string(game_text(player.name, sizeof player.name));
    json.key("status");
    write_status(json, player.status);
    const uint8_t side_index = info != nullptr ? info->side : 0;
    write_side(json, oa::world_side(&world, side_index), side_index);
    json.key("colour");
    if (info != nullptr)
        json.integer(info->color);
    else
        json.null();
    json.key("team");
    write_team(json, player.team);
    json.key("allies");
    json.begin_array();
    for (uint8_t other = 0; other < OA_PLAYER_COUNT; ++other)
        if (other != slot && player.alliance[other] != 0 && player_seated(game.players[other]))
            json.integer(other);
    json.end_array();
    json.key("computer");
    json.boolean(player.status == OA_PLAYER_STATUS_COMPUTER);
    json.key("local");
    json.boolean(slot == game.local_player_index);
    json.key("alive");
    json.boolean(player.unit_count != 0);
    json.key("units");
    json.integer(player.unit_count);
    json.key("kills");
    json.integer(player.kills);
    json.key("losses");
    json.integer(player.losses);
    json.key("units_created");
    json.integer(static_cast<int64_t>(player.units_created));
    json.key("board_row");
    json.integer(player.board_row);
    json.key("economy");
    if (economy_visible(game, slot, shown))
        write_economy(json, player);
    else
        json.null();
    json.end_object();
}

/// Writes the battle room's options, as its host has set them.
///
/// @param[in,out] json the JSON being written
/// @param lobby the lobby
/// @param host the host's setup block
void write_room_options(JsonWriter& json, mp::Lobby& lobby, const mp::PlayerSetupInfo& host) {
    const uint16_t options = host.options;
    json.begin_object();
    json.key("commander");
    const auto rule =
        static_cast<size_t>((options & mp::option::commander_mask) / mp::option::commander_step);
    if (rule < std::size(kCommanderRules))
        json.string(kCommanderRules[rule]);
    else
        json.null();
    json.key("line_of_sight");
    if ((options & mp::option::los_limited) == 0)
        json.string("permanent");
    else
        json.string((options & mp::option::los_true) != 0 ? "true" : "circular");
    json.key("mapped");
    json.boolean((options & mp::option::unmapped) == 0);
    json.key("cheats");
    json.boolean((options & mp::option::cheats_allowed) != 0);
    json.key("fixed_locations");
    json.boolean((options & mp::option::fixed_locations) != 0);
    json.key("watching_allowed");
    json.boolean((options & mp::option::watching_allowed) != 0);
    json.key("locked");
    json.boolean(mp::lobby_options_locked(lobby));
    json.key("password");
    json.boolean((host.status & mp::status::password) != 0);
    json.key("energy");
    json.integer(int64_t{host.energy_hundreds} * 100);
    json.key("metal");
    json.integer(int64_t{host.metal_hundreds} * 100);
    json.key("max_units");
    json.integer(host.max_units);
    json.end_object();
}

/// Writes one player of the battle room.
///
/// @param[in,out] json the JSON being written
/// @param lobby the lobby
/// @param slot the player's slot
void write_room_player(JsonWriter& json, mp::Lobby& lobby, uint8_t slot) {
    oa::Player& player = mp::slot_player(lobby, slot);
    const mp::PlayerSetupInfo* info = mp::slot_info(lobby, slot);
    json.begin_object();
    json.key("slot");
    json.integer(slot);
    json.key("name");
    json.string(game_text(player.name, sizeof player.name));
    json.key("status");
    write_status(json, player.status);
    json.key("ready");
    json.boolean(info != nullptr && (info->options & mp::option::ready) != 0);
    json.key("watcher");
    json.boolean(info != nullptr && (info->options & mp::option::watcher) != 0);
    const uint8_t side_index = info != nullptr ? info->side : 0;
    const oa::Game& block = *lobby.game;
    const oa::Side* side = nullptr;
    if (side_index < block.side_count && side_index < std::size(block.sides))
        side = &block.sides[side_index];
    write_side(json, side, side_index);
    json.key("colour");
    if (info != nullptr && info->color != mp::kNoColor)
        json.integer(info->color);
    else
        json.null();
    json.key("team");
    write_team(json, mp::lobby_player_team(player));
    json.key("ping");
    json.integer(mp::lobby_player_ping(player));
    json.end_object();
}

} // namespace

std::string game_text(const char* bytes, size_t size) {
    return std::string(field_text(bytes, size));
}

std::string_view player_name(const oa::Player& player) noexcept {
    return field_text(player.name, sizeof player.name);
}

bool player_seated(const oa::Player& player) noexcept {
    return player.in_use != 0 && player.index < OA_PLAYER_COUNT &&
           (player.status == OA_PLAYER_STATUS_LOCAL || player.status == OA_PLAYER_STATUS_COMPUTER ||
            player.status == OA_PLAYER_STATUS_MIRRORED);
}

bool match_decided(const oa::Game& game) noexcept {
    return (game.outcome_flags & kDecidedFlags) != 0;
}

std::optional<std::string_view> match_outcome(const oa::Game& game) noexcept {
    if (!match_decided(game))
        return std::nullopt;
    return (game.outcome_flags & outcome_flag::won) != 0 ? "victory" : "defeat";
}

namespace {

/// Tells whether a seat is one the results name.
///
/// @param world the match
/// @param slot the seat
/// @return true when a player sits there, left from there, or produced units there
bool seat_reported(const oa::World& world, uint8_t slot) {
    const oa::Player& player = world.game.players[slot];
    if (player.index != slot)
        return false;
    if (player_seated(player) || player.reject_reason != 0 || player.units_created != 0)
        return true;
    return player.in_use != 0 && !player_name(player).empty();
}

/// Finds the root of a seat's group.
///
/// @param[in,out] parent each seat's parent
/// @param slot the seat
/// @return the group's root
uint8_t find_group(std::array<uint8_t, OA_PLAYER_COUNT>& parent, uint8_t slot) {
    uint8_t root = slot;
    while (parent[root] != root)
        root = parent[root];
    while (parent[slot] != root) {
        const uint8_t next = parent[slot];
        parent[slot] = root;
        slot = next;
    }
    return root;
}

} // namespace

std::optional<uint8_t> match_winner(const oa::Game& game) noexcept {
    if (!match_decided(game))
        return std::nullopt;
    const uint8_t local = game.local_player_index;
    if ((game.outcome_flags & outcome_flag::won) != 0 && local < OA_PLAYER_COUNT)
        return local;
    std::optional<uint8_t> standing;
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        const oa::Player& player = game.players[slot];
        if (!player_seated(player) || player.unit_count == 0)
            continue;
        if (standing)
            return std::nullopt;
        standing = slot;
    }
    return standing;
}

MatchResults match_results(const oa::World& world) {
    const oa::Game& game = world.game;
    MatchResults results;
    results.tick = game.tick;

    struct Listed {
        uint8_t slot{};
        bool competing{};
    };

    std::array<Listed, OA_PLAYER_COUNT> listed{};
    uint8_t listed_count = 0;
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        if (!seat_reported(world, slot))
            continue;
        const oa::Player& player = game.players[slot];
        const oa::PlayerSetupInfo* info = oa::world_player_info(&world, &player);
        const bool departed = player.reject_reason != 0 || !player_seated(player);
        const bool watching =
            !departed && info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) != 0;
        SeatResult seat;
        seat.slot = slot;
        seat.name = game_text(player.name, sizeof player.name);
        if (player.team < OA_PLAYER_NO_TEAM)
            seat.team = player.team;
        seat.side = info != nullptr ? info->side : 0;
        seat.units = player.unit_count;
        seat.kills = player.kills;
        seat.losses = player.losses;
        if (departed)
            seat.outcome = "left";
        else if (watching)
            seat.outcome = "watcher";
        else
            seat.outcome = "drawn";
        listed[listed_count] = Listed{slot, !departed && !watching};
        ++listed_count;
        results.seats.push_back(std::move(seat));
    }

    std::array<uint8_t, OA_PLAYER_COUNT> parent{};
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
        parent[slot] = slot;
    for (uint8_t left = 0; left < listed_count; ++left) {
        if (!listed[left].competing)
            continue;
        const oa::Player& player = game.players[listed[left].slot];
        for (uint8_t right = static_cast<uint8_t>(left + 1); right < listed_count; ++right) {
            if (!listed[right].competing)
                continue;
            const uint8_t other = listed[right].slot;
            const oa::Player& second = game.players[other];
            const bool same_team = player.team < OA_PLAYER_NO_TEAM && player.team == second.team;
            const bool mutual =
                player.alliance[other] != 0 && second.alliance[listed[left].slot] != 0;
            if (!same_team && !mutual)
                continue;
            const uint8_t root = find_group(parent, listed[left].slot);
            parent[find_group(parent, other)] = root;
        }
    }

    std::array<bool, OA_PLAYER_COUNT> group_alive{};
    std::array<bool, OA_PLAYER_COUNT> group_seen{};
    uint8_t alive_groups = 0;
    uint8_t alive_players = 0;
    std::optional<std::string> sole;
    for (uint8_t at = 0; at < listed_count; ++at) {
        if (!listed[at].competing)
            continue;
        const uint8_t slot = listed[at].slot;
        const uint8_t root = find_group(parent, slot);
        if (!group_seen[root]) {
            group_seen[root] = true;
            if (game.players[slot].unit_count != 0)
                group_alive[root] = true;
        } else if (game.players[slot].unit_count != 0) {
            group_alive[root] = true;
        }
        if (game.players[slot].unit_count == 0)
            continue;
        ++alive_players;
        sole = results.seats[at].name;
    }
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
        if (group_seen[slot] && parent[slot] == slot && group_alive[slot])
            ++alive_groups;
    if (alive_players == 1)
        results.winner = sole;

    const bool decided_win = alive_groups == 1;
    for (uint8_t at = 0; at < listed_count; ++at) {
        if (!listed[at].competing)
            continue;
        const uint8_t root = find_group(parent, listed[at].slot);
        if (decided_win)
            results.seats[at].outcome = group_alive[root] ? "won" : "lost";
        else
            results.seats[at].outcome = "drawn";
    }
    return results;
}

void write_result_fields(JsonWriter& json, const MatchResults& results, bool write_winner) {
    json.key("results");
    json.begin_array();
    for (const SeatResult& seat : results.seats) {
        json.begin_object();
        json.key("slot");
        json.integer(seat.slot);
        json.key("name");
        json.string(seat.name);
        json.key("team");
        if (seat.team)
            json.integer(*seat.team);
        else
            json.null();
        json.key("side");
        json.integer(seat.side);
        json.key("outcome");
        json.string(seat.outcome);
        json.key("units");
        json.integer(seat.units);
        json.key("kills");
        json.integer(static_cast<int64_t>(seat.kills));
        json.key("losses");
        json.integer(static_cast<int64_t>(seat.losses));
        json.end_object();
    }
    json.end_array();
    if (write_winner) {
        json.key("winner");
        if (results.winner)
            json.string(*results.winner);
        else
            json.null();
    }
    json.key("tick");
    json.integer(results.tick);
}

void write_map(JsonWriter& json, const oa::World& match) {
    const oa::Game& game = match.game;
    const oa::PlayerSetupInfo* setup = match_setup(match);
    json.begin_object();
    json.key("name");
    const std::string name =
        setup != nullptr ? game_text(setup->map_name, sizeof setup->map_name) : std::string{};
    if (!name.empty())
        json.string(name);
    else
        json.null();
    json.key("width");
    json.integer(static_cast<int64_t>(game.map_width));
    json.key("height");
    json.integer(static_cast<int64_t>(game.map_height));
    json.end_object();
}

void write_rule_fields(JsonWriter& json, const oa::World& match) {
    const oa::Game& game = match.game;
    const oa::PlayerSetupInfo* setup = match_setup(match);
    json.key("commander");
    if (game.session_rules < 0 ||
        static_cast<std::size_t>(game.session_rules) >= std::size(kCommanderRules))
        json.null();
    else
        json.string(kCommanderRules[static_cast<std::size_t>(game.session_rules)]);
    const uint8_t flags = game.visibility_flags;
    json.key("line_of_sight");
    if ((flags & OA_VISIBILITY_LINE_OF_SIGHT) == 0)
        json.string("permanent");
    else if ((flags & OA_VISIBILITY_ALTITUDE_SIGHT) != 0)
        json.string("true");
    else
        json.string("circular");
    json.key("mapped");
    json.boolean((flags & OA_VISIBILITY_MAPPING) == 0);
    json.key("metal");
    json.integer(setup != nullptr ? int64_t{setup->metal_hundreds} * 100 : int64_t{0});
    json.key("energy");
    json.integer(setup != nullptr ? int64_t{setup->energy_hundreds} * 100 : int64_t{0});
    json.key("unit_limit");
    json.integer(static_cast<int64_t>(game.max_units_setting));
}

void write_seated_sides(JsonWriter& json, const oa::World& match) {
    const oa::Game& game = match.game;
    json.begin_array();
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        const oa::Player& player = game.players[slot];
        if (!player_seated(player))
            continue;
        const oa::PlayerSetupInfo* info = oa::world_player_info(&match, &player);
        const uint8_t side_index = info != nullptr ? info->side : 0;
        json.begin_object();
        json.key("slot");
        json.integer(slot);
        write_side(json, oa::world_side(&match, side_index), side_index);
        json.key("team");
        write_team(json, player.team);
        json.end_object();
    }
    json.end_array();
}

void write_match(
    JsonWriter& json, const oa::World* match, std::optional<uint64_t> digest, EconomyView shown
) {
    json.key("in_match");
    json.boolean(match != nullptr);
    if (match == nullptr) {
        json.key("paused");
        json.boolean(false);
        json.key("speed");
        json.null();
        json.key("players");
        json.begin_array();
        json.end_array();
        json.key("local_player");
        json.null();
        json.key("game_over");
        json.boolean(false);
        json.key("outcome");
        json.null();
        json.key("winner");
        json.null();
        return;
    }
    const oa::Game& game = match->game;
    json.key("paused");
    json.boolean((game.sim_run_flags & kRunFlagPaused) != 0);
    json.key("speed");
    json.integer(game.requested_speed);
    json.key("unit_limit");
    json.integer(static_cast<int64_t>(game.max_units_setting));
    json.key("clock");
    write_clock(json, game.tick);
    json.key("players");
    json.begin_array();
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
        if (player_seated(game.players[slot]))
            write_match_player(json, *match, slot, shown);
    json.end_array();
    json.key("local_player");
    if (game.local_player_index < OA_PLAYER_COUNT)
        json.integer(game.local_player_index);
    else
        json.null();
    const bool decided = match_decided(game);
    json.key("game_over");
    json.boolean(decided);
    json.key("outcome");
    if (const auto outcome = match_outcome(game))
        json.string(*outcome);
    else
        json.null();
    json.key("winner");
    if (const auto winner = match_winner(game))
        json.string(game_text(game.players[*winner].name, sizeof game.players[*winner].name));
    else
        json.null();
    if (digest) {
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(*digest));
        json.key("state_hash");
        json.string(hex);
    }
}

void write_room(JsonWriter& json, mp::Lobby* lobby, bool in_room) {
    const bool shown = in_room && lobby != nullptr && lobby->game != nullptr;
    json.key("in_room");
    json.boolean(shown);
    if (!shown) {
        for (const std::string_view name : {"session", "host", "host_slot", "local_slot", "map"}) {
            json.key(name);
            json.null();
        }
        json.key("hosting");
        json.boolean(false);
        json.key("players");
        json.begin_array();
        json.end_array();
        json.key("options");
        json.null();
        json.key("chat");
        json.begin_array();
        json.end_array();
        return;
    }
    oa::Game& game = *lobby->game;
    const uint8_t host = mp::lobby_host_slot(*lobby);
    const uint8_t local = game.local_player_index;
    const mp::PlayerSetupInfo* host_info =
        host < OA_PLAYER_COUNT ? mp::slot_info(*lobby, host) : nullptr;
    json.key("session");
    json.string(game_text(mp::lobby_game_name(game), sizeof game.game_name));
    json.key("host");
    if (host < OA_PLAYER_COUNT)
        json.string(game_text(game.players[host].name, sizeof game.players[host].name));
    else
        json.null();
    json.key("host_slot");
    if (host < OA_PLAYER_COUNT)
        json.integer(host);
    else
        json.null();
    json.key("local_slot");
    if (local < OA_PLAYER_COUNT)
        json.integer(local);
    else
        json.null();
    json.key("hosting");
    json.boolean(host < OA_PLAYER_COUNT && host == local);
    json.key("players");
    json.begin_array();
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
        if (mp::slot_active(mp::slot_player(*lobby, slot)))
            write_room_player(json, *lobby, slot);
    json.end_array();
    json.key("map");
    if (host_info != nullptr && host_info->map_name[0] != '\0')
        json.string(game_text(host_info->map_name, sizeof host_info->map_name));
    else
        json.null();
    json.key("options");
    if (host_info != nullptr)
        write_room_options(json, *lobby, *host_info);
    else
        json.null();
    json.key("chat");
    json.begin_array();
    for (uint32_t line = mp::lobby_chat_tail(game) % mp::kChatLines;
         line != mp::lobby_chat_head(game) % mp::kChatLines;
         line = (line + 1) % mp::kChatLines)
        json.string(game_text(mp::lobby_chat_line(game, line), mp::kChatLineBytes));
    json.end_array();
}

} // namespace oa::app::automation
