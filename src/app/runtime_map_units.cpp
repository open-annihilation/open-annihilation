// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The units a skirmish or multiplayer map places from its OTA schema while
// the mod profile turns setup.map-scripted-units on.

#include "oa/app/runtime.hpp"
#include "oa/sim/match_runtime/mission_unit_binding.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace oa::app {
namespace {

namespace map_units = oa::sim::mission_units;

/// Tells whether a player's setup block marks it watching.
///
/// @param world the match world
/// @param player the player
/// @return true for a watcher
bool watching(World& world, Player& player) {
    const auto* info = world_player_info(&world, &player);
    return info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) != 0;
}

/// Tells whether this machine runs a player: any status but none and a remote player's.
///
/// @param player the player
/// @return true for a human or computer player of this machine
bool runs_here(const Player& player) {
    return player.status != OA_PLAYER_STATUS_FREE && player.status != OA_PLAYER_STATUS_MIRRORED;
}

} // namespace

void Runtime::begin_map_units(bool resumed) {
    map_units_ = {};
    const auto* profile = mod_profile();
    if (profile == nullptr || !profile->rules.setup.map_scripted_units.enabled ||
        campaign_mission_ || !match_)
        return;
    const auto* map = match_map_context();
    if (map == nullptr || map->units == nullptr || map->unit_count <= 0)
        return;
    map_units_.active = true;
    map_units_.player_at_position.fill(-1);
    if (!profile->rules.setup.map_scripted_units.timed_spawns)
        return;
    map_units_.timed.resize(static_cast<std::size_t>(map->unit_count));
    map_units_.timed.resize(
        static_cast<std::size_t>(
            map_units::order_timed_map_units(map->units, map->unit_count, map_units_.timed)
        )
    );
    if (resumed) {
        const uint32_t tick = match_->state().game.tick;
        while (map_units_.next_timed < map_units_.timed.size() &&
               map_units::timed_map_unit_due(
                   map->units[map_units_.timed[map_units_.next_timed]], tick
               ))
            ++map_units_.next_timed;
    }
}

bool Runtime::place_map_units(uint8_t player, int32_t start_position) {
    if (!map_units_.active || !match_ || player >= map_units::map_unit_players)
        return false;
    const auto* map = match_map_context();
    if (map == nullptr || map->units == nullptr)
        return false;
    World& world = match_->state();
    auto& owner = world.game.players[player];
    int32_t counted = 0;
    for (auto& other : world.game.players)
        if (other.in_use != 0 && other.status != OA_PLAYER_STATUS_FREE && !watching(world, other))
            ++counted;
    const bool neutral = map_units::has_neutral_map_units(map->units, map->unit_count) &&
                         owner.in_use != 0 && start_position + 1 == counted &&
                         owner.status == OA_PLAYER_STATUS_COMPUTER && !watching(world, owner);
    if (neutral)
        map_units_.neutral_player = player;
    if (start_position >= 0 && start_position < static_cast<int32_t>(map_units::map_unit_players))
        map_units_.player_at_position[static_cast<std::size_t>(start_position)] = player;
    if (!neutral && (!runs_here(owner) || watching(world, owner)))
        return false;
    std::vector<map_units::MapUnitPick> picks(static_cast<std::size_t>(map->unit_count));
    const std::vector<Side> loaded_sides = oa::data::defs::side_records(side_table_);
    picks.resize(
        static_cast<std::size_t>(map_units::pick_start_map_units(
            map->units,
            map->unit_count,
            start_position,
            neutral,
            std::span<const Side>(loaded_sides),
            picks
        ))
    );
    if (picks.empty())
        return false;
    oa::sim::match_runtime::MissionUnitBinding binding{*match_, {}};
    const auto hooks = oa::sim::match_runtime::mission_unit_hooks(binding);
    std::vector<Unit*> created(static_cast<std::size_t>(map->unit_count), nullptr);
    for (const auto& pick : picks) {
        const auto slot = pick.slot_offset != 0
                              ? static_cast<uint16_t>(owner.base_unit_id + pick.slot_offset)
                              : uint16_t{0};
        created[static_cast<std::size_t>(pick.entry)] =
            map_units::create_map_unit(world, map->units[pick.entry], player, slot, hooks);
    }
    map_units::run_map_unit_scripts(
        map->units, map->unit_count, created.data(), map_units_.point, hooks
    );
    if (!binding.failure.empty())
        std::cerr << "map units: " << binding.failure << '\n';
    return true;
}

void Runtime::move_map_unit_computer_last(
    std::array<int32_t, 10>& positions, int32_t& placing, bool fixed_positions
) {
    if (!map_units_.active || !match_ || fixed_positions)
        return;
    const auto* map = match_map_context();
    if (map == nullptr || !map_units::has_neutral_map_units(map->units, map->unit_count))
        return;
    std::array<uint8_t, map_units::map_unit_players> statuses{};
    for (std::size_t i = 0; i < statuses.size(); ++i)
        statuses[i] = match_->state().game.players[i].status;
    map_units::move_computer_last(positions, statuses, placing);
}

bool Runtime::map_places_neutral_units() {
    const auto* profile = mod_profile();
    if (profile == nullptr || !profile->rules.setup.map_scripted_units.enabled)
        return false;
    const auto* map = match_map_context();
    return map != nullptr && map_units::has_neutral_map_units(map->units, map->unit_count);
}

void Runtime::step_timed_map_units() {
    if (!map_units_.active || !match_ || map_units_.next_timed >= map_units_.timed.size())
        return;
    const auto* map = match_map_context();
    if (map == nullptr || map->units == nullptr)
        return;
    World& world = match_->state();
    const uint32_t tick = world.game.tick;
    oa::sim::match_runtime::MissionUnitBinding binding{*match_, {}};
    const auto hooks = oa::sim::match_runtime::mission_unit_hooks(binding);
    while (map_units_.next_timed < map_units_.timed.size()) {
        const auto& entry = map->units[map_units_.timed[map_units_.next_timed]];
        if (!map_units::timed_map_unit_due(entry, tick))
            break;
        ++map_units_.next_timed;
        const int32_t player = map_units::timed_map_unit_player(
            entry, map_units_.player_at_position, map_units_.neutral_player
        );
        if (player < 0)
            continue;
        auto& owner = world.game.players[player];
        if (!runs_here(owner) || watching(world, owner))
            continue;
        (void)map_units::create_map_unit(world, entry, static_cast<uint8_t>(player), 0, hooks);
    }
}

} // namespace oa::app
