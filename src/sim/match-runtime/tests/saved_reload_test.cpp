// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The gun's reload countdown lives in the unit record. A unit saved on the
// tick its gun fired and restored into another match: the save carries the
// countdown, and the restored match counts it down and fires on the same
// ticks as the match that went on unsaved. A unit created in the slot of one
// that died mid-reload does not inherit the dead gun's countdown.
#include "combat_fixture.hpp"
#include "saved_game.hpp"

#include "oa/data/persist/save_sections.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <utility>

using namespace combat_fixture;

namespace {

// A 1.0 second reloadtime is 30 ticks; a unit at full health and without
// experience waits all of it after a shot.
constexpr const char* gun_reload_time = "1.0";
constexpr uint16_t gun_reload_ticks = 30;
constexpr uint32_t first_shot_limit = 60;
constexpr uint32_t shots_after_load = 3;
// Where the shooter stands and the ground point it fires at, world units.
constexpr uint32_t shooter_x = 64, shooter_z = 64;
constexpr int32_t aim_x = 160, aim_z = 64;

/// Returns the reload countdown a saved unit record holds for a weapon slot.
///
/// @param record unit record save_encode_unit_record wrote
/// @param slot weapon slot 0..2
/// @return the countdown, ticks, from its little-endian word
uint16_t
saved_reload(const std::array<uint8_t, data::persist::unit_record_bytes>& record, size_t slot) {
    const size_t at = data::persist::unit_record::weapons +
                      slot * data::persist::unit_record::weapon_bytes +
                      data::persist::weapon_record::reload;
    return static_cast<uint16_t>(record[at] | (record[at + 1] << 8));
}

/// Spawns the shooter and orders it to fire at a point on open ground.
///
/// @param[in,out] fixture match to spawn into
/// @return the shooter
sim::unit_spawn::Slot& shooter_in(Fixture& fixture) {
    auto& shooter = fixture.spawn(0, shooter_x, shooter_z);
    CHECK(
        fixture.match->issue_attack_ground(shooter.unit_index, {aim_x << 16, 0, aim_z << 16}, false)
    );
    return shooter;
}

/// Saves the shooter on the tick of its first shot, restores it into a new
/// match, then runs both matches on and compares them tick by tick.
void reload_survives_a_save() {
    Options options;
    options.gun_reload_time = gun_reload_time;
    Fixture unsaved(options);
    auto& shooter = shooter_in(unsaved);
    uint32_t ticks = 0;
    while (unsaved.shots_from(shooter) == 0 && ticks < first_shot_limit) {
        unsaved.run(1);
        ++ticks;
    }
    CHECK(unsaved.shots_from(shooter) == 1);
    CHECK(shooter.record.weapons[0].reload == gun_reload_ticks);
    const uint32_t fired_tick = unsaved.match->state().game.tick;

    // The save: the unit record with its weapon slots, and its orders.
    data::persist::SaveContext save{
        &unsaved.match->state(), nullptr, nullptr, nullptr, nullptr, nullptr
    };
    saved_game::ScopedBank bank;
    data::persist::bank_open_account(bank.get(), "Units");
    const auto order_count = saved_game::write_orders(*unsaved.match, shooter, bank.get());
    std::array<uint8_t, data::persist::unit_record_bytes> record{};
    data::persist::save_encode_unit_record(
        &save, &shooter.record, order_count, true, record.data()
    );
    CHECK(saved_reload(record, 0) == gun_reload_ticks);

    // The load: the unit is created again in a new match, then takes its
    // saved weapon slots and orders. Its gun starts with no countdown there,
    // so the countdown it has after the restore comes from the save.
    Fixture loaded(options);
    auto& restored = loaded.spawn(0, shooter_x, shooter_z);
    CHECK(restored.record.id == shooter.record.id);
    CHECK(restored.record.weapons[0].reload == 0);
    loaded.match->state().game.tick = fired_tick;
    for (size_t slot = 0; slot < OA_UNIT_WEAPON_COUNT; ++slot)
        data::persist::save_restore_unit_weapon(
            &loaded.match->state(),
            record.data() + data::persist::unit_record::weapons +
                slot * data::persist::unit_record::weapon_bytes,
            restored.record.weapons[slot],
            data::persist::WeaponIdWidth::byte
        );
    saved_game::restore_orders(*loaded.match, restored, bank.get(), order_count);
    CHECK(restored.record.weapons[0].reload == gun_reload_ticks);

    // Both count the same reload down each tick and fire on the same ticks,
    // the first a full reload after the saved shot.
    uint32_t first_shot_after_load = 0;
    for (uint32_t tick = 1; tick <= shots_after_load * gun_reload_ticks; ++tick) {
        unsaved.run(1);
        loaded.run(1);
        CHECK(restored.record.weapons[0].reload == shooter.record.weapons[0].reload);
        CHECK(loaded.shots_from(restored) + 1 == unsaved.shots_from(shooter));
        if (first_shot_after_load == 0 && loaded.shots_from(restored) == 1)
            first_shot_after_load = tick;
    }
    CHECK(first_shot_after_load == gun_reload_ticks);
    CHECK(loaded.shots_from(restored) == static_cast<int32_t>(shots_after_load));
    std::cout << "saved reload: countdown and shots match after load\n";
}

/// Runs a new shooter until its first shot.
///
/// @param[in,out] fixture match to spawn into
/// @return the shooter and the ticks it took to fire
std::pair<sim::unit_spawn::Slot*, uint32_t> first_shot_of_new_shooter(Fixture& fixture) {
    auto& shooter = shooter_in(fixture);
    CHECK(shooter.record.weapons[0].reload == 0);
    uint32_t ticks = 0;
    while (fixture.shots_from(shooter) == 0 && ticks < first_shot_limit) {
        fixture.run(1);
        ++ticks;
    }
    CHECK(fixture.shots_from(shooter) == 1);
    return {&shooter, ticks};
}

/// Kills a shooter mid-reload and creates another in its unit slot; the
/// newcomer fires as soon as the first shooter did, without waiting out the
/// dead gun's countdown.
void reused_slot_does_not_inherit_reload() {
    Options options;
    options.gun_reload_time = gun_reload_time;
    Fixture fixture(options);
    const auto [dead, fresh_ticks] = first_shot_of_new_shooter(fixture);
    const auto dead_index = dead->unit_index;
    (void)fixture.match->insert_ground_order(dead_index, sim::match_runtime::self_destruct_kind);
    fixture.run(1);
    CHECK((dead->unit->flags & OA_UNIT_FLAG_LIVE) == 0);
    // Death does not clear the unit record; the countdown stays in the slot.
    CHECK(dead->record.weapons[0].reload != 0);

    const auto [newcomer, newcomer_ticks] = first_shot_of_new_shooter(fixture);
    CHECK(newcomer->unit_index == dead_index);
    CHECK(newcomer_ticks == fresh_ticks);
    std::cout << "saved reload: a reused unit slot does not inherit the reload\n";
}

} // namespace

int main() {
    try {
        reload_survives_a_save();
        reused_slot_does_not_inherit_reload();
    } catch (const std::exception& failure) {
        std::cerr << "saved reload: " << failure.what() << '\n';
        return 1;
    }
    return 0;
}
