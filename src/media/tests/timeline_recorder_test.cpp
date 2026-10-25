// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The timeline recorder on a small world built here: the header it starts
// from, the water map, the units already there recorded as created, every
// event hook's fields, samples of live mobile units every
// sample_period_ticks ticks with their flags, players' stats every
// stats_period_ticks ticks, the usable end and take() leaving the recorder
// empty.

#include "oa/core/world.h"
#include "oa/media/director/timeline_recorder.hpp"
#include "oa/sim/match_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <source_location>
#include <string_view>
#include <vector>

namespace director = oa::media::director;
using oa::sim::match_runtime::DeathKind;
using oa::sim::match_runtime::KillOutcome;
using oa::sim::match_runtime::ShotSource;

namespace {

int failures = 0;

/// Records a failed expectation with its line.
void expect(
    bool value,
    std::string_view what,
    const std::source_location where = std::source_location::current()
) {
    if (value)
        return;
    ++failures;
    std::cerr << where.file_name() << ':' << where.line() << ": " << what << '\n';
}

/// The world's unit slots, slot 0 included.
constexpr uint32_t slot_count = 16;

/// Unit types: a commander, a tank and a solar collector.
enum : uint16_t { commander_type = 1, tank_type, solar_type, type_count };

/// Weapons: the commander's laser and the tank's cannon.
constexpr uint8_t laser = 5;
constexpr uint8_t cannon = 7;
/// Players: two that fight and a watcher the replay is watched from.
constexpr uint8_t red = 0;
constexpr uint8_t blue = 1;
constexpr uint8_t watcher = 2;
/// The tick the replay starts on.
constexpr uint32_t start_tick = 100;
/// Slots: red's commander, blue's cloaked tank, blue's solar collector, and a
/// tank red builds later.
constexpr uint16_t red_commander = 1;
constexpr uint16_t blue_tank = 2;
constexpr uint16_t blue_solar = 3;
constexpr uint16_t red_tank = 4;

/// Returns a whole number of map pixels as 16.16 fixed point.
constexpr int32_t fixed(int32_t whole) {
    return static_cast<int32_t>(static_cast<uint32_t>(whole) << 16);
}

/// Copies text into a character array.
template <size_t size>
void set_text(char (&field)[size], std::string_view text) {
    std::memset(field, 0, size);
    std::memcpy(field, text.data(), text.size() < size ? text.size() : size - 1);
}

/// Sets a player up as a live one.
void set_player(oa::World& world, uint8_t index, std::string_view name, bool watching) {
    oa::Player& player{world.game.players[index]};
    player.in_use = 1;
    player.status = OA_PLAYER_STATUS_MIRRORED;
    player.index = index;
    player.info = oa::oa_ref_from_index(index);
    set_text(player.name, name);
    oa::PlayerSetupInfo& info{world.player_info[index]};
    info.side = 0;
    info.color = static_cast<uint8_t>(index + 3);
    info.options = watching ? OA_SETUP_OPTION_WATCHER : uint16_t{0};
}

/// Places a live, finished unit.
oa::Unit& place(
    oa::World& world, uint16_t slot, uint16_t type, uint8_t owner, int32_t x, int32_t y, int32_t z
) {
    oa::Unit& unit{world.units[slot]};
    unit.type_index = type;
    unit.def = oa::oa_ref_from_index(type);
    unit.owner = oa::oa_ref_from_index(owner);
    unit.owner_index = owner;
    unit.id = slot;
    unit.flags = OA_UNIT_FLAG_LIVE;
    unit.position = {fixed(x), fixed(y), fixed(z)};
    unit.health = 100;
    return unit;
}

/// Builds the world: two fighting players and a watcher, red's commander,
/// blue's tank and solar collector.
oa::World* make_world(uint32_t extra_weapons = 0) {
    oa::World* world{oa::world_create()};
    const oa::WorldCapacity capacity{slot_count, type_count, 0, extra_weapons};
    if (world == nullptr || oa::world_alloc_tables(world, &capacity) == 0)
        std::abort();
    world->game.map_pixel_width = 2048;
    world->game.map_pixel_height = 1024;
    world->game.tick = start_tick;
    set_text(world->game.sides[0].commander, "ARMCOM");
    oa::UnitDef& commander{world->unit_defs[commander_type]};
    set_text(commander.unit_name, "armcom");
    commander.bm_code = 1;
    commander.flags = OA_UNIT_DEF_FLAG_BUILDER;
    commander.build_cost_metal = 2500.75f;
    commander.build_cost_energy = 26000.5f;
    commander.max_damage = 3000;
    commander.build_distance = 120;
    commander.weapon1 = oa::oa_ref_from_index(laser);
    oa::UnitDef& tank{world->unit_defs[tank_type]};
    set_text(tank.unit_name, "ARMSTUMP");
    tank.bm_code = 1;
    tank.build_cost_metal = 90.9f;
    tank.max_damage = 600;
    tank.weapon1 = oa::oa_ref_from_index(cannon);
    tank.weapon3 = oa::oa_ref_from_index(laser);
    oa::UnitDef& solar{world->unit_defs[solar_type]};
    set_text(solar.unit_name, "ARMSOLAR");
    solar.bm_code = 0;
    solar.build_cost_metal = 145.0f;
    solar.max_damage = 500;
    oa::WeaponDef& laser_weapon{world->game.weapon_defs[laser]};
    set_text(laser_weapon.name, "Light laser");
    laser_weapon.range = 300;
    laser_weapon.area_of_effect = 8;
    laser_weapon.damage_default = 50;
    laser_weapon.flags = OA_WEAPON_FLAG_LINE_OF_SIGHT;
    oa::WeaponDef& cannon_weapon{world->game.weapon_defs[cannon]};
    set_text(cannon_weapon.name, "Cannon");
    cannon_weapon.range = 1400;
    cannon_weapon.area_of_effect = 300;
    cannon_weapon.damage_default = 120;
    cannon_weapon.shake_magnitude = 4;
    cannon_weapon.flags = OA_WEAPON_FLAG_BALLISTIC;
    set_player(*world, red, "Red", false);
    set_player(*world, blue, "Blue", false);
    set_player(*world, watcher, "Watcher", true);
    world->game.players[red].alliance[watcher] = 1;
    world->game.players[red].kills = 3;
    world->game.players[blue].losses = 3;
    world->game.players[blue].commanders_lost = 1;
    world->game.players[red].commanders_killed = 1;
    world->game.players[red].unit_count = 7;
    world->game.players[red].metal = 812.9f;
    world->game.players[red].energy = -4.5f;
    place(*world, red_commander, commander_type, red, 500, 20, 400);
    oa::Unit& tank_unit{place(*world, blue_tank, tank_type, blue, 1500, 0, 600)};
    tank_unit.state_flags = OA_UNIT_STATE_CLOAKED;
    place(*world, blue_solar, solar_type, blue, 1600, 10, 700);
    return world;
}

/// A visibility hook that hides blue's tank.
bool hide_blue_tank(void*, uint16_t unit) {
    return unit != blue_tank;
}

void test_begin() {
    oa::World* world{make_world()};
    director::TimelineRecorder recorder{};
    recorder.begin(*world, watcher, "Coast To Coast");
    recorder.finish(*world, director::ReplayVerdict{true, true, true, false, 0, 0, {}});
    const director::Timeline timeline{recorder.take()};
    const director::TimelineHeader& header{timeline.header};
    expect(header.map_name == "Coast To Coast", "map name");
    expect(header.map_width == 2048 && header.map_height == 1024, "map bounds");
    expect(
        header.first_tick == start_tick && header.last_tick == start_tick, "first and last tick"
    );
    expect(header.viewer_player == watcher, "viewer");
    expect(header.players.size() == 3, "three players");
    if (header.players.size() == 3) {
        const director::TimelinePlayer& first{header.players[0]};
        expect(first.index == red && first.name == "Red", "red's index and name");
        expect(first.color == 3 && first.side == 0, "red's colour and side");
        expect(!first.watcher && !first.viewer && first.owned_units, "red fights");
        expect(first.start.x == 500 && first.start.y == 20 && first.start.z == 400, "red's start");
        expect(first.allies == (1u << watcher), "red's allies leave out itself");
        const director::TimelinePlayer& second{header.players[1]};
        expect(second.owned_units && second.start.x == 1500, "blue's start is its first unit");
        const director::TimelinePlayer& third{header.players[2]};
        expect(third.watcher && third.viewer && !third.owned_units, "the watcher");
    }
    expect(header.unit_types.size() == 3, "three unit types");
    if (header.unit_types.size() == 3) {
        const director::TimelineUnitType& commander{header.unit_types[0]};
        expect(commander.type_index == commander_type, "types in index order");
        expect(commander.unit_name == "armcom", "unit name");
        expect(commander.mobile && commander.builder && !commander.can_fly, "commander flags");
        expect(commander.metal_cost == 2500 && commander.energy_cost == 26000, "costs truncated");
        expect(commander.max_damage == 3000 && commander.build_distance == 120, "health, reach");
        expect(commander.weapons[0] == laser && commander.weapons[1] == 0, "commander weapons");
        const director::TimelineUnitType& tank{header.unit_types[1]};
        expect(tank.weapons[0] == cannon && tank.weapons[2] == laser, "tank weapons");
        expect(tank.metal_cost == 90, "tank cost truncated");
        expect(!header.unit_types[2].mobile, "a building is not mobile");
    }
    expect(header.weapons.size() == 2, "two weapons");
    if (header.weapons.size() == 2) {
        const director::TimelineWeapon& first{header.weapons[0]};
        expect(first.weapon_id == laser && first.name == "Light laser", "weapons in id order");
        expect(first.range == 300 && first.area_of_effect == 8 && first.damage == 50, "laser");
        expect(first.flags == OA_WEAPON_FLAG_LINE_OF_SIGHT, "laser flags");
        const director::TimelineWeapon& second{header.weapons[1]};
        expect(second.shake_magnitude == 4 && second.area_of_effect == 300, "cannon");
    }
    // Units already there are created on the first tick, commanders first.
    expect(timeline.events.size() == 3, "three units created");
    if (timeline.events.size() == 3) {
        const director::TimelineEvent& commander{timeline.events[0]};
        expect(commander.kind == director::EventKind::created, "created");
        expect(commander.tick == start_tick && commander.unit == red_commander, "commander first");
        expect(commander.flags == director::event_flag::commander, "commander flag");
        expect(commander.owner == red && commander.unit_type == commander_type, "owner and type");
        expect(commander.build_left == 0, "finished");
        expect(commander.at.x == 500 && commander.at.y == 20 && commander.at.z == 400, "place");
        expect(timeline.events[1].unit == blue_tank && timeline.events[1].flags == 0, "tank");
        expect(timeline.events[2].unit == blue_solar, "solar");
    }
    expect(timeline.samples.empty() && timeline.stats.empty(), "begin samples nothing");
    expect(timeline.usable_end_tick == start_tick + 1, "usable end after the last tick");
    oa::world_destroy(world);
}

// The water map: squares of the map's cells at or below the sea level, a
// cell placed by its middle's ground point.
void test_water() {
    oa::World* world{make_world()};
    director::TimelineRecorder recorder{};
    recorder.begin(*world, watcher, "Dry");
    director::Timeline dry{recorder.take()};
    expect(dry.header.water_columns == 32 && dry.header.water_rows == 16, "the squares");
    expect(
        dry.header.water.size() == 32u * 16u && std::all_of(
                                                    dry.header.water.begin(),
                                                    dry.header.water.end(),
                                                    [](uint8_t percent) { return percent == 0; }
                                                ),
        "a world without cells has no water"
    );
    // A map of 8 by 8 cells, 128 map pixels a side: the left half at the
    // sea level, the right half above it but for one sunken cell, whose
    // ground point lies in the square below its neighbours'. The top row's
    // ground points lie above the map.
    world->game.map_pixel_width = 128;
    world->game.map_pixel_height = 128;
    world->game.map_width = 8;
    world->game.map_height = 8;
    world->game.sea_level = 20;
    std::vector<oa::MapPlot> plots(64);
    for (int32_t z{}; z < 8; ++z)
        for (int32_t x{}; x < 8; ++x)
            plots[static_cast<size_t>(z * 8 + x)].height = x < 4 ? 20 : 40;
    plots[4 * 8 + 7].height = 0;
    world->plots = plots.data();
    recorder.begin(*world, watcher, "Wet");
    const director::Timeline wet{recorder.take()};
    world->plots = nullptr;
    const director::TimelineHeader& header{wet.header};
    expect(header.water_columns == 2 && header.water_rows == 2, "two by two squares");
    expect(header.water.size() == 4, "four squares");
    if (header.water.size() == 4) {
        expect(header.water[0] == 100 && header.water[2] == 100, "the left half is sea");
        expect(header.water[1] == 0, "the right half is land");
        expect(header.water[3] == 100 / 13, "one sunken cell of thirteen");
    }
    oa::world_destroy(world);
}

void test_events() {
    oa::World* world{make_world()};
    director::TimelineRecorder recorder{};
    recorder.begin(*world, watcher, "Map");
    const oa::sim::match_runtime::EventHooks hooks{recorder.event_hooks()};
    expect(hooks.context == &recorder, "hooks carry the recorder");
    world->game.tick = start_tick + 1;

    oa::Unit& built{place(*world, red_tank, tank_type, red, -3, 8, 900)};
    built.position.x = -fixed(3) / 2; // -1.5 pixels
    built.build_remaining = 0.5f;
    hooks.unit_created(hooks.context, *world, red_tank);

    oa::Projectile shot{};
    shot.def = oa::oa_ref_from_index(cannon);
    shot.origin = {fixed(510), fixed(30), fixed(410)};
    shot.position = {fixed(1490), fixed(2), fixed(598)};
    shot.source = oa::oa_unit_ref_from_slot(red_commander);
    shot.owner_index = red;
    const oa::FixedVec3 aim{fixed(1500), 0, fixed(600)};
    hooks.shot_placed(hooks.context, *world, shot, ShotSource::weapon, &aim, blue_tank);
    hooks.shot_placed(hooks.context, *world, shot, ShotSource::burst, nullptr, 0);
    hooks.shot_placed(hooks.context, *world, shot, ShotSource::meteor, nullptr, 0);
    hooks.shot_detonated(hooks.context, *world, shot, blue_tank);

    world->game.tick = start_tick + 2;
    hooks.unit_damaged(hooks.context, *world, blue_tank, red_commander, 40, 1);
    hooks.unit_damaged(hooks.context, *world, blue_solar, 0, 12, 1);
    world->units[red_commander].last_attacker_id = blue_tank;
    world->units[red_commander].last_attacker_owner = blue;
    hooks.unit_died(
        hooks.context, *world, red_commander, KillOutcome{DeathKind::weapon, 50, 2}, true
    );
    world->units[blue_solar].last_attacker_owner = OA_PLAYER_COUNT;
    hooks.unit_died(
        hooks.context, *world, blue_solar, KillOutcome{DeathKind::self_destruct}, false
    );
    hooks.unit_finished(hooks.context, *world, red_tank, red_commander);
    // An empty slot reports nothing.
    hooks.unit_damaged(hooks.context, *world, 9, 0, 5, 1);

    recorder.finish(*world, director::ReplayVerdict{});
    const director::Timeline timeline{recorder.take()};
    const auto& events{timeline.events};
    expect(events.size() == 3 + 10, "every event recorded");
    if (events.size() != 13)
        return oa::world_destroy(world);

    const director::TimelineEvent& created{events[3]};
    expect(created.kind == director::EventKind::created && created.tick == start_tick + 1, "made");
    expect(created.unit == red_tank && created.owner == red, "made unit and owner");
    expect(created.build_left == 32768, "half built");
    expect(created.at.x == -2 && created.at.z == 900, "positions round toward minus infinity");
    expect(created.flags == 0, "not a commander");

    const director::TimelineEvent& aimed{events[4]};
    expect(aimed.kind == director::EventKind::shot, "shot");
    expect(aimed.unit == red_commander && aimed.owner == red, "shooter");
    expect(aimed.weapon == cannon, "weapon id");
    expect(aimed.at.x == 510 && aimed.at.y == 30 && aimed.at.z == 410, "muzzle");
    expect(aimed.flags == director::event_flag::has_target, "aim known");
    expect(aimed.target.x == 1500 && aimed.target.z == 600, "aim point");
    expect(aimed.other_unit == blue_tank, "target unit");
    const director::TimelineEvent& burst{events[5]};
    expect(burst.flags == director::event_flag::burst, "burst flag and no aim");
    const director::TimelineEvent& meteor{events[6]};
    expect(meteor.unit == 0, "a meteor has no shooter");

    const director::TimelineEvent& blast{events[7]};
    expect(blast.kind == director::EventKind::detonation, "detonation");
    expect(blast.unit == red_commander && blast.owner == red && blast.weapon == cannon, "blast");
    expect(blast.at.x == 1490 && blast.at.y == 2 && blast.at.z == 598, "where it detonated");
    expect(blast.other_unit == blue_tank, "unit struck");

    const director::TimelineEvent& hit{events[8]};
    expect(hit.kind == director::EventKind::damage && hit.tick == start_tick + 2, "damage");
    expect(hit.unit == blue_tank && hit.owner == blue && hit.unit_type == tank_type, "target");
    expect(hit.other_unit == red_commander && hit.other_owner == red, "attacker");
    expect(hit.amount == 40 && hit.damage_kind == 1, "amount and kind");
    expect(hit.at.x == 1500 && hit.at.z == 600, "target's place");
    const director::TimelineEvent& unattributed{events[9]};
    expect(unattributed.other_unit == 0 && unattributed.other_owner == director::no_player, "none");

    const director::TimelineEvent& death{events[10]};
    expect(death.kind == director::EventKind::death, "death");
    expect(death.unit == red_commander && death.unit_type == commander_type, "who died");
    expect(
        death.flags == (director::event_flag::commander | director::event_flag::settled_elsewhere),
        "commander death settled elsewhere"
    );
    expect(death.other_unit == blue_tank && death.other_owner == blue, "last attacker");
    expect(death.damage_kind == static_cast<uint8_t>(DeathKind::weapon), "death kind");
    const director::TimelineEvent& self_destruct{events[11]};
    expect(self_destruct.other_owner == director::no_player, "no last attacker's owner");
    expect(self_destruct.flags == 0, "not settled elsewhere");

    const director::TimelineEvent& finished{events[12]};
    expect(finished.kind == director::EventKind::finished, "finished");
    expect(finished.unit == red_tank && finished.other_unit == red_commander, "builder");
    oa::world_destroy(world);
}

void test_samples() {
    oa::World* world{make_world()};
    oa::Unit& half{place(*world, red_tank, tank_type, red, 700, 0, 300)};
    half.build_remaining = 0.25f;
    half.attach_parent = oa::oa_unit_ref_from_slot(red_commander);
    // A unit whose live flag is clear is not sampled.
    oa::Unit& dying{place(*world, 5, tank_type, blue, 50, 0, 50)};
    dying.flags = 0;
    director::TimelineRecorder recorder{};
    recorder.begin(*world, watcher, "Map");
    const director::UnitVisibilityHooks visibility{nullptr, &hide_blue_tank};
    for (uint32_t tick{start_tick + 1}; tick <= start_tick + 60; ++tick) {
        world->game.tick = tick;
        recorder.after_tick(*world, visibility);
    }
    // A tick after_tick has already seen is not sampled again.
    recorder.after_tick(*world, visibility);
    recorder.finish(
        *world, director::ReplayVerdict{true, false, true, false, 2, start_tick + 40, "late"}
    );
    const director::Timeline timeline{recorder.take()};
    std::vector<uint32_t> ticks{};
    for (const director::UnitSample& sample : timeline.samples)
        if (ticks.empty() || ticks.back() != sample.tick)
            ticks.push_back(sample.tick);
    expect(
        ticks ==
            std::vector<uint32_t>{
                start_tick + 15, start_tick + 30, start_tick + 45, start_tick + 60
            },
        "samples every 15 ticks from the first"
    );
    expect(timeline.samples.size() == 4u * 3u, "three mobile units each time");
    if (timeline.samples.size() >= 3) {
        const director::UnitSample& commander{timeline.samples[0]};
        expect(commander.unit == red_commander && commander.owner == red, "by slot");
        expect(commander.flags == director::sample_flag::visible, "commander drawn");
        expect(commander.health == 100 && commander.at.y == 20, "health and height");
        const director::UnitSample& tank{timeline.samples[1]};
        expect(tank.unit == blue_tank, "blue's tank");
        expect(tank.flags == director::sample_flag::cloaked, "cloaked and hidden");
        const director::UnitSample& carried{timeline.samples[2]};
        expect(carried.unit == red_tank, "red's tank");
        expect(
            carried.flags == (director::sample_flag::visible | director::sample_flag::carried |
                              director::sample_flag::unfinished),
            "carried and unfinished"
        );
    }
    expect(timeline.stats.size() == 2u * 3u, "stats every 30 ticks for three players");
    if (!timeline.stats.empty()) {
        const director::PlayerStats& red_stats{timeline.stats[0]};
        expect(red_stats.tick == start_tick + 30 && red_stats.player == red, "first stats");
        expect(red_stats.kills == 3 && red_stats.commanders_killed == 1, "kills");
        expect(red_stats.unit_count == 7, "unit count");
        expect(red_stats.metal == 812 && red_stats.energy == -4, "stored, truncated");
        const director::PlayerStats& blue_stats{timeline.stats[1]};
        expect(blue_stats.losses == 3 && blue_stats.commanders_lost == 1, "losses");
    }
    expect(timeline.header.last_tick == start_tick + 60, "last tick");
    expect(timeline.usable_end_tick == start_tick + 40, "usable end at the first error");
    expect(timeline.verdict.errors == 2 && timeline.verdict.last_error == "late", "verdict kept");
    // take() leaves the recorder empty.
    director::Timeline again{recorder.take()};
    expect(again.samples.empty() && again.events.empty(), "empty after take");
    expect(again.header.players.empty(), "no players after take");
    oa::world_destroy(world);
}

void test_wide_weapon() {
    constexpr uint16_t wide = 300;
    oa::World* world{make_world(45)};
    oa::WeaponDef* definition{oa::world_weapon_def(world, oa::oa_ref_from_index(wide))};
    expect(definition != nullptr, "weapon 300 has a record");
    if (definition != nullptr)
        set_text(definition->name, "Wide gun");
    director::TimelineRecorder recorder{};
    recorder.begin(*world, watcher, "Map");
    const oa::sim::match_runtime::EventHooks hooks{recorder.event_hooks()};
    oa::Projectile shot{};
    shot.def = oa::oa_ref_from_index(wide);
    shot.origin = {fixed(510), fixed(30), fixed(410)};
    shot.position = {fixed(1490), fixed(2), fixed(598)};
    shot.source = oa::oa_unit_ref_from_slot(red_commander);
    shot.owner_index = red;
    hooks.shot_placed(hooks.context, *world, shot, ShotSource::weapon, nullptr, 0);
    recorder.finish(*world, director::ReplayVerdict{});
    const director::Timeline timeline{recorder.take()};
    expect(timeline.header.weapons.size() == 3, "weapon 300 after the stock ones");
    if (timeline.header.weapons.size() == 3) {
        expect(timeline.header.weapons[0].weapon_id == laser, "laser first");
        expect(timeline.header.weapons[1].weapon_id == cannon, "cannon second");
        expect(
            timeline.header.weapons[2].weapon_id == wide &&
                timeline.header.weapons[2].name == "Wide gun",
            "weapon 300 last"
        );
    }
    bool recorded{false};
    for (const director::TimelineEvent& event : timeline.events) {
        if (event.kind == director::EventKind::shot && event.weapon == wide)
            recorded = true;
    }
    expect(recorded, "the shot records weapon 300");
    oa::world_destroy(world);
}

void test_null_visibility() {
    oa::World* world{make_world()};
    director::TimelineRecorder recorder{};
    recorder.begin(*world, watcher, "Map");
    world->game.tick = start_tick + 15;
    recorder.after_tick(*world, director::UnitVisibilityHooks{});
    recorder.finish(*world, director::ReplayVerdict{});
    const director::Timeline timeline{recorder.take()};
    expect(timeline.samples.size() == 2, "two mobile units");
    for (const director::UnitSample& sample : timeline.samples)
        expect((sample.flags & director::sample_flag::visible) != 0, "null hook draws every unit");
    oa::world_destroy(world);
}

} // namespace

int main() {
    test_begin();
    test_water();
    test_events();
    test_wide_weapon();
    test_samples();
    test_null_visibility();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "media-director-planner-recorder: ok\n";
    return EXIT_SUCCESS;
}
