// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/core/projectile.h"
#include "oa/core/world.h"

#include <cstdint>
#include <span>

// The game state's bounded pool of projectile records: allocation at the live
// count, retirement, cancelling a dead unit's burst spawners, and the
// end-of-tick compaction.
namespace oa::sim::weapon_execution {

inline constexpr uint32_t maximum_projectile_capacity = 32767;

struct ProjectilePoolUsage {
    uint32_t high_water{}; // greatest occupied prefix, including shots retired during this tick
    uint64_t allocation_failures{}; // shots refused because every record was occupied
};

/// Allocates the projectile record at the live count, which grows by one.
///
/// Only the target unit and the retired bit are cleared.
///
/// @param[in,out] world projectile pool
/// @param[in,out] usage optional allocation budget counters
/// @return the record, or null when the pool is full
[[nodiscard]] Projectile*
allocate_projectile(World& world, ProjectilePoolUsage* usage = nullptr) noexcept;

/// Spawns a projectile no unit fired (the meteor storm's), owned by no player.
///
/// Sharing it with the other players is the caller's.
///
/// @param[in,out] world projectile pool
/// @param weapon_def weapon definition reference
/// @param position start point, 16.16 world coordinates
/// @param velocity 16.16 per tick
/// @param[in,out] usage optional allocation budget counters
/// @return the record, or null when the pool is full
Projectile* spawn_free_projectile(
    World& world,
    oa_ref32 weapon_def,
    const FixedVec3& position,
    const FixedVec3& velocity,
    ProjectilePoolUsage* usage = nullptr
) noexcept;

/// Marks a projectile record retired.
///
/// When the camera follows it, the camera keeps its position for the weapon's hold time
/// and stops following.
///
/// @param[in,out] world camera follow state
/// @param[in,out] projectile record to retire
void retire_projectile(World& world, Projectile& projectile) noexcept;

/// Moves the live records down over the retired ones, in order, and shrinks the live count.
///
/// The camera's follow target moves with its record. With scratch covering the live
/// prefix, every interceptor link follows its target and retired targets become null.
/// That path runs in linear time; the caller allocates scratch before simulation ticks.
///
/// @param[in,out] world projectile pool and camera follow state
/// @param scratch old-to-new indices covering the live count; otherwise keeps compatibility
/// @quirk Without scratch, a record before the first retired one keeps its reference even
///        when its target moved, and a reference to a retired record is left as it was.
void compact_projectiles(World& world, std::span<int16_t> scratch = {}) noexcept;

/// Retires and compacts away every burst spawner a unit fired.
///
/// With scratch covering the live prefix, all matching spawners retire before one compaction.
///
/// @param[in,out] world projectile pool
/// @param source unit whose spawners are cancelled
/// @param scratch old-to-new indices passed to compact_projectiles
/// @quirk Without scratch the scan continues at the next index after each compaction, so the
///        record that slid into a cancelled spawner's place is not examined.
void cancel_burst_spawners(World& world, oa_ref32 source, std::span<int16_t> scratch = {}) noexcept;

} // namespace oa::sim::weapon_execution
