// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/unit_script.hpp"

#include <bit>
#include <cmath>

namespace oa::sim::unit_script {
namespace {

uint32_t bit(uint8_t flags, uint32_t shift) {
    return (static_cast<uint32_t>(flags) >> shift) & 1U;
}

// COB packs a map position as x high word, z low word (both 16.16 high words).
// The unpack rounds x up by one unit when the z half is negative.
struct PackedXz {
    int32_t x{};
    int32_t z{};
};

PackedXz unpack_xz(uint32_t packed) {
    auto x = packed & 0xffff0000U;
    const auto z = packed << 16;
    if (std::bit_cast<int32_t>(z) < 0)
        x += 0x10000U;
    return {std::bit_cast<int32_t>(x), std::bit_cast<int32_t>(z)};
}

uint32_t pack_xz(int32_t x, int32_t z) {
    return static_cast<uint32_t>(z >> 16) + (std::bit_cast<uint32_t>(x) & 0xffff0000U);
}

// A unit referenced by id: its slot must hold a live unit.
const Unit* live_unit(const World* world, uint32_t id) {
    const auto slot = id & 0xffffU;
    if (slot == 0)
        return nullptr;
    const Unit* unit = world_unit_at(world, slot);
    return unit != nullptr && (unit->flags & OA_UNIT_FLAG_LIVE) != 0 ? unit : nullptr;
}

// BUILD_PERCENT_LEFT of a unit: 1 - trunc(remaining * -99), 0 when finished,
// else 1..100.
uint32_t build_percent_left(const Unit& unit) {
    if (unit.build_remaining == 0.0F)
        return 0;
    const double scaled = static_cast<double>(unit.build_remaining) * -99.0;
    if (std::isfinite(scaled) && scaled > -2147483648.0 && scaled < 2147483648.0)
        return 1U - static_cast<uint32_t>(static_cast<int32_t>(scaled));
    return 0;
}

using data::match_rules::ScriptExtension;
using data::match_rules::ScriptFidelity;

// Bytes of one unit record in the table the extensions index, as their
// bounds check measures it.
constexpr uint32_t unit_record_bytes = 0x118;

// The highest unit id unit.max-id reports: the unit limit the game
// recorded times 10, or the active limit's when the world records none.
// Enhanced Mode uses only the participating players' allocated ranges.
uint32_t highest_unit_id(const World* world, bool enhanced) {
    if (enhanced)
        return world->unit_slot_count != 0 ? world->unit_slot_count - 1U : 0;
    const uint32_t limit = world->game.max_units_setting != 0 ? world->game.max_units_setting
                                                              : world->game.units_per_player;
    return limit * 10U;
}

// The slot an extension's unit-id argument names, by its low 16 bits.
// Under exact fidelity any slot inside the table up to the highest unit id
// is read, live or not; under safe fidelity only a live unit is.
const Unit*
extension_slot(const World* world, uint32_t argument, ScriptFidelity fidelity, bool enhanced) {
    const uint32_t slot = argument & 0xffffU;
    if (fidelity == ScriptFidelity::safe)
        return live_unit(world, slot);
    return slot <= highest_unit_id(world, enhanced) ? world_unit_at(world, slot) : nullptr;
}

// The slot unit.build-percent-left-of reads: the whole argument times the
// record size, wrapped to 32 bits, must fall on a record before the table's
// last one. Under safe fidelity the argument must name that slot directly
// and a live unit must be there.
const Unit* build_percent_slot(const World* world, uint32_t argument, ScriptFidelity fidelity) {
    if (world->unit_slot_count < 2)
        return nullptr;
    const uint32_t records = world->unit_slot_count - 1U;
    if (fidelity == ScriptFidelity::safe)
        return argument < records ? live_unit(world, argument) : nullptr;
    const uint32_t offset = argument * unit_record_bytes;
    if (offset / unit_record_bytes >= records || offset % unit_record_bytes != 0)
        return nullptr;
    return world_unit_at(world, offset / unit_record_bytes);
}

// The player record of an owner index, or null for one of 10 or more.
const Player* owner_player(const World* world, uint32_t owner_index) {
    return owner_index < OA_PLAYER_COUNT ? &world->game.players[owner_index] : nullptr;
}

int32_t extension_value(
    const World* world,
    const Unit* unit,
    ScriptExtension extension,
    uint32_t argument,
    ScriptFidelity fidelity,
    bool enhanced
) {
    uint32_t value = 0;
    switch (extension) {
    case ScriptExtension::none:
        break;
    case ScriptExtension::unit_kills_x100:
        value = static_cast<uint32_t>(unit->veteran_level) * 100U;
        break;
    case ScriptExtension::unit_min_id:
        value = 1;
        break;
    case ScriptExtension::unit_max_id:
        value = highest_unit_id(world, enhanced);
        break;
    case ScriptExtension::unit_my_id:
        value = unit->id;
        break;
    case ScriptExtension::unit_owner_of:
        if (const Unit* target = extension_slot(world, argument, fidelity, enhanced))
            value = target->owner_index;
        break;
    case ScriptExtension::unit_build_percent_left_of:
        if (const Unit* target = build_percent_slot(world, argument, fidelity))
            value = build_percent_left(*target);
        break;
    case ScriptExtension::unit_allied_with: {
        const Unit* target = extension_slot(world, argument, fidelity, enhanced);
        const Player* caller_owner = owner_player(world, unit->owner_index);
        if (target != nullptr && caller_owner != nullptr && target->owner_index < OA_PLAYER_COUNT)
            value = caller_owner->alliance[target->owner_index] != 0 ? 1U : 0U;
        break;
    }
    case ScriptExtension::unit_is_local:
        if (const Unit* target = extension_slot(world, argument, fidelity, enhanced))
            if (const Player* owner = owner_player(world, target->owner_index))
                value = owner->status == OA_PLAYER_STATUS_LOCAL ||
                                owner->status == OA_PLAYER_STATUS_COMPUTER
                            ? 1U
                            : 0U;
        break;
    }
    return std::bit_cast<int32_t>(value);
}

} // namespace

int32_t unit_script_get_value(
    World* world,
    Unit* unit,
    int32_t selector,
    int32_t first,
    int32_t second,
    const UnitValueServices& services
) {
    const auto argument = std::bit_cast<uint32_t>(first);
    uint32_t value = 0;
    switch (static_cast<UnitValue>(selector)) {
    case UnitValue::activation:
        value = bit(unit->state_flags, 0);
        break;
    case UnitValue::standing_move_orders:
        value = (unit->flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT;
        break;
    case UnitValue::standing_fire_orders:
        value = (unit->flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) >> OA_UNIT_FLAG_FIRE_ORDER_SHIFT;
        break;
    case UnitValue::health: {
        const UnitDef* def = world_unit_def_of(world, unit);
        if (def != nullptr && def->max_damage != 0)
            value =
                static_cast<uint32_t>(static_cast<int32_t>(unit->health) * 100) / def->max_damage;
        break;
    }
    case UnitValue::in_build_stance:
        value = bit(unit->build_flags, 0);
        break;
    case UnitValue::busy:
        value = bit(unit->build_flags, 1);
        break;
    case UnitValue::piece_xz: {
        const auto at = services.piece_world(services.context, world, unit, argument);
        value = pack_xz(at.x, at.z);
        break;
    }
    case UnitValue::piece_y: {
        const oa_fixed height = services.piece_world(services.context, world, unit, argument).y;
        value = std::bit_cast<uint32_t>(height);
        break;
    }
    case UnitValue::unit_xz:
        if (const Unit* target = live_unit(world, argument))
            value = pack_xz(target->position.x, target->position.z);
        break;
    case UnitValue::unit_y:
        if (const Unit* target = live_unit(world, argument)) {
            const oa_fixed height = target->position.y;
            value = std::bit_cast<uint32_t>(height);
        }
        break;
    case UnitValue::unit_height:
        if (const Unit* target = live_unit(world, argument))
            if (const UnitDef* def = world_unit_def_of(world, target)) {
                const oa_fixed model_height = def->model_height;
                value = std::bit_cast<uint32_t>(model_height);
            }
        break;
    case UnitValue::xz_atan: {
        const auto at = unpack_xz(argument);
        const auto heading = services.direction(services.context, at.x, at.z);
        value = static_cast<uint16_t>(heading - unit->heading);
        break;
    }
    case UnitValue::xz_hypot: {
        const auto at = unpack_xz(argument);
        value = services.distance(services.context, at.x, at.z);
        break;
    }
    case UnitValue::atan:
        value = services.direction(services.context, first, second);
        break;
    case UnitValue::hypot:
        value = services.distance(services.context, first, second);
        break;
    case UnitValue::ground_height: {
        const auto at = unpack_xz(argument);
        value = std::bit_cast<uint32_t>(services.ground_height(services.context, world, at.x, at.z))
                << 16;
        break;
    }
    case UnitValue::build_percent_left:
        value = build_percent_left(*unit);
        break;
    case UnitValue::yard_open:
        value = bit(unit->build_flags, 2);
        break;
    case UnitValue::bugger_off:
        value = bit(unit->build_flags, 3);
        break;
    case UnitValue::armored:
        value = bit(unit->state_flags, 1);
        break;
    default: {
        const auto& rules = services.rules.rules();
        if (rules.script_get.count != 0 && selector >= 0 && selector <= 0xffff)
            return extension_value(
                world,
                unit,
                rules.script_get.find(static_cast<uint16_t>(selector)),
                argument,
                rules.script_fidelity,
                services.features.has(data::enhanced_mode::Feature::capacity)
            );
        break;
    }
    }
    return std::bit_cast<int32_t>(value);
}

void unit_script_set_value(
    World* world, Unit* unit, int32_t selector, int32_t value, const UnitValueServices& services
) {
    const auto low_bit = static_cast<uint8_t>(std::bit_cast<uint32_t>(value) & 1U);
    switch (static_cast<UnitValue>(selector)) {
    case UnitValue::activation:
        services.set_state_flag(services.context, world, unit, state_flag_activated, value != 0);
        break;
    case UnitValue::in_build_stance:
        unit->build_flags =
            static_cast<uint8_t>((unit->build_flags & ~build_flag_in_build_stance) | low_bit);
        break;
    case UnitValue::busy:
        unit->build_flags =
            static_cast<uint8_t>((unit->build_flags & ~build_flag_busy) | (low_bit << 1));
        break;
    case UnitValue::yard_open:
        services.set_yard_open(services.context, world, unit, value);
        break;
    case UnitValue::bugger_off:
        unit->build_flags =
            static_cast<uint8_t>((unit->build_flags & ~build_flag_bugger_off) | (low_bit << 3));
        break;
    case UnitValue::armored:
        services.set_state_flag(services.context, world, unit, state_flag_armored, value != 0);
        break;
    default:
        break;
    }
    unit->events = static_cast<uint16_t>(unit->events | event_script_state_changed);
}

} // namespace oa::sim::unit_script
