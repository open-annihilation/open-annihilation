// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/persist/save_sections.hpp"

#include "bank_util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace oa::data::persist {
namespace {

using detail::load_le32;
using detail::store_le16;
using detail::store_le32;

constexpr const char* version_field = "Version";

/// Tests whether a unit slot is live, OA_UNIT_FLAG_LIVE (bit 28) of Unit.flags.
///
/// @param unit unit slot, or null
/// @return true for a live slot; false for a free slot or null
bool unit_live(const Unit* unit) {
    return unit != nullptr && (unit->flags & OA_UNIT_FLAG_LIVE) != 0;
}

const void* resolve_bytes(const SaveContext* save, SaveRef kind, oa_ref32 ref) {
    const SaveHooks* hooks = save->hooks;
    return ref != 0 ? hooks->resolve(hooks->context, kind, ref) : nullptr;
}

// Bits 17..19 carry no unit data and are written as zero.
uint32_t pack_unit_flags(const Unit* unit) {
    namespace f = unit_record_flags;
    const uint32_t flags = unit->flags;
    return ((flags & f::high) << f::high_shift) |
           ((flags & f::construction_dirty) << f::construction_dirty_shift) |
           ((flags & f::low) << f::low_shift) | (unit->build_flags & f::build_nibble);
}

void encode_weapon(const SaveContext* save, const Unit& unit, uint32_t slot, uint8_t* out) {
    namespace w = weapon_record;
    const UnitWeapon& weapon = unit.weapons[slot];
    store_le16(out + w::target_a, static_cast<uint16_t>(weapon.target_a));
    store_le16(out + w::target_b, static_cast<uint16_t>(weapon.target_b));
    store_le32(out + w::aim_ready, weapon.aim_ready);
    const WeaponDef* def = world_weapon_def(save->world, weapon.def);
    store_le32(out + w::weapon_id, def != nullptr ? oa_weapon_def_id(def) : 0u);
    std::memcpy(out + w::muzzle_offset, weapon.muzzle_offset, sizeof(weapon.muzzle_offset));
    store_le16(out + w::reload, weapon.reload);
    store_le16(out + w::aim_heading, static_cast<uint16_t>(weapon.aim_heading));
    store_le16(out + w::aim_pitch, static_cast<uint16_t>(weapon.aim_pitch));
    out[w::stockpile] = saved_stockpile(*save->world, unit, slot);
    out[w::flags] = static_cast<uint8_t>(weapon.flags & w::saved_flags);
}

} // namespace

uint8_t saved_stockpile(const World& world, const Unit& unit, uint32_t slot) noexcept {
    if (slot >= OA_UNIT_WEAPON_COUNT)
        return 0;
    const UnitWeapon& weapon = unit.weapons[slot];
    const uint8_t held = weapon.stockpile;
    const WeaponDef* def = world_weapon_def(&world, weapon.def);
    if (def == nullptr || (def->flags & OA_WEAPON_FLAG_STOCKPILE) == 0)
        return held;
    for (uint32_t earlier = 0; earlier < slot; ++earlier) {
        if (unit.weapons[earlier].def == weapon.def)
            return held;
    }
    if (world.projectiles == nullptr || world.game.projectile_count <= 0)
        return held;

    const oa_ref32 source = world_unit_ref(&world, &unit);
    const int32_t live_count = world.game.projectile_count < OA_PROJECTILE_CAPACITY
                                   ? world.game.projectile_count
                                   : OA_PROJECTILE_CAPACITY;

    struct InFlightRound {
        oa_ref32 def;
        oa_ref32 source;
        uint32_t created_tick;
    };

    std::array<InFlightRound, OA_PROJECTILE_CAPACITY> rounds{};
    uint32_t round_count = 0;
    for (int32_t index = 0; index < live_count; ++index) {
        const Projectile& shot = world.projectiles[index];
        if ((shot.flags & OA_PROJECTILE_FLAG_RETIRED) != 0)
            continue;
        if (shot.def != weapon.def || shot.source != source)
            continue;
        bool already = false;
        for (uint32_t seen = 0; seen < round_count; ++seen) {
            if (rounds[seen].def == shot.def && rounds[seen].source == shot.source &&
                rounds[seen].created_tick == shot.created_tick) {
                already = true;
                break;
            }
        }
        if (already)
            continue;
        rounds[round_count] = InFlightRound{shot.def, shot.source, shot.created_tick};
        ++round_count;
    }

    const uint32_t total = static_cast<uint32_t>(held) + round_count;
    if (total <= held)
        return held;
    if (total > saved_stockpile_limit)
        return held > saved_stockpile_limit ? held : saved_stockpile_limit;
    return static_cast<uint8_t>(total);
}

void save_restore_unit_weapon(
    World* world, const uint8_t* in, UnitWeapon& weapon, WeaponIdWidth width
) {
    namespace w = weapon_record;
    weapon.target_a = static_cast<int16_t>(detail::load_le16(in + w::target_a));
    weapon.target_b = static_cast<int16_t>(detail::load_le16(in + w::target_b));
    weapon.aim_ready = load_le32(in + w::aim_ready);
    if (WeaponDef* def = world_weapon_def(world, weapon.def); def != nullptr) {
        if (width == WeaponIdWidth::wide) {
            const auto id = static_cast<uint16_t>(
                static_cast<uint16_t>(in[w::weapon_id]) |
                (static_cast<uint16_t>(in[w::weapon_id + 1]) << 8)
            );
            oa_weapon_def_set_id(def, id);
        } else {
            def->weapon_id = in[w::weapon_id];
        }
    }
    std::memcpy(weapon.muzzle_offset, in + w::muzzle_offset, sizeof(weapon.muzzle_offset));
    weapon.reload = detail::load_le16(in + w::reload);
    weapon.aim_heading = static_cast<int16_t>(detail::load_le16(in + w::aim_heading));
    weapon.aim_pitch = static_cast<int16_t>(detail::load_le16(in + w::aim_pitch));
    weapon.stockpile = in[w::stockpile];
    weapon.flags =
        static_cast<uint8_t>((weapon.flags & ~w::saved_flags) | (in[w::flags] & w::saved_flags));
}

void save_write_unit_economy(const Unit* unit, Bank* bank) {
    char name[save_name_bytes];
    std::snprintf(name, sizeof(name), save_key::economy_format, unit->id);
    bank_open_blob_name(bank, name);
    bank_blob_seek(bank, 0);
    bank_blob_write(bank, &unit->economy.energy, sizeof(ResourceAccumulator));
    bank_blob_write(bank, &unit->economy.metal, sizeof(ResourceAccumulator));
}

bool save_read_unit_economy(Unit* unit, Bank* bank) {
    char name[save_name_bytes];
    std::snprintf(name, sizeof(name), save_key::economy_format, unit->id);
    if (!bank_open_blob_name(bank, name))
        return false;
    bank_blob_seek(bank, 0);
    bool complete = true;
    for (ResourceAccumulator* accumulator : {&unit->economy.energy, &unit->economy.metal}) {
        float* fields[] = {
            &accumulator->produced,
            &accumulator->requested,
            &accumulator->accepted,
            &accumulator->gate,
            &accumulator->last_produced,
            &accumulator->last_requested
        };
        static_assert(
            sizeof(fields) / sizeof(fields[0]) * sizeof(float) == sizeof(ResourceAccumulator)
        );
        // A short read overwrites only the bytes it returns, as in 3.1c.
        uint8_t image[sizeof(ResourceAccumulator)];
        for (std::size_t i = 0; i < std::size(fields); ++i) {
            uint32_t bits = 0;
            std::memcpy(&bits, fields[i], sizeof(float));
            store_le32(image + i * sizeof(float), bits);
        }
        if (bank_blob_read(bank, image, sizeof(image)) != sizeof(image))
            complete = false;
        for (std::size_t i = 0; i < std::size(fields); ++i) {
            const uint32_t bits = load_le32(image + i * sizeof(float));
            std::memcpy(fields[i], &bits, sizeof(float));
        }
    }
    return complete;
}

void save_write_unit_mobility(const uint8_t* movement, const Unit* unit, Bank* bank) {
    uint8_t blob[mobility_blob_bytes] = {};
    std::memcpy(blob, movement + movement_saved_offset, movement_saved_bytes);
    // Only the low three flag bits are defined; the rest is written as zero.
    blob[movement_saved_bytes] = static_cast<uint8_t>(movement[movement_flags] & 0x07);
    char name[save_name_bytes];
    std::snprintf(name, sizeof(name), save_key::mobility_format, unit->id);
    bank_open_blob_name(bank, name);
    bank_blob_seek(bank, 0);
    bank_blob_write(bank, blob, mobility_blob_bytes);
}

void save_encode_unit_record(
    const SaveContext* save,
    const Unit* unit,
    int32_t order_count,
    bool has_movement,
    uint8_t* record
) {
    namespace r = unit_record;
    std::memset(record, 0, unit_record_bytes);
    const auto* def = world_unit_def_of(save->world, unit);
    if (def != nullptr)
        std::memcpy(
            record + r::type_name,
            def->unit_name,
            strnlen(def->unit_name, sizeof(def->unit_name) - 1)
        );
    record[r::owner] = unit->owner_index;
    store_le16(record + r::id, unit->id);
    store_le32(record + r::order_count, static_cast<uint32_t>(order_count));
    store_le32(record + r::has_movement, has_movement ? 1u : 0u);
    store_le32(
        record + r::position + offsetof(FixedVec3, x), static_cast<uint32_t>(unit->position.x)
    );
    store_le32(
        record + r::position + offsetof(FixedVec3, y), static_cast<uint32_t>(unit->position.y)
    );
    store_le32(
        record + r::position + offsetof(FixedVec3, z), static_cast<uint32_t>(unit->position.z)
    );
    store_le16(record + r::bank, static_cast<uint16_t>(unit->bank));
    store_le16(record + r::heading, unit->heading);
    store_le16(record + r::pitch, static_cast<uint16_t>(unit->pitch));
    store_le16(record + r::health, static_cast<uint16_t>(unit->health));
    store_le16(record + r::veteran_level, unit->veteran_level);
    for (std::size_t i = 0; i < OA_UNIT_WEAPON_COUNT; ++i)
        encode_weapon(
            save, *unit, static_cast<uint32_t>(i), record + r::weapons + i * r::weapon_bytes
        );
    const auto* carrier = world_unit(save->world, unit->attach_parent);
    if (unit_live(carrier)) {
        store_le16(record + r::carrier_id, carrier->id);
        record[r::carrier_slot] = unit->attach_piece;
    } else {
        record[r::carrier_slot] = unit_record_no_carrier;
    }
    // Unit.last_attacker_id holds a unit id, not a ref.
    const auto* linked = world_unit_at(save->world, unit->last_attacker_id);
    if (unit_live(linked))
        store_le16(record + r::linked_id, linked->id);
    record[r::last_attacker_owner] = unit->last_attacker_owner;
    uint32_t extracted = 0;
    std::memcpy(&extracted, &unit->extracted_metal, sizeof(extracted));
    store_le32(record + r::extracted_metal, extracted);
    store_le16(record + r::cell_x, static_cast<uint16_t>(unit->cell_x));
    store_le16(record + r::cell_z, static_cast<uint16_t>(unit->cell_z));
    store_le16(record + r::sight_center_x, unit->sight_center_x);
    store_le16(record + r::sight_center_z, unit->sight_center_z);
    store_le16(record + r::footprint_x, static_cast<uint16_t>(unit->footprint_x));
    store_le16(record + r::footprint_z, static_cast<uint16_t>(unit->footprint_z));
    store_le32(record + r::squad, static_cast<uint32_t>(unit->squad));
    store_le32(record + r::decloak_until_tick, unit->decloak_until_tick);
    uint32_t remaining = 0;
    std::memcpy(&remaining, &unit->build_remaining, sizeof(remaining));
    store_le32(record + r::build_remaining, remaining);
    record[r::damage_kind] = unit->damage_kind;
    record[r::health_percent] = unit->health_percent;
    record[r::previous_health_percent] = unit->previous_health_percent;
    store_le16(record + r::events, unit->events);
    record[r::sight_band] = unit->sight_band;
    record[r::damage_countdown] = unit->damage_countdown;
    store_le16(record + r::state_flags, unit->state_flags);
    store_le32(record + r::flags, pack_unit_flags(unit));
}

void save_write_units(const SaveContext* save, Bank* bank) {
    const SaveHooks* hooks = save->hooks;
    bank_open_account(bank, save_key::units);
    int32_t saved = 0;
    for (uint32_t slot = 0; slot < save->world->unit_slot_count; ++slot) {
        Unit* unit = &save->world->units[slot];
        if (!unit_live(unit))
            continue;
        char name[save_name_bytes];
        std::snprintf(name, sizeof(name), save_key::script_format, saved);
        bank_open_blob_name(bank, name);
        hooks->write_script(hooks->context, unit, bank);

        // Every order is counted, whether or not its blob was written.
        struct OrderWalk {
            const SaveContext* save;
            const Unit* unit;
            Bank* bank;
            int32_t count;
        } walk{save, unit, bank, 0};

        hooks->visit_orders(
            hooks->context,
            unit,
            [](void* context, const SavedOrder* order, const SavedGoal* goal) {
                auto* w = static_cast<OrderWalk*>(context);
                char blob_name[save_name_bytes];
                std::snprintf(
                    blob_name,
                    sizeof(blob_name),
                    save_key::order_format,
                    w->unit->id,
                    static_cast<unsigned>(w->count)
                );
                (void)save_write_order(w->save->world, w->unit, order, goal, w->bank, blob_name);
                ++w->count;
            },
            &walk
        );
        const int32_t orders = walk.count;
        const auto* movement =
            static_cast<const uint8_t*>(resolve_bytes(save, SaveRef::movement, unit->movement));
        if (movement != nullptr)
            save_write_unit_mobility(movement, unit, bank);
        save_write_unit_economy(unit, bank);
        uint8_t record[unit_record_bytes];
        save_encode_unit_record(save, unit, orders, movement != nullptr, record);
        bank_open_blob_id(bank, saved);
        bank_blob_write(bank, record, unit_record_bytes);
        ++saved;
    }
    if (saved > 0) {
        bank_set_int(bank, save_key::unit_count, saved);
        bank_set_int(bank, version_field, unit_snapshot_version);
    }
}

int32_t save_unit_record_ids(const Bank* bank, int32_t count, int32_t** ids) {
    *ids = nullptr;
    const BankAccounts* accounts = bank->accounts;
    if (count <= 0 || accounts == nullptr || accounts->open < 0 ||
        accounts->open >= accounts->count)
        return 0;
    const BankAccount& account = accounts->items[accounts->open];
    if (account.blob_count <= 0)
        return 0;
    auto* found = static_cast<int32_t*>(
        std::malloc(static_cast<std::size_t>(account.blob_count) * sizeof(int32_t))
    );
    if (found == nullptr)
        return 0;
    int32_t stored = 0;
    for (int32_t i = 0; i < account.blob_count; ++i) {
        const BankBlob& blob = account.blobs[i];
        if (blob.named == 0 && blob.id >= 0 && blob.id < count)
            found[stored++] = blob.id;
    }
    std::sort(found, found + stored);
    stored = static_cast<int32_t>(std::unique(found, found + stored) - found);
    if (stored == 0) {
        std::free(found);
        return 0;
    }
    *ids = found;
    return stored;
}

void save_find_unit_records(Bank* bank, std::span<int32_t> records) {
    std::fill(records.begin(), records.end(), -1);
    bank_open_account(bank, save_key::units);
    int32_t* ids = nullptr;
    const int32_t stored =
        save_unit_record_ids(bank, bank_get_int(bank, save_key::unit_count, 0), &ids);
    for (int32_t index = 0; index < stored && ids[index] == index; ++index) {
        uint8_t record[unit_record_bytes];
        if (!bank_open_blob_id(bank, index))
            break;
        bank_blob_seek(bank, 0);
        if (bank_blob_read(bank, record, unit_record_bytes) != unit_record_bytes)
            break;
        const uint16_t id = detail::load_le16(record + unit_record::id);
        if (id < records.size() && records[id] < 0)
            records[id] = index;
    }
    std::free(ids);
}

void save_read_units(SaveContext* save, Bank* bank) {
    if (!bank_open_account(bank, save_key::units) ||
        bank_get_int(bank, version_field, 0) != unit_snapshot_version)
        return;
    const SaveHooks* hooks = save->hooks;
    // Only ids that hold records restore anything, so the walk visits those
    // alone: a forged count or id can neither stall the load nor grow the bank.
    int32_t* ids = nullptr;
    const int32_t stored =
        save_unit_record_ids(bank, bank_get_int(bank, save_key::unit_count, 0), &ids);
    for (int32_t k = 0; k < stored; ++k) {
        if (!bank_open_blob_id(bank, ids[k]))
            continue;
        bank_blob_seek(bank, 0);
        uint8_t record[unit_record_bytes] = {};
        const uint32_t read = bank_blob_read(bank, record, unit_record_bytes);
        if (read != unit_record_bytes) {
            if (read != unit_record_legacy_bytes)
                continue;
            // Older records are passed on with a zero id, which restores nothing.
            store_le16(record + unit_record::id, 0);
        }
        hooks->restore_unit(hooks->context, detail::load_le16(record + unit_record::id), bank);
    }
    std::free(ids);
}

} // namespace oa::data::persist
