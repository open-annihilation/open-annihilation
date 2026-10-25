// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/weapon_execution/interceptor.hpp"
#include "oa/sim/ballistics.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/weapon_execution/projectile_pool.hpp"
#include "oa/data/persist/save_sections.hpp"
#include "oa/data/unit_definitions.hpp"
#include "oa/sim/weapon_execution/weapon_launch.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "oa/test/match_services.hpp"

using namespace oa;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #x);   \
    } while (false)

namespace {
constexpr int32_t map_cells = 32;
constexpr uint8_t ground_height = 30;
constexpr uint8_t sea_level = 20;
constexpr int32_t shore_cell = 24; // cells from here east are sea floor at height 0
constexpr int32_t cliff_x = 11, cliff_z = 5;
constexpr uint8_t cliff_height = 60;
constexpr int32_t rock_x = 15, rock_z = 9;
constexpr int8_t rock_height = 20;
constexpr int32_t sea_rock_x = 27, sea_rock_z = 12;
constexpr int8_t sea_rock_height = 40;
constexpr int16_t unit_target = OA_UNIT_TARGET_IS_UNIT;

constexpr uint32_t fire_at_will = 2;

constexpr uint8_t antinuke_weapon = 30;
constexpr uint8_t nuke_weapon = 122;
constexpr uint8_t shell_weapon = 3;
constexpr uint8_t laser_weapon = 4;
constexpr uint8_t torpedo_weapon = 5;
constexpr uint8_t probe_weapon = 6;
constexpr uint8_t disintegrator_weapon = 22;
constexpr uint8_t depth_charge_weapon = 7;

constexpr std::string_view weapon_tdf = R"([AMD_ROCKET]
{
ID=30; lineofsight=1; vlaunch=1; range=32000; coverage=2000; reloadtime=120; noautorange=1;
weapontimer=4; flighttime=60; weaponvelocity=800; weaponacceleration=60; turnrate=32768;
areaofeffect=96; energypershot=10000; metalpershot=200; stockpile=1; interceptor=1; selfprop=1;
tracks=1; twophase=1; guidance=1; tolerance=4000; explosiongaf=fx; explosionart=explode3;
[DAMAGE] { default=500; }
}
[NUCLEAR_MISSILE]
{
ID=122; lineofsight=1; vlaunch=1; range=32000; reloadtime=180; noautorange=1; weapontimer=5;
flighttime=400; weaponvelocity=350; weaponacceleration=50; turnrate=32768; areaofeffect=512;
edgeeffectiveness=0.25; stockpile=1; targetable=1; commandfire=1; cruise=1; selfprop=1;
twophase=1; guidance=1; tolerance=4000; explosiongaf=commboom; explosionart=commboom;
[DAMAGE] { default=5500; }
}
[SHELL]
{
ID=3; ballistic=1; turret=1; range=600; reloadtime=2; weaponvelocity=300; areaofeffect=32;
explosiongaf=fx; explosionart=explode2; waterexplosiongaf=fx; waterexplosionart=h2oboom1;
[DAMAGE] { default=100; }
}
[LASER]
{
ID=4; lineofsight=1; turret=1; range=400; reloadtime=1; weaponvelocity=600; areaofeffect=48;
explosiongaf=fx; explosionart=explode1; waterexplosiongaf=fx; waterexplosionart=h2oboom1;
[DAMAGE] { default=40; }
}
[TORPEDO]
{
ID=5; lineofsight=1; waterweapon=1; range=400; reloadtime=1; weaponvelocity=200; areaofeffect=16;
explosiongaf=fx; explosionart=explode1; waterexplosiongaf=fx; waterexplosionart=h2oboom2;
[DAMAGE] { default=40; }
}
[DEPTHCHARGE]
{
ID=7; lineofsight=1; waterweapon=1; selfprop=1; burnblow=1; range=410; reloadtime=3;
weapontimer=3; weaponvelocity=110; startvelocity=100; weaponacceleration=15; areaofeffect=16;
explosiongaf=fx; explosionart=explode1; waterexplosiongaf=fx; waterexplosionart=h2oboom2;
[DAMAGE] { default=40; }
}
[PROBE]
{
ID=6; lineofsight=1; interceptor=1; range=100; reloadtime=1; weaponvelocity=100; areaofeffect=16;
explosiongaf=fx; explosionart=explode1;
[DAMAGE] { default=10; }
}
[ARM_DISINTEGRATOR]
{
ID=22; name=Disintegrator; rendertype=3; lineofsight=1; turret=1; model=dgun; range=240;
reloadtime=1.2; weapontimer=4; energypershot=400; weaponvelocity=200; areaofeffect=48;
soundtrigger=1; soundstart=disigun1; soundhit=xplomas2; firestarter=70; beamweapon=1; noexplode=1;
commandfire=1; explosiongaf=fx; explosionart=explode5; waterexplosiongaf=fx; waterexplosionart=h2o;
lavaexplosiongaf=fx; lavaexplosionart=lavasplash; startsmoke=1;
[DAMAGE] { default=50; }
}
)";

using Services = oa::test::QuietServices;

using Scenario = oa::test::EmptyScenario;

// One distinct sequence per explosion entry the weapons above name, so a
// logged record's sprite tells which art went off.
struct Art {
    formats::gaf::Sequence explode1, explode2, explode3, explode5, h2oboom1, h2oboom2, h2o,
        commboom;

    Art() {
        for (auto* sequence :
             {&explode1, &explode2, &explode3, &explode5, &h2oboom1, &h2oboom2, &h2o, &commboom}) {
            sequence->frames.resize(4);
            for (auto& frame : sequence->frames)
                frame.duration = 2;
        }
    }

    const formats::gaf::Sequence* find(std::string_view archive, std::string_view entry) const {
        if (archive.empty())
            return nullptr;
        for (const auto& [name, sequence] :
             {std::pair{"explode1", &explode1},
              std::pair{"explode2", &explode2},
              std::pair{"explode3", &explode3},
              std::pair{"explode5", &explode5},
              std::pair{"h2oboom1", &h2oboom1},
              std::pair{"h2oboom2", &h2oboom2},
              std::pair{"h2o", &h2o},
              std::pair{"commboom", &commboom}})
            if (entry == name)
                return sequence;
        return nullptr;
    }
};

const Art art;

int32_t explosions_logged(sim::match_runtime::Match& match) {
    return match.effects().explosion_count;
}

const sim::effect_particles::ExplosionRecord& latest_explosion(sim::match_runtime::Match& match) {
    const auto& world = match.effects();
    CHECK(world.explosion_count > 0);
    return world.explosions[world.explosion_count - 1];
}

constexpr int32_t fx(int32_t whole) {
    return static_cast<int32_t>(static_cast<uint32_t>(whole) << 16);
}

size_t plot(int32_t x, int32_t z) {
    return static_cast<size_t>(z * map_cells + x);
}

// Flat ground above the sea, a sea floor east of the shore, one raised cell,
// a rock on land and one in the sea. The land rock's plot also carries the
// height the map loader resolves; the sea rock's is only in the feature table.
std::vector<sim::spatial_state::Plot> terrain_plots() {
    std::vector<sim::spatial_state::Plot> plots(map_cells * map_cells);
    for (int32_t z = 0; z < map_cells; ++z)
        for (int32_t x = 0; x < map_cells; ++x) {
            auto& cell = plots[plot(x, z)];
            cell.high_height = cell.low_height = x >= shore_cell ? 0 : ground_height;
        }
    plots[plot(cliff_x, cliff_z)].high_height = plots[plot(cliff_x, cliff_z)].low_height =
        cliff_height;
    auto& rock = plots[plot(rock_x, rock_z)];
    rock.feature_word = 0;
    rock.feature_height = static_cast<uint8_t>(rock_height);
    rock.blocking_feature = true;
    auto& sea_rock = plots[plot(sea_rock_x, sea_rock_z)];
    sea_rock.feature_word = 1;
    return plots;
}

// A live record in the pool, as a weapon constructor leaves it.
oa::Projectile& launch(
    sim::match_runtime::Match& match,
    uint8_t weapon,
    const FixedVec3& from,
    const FixedVec3& velocity,
    uint8_t owner
) {
    auto& world = match.state();
    auto* shot = sim::weapon_execution::allocate_projectile(world);
    CHECK(shot != nullptr);
    sim::weapon_execution::init_projectile_record(
        world, *shot, oa_ref_from_index(weapon), from, &from, world.game.tick, nullptr, 0
    );
    shot->owner_index = owner;
    shot->velocity = velocity;
    shot->lifetime_tick = world.game.tick + 200;
    return *shot;
}

// Runs the projectile tick until a record logs an explosion; returns it.
sim::effect_particles::ExplosionRecord
fly_until_blast(sim::match_runtime::Match& match, int32_t explosions_before, int32_t ticks) {
    for (int32_t step = 0; step < ticks; ++step) {
        ++match.state().game.tick;
        match.update_projectiles();
        if (explosions_logged(match) > explosions_before)
            return latest_explosion(match);
    }
    throw std::runtime_error("projectile never detonated");
}

int32_t cell_of(int32_t fixed) {
    return fixed >> 20;
}

// The intercept and chain tests' distance: each axis' 16.16 square keeps its
// high word, summed in whole units squared.
int32_t squared_units(int32_t dx, int32_t dy, int32_t dz) {
    const auto high = [](int32_t d) {
        return static_cast<uint32_t>(static_cast<uint64_t>(static_cast<int64_t>(d) * d) >> 32);
    };
    return static_cast<int32_t>(high(dx) + high(dy) + high(dz));
}

void ground_contact(sim::match_runtime::Match& match) {
    // A level shell crosses into the raised cell below its height and bursts
    // there, not at the TNT surface under it.
    const auto before = explosions_logged(match);
    launch(
        match, shell_weapon, {fx(cliff_x * 16 - 3), fx(45), fx(cliff_z * 16 + 8)}, {fx(2), 0, 0}, 1
    );
    const auto blast = fly_until_blast(match, before, 10);
    CHECK(cell_of(blast.position.x) == cliff_x && cell_of(blast.position.z) == cliff_z);
    CHECK(blast.position.x == fx(cliff_x * 16 + 1) && blast.position.y == fx(45));
    CHECK(blast.sprite.sequence == &art.explode2);
    CHECK(match.projectiles().empty());

    // Falling onto flat ground it bursts on the first tick below the plot's low height.
    const auto falling = explosions_logged(match);
    launch(match, shell_weapon, {fx(40), fx(ground_height + 3), fx(40)}, {0, -0x18000, 0}, 1);
    const auto landed = fly_until_blast(match, falling, 10);
    CHECK(landed.position.y == fx(ground_height + 3) - 3 * 0x18000);
    CHECK(match.projectiles().empty());
    std::cout << "ballistic ground contact passed\n";
}

void water_contact(sim::match_runtime::Match& match) {
    // A laser dropping into the sea bursts at the surface with the water art.
    const auto before = explosions_logged(match);
    launch(
        match,
        laser_weapon,
        {fx(shore_cell * 16 + 40), fx(sea_level + 5), fx(100)},
        {0, -fx(2), 0},
        1
    );
    const auto splash = fly_until_blast(match, before, 10);
    CHECK(splash.position.y == fx(sea_level - 1));
    CHECK(splash.sprite.sequence == &art.h2oboom1);
    CHECK(match.projectiles().empty());

    // A torpedo goes through the surface, splashing with its water art as it
    // enters, and only stops at the sea floor.
    const auto dive = explosions_logged(match);
    launch(
        match,
        torpedo_weapon,
        {fx(shore_cell * 16 + 40), fx(sea_level + 5), fx(100)},
        {0, -fx(2), 0},
        1
    );
    const auto entry = fly_until_blast(match, dive, 10);
    CHECK(entry.position.y == fx(sea_level - 1) && entry.sprite.sequence == &art.h2oboom2);
    CHECK(match.projectiles().size() == 1);
    const auto floor = fly_until_blast(match, dive + 1, 20);
    CHECK(floor.position.y == fx(-1));
    CHECK(floor.sprite.sequence == &art.h2oboom2);
    CHECK(match.projectiles().empty());

    // The explosion art follows the plot under the burst, not the burst's
    // height: a laser hitting the sea rock above the surface splashes.
    const auto rock_hit = explosions_logged(match);
    launch(
        match,
        laser_weapon,
        {fx(sea_rock_x * 16 - 6), fx(sea_level + 5), fx(sea_rock_z * 16 + 8)},
        {fx(4), 0, 0},
        1
    );
    const auto above = fly_until_blast(match, rock_hit, 10);
    CHECK(cell_of(above.position.x) == sea_rock_x && above.position.y == fx(sea_level + 5));
    CHECK(above.sprite.sequence == &art.h2oboom1);
    CHECK(match.projectiles().empty());
    std::cout << "water contact passed\n";
}

// A self-propelled water weapon launched above the sea, as a depth charge
// dropped over the side, only falls while it is above the surface: level
// (pitch 0) and at its launch speed. Once under the surface it accelerates.
void water_weapon_drops_into_the_sea(sim::match_runtime::Match& match) {
    auto& game = match.state().game;
    const auto gravity = game.gravity;
    game.gravity = sim::ballistics::default_simulation_gravity;
    auto& shot = launch(
        match,
        depth_charge_weapon,
        {fx(shore_cell * 16 + 40), fx(sea_level + 30), fx(100)},
        {fx(1), 0, 0},
        1
    );
    shot.heading = 0x4000;
    shot.pitch = 0x1000;
    shot.speed = fx(1);
    const auto launched = shot.speed;
    auto height = shot.position.y;
    int32_t ticks = 0;
    while (shot.position.y >= fx(sea_level) && ticks++ < 100) {
        ++game.tick;
        match.update_projectiles();
        CHECK(shot.pitch == 0 && shot.speed == launched && shot.velocity.x == fx(1));
        CHECK(shot.position.y < height);
        height = shot.position.y;
    }
    CHECK(ticks < 100 && ticks > 1);
    ++game.tick;
    match.update_projectiles();
    CHECK(shot.speed > launched);
    sim::weapon_execution::retire_projectile(match.state(), shot);
    game.gravity = gravity;
    std::cout << "water weapon drop passed\n";
}

void feature_contact(sim::match_runtime::Match& match) {
    // A low laser flying at the rock bursts on entering its cell and the blast damages it.
    const auto before = explosions_logged(match);
    const auto& rock = match.state().plots[plot(rock_x, rock_z)];
    launch(
        match,
        laser_weapon,
        {fx(rock_x * 16 - 6), fx(ground_height + 5), fx(rock_z * 16 + 8)},
        {fx(4), 0, 0},
        1
    );
    const auto blast = fly_until_blast(match, before, 10);
    CHECK(cell_of(blast.position.x) == rock_x && cell_of(blast.position.z) == rock_z);
    CHECK(rock.feature_record == 40);
    CHECK(match.projectiles().empty());

    // A new shot keeps the feature cell of its pool slot's last shot, so the
    // next shot there passes through the rock's cell once.
    auto& reused = launch(
        match,
        laser_weapon,
        {fx(rock_x * 16 - 6), fx(ground_height + 5), fx(rock_z * 16 + 8)},
        {fx(4), 0, 0},
        1
    );
    CHECK(reused.feature_cell_x == rock_x && reused.feature_cell_z == rock_z);
    const auto stale = explosions_logged(match);
    for (int32_t step = 0; step < 3; ++step) {
        ++match.state().game.tick;
        match.update_projectiles();
    }
    CHECK(explosions_logged(match) == stale && match.projectiles().size() == 1);
    sim::weapon_execution::retire_projectile(match.state(), match.state().projectiles[0]);
    sim::weapon_execution::compact_projectiles(match.state());

    // A mirrored player's shot bursts the same way but leaves the damage to its own machine.
    auto& owner = match.state().game.players[1];
    const auto status = owner.status;
    owner.status = OA_PLAYER_STATUS_MIRRORED;
    auto& mirrored_shot = launch(
        match,
        laser_weapon,
        {fx(rock_x * 16 - 6), fx(ground_height + 5), fx(rock_z * 16 + 8)},
        {fx(4), 0, 0},
        1
    );
    mirrored_shot.feature_cell_x = mirrored_shot.feature_cell_z = -1;
    const auto mirrored = explosions_logged(match);
    const auto mirrored_blast = fly_until_blast(match, mirrored, 10);
    CHECK(cell_of(mirrored_blast.position.x) == rock_x);
    CHECK(rock.feature_record == 40);
    owner.status = status;

    // Above the rock's top the same shot passes over it.
    launch(
        match,
        laser_weapon,
        {fx(rock_x * 16 - 6), fx(ground_height + rock_height + 2), fx(rock_z * 16 + 8)},
        {fx(4), 0, 0},
        1
    );
    const auto passes = explosions_logged(match);
    for (int32_t step = 0; step < 6; ++step) {
        ++match.state().game.tick;
        match.update_projectiles();
    }
    CHECK(explosions_logged(match) == passes && match.projectiles().size() == 1);
    CHECK(cell_of(match.projectiles()[0].position.x) > rock_x);
    sim::weapon_execution::retire_projectile(match.state(), match.state().projectiles[0]);
    sim::weapon_execution::compact_projectiles(match.state());

    // A plot word past the feature table is no feature, whatever height the
    // plot carries: the low shot flies on.
    auto& contact_plot = match.spatial().plots[plot(rock_x, rock_z)];
    contact_plot.feature_word = 2;
    auto& untabled = launch(
        match,
        laser_weapon,
        {fx(rock_x * 16 - 6), fx(ground_height + 5), fx(rock_z * 16 + 8)},
        {fx(4), 0, 0},
        1
    );
    untabled.feature_cell_x = untabled.feature_cell_z = -1;
    const auto untabled_before = explosions_logged(match);
    for (int32_t step = 0; step < 6; ++step) {
        ++match.state().game.tick;
        match.update_projectiles();
    }
    CHECK(explosions_logged(match) == untabled_before && match.projectiles().size() == 1);
    CHECK(cell_of(match.projectiles()[0].position.x) > rock_x);
    sim::weapon_execution::retire_projectile(match.state(), match.state().projectiles[0]);
    sim::weapon_execution::compact_projectiles(match.state());
    contact_plot.feature_word = 0;
    std::cout << "blocking feature contact passed\n";
}

void interception(
    sim::match_runtime::Match& match, sim::unit_spawn::Slot& antinuke, sim::unit_spawn::Slot& silo
) {
    auto& world = match.state();
    // The silo's missile is in flight toward the anti-nuke, which holds one round.
    const FixedVec3 pad{fx(350), fx(ground_height), fx(60)};
    const FixedVec3 aim{fx(70), fx(ground_height), fx(60)};
    auto* nuke = sim::weapon_execution::allocate_projectile(world);
    CHECK(nuke != nullptr);
    const auto nuke_ref = oa_ref_from_index(nuke_weapon);
    const auto antinuke_ref = oa_ref_from_index(antinuke_weapon);
    sim::weapon_execution::init_projectile_record(
        world, *nuke, nuke_ref, pad, &aim, world.game.tick, &silo.record, 0
    );
    const auto rise = sim::weapon_execution::launch_vertical_projectile(
        world.game.weapon_defs[nuke_weapon], world.game.tick
    );
    nuke->heading = rise.heading;
    nuke->pitch = rise.pitch;
    nuke->speed = rise.speed;
    nuke->lifetime_tick = rise.lifetime_tick;
    antinuke.record.weapons[0].stockpile = 1;
    // The fire-at-will weapon sweep aims the idle interceptor at the missile.
    const auto& slot = antinuke.record.weapons[0];
    CHECK((slot.flags & OA_UNIT_WEAPON_ENABLED) != 0 && slot.target_a == 0);
    auto& effects = match.effects();
    effects.explosion_count = 0;
    bool aimed = false, homing = false, intercepted = false;
    // Both records at the end of the last tick they were both in flight.
    FixedVec3 last_nuke{}, last_rocket{};
    bool rocket_seen = false;
    int32_t ticks = 0;
    for (; ticks < 600 && !intercepted; ++ticks) {
        ++world.game.tick;
        match.tick();
        aimed =
            aimed || (slot.target_a == static_cast<int16_t>(pad.x >> 16) && slot.target_b == 60);
        const oa::Projectile* live_nuke = nullptr;
        const oa::Projectile* rocket = nullptr;
        for (const auto& shot : match.projectiles()) {
            if (shot.def == nuke_ref && (shot.flags & OA_PROJECTILE_FLAG_RETIRED) == 0)
                live_nuke = &shot;
            if (shot.def == antinuke_ref && (shot.flags & OA_PROJECTILE_FLAG_RETIRED) == 0)
                rocket = &shot;
        }
        if (rocket != nullptr) {
            // Launched only at a missile it found, which it carries from the start.
            const auto* target = world_projectile(&world, rocket->intercept_target);
            CHECK(target != nullptr && target->def == nuke_ref);
            homing = homing || (rocket->flags & OA_PROJECTILE_PHASE_MASK) != 0;
        }
        if (live_nuke != nullptr && rocket != nullptr) {
            last_nuke = live_nuke->position;
            last_rocket = rocket->position;
            rocket_seen = true;
        }
        intercepted = rocket_seen && live_nuke == nullptr;
        CHECK(intercepted || effects.explosion_count == 0);
    }
    CHECK(aimed && homing && intercepted);
    CHECK(antinuke.record.weapons[0].stockpile == 0);
    // With no round left the sweep drops the interceptor's target.
    CHECK(slot.target_a == 0 && slot.target_b == unit_target);
    CHECK(match.projectiles().empty());
    // The rocket bursts on the first tick it is within its blast width of the
    // missile, and its interceptor blast detonates the missile where it is.
    CHECK(effects.explosion_count == 2);
    const auto& burst = effects.explosions[0];
    const auto& warhead = effects.explosions[1];
    CHECK(burst.sprite.sequence == &art.explode3 && warhead.sprite.sequence == &art.commboom);
    constexpr int32_t rocket_blast = 96 * 96;
    CHECK(
        squared_units(
            last_rocket.x - last_nuke.x, last_rocket.y - last_nuke.y, last_rocket.z - last_nuke.z
        ) >= rocket_blast
    );
    CHECK(
        squared_units(
            burst.position.x - warhead.position.x,
            burst.position.y - warhead.position.y,
            burst.position.z - warhead.position.z
        ) < rocket_blast
    );
    // The missile never came within its own blast radius (256) of the anti-nuke.
    const auto& site = antinuke.record.position;
    CHECK(
        squared_units(
            warhead.position.x - site.x, warhead.position.y - site.y, warhead.position.z - site.z
        ) >= 256 * 256
    );
    std::cout << "anti-nuke interception passed after " << ticks << " ticks\n";
}

// While the silo's nuke and the anti-nuke's rocket are both in flight, each
// stockpile is empty in the match and one in the saved record. Restoring the
// record into a fresh unit of the same type puts the round back.
void stockpile_refund(
    sim::match_runtime::Match& match, sim::unit_spawn::Slot& antinuke, sim::unit_spawn::Slot& silo
) {
    auto& world = match.state();
    CHECK(match.projectiles().empty());
    silo.record.weapons[0].stockpile = 0;
    antinuke.record.weapons[0].stockpile = 0;
    const FixedVec3 pad{fx(350), fx(ground_height), fx(60)};
    const FixedVec3 aim{fx(70), fx(ground_height), fx(60)};
    const auto nuke_ref = oa_ref_from_index(nuke_weapon);
    const auto rocket_ref = oa_ref_from_index(antinuke_weapon);
    auto* nuke = sim::weapon_execution::allocate_projectile(world);
    auto* rocket = sim::weapon_execution::allocate_projectile(world);
    CHECK(nuke != nullptr && rocket != nullptr);
    sim::weapon_execution::init_projectile_record(
        world, *nuke, nuke_ref, pad, &aim, world.game.tick, &silo.record, 0
    );
    sim::weapon_execution::init_projectile_record(
        world, *rocket, rocket_ref, aim, &pad, world.game.tick, &antinuke.record, 0
    );
    CHECK((world.game.weapon_defs[nuke_weapon].flags & OA_WEAPON_FLAG_STOCKPILE) != 0);
    CHECK((world.game.weapon_defs[antinuke_weapon].flags & OA_WEAPON_FLAG_STOCKPILE) != 0);
    CHECK(silo.record.weapons[0].stockpile == 0);
    CHECK(antinuke.record.weapons[0].stockpile == 0);

    data::persist::SaveContext save{&world, nullptr, nullptr, nullptr, nullptr, nullptr};
    std::array<std::array<uint8_t, data::persist::unit_record_bytes>, 2> records{};
    sim::unit_spawn::Slot* launchers[] = {&silo, &antinuke};
    for (std::size_t index = 0; index < 2; ++index) {
        data::persist::save_encode_unit_record(
            &save, &launchers[index]->record, 0, false, records[index].data()
        );
        const auto at =
            data::persist::unit_record::weapons + data::persist::weapon_record::stockpile;
        CHECK(records[index][at] == 1);
        CHECK(launchers[index]->record.weapons[0].stockpile == 0);
    }

    for (std::size_t index = 0; index < 2; ++index) {
        Unit fresh{};
        fresh.weapons[0].def = launchers[index]->record.weapons[0].def;
        data::persist::save_restore_unit_weapon(
            &world,
            records[index].data() + data::persist::unit_record::weapons,
            fresh.weapons[0],
            data::persist::WeaponIdWidth::byte
        );
        CHECK(fresh.weapons[0].stockpile == 1);
        CHECK(launchers[index]->record.weapons[0].stockpile == 0);
    }

    sim::weapon_execution::retire_projectile(world, *nuke);
    sim::weapon_execution::retire_projectile(world, *rocket);
    sim::weapon_execution::compact_projectiles(world);
    CHECK(match.projectiles().empty());
    std::cout << "stockpile refund while both missiles are in flight passed\n";
}

void direct_hit_does_not_chain(sim::match_runtime::Match& match, sim::unit_spawn::Slot& antinuke) {
    // A blast under 17 wide that hits a unit damages only that unit; the
    // interceptor chain belongs to the area blast, so a shot beside it flies on.
    CHECK(match.spatial().plots[plot(4, 3)].ground == antinuke.unit_index);
    const auto health = antinuke.record.health;
    const FixedVec3 bystander_from{fx(60), fx(40), fx(60)};
    const FixedVec3 bystander_velocity{0, fx(1), 0};
    const auto& bystander = launch(match, laser_weapon, bystander_from, bystander_velocity, 1);
    launch(match, probe_weapon, {fx(62), fx(31), fx(60)}, {fx(4), -fx(2), 0}, 1);
    const auto before = explosions_logged(match);
    ++match.state().game.tick;
    match.update_projectiles();
    CHECK(explosions_logged(match) == before + 1);
    const auto& hit = latest_explosion(match);
    CHECK(hit.position.x == fx(66) && hit.position.y == fx(29));
    CHECK(
        squared_units(
            hit.position.x - bystander.position.x, hit.position.y - bystander.position.y, 0
        ) < 16 * 16
    );
    CHECK(antinuke.record.health < health);
    CHECK(
        match.projectiles().size() == 1 &&
        match.projectiles()[0].def == oa_ref_from_index(laser_weapon)
    );
    sim::weapon_execution::retire_projectile(match.state(), match.state().projectiles[0]);
    sim::weapon_execution::compact_projectiles(match.state());
    antinuke.record.health = health;
    std::cout << "direct hit without chain passed\n";
}

void disintegrator_trail(sim::match_runtime::Match& match, sim::unit_spawn::Slot& antinuke) {
    // The D-gun's noexplode ball flies on through its blasts. It bursts with
    // its explode5 art on every tick it is in an enemy's cell below the
    // enemy's top, and on every tick it is below the ground, where it is, and
    // it is retired only when its range runs out: 240 at 200 a second, 36 ticks.
    auto& world = match.state();
    const auto& weapon = world.game.weapon_defs[disintegrator_weapon];
    CHECK((weapon.flags & OA_WEAPON_FLAG_NO_EXPLODE) != 0);
    // Fired from 12 above the ground, 48 west of the anti-nuke, at a point 6
    // above its base, as a commander's hand aims at a target's sweet spot.
    const FixedVec3 muzzle{fx(22), fx(ground_height + 12), fx(60)};
    const FixedVec3 aim{fx(70), fx(ground_height + 6), fx(60)};
    const auto flight = sim::weapon_execution::launch_line_projectile(
        weapon, {muzzle.x, muzzle.y, muzzle.z}, {aim.x, aim.y, aim.z}, world.game.tick
    );
    constexpr uint32_t flight_ticks = 36;
    CHECK(flight.lifetime_tick == world.game.tick + flight_ticks);
    const FixedVec3 velocity{flight.velocity[0], flight.velocity[1], flight.velocity[2]};
    auto& shot = launch(match, disintegrator_weapon, muzzle, velocity, 1);
    shot.lifetime_tick = flight.lifetime_tick;
    auto& effects = match.effects();
    effects.explosion_count = 0;
    const auto health = antinuke.record.health;
    const auto target_x = cell_of(antinuke.record.position.x);
    const auto target_z = cell_of(antinuke.record.position.z);
    FixedVec3 at = muzzle;
    int32_t struck = 0, trail = 0, first_trail = 0;
    for (uint32_t step = 1; step < flight_ticks; ++step) {
        const auto logged = effects.explosion_count;
        ++world.game.tick;
        match.update_projectiles();
        at = {at.x + velocity.x, at.y + velocity.y, at.z + velocity.z};
        CHECK(match.projectiles().size() == 1);
        const bool in_target = cell_of(at.x) == target_x && cell_of(at.z) == target_z;
        const bool underground = (at.y >> 16) < ground_height;
        CHECK(!(in_target && underground));
        CHECK(effects.explosion_count == logged + (in_target || underground ? 1 : 0));
        if (effects.explosion_count == logged)
            continue;
        const auto& blast = latest_explosion(match);
        CHECK(blast.position.x == at.x && blast.position.y == at.y && blast.position.z == at.z);
        CHECK(blast.sprite.sequence == &art.explode5);
        if (in_target) {
            ++struck;
        } else {
            first_trail = first_trail == 0 ? static_cast<int32_t>(step) : first_trail;
            ++trail;
        }
    }
    // Two ticks inside the anti-nuke's cell; then one blast a tick from the
    // 15th, the first below the ground 12 units under the muzzle, to the
    // 35th, the last it flies.
    CHECK(struck == 2 && antinuke.record.health < health);
    CHECK(first_trail == 15 && trail == 21);
    CHECK(effects.explosion_count == struck + trail);
    ++world.game.tick;
    match.update_projectiles();
    CHECK(match.projectiles().empty() && effects.explosion_count == struck + trail);
    antinuke.record.health = health;
    std::cout << "disintegrator trail passed: " << struck << " blasts on the target, " << trail
              << " along the ground\n";
}

void idle_tower_takes_a_target(sim::match_runtime::Match& match, sim::unit_spawn::Slot& antinuke) {
    // An idle laser tower that fires at will is handed the enemy it sees in
    // range by the sweep once the player's target cache has refreshed.
    auto* tower = match.create({1, 3, {88u << 16, ground_height << 16, 60u << 16}, true, 1, 0});
    CHECK(tower);
    tower->unit->object_present = false;
    const auto& aim = tower->record.weapons[0];
    CHECK(aim.target_a == 0 && aim.target_b == unit_target);
    bool aimed = false;
    for (int32_t step = 0; step < 90 && !aimed; ++step) {
        ++match.state().game.tick;
        match.tick();
        aimed = aim.target_a == static_cast<int16_t>(antinuke.unit_index) &&
                aim.target_b == unit_target;
    }
    CHECK(aimed);
    std::cout << "fire-at-will weapon sweep passed\n";
}

void mirrored_units_are_not_swept(
    sim::match_runtime::Match& match, sim::unit_spawn::Slot& antinuke
) {
    // The player slot update runs the player controller, and with it the weapon sweep, only for a player
    // whose record holds a controller (Player.controller), which a mirrored player lacks:
    // its own machine sweeps its units.
    auto* tower = match.create({1, 3, {100u << 16, ground_height << 16, 60u << 16}, true, 1, 0});
    CHECK(tower);
    tower->unit->object_present = false;
    auto& owner = match.state().game.players[1];
    owner.status = OA_PLAYER_STATUS_MIRRORED;
    const auto& aim = tower->record.weapons[0];
    for (int32_t step = 0; step < 90; ++step) {
        ++match.state().game.tick;
        match.tick();
    }
    CHECK(aim.target_a == 0 && aim.target_b == unit_target);
    owner.status = OA_PLAYER_STATUS_COMPUTER;
    bool aimed = false;
    for (int32_t step = 0; step < 90 && !aimed; ++step) {
        ++match.state().game.tick;
        match.tick();
        aimed = aim.target_a == static_cast<int16_t>(antinuke.unit_index) &&
                aim.target_b == unit_target;
    }
    CHECK(aimed);
    std::cout << "mirrored player's units left to their own sweep passed\n";
}

void silo_waits_for_its_aim_script(
    sim::match_runtime::Match& match, sim::unit_spawn::Slot& silo, sim::unit_spawn::Slot& target
) {
    // The vertical launch fires only once the slot's AimPrimary has returned nonzero
    // (UnitWeapon.aim_ready). The silo's AimPrimary sleeps 500 ms, 15 ticks at the match's clock
    // scale of 30, then returns static 0.
    auto& world = match.state();
    auto* instance = match.instance(silo.unit_index);
    CHECK(instance && instance->script());
    auto& vm = instance->script()->vm();
    auto& gun = silo.record.weapons[0];
    const auto nuke_ref = oa_ref_from_index(nuke_weapon);
    const auto in_flight = [&]() -> oa::Projectile* {
        for (int32_t i = 0; i < world.game.projectile_count; ++i) {
            auto& shot = world.projectiles[i];
            if (shot.def == nuke_ref && (shot.flags & OA_PROJECTILE_FLAG_RETIRED) == 0)
                return &shot;
        }
        return nullptr;
    };
    const auto arm = [&](int32_t aim_result) {
        CHECK(vm.set_static(0, aim_result));
        gun.stockpile = 1;
        gun.target_a = static_cast<int16_t>(target.unit_index);
        gun.target_b = unit_target;
    };
    CHECK(in_flight() == nullptr && (gun.flags & OA_UNIT_WEAPON_AIMED) == 0);
    arm(1);
    uint32_t ready_step = 0, launch_step = 0;
    for (uint32_t step = 1; step <= 40 && launch_step == 0; ++step) {
        const bool ready = gun.aim_ready != 0;
        ++world.game.tick;
        match.tick();
        if (ready_step == 0 && gun.aim_ready != 0)
            ready_step = step;
        if (in_flight() != nullptr) {
            CHECK(ready);
            launch_step = step;
        }
    }
    // Queued on step 1 and run by the unit's script tick that step; it wakes
    // 15 ticks later and returns, and the next weapon tick launches.
    CHECK(ready_step == 16 && launch_step == 17);
    // The launch spends the aim and the round.
    CHECK((gun.flags & OA_UNIT_WEAPON_AIMED) == 0 && gun.aim_ready == 0 && gun.stockpile == 0);
    sim::weapon_execution::retire_projectile(world, *in_flight());
    sim::weapon_execution::compact_projectiles(world);

    // An AimPrimary that returns zero keeps the round: the aim bit stays, so
    // the script is not restarted, and nothing launches.
    arm(0);
    for (int32_t step = 0; step < 60; ++step) {
        ++world.game.tick;
        match.tick();
        CHECK(in_flight() == nullptr);
    }
    CHECK((gun.flags & OA_UNIT_WEAPON_AIMED) != 0 && gun.aim_ready == 0 && gun.stockpile == 1);
    gun.stockpile = 0;
    std::cout << "vertical launch waits for AimPrimary passed\n";
}
} // namespace

int main() {
    formats::tnt::Map map;
    map.attribute_width = map.attribute_height = map_cells;
    map.attributes.resize(map_cells * map_cells);
    for (int32_t z = 0; z < map_cells; ++z)
        for (int32_t x = 0; x < map_cells; ++x)
            map.attributes[plot(x, z)].height = x >= shore_cell ? 0 : ground_height;
    map.sea_level = sea_level;
    std::vector<sim::visibility_state::TerrainCell> terrain_values(map_cells * map_cells);
    sim::visibility_state::SightMask mask;
    mask.width = mask.height = 1;
    mask.transparent = 0;
    mask.pixels = {1};
    const std::array masks{mask};
    auto model = std::make_shared<formats::objects3d::Model>();
    model->objects.resize(1);
    model->objects[0].name = "root";
    model->objects[0].vertices = {{0, 0, 0}, {0, 20 << 16, 0}};
    auto script = std::make_shared<formats::cob::CobProgram>();
    // AimPrimary accepts at once.
    namespace op = sim::script_vm::opcode;
    script->code = {op::return_, op::push_constant, 1, op::return_};
    script->scripts = {{"Create", 0}, {"AimPrimary", 1}, {"FirePrimary", 0}, {"RockUnit", 0}};
    script->entry_points = {0, 1, 0, 0};
    script->piece_names = {"root"};
    // The silo's AimPrimary sleeps 500 ms, then returns static 0.
    auto silo_script = std::make_shared<formats::cob::CobProgram>(*script);
    silo_script->code = {
        op::return_, op::push_constant, 500, op::sleep, op::push_static, 0, op::return_
    };
    silo_script->header.static_variable_count = 1;

    // 1 anti-nuke, 2 nuke silo: buildings with a stockpiled vertical launcher; 3 laser tower.
    constexpr size_t type_count = 4;
    std::array<sim::unit_spawn::LoadedType, type_count> loaded;
    std::array<sim::unit_spawn::Type, type_count> types;
    std::array<data::unit_definitions::UnitDefinition, type_count> defs;
    for (size_t i = 1; i < type_count; ++i) {
        loaded[i].model = model;
        loaded[i].script = script;
        types[i].simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE | OA_UNIT_DEF_FLAG_HAS_WEAPONS |
                                    (fire_at_will << OA_UNIT_DEF_FLAG_FIRE_ORDER_SHIFT);
        types[i].simulation.maximum_health = 3000;
        types[i].footprint_x = types[i].footprint_z = 1;
        types[i].bm_code = 0;
        types[i].model = reinterpret_cast<uintptr_t>(model.get());
        types[i].cob = reinterpret_cast<uintptr_t>(script.get());
        defs[i].sight_distance = 200;
        loaded[i].type = types[i];
    }
    loaded[2].script = silo_script;
    types[2].cob = reinterpret_cast<uintptr_t>(silo_script.get());
    loaded[2].type = types[2];
    defs[1].weapon1 = "AMD_ROCKET";
    defs[2].weapon1 = "NUCLEAR_MISSILE";
    defs[3].weapon1 = "LASER";
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    data::unit_definitions::RuntimeDefinitionMetadata metadata;
    std::array<sim::match_runtime::RuntimeTypeFields, type_count> fields{};
    const std::array<uint8_t, 1> yard{4};
    for (size_t i = 1; i < type_count; ++i) {
        fields[i].definition = &defs[i];
        fields[i].yard_mask = yard;
        fields[i].runtime_metadata = &metadata;
        fields[i].target_masks = &target_masks;
    }
    sim::combat_state::WeaponRegistry weapons;
    CHECK(sim::combat_state::install_weapon_text(weapons, weapon_tdf) == 8);
    CHECK(weapons.find("AMD_ROCKET")->coverage == 2000);
    const auto collision_plots = terrain_plots();
    // Sprite rocks keep their damage in the plot's record word.
    std::array<FeatureDef, 2> feature_defs{};
    for (auto& def : feature_defs) {
        def.flags = OA_FEATURE_FLAG_SPRITE | OA_FEATURE_FLAG_BLOCKING;
        def.footprint_x = def.footprint_z = 1;
        def.damage = 1000;
        def.dead_feature = sim::feature_runtime::no_feature;
    }
    feature_defs[0].height = rock_height;
    feature_defs[1].height = sea_rock_height;
    Services services;
    Scenario scenario;
    sim::match_runtime::OfflineInputs input{
        map, loaded, types, fields,    weapons, terrain_values,  masks, 16, 16, 4,    2,
        0,   30,     1,     &scenario, {},      collision_plots, {},    0,  0,  0.0F, feature_defs
    };
    input.effect_sequence = [](std::string_view archive, std::string_view entry) {
        return art.find(archive, entry);
    };
    sim::match_runtime::Match match(input, services);
    CHECK(match.state().game.weapon_defs[antinuke_weapon].coverage == 2000);
    CHECK(match.state().game.sea_level == sea_level);
    match.state().game.gravity = 0;
    // The tree-death console option: weapons damage features.
    match.state().game.console_flags |= OA_CONSOLE_FLAG_TREE_DEATH;
    match.configure_strategic_environment({0, 0.5f, 0});
    std::array<uint8_t, 10> allies{};
    allies[0] = 1;
    for (uint8_t p = 0; p < 2; ++p) {
        match.simulation().players[p].present = true;
        match.simulation().players[p].status = p == 0 ? 1 : 2;
        std::array<uint8_t, 10> own{};
        own[p] = 1;
        match.configure_player_alliances(p, own);
    }
    match.configure_outcomes(0, allies, true);
    match.state().game.tick = 100;

    ground_contact(match);
    water_contact(match);
    water_weapon_drops_into_the_sea(match);
    feature_contact(match);

    auto* antinuke = match.create({0, 1, {70u << 16, ground_height << 16, 60u << 16}, true, 1, 0});
    auto* silo = match.create({1, 2, {350u << 16, ground_height << 16, 60u << 16}, true, 1, 0});
    CHECK(antinuke && silo);
    for (auto* unit : {antinuke, silo}) {
        unit->unit->object_present = false;
        unit->unit->flags |= OA_UNIT_FLAG_LIVE;
    }
    CHECK(antinuke->record.weapons[0].def == oa_ref_from_index(antinuke_weapon));
    interception(match, *antinuke, *silo);
    stockpile_refund(match, *antinuke, *silo);
    direct_hit_does_not_chain(match, *antinuke);
    disintegrator_trail(match, *antinuke);
    idle_tower_takes_a_target(match, *antinuke);
    mirrored_units_are_not_swept(match, *antinuke);
    silo_waits_for_its_aim_script(match, *silo, *antinuke);
    return 0;
}
