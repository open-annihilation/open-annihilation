// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/weapon_execution/interceptor.hpp"

#include "oa/core/weapon_def.h"
#include "oa/base/game_math.hpp"

#include <algorithm>
#include <bit>

namespace oa::sim::weapon_execution {

namespace {

int32_t live_projectile_count(const World& world) noexcept {
    const int32_t count = world.game.projectile_count;
    if (world.projectiles == nullptr || count < 0)
        return 0;
    return std::min(count, static_cast<int32_t>(world_projectile_capacity(&world)));
}

oa_ref32 projectile_ref(const World& world, const Projectile& shot) noexcept {
    return static_cast<oa_ref32>(&shot - world.projectiles) + 1u;
}

} // namespace

int32_t point_distance(const FixedVec3& from, const FixedVec3& to) noexcept {
    const auto delta = [](int32_t a, int32_t b) {
        return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
    };
    const auto length = base::game_math::truncated_length(
        delta(from.x, to.x), delta(from.y, to.y), delta(from.z, to.z)
    );
    return static_cast<int16_t>(static_cast<uint32_t>(length) >> 16);
}

oa_ref32 find_interceptor_target(const World& world, const Unit& unit, uint8_t slot) noexcept {
    if (slot >= OA_UNIT_WEAPON_COUNT)
        return 0;
    const UnitWeapon& weapon = unit.weapons[slot];
    if (weapon.stockpile == 0)
        return 0;
    const WeaponDef* def = world_weapon_def(&world, weapon.def);
    if (def == nullptr)
        return 0;
    const int32_t count = live_projectile_count(world);
    // The coverage square is tested per axis as unsigned |d + coverage| <= 2 * coverage.
    const uint32_t half = static_cast<uint32_t>(def->coverage) << 16;
    const uint32_t full = static_cast<uint32_t>(def->coverage) << 17;
    for (int32_t i = 0; i < count; ++i) {
        const Projectile& shot = world.projectiles[i];
        if (shot.owner_index == unit.owner_index)
            continue;
        const WeaponDef* shot_def = world_weapon_def(&world, shot.def);
        if (shot_def == nullptr || (shot_def->flags & OA_WEAPON_FLAG_TARGETABLE) == 0)
            continue;
        const uint32_t dx =
            static_cast<uint32_t>(unit.position.x) - static_cast<uint32_t>(shot.target.x);
        const uint32_t dz =
            static_cast<uint32_t>(unit.position.z) - static_cast<uint32_t>(shot.target.z);
        if (dx + half > full || dz + half > full)
            continue;
        const oa_ref32 ref = static_cast<oa_ref32>(i) + 1u;
        bool claimed = false;
        for (int32_t j = 0; j < count; ++j) {
            if (world.projectiles[j].intercept_target == ref) {
                claimed = true;
                break;
            }
        }
        if (!claimed)
            return ref;
    }
    return 0;
}

oa_ref32 find_intercept_target_fired_by(
    const World& world, const FixedVec3& target, uint16_t fired_by
) noexcept {
    const int32_t count = live_projectile_count(world);
    for (int32_t i = 0; i < count; ++i) {
        const Projectile& shot = world.projectiles[i];
        if (shot.owner_index == world.game.local_player_index || shot.target.x != target.x ||
            shot.target.z != target.z || shot.target.y != target.y)
            continue;
        const uint32_t slot = oa_unit_slot_from_ref(shot.source);
        if (shot.source != 0 && slot < world.unit_slot_count && world.units[slot].id == fired_by)
            return projectile_ref(world, shot);
    }
    return 0;
}

VerticalLaunchShot plan_vertical_launch_shot(
    const World& world,
    const WeaponDef& weapon,
    const Unit& unit,
    uint8_t slot,
    const FixedVector& muzzle,
    const FixedVector& target,
    uint32_t current_tick
) noexcept {
    VerticalLaunchShot shot;
    if (slot >= OA_UNIT_WEAPON_COUNT)
        return shot;
    const UnitWeapon& weapon_slot = unit.weapons[slot];
    shot.slot_aim = {weapon_slot.aim_heading, weapon_slot.aim_pitch};
    if (weapon_slot.aim_ready == 0)
        return shot;
    const Bearing bearing = bearing_toward(muzzle, target);
    shot.slot_aim = {
        std::bit_cast<int16_t>(bearing.heading), std::bit_cast<int16_t>(bearing.pitch)
    };
    if ((weapon.flags & OA_WEAPON_FLAG_INTERCEPTOR) != 0) {
        shot.intercept_target = find_interceptor_target(world, unit, slot);
        if (shot.intercept_target == 0)
            return shot;
    }
    shot.launch = launch_vertical_projectile(weapon, current_tick);
    shot.fired = true;
    return shot;
}

void init_projectile_record(
    World& world,
    Projectile& record,
    oa_ref32 weapon_def,
    const FixedVec3& muzzle,
    const FixedVec3* target,
    uint32_t burst_tick,
    Unit* source,
    uint16_t query_piece
) noexcept {
    record.def = weapon_def;
    record.position = muzzle;
    record.origin = muzzle;
    if (target != nullptr)
        record.target = *target;
    record.flags = static_cast<uint16_t>(record.flags & ~OA_PROJECTILE_FLAG_BEAM_TAIL);
    record.burst_tick = burst_tick;
    record.burst_remaining = 0;
    record.intercept_target = 0;
    record.created_tick = world.game.tick;
    record.target_unit = 0;
    record.flags = static_cast<uint16_t>(record.flags & ~OA_PROJECTILE_PHASE_MASK);
    if (source == nullptr) {
        record.owner_index = neutral_owner_index;
        record.source = 0;
        return;
    }
    record.source = world_unit_ref(&world, source);
    record.owner_index = source->owner_index;
    if ((source->flags & OA_UNIT_FLAG_BUILDING) != 0 && world.game.follow_unit == record.source &&
        world.projectiles != nullptr)
        world.game.follow_target = projectile_ref(world, record);
    record.query_piece = query_piece;
    source->decloak_until_tick = world.game.tick + fire_decloak_ticks;
}

FixedVec3 projectile_aim_point(
    const World& world, Projectile& shot, SurfaceHeight surface_height, void* context
) noexcept {
    const WeaponDef* def = world_weapon_def(&world, shot.def);
    const bool cruise = def != nullptr && (def->flags & OA_WEAPON_FLAG_CRUISE) != 0;
    if (!cruise) {
        if (shot.intercept_target != 0) {
            const Projectile* target =
                world_projectile(const_cast<World*>(&world), shot.intercept_target);
            if (target != nullptr)
                return target->position;
        }
        if (shot.target_unit != 0) {
            const Unit* target = world_unit(&world, shot.target_unit);
            if (target != nullptr && (target->flags & OA_UNIT_FLAG_LIVE) != 0)
                return target->position;
        }
        return shot.target;
    }
    const int32_t distance = point_distance(shot.position, shot.target);
    shot.origin = shot.target;
    if (distance > cruise_descent_distance) {
        shot.origin.y = static_cast<int32_t>(
            (static_cast<uint32_t>(shot.origin.y) & 0xffffu) |
            (static_cast<uint32_t>(cruise_altitude) << 16)
        );
        return shot.origin;
    }
    shot.origin.y = static_cast<int32_t>(
        static_cast<uint32_t>(surface_height(context, shot.target.x, shot.target.z)) << 16
    );
    return shot.origin;
}

} // namespace oa::sim::weapon_execution
