// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The installed game's weapons/*.tdf, loaded the one way a match loads them:
// data::defs::load_weapon_defs into the WeaponDef table, then each loaded slot
// into the registry. Every installed definition must carry its record's
// values, and a few stock weapons keep the values 3.1c gives them.
#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/weapons.hpp"
#include "oa/sim/combat_state.hpp"
#include "oa/test/game_assets.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

using namespace oa::sim::combat_state;

namespace {

int failures = 0;

/// Reports a failed check and counts it.
///
/// @param what the failure, printed after "FAIL: "
void fail(const std::string& what) {
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

/// Checks one condition and reports it when it fails.
///
/// @param ok the condition
/// @param what the failure, printed after "FAIL: "
void check(bool ok, const std::string& what) {
    if (!ok)
        fail(what);
}

/// Checks that a slot's definition holds its record's values, unit for unit.
///
/// @param registry the installed weapons
/// @param table the table they were installed from
/// @param slot the slot
void check_slot(
    const WeaponRegistry& registry, const oa::data::defs::WeaponTable& table, uint8_t slot
) {
    const oa::WeaponDef& record = table.defs[slot];
    const oa::data::defs::WeaponAssetNames& names = table.assets[slot];
    const WeaponDefinition& definition = registry.definition(slot);
    const std::string key(record.key);
    check(registry.find(key) == &definition, key + " is not found by its name");
    check(
        std::memcmp(&registry.records()[slot], &record, sizeof record) == 0,
        key + " keeps a record other than the one loaded"
    );
    check(
        definition.reload_time_ticks == static_cast<uint16_t>(record.reload_time) &&
            definition.default_damage == static_cast<uint16_t>(record.damage_default) &&
            definition.projectile_velocity == record.weapon_velocity &&
            definition.minimum_barrel_angle_radians == record.min_barrel_angle &&
            definition.range_world_units == record.range && definition.flags == record.flags &&
            definition.start_velocity == record.start_velocity &&
            definition.acceleration == record.weapon_acceleration &&
            definition.turn_rate == static_cast<uint16_t>(record.turn_rate) &&
            definition.areaofeffect == static_cast<uint16_t>(record.area_of_effect) &&
            definition.weapontimer_ticks == static_cast<uint16_t>(record.weapon_timer) &&
            definition.burst_rate_ticks == static_cast<uint16_t>(record.burst_rate) &&
            definition.shake_duration_ticks == record.shake_duration &&
            definition.flight_time_ticks == static_cast<uint16_t>(record.flight_time) &&
            definition.coverage == record.coverage,
        key + " differs from its record"
    );
    check(
        definition.explosion_gaf == names.explosion_gaf &&
            definition.explosion_art == names.explosion_art &&
            definition.water_explosion_art == names.water_explosion_art &&
            definition.soundstart == names.sound_start && definition.soundhit == names.sound_hit,
        key + " lost an asset name"
    );
}

/// Loads every installed weapon and checks the registry against the table.
///
/// @param assets the installed game's store
void check_installed_weapons(const oa::AssetStore& assets) {
    const auto files = oa::data::defs::asset_store_files(&assets);
    const auto table = std::make_unique<oa::data::defs::WeaponTable>();
    const oa::data::defs::WeaponLoadOptions options{nullptr, nullptr, false, false};
    if (oa::data::defs::load_weapon_defs(&files, table.get(), &options) == 0) {
        fail("the install holds no weapons/*.tdf");
        return;
    }
    WeaponRegistry registry;
    const auto installed = install_weapon_table(registry, *table);
    // 3.1c, the Core Contingency, Battle Tactics and the Commander Pack
    // extras fill 198 of the 256 slots.
    check(installed == 198, "installed " + std::to_string(installed) + " weapons, not 198");
    check(table->rejected_ids == 0, "a stock weapon has an ID outside 0..255");
    check(table->defs.size() == OA_WEAPON_DEF_COUNT, "the stock table is not 256 slots");
    for (uint32_t slot = 0; slot < table->defs.size(); ++slot)
        if (table->defs[slot].key[0] != '\0')
            check_slot(registry, *table, static_cast<uint8_t>(slot));

    const auto* laser = registry.find("ARM_LIGHTLASER");
    check(
        laser != nullptr && laser->registry_index == 80 && laser->reload_time_ticks == 15 &&
            laser->range_world_units == 300 && laser->default_damage == 60 &&
            laser->projectile_velocity == 1966080 && laser->tolerance == 500 &&
            laser->flags == 0x00080809u && laser->color == 232 && laser->color2 == 234 &&
            laser->soundstart == "lasrfir3" && laser->water_explosion_art == "h2oboom1",
        "ARM_LIGHTLASER does not load as 3.1c loads it"
    );
    const auto* bomb = registry.find("ARMBOMB");
    check(
        bomb != nullptr && bomb->registry_index == 3 && bomb->range_world_units == 1280 &&
            bomb->default_damage == 158 && bomb->areaofeffect == 48 &&
            (bomb->flags & OA_WEAPON_FLAG_DROPPED) != 0 && bomb->explosion_art == "explode3",
        "ARMBOMB does not load as 3.1c loads it"
    );
    const auto* nuke = registry.find("NUCLEAR_MISSILE");
    check(
        nuke != nullptr && nuke->reload_time_ticks == 5400 && nuke->range_world_units == 32000 &&
            nuke->flight_time_ticks == 12000 && nuke->turn_rate == 1092 &&
            nuke->acceleration == 3640 && nuke->shake_duration_ticks == 45 &&
            damage_against(*nuke, "ARMCOM") == 2900 && damage_against(*nuke, "armpw") == 5500,
        "NUCLEAR_MISSILE does not load as 3.1c loads it"
    );
    oa::data::defs::weapon_table_free(table.get());

    // A lava world loads the lava explosions into the water slot.
    WeaponRegistry lava;
    check(
        install_weapon_files(lava, files, true) == 198,
        "a lava world installs another number of weapons"
    );
    std::cout << "installed " << installed << " weapons from the installed game\n";
}

} // namespace

int main() {
    check_installed_weapons(oa::test::require_game_assets("the installed weapons"));
    return failures == 0 ? 0 : 1;
}
