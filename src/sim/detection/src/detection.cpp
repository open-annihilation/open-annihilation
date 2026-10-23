// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/detection.hpp"

namespace oa::sim::detection {
namespace {

constexpr uint32_t fixed_shift = 16;

int32_t wrap_add(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

int32_t wrap_sub(int32_t a, int32_t b) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

int32_t square_high(int32_t delta) noexcept {
    const auto product = static_cast<int64_t>(delta) * static_cast<int64_t>(delta);
    return static_cast<int32_t>(static_cast<uint64_t>(product) >> 32);
}

int32_t sea_fixed(const World& world) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(world.game.sea_level) << fixed_shift);
}

// An unordered (NaN) fraction also counts as zero.
bool finished(const Unit& unit) noexcept {
    return !(unit.build_remaining < 0.0F) && !(unit.build_remaining > 0.0F);
}

// Owner index through Unit.owner, as the rebuild reads it.
bool owner_index(const World& world, const Unit& unit, uint8_t& index) noexcept {
    const auto* owner = world_unit_owner(&world, &unit);
    if (owner == nullptr)
        return false;
    index = owner->index;
    return true;
}

bool allied(const Player& player, uint8_t other) noexcept {
    return other < sizeof player.alliance && player.alliance[other] != 0;
}

// Whether a unit of the pad list still serves a damaged aircraft at a point.
bool pad_serves(const World& world, uint16_t slot, const FixedVec3& at) noexcept {
    constexpr int32_t reach = repair_pad_reach * repair_pad_reach;
    const auto* pad = world_unit_at(&world, slot);
    if (pad == nullptr || pad->type_index == 0)
        return false;
    const auto* def = world_unit_def_of(&world, pad);
    return def != nullptr && (def->flags & repair_pad_flags) == repair_pad_flags &&
           (pad->state_flags & OA_UNIT_STATE_ACTIVE) != 0 &&
           squared_distance_high(at, pad->position) <= reach;
}
} // namespace

int32_t squared_distance_high(const FixedVec3& a, const FixedVec3& b) noexcept {
    return wrap_add(square_high(wrap_sub(a.z, b.z)), square_high(wrap_sub(a.x, b.x)));
}

bool shares_radar(World& world, const Player& viewer, const Player& owner) noexcept {
    if (viewer.index >= sizeof owner.alliance || owner.alliance[viewer.index] == 0)
        return false;
    const auto* info = world_player_info(&world, &owner);
    return info != nullptr && (info->role & share_radar_role) != 0;
}

void stamp_contact(const ScanRecord& scan, Unit& unit, const World& world) noexcept {
    if (unit.type_index == 0 || unit.owner_index == world.game.viewpoint_player)
        return;
    const auto* def = world_unit_def_of(&world, &unit);
    if (def == nullptr || (def->flags & OA_UNIT_DEF_FLAG_STEALTH) != 0)
        return;
    const auto distance = squared_distance_high(unit.position, scan.position);
    const auto sea = sea_fixed(world);
    if (unit.position.y <= sea && distance < scan.sonar_range_squared)
        unit.flags |= sonar_contact;
    if (sea <= wrap_add(def->model_height, unit.position.y) && distance < scan.radar_range_squared)
        unit.flags |= radar_contact;
}

void stamp_picture(
    const ScanRecord& scan, uint8_t viewer, const Unit& unit, const World& world, uint8_t& bits
) noexcept {
    if (unit.type_index == 0 || unit.owner_index == viewer)
        return;
    const auto* def = world_unit_def_of(&world, &unit);
    if (def == nullptr || (def->flags & OA_UNIT_DEF_FLAG_STEALTH) != 0)
        return;
    const auto distance = squared_distance_high(unit.position, scan.position);
    const auto sea = sea_fixed(world);
    if (unit.position.y <= sea && distance < scan.sonar_range_squared)
        bits = static_cast<uint8_t>(bits | picture_sonar);
    if (sea <= wrap_add(def->model_height, unit.position.y) && distance < scan.radar_range_squared)
        bits = static_cast<uint8_t>(bits | picture_radar);
}

void jam_radar(Unit& unit) noexcept {
    unit.flags = (unit.flags & ~radar_contact) | jammed;
}

void jam_sonar(Unit& unit) noexcept {
    unit.flags = (unit.flags & ~sonar_contact) | jammed;
}

void jam_picture_radar(uint8_t& bits) noexcept {
    bits = static_cast<uint8_t>((bits & ~picture_radar) | picture_jammed);
}

void jam_picture_sonar(uint8_t& bits) noexcept {
    bits = static_cast<uint8_t>((bits & ~picture_sonar) | picture_jammed);
}

bool seat_jammer_jams(
    bool allied_jammers_ignored, const Player& viewer, const Unit& jammer
) noexcept {
    if (!allied_jammers_ignored)
        return true;
    const bool own_or_allied =
        jammer.owner_index == viewer.index ||
        (jammer.owner_index < OA_PLAYER_RECORD_COUNT && viewer.alliance[jammer.owner_index] != 0);
    return !own_or_allied;
}

uint8_t created_picture_bits(const Unit& unit, uint8_t viewer) noexcept {
    if (unit.owner_index == viewer)
        return picture_sonar;
    return static_cast<uint8_t>(0);
}

bool sightings_due(const Sightings& sightings, uint32_t tick) noexcept {
    return sightings.refreshed_tick + sighting_period <= tick;
}

void clear_sightings(Sightings& sightings) noexcept {
    sightings.seen_count = 0;
    sightings.radar_count = 0;
    sightings.pad_count = 0;
    sightings.radar_fallback = 0;
}

bool finished_unit(const Unit& unit) noexcept {
    return unit_is_live_target(unit.flags) && finished(unit);
}

bool sighting_candidate(const World& world, const Player& player, const Unit& unit) noexcept {
    uint8_t owner = 0;
    return unit_is_live_target(unit.flags) && owner_index(world, unit, owner) &&
           !allied(player, owner);
}

bool file_sighting(
    Sightings& sightings, const World& world, const Player& player, const Unit& unit, bool seen
) noexcept {
    uint8_t owner = 0;
    if (!unit_is_live_target(unit.flags) || !owner_index(world, unit, owner))
        return false;
    const auto slot = static_cast<uint16_t>(world_unit_slot(&world, &unit));
    if (!allied(player, owner)) {
        if (seen && (unit.flags & unsighted) == 0 && sightings.seen_count < sightings.capacity)
            sightings.seen[sightings.seen_count++] = slot;
        if ((unit.flags & radar_contact) != 0 && sightings.radar_count < sightings.capacity)
            sightings.radar[sightings.radar_count++] = slot;
        return false;
    }
    if (owner != player.index || !finished_unit(unit))
        return false;
    const auto* def = world_unit_def_of(&world, &unit);
    const bool active = (unit.state_flags & OA_UNIT_STATE_ACTIVE) != 0;
    if (def != nullptr && (def->flags & repair_pad_flags) == repair_pad_flags && active &&
        sightings.pad_count < sightings.capacity)
        sightings.pads[sightings.pad_count++] = slot;
    if (def != nullptr && (def->flags & OA_UNIT_DEF_FLAG_TARGETING_UPGRADE) != 0 && active)
        sightings.radar_fallback = 1;
    return true;
}

uint32_t
pads_in_reach(const Sightings& sightings, const World& world, const FixedVec3& at) noexcept {
    uint32_t count = 0;
    for (uint32_t i = 0; i < sightings.pad_count; ++i)
        if (pad_serves(world, sightings.pads[i], at))
            ++count;
    return count;
}

uint16_t pad_in_reach(
    const Sightings& sightings, const World& world, const FixedVec3& at, uint32_t index
) noexcept {
    for (uint32_t i = 0; i < sightings.pad_count; ++i)
        if (pad_serves(world, sightings.pads[i], at) && index-- == 0)
            return sightings.pads[i];
    return 0;
}

bool sighted_within(
    const Sightings& sightings, const World& world, const FixedVec3& position, int16_t distance
) noexcept {
    const auto reach = static_cast<int32_t>(distance) * static_cast<int32_t>(distance);
    for (uint32_t i = 0; i < sightings.seen_count; ++i) {
        const auto* unit = world_unit_at(&world, sightings.seen[i]);
        if (unit != nullptr && squared_distance_high(position, unit->position) <= reach &&
            unit_is_live_target(unit->flags))
            return true;
    }
    return false;
}

void expose_cloaker(Unit& unit, uint32_t tick) noexcept {
    unit.decloak_until_tick = tick + decloak_hold_ticks;
    unit.flags |= cloak_locked;
}

} // namespace oa::sim::detection
