// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Match side of the computer-player boundary; the decisions live in src/sim/ai.
#include "tick_internal.hpp"

namespace oa::sim::match_runtime {

const data::unit_definitions::UnitDefinition* Match::unit_definition(uint16_t type) const noexcept {
    return type < input_.fields.size() ? input_.fields[type].definition : nullptr;
}

const sim::combat_state::WeaponDefinition* Match::weapon_definition(uint8_t id) const noexcept {
    return &input_.weapons.definition(id);
}

std::span<const uint16_t> Match::squad_members(uint8_t player, uint32_t squad) const {
    if (player >= groups_.size())
        return {};
    const auto found = groups_[player].find(squad);
    if (found == groups_[player].end())
        return {};
    return found->second;
}

void Match::set_unit_squad(uint16_t unit, uint32_t squad) {
    if (unit == 0 || unit >= slots_.size())
        return;
    assign_squad(slots_[unit], squad);
}

void Match::destroy_player_units(uint8_t owner) {
    TickHost host(*this);
    host.destroy_player_units(owner);
}

bool Match::allied(uint8_t player, uint8_t other) const noexcept {
    if (player >= player_alliances_.size() || other >= 10)
        return false;
    if (!player_alliances_[player])
        return player == other;
    return (*player_alliances_[player])[other] != 0;
}

uint8_t Match::primary_order_command_flags(uint16_t unit) const {
    const auto* head = units_.at(unit).primary;
    const auto* record = runtime_order(head);
    return record ? record->extra.command_flags : uint8_t{0};
}

} // namespace oa::sim::match_runtime
