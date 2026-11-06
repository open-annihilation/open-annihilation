// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/trace.hpp"

#include <bit>
#include <cstdarg>
#include <cstdio>

namespace oa::sim::trace {
namespace {

constexpr uint32_t fold_prime = 0x01000193u;
constexpr uint32_t fold_rotation = 13;

constexpr uint32_t first_weapon_field = static_cast<uint32_t>(UnitField::w0_target_a);
constexpr uint32_t fields_per_weapon = 5;

enum class FieldFormat : uint8_t { unsigned_decimal, signed_decimal, hex };

uint32_t signed_word(int32_t value) noexcept {
    return static_cast<uint32_t>(value);
}

uint32_t float_bits(float value) noexcept {
    return std::bit_cast<uint32_t>(value);
}

const UnitSide& side_of(const UnitSide* sides, uint32_t slot) noexcept {
    static constexpr UnitSide none{};
    return sides != nullptr ? sides[slot] : none;
}

uint32_t unit_slot_of(const World& world, oa_ref32 ref) noexcept {
    return ref != 0u && ref <= world.unit_slot_count ? oa_unit_slot_from_ref(ref) : 0u;
}

// The full unit record's fields: an attached unit gives its carrier's id and
// the carrying piece, a free one its position, attitude and movement speed.
uint32_t fold_tick_unit(
    uint32_t state, const World& world, const Unit& unit, const UnitSide& side
) noexcept {
    state = tick_fold(state, unit.type_index);
    if (unit.type_index == 0)
        return state;
    state = tick_fold(state, signed_word(unit.health));
    state = tick_fold(state, float_bits(unit.build_remaining));
    state = tick_fold(state, unit.state_flags);
    state = tick_fold(state, unit.flags & OA_UNIT_FLAG_OCCUPANCY_MASK);
    state = tick_fold(state, unit.attach_parent != 0 ? 1u : 0u);
    if (unit.attach_parent != 0) {
        const Unit* parent = world_unit(&world, unit.attach_parent);
        state = tick_fold(state, parent != nullptr ? parent->id : 0u);
        return tick_fold(state, signed_word(static_cast<int8_t>(unit.attach_piece)));
    }
    state = tick_fold(state, static_cast<uint32_t>(unit.position.x));
    state = tick_fold(state, static_cast<uint32_t>(unit.position.y));
    state = tick_fold(state, static_cast<uint32_t>(unit.position.z));
    state = tick_fold(state, unit.heading);
    state = tick_fold(state, static_cast<uint16_t>(unit.pitch));
    state = tick_fold(state, static_cast<uint16_t>(unit.bank));
    if (side.has_movement != 0)
        state = tick_fold(state, side.movement_speed);
    return state;
}

// A slot not in use is never written by a session, so only its in_use word
// is folded.
uint32_t fold_tick_players(const Game& game) noexcept {
    uint32_t state = 0;
    for (const Player& player : game.players) {
        state = tick_fold(state, player.in_use);
        if (player.in_use == 0)
            continue;
        state = tick_fold(state, float_bits(player.metal));
        state = tick_fold(state, float_bits(player.energy));
    }
    return state;
}

FieldFormat field_format(UnitField field) noexcept {
    switch (field) {
    case UnitField::health:
    case UnitField::pitch:
    case UnitField::bank:
    case UnitField::piece:
        return FieldFormat::signed_decimal;
    case UnitField::flags:
    case UnitField::state:
    case UnitField::build:
    case UnitField::x:
    case UnitField::y:
    case UnitField::z:
    case UnitField::speed:
        return FieldFormat::hex;
    default:
        break;
    }
    if (static_cast<uint32_t>(field) < first_weapon_field)
        return FieldFormat::unsigned_decimal;
    switch ((static_cast<uint32_t>(field) - first_weapon_field) % fields_per_weapon) {
    case 0:
    case 1:
        return FieldFormat::signed_decimal;
    case 3:
        return FieldFormat::hex;
    default:
        return FieldFormat::unsigned_decimal;
    }
}

uint32_t weapon_field_value(const UnitWeapon& weapon, uint32_t part) noexcept {
    switch (part) {
    case 0:
        return signed_word(weapon.target_a);
    case 1:
        return signed_word(weapon.target_b);
    case 2:
        return weapon.reload;
    case 3:
        return weapon.flags;
    default:
        return weapon.stockpile;
    }
}

void fold_section(SectionDigest& digest, uint32_t value) noexcept {
    digest.value = digest_fold(digest.value, value);
}

void fold_units(TickDigest& digest, const World& world, const UnitSide* sides) noexcept {
    auto& units = digest.sections[static_cast<size_t>(Section::units)];
    auto& weapons = digest.sections[static_cast<size_t>(Section::weapons)];
    auto& orders = digest.sections[static_cast<size_t>(Section::orders)];
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const Unit& unit = world.units[slot];
        if (unit.type_index == 0)
            continue;
        const UnitSide& side = side_of(sides, slot);
        SectionDigest* const per_unit[] = {&units, &weapons, &orders};
        for (SectionDigest* section : per_unit) {
            fold_section(*section, slot);
            ++section->items;
        }
        for (size_t index = 0; index < unit_field_count; ++index) {
            const auto field = static_cast<UnitField>(index);
            fold_section(
                digest.sections[static_cast<size_t>(unit_field_section(field))],
                unit_field_value(world, unit, side, field)
            );
        }
    }
}

void fold_projectiles(SectionDigest& digest, const World& world) noexcept {
    int32_t count = world.projectiles != nullptr ? world.game.projectile_count : 0;
    if (count < 0)
        count = 0;
    const auto capacity = static_cast<int32_t>(world_projectile_capacity(&world));
    if (count > capacity)
        count = capacity;
    for (int32_t index = 0; index < count; ++index) {
        const Projectile& shot = world.projectiles[index];
        fold_section(digest, static_cast<uint32_t>(index));
        fold_section(digest, shot.def);
        const FixedVec3* const vectors[] = {&shot.position, &shot.velocity, &shot.target};
        for (const FixedVec3* vector : vectors) {
            fold_section(digest, static_cast<uint32_t>(vector->x));
            fold_section(digest, static_cast<uint32_t>(vector->y));
            fold_section(digest, static_cast<uint32_t>(vector->z));
        }
        fold_section(digest, shot.heading);
        fold_section(digest, shot.pitch);
        fold_section(digest, static_cast<uint32_t>(shot.speed));
        fold_section(digest, shot.target_unit);
        fold_section(digest, shot.source);
        fold_section(digest, shot.intercept_target);
        fold_section(digest, shot.burst_remaining);
        fold_section(digest, shot.lifetime_tick);
        fold_section(digest, shot.owner_index);
        fold_section(digest, shot.flags);
        ++digest.items;
    }
}

void fold_players(SectionDigest& digest, const Game& game) noexcept {
    for (uint32_t index = 0; index < OA_PLAYER_COUNT; ++index) {
        const Player& player = game.players[index];
        fold_section(digest, index);
        fold_section(digest, player.in_use);
        if (player.in_use == 0)
            continue;
        fold_section(digest, player.status);
        fold_section(digest, player.index);
        fold_section(digest, float_bits(player.energy));
        fold_section(digest, float_bits(player.metal));
        fold_section(digest, float_bits(player.energy_storage));
        fold_section(digest, float_bits(player.metal_storage));
        fold_section(digest, player.unit_count);
        fold_section(digest, player.units_created);
        fold_section(digest, signed_word(player.kills));
        fold_section(digest, signed_word(player.losses));
        ++digest.items;
    }
}

void put_word(uint8_t* out, uint32_t value) noexcept {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

void append(char* out, size_t capacity, size_t& length, const char* format, ...) noexcept {
    va_list arguments;
    va_start(arguments, format);
    char* at = length < capacity ? out + length : nullptr;
    const size_t room = length < capacity ? capacity - length : 0;
    const int written = std::vsnprintf(at, room, format, arguments);
    va_end(arguments);
    if (written > 0)
        length += static_cast<size_t>(written);
}

} // namespace

uint32_t tick_fold(uint32_t state, uint32_t value) noexcept {
    return std::rotl((state ^ value) * fold_prime, fold_rotation);
}

uint64_t digest_fold(uint64_t state, uint32_t value) noexcept {
    for (uint32_t byte = 0; byte < 4; ++byte) {
        state ^= (value >> (byte * 8)) & 0xffu;
        state *= digest_prime;
    }
    return state;
}

TickRecord
sample_tick_record(const World& world, const UnitSide* sides, uint32_t core_random) noexcept {
    TickRecord record{};
    record.tick = world.game.tick;
    record.random = core_random;
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const Unit& unit = world.units[slot];
        const UnitSide& side = side_of(sides, slot);
        if (side.has_movement == 0 || (unit.flags & OA_UNIT_FLAG_LIVE) == 0)
            continue;
        ++record.live_units;
        record.unit_fold = fold_tick_unit(record.unit_fold, world, unit, side);
    }
    record.economy_fold = fold_tick_players(world.game);
    return record;
}

TickDigest tick_digest(
    const World& world,
    const UnitSide* sides,
    const RandomState& random,
    const SectionDigest* rule_state
) noexcept {
    TickDigest digest{};
    digest.tick = world.game.tick;
    for (SectionDigest& section : digest.sections)
        section.value = digest_basis;
    fold_units(digest, world, sides);
    fold_projectiles(digest.sections[static_cast<size_t>(Section::projectiles)], world);
    fold_players(digest.sections[static_cast<size_t>(Section::players)], world.game);
    auto& generators = digest.sections[static_cast<size_t>(Section::random)];
    fold_section(generators, random.core);
    fold_section(generators, random.lcg);
    auto& total = digest.sections[static_cast<size_t>(Section::total)];
    fold_section(total, digest.tick);
    for (size_t index = 1; index < section_count; ++index) {
        const SectionDigest& section = digest.sections[index];
        fold_section(total, static_cast<uint32_t>(section.value));
        fold_section(total, static_cast<uint32_t>(section.value >> 32));
        fold_section(total, section.items);
    }
    if (rule_state != nullptr) {
        fold_section(total, static_cast<uint32_t>(rule_state->value));
        fold_section(total, static_cast<uint32_t>(rule_state->value >> 32));
        fold_section(total, rule_state->items);
    }
    return digest;
}

const char* section_name(Section section) noexcept {
    switch (section) {
    case Section::total:
        return "total";
    case Section::units:
        return "units";
    case Section::weapons:
        return "weapons";
    case Section::orders:
        return "orders";
    case Section::projectiles:
        return "projectiles";
    case Section::players:
        return "players";
    case Section::random:
        return "random";
    case Section::count:
        break;
    }
    return "unknown";
}

const char* unit_field_name(UnitField field) noexcept {
    static constexpr const char* names[unit_field_count] = {
        "type",
        "owner",
        "flags",
        "state",
        "health",
        "build",
        "x",
        "y",
        "z",
        "heading",
        "pitch",
        "bank",
        "parent",
        "piece",
        "movement",
        "speed",
        "order",
        "order_kind",
        "w0_target_a",
        "w0_target_b",
        "w0_reload",
        "w0_flags",
        "w0_stockpile",
        "w1_target_a",
        "w1_target_b",
        "w1_reload",
        "w1_flags",
        "w1_stockpile",
        "w2_target_a",
        "w2_target_b",
        "w2_reload",
        "w2_flags",
        "w2_stockpile",
    };
    const auto index = static_cast<size_t>(field);
    return index < unit_field_count ? names[index] : "unknown";
}

Section unit_field_section(UnitField field) noexcept {
    if (field == UnitField::order || field == UnitField::order_kind)
        return Section::orders;
    return static_cast<uint32_t>(field) < first_weapon_field ? Section::units : Section::weapons;
}

uint32_t unit_field_value(
    const World& world, const Unit& unit, const UnitSide& side, UnitField field
) noexcept {
    switch (field) {
    case UnitField::type:
        return unit.type_index;
    case UnitField::owner:
        return unit.owner_index;
    case UnitField::flags:
        return unit.flags;
    case UnitField::state:
        return unit.state_flags;
    case UnitField::health:
        return signed_word(unit.health);
    case UnitField::build:
        return float_bits(unit.build_remaining);
    case UnitField::x:
        return static_cast<uint32_t>(unit.position.x);
    case UnitField::y:
        return static_cast<uint32_t>(unit.position.y);
    case UnitField::z:
        return static_cast<uint32_t>(unit.position.z);
    case UnitField::heading:
        return unit.heading;
    case UnitField::pitch:
        return signed_word(unit.pitch);
    case UnitField::bank:
        return signed_word(unit.bank);
    case UnitField::parent:
        return unit_slot_of(world, unit.attach_parent);
    case UnitField::piece:
        return signed_word(static_cast<int8_t>(unit.attach_piece));
    case UnitField::movement:
        return side.has_movement;
    case UnitField::speed:
        return side.movement_speed;
    case UnitField::order:
        return side.has_order;
    case UnitField::order_kind:
        return side.order_kind;
    default:
        break;
    }
    const auto index = static_cast<uint32_t>(field) - first_weapon_field;
    if (index >= OA_UNIT_WEAPON_COUNT * fields_per_weapon)
        return 0;
    return weapon_field_value(unit.weapons[index / fields_per_weapon], index % fields_per_weapon);
}

size_t format_unit(
    const World& world,
    uint32_t slot,
    const UnitSide* sides,
    uint32_t tick,
    char* out,
    size_t capacity
) noexcept {
    size_t length = 0;
    append(out, capacity, length, "tick=%u slot=%u", tick, slot);
    if (slot < world.unit_slot_count) {
        const Unit& unit = world.units[slot];
        const UnitSide& side = side_of(sides, slot);
        for (size_t index = 0; index < unit_field_count; ++index) {
            const auto field = static_cast<UnitField>(index);
            const uint32_t value = unit_field_value(world, unit, side, field);
            switch (field_format(field)) {
            case FieldFormat::signed_decimal:
                append(
                    out,
                    capacity,
                    length,
                    " %s=%d",
                    unit_field_name(field),
                    static_cast<int32_t>(value)
                );
                break;
            case FieldFormat::hex:
                append(out, capacity, length, " %s=0x%08x", unit_field_name(field), value);
                break;
            case FieldFormat::unsigned_decimal:
                append(out, capacity, length, " %s=%u", unit_field_name(field), value);
                break;
            }
        }
    }
    append(out, capacity, length, "\n");
    return length;
}

void encode_header(uint8_t (&out)[header_size], uint32_t seed) noexcept {
    put_word(out, stream_magic);
    put_word(out + 4, stream_version);
    put_word(out + 8, static_cast<uint32_t>(record_size));
    put_word(out + 12, seed);
}

void encode_tick_record(uint8_t (&out)[record_size], const TickRecord& record) noexcept {
    put_word(out, static_cast<uint32_t>(RecordKind::tick));
    put_word(out + 4, record.tick);
    put_word(out + 8, record.live_units);
    put_word(out + 12, record.unit_fold);
    put_word(out + 16, record.economy_fold);
    put_word(out + 20, record.random);
}

void encode_section_record(
    uint8_t (&out)[record_size], uint32_t tick, Section section, const SectionDigest& digest
) noexcept {
    put_word(out, static_cast<uint32_t>(RecordKind::section));
    put_word(out + 4, tick);
    put_word(out + 8, static_cast<uint32_t>(section));
    put_word(out + 12, static_cast<uint32_t>(digest.value));
    put_word(out + 16, static_cast<uint32_t>(digest.value >> 32));
    put_word(out + 20, digest.items);
}

} // namespace oa::sim::trace
