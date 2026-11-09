// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Current live unit totals by owner, read from the canonical unit table.
#pragma once

#include "oa/core/world.h"

#include <array>
#include <cstdint>

namespace oa::app::battle_counts {

using Counts = std::array<uint32_t, OA_PLAYER_COUNT>;

/// Counts typed, positive-health live units by their current owner.
///
/// Death-pending units and the reserved slot are excluded. Unfinished units
/// count once they have positive health; completion is not required. Each
/// call reads the current records, including births and ownership changes.
///
/// @param world the canonical unit table
/// @return the current counts; all zero when no unit table is bound
[[nodiscard]] inline Counts count_live_units(const oa::World& world) noexcept {
    Counts counts{};
    if (world.units == nullptr)
        return counts;
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const auto& unit = world.units[slot];
        if (unit.type_index != 0 && oa::unit_is_live_target(unit.flags) && unit.health > 0 &&
            unit.owner_index < counts.size())
            ++counts[unit.owner_index];
    }
    return counts;
}

} // namespace oa::app::battle_counts
