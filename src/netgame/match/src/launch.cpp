// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/match/launch.hpp"

#include "oa/netgame/match/net_match.hpp"
#include "oa/netgame/wire.hpp"
#include "oa/sim/visibility_state.hpp"

#include <algorithm>
#include <cstring>

namespace oa::netgame::match {
namespace {

namespace mp = oa::ui::frontend_multiplayer;

constexpr uint16_t visibility_option_shift = 8; // PlayerSetupInfo.options bits 8..10
constexpr uint8_t visibility_option_bits = 0x07;
constexpr uint8_t role_host = 0x01;
// Game.setup_options bit: the game's options are locked, as a
// launch's lockOptions locks them.
constexpr uint16_t setup_option_locked = 0x0001;

bool slot_live(uint8_t status) {
    return status == OA_PLAYER_STATUS_LOCAL || status == OA_PLAYER_STATUS_COMPUTER ||
           status == OA_PLAYER_STATUS_MIRRORED;
}

/// Tells whether one player's unit range comes before another's in a network game.
///
/// Outside network games the ranges follow slot order instead.
///
/// @param left First player.
/// @param right Second player.
/// @return True when left's player id is smaller, compared unsigned.
bool unit_range_before(const Player& left, const Player& right) noexcept {
    return left.player_id < right.player_id;
}

} // namespace

uint32_t launch_random_seed(uint64_t counter) noexcept {
    return static_cast<uint32_t>(counter >> 32) + static_cast<uint32_t>(counter);
}

int32_t unit_def_id_bits_for_count(uint32_t count) noexcept {
    int32_t bits = 0;
    for (auto remaining = static_cast<int32_t>(count); remaining != 0; remaining >>= 1)
        ++bits;
    return bits;
}

const PlayerSetupInfo* launch_host_info(World* world) noexcept {
    for (auto& player : world->game.players) {
        const auto* info = world_player_info(world, &player);
        if (player.in_use != 0 && info != nullptr && (info->role & role_host) != 0)
            return info;
    }
    return nullptr;
}

bool match_launch_apply(mp::Lobby& lobby, World* world) noexcept {
    if (lobby.game == nullptr || lobby.game->local_player_index >= OA_PLAYER_COUNT)
        return false;
    auto& game = world->game;
    for (uint32_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        const auto& from = mp::slot_player(lobby, static_cast<int32_t>(slot));
        auto& player = game.players[slot];
        const bool live = from.in_use != 0 && slot_live(from.status);
        player.in_use = live ? 1u : 0u;
        player.status = live ? from.status : static_cast<uint8_t>(OA_PLAYER_STATUS_FREE);
        // Free slots sort after every live id when unit ranges are numbered.
        player.player_id = live ? from.player_id : no_player_id;
        player.index = live ? static_cast<uint8_t>(slot) : static_cast<uint8_t>(OA_PLAYER_COUNT);
        player.team = from.team;
        player.machine_group = live ? from.machine_group : 0u;
        player.reject_reason = 0;
        player.last_sim_tick = 0;
        std::memcpy(player.name, from.name, sizeof player.name);
        std::memcpy(player.second_name, from.second_name, sizeof player.second_name);
        std::memcpy(player.alliance, from.alliance, sizeof player.alliance);
        std::memcpy(player.allied_by, from.allied_by, sizeof player.allied_by);
        if (const auto* info = mp::slot_info(lobby, static_cast<int32_t>(slot)))
            std::memcpy(&world->player_info[slot], info, sizeof(PlayerSetupInfo));
        else
            world->player_info[slot] = PlayerSetupInfo{};
        player.info = oa_ref_from_index(slot);
    }
    game.local_player_index = lobby.game->local_player_index;
    game.viewpoint_player = lobby.game->local_player_index;
    // The match counts the players the battle room seated, wherever they sit:
    // an open slot between two players counts for nothing. The start
    // positions were chosen from the slot table (match_layout_player_count).
    game.player_count = mp::lobby_player_count(*lobby.game);
    // Under an active launch the battle room set the lock from the
    // launch's lockOptions, set or clear, and the match plays with it: the
    // in-game menu withholds CONTROL while it is set and the score report
    // says the options are locked. Otherwise the match keeps its own.
    if (mp::lobby_launch_block_active(lobby))
        game.setup_options = static_cast<uint16_t>(
            (game.setup_options & ~setup_option_locked) |
            (lobby.game->setup_options & setup_option_locked)
        );
    mp::note_shared_machines(game);
    if (const auto* host = launch_host_info(world))
        match_apply_host_options(world, *host);
    game.unit_def_id_bits = unit_def_id_bits_for_count(world->unit_def_count);
    game.tick = 0;
    game.session_flags = static_cast<uint8_t>(
        game.session_flags | mp::kNetFlagLive |
        (lobby.game->session_flags & mp::kNetFlagGameStarted)
    );
    return true;
}

void match_apply_host_options(World* world, const PlayerSetupInfo& host) noexcept {
    auto& game = world->game;
    game.session_rules = static_cast<int32_t>(
        (host.options & OA_SETUP_OPTION_COMMANDER_MASK) >> OA_SETUP_OPTION_COMMANDER_SHIFT
    );
    game.visibility_flags = static_cast<uint8_t>(
        (game.visibility_flags & ~visibility_option_bits) |
        ((host.options >> visibility_option_shift) & visibility_option_bits)
    );
}

bool match_slot_watcher(const World* world, uint8_t slot) noexcept {
    if (world == nullptr || slot >= OA_PLAYER_COUNT)
        return false;
    const auto& player = world->game.players[slot];
    const auto* info = player.info != 0 && player.info <= OA_PLAYER_COUNT
                           ? &world->player_info[player.info - 1]
                           : nullptr;
    return player.in_use != 0 && info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) != 0;
}

uint8_t match_first_side_past_table(const World* world) noexcept {
    if (world == nullptr || world->game.side_count == 0)
        return OA_PLAYER_COUNT;
    const auto side_count = world->game.side_count;
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        const auto& player = world->game.players[slot];
        if (player.in_use == 0)
            continue;
        if (player.status != OA_PLAYER_STATUS_LOCAL && player.status != OA_PLAYER_STATUS_COMPUTER)
            continue;
        if (match_slot_watcher(world, slot))
            continue;
        const auto* info = player.info != 0 && player.info <= OA_PLAYER_COUNT
                               ? &world->player_info[player.info - 1]
                               : nullptr;
        if (info != nullptr && info->side >= side_count)
            return slot;
    }
    return OA_PLAYER_COUNT;
}

void match_apply_watcher_view(World* world) noexcept {
    if (match_slot_watcher(world, world->game.local_player_index))
        match_show_whole_map(world);
}

void match_show_whole_map(World* world) noexcept {
    auto& game = world->game;
    constexpr auto shown_everywhere =
        sim::visibility_state::terrain_mapping | sim::visibility_state::update_sight_grid;
    game.visibility_flags = static_cast<uint8_t>(game.visibility_flags & ~shown_everywhere);
}

int32_t match_layout_player_count(const Game& game) noexcept {
    int32_t count = game.player_count;
    for (int32_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
        if (game.slot_table[slot] != 0 && game.slot_table[slot] != no_player_id)
            count = slot + 1;
    return count;
}

void match_assign_unit_ranges(World* world) noexcept {
    const auto per_player = static_cast<uint32_t>(world->game.units_per_player);
    uint8_t order[OA_PLAYER_COUNT];
    for (uint8_t i = 0; i < OA_PLAYER_COUNT; ++i)
        order[i] = i;
    std::stable_sort(order, order + OA_PLAYER_COUNT, [world](uint8_t a, uint8_t b) {
        return unit_range_before(world->game.players[a], world->game.players[b]);
    });
    for (uint32_t rank = 0; rank < OA_PLAYER_COUNT; ++rank) {
        auto& player = world->game.players[order[rank]];
        const auto first = per_player * rank + 1u;
        const auto last = first + per_player - 1u;
        if (per_player == 0 || last >= world->unit_slot_count) {
            player.first_unit = player.last_unit = 0;
            continue;
        }
        player.first_unit = oa_ref_from_index(first);
        player.last_unit = oa_ref_from_index(last);
        player.base_unit_id = world->units[first].id;
        player.last_unit_id = world->units[last].id;
        for (auto slot = first; slot <= last; ++slot) {
            auto& unit = world->units[slot];
            unit.owner = oa_ref_from_index(order[rank]);
            unit.owner_index = player.index;
            unit.squad = -1;
        }
    }
}

} // namespace oa::netgame::match
