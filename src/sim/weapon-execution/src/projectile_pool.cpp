// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/weapon_execution/projectile_pool.hpp"

#include "oa/core/weapon_def.h"
#include "oa/sim/weapon_execution/interceptor.hpp"

#include <algorithm>
#include <limits>

namespace oa::sim::weapon_execution {

namespace {

int32_t live_count(const World& world) noexcept {
    const int32_t count = world.game.projectile_count;
    if (world.projectiles == nullptr || count < 0)
        return 0;
    const auto capacity = static_cast<int32_t>(
        std::min(world_projectile_capacity(&world), maximum_projectile_capacity)
    );
    return std::min(count, capacity);
}

oa_ref32 pool_ref(const World& world, const Projectile& shot) noexcept {
    if (world.projectiles == nullptr || &shot < world.projectiles ||
        &shot >= world.projectiles + world_projectile_capacity(&world))
        return 0;
    return static_cast<oa_ref32>(&shot - world.projectiles) + 1u;
}

} // namespace

Projectile* allocate_projectile(World& world, ProjectilePoolUsage* usage) noexcept {
    const int32_t count = world.game.projectile_count;
    if (world.projectiles == nullptr || count < 0)
        return nullptr;
    const auto capacity = static_cast<int32_t>(
        std::min(world_projectile_capacity(&world), maximum_projectile_capacity)
    );
    if (count >= capacity) {
        if (usage != nullptr && usage->allocation_failures != std::numeric_limits<uint64_t>::max())
            ++usage->allocation_failures;
        return nullptr;
    }
    Projectile& record = world.projectiles[count];
    world.game.projectile_count = count + 1;
    if (usage != nullptr)
        usage->high_water = std::max(usage->high_water, static_cast<uint32_t>(count + 1));
    record.target_unit = 0;
    record.flags = static_cast<uint16_t>(record.flags & ~OA_PROJECTILE_FLAG_RETIRED);
    return &record;
}

Projectile* spawn_free_projectile(
    World& world,
    oa_ref32 weapon_def,
    const FixedVec3& position,
    const FixedVec3& velocity,
    ProjectilePoolUsage* usage
) noexcept {
    Projectile* shot = allocate_projectile(world, usage);
    if (shot == nullptr)
        return nullptr;
    init_projectile_record(
        world, *shot, weapon_def, position, nullptr, world.game.tick, nullptr, 0
    );
    shot->velocity = velocity;
    return shot;
}

void retire_projectile(World& world, Projectile& shot) noexcept {
    const oa_ref32 ref = pool_ref(world, shot);
    if (ref != 0 && world.game.follow_target == ref) {
        world.game.follow_point = shot.position;
        const WeaponDef* def = world_weapon_def(&world, shot.def);
        world.game.follow_point_ticks = def != nullptr ? def->hold_time : int16_t{};
        world.game.follow_target = 0;
    }
    shot.flags = static_cast<uint16_t>(shot.flags | OA_PROJECTILE_FLAG_RETIRED);
}

void compact_projectiles(World& world, std::span<int16_t> scratch) noexcept {
    const int32_t count = live_count(world);
    Projectile* pool = world.projectiles;
    const bool remap_all = scratch.size() >= static_cast<std::size_t>(count) && !scratch.empty();
    int32_t live = 0;
    for (int32_t index = 0; index < count; ++index) {
        Projectile& shot = pool[index];
        shot.compact_index = static_cast<int16_t>(index);
        if ((shot.flags & OA_PROJECTILE_FLAG_RETIRED) != 0) {
            if (remap_all)
                scratch[index] = -1;
            continue;
        }
        if (remap_all)
            scratch[index] = static_cast<int16_t>(live);
        if (index != live) {
            if (world.game.follow_target == oa_ref_from_index(static_cast<uint32_t>(index)))
                world.game.follow_target = oa_ref_from_index(static_cast<uint32_t>(live));
            pool[live] = shot;
        }
        ++live;
    }
    for (int32_t index = 0; index < live; ++index) {
        Projectile& shot = pool[index];
        if (shot.intercept_target == 0 || (!remap_all && shot.compact_index == index))
            continue;
        const uint32_t target = shot.intercept_target - 1u;
        if (remap_all) {
            shot.intercept_target = target < static_cast<uint32_t>(count) && scratch[target] >= 0
                                        ? oa_ref_from_index(static_cast<uint32_t>(scratch[target]))
                                        : 0u;
            continue;
        }
        // The fixed-pool path keeps unmoved links and links to retired targets unchanged.
        for (int32_t candidate = 0; candidate < live; ++candidate) {
            if (static_cast<uint32_t>(pool[candidate].compact_index) != target)
                continue;
            shot.intercept_target = oa_ref_from_index(static_cast<uint32_t>(candidate));
            break;
        }
    }
    if (pool != nullptr)
        world.game.projectile_count = live;
}

void cancel_burst_spawners(World& world, oa_ref32 source, std::span<int16_t> scratch) noexcept {
    for (int32_t index = 0; index < live_count(world); ++index) {
        Projectile& shot = world.projectiles[index];
        if (shot.burst_remaining == 0 || shot.source != source)
            continue;
        retire_projectile(world, shot);
        compact_projectiles(world, scratch);
    }
}

} // namespace oa::sim::weapon_execution
