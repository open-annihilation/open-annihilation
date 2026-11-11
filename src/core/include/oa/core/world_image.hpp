// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The tables of a World as bytes: what a keyframe copies out of the world and
// back (docs/replays.md). A table is a block of records the World points at;
// every record in them is canonical and refers to the others by index, so the
// bytes of a table can be put back as they were taken.

#include "oa/core/world.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace oa {

/// One table of a World as a block of bytes.
struct WorldImageTable {
    const char* name{}; ///< short name, at most 31 bytes, which a keyframe names its section by
    void* data{};       ///< the first byte; null for a table the World does not hold
    std::size_t bytes{};
};

/// How many tables world_image_tables lists.
inline constexpr std::size_t world_image_table_count = 12;

/// Lists every block of records a World owns or points at, as bytes.
///
/// The tables are the game state, the unit table, the unit, feature and extra
/// weapon types, the extra sides, the projectile pool, the players' economy
/// and setup blocks, the map's plots and sight grid, and the placed-feature
/// pool. A table the World does not hold has no data and no bytes. The
/// loose fields of World (the table sizes, the map's water damage) are not in
/// any table.
///
/// @param world The world; its tables are not changed.
/// @return The tables, in a fixed order.
[[nodiscard]] inline std::array<WorldImageTable, world_image_table_count>
world_image_tables(World* world) noexcept {
    const auto cells =
        static_cast<std::size_t>(world->game.map_width > 0 ? world->game.map_width : 0) *
        static_cast<std::size_t>(world->game.map_height > 0 ? world->game.map_height : 0);
    std::array<WorldImageTable, world_image_table_count> tables{{
        {"game", &world->game, sizeof world->game},
        {"units", world->units, static_cast<std::size_t>(world->unit_slot_count) * sizeof(Unit)},
        {"unit_types",
         world->unit_defs,
         static_cast<std::size_t>(world->unit_def_count) * sizeof(UnitDef)},
        {"feature_types",
         world->feature_defs,
         static_cast<std::size_t>(world->feature_def_count) * sizeof(FeatureDef)},
        {"extra_weapons",
         world->extra_weapon_defs,
         static_cast<std::size_t>(world->extra_weapon_def_count) * sizeof(WeaponDef)},
        {"extra_sides",
         world->extra_sides,
         static_cast<std::size_t>(world->extra_side_count) * sizeof(Side)},
        {"projectiles", world->projectiles, OA_PROJECTILE_CAPACITY * sizeof(Projectile)},
        {"economy", world->player_economy, sizeof world->player_economy},
        {"player_setup", world->player_info, sizeof world->player_info},
        {"plots", world->plots, cells * sizeof(MapPlot)},
        {"sight_grid", world->sight_grid, cells * sizeof(uint16_t)},
        {"placed_features",
         world->placed_features,
         static_cast<std::size_t>(world->placed_feature_count) * OA_PLACED_FEATURE_BYTES},
    }};
    // A table the World does not hold has no bytes, whatever its count says.
    for (auto& table : tables)
        if (table.data == nullptr)
            table.bytes = 0;
    return tables;
}

} // namespace oa
