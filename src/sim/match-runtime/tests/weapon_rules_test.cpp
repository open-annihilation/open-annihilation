// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The weapons.* hacks and the weapon data keys in the running match: the
// targeting sweep dropping out-of-reach targets, timed shells, a
// surface-firing water weapon's flight, an ownerless blast without damage,
// the reach test's data keys and the vertical-launch constructor of a weapon
// that is also a turret. Each runs as 3.1c and under the rule.
#include "combat_fixture.hpp"
#include "oa/sim/weapon_execution/interceptor.hpp"
#include "oa/sim/weapon_execution/projectile_pool.hpp"
#include "oa/sim/weapon_execution/retaliation.hpp"
#include "oa/sim/weapon_execution/weapon_launch.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace combat_fixture;
using data::match_rules::WeaponsTimedShellDetonationRule;
using data::match_rules::WeaponTypeRules;

constexpr uint32_t fire_order_mask = OA_UNIT_FLAG_FIRE_ORDER_MASK;
constexpr uint32_t airborne = 2;

constexpr int32_t fx(int32_t whole) {
    return static_cast<int32_t>(static_cast<uint32_t>(whole) << 16);
}

/// Replaces the fixture's weapons with a TDF text and copies them into the
/// game's weapon table, as the match was given them.
///
/// @param f the fixture
/// @param tdf the weapon sections
/// @param count how many sections the text holds
void reinstall(Fixture& f, const std::string& tdf, size_t count) {
    CHECK(sim::combat_state::install_weapon_text(f.weapons, tdf) == count);
    std::copy(
        f.weapons.records().begin(),
        f.weapons.records().end(),
        std::begin(f.match->state().game.weapon_defs)
    );
}

/// Returns the per-weapon rules of a match with one weapon ID given its own.
///
/// @param weapon_id the weapon's ID
/// @param rules its rules
/// @return a table indexed by weapon ID
std::vector<WeaponTypeRules> one_weapon(uint16_t weapon_id, WeaponTypeRules rules) {
    std::vector<WeaponTypeRules> table(
        std::max(
            static_cast<std::size_t>(OA_WEAPON_DEF_COUNT), static_cast<std::size_t>(weapon_id) + 1
        )
    );
    table[weapon_id] = rules;
    return table;
}

/// Places a unit at a point, in both the simulation's copy and the record.
///
/// @param slot the unit
/// @param x whole world units
/// @param y whole world units
/// @param z whole world units
void place(sim::unit_spawn::Slot& slot, int32_t x, int32_t y, int32_t z) {
    slot.unit->position = {
        static_cast<uint32_t>(fx(x)), static_cast<uint32_t>(fx(y)), static_cast<uint32_t>(fx(z))
    };
    slot.record.position = {fx(x), fx(y), fx(z)};
}

void set_fire_order(sim::unit_spawn::Slot& slot, uint32_t order) {
    const auto flags =
        (slot.unit->flags & ~fire_order_mask) | (order << OA_UNIT_FLAG_FIRE_ORDER_SHIFT);
    slot.unit->flags = flags;
    slot.record.flags = flags;
}

/// Runs player 0's targeting pass until it has come round to every unit slot
/// of the player once.
///
/// @param f the fixture
void sweep_round(Fixture& f) {
    auto& world = f.match->state();
    uint32_t count = 0;
    (void)oa::world_player_units(&world, &world.game.players[0], &count);
    CHECK(count != 0);
    for (uint32_t pass = 0; pass < count; ++pass)
        f.match->sweep_weapon_targets(0, false);
}

// A 60-unit gun, so that units on the 256-unit map can stand out of its reach.
const std::string short_gun =
    "[TESTGUN]{id=1; reloadtime=0.1; range=60; lineofsight=1; weaponvelocity=100; turret=1;"
    " [DAMAGE]{default=10;}}";

// weapons.retarget-out-of-range: the time-sliced targeting pass keeps a
// slot's enemy target in 3.1c however far it goes. Under the rule a target
// out of reach loses only its index word, the pass leaves the slot empty that
// round, and on its next round the slot takes the enemy in reach.
void out_of_reach_targets(bool rule) {
    Options options;
    options.rules.weapons.retarget_out_of_range.enabled = rule;
    options.sight_cells = 7;
    Fixture f(options);
    f.match->state().game.console_flags |= OA_CONSOLE_FLAG_SHOOT_ALL;
    reinstall(f, short_gun, 1);
    auto& shooter = f.spawn(0, 40, 40);
    auto& far = f.spawn(1, 200, 40);
    auto& near = f.spawn(1, 80, 40);
    for (auto* unit : {&shooter, &far, &near})
        set_fire_order(*unit, 0);
    f.run(30);
    set_fire_order(shooter, 2);
    auto& slot = shooter.record.weapons[0];
    slot.flags =
        static_cast<uint8_t>(slot.flags | OA_UNIT_WEAPON_ENABLED | OA_UNIT_WEAPON_RETALIATE);
    sim::weapon_execution::aim_slot_at_unit(shooter.record, far.record, 0);
    CHECK(!f.match->weapon_can_reach(shooter.unit_index, far.unit_index, 0));
    CHECK(f.match->weapon_can_reach(shooter.unit_index, near.unit_index, 0));
    sweep_round(f);
    CHECK(slot.target_b == OA_UNIT_TARGET_IS_UNIT);
    if (!rule) {
        CHECK(slot.target_a == static_cast<int16_t>(far.unit_index));
        sweep_round(f);
        CHECK(slot.target_a == static_cast<int16_t>(far.unit_index));
    } else {
        CHECK(slot.target_a == 0);
        sweep_round(f);
        CHECK(slot.target_a == static_cast<int16_t>(near.unit_index));
        // A target in reach stays.
        sweep_round(f);
        CHECK(slot.target_a == static_cast<int16_t>(near.unit_index));
    }
}

// Under the rule the pass also takes a unit whose fire order is 3, which 3.1c
// skips; such a unit loses its out-of-reach target, but only fire at will
// lets the pass give it a new one.
void fire_order_three(bool rule) {
    Options options;
    options.rules.weapons.retarget_out_of_range.enabled = rule;
    options.sight_cells = 7;
    Fixture f(options);
    f.match->state().game.console_flags |= OA_CONSOLE_FLAG_SHOOT_ALL;
    reinstall(f, short_gun, 1);
    auto& shooter = f.spawn(0, 40, 40);
    auto& far = f.spawn(1, 200, 40);
    auto& near = f.spawn(1, 80, 40);
    for (auto* unit : {&shooter, &far, &near})
        set_fire_order(*unit, 0);
    f.run(30);
    set_fire_order(shooter, 3);
    auto& slot = shooter.record.weapons[0];
    slot.flags =
        static_cast<uint8_t>(slot.flags | OA_UNIT_WEAPON_ENABLED | OA_UNIT_WEAPON_RETALIATE);
    sim::weapon_execution::aim_slot_at_unit(shooter.record, far.record, 0);
    sweep_round(f);
    CHECK(slot.target_a == (rule ? 0 : static_cast<int16_t>(far.unit_index)));
    sweep_round(f);
    CHECK(slot.target_a == (rule ? 0 : static_cast<int16_t>(far.unit_index)));
    CHECK(slot.target_b == OA_UNIT_TARGET_IS_UNIT);
}

// Ballistic shells with a 3-tick weapontimer: one plain, one burnblow, one
// noautorange and one with both.
const std::string timed_shells =
    "[TESTGUN]{id=1; reloadtime=0.1; range=400; lineofsight=1; weaponvelocity=100; turret=1;"
    " [DAMAGE]{default=10;}}"
    "[PLAINSHELL]{id=3; ballistic=1; range=600; weaponvelocity=300; weapontimer=0.1;"
    " [DAMAGE]{default=10;}}"
    "[BURNSHELL]{id=4; ballistic=1; range=600; weaponvelocity=300; weapontimer=0.1; burnblow=1;"
    " [DAMAGE]{default=10;}}"
    "[NORANGESHELL]{id=5; ballistic=1; range=600; weaponvelocity=300; weapontimer=0.1;"
    " noautorange=1; [DAMAGE]{default=10;}}"
    "[BOTHSHELL]{id=6; ballistic=1; range=600; weaponvelocity=300; weapontimer=0.1; burnblow=1;"
    " noautorange=1; [DAMAGE]{default=10;}}";

struct Detonations {
    int32_t count{};
};

/// Fires a shell high over the map and flies it until its timer runs out.
///
/// @param f the fixture
/// @param weapon its weapon ID
/// @return true when it exploded, false when it went out in a puff
bool shell_explodes(Fixture& f, uint8_t weapon) {
    Detonations seen;
    f.match->event_hooks.context = &seen;
    f.match->event_hooks.shot_detonated =
        [](void* context, const oa::World&, const oa::Projectile&, uint16_t) {
            ++static_cast<Detonations*>(context)->count;
        };
    auto& world = f.match->state();
    auto* shot = sim::weapon_execution::allocate_projectile(world);
    CHECK(shot != nullptr);
    const FixedVec3 from{fx(128), fx(200), fx(128)};
    sim::weapon_execution::init_projectile_record(
        world, *shot, oa_ref_from_index(weapon), from, &from, world.game.tick, nullptr, 0
    );
    shot->owner_index = 1;
    shot->lifetime_tick = world.game.tick + 3;
    for (int step = 0; step < 6 && !f.match->projectiles().empty(); ++step) {
        ++world.game.tick;
        f.match->update_projectiles();
    }
    CHECK(f.match->projectiles().empty());
    f.match->event_hooks.shot_detonated = nullptr;
    return seen.count == 1;
}

// weapons.timed-shell-detonation: 3.1c explodes a timed-out shell only with
// burnblow; the not-noautorange rule explodes it unless it has noautorange.
// The hack at its burnblow rule plays as 3.1c.
void timed_shells_run_out(bool enabled, WeaponsTimedShellDetonationRule rule) {
    Options options;
    options.rules.weapons.timed_shell_detonation.enabled = enabled;
    options.rules.weapons.timed_shell_detonation.rule = rule;
    Fixture f(options);
    reinstall(f, timed_shells, 5);
    const bool hacked = enabled && rule == WeaponsTimedShellDetonationRule::not_noautorange;
    CHECK(shell_explodes(f, 3) == hacked);
    CHECK(shell_explodes(f, 4));
    CHECK(!shell_explodes(f, 5));
    CHECK(shell_explodes(f, 6) == !hacked);
}

// A self-propelled water weapon fired above the sea. In 3.1c it only falls
// there, level and at its launch speed; with the surface-fire key it flies
// as it would under water, accelerating along its pitch.
void surface_fire_flight(bool key) {
    Options options;
    options.high_ground_from = 12; // sea level 30 over the cells west of column 12
    if (key)
        options.weapon_rules = one_weapon(7, {false, true, false, false});
    Fixture f(options);
    reinstall(
        f,
        short_gun + "[SUBMISSILE]{id=7; lineofsight=1; waterweapon=1; selfprop=1; range=400;"
                    " weaponvelocity=110; startvelocity=30; weaponacceleration=15; flighttime=10;"
                    " [DAMAGE]{default=40;}}",
        2
    );
    auto& world = f.match->state();
    CHECK(world.game.sea_level == 30);
    auto* shot = sim::weapon_execution::allocate_projectile(world);
    CHECK(shot != nullptr);
    const FixedVec3 from{fx(40), fx(80), fx(40)};
    sim::weapon_execution::init_projectile_record(
        world, *shot, oa_ref_from_index(7), from, &from, world.game.tick, nullptr, 0
    );
    shot->owner_index = 1;
    shot->heading = 0x4000;
    shot->pitch = 0x1000;
    shot->speed = fx(1);
    shot->lifetime_tick = world.game.tick + 50;
    ++world.game.tick;
    f.match->update_projectiles();
    CHECK(!f.match->projectiles().empty());
    if (key) {
        CHECK(shot->pitch == 0x1000 && shot->speed > fx(1));
    } else {
        CHECK(shot->pitch == 0 && shot->speed == fx(1));
    }
}

// weapons.no-map-alert: an ownerless shot whose [DAMAGE] default is 0 skips
// its area blast, so a type the DAMAGE table names takes nothing. A shot with
// a source, or a weapon without the key, still blasts.
void ownerless_blast(bool key) {
    Options options;
    if (key)
        options.weapon_rules = one_weapon(8, {false, false, false, true});
    Fixture f(options);
    f.def.unit_name = unit_name;
    reinstall(
        f,
        short_gun +
            "[QUIETBLAST]{id=8; lineofsight=1; range=100; weaponvelocity=100; areaofeffect=64;"
            " [DAMAGE]{default=0; TESTUNIT=100;}}",
        2
    );
    auto& victim = f.spawn(0, 100, 100);
    auto& shooter = f.spawn(1, 200, 200);
    const auto blast = [&](bool sourced) {
        auto& world = f.match->state();
        auto* shot = sim::weapon_execution::allocate_projectile(world);
        CHECK(shot != nullptr);
        const FixedVec3 at{
            victim.record.position.x, victim.record.position.y, victim.record.position.z
        };
        sim::weapon_execution::init_projectile_record(
            world, *shot, oa_ref_from_index(8), at, &at, world.game.tick, nullptr, 0
        );
        shot->owner_index = 1;
        if (sourced)
            shot->source = oa_unit_ref_from_slot(shooter.unit_index);
        const auto before = victim.unit->health;
        f.match->detonate(*shot, nullptr);
        return before - victim.unit->health;
    };
    CHECK(blast(false) == (key ? 0 : 100));
    CHECK(blast(true) == 100);
}

// The weapon data keys in the match's reach test: not-to-air refuses an
// airborne target; a water weapon refuses a land target unless it has the
// surface-fire key.
void reach_keys_in_match() {
    for (const bool key : {false, true}) {
        Options options;
        options.high_ground_from = 12;
        if (key)
            options.weapon_rules = one_weapon(1, {true, false, false, false});
        Fixture f(options);
        auto& shooter = f.spawn(0, 200, 40);
        auto& plane = f.spawn(1, 220, 40);
        place(shooter, 200, 60, 40);
        place(plane, 220, 120, 40);
        plane.unit->flags = (plane.unit->flags & ~3u) | airborne;
        CHECK(f.match->weapon_can_reach(shooter.unit_index, plane.unit_index, 0) == !key);
    }
    for (const bool key : {false, true}) {
        Options options;
        options.high_ground_from = 12;
        if (key)
            options.weapon_rules = one_weapon(1, {false, true, false, false});
        Fixture f(options);
        reinstall(
            f,
            "[TESTGUN]{id=1; reloadtime=0.1; range=400; lineofsight=1; waterweapon=1;"
            " weaponvelocity=100; [DAMAGE]{default=10;}}",
            1
        );
        auto& sub = f.spawn(0, 100, 40);
        auto& tank = f.spawn(1, 220, 40);
        place(sub, 100, 10, 40);
        place(tank, 220, 60, 40);
        CHECK(f.match->weapon_can_reach(sub.unit_index, tank.unit_index, 0) == key);
    }
}

// A not-to-air key bound to the whole id applies to that weapon. The same
// key bound to the id's low byte does not.
void rules_follow_the_whole_id() {
    const WeaponTypeRules not_to_air{true, false, false, false};
    const auto place_pair = [](Fixture& f) {
        auto& shooter = f.spawn(0, 200, 40);
        auto& plane = f.spawn(1, 220, 40);
        place(shooter, 200, 60, 40);
        place(plane, 220, 120, 40);
        plane.unit->flags = (plane.unit->flags & ~3u) | airborne;
        return f.match->weapon_can_reach(shooter.unit_index, plane.unit_index, 0);
    };
    Options wide;
    wide.gun_id = 300;
    wide.weapon_types = 1024;
    wide.weapon_rules = one_weapon(300, not_to_air);
    Fixture bound(wide);
    CHECK(!place_pair(bound));

    Options folded;
    folded.gun_id = 300;
    folded.weapon_types = 1024;
    folded.weapon_rules = one_weapon(44, not_to_air);
    Fixture low(folded);
    CHECK(place_pair(low));
}

// weapons.vlaunch-before-turret: a weapon with turret and vlaunch is aimed as
// a turret either way. 3.1c fires it with the turret constructor, along its
// aim; the rule fires it with the vertical-launch constructor, straight up.
void vertical_launch_turret(bool rule) {
    Options options;
    options.rules.weapons.vlaunch_before_turret.enabled = rule;
    Fixture f(options);
    reinstall(
        f,
        "[TESTGUN]{id=1; reloadtime=0.1; range=400; lineofsight=1; selfprop=1; turret=1;"
        " vlaunch=1; weaponvelocity=100; flighttime=5; tolerance=65535; pitchtolerance=65535;"
        " [DAMAGE]{default=10;}}",
        1
    );
    auto& shooter = f.spawn(0, 40, 40);
    auto& target = f.spawn(1, 120, 40);

    struct Placed {
        int32_t count{};
        uint16_t pitch{};
    } placed;

    f.match->event_hooks.context = &placed;
    f.match->event_hooks.shot_placed = [](void* context,
                                          const oa::World&,
                                          const oa::Projectile& shot,
                                          sim::match_runtime::ShotSource,
                                          const oa::FixedVec3*,
                                          uint16_t) {
        auto& seen = *static_cast<Placed*>(context);
        if (seen.count++ == 0)
            seen.pitch = shot.pitch;
    };
    sim::weapon_execution::aim_slot_at_unit(shooter.record, target.record, 0);
    for (int step = 0; step < 30 && placed.count == 0; ++step)
        f.run(1);
    CHECK(placed.count > 0);
    if (rule)
        CHECK(placed.pitch == sim::weapon_execution::vertical_launch_pitch);
    else
        CHECK(placed.pitch != sim::weapon_execution::vertical_launch_pitch);
    f.match->event_hooks.shot_placed = nullptr;
}

} // namespace

int main() {
    try {
        for (const bool rule : {false, true}) {
            out_of_reach_targets(rule);
            fire_order_three(rule);
            surface_fire_flight(rule);
            ownerless_blast(rule);
            vertical_launch_turret(rule);
        }
        timed_shells_run_out(false, WeaponsTimedShellDetonationRule::burnblow);
        timed_shells_run_out(true, WeaponsTimedShellDetonationRule::burnblow);
        timed_shells_run_out(true, WeaponsTimedShellDetonationRule::not_noautorange);
        reach_keys_in_match();
        rules_follow_the_whole_id();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "weapon rules passed\n";
    return 0;
}
