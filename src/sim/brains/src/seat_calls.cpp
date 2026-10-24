// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/brains/seat_calls.hpp"

#include "oa/sim/match_runtime.hpp"
#include "oa/sim/simulation_state.hpp"
#include "oa/sim/weapon_execution/retaliation.hpp"

#include <cstdint>
#include <exception>

namespace oa::sim::brains {
namespace {

using sim::ai::ComputerHost;
using match_runtime::Match;

ComputerHost* host_of(void* context) noexcept {
    return static_cast<ComputerHost*>(context);
}

int32_t unit_squad_of(void* context, uint16_t slot) {
    const ComputerHost* host = host_of(context);
    if (host == nullptr || host->world == nullptr)
        return -1;
    const oa::Unit* unit = oa::world_unit_at(host->world, slot);
    return unit != nullptr ? unit->squad : -1;
}

bool nearest_enemy_of(
    void* context,
    uint8_t player,
    int32_t x,
    int32_t z,
    bool widened,
    bool skip_submerged,
    uint16_t* out_slot
) {
    ComputerHost* host = host_of(context);
    if (host == nullptr || host->world == nullptr || host->allied == nullptr || out_slot == nullptr)
        return false;
    uint8_t relation[OA_PLAYER_COUNT + 1]{};
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index)
        relation[index] = host->allied(host->context, player, index) ? 1 : 0;
    relation[OA_PLAYER_COUNT] = 1;
    sim::simulation_state::CandidateFilter filter{};
    filter.widened_flags = widened;
    filter.skip_submerged = skip_submerged;
    oa::Unit* unit =
        sim::simulation_state::nearest_candidate_unit(*host->world, relation, x, z, filter);
    if (unit == nullptr)
        return false;
    const uint32_t slot = oa::world_unit_slot(host->world, unit);
    if (slot > 65535u)
        return false;
    *out_slot = static_cast<uint16_t>(slot);
    return true;
}

void write_stance_of(void* context, uint16_t slot, int32_t move, int32_t fire) {
    ComputerHost* host = host_of(context);
    if (host == nullptr || host->world == nullptr)
        return;
    oa::Unit* unit = oa::world_unit_at(host->world, slot);
    if (unit == nullptr)
        return;
    if (move >= 0) {
        unit->flags = (unit->flags & ~OA_UNIT_FLAG_MOVE_ORDER_MASK) |
                      (static_cast<uint32_t>(move) << OA_UNIT_FLAG_MOVE_ORDER_SHIFT);
    }
    if (fire >= 0) {
        unit->flags = (unit->flags & ~OA_UNIT_FLAG_FIRE_ORDER_MASK) |
                      (static_cast<uint32_t>(fire) << OA_UNIT_FLAG_FIRE_ORDER_SHIFT);
    }
}

void section_noop(void*, const char*) {
}

uint8_t kind_zero(void*, uint16_t) {
    return 0;
}

uint8_t match_primary_kind(void* context, uint16_t slot) {
    const ComputerHost* host = host_of(context);
    if (host == nullptr || host->context == nullptr)
        return 0;
    try {
        const auto* head = static_cast<Match*>(host->context)->orders(slot).primary;
        return head != nullptr ? head->kind : static_cast<uint8_t>(0);
    } catch (const std::exception&) {
        return 0;
    }
}

bool match_site_facing(
    void* context, uint16_t type, int32_t cell_x, int32_t cell_z, int32_t facing, bool yard
) {
    const ComputerHost* host = host_of(context);
    if (host == nullptr || host->context == nullptr)
        return false;
    auto& match = *static_cast<Match*>(host->context);
    try {
        if (yard) {
            match_runtime::BuildSiteOptions options;
            options.facing = static_cast<uint8_t>(facing);
            return match.building_site_clear(type, cell_x, cell_z, 0, options);
        }
        return match.site_clear_for(type, cell_x, cell_z, 0, 1, static_cast<uint8_t>(facing));
    } catch (const std::exception&) {
        return false;
    }
}

bool match_site_fogged(
    void* context,
    uint8_t player,
    uint16_t type,
    int32_t cell_x,
    int32_t cell_z,
    int32_t facing,
    bool yard
) {
    (void)yard;
    const ComputerHost* host = host_of(context);
    if (host == nullptr || host->context == nullptr)
        return false;
    auto& match = *static_cast<Match*>(host->context);
    try {
        match_runtime::BuildSiteOptions options;
        options.facing = static_cast<uint8_t>(facing);
        return match.building_site(type, cell_x, cell_z, 0, player, options).has_value();
    } catch (const std::exception&) {
        return false;
    }
}

bool match_attack_queued(void* context, uint16_t slot, uint16_t target) {
    const ComputerHost* host = host_of(context);
    if (host == nullptr || host->context == nullptr)
        return false;
    return static_cast<Match*>(host->context)->issue_attack_command(slot, target, true, nullptr);
}

bool match_build_shaped(
    void* context,
    uint16_t slot,
    uint16_t type,
    const oa::FixedVec3* at,
    bool queued,
    int32_t facing
) {
    const ComputerHost* host = host_of(context);
    if (host == nullptr || host->context == nullptr || at == nullptr)
        return false;
    auto& match = *static_cast<Match*>(host->context);
    const sim::ground_orders::Point point{at->x, at->y, at->z};
    return !match.order_refused(
        match.issue_mobile_build(slot, type, point, queued, static_cast<uint8_t>(facing))
    );
}

bool match_weapon_slot(void* context, uint16_t unit, const oa::FixedVec3* at, uint8_t weapon) {
    const ComputerHost* host = host_of(context);
    if (host == nullptr || host->context == nullptr || at == nullptr)
        return false;
    auto& match = *static_cast<Match*>(host->context);
    const oa::Unit* shooter = oa::world_unit_at(&match.state(), unit);
    return shooter != nullptr && sim::weapon_execution::slot_reaches_point(
                                     match.state(), *shooter, *at, weapon, match.rules_view()
                                 );
}

bool host_site_facing(
    void* context, uint16_t type, int32_t cell_x, int32_t cell_z, int32_t, bool yard
) {
    const ComputerHost* host = host_of(context);
    if (host == nullptr)
        return false;
    if (yard)
        return host->building_site != nullptr &&
               host->building_site(host->context, type, cell_x, cell_z);
    return host->site_clear != nullptr && host->site_clear(host->context, type, cell_x, cell_z);
}

bool host_site_fogged(
    void* context, uint8_t, uint16_t type, int32_t cell_x, int32_t cell_z, int32_t, bool
) {
    const ComputerHost* host = host_of(context);
    if (host == nullptr || host->building_site == nullptr)
        return false;
    return host->building_site(host->context, type, cell_x, cell_z);
}

bool host_attack_queued(void* context, uint16_t slot, uint16_t target) {
    const ComputerHost* host = host_of(context);
    if (host == nullptr || host->order_attack == nullptr)
        return false;
    return host->order_attack(host->context, slot, target);
}

bool host_build_shaped(
    void* context, uint16_t slot, uint16_t type, const oa::FixedVec3* at, bool, int32_t
) {
    const ComputerHost* host = host_of(context);
    if (host == nullptr || host->order_build == nullptr || at == nullptr)
        return false;
    return host->order_build(host->context, slot, type, at);
}

bool host_weapon_slot(void* context, uint16_t unit, const oa::FixedVec3* at, uint8_t weapon) {
    const ComputerHost* host = host_of(context);
    if (host == nullptr || weapon != 0 || host->weapon_reaches == nullptr || at == nullptr)
        return false;
    return host->weapon_reaches(host->context, unit, at);
}

void fill_shared(SeatCalls& calls) noexcept {
    calls.unit_squad = unit_squad_of;
    calls.nearest_enemy = nearest_enemy_of;
    calls.write_stance = write_stance_of;
    calls.section = section_noop;
    calls.refuse = nullptr;
    calls.knowledge = nullptr;
}

const sim::ai::ComputerKnowledge*
match_knowledge(void* context, uint8_t player, uint32_t* refreshed_tick) {
    const ComputerHost* host = host_of(context);
    if (host == nullptr || host->context == nullptr)
        return nullptr;
    try {
        auto& match = *static_cast<Match*>(host->context);
        const uint32_t refreshed = match.sightings(player).refreshed_tick;
        const sim::ai::ComputerKnowledge* knowledge =
            sim::ai::computer_player_knowledge(sim::ai::match_computer_players(match), player);
        if (knowledge == nullptr)
            return nullptr;
        if (refreshed_tick != nullptr)
            *refreshed_tick = refreshed;
        return knowledge;
    } catch (const std::exception&) {
        return nullptr;
    }
}

uint32_t match_builds_held_until(void* context, uint8_t player) {
    if (player >= OA_PLAYER_COUNT)
        return 0;
    const ComputerHost* host = host_of(context);
    if (host == nullptr || host->context == nullptr)
        return 0;
    try {
        auto& match = *static_cast<Match*>(host->context);
        const sim::ai::ComputerPlayers* players = sim::ai::match_computer_players(match);
        if (players == nullptr)
            return 0;
        return players->players[player].commander_build_tick;
    } catch (const std::exception&) {
        return 0;
    }
}

} // namespace

SeatCalls seat_calls_for_match(Match& match) {
    SeatCalls calls;
    calls.host = sim::ai::match_computer_host(match);
    fill_shared(calls);
    calls.primary_kind = match_primary_kind;
    calls.site_facing = match_site_facing;
    calls.site_fogged = match_site_fogged;
    calls.attack_queued = match_attack_queued;
    calls.build_shaped = match_build_shaped;
    calls.weapon_reaches_slot = match_weapon_slot;
    calls.knowledge = match_knowledge;
    calls.builds_held_until = match_builds_held_until;
    return calls;
}

SeatCalls seat_calls_from_computer_host(const ComputerHost& host) {
    SeatCalls calls;
    calls.host = host;
    fill_shared(calls);
    calls.primary_kind = kind_zero;
    calls.site_facing = host_site_facing;
    calls.site_fogged = host_site_fogged;
    calls.attack_queued = host_attack_queued;
    calls.build_shaped = host_build_shaped;
    calls.weapon_reaches_slot = host_weapon_slot;
    return calls;
}

} // namespace oa::sim::brains
